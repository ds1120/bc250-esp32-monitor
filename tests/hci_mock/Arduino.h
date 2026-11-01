#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <deque>
#include <algorithm>
class String:public std::string {public:using std::string::string;using std::string::operator=;String(int n):std::string(std::to_string(n)){} };
inline uint32_t mockTime=2000;
inline uint32_t millis(){return mockTime;}
struct MockSerial {
 std::deque<uint8_t> input;std::vector<uint8_t> output;int room=2048;
 int available(){return input.size();}int read(){int c=input.front();input.pop_front();return c;}
 int availableForWrite(){return room;}
 size_t write(const uint8_t* data,size_t length){output.insert(output.end(),data,data+length);return length;}
};
inline MockSerial Serial;
struct MockQueue{unsigned max,size;std::deque<std::vector<uint8_t>> values;};
using QueueHandle_t=MockQueue*;using TaskHandle_t=void*;using SemaphoreHandle_t=void*;
constexpr int pdTRUE=1,pdPASS=1;constexpr unsigned portMAX_DELAY=~0u;
inline unsigned pdMS_TO_TICKS(unsigned n){return n;}
inline QueueHandle_t xQueueCreate(unsigned n,unsigned size){return new MockQueue{n,size,{}};}
inline int xQueueSend(QueueHandle_t q,const void* data,unsigned){if(q->values.size()==q->max)return 0;q->values.emplace_back(static_cast<const uint8_t*>(data),static_cast<const uint8_t*>(data)+q->size);return 1;}
inline int xQueueReceive(QueueHandle_t q,void* data,unsigned){if(q->values.empty())return 0;memcpy(data,q->values.front().data(),q->size);q->values.pop_front();return 1;}
inline void xQueueReset(QueueHandle_t q){q->values.clear();}
inline void vTaskDelay(unsigned){}
inline int xTaskCreatePinnedToCore(void(*)(void*),const char*,unsigned,void*,unsigned,TaskHandle_t* task,int){*task=reinterpret_cast<void*>(1);return 1;}

inline int xQueueOverwrite(QueueHandle_t q,const void* data){q->values.clear();return xQueueSend(q,data,0);}
