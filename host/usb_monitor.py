"""Send 5-second monitoring samples inside a private, framed HCI vendor command.

The USB UART remains a normal BlueZ H4 transport. Never opens/resets the serial
port itself, and never sends vendor commands to an unrelated Bluetooth dongle.
"""
import json
import math
from pathlib import Path
import socket
import select
import struct
import threading
import time
import uuid
import subprocess
import ipaddress
import re
import shlex

MONITOR_ENABLE_FILE=Path('/run/bc250-usb-features/monitor.enabled')
OPCODE = 0xFCF0
FORMAT = '<8sI16sI8fH'
KEYS = ('cpu_percent','cpu_temp_c','gpu_percent','gpu_temp_c',
        'ram_used_bytes','ram_total_bytes','vram_used_bytes','vram_total_bytes')


def encode_sample(state, metrics, sequence):
    values, flags = [], 256 if state.get('state') == 'running' else 0
    for i, key in enumerate(KEYS):
        value = metrics.get(key)
        valid = isinstance(value, (int,float)) and not isinstance(value,bool) and math.isfinite(value)
        value = float(value) / (1073741824 if i >= 4 else 1) if valid else 0.0
        if valid:
            minimum = -100 if i in (1,3) else 0
            maximum = 100 if i in (0,2) else 250 if i in (1,3) else 1048576
            valid = minimum <= value <= maximum
        if valid:flags |= 1 << i
        values.append(value if valid else 0.0)
    payload = struct.pack(FORMAT,b'BC250M1\0',sequence & 0xffffffff,
                          uuid.UUID(state['boot_id']).bytes,
                          int(state['uptime_seconds']) & 0xffffffff,*values,flags)
    return struct.pack('<BHB',1,OPCODE,len(payload)) + payload


def find_controller(device, sys_root=Path('/sys/class/bluetooth')):
    tty = Path(device).resolve().name
    if not tty.startswith('ttyUSB') or not Path(device).exists():return None
    for hci in sorted(sys_root.glob('hci*')):
        if not hci.name[3:].isdigit():continue
        # HCI UART's sysfs parent is its exact configured USB serial device.
        if tty in hci.resolve().parts:return int(hci.name[3:])
    return None


INFO_OPCODE=0xFCF1
INFO_FORMAT='<8s4sH6s64s'

def encode_host_info(info):
    address=ipaddress.IPv4Address(info['agent_host'])
    if address.is_unspecified or address.is_loopback or address.is_multicast:raise ValueError('Invalid agent address')
    port=int(info['agent_port'])
    if not 1<=port<=65535:raise ValueError('Invalid port')
    mac=bytes.fromhex(info['bluetooth_address'].replace(':',''))
    if len(mac)!=6 or mac[0]&1 or not any(mac):raise ValueError('Invalid adapter address')
    hostname=info['pc_name']
    if not re.fullmatch('[A-Za-z0-9_.-]{1,63}',hostname):raise ValueError('Invalid hostname')
    body=struct.pack(INFO_FORMAT,b'BC250I1\0',address.packed,port,mac,hostname.encode())
    return struct.pack('<BHB',1,INFO_OPCODE,len(body))+body

def adapter_address(index):
    output=subprocess.run(['/usr/bin/busctl','--system','get-property','org.bluez',f'/org/bluez/hci{index}','org.bluez.Adapter1','Address'],check=True,capture_output=True,text=True,timeout=3).stdout
    result=shlex.split(output)
    if len(result)!=2 or result[0]!='s':raise ValueError('Adapter address unavailable')
    return result[1].upper()

def read_host_info(index,agent_port):
    """Read local routing and the exact USB-backed adapter; never scan the LAN."""
    def run(args):return subprocess.run(args,check=True,capture_output=True,text=True,timeout=3).stdout
    routes=json.loads(run(['/usr/bin/ip','-j','-4','route','show','default']))
    candidates=[r for r in routes if r.get('dev') and r.get('type','unicast')=='unicast']
    if not candidates:return None
    metric=min(r.get('metric',0) for r in candidates)
    interfaces={r['dev'] for r in candidates if r.get('metric',0)==metric}
    if len(interfaces)!=1:return None
    interface=next(iter(interfaces))
    addresses=json.loads(run(['/usr/bin/ip','-j','-4','addr','show','dev',interface]))
    ips={a['local'] for item in addresses for a in item.get('addr_info',[]) if a.get('scope')=='global' and not a.get('secondary')}
    if len(ips)!=1:return None
    return {'agent_host':next(iter(ips)),'agent_port':agent_port,'bluetooth_address':adapter_address(index),'pc_name':socket.gethostname()}


class UsbMonitorPublisher:
    def __init__(self, controller, config=Path('/etc/bc250-esp-bluetooth.json'), agent_port=9091):
        self.controller,self.config = controller,config
        self.sequence = 0
        self.sock = None
        self.index = None
        self.interval = 5.0
        self.agent_port=agent_port
        self.info_at=0

    def tick(self):
        if not MONITOR_ENABLE_FILE.exists():
            if self.sock:self.sock.close();self.sock=None
            return
        try:
            device = json.loads(self.config.read_text())['device']
            index = find_controller(device)
            if index is None:
                self.close();return
            if self.sock is None or index != self.index:
                self.close()
                self.sock = socket.socket(socket.AF_BLUETOOTH,socket.SOCK_RAW,socket.BTPROTO_HCI)
                self.sock.setblocking(False)
                self.sock.setsockopt(0,2,struct.pack("<IIIH2x",1<<4,1<<14,0,OPCODE))
                self.sock.bind((index,0))  # HCI_CHANNEL_RAW
                self.index = index
                self.info_at=0
            self.sequence += 1
            data = encode_sample(self.controller.state(),self.controller.metrics.collect(),self.sequence)
            if self.sock.send(data) != len(data):self.close();return
            if select.select([self.sock],[],[],0.25)[0]:
                reply=self.sock.recv(260)
                if len(reply)>=9 and reply[:2]==b'\x04\x0e' and reply[4:7]==b'\xf0\xfc\0':
                    interval=int.from_bytes(reply[7:9],'little')
                    if 1000<=interval<=5000:self.interval=interval/1000
            if time.monotonic()-self.info_at>=30:
                self.info_at=time.monotonic()
                try:
                    info=read_host_info(index,self.agent_port)
                    if info:self.sock.send(encode_host_info(info))
                except (OSError,ValueError,KeyError,TypeError,subprocess.SubprocessError):pass
        except (OSError,ValueError,KeyError,TypeError,AttributeError):
            self.close()  # Existing Wi-Fi monitoring remains the fallback.

    def close(self):
        if self.sock is not None:self.sock.close()
        self.sock,self.index = None,None

    def run(self):
        while True:
            started=time.monotonic()
            self.tick()
            time.sleep(max(0.1,self.interval-(time.monotonic()-started)))

    def start(self):
        threading.Thread(target=self.run,name='usb-monitor',daemon=True).start()
