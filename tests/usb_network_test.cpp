#include "../firmware/src/usb_network_bridge.h"
#include <cassert>
#include <cstdio>
#include <vector>
using namespace UsbNetworkProtocol;
static std::vector<std::vector<uint8_t>> events;
static bool emit(const uint8_t*p,size_t n){events.emplace_back(p,p+n);return true;}
struct UsbNetworkBridgeTestAccess {static void tick(UsbNetworkBridge&b){b.tick();}};
int main(){
 uint8_t packet[260];Frame f;uint16_t session,sequence;
 size_t n=encode(packet,HELLO,0,123,1,nullptr,0);
 assert(decode(packet,n,f)&&commandValid(f));auto bad=std::vector<uint8_t>(packet,packet+n);bad[6]^=1;assert(!decode(bad.data(),bad.size(),f));
 assert(!decode(packet,n-1,f));assert(!encode(packet,DATA,1,1,1,packet,225));
 UsbNetworkBridge b;b.begin(emit);b.setAllowed(false);assert(b.accept(packet,n,session,sequence)==4);b.setAllowed(true);
 assert(b.accept(packet,n,session,sequence)==0);assert(b.accept(packet,n,session,sequence)==0);
 UsbNetworkBridgeTestAccess::tick(b);UsbNetworkBridgeTestAccess::tick(b);assert(events.size()==1);
 assert(decode(events.back().data()+3,events.back().size()-3,f)&&f.op==WELCOME&&f.session==123);
 auto first=events.back();mockTime+=1001;UsbNetworkBridgeTestAccess::tick(b);assert(events.size()==2&&events.back()==first);
 uint8_t ack[2];put16(ack,f.sequence);n=encode(packet,ACK,0,123,2,ack,2);assert(b.accept(packet,n,session,sequence)==0);UsbNetworkBridgeTestAccess::tick(b);
 uint8_t address[]={1,1,1,1,1,1,0,80};n=encode(packet,OPEN,1,123,3,address,sizeof(address));assert(b.accept(packet,n,session,sequence)==0);UsbNetworkBridgeTestAccess::tick(b);UsbNetworkBridgeTestAccess::tick(b);
 assert(decode(events.back().data()+3,events.back().size()-3,f)&&f.op==OPENED&&f.data[0]==0);
 put16(ack,f.sequence);n=encode(packet,ACK,0,123,4,ack,2);assert(b.accept(packet,n,session,sequence)==0);UsbNetworkBridgeTestAccess::tick(b);
 const uint8_t data[]={10,20,30};n=encode(packet,DATA,1,123,5,data,3);assert(b.accept(packet,n,session,sequence)==0);auto saved=std::vector<uint8_t>(packet,packet+n);size_t an=encode(packet,ACK,0,123,6,ack,2);assert(b.accept(packet,an,session,sequence)==0);assert(b.accept(saved.data(),saved.size(),session,sequence)==0);UsbNetworkBridgeTestAccess::tick(b);UsbNetworkBridgeTestAccess::tick(b);assert(mockTcpWritten==std::vector<uint8_t>({10,20,30}));
 n=encode(packet,DATA,1,124,6,data,3);assert(b.accept(packet,n,session,sequence)==4);
 puts("PASS: network CRC/framing, session isolation, duplicate suppression, event retransmit and TCP data delivery");
}
