#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <ArduinoJson.h>
#include "usb_network_protocol.h"
// Socket work runs on a separate task. The HCI UART task only validates/enqueues.
class UsbNetworkBridge {
 friend struct UsbNetworkBridgeTestAccess;
 using Frame=UsbNetworkProtocol::Frame;
 using Emitter=bool(*)(const uint8_t*,size_t);
 struct Slot {WiFiClient tcp;WiFiUDP udp;uint8_t kind=0;uint16_t udpId=0,udpSize=0,udpAt=0;uint8_t udpOut[UsbNetworkProtocol::UDP_MAX];};
 Slot slots[UsbNetworkProtocol::STREAMS];
 QueueHandle_t commands=nullptr;TaskHandle_t task=nullptr;Emitter emit=nullptr;
 volatile uint16_t session=0,acked=0;volatile bool enabled=false,pcReady=false,allowed=true,resetRequested=false;
 uint16_t lastCommand=0,lastCommandSession=0,eventSequence=0;
 uint8_t pending[260];size_t pendingSize=0;uint16_t pendingSequence=0;uint32_t pendingAt=0,pendingCreated=0,hostSeen=0,writeStarted=0;
 bool pendingSent=false;uint8_t scan=0;
 uint8_t udpIn[UsbNetworkProtocol::UDP_MAX],udpAddress[6];uint16_t udpInSize=0,udpInAt=0,udpInId=0;uint8_t udpInStream=0;
 volatile uint32_t bytesTx=0,bytesRx=0,errors=0;
 Frame writing{};size_t writeAt=0;bool haveWrite=false;
 static void worker(void*p){auto*self=static_cast<UsbNetworkBridge*>(p);for(;;){self->tick();vTaskDelay(1);}}
 void closeSlot(unsigned i){slots[i].tcp.stop();slots[i].udp.stop();slots[i].kind=0;slots[i].udpAt=slots[i].udpSize=0;}
 void closeAll(){for(unsigned i=0;i<UsbNetworkProtocol::STREAMS;i++)closeSlot(i);pendingSize=0;pendingSent=false;haveWrite=false;udpInSize=udpInAt=0;pcReady=false;}
 void event(uint8_t type,uint8_t stream,const uint8_t*data,size_t n){
  if(pendingSize)return;
  if(++eventSequence==0)++eventSequence;
  pendingSequence=eventSequence;pending[0]=4;pending[1]=0xff;
  size_t len=UsbNetworkProtocol::encode(pending+3,type,stream,session,eventSequence,data,n);pending[2]=len;pendingSize=len+3;pendingSent=false;pendingAt=pendingCreated=millis();
 }
 bool flushEvent(){
  if(!pendingSize)return false;
  if(acked==pendingSequence){pendingSize=0;pendingSent=false;return false;}
  if(millis()-pendingCreated>10000){errors++;closeAll();enabled=false;session=0;return true;}
  if(!pendingSent||millis()-pendingAt>=1000){if(emit&&emit(pending,pendingSize)){pendingSent=true;pendingAt=millis();}}
  return true;
 }
 bool connectTcp(Slot&s,const Frame&f){
  IPAddress address;uint16_t port=uint16_t(f.data[f.size-2])<<8|f.data[f.size-1];
  if(f.data[1]==1)address=IPAddress(f.data+2);
  else {char name[64];memcpy(name,f.data+3,f.data[2]);name[f.data[2]]=0;if(WiFi.hostByName(name,address)!=1)return false;}
  s.tcp.setTimeout(1000);s.tcp.setNoDelay(true);return s.tcp.connect(address,port,1500)==1;
 }
 void handle(const Frame&f){
  using namespace UsbNetworkProtocol;
  if(f.op==HELLO){closeAll();enabled=true;uint8_t reply[]={1,STREAMS,DATA_MAX,1};event(WELCOME,0,reply,sizeof(reply));return;}
  if(f.session!=session)return;
  if(f.op==DISABLE){closeAll();enabled=false;return;}
  if(f.op==READY){pcReady=true;return;}
  if(!enabled||f.stream<1||f.stream>STREAMS)return;
  Slot&s=slots[f.stream-1];
  if(f.op==OPEN){closeSlot(f.stream-1);bool ok=ESP.getFreeHeap()>=14000&&WiFi.status()==WL_CONNECTED&&(f.data[0]==2?s.udp.begin(0)==1:connectTcp(s,f));if(ok)s.kind=f.data[0];uint8_t result=ok?0:5;event(OPENED,f.stream,&result,1);return;}
  if(f.op==CLOSE){closeSlot(f.stream-1);uint8_t reason=0;event(CLOSED,f.stream,&reason,1);return;}
  if(f.op==DATA&&s.kind==1){writing=f;writeAt=0;haveWrite=true;writeStarted=millis();return;}
  if(f.op==UsbNetworkProtocol::UDP&&s.kind==2){
   uint16_t id=u16(f.data),total=u16(f.data+2),offset=u16(f.data+4);size_t count=f.size-12;
   if(offset==0){s.udpId=id;s.udpSize=total;s.udpAt=0;}
   if(id!=s.udpId||total!=s.udpSize||offset!=s.udpAt){errors++;return;}
   memcpy(s.udpOut+offset,f.data+12,count);s.udpAt+=count;
   if(s.udpAt==s.udpSize){IPAddress ip(f.data+6);uint16_t port=uint16_t(f.data[10])<<8|f.data[11];if(s.udp.beginPacket(ip,port)==1){s.udp.write(s.udpOut,total);if(s.udp.endPacket()==1)bytesTx+=total;else errors++;}else errors++;s.udpAt=s.udpSize=0;}
  }
 }
 void tick(){
  using namespace UsbNetworkProtocol;
  if(resetRequested){closeAll();resetRequested=false;}
  if(!allowed){if(enabled)closeAll();enabled=false;session=0;if(commands)xQueueReset(commands);return;}
  if(enabled&&millis()-hostSeen>20000){closeAll();enabled=false;session=0;return;}
  if(flushEvent())return;
  if(haveWrite){Slot&s=slots[writing.stream-1];if(s.kind!=1||!s.tcp.connected()||millis()-writeStarted>5000){haveWrite=false;closeSlot(writing.stream-1);uint8_t reason=1;event(CLOSED,writing.stream,&reason,1);return;}size_t n=s.tcp.write(writing.data+writeAt,writing.size-writeAt);writeAt+=n;bytesTx+=n;if(n)writeStarted=millis();if(writeAt==writing.size)haveWrite=false;return;}
  Frame f;if(commands&&xQueueReceive(commands,&f,0)==pdTRUE){handle(f);return;}
  if(!enabled)return;
  if(udpInSize){uint8_t data[DATA_MAX];put16(data,udpInId);put16(data+2,udpInSize);put16(data+4,udpInAt);memcpy(data+6,udpAddress,6);size_t count=udpInSize-udpInAt;if(count>DATA_MAX-12)count=DATA_MAX-12;memcpy(data+12,udpIn+udpInAt,count);udpInAt+=count;event(DATAGRAM,udpInStream,data,count+12);if(udpInAt==udpInSize)udpInAt=udpInSize=0;return;}
  for(unsigned j=0;j<STREAMS;j++){
   unsigned i=(scan+j)%STREAMS;Slot&s=slots[i];
   if(s.kind==1){int available=s.tcp.available();if(available>0){uint8_t data[DATA_MAX];int n=s.tcp.read(data,available>int(DATA_MAX)?DATA_MAX:available);if(n>0){bytesRx+=n;event(RECEIVED,i+1,data,n);scan=(i+1)%STREAMS;return;}}else if(!s.tcp.connected()){closeSlot(i);uint8_t reason=0;event(CLOSED,i+1,&reason,1);return;}}
   if(s.kind==2){int n=s.udp.parsePacket();if(n>0){if(n>int(UDP_MAX)){while(s.udp.available())s.udp.read();errors++;continue;}IPAddress ip=s.udp.remoteIP();for(int k=0;k<4;k++)udpAddress[k]=ip[k];uint16_t port=s.udp.remotePort();udpAddress[4]=port>>8;udpAddress[5]=port;int read=s.udp.read(udpIn,n);if(read>0){bytesRx+=read;udpInSize=read;udpInAt=0;udpInStream=i+1;if(++udpInId==0)++udpInId;scan=(i+1)%STREAMS;return;}}}
  }
 }
public:
 void begin(Emitter callback){emit=callback;}
 void setAllowed(bool value){allowed=value;}
 uint8_t accept(const uint8_t*p,size_t n,uint16_t&outSession,uint16_t&outSequence){
  using namespace UsbNetworkProtocol;
  outSession=n>=9?u16(p+5):0;outSequence=n>=9?u16(p+7):0;
  Frame f;if(!decode(p,n,f)||!commandValid(f))return 2;
  if(!allowed)return 4;
  if(f.op==HELLO&&!task){
   if(ESP.getFreeHeap()<24000)return 5;
   if(!commands)commands=xQueueCreate(8,sizeof(Frame));
   if(!commands||xTaskCreatePinnedToCore(worker,"usb-net",6144,this,1,&task,0)!=pdPASS)return 5;
  }
  if(!task)return 4;
  if(f.op==ACK&&f.session==session){acked=u16(f.data);hostSeen=millis();return 0;}
  if(f.session==lastCommandSession&&f.sequence==lastCommand)return 0;
  if(f.op!=HELLO&&f.session!=session)return 4;
  hostSeen=millis();
  if(f.op==PING){if(!enabled)return 4;if(WiFi.status()!=WL_CONNECTED)return 3;lastCommand=f.sequence;lastCommandSession=f.session;return 0;}
  if(f.op==HELLO){if(WiFi.status()!=WL_CONNECTED)return 3;session=f.session;acked=0;lastCommand=0;resetRequested=true;xQueueReset(commands);}
  if(xQueueSend(commands,&f,0)!=pdTRUE)return 1;
  lastCommand=f.sequence;lastCommandSession=f.session;return 0;
 }
 bool requestPC(bool start){uint8_t data=start?1:0,packet[16];packet[0]=4;packet[1]=0xff;size_t n=UsbNetworkProtocol::encode(packet+3,UsbNetworkProtocol::CONTROL,0,0,0,&data,1);packet[2]=n;return emit&&emit(packet,n+3);}
 void status(JsonObject doc){doc["supported"]=true;doc["enabled"]=enabled;doc["pc_ready"]=pcReady;doc["wifi_connected"]=WiFi.status()==WL_CONNECTED;doc["bytes_to_wifi"]=static_cast<uint32_t>(bytesTx);doc["bytes_from_wifi"]=static_cast<uint32_t>(bytesRx);doc["errors"]=static_cast<uint32_t>(errors);doc["max_streams"]=UsbNetworkProtocol::STREAMS;}
};
