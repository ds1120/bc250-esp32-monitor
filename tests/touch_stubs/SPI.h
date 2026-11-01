#pragma once
#include <stdint.h>
#define MSBFIRST 1
#define SPI_MODE0 0
struct SPISettings {SPISettings(int,int,int){}};
class SPIClass {public:void begin(){};void beginTransaction(SPISettings){};void endTransaction(){};uint8_t transfer(uint8_t x){return x;}uint16_t transfer16(uint16_t x){return x;}};
extern SPIClass SPI;
