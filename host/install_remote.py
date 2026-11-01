"""Root-side companion installation, invoked from a private temporary folder."""
import datetime
import json
import ipaddress
from environment import discover_lan_network
import os
from pathlib import Path
import shutil
import subprocess

stage=Path(__file__).resolve().parent
assert os.geteuid()==0
if not shutil.which('systemctl'):raise RuntimeError('systemd is required')
options=json.loads((stage/'installation.json').read_text()) if (stage/'installation.json').exists() else {}
lan=options.get('lan_cidr') or discover_lan_network()
network=ipaddress.ip_network(lan,strict=False)
if network.version!=4 or not network.is_private or network.is_loopback:raise ValueError('A private LAN IPv4 network is required')
lan=str(network)
token=(stage/'token').read_text().strip()
assert len(token)==64 and all(c in '0123456789abcdef' for c in token)
compiler=shutil.which('cc') or shutil.which('gcc')
if not compiler:raise RuntimeError('C compiler is required for the final shutdown hook')
compile_args=[compiler,'-O2','-Wall','-Wextra','-o',str(stage/'shutdown-hook'),str(stage/'final_shutdown.c')]
compiled=subprocess.run(compile_args[:1]+['-static']+compile_args[1:],capture_output=True,text=True)
if compiled.returncode:
    compiled=subprocess.run(compile_args,capture_output=True,text=True,check=True)
backup=Path('/var/lib/bc250-power-manager/backups')/datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f')
backup.mkdir(parents=True,mode=0o700)
for source,dest,mode in [(stage/'agent.py',Path('/var/lib/bc250-power-manager/agent.py'),0o755),
                         (stage/'usb_monitor.py',Path('/var/lib/bc250-power-manager/usb_monitor.py'),0o755),
                         (stage/'service',Path('/etc/systemd/system/bc250-power-manager.service'),0o644),
                         (stage/'token',Path('/etc/bc250-power-manager/token'),0o600),
                         (stage/'shutdown-hook',Path('/usr/lib/systemd/system-shutdown/bc250-panel'),0o755)]:
    dest.parent.mkdir(parents=True,exist_ok=True)
    if dest.exists():
        shutil.copy2(dest,backup/dest.name)
    shutil.copyfile(source,dest)
    dest.chmod(mode)
    os.chown(dest,0,0)
env=dict(os.environ,LC_ALL='C')
def run(args):return subprocess.run(args,check=True,capture_output=True,text=True,env=env)
if shutil.which('ufw') and 'Status: active' in run(['ufw','status']).stdout:
    run(['ufw','allow','from',lan,'to','any','port','9091','proto','tcp'])
if shutil.which('firewall-cmd') and subprocess.run(['systemctl','is-active','--quiet','firewalld']).returncode==0:
    rule=f'rule family="ipv4" source address="{lan}" port port="9091" protocol="tcp" accept'
    run(['firewall-cmd','--permanent','--add-rich-rule='+rule])
    run(['firewall-cmd','--add-rich-rule='+rule])
run(['systemctl','daemon-reload'])
run(['systemctl','enable','bc250-power-manager.service'])
run(['systemctl','restart','bc250-power-manager.service'])
assert run(['systemctl','is-active','bc250-power-manager.service']).stdout.strip()=='active'
print('Companion service active. Backup: '+str(backup))
