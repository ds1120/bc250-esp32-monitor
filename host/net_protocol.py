"""Framed HCI vendor transport for simultaneous Bluetooth and emergency IPv4."""
import binascii,struct
OPCODE=0xFCF2
DATA_MAX=224
STREAMS=3
UDP_MAX=1472
HELLO,OPEN,DATA,CLOSE,ACK,UDP,DISABLE,PING,READY=range(1,10)
WELCOME,OPENED,RECEIVED,CLOSED,DATAGRAM,CONTROL=0x80,0x81,0x82,0x83,0x84,0x90
HEADER=struct.Struct('<2sBBBHH')

def encode(op,stream,session,sequence,data=b''):
 if len(data)>DATA_MAX:raise ValueError('network frame too large')
 body=HEADER.pack(b'BN',1,op,stream,session,sequence)+data
 return body+struct.pack('<H',binascii.crc_hqx(body,0xffff))

def decode(data):
 if not 11<=len(data)<=11+DATA_MAX:raise ValueError('network frame length')
 magic,version,op,stream,session,sequence=HEADER.unpack_from(data)
 if magic!=b'BN' or version!=1 or struct.unpack('<H',data[-2:])[0]!=binascii.crc_hqx(data[:-2],0xffff):raise ValueError('network frame integrity')
 return op,stream,session,sequence,data[HEADER.size:-2]

def command(op,stream,session,sequence,data=b''):
 payload=encode(op,stream,session,sequence,data)
 return struct.pack('<BHB',1,OPCODE,len(payload))+payload

def parse_ack(packet):
 if len(packet)==12 and packet[:3]==bytes([4,14,9]) and packet[4:7]==bytes([0xf2,0xfc,0]):
  return struct.unpack('<HHB',packet[7:])
 return None
