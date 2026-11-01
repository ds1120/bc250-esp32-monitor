#!/usr/bin/python3
"""USB-HCI socket relay + loopback SOCKS5, optionally exposed as a TUN link.

No serial port is opened. All traffic shares the configured Bluetooth H4 device.
Internet routes are added only while the explicit emergency service is running.
"""
import argparse,asyncio,contextlib,ipaddress,json,logging,os,secrets,signal,socket,struct,subprocess,time,uuid
from pathlib import Path
import sys
sys.path.insert(0,str(Path(__file__).resolve().parent))
import net_protocol as P
from usb_monitor import find_controller

LOG=logging.getLogger('bc250-network')
CONFIG=Path('/etc/bc250-esp-network.json')
STATUS=Path('/run/bc250-esp-network/status.json')

class Transport:
 def __init__(self,index):
  self.index=index;self.session=secrets.randbelow(65535)+1;self.sequence=0
  self.sock=None;self.lock=asyncio.Lock();self.acks={};self.welcome=None;self.failure=None;self.handler=None
  self.event_pending=set();self.last_event=0;self.receiver=None
 async def start(self):
  loop=asyncio.get_running_loop();self.failure=loop.create_future();self.welcome=loop.create_future()
  self.sock=socket.socket(socket.AF_BLUETOOTH,socket.SOCK_RAW,socket.BTPROTO_HCI)
  self.sock.setblocking(False);self.sock.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,262144)
  self.sock.setsockopt(0,2,struct.pack('<IIIH2x',1<<4,(1<<14)|(1<<16),1<<31,P.OPCODE))
  self.sock.bind((self.index,0));self.receiver=asyncio.create_task(self.receive())
  await self.send(P.HELLO)
  reply=await asyncio.wait_for(asyncio.shield(self.welcome),5)
  if len(reply)<4 or reply[0]!=1:raise RuntimeError('Unsupported ESP32 network protocol')
 async def send(self,op,stream=0,data=b''):
  # Event acknowledgements must pass a DATA sender waiting for queue space.
  # Otherwise a full ESP queue and its pending event would wait on each other.
  if op==P.ACK:return await self._send(op,stream,data)
  async with self.lock:return await self._send(op,stream,data)
 async def _send(self,op,stream,data):
  if self.failure is not None and self.failure.done():raise ConnectionError('HCI network link stopped')
  self.sequence=self.sequence%65535+1;seq=self.sequence
  packet=P.command(op,stream,self.session,seq,data)
  future=asyncio.get_running_loop().create_future();self.acks[seq]=future
  try:
   started=time.monotonic();timeouts=0
   while True:
    await asyncio.get_running_loop().sock_sendall(self.sock,packet)
    try:status=await asyncio.wait_for(asyncio.shield(future),1.5)
    except asyncio.TimeoutError:
     timeouts+=1
     if timeouts>=3:raise ConnectionError('ESP32 network acknowledgement timed out')
     continue
    if status==0:return
    if status==1 and time.monotonic()-started<6:
     future=asyncio.get_running_loop().create_future();self.acks[seq]=future;await asyncio.sleep(.025);continue
    raise ConnectionError(f'ESP32 network command failed: {status}')
  finally:self.acks.pop(seq,None)
 async def receive(self):
  try:
   while True:
    packet=await asyncio.get_running_loop().sock_recv(self.sock,2048)
    if packet==b'\x04\x10\x01\x01':raise ConnectionResetError('ESP32 restarted')
    ack=P.parse_ack(packet)
    if ack:
     session,seq,status=ack;future=self.acks.get(seq)
     if session==self.session and future is not None and not future.done():future.set_result(status)
     continue
    if len(packet)<3 or packet[:2]!=b'\x04\xff' or packet[2]!=len(packet)-3:continue
    try:op,stream,session,seq,data=P.decode(packet[3:])
    except ValueError:continue
    if session!=self.session or not seq:continue
    if seq in self.event_pending:continue
    if seq==self.last_event:
     asyncio.create_task(self.ack_event(seq));continue
    self.event_pending.add(seq);asyncio.create_task(self.event(op,stream,seq,data))
  except asyncio.CancelledError:raise
  except Exception as exc:
   LOG.error('USB network link stopped: %s',exc)
   if not self.failure.done():self.failure.set_result(str(exc))
   for f in self.acks.values():
    if not f.done():f.set_exception(ConnectionError(str(exc)))
 async def ack_event(self,seq):
  try:await self.send(P.ACK,data=struct.pack('<H',seq))
  except (OSError,RuntimeError,ConnectionError):pass
 async def event(self,op,stream,seq,data):
  try:
   if op==P.WELCOME:
    if not self.welcome.done():self.welcome.set_result(data)
   elif self.handler:await self.handler(op,stream,data)
   self.last_event=seq
   await self.ack_event(seq)
  except Exception as exc:
   LOG.error('Network event processing failed: %s',exc)
   if not self.failure.done():self.failure.set_result(str(exc))
  finally:self.event_pending.discard(seq)
 async def close(self):
  if self.sock is not None:
   with contextlib.suppress(Exception):await asyncio.wait_for(self.send(P.DISABLE),1)
  if self.receiver:self.receiver.cancel()
  if self.receiver:
   with contextlib.suppress(asyncio.CancelledError):await self.receiver
  if self.sock:self.sock.close()

class Channel:
 def __init__(self,stream,writer=None,udp=False):
  loop=asyncio.get_running_loop();self.stream=stream;self.writer=writer;self.udp=udp
  self.opened=loop.create_future();self.ready=asyncio.Event();self.closed=asyncio.Event();self.endpoint=None;self.peer=None;self.udp_id=0
  self.udp_lock=asyncio.Lock();self.datagram_id=None;self.datagram=bytearray();self.datagram_total=0

class UDPProtocol(asyncio.DatagramProtocol):
 def __init__(self,bridge,channel):self.bridge=bridge;self.channel=channel
 def connection_made(self,transport):self.channel.endpoint=transport
 def datagram_received(self,data,addr):
  channel=self.channel
  if addr[0]!='127.0.0.1' or len(data)<10 or data[:4]!=b'\0\0\0\x01':return
  if channel.peer is None:channel.peer=addr
  if addr!=channel.peer:return
  asyncio.create_task(self.bridge.send_datagram(channel,data[4:8],data[8:10],data[10:]))

class SocksBridge:
 def __init__(self,transport,port):self.transport=transport;self.port=port;self.channels={};self.server=None;self.tasks=set();self.bytes_tx=0;self.bytes_rx=0
 async def start(self):
  self.transport.handler=self.event
  self.server=await asyncio.start_server(self.accept,'127.0.0.1',self.port,limit=16384)
 def allocate(self,writer,udp):
  for n in range(1,P.STREAMS+1):
   if n not in self.channels:
    c=Channel(n,writer,udp);self.channels[n]=c;return c
  raise RuntimeError('All emergency network streams are busy')
 async def event(self,op,stream,data):
  c=self.channels.get(stream)
  if c is None:return
  if op==P.OPENED:
   if not c.opened.done():c.opened.set_result(data[0] if data else 1)
  elif op==P.RECEIVED:
   await asyncio.wait_for(c.ready.wait(),5)
   if c.writer is not None:c.writer.write(data);await c.writer.drain();self.bytes_rx+=len(data)
  elif op==P.CLOSED:
   c.closed.set()
   if c.writer:c.writer.close()
  elif op==P.DATAGRAM and c.udp and len(data)>12:
   ident,total,offset=struct.unpack_from('<HHH',data);piece=data[12:]
   if total>P.UDP_MAX:return
   if offset==0:c.datagram_id=ident;c.datagram=bytearray();c.datagram_total=total
   if ident!=c.datagram_id or total!=c.datagram_total or offset!=len(c.datagram):raise ValueError('Invalid UDP fragment order')
   c.datagram.extend(piece)
   if len(c.datagram)>total:raise ValueError('Invalid UDP fragment length')
   if len(c.datagram)==total and c.peer and c.endpoint:
    c.endpoint.sendto(b'\0\0\0\x01'+data[6:12]+bytes(c.datagram),c.peer);self.bytes_rx+=total
 async def send_datagram(self,c,ip,port,data):
  if not data or len(data)>P.UDP_MAX:return
  async with c.udp_lock:
   c.udp_id=c.udp_id%65535+1
   try:
    for offset in range(0,len(data),P.DATA_MAX-12):
     payload=struct.pack('<HHH',c.udp_id,len(data),offset)+ip+port+data[offset:offset+P.DATA_MAX-12]
     await self.transport.send(P.UDP,c.stream,payload)
    self.bytes_tx+=len(data)
   except Exception as e:LOG.debug('UDP send failed: %s',e)
 async def accept(self,reader,writer):
  task=asyncio.current_task();self.tasks.add(task);channel=None;success=False
  try:
   version,n=await asyncio.wait_for(reader.readexactly(2),5)
   methods=await reader.readexactly(n)
   if version!=5 or 0 not in methods:writer.write(b'\x05\xff');await writer.drain();return
   writer.write(b'\x05\0');await writer.drain()
   version,cmd,reserved,atyp=await reader.readexactly(4)
   if version!=5 or reserved!=0:raise ValueError('Invalid SOCKS request')
   if atyp==1:address=b'\x01'+await reader.readexactly(6)
   elif atyp==3:
    count=(await reader.readexactly(1))[0]
    if not 1<=count<=63:raise ValueError('SOCKS hostname too long')
    address=b'\x03'+bytes([count])+await reader.readexactly(count+2)
   else:writer.write(b'\x05\x08\0\x01'+bytes(6));await writer.drain();return
   if cmd not in (1,3):writer.write(b'\x05\x07\0\x01'+bytes(6));await writer.drain();return
   channel=self.allocate(writer,cmd==3)
   await self.transport.send(P.OPEN,channel.stream,b'\x02' if cmd==3 else b'\x01'+address)
   code=await asyncio.wait_for(asyncio.shield(channel.opened),5)
   if code:writer.write(bytes([5,code,0,1])+bytes(6));await writer.drain();return
   if cmd==3:
    endpoint,_=await asyncio.get_running_loop().create_datagram_endpoint(lambda:UDPProtocol(self,channel),local_addr=('127.0.0.1',0))
    port=endpoint.get_extra_info('sockname')[1];reply=b'\x05\0\0\x01\x7f\0\0\x01'+struct.pack('!H',port)
   else:reply=b'\x05\0\0\x01'+bytes(6)
   writer.write(reply);await writer.drain();success=True;channel.ready.set()
   if cmd==3:
    while await reader.read(256):pass
   else:
    while True:
     data=await reader.read(P.DATA_MAX)
     if not data:break
     await self.transport.send(P.DATA,channel.stream,data);self.bytes_tx+=len(data)
  except (OSError,ValueError,RuntimeError,asyncio.IncompleteReadError,asyncio.TimeoutError) as exc:
   LOG.debug('SOCKS connection closed: %s',exc)
   if not success:
    with contextlib.suppress(Exception):writer.write(b'\x05\x01\0\x01'+bytes(6));await writer.drain()
  finally:
   if channel:
    if channel.endpoint:channel.endpoint.close()
    if not channel.closed.is_set():
     with contextlib.suppress(Exception):
      await asyncio.wait_for(self.transport.send(P.CLOSE,channel.stream),1)
      await asyncio.wait_for(channel.closed.wait(),2)
    self.channels.pop(channel.stream,None)
   writer.close()
   with contextlib.suppress(Exception):await writer.wait_closed()
   self.tasks.discard(task)
 async def close(self):
  if self.server:self.server.close();await self.server.wait_closed()
  for task in list(self.tasks):task.cancel()
  if self.tasks:await asyncio.gather(*list(self.tasks),return_exceptions=True)

class NetworkLink:
 def __init__(self,config,owner_file=Path('/run/bc250-esp-network/owner.json')):self.config=config;self.uuid=None;self.process=None;self.interface=config.get('interface','espnet0');self.active=False;self.owner_file=owner_file
 def run(self,*args,check=True):return subprocess.run(args,check=check,capture_output=True,text=True,timeout=20)
 def start(self,port):
  interface=self.interface
  if not __import__('re').fullmatch('[A-Za-z][A-Za-z0-9_-]{0,14}',interface):raise ValueError('Invalid interface name')
  if self.owner_file.exists():
   previous=json.loads(self.owner_file.read_text());old_uuid=previous.get('uuid','');pid=previous.get('pid',0)
   if not __import__('re').fullmatch('[0-9a-f-]{36}',old_uuid):raise RuntimeError('Invalid emergency ownership record')
   if isinstance(pid,int) and pid>0 and Path(f'/proc/{pid}').exists():raise RuntimeError('Another emergency network process still owns the interface')
   # Only recover a UUID from our root-owned record, never a connection selected by name.
   self.run('/usr/bin/nmcli','connection','down','uuid',old_uuid,check=False)
   removed=self.run('/usr/bin/nmcli','connection','delete','uuid',old_uuid,check=False)
   if removed.returncode not in (0,10):raise RuntimeError('Cannot remove stale emergency profile')
   self.owner_file.unlink()
  if Path('/sys/class/net',interface).exists():raise RuntimeError('Emergency interface already exists; refusing to alter it')
  # This service exclusively creates/removes its own profile. Never change a LAN profile.
  routes=json.loads(self.run('/usr/bin/ip','-j','-4','route','show','table','all').stdout)
  networks=[]
  for route in routes:
   if route.get('dst') not in (None,'default'):
    with contextlib.suppress(ValueError):networks.append(ipaddress.ip_network(route['dst'],strict=False))
  subnet=next((ipaddress.ip_network(f'198.18.{n}.0/30') for n in range(256) if not any(ipaddress.ip_network(f'198.18.{n}.0/30').overlaps(existing) for existing in networks)),None)
  if subnet is None:raise RuntimeError('No free emergency subnet; configure a non-conflicting range')
  address=str(next(subnet.hosts()))+'/30';self.uuid=str(uuid.uuid4())
  try:
   self.run('/usr/bin/nmcli','connection','add','type','tun','mode','tun','ifname',interface,'con-name','BC250 ESP32 emergency','connection.uuid',self.uuid,'connection.autoconnect','no','ipv4.method','manual','ipv4.addresses',address,'ipv4.routes','0.0.0.0/0 0.0.0.0 900','ipv4.route-metric','900','ipv4.dns','1.1.1.1,8.8.8.8','ipv4.ignore-auto-dns','yes','ipv6.method','disabled')
   self.owner_file.parent.mkdir(parents=True,exist_ok=True)
   self.owner_file.write_text(json.dumps({'uuid':self.uuid,'interface':interface,'pid':os.getpid()}));self.owner_file.chmod(0o600)
   self.run('/usr/bin/nmcli','connection','up','uuid',self.uuid)
   binary=self.config.get('tun2socks','/usr/local/lib/bc250/tun2socks')
   self.process=subprocess.Popen([binary,'-device','tun://'+interface,'-proxy',f'socks5://127.0.0.1:{port}','-mtu','1200','-loglevel','warn'])
   self.active=True
  except Exception:self.close();raise
 def close(self):
  if self.process:
   self.process.terminate()
   try:self.process.wait(timeout=4)
   except subprocess.TimeoutExpired:self.process.kill();self.process.wait()
   self.process=None
  if self.uuid:
   with contextlib.suppress(OSError,subprocess.SubprocessError):self.run('/usr/bin/nmcli','connection','down','uuid',self.uuid,check=False)
   removed=None
   with contextlib.suppress(OSError,subprocess.SubprocessError):removed=self.run('/usr/bin/nmcli','connection','delete','uuid',self.uuid,check=False)
   if removed is not None and removed.returncode in (0,10):self.owner_file.unlink(missing_ok=True)
   self.uuid=None
  self.active=False

async def main_async(args):
 config=json.loads(args.config.read_text()) if args.config.exists() else {}
 bt=json.loads(Path(config.get('bluetooth_config','/etc/bc250-esp-bluetooth.json')).read_text())
 index=find_controller(bt['device'])
 if index is None:raise RuntimeError('Configured ESP32 Bluetooth transport is unavailable')
 port=int(config.get('proxy_port',10808))
 if not 1024<=port<=65535:raise ValueError('Invalid proxy port')
 transport=Transport(index);bridge=SocksBridge(transport,port);link=NetworkLink(config);stop=asyncio.Event()
 for sig in (signal.SIGINT,signal.SIGTERM):asyncio.get_running_loop().add_signal_handler(sig,stop.set)
 STATUS.parent.mkdir(parents=True,exist_ok=True)
 def status(active,error=''):
  tmp=STATUS.with_suffix('.tmp');tmp.write_text(json.dumps({'active':active,'interface':link.interface if link.active else None,'proxy_port':port,'tcp_streams':len(bridge.channels),'bytes_to_wifi':bridge.bytes_tx,'bytes_from_wifi':bridge.bytes_rx,'error':error}));tmp.chmod(0o644);tmp.replace(STATUS)
 try:
  await transport.start();await bridge.start()
  if not args.proxy_only:await asyncio.to_thread(link.start,port)
  await transport.send(P.READY);status(True);LOG.info('ESP32 emergency internet ready; Bluetooth remains active')
  async def heartbeat():
   while True:
    await asyncio.sleep(4)
    if link.process and link.process.poll() is not None:raise RuntimeError('tun2socks exited')
    await transport.send(P.PING);status(True)
  beat=asyncio.create_task(heartbeat());stopped=asyncio.create_task(stop.wait())
  done,_=await asyncio.wait([beat,stopped,transport.failure],return_when=asyncio.FIRST_COMPLETED)
  if beat in done:beat.result()
  if transport.failure in done:raise ConnectionError(transport.failure.result())
  beat.cancel();stopped.cancel()
 finally:
  await asyncio.to_thread(link.close);await bridge.close();await transport.close();status(False)

def main():
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--config',type=Path,default=CONFIG);parser.add_argument('--proxy-only',action='store_true',help='Verify transport without creating network routes');args=parser.parse_args()
 if os.geteuid()!=0:parser.error('Root is needed for the HCI socket and network interface')
 logging.basicConfig(level=logging.INFO,format='%(asctime)s %(levelname)s %(message)s')
 asyncio.run(main_async(args))
if __name__=='__main__':main()
