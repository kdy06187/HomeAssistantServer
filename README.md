# home-assistant-core

커스텀 TCP 프로토콜로 아두이노 등 DIY 기기를, Matter(`chip-tool`)로 상용 기기를 제어하는 C++17 IoT 서버입니다. 안드로이드 앱이 REST API(v1.0)로 서버를 씁니다. 집 LAN 전용(평문 HTTP)입니다.

## 구조 (한눈에)
| 모듈 | 역할 |
|---|---|
| `main.cpp` | 부품을 만들어 연결(Composition Root) |
| `HTTPServer` | REST API(8000), 토큰 인증 |
| `DeviceManager` | 기기 목록·명령·전력 조회의 중심 |
| `TCPDriver` / `TcpServer` | DIY 기기 접속(8080), 기기 메시지 주고받기 |
| `MatterController` / `ChipToolRunner` | `chip-tool`을 셸 없이 실행해 Matter 기기 제어 |
| `DatabaseManager` | SQLite 저장(기기·전력·이력), 시작 때 마이그레이션·백업 |
| `PCDriver` | PC 기기 규칙(꺼진 PC 처리 등) |

자세한 설계·락 규칙: [docs/02_설계.md](docs/02_설계.md)

## 빌드·테스트
사전 준비(Matter SDK 서브모듈 빌드, SimpleBLE 설치)와 옵션은 [CLAUDE.md](CLAUDE.md)를 봅니다.
```bash
# 개발 빌드 + 테스트
cmake -S . -B build-dev && cmake --build build-dev -j"$(nproc)" && ctest --test-dir build-dev
# TSan(스레드 경합 검사) 빌드 + 테스트
cmake -S . -B build-tsan -DHAC_ENABLE_TSAN=ON -DHAC_BUILD_SERVER=OFF && cmake --build build-tsan -j"$(nproc)" && ctest --test-dir build-tsan
```
- 빌드 유형(`CMAKE_BUILD_TYPE`)은 비워 둡니다(이유는 CLAUDE.md).
- 서버 실행 파일 이름: `HomeAssistantCore`, 테스트: `hac_tests`.
- 검증 결과: [docs/08_검증-1차.md](docs/08_검증-1차.md)

## 실행
```bash
cd build && ./HomeAssistantCore
```
- DB·설정 파일을 `../` 상대 경로로 열므로 `build/`에서 실행합니다.
- 포트: 8000(REST, 앱), 8080(DIY 기기 TCP). 하나라도 못 열면 종료 코드 1로 끝납니다.
- 운영은 tmux 세션 `hac` 안에서 실행합니다. 순서·확인·롤백: [docs/06_배포.md](docs/06_배포.md)

## 설정·비밀 파일 (값은 문서·git에 적지 않음)
| 위치 | 내용 |
|---|---|
| `config/api_token` | 앱 인증 토큰(권한 600) |
| `config/chip-tool/` | chip-tool 저장소(페어링 정보, 권한 700) |
| `config/registered_devices.txt`, `db/home_assistant.db` | 기기 등록·DB (런타임 생성) |

모두 `.gitignore` 대상입니다. 토큰 교체 절차는 [docs/06_배포.md](docs/06_배포.md) 2.1절을 봅니다.

## 문서 목록 (`docs/`)
| 문서 | 내용 |
|---|---|
| [01_요구사항.md](docs/01_요구사항.md) | 요구사항(REQ-01~18, NFR-01~07) |
| [02_설계.md](docs/02_설계.md) | 구조·모듈·데이터·흐름 설계 |
| [03_설계결정/](docs/03_설계결정/) | 설계 결정 기록(ADR-001~011) |
| [04_진행현황.md](docs/04_진행현황.md) | 현재 단계, REQ별 상태, 다음 할 일 |
| [05_변경이력.md](docs/05_변경이력.md) | 단계·커밋별 변경 이력, 2차로 넘긴 항목 |
| [06_배포.md](docs/06_배포.md) | 배포 순서·롤백·상태 확인·배포 기록 |
| [07_실기시험-1차.md](docs/07_실기시험-1차.md) | 실제 기기 시험 절차와 결과 |
| [08_검증-1차.md](docs/08_검증-1차.md) | 1차 검증(테스트·TSan·보안 점검) |
| [api-contract.md](docs/api-contract.md) | REST 계약 v1.0(현재 운영) |
| [api-contract-v2.0-초안.md](docs/api-contract-v2.0-초안.md) | REST 계약 v2.0 초안(2차) |
