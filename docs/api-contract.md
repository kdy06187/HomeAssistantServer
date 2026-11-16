# HomeAssistant REST API 계약서

| 항목 | 값 |
|---|---|
| 계약 버전 | **v1.0** |
| 상태 | **승인됨** (2026-11-16, 사용자) |
| 작성일 | 2026-11-16 |
| 기준 코드 | `src/HTTPServer.cpp`, `src/ApiAuth.cpp`, `src/main.cpp` (main `d7aa2c6` 시점 작업 트리) |
| 원본 위치 | 서버 저장소 `docs/api-contract.md` (이 파일). 앱은 버전이 붙은 사본을 둔다 |
| 변경 규칙 | 서버를 먼저 바꾸고 앱은 나중에 바꾼다. 바꿀 때마다 버전을 올리고 맨 아래 변경 이력에 적는다 |

> **v1.0의 원칙**: 이 문서는 **지금 서버가 실제로 하는 동작**을 그대로 적는다(As-Is).
> 2단계 설계에서 REST를 바꾸면(진행현황 문서의 "REST는 변경 가능" 결정) v2.0으로 올린다.
> 지금 동작 중 고칠 점은 §6 "알려진 비일관성"에 따로 모았다. 앱은 §6에 적힌 동작까지 감안해서 구현한다.

---

## 1. 공통 규칙

| 항목 | 내용 | 근거 |
|---|---|---|
| Base URL | `http://<서버 IP>:8000` | `main.cpp:57` |
| 전송 | HTTP 평문(TLS 없음). 집 LAN 안에서만 쓴다 | `HTTPServer.cpp:120` `listen("0.0.0.0", ...)` |
| 데이터 형식 | 요청과 응답 모두 JSON, UTF-8. 응답 헤더는 `Content-Type: application/json` | 각 핸들러의 `set_content` |
| 인증 | 모든 경로에 `Authorization: Bearer <토큰>` 헤더 필요 | `HTTPServer.cpp:47-57` |
| 포트 8080 | DIY 기기 전용 TCP 포트. **앱은 이 포트를 쓰지 않는다** | `main.cpp:37` |

### 1.1 인증

- 토큰은 서버 PC의 토큰 파일에 있다. 이 파일은 권한이 600이어야 하고 토큰은 32자 이상이어야 한다. 앱에는 설정 화면 등으로 넣는다.
- **토큰 값은 코드·문서·저장소에 쓰지 않는다.**
- 헤더가 없거나 토큰이 틀리면 아래처럼 응답한다. 인증 검사는 경로를 찾기 전에 하므로, 없는 경로로 요청해도 토큰이 틀리면 401이다.

```
HTTP/1.1 401 Unauthorized
WWW-Authenticate: Bearer
{"error": "인증이 필요합니다."}
```

### 1.2 공통 값

| 이름 | 값 | 설명 |
|---|---|---|
| `protocolType` / `type` | `"MATTER"`, `"TCP_DIY"` | 기기 프로토콜. 목록 응답에는 `"UNKNOWN"`이 올 수도 있다 |
| 기기 상태 `state` | `"ON"`, `"OFF"`, `"OFFLINE"`, `"UNKNOWN"` | 앱은 이 외의 문자열이 와도 죽지 말고 "알 수 없음"으로 표시한다 |
| 제어 명령 `command` | `"ON"`, `"OFF"` | 그 외 명령은 거부한다 (`HTTPServer.cpp:13-15`) |
| `driverTypes` 원소 | `1`=PC_DRIVER, `2`=TV_DRIVER, `3`=TIMER_DRIVER | `include/DriverType.hpp`. `0`(NONE)은 보내지 않는다 |
| `deviceId` | 문자열. Matter는 시리얼 번호, TCP_DIY는 `ARD_<기기 IP>`(예: `ARD_192.168.0.50`). IP가 바뀌면 ID도 바뀐다 | URL 경로에 넣을 때는 `/`를 쓸 수 없다 |

---

## 2. 엔드포인트 요약

| # | 메서드 | 경로 | 하는 일 |
|---|---|---|---|
| E1 | GET | `/api/devices` | 기기 목록 조회 |
| E2 | POST | `/api/control` | 기기 켜기/끄기 |
| E3 | POST | `/api/commission` | 새 기기 등록(커미셔닝) |
| E4 | POST | `/api/devices/delete` | 기기 삭제 |
| E5 | GET | `/api/devices/{deviceId}/state` | 기기 상태 1건 조회 |
| E6 | GET | `/api/devices/{deviceId}/energy` | 현재 전력과 누적 전력량 조회 |
| E7 | GET | `/api/devices/{deviceId}/energy/history` | 누적 전력량 기록 조회 |

---

## 3. 엔드포인트 상세

### E1. GET `/api/devices` — 기기 목록

**응답 200**
```json
{
  "status": "success",
  "total_count": 2,
  "devices": [
    { "id": "ARD_192.168.0.50", "name": "거실 전등", "type": "TCP_DIY", "state": "ON" },
    { "id": "SN12345",   "name": "스마트 플러그", "type": "MATTER", "state": "OFF" }
  ]
}
```
- `state`는 서버에 저장된 마지막 상태다. 기기에 새로 물어본 값이 아니다. 최신 값이 필요하면 E5를 쓴다.
- 서버가 재시작한 직후에는 DB에 ON/OFF만 저장되어 있어서 `OFFLINE`이 `ON` 또는 `OFF`로 보일 수 있다. 헬스 체크는 60초마다 돈다.

### E2. POST `/api/control` — 켜기/끄기

**요청**
```json
{ "deviceId": "ARD_192.168.0.50", "command": "ON" }
```

**응답**
| 경우 | HTTP | 본문 |
|---|---|---|
| 명령 성공 | 200 | `{"status": "ON", "message": "명령 성공!"}` |
| 명령 실패(기기 응답 없음, 미등록 ID 등) | **200** | `{"status": "<현재 상태 또는 UNKNOWN>", "message": "명령 실패!"}` |
| 허용되지 않은 명령 | 400 | `{"result": "error", "message": "허용되지 않은 명령입니다."}` |
| JSON 형식 오류, 필드 누락 | 400 | `{"result": "error", "message": "잘못된 JSON 형식입니다."}` |

- 이 경로의 `status`는 **기기 상태**다(E1의 `status`와 뜻이 다르다).
- 명령이 실패해도 200이 온다. 성공 여부는 `message`로 구분해야 한다(§6 I-01).
- PC 드라이버가 붙은 기기는 명령이 막히거나 늦게 실행될 수 있다. 예: `OFF`를 보내면 PC에 종료 메시지를 보내고 전원 차단은 45초 뒤에 한다. 이때 응답은 상태가 바뀌기 전에 오므로, 앱은 잠시 뒤 E5로 상태를 다시 확인한다.

### E3. POST `/api/commission` — 기기 등록

**요청**
```json
{
  "protocolType": "MATTER",
  "deviceName": "스마트 플러그",
  "payload": "34970112332",
  "ssid": "MyWiFi",
  "password": "********",
  "driverTypes": [1]
}
```

| 필드 | 필수 | 설명 |
|---|---|---|
| `protocolType` | 예 | `"MATTER"` 또는 `"TCP_DIY"` |
| `deviceName` | 예 | 앱에 표시할 이름 |
| `payload` | 예 | MATTER: 페어링 코드(수동 코드나 QR 내용). TCP_DIY: 기기가 접속할 서버 IP. 빈 문자열이면 서버의 기본 IP를 쓴다 |
| `ssid`, `password` | 아니오 | 기기에 넣을 Wi-Fi 정보. 빠지면 `""`로 처리한다 |
| `driverTypes` | 아니오 | 정수 배열(§1.2). 빠지면 기기 드라이버를 붙이지 않는다 |

**응답**
| 경우 | HTTP | 본문 |
|---|---|---|
| 성공 | 200 | `{"result": "success", "message": "기기 등록 성공!"}` |
| 등록 실패 | 400 | `{"result": "error", "message": "기기 등록 실패!"}` |
| 알 수 없는 protocolType | 400 | `{"result": "error", "message": "알 수 없는 프로토콜 타입입니다."}` |
| JSON 오류, 필수 필드 누락 | 400 | `{"result": "error", "message": "잘못된 JSON 형식입니다."}` |

- **오래 걸리는 동기 요청이다.** TCP_DIY는 BLE 스캔 5초에 연결 대기까지 더해지고, Matter도 페어링이 끝날 때까지 응답이 오지 않는다. 앱의 읽기 타임아웃은 **120초 이상**으로 잡는다. (서버 쪽 상한은 정해져 있지 않다. §6 I-04)
- 성공 응답에는 새 기기의 `deviceId`가 없다. 등록 후 E1을 다시 불러 목록을 갱신한다.
- 요청에 Wi-Fi 비밀번호가 들어 있으므로, 앱은 이 요청 본문을 로그에 남기지 않는다.

### E4. POST `/api/devices/delete` — 기기 삭제

**요청**
```json
{ "deviceId": "SN12345" }
```

**응답**
| 경우 | HTTP | 본문 |
|---|---|---|
| 성공 | 200 | `{"result": "success", "message": "기기 삭제 성공!"}` |
| 없는 기기 또는 해제 실패 | 400 | `{"result": "error", "message": "기기 삭제 실패 또는 기기 없음"}` |
| JSON 오류 | 400 | `{"result": "error", "message": "잘못된 JSON 형식입니다."}` |

### E5. GET `/api/devices/{deviceId}/state` — 상태 조회

**쿼리 파라미터**
| 이름 | 필수 | 값 | 설명 |
|---|---|---|---|
| `isManualRefresh` | 아니오 | `true` / 그 외 | `true`면 사용자가 직접 새로고침한 요청으로 보고 기기에 바로 물어본다. 그 외 값이거나 빠지면 `false`로 처리한다 |

**응답 200**
```json
{ "deviceId": "SN12345", "state": "ON" }
```
- 없는 기기 ID여도 **200**에 `"state": "UNKNOWN"`이 온다(§6 I-02).

### E6. GET `/api/devices/{deviceId}/energy` — 현재 전력

**응답 200**
```json
{ "device_status": "ON", "activePower_mW": 15230, "totalEnergy_mWh": 1048576 }
```
| 필드 | 형식 | 단위 |
|---|---|---|
| `device_status` | 문자열 | §1.2 상태 값 |
| `activePower_mW` | 정수 | 밀리와트 (W로 표시하려면 1000으로 나눈다) |
| `totalEnergy_mWh` | 정수(64비트로 받을 것) | 밀리와트시. DB 값으로 보정한 누적값 |

**오류**
| 경우 | HTTP | 본문 |
|---|---|---|
| 없는 기기 | 400 | `{"error": "등록되지 않은 기기입니다."}` |
| 서버 내부 오류 | 400 | `{"error": "서버 내부 데이터 처리 오류입니다."}` |

### E7. GET `/api/devices/{deviceId}/energy/history` — 전력량 기록

**응답 200**
```json
{
  "device_status": "ON",
  "logs": [
    { "timestamp": "2026-11-16 09:30:00", "totalEnergy_mWh": 1048576 },
    { "timestamp": "2026-11-16 09:20:00", "totalEnergy_mWh": 1040000 }
  ]
}
```
- 최신 기록부터 **최대 50건**이 온다(`DeviceManager.hpp:52`의 기본값). 기간이나 개수는 지정할 수 없다.
- `timestamp`는 SQLite `CURRENT_TIMESTAMP` 형식인 `"YYYY-MM-DD HH:MM:SS"`이고 **UTC**다. 앱에서 현지 시간으로 바꿔 표시한다.
- `totalEnergy_mWh`는 그 시점의 **누적값**이다. 구간 사용량은 앱에서 이웃한 두 값의 차이로 계산한다.
- 기록이 없으면 `logs`는 빈 배열이다.

**오류**: 없는 기기거나 서버 내부 오류면 **200**에 `{"error": "..."}`가 온다(§6 I-02).

---

## 4. 앱 구현 가이드 (v1.0 기준)

1. 성공 판정은 **HTTP 코드와 본문을 함께** 본다.
   - 2xx이면서 본문에 `error` 키가 없고 `result`도 `"error"`가 아니면 성공으로 본다.
   - E2는 거기에 더해 `message == "명령 성공!"`이어야 성공이다.
2. 401이 오면 토큰을 다시 입력하는 화면으로 보낸다.
3. 오류 메시지는 `message` 또는 `error` 중 있는 키에서 읽는다.
4. 응답에 처음 보는 필드가 있으면 무시한다(새 필드가 추가되어도 앱이 깨지지 않게).

## 5. 범위 밖 (v1.0에 없음)

- 서버가 앱에 먼저 알려 주는 기능(푸시, WebSocket, SSE). 상태는 앱이 E1이나 E5를 다시 불러서 확인한다.
- 기기 이름 변경, 드라이버 변경 API
- HTTPS, 여러 사용자 계정

## 6. 알려진 비일관성 (v2.0 설계 때 정리할 후보, 지금 동작은 그대로)

| ID | 내용 | 영향 |
|---|---|---|
| I-01 | E2는 명령이 실패해도 200을 준다 | 앱이 HTTP 코드만 보면 실패를 놓친다 |
| I-02 | 없는 기기를 E5는 200 + `UNKNOWN`으로, E6은 400으로, E7은 200 + `error`로 응답한다. 404를 쓰는 곳이 없다 | 오류 처리를 경로마다 따로 짜야 한다 |
| I-03 | 오류 본문 모양이 `{"result","message"}`와 `{"error"}` 두 가지다. `status` 키도 E1에서는 처리 결과, E2에서는 기기 상태를 뜻한다. 키 표기도 camelCase와 snake_case가 섞여 있다 | 파싱 코드가 복잡해진다 |
| I-04 | E3이 동기 방식이고 서버 쪽 시간 제한이 없다 | 앱이 오래 기다려야 하고, 타임아웃이 나면 등록 결과를 알 수 없다 |
| I-05 | E2·E4의 오류 상태 코드를 본문에 `"error"` 문자열이 있는지로 정한다 | 서버 내부 문제. 앱에는 영향 없음 |
| I-06 | 평문 HTTP라 토큰이 LAN에서 그대로 보인다 | 외부 접속 전에 TLS가 필요하다(진행현황의 외부 접속 계획과 연결) |

---

## 변경 이력

| 버전 | 날짜 | 변경 | 서버 반영 | 앱 반영 |
|---|---|---|---|---|
| v1.0 | 2026-11-16 | 최초 작성. 현재 코드 동작(As-Is)을 문서화 | 해당 없음(코드 그대로) | ⬜ 사본 배치 필요 |
