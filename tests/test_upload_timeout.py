import pathlib,subprocess,tempfile,unittest

class UploadTimeoutTests(unittest.TestCase):
 def test_production_reader_recovers_from_stalled_upload(self):
  root=pathlib.Path(__file__).parents[1]
  source=(root/'firmware/lib/WebServer/src/Parsing.cpp').read_text()
  start=source.index('int WebServer::_uploadReadByte(')
  end=source.index('bool WebServer::_parseForm(',start)
  method=source[start:end]
  harness=r"""
#include <cstdint>
#include <cassert>
static uint32_t now=0,steps=0;
uint32_t millis(){return now;}
void delay(unsigned n){now+=n;assert(++steps<6000);}
struct WiFiClient {
 bool live=true,stopped=false;uint32_t readyAt=0;int value=-1;
 int available(){return value>=0&&uint32_t(now-readyAt)<0x80000000u;}
 bool connected(){return live;}
 int read(){if(available()){int result=value;value=-1;return result;}return -1;}
 void stop(){stopped=true;live=false;}
};
class WebServer {public:int _uploadReadByte(WiFiClient&);};
"""+method+r"""
int main(){
 WebServer server;
 WiFiClient immediate;immediate.value=42;assert(server._uploadReadByte(immediate)==42&&now==0);
 WiFiClient delayed;delayed.value=7;delayed.readyAt=100;assert(server._uploadReadByte(delayed)==7&&now==100);
 now=0;steps=0;WiFiClient silent;assert(server._uploadReadByte(silent)==-1&&silent.stopped&&now==10000);
 now=0;steps=0;WiFiClient disconnected;disconnected.live=false;assert(server._uploadReadByte(disconnected)==-1&&now==0);
 now=0xfffffff0u;steps=0;WiFiClient wrap;assert(server._uploadReadByte(wrap)==-1&&wrap.stopped&&steps==5000);
}
"""
  with tempfile.TemporaryDirectory() as directory:
   file=pathlib.Path(directory)/'test.cpp';file.write_text(harness);binary=pathlib.Path(directory)/'test'
   subprocess.run(['g++','-std=c++17',str(file),'-o',str(binary)],check=True,capture_output=True)
   subprocess.run([str(binary)],check=True,timeout=3,capture_output=True)

if __name__=='__main__':unittest.main()
