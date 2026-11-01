"""Initial ESP32 app installation on an explicitly selected, power-isolated board."""
import argparse,hashlib,struct,subprocess,sys
from pathlib import Path

def validate_backup(data,image_size):
 if len(data)!=4194304:raise ValueError('A complete 4 MiB backup from this board is required')
 slots={}
 for offset in range(0x8000,0x9000,32):
  magic,kind,subtype,start,size,label,flags=struct.unpack('<HBBII16sI',data[offset:offset+32])
  if magic!=0xaa50:break
  if kind==0:slots[label.rstrip(b'\0').decode()]=(start,size)
 if slots.get('app0')!=(0x10000,0x140000) or slots.get('app1')!=(0x150000,0x140000):raise ValueError('Partition layout differs from this project; use a board-specific initial flash procedure')
 if image_size>0x140000:raise ValueError('Firmware does not fit an OTA slot')

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--port',required=True);p.add_argument('--factory-backup',type=Path,required=True);p.add_argument('--firmware',type=Path,required=True);p.add_argument('--expected-mac');p.add_argument('--power-isolated',action='store_true');a=p.parse_args()
 if not a.power_isolated:p.error('Shut down/isolate relay and ATX power before first flashing or reset')
 image=a.firmware.read_bytes()
 if not image or image[0]!=0xe9:p.error('Invalid ESP32 image')
 validate_backup(a.factory_backup.read_bytes(),len(image))
 import esptool
 device=esptool.detect_chip(a.port,115200)
 try:
  if device.CHIP_NAME!='ESP32':raise SystemExit('This build targets classic ESP32 only')
  mac=':'.join(f'{b:02X}' for b in device.read_mac())
  if a.expected_mac and mac!=a.expected_mac.upper():raise SystemExit('Selected board MAC did not match --expected-mac')
 finally:device._port.close()
 for command,after,args in [('write-flash','no-reset',['0x10000',str(a.firmware)]),('verify-flash','no-reset',['0x10000',str(a.firmware)]),('erase-region','hard-reset',['0xe000','0x2000'])]:
  subprocess.run([sys.executable,'-m','esptool','--chip','esp32','--port',a.port,'--baud','460800','--after',after,command]+args,check=True)
 print('Application verified; app0 selected. NVS and partition table preserved. SHA256='+hashlib.sha256(image).hexdigest())
if __name__=='__main__':main()
