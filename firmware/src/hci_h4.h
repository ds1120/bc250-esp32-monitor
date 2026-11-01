#pragma once
#include <stdint.h>
#include <stddef.h>

// Linux-to-controller H4 packets. No text commands are accepted on this channel.
class HciH4Parser {
public:
    static constexpr size_t CAPACITY=1030;
private:
    uint8_t data_[CAPACITY]={};
    size_t used_=0,header_=0,total_=0;
    bool ready_=false;
public:
    void reset(){used_=header_=total_=0;ready_=false;}
    bool ready()const{return ready_;}
    const uint8_t* data()const{return data_;}
    size_t size()const{return used_;}
    // false means an invalid byte/header; caller may count and continue scanning.
    bool push(uint8_t byte){
        if(ready_)return false;
        if(!used_){
            header_=byte==1?4:byte==2?5:byte==3?4:0;
            if(!header_)return false;
        }
        if(used_>=CAPACITY){reset();return false;}
        data_[used_++]=byte;
        if(used_==header_){
            size_t payload=data_[0]==2?(size_t(data_[3])|(size_t(data_[4])<<8)):data_[3];
            total_=header_+payload;
            if(total_>CAPACITY){reset();return false;}
        }
        if(total_&&used_==total_)ready_=true;
        return true;
    }
    bool partial()const{return used_&&!ready_;}
};
