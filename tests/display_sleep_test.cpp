#include "../firmware/src/display_sleep.h"
#include <cassert>
#include <cstdio>
int main(){
    using DisplaySleep::due;
    assert(!due(false,true,9999,0));
    assert(due(false,true,10000,0));
    assert(!due(true,true,20000,0));
    assert(!due(false,false,20000,0));
    assert(!due(false,true,15000,6000));
    assert(due(false,true,16000,6000));
    const uint32_t start=UINT32_MAX-5000;
    assert(!due(false,true,start+9999,start));
    assert(due(false,true,start+10000,start));
    puts("PASS: delay boundary, relay ON, sleeping, touch reset, timer wrap");
}
