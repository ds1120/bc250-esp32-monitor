import importlib.util,pathlib,unittest,socket,threading,hashlib
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
from urllib.request import Request
spec=importlib.util.spec_from_file_location('wireless_update',pathlib.Path(__file__).parents[1]/'wireless_update.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
class TransportTests(unittest.TestCase):
 def test_smaller_segments_preserve_http_upload(self):
  received=[]
  class Handler(BaseHTTPRequestHandler):
   def log_message(self,*args):pass
   def do_POST(self):
    data=self.rfile.read(int(self.headers['Content-Length']))
    received.append((hashlib.sha256(data).digest(),self.connection.getsockopt(socket.IPPROTO_TCP,socket.TCP_MAXSEG)))
    self.send_response(200);self.send_header('Content-Length','2');self.end_headers();self.wfile.write(b'OK')
  server=ThreadingHTTPServer(('127.0.0.1',0),Handler);thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
  try:
   data=bytes(range(256))*2048
   for mss in [0,536]:
    with m.make_opener(mss).open(Request(f'http://127.0.0.1:{server.server_port}/upload',data=data),timeout=5) as response:self.assertEqual(response.read(),b'OK')
   self.assertEqual([r[0] for r in received],[hashlib.sha256(data).digest()]*2)
   self.assertLessEqual(received[1][1],536)
  finally:server.shutdown();server.server_close();thread.join()
