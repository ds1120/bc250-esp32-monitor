import importlib.util
from pathlib import Path
import math
import struct
import tempfile
import unittest
from unittest.mock import patch,Mock
spec=importlib.util.spec_from_file_location('usb_monitor',Path(__file__).parents[1]/'host/usb_monitor.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)

class UsbMonitorTests(unittest.TestCase):
    def test_wire_format_validity_and_missing_values(self):
        state={'state':'running','boot_id':'00112233-4455-6677-8899-aabbccddeeff','uptime_seconds':123}
        packet=m.encode_sample(state,{'cpu_percent':25.5,'cpu_temp_c':50,'gpu_percent':None,'gpu_temp_c':float('nan'),'ram_used_bytes':2*1073741824},7)
        self.assertEqual(packet[:4],b'\x01\xf0\xfc\x42')
        values=struct.unpack(m.FORMAT,packet[4:])
        self.assertEqual(values[:4],(b'BC250M1\0',7,bytes.fromhex('00112233445566778899aabbccddeeff'),123))
        self.assertEqual(values[4],25.5);self.assertEqual(values[8],2.0)
        self.assertEqual(values[-1],256|1|2|16)
        state['state']='shutdown_requested'
        self.assertEqual(struct.unpack(m.FORMAT,m.encode_sample(state,{'cpu_percent':101},0)[4:])[-1],0)

    def test_unrelated_adapter_is_never_selected(self):
        with tempfile.TemporaryDirectory() as directory:
            p=Path(directory);dev=p/'ttyUSB0';dev.touch();sys=p/'bluetooth';sys.mkdir()
            usb=p/'usb6/bluetooth/hci0';usb.mkdir(parents=True);(sys/'hci0').symlink_to(usb)
            self.assertIsNone(m.find_controller(str(dev),sys))
            uart=p/'ttyUSB0-device/ttyUSB0/bluetooth/hci1';uart.mkdir(parents=True);(sys/'hci1').symlink_to(uart)
            self.assertEqual(m.find_controller(str(dev),sys),1)
    def test_admin_interval_echo_updates_usb_sender(self):
        with tempfile.TemporaryDirectory() as directory:
            config=Path(directory)/'config.json';config.write_text('{"device":"/dev/ttyUSB0"}')
            controller=Mock();controller.state.return_value={'state':'running','boot_id':'00112233-4455-6677-8899-aabbccddeeff','uptime_seconds':123};controller.metrics.collect.return_value={'cpu_percent':25.5}
            sock=Mock();sock.send.side_effect=lambda data:len(data);sock.recv.return_value=bytes([4,14,6,1,0xf0,0xfc,0,0xe8,3])
            with patch.object(m,'MONITOR_ENABLE_FILE',config),patch.object(m,'read_host_info',return_value=None),patch.object(m,'find_controller',return_value=1),patch.object(m.socket,'socket',return_value=sock),patch.object(m.select,'select',return_value=([sock],[],[])):
                publisher=m.UsbMonitorPublisher(controller,config);publisher.tick();self.assertEqual(publisher.interval,1.0)
                sock.bind.assert_called_once_with((1,0));self.assertEqual(sock.send.call_args.args[0][:4],bytes([1,0xf0,0xfc,66]))

    def test_disabled_monitor_never_opens_hci(self):
        with tempfile.TemporaryDirectory() as directory,patch.object(m.socket,'socket') as socket:
            with patch.object(m,'MONITOR_ENABLE_FILE',Path(directory)/'absent'):
                m.UsbMonitorPublisher(Mock()).tick();socket.assert_not_called()

    def test_host_info_wire_format_and_validation(self):
        info={'agent_host':'192.0.2.10','agent_port':9091,'bluetooth_address':'02:00:00:00:00:01','pc_name':'linux-pc'}
        packet=m.encode_host_info(info)
        self.assertEqual(packet[:4],bytes([1,0xf1,0xfc,84]))
        decoded=struct.unpack(m.INFO_FORMAT,packet[4:])
        self.assertEqual(decoded[0],b'BC250I1\0')
        self.assertEqual(decoded[2],9091)
        for key,value in [('agent_host','127.0.0.1'),('agent_port',0),('pc_name','bad name'),('bluetooth_address','01:00:00:00:00:01')]:
            with self.assertRaises(ValueError):m.encode_host_info({**info,key:value})

if __name__=='__main__' :unittest.main()
