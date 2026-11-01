#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
namespace UsbNetworkProtocol {
constexpr uint16_t OPCODE=0xfcf2;
constexpr unsigned DATA_MAX=224,STREAMS=3,UDP_MAX=1472;
enum Op:uint8_t{HELLO=1,OPEN=2,DATA=3,CLOSE=4,ACK=5,UDP=6,DISABLE=7,PING=8,READY=9};
enum Event:uint8_t{WELCOME=0x80,OPENED=0x81,RECEIVED=0x82,CLOSED=0x83,DATAGRAM=0x84,CONTROL=0x90};
struct Frame {uint8_t op,stream;uint16_t session,sequence,size;uint8_t data[DATA_MAX];};
inline uint16_t u16(const uint8_t*p){return p[0]|uint16_t(p[1])<<8;}
inline void put16(uint8_t*p,uint16_t v){p[0]=v;p[1]=v>>8;}
inline uint16_t crc(const uint8_t*p,size_t n){uint16_t c=0xffff;while(n--){c^=uint16_t(*p++)<<8;for(int i=0;i<8;i++)c=(c&0x8000)?uint16_t(c<<1)^0x1021:uint16_t(c<<1);}return c;}
inline size_t encode(uint8_t*out,uint8_t op,uint8_t stream,uint16_t session,uint16_t seq,const uint8_t*data,size_t n){
 if(n>DATA_MAX)return 0;out[0]='B';out[1]='N';out[2]=1;out[3]=op;out[4]=stream;put16(out+5,session);put16(out+7,seq);if(n)memcpy(out+9,data,n);put16(out+9+n,crc(out,9+n));return 11+n;
}
inline bool decode(const uint8_t*p,size_t n,Frame&out){
 if(n<11||n>11+DATA_MAX||p[0]!='B'||p[1]!='N'||p[2]!=1||u16(p+n-2)!=crc(p,n-2))return false;
 out.op=p[3];out.stream=p[4];out.session=u16(p+5);out.sequence=u16(p+7);out.size=n-11;if(out.size)memcpy(out.data,p+9,out.size);return true;
}
inline bool commandValid(const Frame&f){
 if(!f.session||!f.sequence||f.stream>STREAMS)return false;
 switch(f.op){
 case HELLO:case DISABLE:case PING:case READY:return f.stream==0&&f.size==0;
 case ACK:return f.stream==0&&f.size==2;
 case OPEN:if(!f.stream||!f.size)return false;if(f.data[0]==2)return f.size==1;if(f.data[0]!=1||f.size<3)return false;if(f.data[1]==1)return f.size==8&&(f.data[6]||f.data[7]);if(f.data[1]==3)return f.size>=6&&f.data[2]>0&&f.data[2]<=63&&f.size==size_t(f.data[2])+5&&(f.data[f.size-2]||f.data[f.size-1]);return false;
 case DATA:return f.stream&&f.size>0;
 case CLOSE:return f.stream&&f.size==0;
 case UDP:if(!f.stream||f.size<=12)return false;return u16(f.data+2)>0&&u16(f.data+2)<=UDP_MAX&&u16(f.data+4)+f.size-12<=u16(f.data+2)&&(f.data[10]||f.data[11]);
 default:return false;
 }
}
}
