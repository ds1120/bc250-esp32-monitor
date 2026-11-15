#pragma once
#include <stdint.h>
namespace DisplaySleep {
constexpr uint32_t DELAY_MS=10000;
inline bool due(bool relayHigh,bool awake,uint32_t now,uint32_t activityAt){
    return !relayHigh&&awake&&uint32_t(now-activityAt)>=DELAY_MS;
}
}
