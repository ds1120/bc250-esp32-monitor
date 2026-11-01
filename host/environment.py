"""Non-invasive discovery for first installation. Never opens a serial port."""
from pathlib import Path
import ipaddress,json,subprocess

def discover_usb_device(base=Path('/dev/serial')):
    candidates={}
    for folder in ('by-path','by-id'):
        for path in sorted((base/folder).glob('*')):
            target=path.resolve()
            if not target.name.startswith('ttyUSB'):continue
            result=subprocess.run(['udevadm','info','--query=property','--name',str(target)],capture_output=True,text=True,check=True,timeout=3)
            props=dict(line.split('=',1) for line in result.stdout.splitlines() if '=' in line)
            if props.get('ID_VENDOR_ID') not in ('1a86','10c4','0403'):continue
            candidates.setdefault(str(target),str(path))
    if len(candidates)!=1:raise ValueError(f'Expected one USB-UART candidate, found {len(candidates)}; select --device explicitly')
    return next(iter(candidates.values()))

def discover_lan_network():
    result=subprocess.run(['ip','-j','-4','addr','show','scope','global'],capture_output=True,text=True,check=True,timeout=3)
    networks=set()
    for item in json.loads(result.stdout):
        if item.get('operstate') not in ('UP','UNKNOWN'):continue
        for addr in item.get('addr_info',[]):
            if addr.get('family')!='inet':continue
            network=ipaddress.ip_network(f"{addr['local']}/{addr['prefixlen']}",strict=False)
            if network.is_private and not network.is_loopback:networks.add(str(network))
    if len(networks)!=1:raise ValueError('Cannot choose a unique LAN network; supply --lan-cidr')
    return next(iter(networks))
