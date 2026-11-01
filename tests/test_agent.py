import importlib.util
import json
from pathlib import Path
import tempfile
import threading
import time
from types import SimpleNamespace
import unittest
import socket
import struct
import os
from urllib.request import Request, build_opener, ProxyHandler
from urllib.error import HTTPError
from http.server import ThreadingHTTPServer

spec=importlib.util.spec_from_file_location('agent',Path(__file__).resolve().parents[1]/'host/agent.py')
agent=importlib.util.module_from_spec(spec)
spec.loader.exec_module(agent)


class MonitoringTests(unittest.TestCase):
    @unittest.skipIf(os.name=='nt','BlueZ uses Linux MAC-address directory names; verified on BC-250')
    def test_only_supported_bonded_gamepads_are_exported(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            adapter=root/'02:00:00:00:00:01'
            for index,name,key in [(1,'Pro Controller','LinkKey'),(2,'DualSense Wireless Controller','LinkKey'),
                                   (3,'Xbox Wireless Controller','LongTermKey'),(4,'Headphones','LinkKey'),
                                   (5,'Pro Controller',None)]:
                folder=adapter/f'02:00:00:00:01:{index:02X}'
                folder.mkdir(parents=True)
                text='[General]\nName='+name+'\n'
                if key:text+='['+key+']\nKey='+'00'*16+'\n'
                if index==3:text+='[IdentityResolvingKey]\nKey='+'11'*16+'\n'
                (folder/'info').write_text(text)
            exported=agent.GamepadRegistry(root).read()
            self.assertEqual([x['family'] for x in exported['controllers']],['switch','ps5','xbox'])
            self.assertEqual(exported['controllers'][2]['irk'],'11'*16)
            self.assertNotIn('irk',exported['controllers'][0])

    def test_final_shutdown_frame_headers_and_checksums(self):
        payload=b'{"type":"bc250-poweroff-v1"}'
        frame=agent.FinalShutdownPacket.frame('02:00:00:00:00:01','02:00:00:00:00:02',
                                             '192.0.2.10','192.0.2.20',payload)
        self.assertEqual(frame[:6],bytes.fromhex('020000000002'))
        self.assertEqual(frame[12:14],b'\x08\x00')
        ip=frame[14:34]
        total=sum(struct.unpack('!10H',ip))
        while total>>16:total=(total&65535)+(total>>16)
        self.assertEqual(total,65535)
        self.assertEqual(ip[12:16],socket.inet_aton('192.0.2.10'))
        self.assertEqual(ip[16:20],socket.inet_aton('192.0.2.20'))
        self.assertEqual(struct.unpack('!HHHH',frame[34:42]),(9093,9092,8+len(payload),0))
        self.assertEqual(frame[42:],payload)

    def test_kernel_counters_missing_sensors_and_cpu_delta(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            proc=root/'proc'
            proc.mkdir()
            (proc/'stat').write_text('cpu 100 0 100 800 0 0 0 0 100 0\n')
            (proc/'meminfo').write_text('MemTotal: 16384 kB\nMemAvailable: 4096 kB\n')
            metrics=agent.Metrics(proc,root/'sys')
            (proc/'stat').write_text('cpu 120 0 110 870 0 0 0 0 120 0\n')
            value=metrics.collect()
            self.assertEqual(value['cpu_percent'],30)
            self.assertEqual(value['ram_used_bytes'],12288*1024)
            self.assertIsNone(value['gpu_percent'])
            self.assertIsNone(value['cpu_temp_c'])
            hw=root/'sys/class/hwmon/hwmon2'
            hw.mkdir(parents=True)
            (hw/'name').write_text('k10temp')
            (hw/'temp1_input').write_text('54750')
            device=root/'sys/class/drm/card1/device'
            gpu=device/'hwmon/hwmon3'
            gpu.mkdir(parents=True)
            for name,data in {'vendor':'0x1002','gpu_busy_percent':'42','mem_info_vram_used':'1073741824',
                              'mem_info_vram_total':'4294967296'}.items():
                (device/name).write_text(data)
            (gpu/'temp1_input').write_text('48000')
            value=metrics.collect()
            self.assertIsNone(value['cpu_percent'])  # No ticks advanced: not a measured zero.
            self.assertEqual(value['cpu_temp_c'],54.8)
            self.assertEqual(value['gpu_temp_c'],48)
            self.assertEqual(value['gpu_percent'],42)
            (device/'gpu_busy_percent').write_text('not supported')
            self.assertIsNone(metrics.collect()['gpu_percent'])

    def test_api_compatibility_authentication_and_power_guards(self):
        calls=[]
        controller=agent.Controller('a'*64,runner=lambda argv,**kw:(calls.append(argv) or SimpleNamespace(returncode=0)),
                    metrics=SimpleNamespace(collect=lambda:{'cpu_percent':30,'gpu_percent':None}))
        controller.state=lambda:{'device':'bc250','state':controller.pending or 'running',
                                 'uptime_seconds':123,'boot_id':'test-only'}
        server=ThreadingHTTPServer(('127.0.0.1',0),agent.make_handler(controller))
        threading.Thread(target=server.serve_forever,daemon=True).start()
        opener=build_opener(ProxyHandler({}))
        def call(path,method='GET',body=None,auth=True):
            headers={'Content-Type':'application/json'}
            if auth:headers['Authorization']='Bearer '+'a'*64
            req=Request(f'http://127.0.0.1:{server.server_port}'+path,
                        data=json.dumps(body).encode() if body is not None else None,headers=headers,method=method)
            try:
                with opener.open(req,timeout=3) as response:return response.status,json.load(response)
            except HTTPError as error:return error.code,json.load(error)
        try:
            self.assertEqual(call('/v1/metrics',auth=False)[0],401)
            status=call('/v1/status')[1]
            self.assertNotIn('metrics',status)  # Existing ESP32 has a 500-byte reply limit.
            self.assertLess(len(json.dumps(status)),500)
            self.assertIsNone(call('/v1/metrics')[1]['metrics']['gpu_percent'])
            for body in [{'confirm':False},{'confirm':1}]:
                self.assertEqual(call('/v1/shutdown','POST',body)[0],400)
            self.assertEqual(call('/v1/reboot','POST',{'confirm':True},auth=False)[0],401)
            self.assertEqual(calls,[])
            self.assertEqual(call('/v1/reboot','POST',{'confirm':True})[0],202)
            self.assertEqual(call('/v1/shutdown','POST',{'confirm':True})[0],409)
            time.sleep(2.1)
            self.assertEqual(calls,[['/usr/bin/systemctl','--no-block','reboot']])
        finally:
            server.shutdown()
            server.server_close()


if __name__=='__main__':unittest.main()
