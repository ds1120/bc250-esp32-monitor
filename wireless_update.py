"""Authenticated Wi-Fi update; live relay-hold tests require an explicit flag."""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import secrets
import time
import urllib.request


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--panel',required=True,help='Current ESP32 address from its screen or your router')
    parser.add_argument('--settings',type=Path,required=True)
    parser.add_argument('--firmware',type=Path,default=Path(__file__).resolve().parent/'binaries/firmware.bin')
    parser.add_argument('--keep-power-test',action='store_true',help='Explicitly test OTA while the BC250 relay stays ON; save all work first')
    parser.add_argument('--expected-version',help='Expected full firmware version after reboot')
    parser.add_argument('--report',type=Path,help='Save credential-free update verification here')
    args=parser.parse_args()
    settings=json.loads(args.settings.read_text(encoding='utf-8-sig'))
    authorization='Basic '+base64.b64encode(('bc250:'+settings['pin']).encode()).decode()
    opener=urllib.request.build_opener(urllib.request.ProxyHandler({}))
    def status():
        with opener.open(urllib.request.Request('http://'+args.panel+'/api/status',
                         headers={'Authorization':authorization}),timeout=4) as response:return json.load(response)
    before=status()
    live=before['relay_output']=='HIGH'
    if live and not args.keep_power_test:raise SystemExit('Shut down BC-250 first, or explicitly select --keep-power-test')
    if live and not before.get('ota_keep_power_supported'):raise SystemExit('Running firmware does not support relay-hold OTA')
    if not before.get('ota_supported'):raise SystemExit('This firmware needs its initial USB update')
    image=args.firmware.read_bytes()
    if not image or image[0]!=0xe9:raise SystemExit('Invalid ESP32 image')
    if live and b'BC250-RELAY-HOLD1@50000200' not in image:raise SystemExit('Image does not declare the compatible relay-hold ABI')
    boot_file=Path('/proc/sys/kernel/random/boot_id')
    pc_boot_before=boot_file.read_text().strip() if boot_file.exists() else None
    digest=hashlib.sha256(image).hexdigest()
    boundary='BC250'+secrets.token_hex(12)
    body=(f'--{boundary}\r\nContent-Disposition: form-data; name="firmware"; filename="firmware.bin"\r\n'
          'Content-Type: application/octet-stream\r\n\r\n').encode()+image+f'\r\n--{boundary}--\r\n'.encode()
    request=urllib.request.Request('http://'+args.panel+'/api/update',data=body,
             headers={'Authorization':authorization,'X-BC250-UI':'1','X-Firmware-SHA256':digest,
                      'Content-Type':'multipart/form-data; boundary='+boundary},method='POST')
    with opener.open(request,timeout=45) as response:assert json.load(response)['updated'] is True
    time.sleep(3)
    for attempt in range(20):
        try:
            after=status()
            if after['running_partition']!=before['running_partition']:
                assert after['configured'] and after.get('touch_calibrated')==before.get('touch_calibrated') and after['relay_output']==('HIGH' if live else 'LOW')
                if args.expected_version:assert after['firmware']==args.expected_version
                if live:assert after.get('relay_resumed_after_restart') is True
                report={'wireless_update_verified':True,'firmware':after['firmware'],
                    'partition_before':before['running_partition'],'partition_after':after['running_partition'],
                    'sha256':digest,'saved_settings_preserved':True,
                    'keep_power_test':live,'relay_resumed_after_restart':after.get('relay_resumed_after_restart'),
                    'pc_boot_id_unchanged':boot_file.read_text().strip()==pc_boot_before if pc_boot_before else None,
                    'physical_gpio_continuity_measured':False}
                if args.report:args.report.write_text(json.dumps(report,indent=2)+'\n')
                print(json.dumps(report))
                return
        except (OSError,ValueError):pass
        time.sleep(1)
    raise SystemExit('Upload accepted; reboot confirmation unavailable. Check the LCD before using power controls.')


if __name__=='__main__':main()
