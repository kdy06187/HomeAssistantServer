# CLAUDE.md

DIY 기기(아두이노 등)는 자체 TCP/JSON 프로토콜로, 상용 기기는 Matter(chip-tool)로 제어하는 C++17 IoT 서버입니다. 안드로이드 앱이 REST API(8000번)로 서버를 쓰고, DIY 기기는 8080번으로 접속합니다. 집 LAN 전용(평문 HTTP)입니다.

## 빌드·테스트·실행

사전 준비: Matter SDK 서브모듈을 `third_party/connectedhomeip/out/host`에 GN/Ninja로 빌드(기본 인자), SimpleBLE 설치.

- CMake 옵션: `HAC_BUILD_TESTS`(기본 ON), `HAC_BUILD_SERVER`(기본 ON, Matter·BLE 필요), `HAC_ENABLE_TSAN`(기본 OFF, `HAC_BUILD_SERVER=OFF`와 함께).
- 빌드 유형은 비워 둡니다. 최적화(`RelWithDebInfo` 등)를 켜면 SDK의 시험용 정적 라이브러리가 링크에 끌려와 실패합니다.

```bash
cmake -S . -B build-dev && cmake --build build-dev -j"$(nproc)" && ctest --test-dir build-dev
```
```bash
cmake -S . -B build-tsan -DHAC_ENABLE_TSAN=ON -DHAC_BUILD_SERVER=OFF && cmake --build build-tsan -j"$(nproc)" && ctest --test-dir build-tsan
```
```bash
cd build && ./HomeAssistantCore
```
- DB·등록 파일·토큰·chip-tool·PAA 인증서를 `../` 상대 경로로 열므로 `build/`(또는 같은 깊이의 폴더)에서 실행합니다.
- 시작 때 토큰·DB·8080·8000 중 하나라도 실패하면 종료 코드 1로 끝납니다. `umask(077)`로 서버와 chip-tool이 만드는 파일은 소유자만 읽습니다.
- 새 소스: CHIP·SimpleBLE가 필요 없으면 `hac_core`, 필요하면 `hac_drivers`(OBJECT 라이브러리)에 추가합니다. 테스트는 `tests/unit`·`tests/integration`, 가짜는 `tests/fakes`(FakeProtocolDriver, FakeChipToolRunner, FakeClock, MockMessageSender).

## 아키텍처

`main.cpp`가 Composition Root입니다. DatabaseManager·DriverManager·SystemClock → DeviceManager를 만들고 드라이버를 주입합니다(싱글톤 없음).

- **프로토콜 드라이버**(`ProtocolDriver`: `TCPDriver`, `MatterController`): 하드웨어 통신. `commission`은 `Result<CommissionOutcome>`을 돌려주고 등록(DB 먼저 → 메모리)은 DeviceManager가 합니다. `readEnergy`는 Matter만 구현.
- **기기 드라이버**(`IDeviceDriver`: `PCDriver`): 하드웨어 명령 전에 실행되어 `DriverResult`로 막거나 지연. 팩토리는 main에서만 `DriverManager`에 등록합니다(PC 종료 메시지는 항상 TCPDriver의 `broadcast`).
- **결과 형식**: `Result<T>`·`Status`(`include/Result.hpp`), 명령은 `CommandResult`(`include/Command.hpp`). 기기 출력 해석은 순수 함수 `ChipToolOutputParser`·`TcpProtocol`.

### 명령·커미셔닝
- `POST /api/control` → `DeviceManager::executeCommand` → 기기 드라이버 → 프로토콜 드라이버 → 상태 갱신. 지연(PC OFF 45초)은 분리 스레드(2차에서 스케줄러로).
- `POST /api/commission`은 동기(페어링 최대 180초). `payload`는 MATTER 설정 코드 / TCP_DIY 서버 IP.

### MatterController
- chip-tool은 `ChipToolRunner`(`ProcessChipToolRunner`)로만 실행합니다: 셸 없이 fork+execv, 인자 배열(`str:` 접두사), 새 프로세스 그룹, 제한 시간(제어·상태·전력 5초, 시리얼·unpair 10초, 페어링 180초) 뒤 SIGTERM→SIGKILL, 출력 1MiB 상한, 비밀값 마스킹.
- 성공 판정은 **종료 코드 0 + 출력 성공 문구**(실기 2026-11-20에서 모든 명령이 성공 때 0 확인).
- 시리얼↔nodeId는 `config/registered_devices.txt`(`serial,nodeId`, 임시 파일+fsync+rename). chip-tool 저장소(fabric 키·KVS)는 모든 실행에 `--storage-directory ../config/chip-tool`(0700, 시작 때 생성)로 넘깁니다. `/tmp`는 tmpfs라 재부팅하면 지워지므로 쓰지 않습니다(SDK 고정 플랫폼 ini 3개만 `/tmp`에 남음).

### TCP
- `TcpServer`(수락·연결 맵·상한 32·재접속 교체) + `TcpConnection`(요청·응답 1쌍 락, 1초 마감, 한 줄 4KiB, 마지막 유효 JSON 객체 줄 사용, 무응답이면 연결 닫음). 기기 메시지 바이트는 기존과 같아야 합니다(펌웨어 호환).
- 기기 ID: `ARD_<클라이언트 IP>`(2차 12번에서 `TCP_<MAC>`).

### 데이터
- SQLite. 시작 때 `user_version<1`이면 `<db>.bak-v<버전>-<UTC>`(0600) 백업 후 v1 마이그레이션(한 트랜잭션). 값은 모두 바인딩.
- `ProtocolType`·`DriverType`은 정수로 저장되므로 기존 값의 번호·순서를 바꾸지 않습니다.
- `is_active`만 저장되어 재시작하면 OFFLINE·UNKNOWN은 ON/OFF로 로드됩니다.

### 스레드·락
- 락 안에서는 메모리만 다룹니다. 드라이버(chip-tool·소켓)와 DB 호출은 락 밖. 전력 조회는 기기별 전력 락. 순서 표는 `docs/02_설계.md` 3.4.
- HTTP 서버·TCP 수락·60초 헬스체크(PeriodicTasks)·지연 명령이 각각 스레드. 종료는 kill(SIGTERM 처리는 2차 11번).

## 드라이버 추가
- **기기 드라이버**: `DriverType`에 값 추가(번호 유지) → `IDeviceDriver` 구현 → main에서 `DriverManager::registerDriver`로 팩토리 등록.
- **프로토콜 드라이버**: `ProtocolDriver`와 `IMessageSender`(`sendTo`·`broadcast`)를 구현하고 main에서 `registerDriver`.

## 저장소 규칙
- `.gitignore`가 `build/`, `build-*/`, `db/home_assistant.db`, `config/registered_devices.txt`, `config/api_token`을 뺍니다. 그래도 스테이징은 파일을 지정해서 합니다(서브모듈 `third_party/connectedhomeip`가 수정 표시로 보일 수 있음 — 실행 권한 변경뿐, 커밋하지 않음).
- 커밋 메시지는 `feat) `/`fix) ` + 한글 설명, 끝에 관련 REQ-ID.
- 코드 주석·로그는 한글, 로그는 `[Component]` 태그로 시작. 비밀값(토큰·PIN·Wi-Fi 비밀번호)은 로그에 쓰지 않습니다.
- `src/httplib.h`, `src/json.hpp`는 외부 라이브러리라 수정하지 않습니다.
- 문서: 진행 현황 `docs/04_진행현황.md`, 설계 `docs/02_설계.md`, 계약 `docs/api-contract.md`(v1.0).
