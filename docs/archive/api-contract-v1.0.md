> **보관본**: v2.0으로 대체(2026-11-28 승인, 서버 main `4e13d7d`, 전환일 태그 `v2.0-deploy-1`). 1차 운영(`v1.0-deploy-1`)의 근거 문서라 보관한다. 현재 계약은 `docs/api-contract.md`.

# HomeAssistant REST API 계약서

| 항목 | 값 |
|---|---|
| 버전 / 상태 | **v1.0** / **승인됨** (2026-11-16, 사용자) |
| 기준 코드 | `src/HTTPServer.cpp`, `src/ApiAuth.cpp`, `src/main.cpp` (main `d7aa2c6` 시점 작업 트리) |
| 원본 위치 | 서버 저장소 `docs/api-contract.md`. 앱은 버전이 붙은 사본을 둔다 |
| 변경 규칙 | 서버를 먼저, 앱은 나중에 바꾼다. 바꿀 때마다 버전을 올리고 맨 아래 변경 이력에 적는다 |

> **원칙**: 지금 서버가 실제로 하는 동작을 그대로 적는다(As-Is). REST를 바꾸면 v2.0으로 올린다.
> 고칠 점은 §6에 모았다. 앱은 §6의 동작까지 감안해 구현한다.

---

## 1. 공통 규칙

| 항목 | 내용 | 근거 |
|---|---|---|
| Base URL | `http://<서버 IP>:8000` | `main.cpp:57` |
| 전송 | HTTP 평문(TLS 없음). 집 LAN 안에서만 쓴다 | `HTTPServer.cpp:120` `listen("0.0.0.0", ...)` |
| 데이터 형식 | 요청·응답 모두 JSON, UTF-8. 응답 헤더 `Content-Type: application/json` | 각 핸들러의 `set_content` |
| 인증 | 모든 경로에 `Authorization: Bearer <토큰>` 헤더 필요 | `HTTPServer.cpp:47-57` |
| 포트 8080 | DIY 기기 전용 TCP 포트. **앱은 쓰지 않는다** | `main.cpp:37` |

### 1.1 인증

- 토큰은 서버 PC의 토큰 파일에 있다(권한 600, 토큰 32자 이상). 앱에는 설정 화면 등으로 넣는다.
- **토큰 값은 코드·문서·저장소에 쓰지 않는다.**
- 헤더가 없거나 토큰이 틀리면 아래처럼 응답한다. 인증 검사는 경로를 찾기 전에 하므로, 없는 경로여도 토큰이 틀리면 401이다.

```
HTTP/1.1 401 Unauthorized
WWW-Authenticate: Bearer
{"error": "인증이 필요합니다."}
```

### 1.2 공통 값

| 이름 | 값 | 설명 |
|---|---|---|
| `protocolType` / `type` | `"MATTER"`, `"TCP_DIY"` | 기기 프로토콜. 목록 응답에는 `"UNKNOWN"`이 올 수도 있다 |
| 기기 상태 `state` | `"ON"`, `"OFF"`, `"OFFLINE"`, `"UNKNOWN"` | 이 외의 문자열이 와도 앱은 죽지 말고 "알 수 없음"으로 표시한다 |
| 제어 명령 `command` | `"ON"`, `"OFF"` | 그 외는 거부 (`HTTPServer.cpp:13-15`) |
| `driverTypes` 원소 | `1`=PC_DRIVER, `2`=TV_DRIVER, `3`=TIMER_DRIVER | `include/DriverType.hpp`. `0`(NONE)은 보내지 않는다 |
| `deviceId` | 문자열. Matter는 시리얼 번호, TCP_DIY는 `ARD_<기기 IP>`(예: `ARD_192.168.0.50`). IP가 바뀌면 ID도 바뀐다 | URL 경로에 `/` 불가 |

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

### E1. GET `/api/devices`

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
- `state`는 서버에 저장된 마지막 상태다(기기에 새로 물은 값이 아님). 최신 값은 E5.
- 서버 재시작 직후에는 DB에 ON/OFF만 있어 `OFFLINE`이 `ON` 또는 `OFF`로 보일 수 있다. 헬스 체크는 60초마다.

### E2. POST `/api/control`

**요청**: `{ "deviceId": "ARD_192.168.0.50", "command": "ON" }`

| 경우 | HTTP | 본문 |
|---|---|---|
| 명령 성공 | 200 | `{"status": "ON", "message": "명령 성공!"}` |
| 명령 실패(기기 응답 없음, 미등록 ID 등) | **200** | `{"status": "<현재 상태 또는 UNKNOWN>", "message": "명령 실패!"}` |
| 허용되지 않은 명령 | 400 | `{"result": "error", "message": "허용되지 않은 명령입니다."}` |
| JSON 형식 오류, 필드 누락 | 400 | `{"result": "error", "message": "잘못된 JSON 형식입니다."}` |

- 여기서 `status`는 **기기 상태**다(E1의 `status`와 뜻이 다름).
- 실패해도 200이므로 성공 여부는 `message`로 구분한다(§6 I-01).
- PC 드라이버가 붙은 기기는 명령이 막히거나 늦게 실행될 수 있다. 예: `OFF`는 PC에 종료 메시지를 보내고 전원 차단은 45초 뒤. 응답은 상태가 바뀌기 전에 오므로 앱은 잠시 뒤 E5로 다시 확인한다.

### E3. POST `/api/commission`

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
| `payload` | 예 | MATTER: 페어링 코드(수동 코드나 QR 내용). TCP_DIY: 기기가 접속할 서버 IP(빈 문자열이면 서버 기본 IP) |
| `ssid`, `password` | 아니오 | 기기에 넣을 Wi-Fi 정보. 빠지면 `""` |
| `driverTypes` | 아니오 | 정수 배열(§1.2). 빠지면 드라이버를 붙이지 않음 |

| 경우 | HTTP | 본문 |
|---|---|---|
| 성공 | 200 | `{"result": "success", "message": "기기 등록 성공!"}` |
| 등록 실패 | 400 | `{"result": "error", "message": "기기 등록 실패!"}` |
| 알 수 없는 protocolType | 400 | `{"result": "error", "message": "알 수 없는 프로토콜 타입입니다."}` |
| JSON 오류, 필수 필드 누락 | 400 | `{"result": "error", "message": "잘못된 JSON 형식입니다."}` |

- **오래 걸리는 동기 요청이다.** TCP_DIY는 BLE 스캔 5초에 연결 대기가 더해지고, Matter는 페어링이 끝날 때까지 응답이 없다. 앱 읽기 타임아웃은 **120초 이상**. (서버 쪽 상한 없음, §6 I-04)
- 성공 응답에 새 기기의 `deviceId`가 없다. 등록 후 E1로 목록을 갱신한다.
- 요청에 Wi-Fi 비밀번호가 있으므로 앱은 이 요청 본문을 로그에 남기지 않는다.

### E4. POST `/api/devices/delete`

**요청**: `{ "deviceId": "SN12345" }`

| 경우 | HTTP | 본문 |
|---|---|---|
| 성공 | 200 | `{"result": "success", "message": "기기 삭제 성공!"}` |
| 없는 기기 또는 해제 실패 | 400 | `{"result": "error", "message": "기기 삭제 실패 또는 기기 없음"}` |
| JSON 오류 | 400 | `{"result": "error", "message": "잘못된 JSON 형식입니다."}` |

### E5. GET `/api/devices/{deviceId}/state`

| 쿼리 | 필수 | 값 | 설명 |
|---|---|---|---|
| `isManualRefresh` | 아니오 | `true` / 그 외 | `true`면 사용자가 직접 새로고침한 것으로 보고 기기에 바로 물어본다. 그 외·누락은 `false` |

**응답 200**: `{ "deviceId": "SN12345", "state": "ON" }`
- 없는 기기 ID여도 **200**에 `"state": "UNKNOWN"`(§6 I-02).

### E6. GET `/api/devices/{deviceId}/energy`

**응답 200**
```json
{ "device_status": "ON", "activePower_mW": 15230, "totalEnergy_mWh": 1048576 }
```
| 필드 | 형식 | 단위 |
|---|---|---|
| `device_status` | 문자열 | §1.2 상태 값 |
| `activePower_mW` | 정수 | 밀리와트 (W는 1000으로 나눔) |
| `totalEnergy_mWh` | 정수(64비트로 받을 것) | 밀리와트시. DB 값으로 보정한 누적값 |

| 오류 | HTTP | 본문 |
|---|---|---|
| 없는 기기 | 400 | `{"error": "등록되지 않은 기기입니다."}` |
| 서버 내부 오류 | 400 | `{"error": "서버 내부 데이터 처리 오류입니다."}` |

### E7. GET `/api/devices/{deviceId}/energy/history`

**응답 200**
```json
{
  "device_status": "ON",
  "logs": [
    { "timestamp": "2026-11-16 09:20:00", "totalEnergy_mWh": 1040000 },
    { "timestamp": "2026-11-16 09:30:00", "totalEnergy_mWh": 1048576 }
  ]
}
```
- 최신 **최대 50건**(`DeviceManager.hpp:52` 기본값)이 **오래된 것부터 정렬(오름차순)**로 온다. 기간·개수는 지정할 수 없다.
- `timestamp`는 SQLite `CURRENT_TIMESTAMP` 형식 `"YYYY-MM-DD HH:MM:SS"`, **UTC**. 앱에서 현지 시간으로 바꿔 표시한다.
- `totalEnergy_mWh`는 그 시점의 **누적값**. 구간 사용량은 이웃한 두 값의 차이로 계산한다.
- 기록이 없으면 `logs`는 빈 배열.
- **오류**: 없는 기기거나 서버 내부 오류면 **200**에 `{"error": "..."}`(§6 I-02).

---

## 4. 앱 구현 가이드 (v1.0 기준)

1. 성공 판정은 **HTTP 코드와 본문을 함께** 본다. 2xx이면서 본문에 `error` 키가 없고 `result`도 `"error"`가 아니면 성공. E2는 추가로 `message == "명령 성공!"`이어야 성공.
2. 401이면 토큰 재입력 화면으로 보낸다.
3. 오류 메시지는 `message` 또는 `error` 중 있는 키에서 읽는다.
4. 처음 보는 응답 필드는 무시한다(새 필드가 추가되어도 깨지지 않게).

## 5. 범위 밖 (v1.0에 없음)

- 서버가 앱에 먼저 알려 주는 기능(푸시, WebSocket, SSE). 상태는 앱이 E1·E5를 다시 불러 확인한다.
- 기기 이름 변경, 드라이버 변경 API
- HTTPS, 여러 사용자 계정

## 6. 알려진 비일관성 (v2.0 설계 때 정리할 후보, 지금 동작은 그대로)

| ID | 내용 | 영향 |
|---|---|---|
| I-01 | E2는 명령이 실패해도 200을 준다 | 앱이 HTTP 코드만 보면 실패를 놓친다 |
| I-02 | 없는 기기를 E5는 200 + `UNKNOWN`, E6은 400, E7은 200 + `error`로 응답한다. 404를 쓰는 곳이 없다 | 오류 처리를 경로마다 따로 짜야 한다 |
| I-03 | 오류 본문 모양이 `{"result","message"}`와 `{"error"}` 두 가지다. `status` 키도 E1에서는 처리 결과, E2에서는 기기 상태를 뜻한다. 키 표기도 camelCase와 snake_case가 섞여 있다 | 파싱 코드가 복잡해진다 |
| I-04 | E3이 동기 방식이고 서버 쪽 시간 제한이 없다 | 앱이 오래 기다려야 하고, 타임아웃이 나면 등록 결과를 알 수 없다 |
| I-05 | E2·E4의 오류 상태 코드를 본문에 `"error"` 문자열이 있는지로 정한다 | 서버 내부 문제. 앱에는 영향 없음 |
| I-06 | 평문 HTTP라 토큰이 LAN에서 그대로 보인다 | 외부 접속 전에 TLS가 필요하다(진행현황의 외부 접속 계획과 연결) |

---

## 변경 이력

| 버전 | 날짜 | 변경 | 서버 반영 | 앱 반영 |
|---|---|---|---|---|
| v1.0 | 2026-11-16 | 최초 작성. 현재 코드 동작(As-Is)을 문서화 | 해당 없음(코드 그대로) | ⬜ 사본 배치 필요 |
| v1.0 | 2026-11-18 | 형식 정리(내용 변경 없음) | 해당 없음 | 해당 없음 |
| v1.0 | 2026-11-18 | E7 순서 설명 정정(N-02, 동작 변경 없음) | 해당 없음 | 해당 없음 |
