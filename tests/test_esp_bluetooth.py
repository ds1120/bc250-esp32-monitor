import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import subprocess
import os
import pty
import termios

spec = importlib.util.spec_from_file_location('esp_bluetooth', Path(__file__).parents[1]/'host/esp_bluetooth.py')
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)

class BluetoothAttachTests(unittest.TestCase):
    def config(self, device):
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory)/'config.json'
            p.write_text(json.dumps({'device':device,'bluetooth_address':'02:00:00:00:00:01'}))
            return m.load_config(p)

    def test_explicit_port_validation(self):
        self.assertEqual(self.config('/dev/serial/by-path/pci-0000:00:10.0-usb-0:1:1.0-port0')[1], '02:00:00:00:00:01')
        for path in ['/dev/ttyS0', '/etc/passwd', '/dev/serial/by-id/../../etc/passwd', '/dev/ttyUSB0;reboot']:
            with self.assertRaises(ValueError):self.config(path)

    def test_no_flow_control_or_reset_program(self):
        command=m.attach_command('/dev/ttyUSB0')
        self.assertEqual(command[:2],['/usr/bin/python3','-I'])
        self.assertEqual(command[3:],['--attach-only','--device','/dev/ttyUSB0'])

    def test_uart_sets_both_speeds_before_kernel_attachment(self):
        master,slave=pty.openpty()
        calls=[]
        def ioctl(fd,request,value):
            attrs=termios.tcgetattr(fd)
            self.assertEqual(attrs[4:6],[termios.B921600]*2)
            self.assertFalse(attrs[2] & (termios.HUPCL|termios.CRTSCTS))
            calls.append(request)
            return 0
        try:
            with patch.object(m,'STOP',True),patch.object(m.fcntl,'ioctl',side_effect=ioctl),patch.object(m.socket,'socket'):
                m.attach_uart(os.ttyname(slave))
            self.assertEqual(calls,[termios.TIOCSETD,0x400455CB,0x400455C8,0x800455CA,termios.TIOCSETD])
        finally:
            os.close(slave);os.close(master)

    def test_only_explicit_hardware_restart_schedules_recovery(self):
        self.assertTrue(m.is_controller_restart(b'\x04\x10\x01\x01'))
        for packet in [b'',b'\x04\x0e\x04\x01\x03\x0c\x00',b'\x04\x10\x01\x02',b'\x04\x10\x01\x01\x00']:
            self.assertFalse(m.is_controller_restart(packet))

    def test_recovery_never_powers_unrelated_adapter(self):
        with patch.object(m,'Path') as path,patch.object(m.subprocess,'run') as run:
            path.return_value.resolve.return_value.name='ttyUSB0'
            path.return_value.__truediv__.return_value.resolve.return_value.parts=('devices','usb','hci1')
            self.assertFalse(m.restore_power_after_restart(1,'/dev/ttyUSB0'))
            run.assert_not_called()

    def test_duplicate_mac_blocks_registration(self):
        with patch.object(m.subprocess,'run',return_value=subprocess.CompletedProcess([],0,'Controller 02:00:00:00:00:01 BC-250 [default]\n','')):
            self.assertTrue(m.duplicate_controller('02:00:00:00:00:01'))
        with patch.object(m.subprocess,'run',return_value=subprocess.CompletedProcess([],0,'','')):
            self.assertFalse(m.duplicate_controller('02:00:00:00:00:01'))
        with patch.object(m.subprocess,'run',side_effect=subprocess.TimeoutExpired('bluetoothctl',5)):
            self.assertTrue(m.duplicate_controller('02:00:00:00:00:01'))

if __name__=='__main__':unittest.main()
