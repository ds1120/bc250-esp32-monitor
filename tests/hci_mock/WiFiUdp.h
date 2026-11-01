#pragma once
#include "WiFi.h"
class WiFiUDP {public:void stop(){}int begin(int){return 1;}int beginPacket(IPAddress,uint16_t){return 1;}size_t write(const uint8_t*,size_t n){return n;}int endPacket(){return 1;}int parsePacket(){return 0;}int available(){return 0;}int read(){return -1;}int read(uint8_t*,size_t){return 0;}IPAddress remoteIP(){return IPAddress();}uint16_t remotePort(){return 53;}};
