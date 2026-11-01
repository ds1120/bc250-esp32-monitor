#pragma once
#include <Arduino.h>
inline SemaphoreHandle_t xSemaphoreCreateMutex(){return reinterpret_cast<void*>(1);}
inline int xSemaphoreTake(SemaphoreHandle_t,unsigned){return 1;}
inline void xSemaphoreGive(SemaphoreHandle_t){}
