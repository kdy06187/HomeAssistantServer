# CLAUDE.md

DIY 기기(아두이노 등)는 자체 TCP/JSON 프로토콜로, 상용 기기는 Matter로 제어하는 C++17 IoT 서버입니다. 안드로이드 앱이 REST API(8000번 포트)로 서버를 사용하고, DIY 기기는 8080번 포트로 접속합니다.

## 빌드와 실행

사전 준비: Matter SDK 서브모듈을 `third_party/connectedhomeip/out/host`에 GN/Ninja로 빌드하고(기본 인자), SimpleBLE를 설치합니다.

```bash
cd build && cmake .. && make -j"$(nproc)"
```

```bash
cd build && ./HomeAssistantCore
```

- DB, 등록 파일, `chip-tool`, PAA 인증서를 상대 경로로 열기 때문에 반드시 `build/`에서 실행합니다.
- 테스트와 린트 설정은 없습니다.
- 새 소스 파일은 `add_executable` 목록에 추가합니다. `SOURCES` 변수는 사용되지 않습니다.

## 아키텍처

싱글톤 `DeviceManager`가 기기 맵을 소유하고 두 종류의 플러그인을 호출합니다.

- **프로토콜 드라이버**(`ProtocolDriver`: `TCPDriver`, `MatterController`): 하드웨어와 통신하며, `main.cpp`에서 `ProtocolType`마다 하나씩 등록합니다.
- **기기 드라이버**(`IDeviceDriver`: `PCDriver`): `Device`에 부착되는 훅입니다. 하드웨어 명령보다 먼저 실행되고, `DriverResult`로 명령을 막거나 지연시킵니다. `DriverManager`가 관리합니다.

### 명령 흐름

`POST /api/control` → `DeviceManager::executeCommand` → 기기 드라이버 → 프로토콜 드라이버의 `sendCommand` → 상태 갱신

- 지연이 요청되면 하드웨어 단계는 분리된 스레드에서 실행되어, 상태가 바뀌기 전에 HTTP 응답이 반환됩니다. `PCDriver`는 `OFF`를 받으면 종료 메시지를 보내고 전원 차단을 45초 지연시킵니다.
- `main.cpp`는 `PCDriver` 팩토리를 의도적으로 덮어써 항상 `TCPDriver`를 발송기로 사용합니다. 그래서 Matter 플러그에 연결된 PC도 종료 메시지는 TCP로 받습니다.

### 커미셔닝 흐름

`POST /api/commission` → `startCommissioning`이 기기 드라이버를 기기 **이름** 기준으로 예약 → 프로토콜 드라이버가 페어링 → `addDevice`가 예약된 드라이버를 부착하고 저장

- 페어링은 동기 방식이라 끝날 때까지 HTTP 요청이 열려 있습니다.
- `payload`는 `MATTER`에서는 설정 코드, `TCP_DIY`에서는 기기에 전달할 서버 IP입니다.

### 프로토콜 드라이버

- **`MatterController`**: `chip-tool`을 셸로 실행하고 출력 문자열로 결과를 판단하므로, 출력이 바뀌면 깨질 수 있습니다. SDK는 설정 코드 파싱에만 사용합니다.
- **`TCPDriver`**: 요청은 줄바꿈으로 구분된 JSON이고 응답은 1초간 기다립니다. 실패하면 소켓을 제거하므로 기기가 다시 접속할 때까지 오프라인입니다. 새 기기는 BLE로 Wi-Fi와 서버 정보를 받은 뒤 접속합니다.

### 기기 ID

- **TCP:** `ARD_<클라이언트 IP>`
- **Matter:** 시리얼 번호. `config/registered_devices.txt`(`serial,nodeId`)로 노드 ID로 변환하므로 이 파일이 DB와 일치해야 합니다.

### 데이터 저장

`DatabaseManager`(SQLite)가 시작 시 테이블을 생성합니다.

- `ProtocolType`과 `DriverType`은 정수로 저장되므로 기존 열거형 값의 번호나 순서를 바꾸면 안 됩니다.
- `is_active`(1/0)만 저장되어, 재시작하면 `OFFLINE`과 `UNKNOWN`은 사라지고 `ON` 또는 `OFF`로 로드됩니다.
- `device_energy_stats`는 플러그 카운터의 증가분을 누적하고, `state_history_logs`는 헬스 체크가 기록하는 전력량 시계열입니다.

### 스레드

TCP 수락 루프, HTTP 서버, 60초 주기 헬스 체크, 지연된 하드웨어 명령이 각각 별도 스레드에서 실행됩니다.

- `DeviceManager::mutex_`가 기기 맵을 보호합니다. `getDeviceActivePower`와 `getDeviceTotalEnergy`는 `chip-tool` 실행 중에도 락을 유지합니다.
- `svr.stop()`을 호출하는 곳이 없어 `main()`은 `HTTPServer::stop()`에서 멈춘 채 살아 있고, 그 뒤의 종료 코드는 실행되지 않습니다.

## 드라이버 추가

**기기 드라이버:** `DriverType`에 값을 추가하고, `IDeviceDriver`를 구현해 `PCDriver.cpp`처럼 `DriverRegistrar`로 등록합니다. 클라이언트는 커미셔닝 요청의 `driverTypes` 정수 배열로 선택합니다.

**프로토콜 드라이버:** `IMessageSender`도 구현해야 합니다. 구현하지 않으면 서버 시작 시 기기 드라이버가 다시 부착되지 않습니다.

## 저장소 규칙

- `.gitignore`가 없고 `build/`, `db/home_assistant.db`, `config/registered_devices.txt`가 추적되므로, 스테이징할 때는 파일을 명시적으로 지정하고 이 파일들은 의도한 경우가 아니면 제외합니다.
- 커밋 메시지는 `feat) ` 또는 `fix) ` 접두사 뒤에 한글 설명을 붙입니다.
- 코드 주석과 로그는 한글로 쓰고, 로그는 `[Component]` 태그로 시작합니다.
- `src/httplib.h`와 `src/json.hpp`는 외부 라이브러리이므로 수정하지 않습니다.
