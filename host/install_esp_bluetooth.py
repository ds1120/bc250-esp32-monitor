#!/usr/bin/python3
"""Install automatic ESP32 Bluetooth attachment. Does not open/reset USB.

Select the ESP32's exact USB by-id/by-path name after connecting with PC off.
Installation enables NEXT BOOT only. It never starts btattach in the running PC.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import time
from esp_bluetooth import load_config
from environment import discover_usb_device


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device', help='Exact USB-UART path; auto-selects only one candidate when omitted')
    parser.add_argument('--bluetooth-address', help='Optional legacy adapter identity to avoid duplicates; native identity is automatic')
    args = parser.parse_args()
    if os.geteuid() != 0:
        raise SystemExit('Run this installer with pkexec or sudo')
    if not Path('/usr/bin/bluetoothctl').exists():
        raise SystemExit('Install BlueZ and bluetoothctl first')
    config_path=Path('/etc/bc250-esp-bluetooth.json')
    existing=json.loads(config_path.read_text()) if config_path.exists() else {}
    device=args.device or existing.get('device') or discover_usb_device()
    address=args.bluetooth_address if args.bluetooth_address is not None else existing.get('bluetooth_address')
    config = {'device': device, 'bluetooth_address': address}
    # Validate before touching the system. No USB access.
    import tempfile
    with tempfile.TemporaryDirectory() as temporary:
        candidate = Path(temporary) / 'config.json'
        candidate.write_text(json.dumps(config))
        load_config(candidate)
    root = Path(__file__).resolve().parent
    backup = Path('/var/lib/bc250-esp-bluetooth') / time.strftime('%Y%m%d-%H%M%S')
    backup.mkdir(parents=True, mode=0o700)
    targets = [Path('/etc/bc250-esp-bluetooth.json'),
               Path('/usr/local/lib/bc250/esp_bluetooth.py'),
               Path('/etc/systemd/system/bc250-esp-bluetooth.service')]
    for target in targets:
        if target.exists():
            shutil.copy2(target, backup / target.name)
    targets[1].parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(root / 'esp_bluetooth.py', targets[1])
    targets[1].chmod(0o755)
    shutil.copyfile(root / 'bc250-esp-bluetooth.service', targets[2])
    targets[2].chmod(0o644)
    targets[0].write_text(json.dumps(config, indent=2) + '\n')
    targets[0].chmod(0o600)
    # Deploy the USB telemetry sender beside the existing agent; next boot only.
    agent_dir = Path('/var/lib/bc250-power-manager')
    if agent_dir.exists():
        for name in ('agent.py','usb_monitor.py'):
            destination = agent_dir / name
            if destination.exists():shutil.copy2(destination,backup / name)
            shutil.copyfile(root / name,destination)
            destination.chmod(0o755)
        unit = Path('/etc/systemd/system/bc250-power-manager.service')
        if unit.exists():shutil.copy2(unit,backup / unit.name)
        shutil.copyfile(root / unit.name,unit)
        unit.chmod(0o644)
    subprocess.run(['systemctl', 'daemon-reload'], check=True)
    subprocess.run(['systemctl', 'enable', 'bc250-esp-bluetooth.service'], check=True)
    print(json.dumps({'enabled_for_next_boot': True, 'started': False,
                      'usb_opened': False, 'backup': str(backup)}))


if __name__ == '__main__':
    main()
