# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 개요

DIY 기기(아두이노 등)는 자체 TCP/JSON 프로토콜로, 상용 기기는 Matter로 제어하는 C++17 IoT 서버입니다. 모바일(안드로이드) 클라이언트가 REST API를 통해 서버를 사용합니다.

## 명령어

사전 준비: `third_party/connectedhomeip`의 Matter SDK 서브모듈을 GN/Ninja로 `out/host`에 빌드해 두어야 합니다(기본 인자 사용). CMake가 이 디렉터리 아래의 모든 `.a` 파일을 링크하고, 서버는 여기서 나온 `chip-tool` 바이너리를 실행합니다. `find_package(simpleble)`가 찾을 수 있도록 SimpleBLE도 설치되어 있어야 합니다.

```bash
cd build && cmake .. && make -j"$(nproc)"
```

```bash
cd build && ./HomeAssistantCore
```

바이너리는 `build/`에서 실행해야 합니다. DB, 기기 등록 파일, `chip-tool`, PAA 인증서를 모두 작업 디렉터리 기준 상대 경로(`../db/...`, `../config/...`, `../third_party/...`, `../paa_certs/...`)로 열기 때문입니다.

테스트와 린트 설정은 없습니다. 새 소스 파일은 `CMakeLists.txt`의 `add_executable` 목록에 추가해야 합니다. 그 위의 `SOURCES` 변수는 사용되지 않습니다.

포트: REST API는 8000, 기기 TCP 서버는 8080입니다.

## 아키텍처

싱글톤인 `DeviceManager`가 기기 맵을 소유하며, 진입점과 두 종류의 플러그인 사이에 위치합니다.

- **프로토콜 드라이버** (`ProtocolDriver`: `TCPDriver`, `MatterController`)는 하드웨어와 통신합니다. `ProtocolType`마다 하나씩 있으며 `main.cpp`에서 등록합니다.
- **기기 드라이버** (`IDeviceDriver`: `PCDriver`)는 `Device`에 부착되는 기기별 훅입니다. 하드웨어 명령보다 먼저 실행되고, `DriverResult`를 반환해 명령을 막거나 지연시킬 수 있습니다. 팩토리 보관과 실행은 `DriverManager`가 담당합니다.

### 명령 흐름

`POST /api/control` → `DeviceManager::executeCommand` → 부착된 모든 기기 드라이버 → 프로토콜 드라이버의 `sendCommand`(기기 상태도 갱신) 순서로 진행됩니다. 드라이버가 지연을 요청하면 하드웨어 단계는 분리된(detached) 스레드에서 실행되므로, 하드웨어 상태가 바뀌기 전에 HTTP 응답이 먼저 반환됩니다. 예를 들어 `PCDriver`는 `OFF`를 받으면 TCP로 OS 종료 메시지를 보낸 뒤 전원 차단을 45초 지연시킵니다.

`main.cpp`는 `PCDriver`가 자체 등록한 팩토리를 의도적으로 덮어써서, 기기의 프로토콜과 상관없이 항상 `TCPDriver`를 `IMessageSender`로 사용하게 합니다. 따라서 Matter 플러그에 연결된 PC도 종료 메시지는 TCP로 받습니다.

### 커미셔닝 흐름

`POST /api/commission` → `DeviceManager::startCommissioning`이 요청된 기기 드라이버를 기기 **이름** 기준으로 예약 → 프로토콜 드라이버가 기기를 페어링(동기 방식이라 페어링이 끝날 때까지 HTTP 요청이 열려 있음) → 드라이버가 `addDevice`를 호출해 예약된 드라이버를 부착하고 기기를 저장합니다. `payload` 필드는 `MATTER`에서는 Matter 설정 코드, `TCP_DIY`에서는 기기에 전달할 서버 IP를 뜻합니다.

### 프로토콜 드라이버

- **`MatterController`는 제어할 때 Matter SDK를 런타임에 직접 쓰지 않습니다.** `chip-tool`을 셸로 실행하고 출력에서 문자열을 검색해 성공 여부를 판단하므로, 해당 문자열이나 `chip-tool` 동작이 바뀌면 깨질 수 있습니다. SDK는 설정 페이로드 파싱 용도로만 링크됩니다.
- **`TCPDriver`**는 8080 포트에서 기기 연결을 받아 기기 ID와 소켓의 맵을 유지합니다. 요청은 줄바꿈으로 구분된 JSON이고, 응답은 1초간 블로킹으로 기다립니다. 전송에 실패하거나 응답 시간이 초과되면 소켓을 닫고 맵에서 제거하므로, 기기가 다시 연결할 때까지 오프라인 상태가 됩니다. 새 기기는 BLE로 Wi-Fi와 서버 정보를 받은 뒤 TCP로 접속해 옵니다.

### 기기 ID

ID는 프로토콜마다 다르며 DB의 기본 키로 쓰입니다.

- **TCP:** `ARD_<클라이언트 IP>`, 연결 시 할당됩니다.
- **Matter:** 기기 시리얼 번호입니다. `chip-tool`에는 노드 ID가 필요하므로, `MatterController`가 `config/registered_devices.txt`(한 줄에 `serial,nodeId`)를 이용해 시리얼을 노드 ID로 변환합니다. 명령과 페어링 해제는 이 파일이 DB와 일치해야 정상 동작합니다.

### 데이터 저장

싱글톤인 `DatabaseManager`(SQLite)가 기기와 전력량 데이터를 저장하고, 시작 시 테이블을 생성합니다.

- `ProtocolType`과 `DriverType`은 정수로 저장되므로(`driverTypes`는 쉼표로 구분된 목록) 기존 열거형 값의 번호나 순서를 바꾸면 안 됩니다.
- `is_active`(1/0)만 저장되기 때문에 `OFFLINE`과 `UNKNOWN`은 재시작하면 사라지고, 기기는 `ON` 또는 `OFF`로 로드됩니다.
- `device_energy_stats`는 플러그 카운터의 증가분을 누적하므로 카운터가 초기화되어도 `real_total_mwh`가 유지됩니다. `state_history_logs`는 헬스 체크가 기록하는 시계열 데이터입니다.

### 스레드

TCP 수락 루프, HTTP 서버, 60초 주기 헬스 체크(모든 기기 상태 조회, Matter 전력량 기록), 지연된 하드웨어 명령이 각각 별도 스레드에서 실행됩니다. `DeviceManager::mutex_`가 기기 맵을 보호합니다. `executeCommand`와 `getDeviceState`는 I/O 전에 락을 해제하지만, `getDeviceActivePower`와 `getDeviceTotalEnergy`는 `chip-tool`이 실행되는 동안 락을 유지합니다.

`HTTPServer::stop()`은 `listen()`에서 블로킹된 스레드를 join하는데, `svr.stop()`을 호출하는 곳이 없습니다. 그래서 `main()`은 이 호출에서 멈춘 채로 살아 있고, 그 뒤의 종료 코드는 실행되지 않습니다.

## 기기 드라이버 추가

1. `DriverType`에 값을 추가합니다.
2. `IDeviceDriver`를 구현하고, `PCDriver.cpp`처럼 파일 내부의 `DriverRegistrar`로 등록합니다.
3. `.cpp` 파일을 `add_executable`에 추가합니다.

클라이언트는 커미셔닝 요청의 정수 배열 `driverTypes`로 드라이버를 선택합니다. 새 `ProtocolDriver`는 `IMessageSender`도 구현해야 합니다. `DeviceManager`가 `dynamic_cast`로 발송기를 찾기 때문에, 구현하지 않으면 로드 시 기기 드라이버가 다시 부착되지 않습니다.

## 저장소 규칙

- `.gitignore`가 없고 `build/`, `db/home_assistant.db`, `config/registered_devices.txt`가 추적되고 있어서, 빌드하거나 실행하면 수정된 파일로 표시됩니다. 스테이징할 때는 파일을 명시적으로 지정하고, 의도적으로 바꾼 경우가 아니면 이 파일들은 제외합니다.
- 커밋 메시지는 `feat) ` 또는 `fix) ` 접두사 뒤에 한글 설명을 붙입니다.
- 코드 주석과 로그 메시지는 한글로 쓰고, 로그는 `[Component]` 태그로 시작합니다.
- `src/httplib.h`와 `src/json.hpp`는 외부에서 가져온 단일 헤더 라이브러리이므로 수정하지 않습니다.
