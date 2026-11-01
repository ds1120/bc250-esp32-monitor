#!/usr/bin/env python3
"""LAN-only BC250 status and authenticated, confirmed OS power requests."""
import argparse
import hmac
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import subprocess
import threading
import time
import hashlib
import ipaddress
import re
import socket
import struct
import configparser
import uuid

try:
    from usb_monitor import UsbMonitorPublisher,find_controller,adapter_address
except ImportError:
    UsbMonitorPublisher = None
    find_controller = adapter_address = None


class GamepadRegistry:
    """Export only supported, bonded controllers from this host's BlueZ store."""
    def __init__(self, root=Path('/var/lib/bluetooth')):
        self.root=root

    def read(self):
        adapters=[]
        preferred=None
        if self.root==Path('/var/lib/bluetooth') and find_controller is not None:
            try:
                device=json.loads(Path('/etc/bc250-esp-bluetooth.json').read_text())['device']
                index=find_controller(device)
                if index is not None:preferred=adapter_address(index)
            except (OSError,ValueError,KeyError,subprocess.SubprocessError):pass
        for adapter in sorted(self.root.glob('*')):
            if not re.fullmatch(r'(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}',adapter.name):continue
            controllers=[]
            for folder in sorted(adapter.glob('*')):
                if not re.fullmatch(r'(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}',folder.name):continue
                info=configparser.ConfigParser(interpolation=None)
                try:
                    if not info.read(folder/'info'):continue
                except (OSError,configparser.Error):continue
                if not any(info.has_section(k) for k in ['LinkKey','LongTermKey','PeripheralLongTermKey','SlaveLongTermKey']):continue
                name=info.get('General','Name',fallback='')
                modalias=info.get('DeviceID','Vendor',fallback='')
                try:vendor=int(modalias,0)
                except ValueError:vendor=0
                try:product=int(info.get('DeviceID','Product',fallback='0'),0)
                except ValueError:product=0
                try:device_class=int(info.get('General','Class',fallback='0'),0)
                except ValueError:device_class=0
                lower=name.lower()
                family='switch' if ('pro controller' in lower or vendor==0x057e and product==0x2009) else \
                       'ps5' if ('dualsense' in lower or vendor==0x054c and product in (0x0ce6,0x0df2)) else \
                       'xbox' if ('xbox' in lower or vendor==0x045e and device_class&0x1f3c==0x0508) else None
                if not family:continue
                peer={'address':folder.name.upper(),'name':name[:39] or family,'family':family}
                key=info.get('IdentityResolvingKey','Key',fallback='')
                if re.fullmatch('[0-9A-Fa-f]{32}',key):peer['irk']=key
                controllers.append(peer)
            adapters.append({'host_mac':adapter.name.upper(),'controllers':controllers[:6]})
        if preferred:
            return next((a for a in adapters if a['host_mac']==preferred),{'host_mac':preferred,'controllers':[]})
        if len(adapters)>1:return {'error':'multiple_bluetooth_adapters_select_explicitly'}
        return adapters[0] if adapters else {'error':'no_bluetooth_adapter'}


class FinalShutdownPacket:
    """Prepare an authenticated frame while networking is up; send only at final shutdown."""
    def __init__(self, token, destination=Path('/run/bc250-power-manager/final-packet')):
        self.token, self.destination = token, destination
        self.last = None
        self.last_usb = None
        self.lock = threading.Lock()

    def prepare_usb(self,device,nonce):
        if len(nonce)!=16 or not any(nonce):return
        target=Path(device).resolve()
        if not re.fullmatch(r'/dev/ttyUSB[0-9]+',str(target)):return
        boot=Path('/proc/sys/kernel/random/boot_id').read_text().strip()
        identity=(str(target),nonce,boot)
        with self.lock:
            if self.last_usb==identity:return
            message=f'bc250-usb-poweroff-v1|{boot}|{nonce.hex()}'.encode()
            signature=hmac.new(self.token.encode(),message,hashlib.sha256).digest()
            payload=b'BC250F2\0'+uuid.UUID(boot).bytes+nonce+signature
            packet=struct.pack('<BHB',1,0xfcf3,len(payload))+payload
            data=b'BC250U1\n'+str(target).encode().ljust(32,b'\0')+boot.encode()+struct.pack('!H',len(packet))+packet
            destination=self.destination.with_name('final-usb-packet')
            destination.parent.mkdir(parents=True,exist_ok=True,mode=0o700)
            temporary=destination.with_suffix('.tmp');temporary.write_bytes(data);temporary.chmod(0o600);temporary.replace(destination)
            self.last_usb=identity

    @staticmethod
    def frame(source_mac, target_mac, source_ip, target_ip, payload):
        ethernet = bytes.fromhex(target_mac.replace(':','')+source_mac.replace(':',''))+b'\x08\x00'
        udp = struct.pack('!HHHH',9093,9092,8+len(payload),0)+payload
        header = struct.pack('!BBHHHBBH4s4s',0x45,0,20+len(udp),0,0,64,17,0,
                            socket.inet_aton(source_ip),socket.inet_aton(target_ip))
        words=struct.unpack('!10H',header)
        total=sum(words)
        while total>>16:total=(total&65535)+(total>>16)
        checksum=(~total)&65535
        header=header[:10]+struct.pack('!H',checksum)+header[12:]
        return ethernet+header+udp

    def prepare(self, address, nonce, mac):
        if not re.fullmatch('[0-9a-f]{32}',nonce or '') or not re.fullmatch('(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}',mac or ''):
            return
        if not ipaddress.ip_address(address).is_private:
            return
        identity=(address,nonce,mac)
        with self.lock:
            if self.last==identity:return
            route=json.loads(subprocess.run(['/usr/bin/ip','-j','route','get',address],
                             capture_output=True,text=True,check=True,timeout=2).stdout)[0]
            interface=route['dev']
            if not re.fullmatch('[A-Za-z0-9_.-]{1,15}',interface):return
            source_mac=Path('/sys/class/net',interface,'address').read_text().strip()
            source_ip=route.get('prefsrc') or route['src']
            boot=Path('/proc/sys/kernel/random/boot_id').read_text().strip()
            message=f'bc250-poweroff-v1|{boot}|{nonce}'
            payload=json.dumps({'type':'bc250-poweroff-v1','boot_id':boot,'nonce':nonce,
                       'signature':hmac.new(self.token.encode(),message.encode(),hashlib.sha256).hexdigest()},
                       separators=(',',':')).encode()
            frame=self.frame(source_mac,mac,source_ip,address,payload)
            data=b'BC250F1\n'+interface.encode().ljust(16,b'\0')+boot.encode()+struct.pack('!H',len(frame))+frame
            self.destination.parent.mkdir(mode=0o700,parents=True,exist_ok=True)
            temporary=self.destination.with_suffix('.tmp')
            temporary.write_bytes(data)
            temporary.chmod(0o600)
            temporary.replace(self.destination)
            self.last=identity


def read_number(path, divisor=1):
    try:
        return round(float(path.read_text().strip()) / divisor, 1)
    except (OSError, ValueError):
        return None


class Metrics:
    """Read kernel counters only. Missing sensors are null, never zero."""
    def __init__(self, proc=Path('/proc'), sys=Path('/sys')):
        self.proc, self.sys = proc, sys
        self.lock = threading.Lock()
        self.previous = self.cpu_ticks()

    def cpu_ticks(self):
        try:
            values = [int(x) for x in (self.proc/'stat').read_text().splitlines()[0].split()[1:9]]
            return sum(values), values[3] + values[4]
        except (OSError, ValueError, IndexError):
            return None

    def collect(self):
        with self.lock:
            now = self.cpu_ticks()
            cpu = None
            if now and self.previous:
                total = now[0] - self.previous[0]
                idle = now[1] - self.previous[1]
                if total > 0 and 0 <= idle <= total:
                    cpu = round(100 * (total-idle) / total, 1)
            self.previous = now
            memory = {}
            try:
                memory = {k: int(v.split()[0])*1024 for k,v in
                          (line.split(':',1) for line in (self.proc/'meminfo').read_text().splitlines())}
            except (OSError, ValueError, IndexError):
                pass
            total = memory.get('MemTotal')
            available = memory.get('MemAvailable')
            used = max(0, total-available) if total is not None and available is not None else None
            result = {'cpu_percent':cpu, 'cpu_temp_c':None, 'gpu_percent':None,
                      'gpu_temp_c':None, 'ram_used_bytes':used, 'ram_total_bytes':total,
                      'vram_used_bytes':None, 'vram_total_bytes':None}
            # CPU package/Tctl; do not label a GPU or NVMe sensor as CPU.
            for hw in sorted((self.sys/'class/hwmon').glob('hwmon*')):
                try:
                    if hw.joinpath('name').read_text().strip() not in ('k10temp','zenpower','coretemp'):
                        continue
                except OSError:
                    continue
                candidates = list(hw.glob('temp*_input'))
                candidates.sort(key=lambda p: p.name != 'temp1_input')
                for sensor in candidates:
                    value = read_number(sensor, 1000)
                    if value is not None:
                        result['cpu_temp_c'] = value
                        break
                if result['cpu_temp_c'] is not None:
                    break
            for card in sorted((self.sys/'class/drm').glob('card[0-9]*')):
                if not card.name[4:].isdigit():
                    continue
                device = card/'device'
                try:
                    if device.joinpath('vendor').read_text().strip() != '0x1002':
                        continue
                except OSError:
                    continue
                result['gpu_percent'] = read_number(device/'gpu_busy_percent')
                for key,filename in [('vram_used_bytes','mem_info_vram_used'),('vram_total_bytes','mem_info_vram_total')]:
                    value = read_number(device/filename)
                    result[key] = int(value) if value is not None else None
                for hw in sorted((device/'hwmon').glob('hwmon*')):
                    value = read_number(hw/'temp1_input',1000)
                    if value is not None:
                        result['gpu_temp_c'] = value
                        break
                break
            return result

class Controller:
    def __init__(self, token, runner=None, metrics=None):
        self.token = token
        self.runner = runner or subprocess.run
        self.lock = threading.Lock()
        self.pending = None
        self.metrics = metrics or Metrics()
        self.final_packet = FinalShutdownPacket(token)
        self.gamepads = GamepadRegistry()

    def state(self):
        return {'device':'bc250', 'state':self.pending or 'running',
                'uptime_seconds':int(float(Path('/proc/uptime').read_text().split()[0])),
                'boot_id':Path('/proc/sys/kernel/random/boot_id').read_text().strip()}

    def request(self, action):
        if action not in ('shutdown','reboot'):
            return 400, {'error':'invalid_action'}
        with self.lock:
            if self.pending:
                return 409, {'error':'action_already_pending'}
            self.pending = action + '_requested'
        def execute():
            time.sleep(2)
            try:
                result = self.runner(['/usr/bin/systemctl', '--no-block',
                                     'poweroff' if action=='shutdown' else 'reboot'],
                                     capture_output=True, timeout=10)
                if result.returncode:
                    self.pending = None
            except (OSError, subprocess.SubprocessError):
                self.pending = None
        threading.Thread(target=execute, daemon=True).start()
        return 202, {'device':'bc250', 'state':self.pending}

def make_handler(controller):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass  # Never log Authorization headers or tokens.

        def reply(self, status, body):
            data = json.dumps(body, separators=(',',':')).encode()
            self.send_response(status)
            self.send_header('Content-Type','application/json')
            self.send_header('Content-Length',str(len(data)))
            self.send_header('Cache-Control','no-store')
            self.end_headers()
            self.wfile.write(data)

        def authorized(self):
            value = self.headers.get('Authorization','')
            try:
                return hmac.compare_digest(value, 'Bearer '+controller.token)
            except TypeError:
                return False

        def do_GET(self):
            if not self.authorized():
                return self.reply(401, {'error':'unauthorized'})
            if self.path not in ('/v1/status','/v1/metrics','/v1/gamepads'):
                return self.reply(404, {'error':'not_found'})
            if self.path=='/v1/gamepads':
                try:return self.reply(200,controller.gamepads.read())
                except OSError:return self.reply(503,{'error':'gamepad_registry_unavailable'})
            try:
                controller.final_packet.prepare(self.client_address[0],
                    self.headers.get('X-BC250-Panel-Nonce'),self.headers.get('X-BC250-Panel-MAC'))
            except (OSError, ValueError, KeyError, subprocess.SubprocessError):
                pass  # Monitoring remains available if shutdown notification cannot be prepared.
            data = controller.state()
            if self.path == '/v1/metrics':
                data['metrics'] = controller.metrics.collect()
            self.reply(200, data)

        def do_POST(self):
            if not self.authorized():
                return self.reply(401, {'error':'unauthorized'})
            if self.path not in ('/v1/shutdown','/v1/reboot'):
                return self.reply(404, {'error':'not_found'})
            if self.headers.get_content_type() != 'application/json':
                return self.reply(415, {'error':'json_required'})
            try:
                size = int(self.headers.get('Content-Length','0'))
                if not 0 < size <= 128:
                    raise ValueError()
                self.connection.settimeout(3)
                body = json.loads(self.rfile.read(size))
                if not isinstance(body,dict) or body.get('confirm') is not True:
                    return self.reply(400, {'error':'confirmation_required'})
            except (ValueError, OSError):
                return self.reply(400, {'error':'invalid_request'})
            status, data = controller.request(self.path.rsplit('/',1)[-1])
            self.reply(status, data)
    return Handler

if __name__=='__main__':
    p=argparse.ArgumentParser()
    p.add_argument('--token-file', default='/etc/bc250-power-manager/token')
    p.add_argument('--bind', default='0.0.0.0')
    p.add_argument('--port',type=int,default=9091)
    args=p.parse_args()
    token=Path(args.token_file).read_text().strip()
    if len(token)!=64 or any(c not in '0123456789abcdef' for c in token):
        raise SystemExit('Invalid token file')
    controller=Controller(token)
    if UsbMonitorPublisher is not None:UsbMonitorPublisher(controller,agent_port=args.port).start()
    server=ThreadingHTTPServer((args.bind,args.port),make_handler(controller))
    server.daemon_threads=True
    server.serve_forever()
