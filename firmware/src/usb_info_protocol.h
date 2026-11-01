#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
namespace UsbInfoProtocol {
constexpr uint16_t OPCODE=0xfcf1;
constexpr size_t SIZE=84;
struct Info {uint8_t ip[4],bluetooth[6];uint16_t port;char hostname[64];};
inline bool decode(const uint8_t* p,size_t n,Info& out){
 if(n!=SIZE||memcmp(p,"BC250I1\0",8))return false;
 if(!p[8]||p[8]==127||p[8]>=224)return false;
 uint16_t port=p[12]|uint16_t(p[13])<<8;if(!port)return false;
 bool any=false;for(unsigned i=0;i<6;i++)any|=p[14+i]!=0;
 if(!any||(p[14]&1))return false;
 const uint8_t* end=static_cast<const uint8_t*>(memchr(p+20,0,64));if(!end||p[20]==0)return false;
 for(const uint8_t* q=p+20;q<end;q++)if(!((*q>='A'&&*q<='Z')||(*q>='a'&&*q<='z')||(*q>='0'&&*q<='9')||*q=='-'||*q=='_'||*q=='.'))return false;
 memcpy(out.ip,p+8,4);out.port=port;memcpy(out.bluetooth,p+14,6);memcpy(out.hostname,p+20,64);return true;
}
}
