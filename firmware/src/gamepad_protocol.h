#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace GamepadProtocol {
inline bool address(const char* value,uint8_t out[6]){
    if(!value||strlen(value)!=17)return false;
    for(int i=0;i<6;i++){
        unsigned n=0;
        for(int j=0;j<2;j++){
            char c=value[i*3+j];int h=c>='0'&&c<='9'?c-'0':c>='A'&&c<='F'?c-'A'+10:c>='a'&&c<='f'?c-'a'+10:-1;
            if(h<0)return false;
            n=(n<<4)|h;
        }
        out[i]=n;if(i<5&&value[i*3+2]!=':')return false;
    }
    uint8_t nonzero=0;for(int i=0;i<6;i++)nonzero|=out[i];
    return nonzero&&!(out[0]&1);
}
inline bool event(const uint8_t* data,size_t length,uint8_t code){
    return data&&length>=3&&data[0]==4&&data[1]==code&&static_cast<size_t>(data[2])+3<=length;
}
inline bool connectionRequest(const uint8_t* data,size_t length,uint8_t address[6]){
    if(!event(data,length,4)||data[2]<10||data[12]!=1)return false;
    for(int i=0;i<6;i++)address[i]=data[8-i];
    return true;
}
template<class Callback> inline void advertisements(const uint8_t* data,size_t length,Callback callback){
    if(!event(data,length,0x3e)||data[2]<2)return;
    const size_t end=static_cast<size_t>(data[2])+3;
    size_t pos=5;uint8_t count=data[4];
    if(data[3]==2){
        for(unsigned i=0;i<count;i++){
            if(pos+9>end)return;
            uint8_t type=data[pos],addressType=data[pos+1],address[6];
            for(int j=0;j<6;j++)address[j]=data[pos+7-j];
            size_t size=data[pos+8];
            if(pos+10+size>end)return;
            int8_t rssi=static_cast<int8_t>(data[pos+9+size]);
            if(type<=4)callback(address,addressType,rssi);
            pos+=10+size;
        }
    }else if(data[3]==0x0b){
        for(unsigned i=0;i<count;i++){
            if(pos+16>end)return;
            uint8_t address[6];for(int j=0;j<6;j++)address[j]=data[pos+7-j];
            callback(address,data[pos+1],static_cast<int8_t>(data[pos+15]));
            pos+=16;
        }
    }
}
}
