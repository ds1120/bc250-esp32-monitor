"""Install on an explicitly selected systemd Linux host; no power requests."""
import argparse
import getpass
import ipaddress
import json
from pathlib import Path
import shlex
import uuid
import urllib.request

import paramiko

BASE = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', required=True, help='Target LAN IPv4 address')
    parser.add_argument('--user', default=getpass.getuser())
    parser.add_argument('--lan-cidr',help='Firewall source network; auto-detect only when unambiguous')
    parser.add_argument('--settings', type=Path, required=True)
    parser.add_argument('--key', type=Path)
    parser.add_argument('--password', action='store_true', help='Prompt securely for SSH/sudo password')
    args = parser.parse_args()
    if not ipaddress.ip_address(args.host).is_private:
        parser.error('A private LAN address is required')
    token = json.loads(args.settings.read_text(encoding='utf-8'))['token']
    if len(token) != 64 or any(c not in '0123456789abcdef' for c in token):
        parser.error('Invalid companion token')
    password = getpass.getpass('SSH/sudo password: ') if args.password else None
    client = paramiko.SSHClient()
    client.load_system_host_keys()
    client.connect(args.host, username=args.user, password=password,
                   key_filename=str(args.key) if args.key else None,
                   look_for_keys=not args.password, allow_agent=not args.password,
                   timeout=8, banner_timeout=8, auth_timeout=8)
    stage = '/tmp/bc250-monitor-' + uuid.uuid4().hex

    def execute(command, stdin=None):
        inp, out, err = client.exec_command(command, timeout=45)
        if stdin:
            inp.write(stdin)
            inp.flush()
        inp.channel.shutdown_write()
        output, error = out.read().decode(), err.read().decode()
        if out.channel.recv_exit_status():
            raise RuntimeError(error or output)
        return output

    try:
        identity = execute('cat /etc/os-release; hostname')
        if execute('command -v systemctl').strip()=='':
            raise RuntimeError('The selected host must use systemd')
        with client.open_sftp() as sftp:
            sftp.mkdir(stage, 0o700)
            for local, name in [(BASE/'host/agent.py','agent.py'),
                                (BASE/'host/bc250-power-manager.service','service'),
                                (BASE/'host/final_shutdown.c','final_shutdown.c'),
                                (BASE/'host/install_remote.py','install_remote.py'),
                                (BASE/'host/environment.py','environment.py'),
                                (BASE/'host/usb_monitor.py','usb_monitor.py')]:
                sftp.put(str(local),stage+'/'+name)
                sftp.chmod(stage+'/'+name,0o600)
            with sftp.open(stage+'/token','w') as file:
                file.write(token+'\n')
            sftp.chmod(stage+'/token',0o600)
            with sftp.open(stage+'/installation.json','w') as file:file.write(json.dumps({'lan_cidr':args.lan_cidr}))
            sftp.chmod(stage+'/installation.json',0o600)
        command = 'sudo '+('-S -p "" ' if password else '-n ')+'python3 '+shlex.quote(stage+'/install_remote.py')
        print(execute(command, password+'\n' if password else None).strip())
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        for endpoint in ['status','metrics']:
            request = urllib.request.Request(f'http://{args.host}:9091/v1/{endpoint}',
                      headers={'Authorization':'Bearer '+token})
            with opener.open(request,timeout=5) as response:
                data = json.load(response)
            assert data['device']=='bc250' and data['state']=='running'
            if endpoint=='metrics':
                print(json.dumps({'verified':True,'metrics':data['metrics']},ensure_ascii=False))
        print('Installed. Authenticated LAN status and metrics verified; no power command sent.')
    finally:
        try:
            with client.open_sftp() as sftp:
                for name in ['agent.py','service','install_remote.py','token','final_shutdown.c','shutdown-hook','environment.py','usb_monitor.py','installation.json']:
                    try: sftp.remove(stage+'/'+name)
                    except OSError: pass
                try: sftp.rmdir(stage)
                except OSError: pass
        finally:
            client.close()


if __name__=='__main__':
    main()
