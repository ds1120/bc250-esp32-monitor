# bc250-esp32-monitor

**BC-250용 ESP32 Wi-Fi 시스템 모니터링 · 릴레이 전원 관리 · 등록 패드 깨우기 프로젝트**

직접 장치를 만들고 배선을 맞추며 얻은 결과를 공유합니다. 더 좋은 아이디어와 경험을 가진 분들의 도움을 받아 개선하려고 공개했습니다. 제안·재현 가능한 버그 보고·문서 개선·PR을 환영합니다.

기준 하드웨어는 **ESP32-2432S028 / ST7789**입니다. 보드 정보와 확인된 배선 범위는 [하드웨어 문서](docs/HARDWARE.md)를 먼저 확인하세요. 실제 릴레이 접점·ATX 배선도는 소유자가 추후 갱신할 예정이며 전체 배선이 검증됐다고 가정하면 안 됩니다.

## 현재 버전: 5.8.6

- 잘못된 패드 설정이 Wi-Fi MAC을 0으로 만드는 부팅 문제를 방지합니다. Wi-Fi는 eFuse 원래 주소를 사용하며, 잘못된 설정은 내부 `pad_cfg_bad`에 보관 후 빈 패드 목록으로 복구합니다. 정상 등록 목록·수동 모드·Wi-Fi/PIN/토큰은 유지합니다.
- SD 상태 등 알림을 우측 하단의 작은 알림으로 변경했습니다. 확인 버튼 없이 3초 후 닫힙니다. SD 없이 내장 화면에서도 적용됩니다.
- 개인 프로콘 주소는 장치 NVS에 보존하며 공개 소스·기본 바이너리에는 포함하지 않습니다. 패드 실물 깨우기와 GPIO 순간 파형은 별도 실측이 필요합니다.


평상시에는 **USB-C 데이터 케이블을 연결하지 않고 Wi-Fi로 운용**합니다. USB 연결 상태에서 BC250 부팅이 진행되지 않는 문제가 5.5에서도 보고됐고, 원인은 아직 확정하지 못했습니다. 5.8.5는 관리자 화면의 USB 제어·상태·시험 메뉴와 실행 펌웨어의 USB HCI/모니터링/무선랜 통합을 제거합니다. PC USB 서비스도 사용하지 않습니다. 최초 USB 플래시/설정 도구와 이전 실험 소스는 남아 있지만, 현재 실행 기능이 아닙니다.

- Wi-Fi로 CPU/GPU 사용률·온도, RAM/VRAM 표시
- Wi-Fi 관리자 화면과 LCD에서 정상 종료·재부팅·릴레이·화면 제어
- 인증된 Linux 최종 종료 신호를 확인한 뒤 릴레이 OFF
- 호환된 Wi-Fi OTA의 예정된 재부팅에서 릴레이 HIGH 유지/복구
- 등록된 지원 패드의 재연결 신호로 전원 켜기 대기
- 유효한 SD 설정을 내부 NVS에도 저장해 핵심 PC 연결·패드·갱신 설정 유지
- SD 관리자 화면 또는 내장 복구 화면, 테마·갱신 간격·SD 파일 관리

실제 전원 상태 센서는 없으며 릴레이 출력 요청 상태를 표시합니다. 무선 패드 깨우기는 USB Bluetooth 수신기 기능과 별개입니다. 모든 패드의 호환성이나 현재 배선에서의 실제 깨우기 동작을 보장하지 않습니다.

## 검증 상태

| 항목 | 결과 |
|---|---|
| ST7789 빌드·OTA 슬롯 크기·RTC 재시작 ABI | 통과 |
| Python 회귀 테스트 | 26개 통과 (이전 USB 모듈 테스트 포함) |
| Wi-Fi 관리자 화면 | USB 항목 제거 후 상태 갱신·전원 버튼 회귀 검사 통과 |
| 실제 5.5 → 5.8.5 Wi-Fi OTA | PC 부팅 ID 유지, 릴레이 HIGH 복구 확인 |
| 현재 SD 관리자 화면 | 새 기본 화면 반영, 동작 설정 보존 확인 |
| 개인 패드 설정 | 사용자 장치의 패드 1개를 NVS 저장, 업데이트 후 일치 확인; 공개 소스에 주소 미포함 |
| 내부 핵심 설정 | PC 연결/패드 설정 일치 상태 확인 |
| SD를 실제로 뺀 부팅, 새 빈 보드 최초 설치 | 전체 실물 검증 전 |
| USB 연결 상태 부팅 문제 | 미해결; 5.5도 동일 증상 보고 |
| GPIO 순간 파형·정전·브라운아웃·EN 리셋 | 출력 연속성 미계측·미보장 |

[현재 검증 기록](binaries/verification-5.8.6.json), [OTA 중단 대응](docs/OTA_RECOVERY.md), [부팅 진단](docs/BOOT_DIAGNOSIS.md)을 확인하세요. 예기치 않은 종료를 조사하며 걸었던 원격 종료 차단은 사용자 요청으로 해제됐습니다. 관리자 화면과 LCD 모두 사용하며 요청 출처·결과는 journal에 남습니다.

## 최초 준비

systemd 기반 Linux, Python 3, C/C++ 컴파일러, iproute2가 필요합니다. 현재 실물 검증 환경은 CachyOS BC-250입니다. 다른 배포판은 별도 검증이 필요합니다. SD는 FAT32를 사용하며 사용자 화면/파일을 쓰지 않으면 핵심 운용에 필수는 아닙니다.

```bash
git clone https://github.com/ds1120/bc250-esp32-monitor.git
cd bc250-esp32-monitor
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
```

최초 USB 플래시·설정은 PC/릴레이 전원을 분리한 상태에서 수행합니다. 같은 보드의 전체 플래시 백업에는 비밀정보가 있으므로 개인적으로 보관합니다. 호환 파티션의 앱 설치 예시:

```bash
.venv/bin/python -m esptool --chip esp32 --port /dev/ttyUSB0 \
  read-flash 0x0 0x400000 factory.private.bin
.venv/bin/python flash_panel.py --port /dev/ttyUSB0 \
  --factory-backup factory.private.bin \
  --firmware binaries/bc250-monitor-5.8.6.bin --power-isolated

# 실제 Wi-Fi와 PC 주소를 입력. 개인 주소 기본값은 포함하지 않음
.venv/bin/python configure.py --port /dev/ttyUSB0 \
  --host <실제_PC_IP> --power-isolated
sudo .venv/bin/python host/install_local.py \
  --settings "$HOME/.config/bc250-monitor/setup.json"
```

새 보드의 초기 파티션 배치가 다르면 PlatformIO의 전체 업로드를 전원 분리 상태에서 수행해야 합니다. 현재 터치 코드는 NVS의 기존 affine 보정을 읽으며 새 보드용 보정 마법사는 없습니다. 보정이 없는 새 장치는 내부망 웹 관리 화면을 사용합니다. LCD는 런타임 자동 감지하지 않으며 ST7789 이외 보드는 핀/드라이버 검토가 필요합니다.

Linux 설치기는 현재 Wi-Fi companion만 설치하며 USB 어댑터 설치기를 자동 실행하지 않습니다. PC 주소는 최초 설정 또는 SD의 `network.agent_host`에서 지정합니다. 기존 USB 자동 정보 수집은 현재 버전에 없습니다. 원격 설치는 `install_bc250.py --host <실제_PC_IP> --user <계정> --settings <개인_설정파일>`을 사용합니다.

## SD와 내부 설정

시작할 때 내부 NVS 값을 읽고, 유효한 SD 설정이 있으면 그 값을 적용해 핵심 항목을 NVS에도 보존합니다. PC 주소·포트, 패드 대상/주소·수동 모드·활성화, 모니터링 간격과 화면 복귀 시간을 저장합니다. Wi-Fi 비밀번호·관리자 PIN·서버 토큰·기존 터치 보정은 NVS에 남습니다. 개인 패드 주소와 IRK는 공개 기본값에 넣지 않습니다.

SD가 없으면 내장 관리자 화면과 NVS의 핵심 설정을 사용하도록 설계됐습니다. SD의 사용자 이미지·웹 파일·일부 LCD 외형 설정은 SD 없이 제공되지 않거나 기본값으로 돌아갑니다. 현재 장치의 실제 카드 제거 부팅은 아직 별도 확인 전입니다. 탈착은 전원을 끈 상태에서 합니다. [설정 설명](docs/CONFIGURATION.md)을 참고하세요.

## 이후 Wi-Fi 업데이트

관리자 화면에서 `binaries/bc250-monitor-5.8.6.bin`을 선택하거나 다음 도구를 사용합니다. SD의 새 기본 화면은 별도로 ‘펌웨어 기본화면으로 복원’해야 하며 기존 동작 설정은 보존됩니다.

```bash
.venv/bin/python wireless_update.py --panel <현재_ESP32_IP> \
  --settings "$HOME/.config/bc250-monitor/setup.json" \
  --firmware binaries/bc250-monitor-5.8.6.bin
```

기본은 PC가 꺼진 상태이며, 실행 중 전원 유지 시험은 작업을 저장한 뒤 `--keep-power-test`를 명시합니다. PC 업로더는 작은 TCP 세그먼트(536)와 기본 120초 제한을 사용하며 느린 링크는 `--upload-timeout 300`으로 조절합니다. 업로드가 멈췄다고 ESP32 리셋 버튼을 누르면 PC 전원이 끊길 수 있습니다. 예정된 OTA 소프트웨어 재부팅과 EN/정전/USB 업로더 리셋은 다른 조건입니다.

## 소스에서 빌드

```bash
.venv/bin/pip install -r requirements-dev.txt
.venv/bin/pio run -d firmware -e st7789
.venv/bin/python -m unittest discover -s tests -p 'test_*.py'
node tests/wifi_admin_test.js
```

출력은 `firmware/.pio/build/st7789/firmware.bin`입니다. 내장/SD 기본 화면은 HTML/CSS/JS 원본에서 생성됩니다. 일부 이전 USB 실험용 소스/테스트는 남아 있으나 현재 `main.cpp`에는 연결되지 않으며 현행 기능으로 취급하지 않습니다. 이전 실험 설치기를 현재 버전에 실행하지 마세요.

하드웨어 배선 재현, 빈 보드 초기 설정, 패드 깨우기 호환성, 여러 배포판, 전원/통신 복구 개선을 환영합니다. [Issues](https://github.com/ds1120/bc250-esp32-monitor/issues)와 [기여 안내](CONTRIBUTING.md)를 이용하세요. 비밀번호·PIN·토큰·IRK·전체 플래시 백업은 첨부하지 마세요.

프로젝트 코드는 [MIT](LICENSE)이며 외부 라이브러리는 [THIRD_PARTY.md](THIRD_PARTY.md)와 각 파일의 라이선스를 따릅니다. 수정된 WebServer의 LGPL 라이선스와 전체 소스도 함께 제공합니다.

5.8.6 관리자 페이지/API/OTA는 사용자 요청에 따라 로그인 없이 동작합니다. 같은 LAN에서 전원 제어와 업데이트가 가능하므로 신뢰할 수 있는 내부망에서 사용하며 외부 포트 개방은 전제로 하지 않습니다. Wi-Fi 접속 비밀번호와 Linux companion 토큰은 유지합니다. OTA 체크섬·요청 헤더·릴레이 보호 검사는 유지합니다. 기존 PIN 값은 마이그레이션 호환을 위해 저장돼 있어도 사용하지 않습니다.
