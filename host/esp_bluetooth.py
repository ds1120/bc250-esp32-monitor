#!/usr/bin/python3
"""Attach one explicitly configured ESP32 USB-UART controller to Linux BlueZ.

Uses the Linux H4 line discipline; no uploader or DTR/RTS reset pulses.
The kernel HCI Reset command resets Bluetooth only, never ESP32 GPIOs.
"""
import binascii
import argparse
import fcntl
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import time
import struct
import termios
import socket
import select
import binascii

CONFIG = Path('/etc/bc250-esp-bluetooth.json')
STOP = False


def load_config(path):
    data = json.loads(Path(path).read_text())
    device = data.get('device', '')
    # Select one USB serial device explicitly; never probe arbitrary serial ports.
    if not re.fullmatch(r'/dev/(?:serial/by-(?:id|path)/[A-Za-z0-9_.:+-]+|ttyUSB[0-9]+)', device):
        raise ValueError('Expected an explicit /dev/serial/by-id/... or by-path/... USB device')
    mac = (data.get('bluetooth_address') or '').upper()
    if mac and not re.fullmatch(r'(?:[0-9A-F]{2}:){5}[0-9A-F]{2}', mac):
        raise ValueError('Invalid ESP32 Bluetooth address')
    return device, mac


def attach_command(device):
    return ['/usr/bin/python3', '-I', str(Path(__file__).resolve()),
            '--attach-only', '--device', device]


def is_controller_restart(packet):
    # ESP32 sends standard HCI Hardware Error after a planned MCU restart.
    # A user turning Bluetooth off does not emit this event.
    return packet == b'\x04\x10\x01\x01'

def requested_usb_action(packet):
    if len(packet)!=15 or packet[:3]!=b'\x04\xff\x0c':return None
    body=packet[3:]
    if binascii.crc_hqx(body[:-2],0xffff)!=int.from_bytes(body[-2:],'little'):return None
    if struct.unpack('<2sBBBHH',body[:9])!=(b'BN',1,0x90,0,0,0):return None
    return {0:'network-off',1:'network-on',2:'monitor-off',3:'monitor-on'}.get(body[9])


def request_usb_action(action,index,device):
    hci=Path('/sys/class/bluetooth')/f'hci{index}'
    if Path(device).resolve().name not in hci.resolve().parts:return False
    if action in ('monitor-on','monitor-off'):
        marker=Path('/run/bc250-usb-features/monitor.enabled')
        if action=='monitor-on':marker.write_text('enabled from authenticated panel UI\n');marker.chmod(0o600)
        else:marker.unlink(missing_ok=True)
        return True
    if action not in ('network-on','network-off'):return False
    return subprocess.run(['/usr/bin/systemctl','--no-block','start' if action=='network-on' else 'stop','bc250-esp-network.service'],capture_output=True,text=True,timeout=5).returncode==0


def restore_power_after_restart(index, device):
    hci = Path('/sys/class/bluetooth') / f'hci{index}'
    if Path(device).resolve().name not in hci.resolve().parts:
        return False  # Never power an unrelated adapter.
    result = subprocess.run(['/usr/bin/busctl', '--system', '--timeout=5s',
                             'set-property', 'org.bluez', f'/org/bluez/hci{index}',
                             'org.bluez.Adapter1', 'Powered', 'b', 'true'],
                            capture_output=True, text=True, timeout=7)
    return result.returncode == 0


def attach_uart(device):
    """Configure both UART speeds before attaching the kernel H4 transport.

    BlueZ 5.87's btattach sets only c_cflag on this host; the observed output
    speed is B0 and HCI Reset times out. tcsetattr here sets both speeds using
    the platform's B921600 constant. Do not pulse DTR/RTS or enable HUPCL.
    """
    Path('/run/bc250-usb-features/monitor.enabled').unlink(missing_ok=True)
    subprocess.run(['/usr/bin/systemctl','--no-block','stop','bc250-esp-network.service'],capture_output=True,timeout=5)
    fd = os.open(device, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    attached = False
    events = None
    try:
        attrs = termios.tcgetattr(fd)
        attrs[0] = attrs[1] = attrs[3] = 0
        attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        attrs[4] = attrs[5] = termios.B921600
        attrs[6][termios.VMIN] = 0
        attrs[6][termios.VTIME] = 0
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
        actual = termios.tcgetattr(fd)
        if actual[4:6] != [termios.B921600, termios.B921600]:
            raise RuntimeError('USB UART did not accept 921600 baud in both directions')
        termios.tcflush(fd, termios.TCIOFLUSH)
        fcntl.ioctl(fd, termios.TIOCSETD, struct.pack('i', 15))  # N_HCI
        attached = True
        fcntl.ioctl(fd, 0x400455CB, 2)  # HCIUARTSETFLAGS: RESET_ON_INIT
        fcntl.ioctl(fd, 0x400455C8, 0)  # HCIUARTSETPROTO: H4
        index = fcntl.ioctl(fd, 0x800455CA, 0)  # HCIUARTGETDEVICE
        log(f'Kernel H4 controller hci{index} attached at 921600 baud')
        events = socket.socket(socket.AF_BLUETOOTH, socket.SOCK_RAW, socket.BTPROTO_HCI)
        events.setblocking(False)
        events.setsockopt(0, 2, struct.pack('<IIIH2x', 1 << 4, 1 << 16, 1 << 31, 0))
        events.bind((index, 0))
        retry_at = 0
        retries = 0
        while not STOP and Path(device).exists():
            if select.select([events], [], [], 0.5)[0]:
                try:
                    packet = events.recv(260)
                except BlockingIOError:
                    packet = b''
                action=requested_usb_action(packet)
                if action:
                    log('USB feature request: '+action+' accepted='+str(request_usb_action(action,index,device)))
                if is_controller_restart(packet):
                    log('ESP32 enabled/restarted USB; reattaching HCI transport')
                    return
            if retry_at and time.monotonic() >= retry_at:
                try:
                    recovered = restore_power_after_restart(index, device)
                except (OSError, subprocess.TimeoutExpired):
                    recovered = False
                retries += 1
                if recovered:
                    log('Bluetooth powered ON after ESP32 restart')
                    retry_at = 0
                elif retries >= 5:
                    log('Bluetooth recovery failed after five attempts; Wi-Fi monitoring remains available')
                    retry_at = 0
                else:
                    retry_at = time.monotonic() + 2 * retries
    finally:
        if events is not None:
            events.close()
        if attached:
            try:
                fcntl.ioctl(fd, termios.TIOCSETD, struct.pack('i', 0))
            except OSError:
                pass
        os.close(fd)


def duplicate_controller(mac):
    if not mac:return False  # Native ESP32 identity is discovered by BlueZ after attachment.
    try:
        result = subprocess.run(['/usr/bin/bluetoothctl', 'list'], text=True,
                                capture_output=True, timeout=5)
    except (OSError, subprocess.TimeoutExpired):
        return True  # Do not attach while duplicate identity cannot be checked.
    return result.returncode != 0 or any(
        line.startswith('Controller ' + mac + ' ') for line in result.stdout.splitlines())


def log(message):
    print(message, flush=True)


def stop_signal(*_):
    global STOP
    STOP = True


def supervise(device, mac):
    global STOP
    child = None
    last_reason = None
    try:
        while not STOP:
            if child:
                if child.poll() is not None:
                    log('Bluetooth attachment exited; waiting before retry')
                    child = None
                    time.sleep(2)
                elif not Path(device).exists():
                    log('Configured USB cable removed; detaching Bluetooth')
                    child.terminate()
                    try:
                        child.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        child.kill()
                        child.wait()
                    child = None
                else:
                    time.sleep(1)
                continue
            reason = 'waiting for configured ESP32 USB device' if not Path(device).exists() else \
                     'remove the old Bluetooth dongle with the same address first' if duplicate_controller(mac) else None
            if reason:
                if reason != last_reason:
                    log(reason)
                    last_reason = reason
                time.sleep(1)
                continue
            last_reason = None
            log('Attaching ESP32 as standard Bluetooth controller (H4, 921600 baud)')
            child = subprocess.Popen(attach_command(device))
            time.sleep(1)
    finally:
        if child and child.poll() is None:
            child.terminate()
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', type=Path, default=CONFIG)
    parser.add_argument('--check', action='store_true', help='Validate only; never open USB')
    parser.add_argument('--attach-only', action='store_true', help=argparse.SUPPRESS)
    parser.add_argument('--device', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.attach_only:
        if os.geteuid() != 0 or not args.device:
            raise SystemExit('Internal UART attachment requires root and an explicit device')
        for sig in (signal.SIGTERM, signal.SIGINT):
            signal.signal(sig, stop_signal)
        attach_uart(args.device)
        return
    device, mac = load_config(args.config)
    if args.check:
        print(json.dumps({'device': device, 'bluetooth_address': mac,
                          'device_present': Path(device).exists(), 'command': attach_command(device),
                          'usb_opened': False}))
        return
    if os.geteuid() != 0:
        raise SystemExit('The system service needs root to register an HCI UART controller')
    for sig in (signal.SIGTERM, signal.SIGINT):
        signal.signal(sig, stop_signal)
    subprocess.run(['/usr/bin/modprobe', 'hci_uart'], check=True)
    # With no physical Bluetooth dongle, bluetooth.service may have been
    # skipped before hci_uart created /sys/class/bluetooth. Start it again
    # before asking BlueZ to check for duplicate controller identities.
    subprocess.run(['/usr/bin/systemctl', 'start', 'bluetooth.service'], check=True)
    Path('/run/bc250-usb-features/monitor.enabled').unlink(missing_ok=True)
    supervise(device, mac)


if __name__ == '__main__':
    main()
