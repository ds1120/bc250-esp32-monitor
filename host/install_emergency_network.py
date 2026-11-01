#!/usr/bin/python3
"""Install optional, offline-ready emergency internet without changing LAN routes."""
import argparse,datetime,hashlib,io,json,os,shutil,subprocess,urllib.request,zipfile
from pathlib import Path
VERSION='v2.7.0'
URL='https://github.com/xjasonlyu/tun2socks/releases/download/v2.7.0/tun2socks-linux-amd64.zip'
SHA256='a612baa287a3b6de6221f74fd02b442a50888508227ecf51e1288a5ccbb77381'

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--activate-host',action='store_true',help='Restart Bluetooth attachment to load the authenticated UI toggle');p.add_argument('--archive',type=Path,help='Previously downloaded official, hash-verified archive');a=p.parse_args()
 if os.geteuid()!=0:p.error('Run installer with sudo or pkexec')
 if os.uname().machine not in ('x86_64','amd64'):p.error('This installer bundles the upstream Linux amd64 runtime; other architectures need their matching official runtime')
 for program in ['nmcli','ip','systemctl','busctl']:
  if not shutil.which(program):p.error(program+' is required')
 if not Path('/etc/bc250-esp-bluetooth.json').exists():p.error('Install the ESP32 Bluetooth transport first')
 data=a.archive.read_bytes() if a.archive else urllib.request.urlopen(URL,timeout=60).read()
 if hashlib.sha256(data).hexdigest()!=SHA256:raise SystemExit('Upstream runtime checksum mismatch; no files installed')
 binary=zipfile.ZipFile(io.BytesIO(data)).read('tun2socks-linux-amd64')
 source=Path(__file__).resolve().parent;backup=Path('/var/lib/bc250-esp-network')/datetime.datetime.now().strftime('%Y%m%d-%H%M%S');backup.mkdir(parents=True,mode=0o700)
 compiler=shutil.which('cc') or shutil.which('gcc')
 if not compiler:raise SystemExit('C compiler is required for the final shutdown hook')
 compiled=backup/'new-shutdown-hook'
 args=[compiler,'-O2','-Wall','-Wextra','-o',str(compiled),str(source/'final_shutdown.c')]
 result=subprocess.run(args[:1]+['-static']+args[1:],capture_output=True,text=True)
 if result.returncode:subprocess.run(args,check=True)
 files=[(source/'bc250-esp-bluetooth.service',Path('/etc/systemd/system/bc250-esp-bluetooth.service'),0o644),(source/'esp_network.py',Path('/usr/local/lib/bc250/esp_network.py'),0o755),(source/'net_protocol.py',Path('/usr/local/lib/bc250/net_protocol.py'),0o644),(source/'usb_monitor.py',Path('/usr/local/lib/bc250/usb_monitor.py'),0o644),(source/'esp_bluetooth.py',Path('/usr/local/lib/bc250/esp_bluetooth.py'),0o755),(source/'bc250-esp-network.service',Path('/etc/systemd/system/bc250-esp-network.service'),0o644)]
 for src,dest,mode in files:
  dest.parent.mkdir(parents=True,exist_ok=True)
  if dest.exists():shutil.copy2(dest,backup/dest.name)
  shutil.copyfile(src,dest);dest.chmod(mode)
 target=Path('/usr/local/lib/bc250/tun2socks')
 if target.exists():shutil.copy2(target,backup/'tun2socks')
 target.write_bytes(binary);target.chmod(0o755)
 license_file=Path('/usr/local/share/licenses/bc250-esp-network/tun2socks-LICENSE');license_file.parent.mkdir(parents=True,exist_ok=True)
 license_file.write_bytes((source.parent/'licenses/tun2socks.LICENSE').read_bytes());license_file.chmod(0o644)
 # Upgrade the monitoring publisher and final-shutdown hook as one compatible set.
 for name in ['agent.py','usb_monitor.py']:
  dest=Path('/var/lib/bc250-power-manager')/name
  if dest.exists():shutil.copy2(dest,backup/name)
  shutil.copy2(source/name,dest);dest.chmod(0o755)
 hook=Path('/usr/lib/systemd/system-shutdown/bc250-panel')
 if hook.exists():shutil.copy2(hook,backup/'previous-shutdown-hook')
 shutil.copy2(compiled,hook);hook.chmod(0o755)
 config=Path('/etc/bc250-esp-network.json')
 if not config.exists():
  config.write_text(json.dumps({'interface':'espnet0','proxy_port':10808,'tun2socks':str(target),'bluetooth_config':'/etc/bc250-esp-bluetooth.json'},indent=2)+'\n');config.chmod(0o600)
 cli=Path('/usr/local/bin/bc250-esp-internet')
 cli.write_text('''#!/usr/bin/python3
import argparse,json,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('action',choices=['on','off','status']);a=p.parse_args()
if a.action=='status':
 f=Path('/run/bc250-esp-network/status.json');print(f.read_text() if f.exists() else '{"active":false}')
else:subprocess.run(['/usr/bin/pkexec','/usr/bin/systemctl','start' if a.action=='on' else 'stop','bc250-esp-network.service'],check=True)
''');cli.chmod(0o755)
 desktop=Path('/usr/share/applications/bc250-esp-internet.desktop');desktop.write_text('''[Desktop Entry]
Type=Application
Name=ESP32 Emergency Internet
Name[ko]=ESP32 비상 인터넷
Comment=Enable the offline-ready ESP32 network connection while keeping Bluetooth
Exec=/usr/local/bin/bc250-esp-internet on
Icon=network-wireless
Terminal=false
Categories=Network;
Actions=Stop;
[Desktop Action Stop]
Name=Turn off emergency internet
Name[ko]=비상 인터넷 끄기
Exec=/usr/local/bin/bc250-esp-internet off
''');desktop.chmod(0o644)
 subprocess.run(['systemctl','daemon-reload'],check=True)
 if a.activate_host:
  subprocess.run(['systemctl','restart','bc250-power-manager.service'],check=True)
  subprocess.run(['systemctl','restart','bc250-esp-bluetooth.service'],check=True)
 print(json.dumps({'installed':True,'emergency_network_started':False,'routes_changed':False,'runtime_version':VERSION,'runtime_archive_sha256':SHA256,'backup':str(backup)}))
if __name__=='__main__':main()
