import importlib.util,json,subprocess,tempfile,unittest
from pathlib import Path
from unittest.mock import patch
s=importlib.util.spec_from_file_location('environment',Path(__file__).parents[1]/'host/environment.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m)
class DiscoveryTests(unittest.TestCase):
 def test_same_uart_aliases_choose_one_stable_path(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d);base=root/'serial';(base/'by-id').mkdir(parents=True);(base/'by-path').mkdir();tty=root/'ttyUSB0';tty.touch()
   a=base/'by-path/pci-usb-port0';a.symlink_to(tty);(base/'by-id/usb-serial').symlink_to(tty)
   with patch.object(m.subprocess,'run',return_value=subprocess.CompletedProcess([],0,'ID_VENDOR_ID=1a86\n')):
    self.assertEqual(m.discover_usb_device(base),str(a))
    other=root/'ttyUSB1';other.touch();(base/'by-path/other').symlink_to(other)
    with self.assertRaises(ValueError):m.discover_usb_device(base)
 def test_multiple_lan_networks_require_selection(self):
  data=[{'operstate':'UP','addr_info':[{'family':'inet','local':'10.2.3.4','prefixlen':24}]}]
  with patch.object(m.subprocess,'run',return_value=subprocess.CompletedProcess([],0,json.dumps(data))):self.assertEqual(m.discover_lan_network(),'10.2.3.0/24')
  data.append({'operstate':'UP','addr_info':[{'family':'inet','local':'172.20.1.2','prefixlen':24}]})
  with patch.object(m.subprocess,'run',return_value=subprocess.CompletedProcess([],0,json.dumps(data))):
   with self.assertRaises(ValueError):m.discover_lan_network()
if __name__=='__main__':unittest.main()
