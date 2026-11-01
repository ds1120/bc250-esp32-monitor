import asyncio,hashlib,importlib.util,json,socket,struct,sys,tempfile,unittest
from pathlib import Path
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).parents[1]/'host'))
import net_protocol as P
import esp_network as N

class ProtocolTests(unittest.TestCase):
 def test_frame_integrity_and_boundaries(self):
  data=P.encode(P.DATA,1,123,45,b'hello')
  self.assertEqual(P.decode(data),(P.DATA,1,123,45,b'hello'))
  for i in range(len(data)):
   bad=bytearray(data);bad[i]^=1
   with self.assertRaises(ValueError):P.decode(bytes(bad))
  with self.assertRaises(ValueError):P.encode(P.DATA,1,1,1,bytes(225))
 def test_ack_demultiplex(self):
  packet=bytes([4,14,9,1,0xf2,0xfc,0])+struct.pack('<HHB',123,5,1)
  self.assertEqual(P.parse_ack(packet),(123,5,1));self.assertIsNone(P.parse_ack(packet[:-1]))

class TransportSchedulingTests(unittest.IsolatedAsyncioTestCase):
 async def test_event_ack_passes_blocked_data_sender(self):
  transport=N.Transport(0);seen=[];blocked=asyncio.Event();release=asyncio.Event()
  async def send(op,stream,data):
   seen.append(op)
   if op==P.DATA:blocked.set();await release.wait()
  transport._send=send
  pending=asyncio.create_task(transport.send(P.DATA,1,b'data'))
  await blocked.wait()
  await asyncio.wait_for(transport.send(P.ACK,data=b'\x01\0'),.2)
  self.assertEqual(seen,[P.DATA,P.ACK]);release.set();await pending

class FakeTransport:
 def __init__(self):self.handler=None;self.udp={};self.frames=[]
 async def send(self,op,stream=0,data=b''):
  self.frames.append((op,stream,data))
  if op==P.CLOSE:await self.handler(P.CLOSED,stream,b'\0')
  if op==P.OPEN:await self.handler(P.OPENED,stream,b'\0')
  if op==P.DATA:await self.handler(P.RECEIVED,stream,data)
  if op==P.UDP:
   ident,total,offset=struct.unpack_from('<HHH',data)
   if offset==0:self.udp[stream]=(ident,bytearray())
   saved,buf=self.udp[stream]
   assert ident==saved and offset==len(buf);buf.extend(data[12:]);await asyncio.sleep(.001)
   if len(buf)==total:
    for start in range(0,total,P.DATA_MAX-12):
     reply=struct.pack('<HHH',ident,total,start)+data[6:12]+buf[start:start+P.DATA_MAX-12]
     await self.handler(P.DATAGRAM,stream,reply)

class SocksTests(unittest.IsolatedAsyncioTestCase):
 async def asyncSetUp(self):
  self.transport=FakeTransport();self.bridge=N.SocksBridge(self.transport,0);await self.bridge.start();self.port=self.bridge.server.sockets[0].getsockname()[1]
 async def asyncTearDown(self):await self.bridge.close()
 async def connect(self,cmd=1):
  reader,writer=await asyncio.open_connection('127.0.0.1',self.port)
  writer.write(b'\x05\x01\0');await writer.drain();self.assertEqual(await reader.readexactly(2),b'\x05\0')
  writer.write(bytes([5,cmd,0,1])+socket.inet_aton('1.1.1.1')+struct.pack('!H',443));await writer.drain();reply=await reader.readexactly(10);self.assertEqual(reply[:2],b'\x05\0');return reader,writer,reply
 async def test_tcp_stream_preserves_bytes_across_frames(self):
  reader,writer,_=await self.connect();data=bytes(range(256))*20
  writer.write(data);await writer.drain();received=await asyncio.wait_for(reader.readexactly(len(data)),5)
  self.assertEqual(hashlib.sha256(received).digest(),hashlib.sha256(data).digest())
  writer.close();await writer.wait_closed()
 async def test_udp_fragmentation_and_concurrent_datagrams(self):
  reader,writer,reply=await self.connect(3);port=struct.unpack('!H',reply[8:10])[0]
  sock=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);sock.setblocking(False);sock.bind(('127.0.0.1',0));loop=asyncio.get_running_loop()
  payloads=[b'a'*700,b'b'*900]
  for payload in payloads:await loop.sock_sendto(sock,b'\0\0\0\x01'+socket.inet_aton('1.1.1.1')+struct.pack('!H',53)+payload,('127.0.0.1',port))
  received=[]
  for _ in payloads:
   data,_=await asyncio.wait_for(loop.sock_recvfrom(sock,2048),5);received.append(data[10:])
  self.assertEqual(received,payloads);sock.close();writer.close();await writer.wait_closed()

if __name__=='__main__':unittest.main()
