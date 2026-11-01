#include <vector>
#include <utility>
#include <cassert>
#include <cstdio>
#define private public
#include "../firmware/lib/XPT2046_Touchscreen/XPT2046_Touchscreen.h"
#undef private
SPIClass SPI;
std::vector<std::pair<int,int>> writes;
int clockLevel=0,readBit=0;
uint16_t response=0xa35c;
void pinMode(uint8_t,int){}
void digitalWrite(uint8_t pin,int value){writes.push_back({pin,value});if(pin==25)clockLevel=value;}
int digitalRead(uint8_t pin){assert(pin==39);assert(clockLevel==HIGH);return (response>>(15-readBit++))&1;}
void delayMicroseconds(unsigned){}
uint32_t millis(){return 10000;}
int main(){
    XPT2046_Touchscreen touch(33);
    assert(touch.beginSoftware(25,39,32));
    assert(writes.back()==std::make_pair(33,HIGH));writes.clear();
    assert(touch.transfer16(0xb190)==response);assert(readBit==16);assert(clockLevel==LOW);
    unsigned count=0;uint16_t sent=0;
    for(auto w:writes)if(w.first==32){sent=(sent<<1)|w.second;count++;}
    assert(count==16);assert(sent==0xb190);
    // A second instance can still use the original hardware SPI path.
    XPT2046_Touchscreen hardware(33);assert(hardware.begin(SPI));assert(hardware.transfer16(0xbeef)==0xbeef);
    puts("PASS: software touch SPI MSB-first transfer, sampled clock phase, idle level and hardware fallback");
}
