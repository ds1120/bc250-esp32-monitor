#include "../firmware/src/usb_hci_bridge.h"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
int main(int argc,char** argv){
 UsbHciBridge bridge;assert(bridge.begin());assert(mockRadioState==1);
 // Standby radio belongs to the wake receiver until it releases the controller.
 mockRadioState=2;bridge.service(true);assert(!bridge.running());
 mockRadioState=1;mockTime+=1000;bridge.service(true);assert(bridge.running());
 // Host command fragments go to VHCI, not to the relay/console command parser.
 Serial.input={1,3};bridge.service(true);assert(mockCommands.empty());
 Serial.input={12,0};bridge.service(true);bridge.service(true);
 assert(mockCommands.size()==1&&mockCommands[0]==std::vector<uint8_t>({1,3,12,0}));
 // Controller backpressure retains one complete command until credit is available.
 mockCanSend=false;Serial.input={1,3,12,0};bridge.service(true);assert(mockCommands.size()==1);
 mockCanSend=true;bridge.service(true);assert(mockCommands.size()==2);
 // Controller-to-host packets survive short USB writes without text or corruption.
 uint8_t reply[]={4,14,4,1,3,12,0};mockCallback->notify_host_recv(reply,sizeof(reply));
 Serial.room=3;bridge.service(true);assert(Serial.output.size()==3);
 bridge.service(true);bridge.service(true);assert(Serial.output==std::vector<uint8_t>(reply,reply+7));
 // Failed disable retains ownership, so standby cannot use the radio concurrently.
 mockDisableError=259;bridge.service(false);assert(bridge.running());
 mockDisableError=0;Serial.input={1,3,12,0};bridge.service(false);assert(!bridge.running()&&mockRadioState==1&&Serial.input.empty());
 // Restart clears partial host packets; the next HCI Reset remains intact.
 mockTime+=1000;bridge.service(true);Serial.input={1,3};bridge.service(true);
 bridge.service(false);mockTime+=1000;bridge.service(true);Serial.input={1,3,12,0};
 bridge.service(true);bridge.service(true);assert(mockCommands.size()==3);
 if(argc>1){
    std::ifstream file(argv[1],std::ios::binary);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)),{});
    assert(data.size()==70);Serial.output.clear();Serial.room=2048;
    Serial.input=std::deque<uint8_t>(data.begin(),data.end());bridge.setMonitorInterval(3000);
    bridge.service(true);bridge.service(true);
    UsbMonitorProtocol::Sample sample;assert(bridge.takeMonitor(sample));
    assert(sample.running&&sample.uptime==123&&sample.values[0]==25.5f);
    assert(mockCommands.size()==3); // Vendor sample never reached the BT controller.
    assert(Serial.output.size()==25&&std::equal(Serial.output.begin(),Serial.output.begin()+9,std::vector<uint8_t>({4,14,22,1,0xf0,0xfc,0,0xb8,0x0b}).begin()));
 }
 if(argc>2){
    std::ifstream file(argv[2],std::ios::binary);std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)),{});
    Serial.output.clear();Serial.room=2048;Serial.input=std::deque<uint8_t>(data.begin(),data.end());
    mockCanSend=false;bridge.service(true);bridge.service(true);mockCanSend=true;
    UsbInfoProtocol::Info info;assert(bridge.takeInfo(info));assert(info.port==9091&&std::string(info.hostname)=="linux-pc");
    assert(mockCommands.size()==3&&Serial.output==std::vector<uint8_t>({4,14,4,1,0xf1,0xfc,0}));
 }
 Serial.room=2048;Serial.output.clear();Serial.input={1,0xf0,0xfc,0};bridge.service(true);bridge.service(true);
 assert(mockCommands.size()==3&&Serial.output.size()==25&&Serial.output[6]==0x12);
 Serial.output.clear();bridge.service(false);bridge.notifyHostRestart();mockTime+=1000;bridge.service(true);
 assert(Serial.output==std::vector<uint8_t>({4,0x10,1,1}));
 bridge.service(true);assert(Serial.output.size()==4);
 puts("PASS: radio ownership, fragmented HCI, controller backpressure, partial USB writes, failed disable, restart");
}
