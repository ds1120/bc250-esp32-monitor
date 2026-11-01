#include "../firmware/src/gamepad_protocol.h"
#include <assert.h>
#include <stdio.h>

int main(){
    uint8_t address[6],expected[]={0x02,0,0,0,0,0x02};
    assert(GamepadProtocol::address("02:00:00:00:00:02",address));
    assert(!memcmp(address,expected,6));
    assert(!GamepadProtocol::address("02:00:00:00:00:0G",address));
    assert(!GamepadProtocol::address("00:00:00:00:00:00",address));
    assert(!GamepadProtocol::address("03:00:00:00:00:02",address));
    uint8_t request[]={4,4,10,0x02,0,0,0,0,0x02,8,0x25,0,1};
    assert(GamepadProtocol::connectionRequest(request,sizeof(request),address));
    assert(!memcmp(address,expected,6));
    assert(!GamepadProtocol::connectionRequest(request,sizeof(request)-1,address));
    request[12]=0;assert(!GamepadProtocol::connectionRequest(request,sizeof(request),address));
    uint8_t ble[]={4,0x3e,12,2,1,0,0,0x02,0,0,0,0,0x02,0,0xd0};
    int count=0;
    auto found=[&](const uint8_t* a,uint8_t type,int8_t rssi){assert(!memcmp(a,expected,6));assert(type==0);assert(rssi==-48);count++;};
    GamepadProtocol::advertisements(ble,sizeof(ble),found);assert(count==1);
    GamepadProtocol::advertisements(ble,sizeof(ble)-1,found);assert(count==1);
    ble[13]=255;GamepadProtocol::advertisements(ble,sizeof(ble),found);assert(count==1);
    uint8_t directed[]={4,0x3e,18,0x0b,1,1,0,0x02,0,0,0,0,0x02,1,1,2,3,4,5,6,0xd0};
    GamepadProtocol::advertisements(directed,sizeof(directed),found);assert(count==2);
    puts("PASS: Classic wake request, BLE advertising, directed advertising and malformed packet boundaries");
}
