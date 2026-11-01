"""Keep the one-shot RTC relay restart ticket ABI stable across OTA images."""
Import('env')
from pathlib import Path
import subprocess

def check_abi(source,target,env):
    nm=Path(env.PioPlatform().get_package_dir('toolchain-xtensa-esp32'))/'bin/xtensa-esp32-elf-nm'
    symbols=subprocess.check_output([str(nm),'-g',str(target[0])],text=True)
    line=next((line for line in symbols.splitlines() if line.endswith(' restartTicket')),None)
    if not line or int(line.split()[0],16)!=0x50000200:
        raise RuntimeError('Relay restart RTC ABI moved; do not ship this OTA image with keep-power support')
    print('Relay restart ABI verified: 0x50000200')
env.AddPostAction('$BUILD_DIR/${PROGNAME}.elf',check_abi)
