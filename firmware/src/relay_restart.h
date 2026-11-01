#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

// RTC RAM, one-shot permission to resume HIGH after our own software restart.
// Never restores HIGH after cold start, brownout, watchdog or external reset.
struct RelayRestartTicket {uint32_t magic,inverse,level;};
namespace RelayRestart {
constexpr uint32_t MAGIC=0x524C5935;
constexpr char ABI[]="BC250-RELAY-HOLD1@50000200";
class AbiScanner {
    size_t matched=0;bool found=false;
public:
    void reset(){matched=0;found=false;}
    void feed(const uint8_t* bytes,size_t size){
        for(size_t i=0;i<size&&!found;i++){
            if(bytes[i]==static_cast<uint8_t>(ABI[matched]))matched++;
            else matched=bytes[i]==static_cast<uint8_t>(ABI[0])?1:0;
            if(matched==sizeof(ABI)-1)found=true;
        }
    }
    bool compatible()const{return found;}
};
inline void clear(volatile RelayRestartTicket& t){t.magic=0;t.inverse=0;t.level=0;}
inline void arm(volatile RelayRestartTicket& t){t.level=1;t.inverse=~MAGIC;t.magic=MAGIC;}
inline bool consume(volatile RelayRestartTicket& t,bool softwareReset,bool padHeldHigh){
    bool keep=softwareReset&&padHeldHigh&&t.magic==MAGIC&&t.inverse==~MAGIC&&t.level==1;
    clear(t);return keep;
}
}
