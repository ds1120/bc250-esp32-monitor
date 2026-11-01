# 설정과 자동 수집

## 저장 위치와 우선순위

- SD `/bc250/settings.json`: PC 대상 주소·포트, PC 이름·Bluetooth 주소, 화면/갱신/패드 동작 설정.
- ESP32 NVS: Wi-Fi SSID/비밀번호, 관리자 PIN, 인증 토큰, 기존 설정과 터치 보정. 비밀정보를 SD로 내보내지 않습니다.
- PC `/etc/bc250-esp-bluetooth.json`: 선택한 안정적인 USB 경로와 선택적 구형 어댑터 주소. 설치 시 기존 파일을 우선 보존합니다.
- PC `/etc/bc250-power-manager/token`: HTTP 인증 토큰, root 전용.
- 최초 설정 파일 `~/.config/bc250-monitor/setup.json`: 최초 설치에 쓰는 개인 파일(0600). Git에 올리지 않습니다.

부팅 때 SD의 저장값을 먼저 사용합니다. 예전 SD에 새 섹션이 없으면 기존 NVS 값을 유지합니다. USB로 받은 PC 정보는 비어 있는 값만 채우고 SD에 원자적으로 저장합니다. 기존 값을 자동 변경하지 않으므로 PC/네트워크를 바꿨다면 대상 항목을 명시적으로 수정하거나 비워야 합니다.

`network.agent_host`를 비우면 다음 USB 정보 수신 때 PC 주소를 채웁니다. 기본 `agent_port`는 프로토콜의 9091이며 자동 수집할 때 companion의 실제 포트를 전달합니다. 정보 수신은 약 30초 간격이므로 처음 연결 후 잠시 기다려야 할 수 있습니다. SD카드가 없으면 기존 NVS 기반 동작을 유지하지만 PC 이름 등 SD 전용 정보는 다음 부팅까지 보존되지 않습니다.

## 자동으로 읽는 정보

| 값 | 출처 | 모호할 때 |
|---|---|---|
| USB-UART 경로 | udev VID와 by-path/by-id | 후보가 하나가 아니면 `--device` 필요 |
| PC IPv4 | Linux 기본 경로와 해당 인터페이스 주소 | 동률 경로/여러 주소이면 저장하지 않음 |
| PC 이름 | Linux hostname | 형식이 맞지 않으면 저장하지 않음 |
| Bluetooth 주소 | 정확히 선택한 USB-UART의 BlueZ 어댑터 | 다른 동글을 대신 선택하지 않음 |
| 패드 목록 | 해당 BlueZ 어댑터의 실제 페어링 저장소 | 여러 어댑터 중 임의의 ‘패드가 많은 것’을 고르지 않음 |
| LAN 방화벽 대역 | 로컬 IPv4 인터페이스 | 여러 대역이면 `--lan-cidr` 필요 |

초기 Wi-Fi와 PIN은 사용자 입력이 필요합니다. 토큰은 개인 설정에 없을 때만 임의 생성하며 같은 토큰을 ESP32와 companion에 설치합니다. 개인 IP·MAC·사용자 계정은 소스 기본값에 넣지 않습니다.

## 자동 수집하지 않는 정보

LCD 종류, 릴레이 극성·핀·접점, ATX 배선과 전원 절연은 자동 추정하지 않습니다. [HARDWARE.md](HARDWARE.md)에 확인된 기준과 미확인 영역을 기록합니다.

TCP 9091, 최종 종료 신호용 UDP 9092/9093, USB 921600 baud, RTC 재시작 티켓 주소는 양쪽이 공유하는 프로토콜/하드웨어 상수입니다. 개인 설정이 아니며 한쪽만 바꾸면 호환성이 깨집니다.

## 하드코딩 정리 내역

| 파일 | 정리한 항목 |
|---|---|
| `firmware/src/main.cpp` | 개인 PC IP 삭제, 실제 PC/패드 MAC 기본값 삭제, ESP32의 고유 BT 주소 사용 |
| `host/install_esp_bluetooth.py` | 특정 BT MAC 기본값 삭제, USB 후보 자동 수집, 기존 설정 보존 |
| `host/agent.py` | 이름이 BC-250인 특정 PC라는 가정 없음, 정확한 USB Bluetooth 어댑터 우선 |
| `host/install_remote.py` | CachyOS/특정 LAN 고정 조건 제거, 실제 LAN 감지·명시 선택 |
| `install_bc250.py` | 개인 IP 삭제, 사용자 계정은 실행 계정 또는 명시값 |
| `wireless_update.py` | 실제 패널 주소를 명시하도록 변경 |
| `flash_panel.py` | 특정 보드 MAC·개인 백업 SHA 고정 검사 삭제, 명시한 보드·파티션·크기 검증 |
| `firmware/platformio.ini` | Windows COM8 고정 삭제 |
| `tests/` | 실제 PC/패드 주소 대신 시험용 주소 사용 |

## Bluetooth 재시작 복구

ESP32가 예정된 재부팅 후 보낸 HCI Hardware Error 이벤트만 복구 대상으로 감지합니다. 해당 USB에 연결된 어댑터만 다시 켜며, 최대 5회 재시도합니다. 사용자가 Bluetooth를 직접 끈 행위는 같은 이벤트를 만들지 않아 자동으로 켜지지 않습니다. 첫 펌웨어의 비정상 리셋이나 케이블 단선은 별도 처리 대상이며, 전원 제어를 추측하지 않습니다.
