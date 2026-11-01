#pragma once
#include <stdint.h>
#define OUTPUT 1
#define INPUT 0
#define HIGH 1
#define LOW 0
#define FALLING 2
void pinMode(uint8_t,int);
void digitalWrite(uint8_t,int);
int digitalRead(uint8_t);
void delayMicroseconds(unsigned);
uint32_t millis();
inline int digitalPinToInterrupt(uint8_t p){return p;}
inline void attachInterrupt(int,void(*)(),int){}
