#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
namespace UsbMonitorProtocol {
constexpr uint16_t OPCODE=0xfcf0;
constexpr size_t SIZE=66;
constexpr uint32_t STALE_MS=10000;
struct Sample {uint32_t sequence,uptime;uint8_t boot[16];float values[8];bool running;};
inline uint32_t u32(const uint8_t* p){return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24;}
inline bool decode(const uint8_t* p,size_t n,Sample& out){
    if(n!=SIZE||memcmp(p,"BC250M1\0",8))return false;
    out.sequence=u32(p+8);memcpy(out.boot,p+12,16);out.uptime=u32(p+28);
    uint16_t flags=p[64]|uint16_t(p[65])<<8;out.running=flags&256;
    for(unsigned i=0;i<8;i++){
        uint32_t bits=u32(p+32+4*i);float value;memcpy(&value,&bits,4);
        if(!(flags&(1<<i))){out.values[i]=NAN;continue;}
        float minimum=i==1||i==3?-100:0;
        float maximum=i==0||i==2?100:i==1||i==3?250:1048576;
        if(!isfinite(value)||value<minimum||value>maximum)return false;
        out.values[i]=value;
    }
    return true;
}
}
