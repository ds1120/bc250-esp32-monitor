#!/usr/bin/env python3
"""First provisioning only; credentials stay in a private local file and ESP NVS."""
import argparse,getpass,ipaddress,json,os,secrets,time
from pathlib import Path

def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--port');p.add_argument('--settings',type=Path,default=Path.home()/'.config/bc250-monitor/setup.json')
 p.add_argument('--ssid');p.add_argument('--host',help='Wi-Fi companion IP; set this or configure network.agent_host on SD')
 p.add_argument('--power-isolated',action='store_true',help='Confirm PC/relay power is isolated for first provisioning')
 a=p.parse_args()
 if not a.power_isolated:p.error('First provisioning requires --power-isolated; never reconfigure a running relay controller')
 import serial
 from host.environment import discover_usb_device
 settings=json.loads(a.settings.read_text()) if a.settings.exists() else {}
 if a.ssid is not None:settings['ssid']=a.ssid
 if a.host is not None:settings['host']=a.host
 if not settings.get('ssid'):settings['ssid']=input('Wi-Fi SSID: ').strip()
 if 'password' not in settings:settings['password']=getpass.getpass('Wi-Fi password (empty for open network): ')
 if not settings.get('pin'):
  first=getpass.getpass('Administrator PIN (6–16 characters): ')
  if first!=getpass.getpass('Repeat PIN: '):p.error('PINs did not match')
  settings['pin']=first
 if not settings.get('token'):settings['token']=secrets.token_hex(32)
 settings.setdefault('host','')
 if settings['host']:ipaddress.IPv4Address(settings['host'])
 if not 1<=len(settings['ssid'].encode())<=32 or len(settings['password'].encode())>63 or not 6<=len(settings['pin'])<=16:p.error('Invalid Wi-Fi/PIN lengths')
 if len(settings['token'])!=64 or any(c not in '0123456789abcdef' for c in settings['token']):p.error('Token must be 64 lowercase hex characters')
 port=a.port or discover_usb_device()
 ser=serial.Serial();ser.port=port;ser.baudrate=115200;ser.timeout=.2;ser.write_timeout=3;ser.dtr=False;ser.rts=False
 with ser:
  ser.write((json.dumps({'command':'provision',**{k:settings[k] for k in ['ssid','password','token','host','pin']}})+'\n').encode())
  deadline=time.monotonic()+10;ok=False
  while time.monotonic()<deadline:
   try:
    reply=json.loads(ser.readline())
    if reply.get('event')=='provisioned':ok=reply.get('success') is True;break
   except (ValueError,UnicodeError):continue
  if not ok:raise SystemExit('Provisioning was not acknowledged; no local settings were replaced. Check isolated power and first-boot serial mode.')
 a.settings.parent.mkdir(parents=True,exist_ok=True,mode=0o700)
 tmp=a.settings.with_suffix('.tmp');fd=os.open(tmp,os.O_WRONLY|os.O_CREAT|os.O_TRUNC,0o600)
 with os.fdopen(fd,'w') as f:json.dump(settings,f,indent=2)
 tmp.chmod(0o600);tmp.replace(a.settings)
 print('Provisioned. Private settings saved to '+str(a.settings)+'; do not publish this file.')
if __name__=='__main__':main()
