# bc250-esp32-monitor

**BC-250용 ESP32 시스템 모니터링 · 전원 관리 · USB 블루투스 실험 프로젝트**

ESP32-based monitoring, relay power management, and a Linux Bluetooth adapter for BC-250.

직접 장치를 만들고 배선을 맞추며 얻은 결과를 공유합니다. 혼자 만든 기능을 완성품으로 선언하기보다는, 더 좋은 아이디어와 경험을 가진 분들의 도움을 받아 개선하려고 공개했습니다. 제안·재현 가능한 버그 보고·문서 개선·PR을 환영합니다.

> 기준 하드웨어는 **ESP32-2432S028 / ST7789**입니다. 보드와 LCD 정보, 배선의 확인 범위는 **[하드웨어 문서](docs/HARDWARE.md)**를 먼저 확인하세요. 실제 릴레이 접점·ATX 배선도는 소유자가 다른 PC에서 추후 갱신할 예정입니다. 이 저장소에 검증된 전체 배선도가 있다고 가정하지 마세요.

## 기능

- LCD와 Wi-Fi 관리자 화면에서 CPU/GPU 사용률·온도, RAM/VRAM 상태 표시
- USB 모니터링 우선, 수신이 없으면 Wi-Fi 경로로 복귀
- Linux BlueZ의 표준 Bluetooth 어댑터로 ESP32 사용
- USB 비상 인터넷은 5.8.1에서 제거했습니다. [중단된 구현 기록](docs/EMERGENCY_NETWORK.md)
- PC 종료 후 등록된 패드의 재연결 신호를 감지하는 전원 켜기 기능
- 인증된 정상 종료 요청과 Linux의 마지막 종료 신호를 연동한 릴레이 OFF
- 호환된 OTA의 예정된 재부팅에서 릴레이 HIGH 상태 복구
- SD카드의 설정을 우선 읽고, 비어 있는 PC IP·이름·Bluetooth 정보만 USB에서 수집해 저장
- 모바일·데스크톱 관리자 화면, 4개 테마, 갱신 간격·SD 파일 관리

음성·키보드 등 모든 Bluetooth 장치의 호환성을 검증한 것은 아닙니다. Linux 표준 어댑터 등록과 패드 연결을 실물에서 확인했습니다. Windows용 USB Bluetooth 클래스 장치가 아닙니다.

## 버전과 검증

안정 공개판: **5.7**. 현재 장치 시험 빌드: **5.8.1** (USB 무선랜 제거, USB 연결 상태 부팅 재검증 필요), PlatformIO 환경: **`st7789`**.

| 항목 | 확인 결과 |
|---|---|
| 펌웨어 빌드·OTA 슬롯 용량·RTC 재시작 ABI | 통과 |
| Python 테스트 | 17개 통과 |
| H4, USB 정보 Python↔C++ 호환, 누락 항목 병합 정책 | 통과 |
| 실제 5.7 → 5.8.1 OTA | PC 실행과 부팅 ID 유지, 릴레이 HIGH 복구 확인 |
| 5.8.1 OTA 후 Bluetooth | 코드 유지, PC 어댑터 재연결 확인 필요 |
| USB 비상 인터넷 | 5.8.1에서 제거, PC 서비스 inactive/disabled |
| SD 관리자 화면 | 5.8.1 기본 화면 복원, 기존 동작 설정 보존 확인 |
| 완전히 빈 새 보드·빈 SD의 전체 최초 설치 | 아직 전체 실물 검증 전 |
| 실제 GPIO의 순간 파형·정전·브라운아웃·EN 리셋 | 출력 연속성 미계측·미보장 |

실제 판정은 [`binaries/verification-5.8.1.json`](binaries/verification-5.8.1.json)을 확인하세요. 실제 배선 쇼트 가능성은 소프트웨어가 진단하거나 방지하지 못합니다. 사용하지 않는 단자를 각각 절연하고 전원 배선은 별도로 검증해야 합니다.

## 파일 구성

```text
firmware/                 ESP32 코드·보드 설정·파티션·내장/SD 화면 생성
host/                     Linux 모니터링·전원 종료·USB H4 연결 프로그램
sdcard/bc250/             관리자 HTML/CSS/JS, 비밀정보 없는 설정 예제
binaries/                 배포용 앱 .bin, SHA-256, 검증 기록
configure.py              최초 Wi-Fi/PIN/토큰 설정
flash_panel.py            기존 호환 파티션/NVS를 보존하는 최초 앱 설치 도구
install_bc250.py          명시적으로 지정한 Linux PC에 SSH 설치
wireless_update.py        체크섬을 검증하는 Wi-Fi 업데이트 도구
tests/                    Python·C++ 회귀 테스트
docs/HARDWARE.md          하드웨어 기준·배선 확인 범위
docs/CONFIGURATION.md     자동 수집·SD 저장·설정 우선순위
PROJECT.md                프로젝트 원칙과 공개 범위
CONTRIBUTING.md           아이디어·이슈·PR 안내
```

## 준비

systemd 기반 Linux, BlueZ(`bluetoothctl`·`busctl`), `iproute2`, C/C++ 컴파일러, Python 3, 데이터용 USB 케이블, FAT32 SD카드가 필요합니다. 현재 실물 검증 환경은 CachyOS BC-250입니다. 다른 systemd 배포판은 설치 도구가 지원하지만 별도 검증이 필요합니다.

```bash
git clone https://github.com/ds1120/bc250-esp32-monitor.git
cd bc250-esp32-monitor
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
```

## 최초 설정 흐름

**펌웨어만 한 번 설치하면 모든 준비가 끝나는 것은 아닙니다.** 첫 사용에는 전원·릴레이 배선 확인, Wi-Fi/PIN 설정, Linux 프로그램 설치가 필요합니다. 이후 부팅은 저장된 SD/NVS 설정을 사용합니다.

1. [하드웨어 문서](docs/HARDWARE.md)를 확인하고 PC를 정상 종료합니다. 최초 USB 플래시·재설정은 릴레이/ATX 전원을 분리한 조건에서 진행하세요.
2. 같은 보드의 전체 플래시 백업을 개인 파일로 확보합니다. 전체 백업에는 인증 정보가 있을 수 있으므로 공개 저장소에 올리지 마세요.
3. 기존 파티션이 `firmware/partitions.csv`와 같으면 아래 도구로 앱만 설치합니다. 다른 레이아웃은 자동으로 거부합니다.
4. 처음 설정된 적이 없는 보드는 `configure.py`로 Wi-Fi와 관리자 PIN을 입력합니다. PC IP를 생략하면 USB에서 수집합니다.
5. Linux 프로그램을 설치합니다. USB 후보가 하나일 때만 자동 선택하며, 여러 개이면 `--device`로 지정합니다.
6. 다음 부팅에 USB로 연결되면 SD의 비어 있는 정보를 채웁니다. 관리자 화면에서 `모니터링 · USB`와 Bluetooth 상태를 확인합니다.

예시 경로는 자신의 실제 장치 경로로 바꾸세요.

```bash
# 전원이 분리된 상태에서, 선택한 보드의 백업
.venv/bin/python -m esptool --chip esp32 --port /dev/ttyUSB0 \
  read-flash 0x0 0x400000 factory.private.bin

# 호환 파티션을 가진 보드: 기존 NVS/터치 보정을 보존하는 앱 설치
.venv/bin/python flash_panel.py --port /dev/ttyUSB0 \
  --factory-backup factory.private.bin \
  --firmware binaries/bc250-monitor-5.8.1.bin --power-isolated

# 최초 Wi-Fi/PIN 설정. 기존 비밀 설정 파일이 있으면 누락된 값만 입력
.venv/bin/python configure.py --port /dev/ttyUSB0 --power-isolated

# 이 PC에 companion 설치. 개인 설정 파일 경로를 명시적으로 전달
sudo .venv/bin/python host/install_local.py \
  --settings "$HOME/.config/bc250-monitor/setup.json"
```

초기 파티션 배치가 다른 새 보드는 `pio run -d firmware -e st7789 -t upload --upload-port <실제 포트>`를 전원 분리 상태에서 사용해 빌드 설정과 파티션을 설치할 수 있습니다. 기존 데이터를 먼저 백업하세요. 최초 펌웨어 설치 도구와 완전히 빈 보드의 전체 설정은 별도 실물 검증이 남아 있습니다.

**터치 보정:** 현재 코드는 NVS의 기존 affine 보정을 읽어 사용합니다. 완전히 새 보드에 그 보정이 없으면 LCD 터치 조작을 사용할 수 없으며, 인증된 웹 관리 화면을 이용해야 합니다. 새 보드용 터치 보정 마법사는 아직 구현되지 않았습니다. 이 부분도 기여를 환영합니다.

원격 설치는 `install_bc250.py --host <실제 PC IP> --user <계정> --settings <개인 설정 파일>`로 실행합니다. LAN 대역이 여러 개이면 `--lan-cidr`를 명시하세요. 개인 IP·계정·MAC 기본값은 포함하지 않습니다.

## 이후 업데이트

관리자 화면의 **무선 업데이트**에서 앱 파일 `binaries/bc250-monitor-5.8.1.bin`을 선택합니다. Wi-Fi·PIN·토큰과 기존 터치 보정은 NVS에 남습니다. SD 화면은 업데이트 시 사용자 파일을 자동 덮어쓰지 않으므로, 새 기본 화면이 필요하면 ‘펌웨어 기본화면으로 복원’을 선택하세요.

보조전원 없는 시험은 현재 장치에서 성공했지만 예정된 OTA 소프트웨어 재부팅에 한정됩니다. USB 업로더·전원 상실·EN 리셋까지 보장하는 기능은 아닙니다. 릴레이가 OFF이면 PC 본체 버튼만으로 켜지지 않을 수 있어 ESP32 전원을 확보한 뒤 관리자 화면에서 전원 공급을 켜야 합니다.

```bash
# 기본은 PC가 꺼진 상태의 OTA
.venv/bin/python wireless_update.py --panel <현재 ESP32 IP> \
  --settings "$HOME/.config/bc250-monitor/setup.json" \
  --firmware binaries/bc250-monitor-5.8.1.bin
```

전원 유지 시험은 작업을 저장한 뒤 `--keep-power-test`를 명시합니다. 관리자 HTTP API는 신뢰할 수 있는 LAN에서 사용하며 인터넷에 직접 노출하지 마세요.

## 소스에서 빌드

```bash
.venv/bin/pip install -r requirements-dev.txt
.venv/bin/pio run -d firmware -e st7789
# 출력: firmware/.pio/build/st7789/firmware.bin
.venv/bin/python -m unittest discover -s tests -p 'test_*.py'
```

LCD는 자동 판별하지 않습니다. 현재 보드의 드라이버 ID는 **ST7789**이며, `ili9341` 환경은 별도 패널용입니다. 업로드 포트는 고정하지 않고 사용자가 선택합니다. HTML/CSS/JS의 내장 복구 화면과 SD 기본파일은 빌드 때 원본에서 생성됩니다.

## 함께 개선하고 싶은 부분

- 릴레이·ATX·USB 전원 배선의 재현 가능한 문서화와 실제 회로 검증
- 빈 보드의 최초 설정·터치 보정 경험
- 여러 Linux 배포판·여러 USB 장치가 있는 환경
- 다양한 패드의 페어링·입력·종료 후 깨우기 호환성
- OTA와 통신 오류에서의 복구 동작, 실제 GPIO 측정

아이디어는 [Issues](https://github.com/ds1120/bc250-esp32-monitor/issues)에 남겨주세요. 변경 제안은 [기여 안내](CONTRIBUTING.md)를 확인해주세요. PIN·토큰·Wi-Fi 비밀번호·BLE IRK·실제 전체 플래시 백업은 첨부하지 마세요.

## 라이선스

프로젝트 코드는 [MIT](LICENSE)입니다. 포함된 외부 라이브러리의 저작권·라이선스는 각 파일과 [THIRD_PARTY.md](THIRD_PARTY.md)를 따릅니다.
