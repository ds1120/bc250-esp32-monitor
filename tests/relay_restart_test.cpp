#include "../firmware/src/relay_restart.h"
#include <cassert>
#include <cstdio>
int main(){
 RelayRestartTicket t{};
 assert(!RelayRestart::consume(t,true,true));
 RelayRestart::arm(t);assert(RelayRestart::consume(t,true,true));
 assert(!RelayRestart::consume(t,true,true)); // one-shot
 RelayRestart::arm(t);assert(!RelayRestart::consume(t,false,true)); // cold / watchdog
 RelayRestart::arm(t);assert(!RelayRestart::consume(t,true,false)); // hold failed
 RelayRestart::arm(t);t.inverse^=1;assert(!RelayRestart::consume(t,true,true));
 RelayRestart::arm(t);t.level=0;assert(!RelayRestart::consume(t,true,true));
 RelayRestart::arm(t);RelayRestart::clear(t);assert(!RelayRestart::consume(t,true,true));
 for(size_t split=0;split<sizeof(RelayRestart::ABI);split++){
    RelayRestart::AbiScanner scanner;
    scanner.feed(reinterpret_cast<const uint8_t*>(RelayRestart::ABI),split);
    scanner.feed(reinterpret_cast<const uint8_t*>(RelayRestart::ABI)+split,sizeof(RelayRestart::ABI)-1-split);
    assert(scanner.compatible());scanner.reset();assert(!scanner.compatible());
 }
 puts("PASS: planned-only relay resume, one-shot, cold/watchdog rejection, failed hold and corruption");
}
