#pragma once
#include "Arduino.h"
constexpr int WL_CONNECTED=3;
class IPAddress {uint8_t value[4]{};public:IPAddress()=default;IPAddress(const uint8_t*p){memcpy(value,p,4);}uint8_t operator[](unsigned i)const{return value[i];}};
inline std::vector<uint8_t> mockTcpWritten;
class WiFiClient {bool up=false;public:void stop(){up=false;}void setTimeout(int){}void setNoDelay(bool){}int connect(IPAddress,uint16_t,int){up=true;return 1;}bool connected(){return up;}size_t write(const uint8_t*p,size_t n){mockTcpWritten.insert(mockTcpWritten.end(),p,p+n);return n;}int available(){return 0;}int read(uint8_t*,size_t){return 0;}};
struct WiFiClass{int status(){return WL_CONNECTED;}int hostByName(const char*,IPAddress&){return 1;}};
inline WiFiClass WiFi;
struct ESPClass {unsigned getFreeHeap(){return 200000;}};inline ESPClass ESP;
