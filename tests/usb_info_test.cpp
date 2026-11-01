#include "../firmware/src/usb_info_protocol.h"
#include "../firmware/src/settings_merge.h"
#include <cassert>
#include <string>
#include <fstream>
#include <vector>
#include <iterator>
#include <cstdio>
int main(int argc,char**argv){
 assert(argc==2);std::ifstream f(argv[1],std::ios::binary);std::vector<uint8_t>b((std::istreambuf_iterator<char>(f)),{});
 UsbInfoProtocol::Info info;assert(b.size()==88&&UsbInfoProtocol::decode(b.data()+4,b.size()-4,info));
 assert(info.ip[0]==192&&info.port==9091&&std::string(info.hostname)=="linux-pc");
 auto bad=b;bad[12]=127;assert(!UsbInfoProtocol::decode(bad.data()+4,84,info));
 bad=b;bad[18]=1;assert(!UsbInfoProtocol::decode(bad.data()+4,84,info));
 assert(!UsbInfoProtocol::decode(b.data()+4,83,info));
 std::string saved="",detected="192.0.2.10";
 assert(RuntimeSettings::fillMissing(saved,detected)&&saved==detected);
 assert(!RuntimeSettings::fillMissing(saved,std::string("192.0.2.20"))&&saved==detected);
 assert(!RuntimeSettings::fillMissing(saved,std::string("")));
 std::string empty="";assert(!RuntimeSettings::fillMissing(empty,std::string("")));
 puts("PASS: Python/C++ USB info framing, invalid metadata rejection, fill-only-missing settings policy");
}
