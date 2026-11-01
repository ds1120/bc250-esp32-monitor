#!/usr/bin/env python3
"""Install companion services locally from the private provisioning file."""
import argparse,json,os,shutil,subprocess,sys,tempfile
from pathlib import Path

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--settings',type=Path,required=True);p.add_argument('--lan-cidr');a=p.parse_args()
 if os.geteuid()!=0:p.error('Run with sudo; root is needed only for installation')
 settings=json.loads(a.settings.read_text());token=settings['token']
 if len(token)!=64 or any(c not in '0123456789abcdef' for c in token):p.error('Invalid token')
 root=Path(__file__).resolve().parent
 with tempfile.TemporaryDirectory(prefix='bc250-install-',dir='/run') as d:
  stage=Path(d)
  for name in ['agent.py','usb_monitor.py','final_shutdown.c','install_remote.py','environment.py']:
   shutil.copy2(root/name,stage/name)
  shutil.copy2(root/'bc250-power-manager.service',stage/'service')
  (stage/'token').write_text(token+'\n');(stage/'token').chmod(0o600)
  (stage/'installation.json').write_text(json.dumps({'lan_cidr':a.lan_cidr}))
  subprocess.run([sys.executable,str(stage/'install_remote.py')],check=True)
 print('Wi-Fi companion installed. Configure the panel agent_host with this PC address; USB transport is not installed.')
if __name__=='__main__':main()
