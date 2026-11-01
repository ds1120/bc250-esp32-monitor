#include "../firmware/src/hci_h4.h"
#include <cassert>
#include <vector>
#include <cstdio>
int main(){
    HciH4Parser p;
    // Real HCI Reset, fragmented exactly as USB serial may deliver it.
    const uint8_t reset[]={1,3,12,0};
    for(unsigned i=0;i<4;i++){assert(p.push(reset[i]));assert(p.ready()==(i==3));}
    assert(p.size()==4&&p.data()[1]==3);assert(!p.push(1));p.reset();
    // ACL payload bytes may look like H4 headers. Do not split them.
    const uint8_t acl[]={2,1,0,4,0,1,2,3,4};
    for(auto c:acl){assert(p.push(c));}assert(p.ready()&&p.size()==sizeof(acl));p.reset();
    // Maximum supported ACL and rejection of oversized declared payloads.
    std::vector<uint8_t> big={2,1,0,1,4};big.resize(HciH4Parser::CAPACITY,0x55);
    for(auto c:big){assert(p.push(c));}assert(p.ready());p.reset();
    for(auto c:std::vector<uint8_t>{2,1,0,255})assert(p.push(c));
    assert(!p.push(255)&&!p.partial());
    // Noise / controller event input never becomes a PC console command.
    assert(!p.push('s'));assert(!p.push(4));
    // SCO frame and explicit timeout/reset of a partial packet.
    for(auto c:std::vector<uint8_t>{3,1,0,2,42,43}){assert(p.push(c));}assert(p.ready());p.reset();
    assert(p.push(1)&&p.partial());p.reset();assert(!p.partial());
    for(auto c:reset){assert(p.push(c));}assert(p.ready());
    puts("PASS: H4 Reset/ACL/SCO, fragmentation, packet bounds, noise, recovery");
}
