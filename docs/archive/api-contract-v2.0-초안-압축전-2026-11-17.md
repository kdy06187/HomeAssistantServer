# HomeAssistant REST API 계약서

| 항목 | 값 |
|---|---|
| 계약 버전 | **v2.0** |
| 상태 | **초안(승인 전)** |
| 작성일 | 2026-11-17 |
| 기준 | `docs/02_설계.md`, ADR-003·005·006·007·009 (코드는 아직 v1.0) |
| 이전 버전 | `docs/api-contract.md` v1.0 (승인 2026-11-16, As-Is) |
| 원본 위치 | 승인되면 서버 저장소 `docs/api-contract.md`를 v2.0으로 교체하고 v1.0은 이력으로 남긴다. 앱은 버전이 붙은 사본을 둔다 |
| 변경 규칙 | 서버를 먼저 바꾸고 앱은 나중에 바꾼다. 바꿀 때마다 버전을 올리고 맨 아래 변경 이력에 적는다 |

> **v2.0의 원칙**: 서버 2차 릴리스(REQ-08~13·15·16)에서 **한 번에** 적용하는 목표(To-Be) 계약이다. 서버 1차 릴리스(내부 재설계)까지는 v1.0이 그대로 유효하다.
> 전체 변경은 §6 MAJOR 변경 목록, 전환 방법은 §7 앱 반영 순서를 본다.

---

## 1. 공통 규칙

| 항목 | 내용 | 근거 |
|---|---|---|
| Base URL | `https://<서버 기기명>.<tailnet 이름>.ts.net` (포트 없음, 443). 휴대폰에 Tailscale이 켜져 있어야 한다 | ADR-005 |
| 전송 | HTTPS. TLS는 서버 PC의 `tailscale serve`가 종단하고 서버 프로그램은 `127.0.0.1:8000`에서만 듣는다. 집 Wi-Fi에서 `http://<IP>:8000` 직접 접속은 **없다** | ADR-005, REQ-14, I-06 |
| 데이터 형식 | 요청·응답 JSON, UTF-8, `Content-Type: application/json` | |
| 인증 | 모든 경로에 `Authorization: Bearer <토큰>` (없는 경로 포함) | ADR-009 |
| 본문 크기 | 요청 본문 최대 16KiB. 넘으면 413 | REQ-15 |
| 키 표기 | lowerCamelCase. 단위 접미사 `_mW`(밀리와트), `_mWh`(밀리와트시)는 대소문자 구분을 위해 유지 | ADR-003, I-03 |
| 시각 | 응답의 시각은 ISO 8601 UTC(`2026-11-16T00:30:00Z`). 날짜 파라미터(`from`/`to`)는 한국 시간(+09:00) 날짜 `YYYY-MM-DD` | ADR-007 |
| 성공 판정 | **HTTP 상태코드만으로** 판정한다. 성공 본문에는 `result`/`status`/`message` 같은 처리 결과 키가 없다 | I-01, I-03 |
| 포트 8080 | DIY 기기 전용 TCP 포트(LAN). **앱은 쓰지 않는다** | |

### 1.1 오류 본문 (모든 오류 공통, 한 가지 모양)

```json
{ "error": { "code": "DEVICE_NOT_FOUND", "message": "등록되지 않은 기기입니다." } }
```

- 앱은 `error.code`로 분기하고 `error.message`는 표시용으로만 쓴다(문구는 바뀔 수 있다).
- 모르는 `code`가 오면 HTTP 상태코드 기준의 일반 오류로 처리한다(코드 추가는 MINOR 변경).

| HTTP | `code` | 언제 |
|---|---|---|
| 400 | `INVALID_REQUEST` | JSON 형식 오류, 필수 필드 누락, 허용 안 된 값(명령·프로토콜·파라미터) |
| 400 | `UNSUPPORTED_OPERATION` | 기기 종류가 지원하지 않는 요청(TCP_DIY 기기에 E6·E7) |
| 401 | `UNAUTHORIZED` | 토큰 없음·틀림. 헤더 `WWW-Authenticate: Bearer`. POST에서도 응답 본문이 정상 전달된다(N-03(신규) 수정) |
| 404 | `DEVICE_NOT_FOUND` | 없는 기기 또는 삭제(숨김)된 기기 |
| 404 | `JOB_NOT_FOUND` | 없는 커미셔닝 작업(서버 재시작 후 포함) |
| 404 | `NOT_FOUND` | 없는 경로 |
| 409 | `COMMAND_BLOCKED` | 기기 드라이버가 명령을 막음 |
| 409 | `COMMISSION_BUSY` | 다른 커미셔닝이 실행 중(동시 1개) |
| 413 | `PAYLOAD_TOO_LARGE` | 본문 16KiB 초과 (**확인 필요**: 이 경우 본문 모양이 다를 수 있다. 앱은 413이면 본문 없이도 처리) |
| 500 | `INTERNAL_ERROR` | 서버 내부 오류 |
| 502 | `DEVICE_UNREACHABLE` | 기기 또는 Matter 제어 도구가 응답하지 않거나 실패 |

### 1.2 공통 값

| 이름 | 값 | 설명 |
|---|---|---|
| `protocolType` | `"MATTER"`, `"TCP_DIY"` | 목록에 `"UNKNOWN"`이 올 수도 있다 |
| `state` | `"ON"`, `"OFF"`, `"OFFLINE"`, `"UNKNOWN"` | 기기 상태. 이 외의 문자열이 와도 앱은 "알 수 없음"으로 표시 |
| `command` | `"ON"`, `"OFF"` | |
| `driverTypes` 원소 | `1`=PC_DRIVER, `2`=TV_DRIVER, `3`=TIMER_DRIVER | `0`은 보내지 않는다 |
| `deviceId` | 문자열, **불투명 값**(앱은 형식을 해석하지 않는다). 현재 형식: Matter = 시리얼 번호, TCP_DIY = `TCP_` + Wi-Fi MAC 16진 12자(예 `TCP_A1B2C3D4E5F6`). IP가 바뀌어도 ID는 그대로 | ADR-006 |
| `ip` | TCP_DIY 기기의 마지막 접속 IP(문자열), Matter는 `null` | 표시용 |

---

## 2. 엔드포인트 요약

| # | 메서드 | 경로 | 하는 일 | v1.0 대비 |
|---|---|---|---|---|
| E1 | GET | `/api/devices` | 기기 목록 | 본문 키 변경 |
| E2 | POST | `/api/control` | 켜기/끄기 | 결과별 상태코드(200/202/오류) |
| E3 | POST | `/api/commission` | 등록 작업 시작 | **비동기**(202 + 작업 ID) |
| E8 | GET | `/api/commission/{jobId}` | 등록 작업 상태 | **신규** |
| E4 | POST | `/api/devices/delete` | 삭제(숨김) | 의미·응답 변경 |
| E5 | GET | `/api/devices/{deviceId}/state` | 상태 1건 | 없는 기기 404 |
| E6 | GET | `/api/devices/{deviceId}/energy` | 현재 전력·누적 | 키·상태코드 변경 |
| E7 | GET | `/api/devices/{deviceId}/energy/history` | 전력량 기록·집계 | 파라미터 추가, 키·시각 형식 변경 |

경로는 v1.0과 같다(E8만 추가). `DELETE /api/devices/{id}`로 바꾸는 안은 앱 변경만 늘어 채택하지 않았다.

---

## 3. 엔드포인트 상세

### E1. GET `/api/devices` — 기기 목록

**응답 200**
```json
{
  "totalCount": 2,
  "devices": [
    { "deviceId": "TCP_A1B2C3D4E5F6", "name": "거실 전등", "protocolType": "TCP_DIY", "state": "ON", "ip": "192.168.0.50" },
    { "deviceId": "SN12345", "name": "스마트 플러그", "protocolType": "MATTER", "state": "OFF", "ip": null }
  ]
}
```
- 삭제(숨김)된 기기는 나오지 않는다.
- `state`는 서버에 저장된 마지막 상태다(최신은 E5). 재시작 직후 `OFFLINE`이 `ON`/`OFF`로 보일 수 있다(헬스체크 60초).

### E2. POST `/api/control` — 켜기/끄기

**요청**: `{ "deviceId": "TCP_A1B2C3D4E5F6", "command": "OFF" }`

| 경우 | HTTP | 본문 |
|---|---|---|
| 즉시 실행 성공 | 200 | `{"deviceId": "...", "state": "OFF"}` |
| 지연 실행 수락(예: PC 드라이버의 OFF) | **202** | `{"deviceId": "...", "state": "ON", "pendingCommand": "OFF", "delaySeconds": 45}` |
| 같은 명령이 이미 대기 중 | 202 | 위와 같은 모양, `delaySeconds`는 남은 초. 종료 메시지를 다시 보내지 않는다 |
| 기기 드라이버가 막음 | 409 | `COMMAND_BLOCKED` |
| 기기 응답 없음·실패 | 502 | `DEVICE_UNREACHABLE` (서버는 상태를 `OFFLINE`으로 기록) |
| 없는 기기·숨김 기기 | 404 | `DEVICE_NOT_FOUND` |
| 허용 안 된 명령, JSON 오류 | 400 | `INVALID_REQUEST` |

- 202의 `state`는 응답 시점(바뀌기 전) 상태다. 앱은 `delaySeconds` 뒤 E5로 확인한다. 지연 실행의 결과는 응답으로 오지 않는다.
- 지연 OFF가 대기 중일 때 `ON`을 보내면 대기 중인 OFF는 취소된다.
- 서버가 종료되면 대기 중인 지연 명령은 실행되지 않고 취소된다(Q-06).

### E3. POST `/api/commission` — 등록 작업 시작 (비동기)

**요청** (v1.0과 같음)
```json
{ "protocolType": "MATTER", "deviceName": "스마트 플러그", "payload": "34970112332",
  "ssid": "MyWiFi", "password": "********", "driverTypes": [1] }
```
| 필드 | 필수 | 설명 |
|---|---|---|
| `protocolType` | 예 | `"MATTER"` / `"TCP_DIY"` |
| `deviceName` | 예 | 표시 이름 |
| `payload` | 예 | MATTER: 페어링 코드(수동 코드나 QR 내용). TCP_DIY: 기기가 접속할 서버의 **LAN IP**(Tailscale 주소 아님). 빈 문자열이면 서버 기본값 |
| `ssid`, `password` | 아니오 | 기기에 넣을 Wi-Fi 정보. 빠지면 `""` |
| `driverTypes` | 아니오 | 정수 배열 |

| 경우 | HTTP | 본문 |
|---|---|---|
| 작업 접수 | **202** | `{"jobId": "c-20261117-0001", "state": "PENDING"}` |
| 다른 등록이 실행 중 | 409 | `COMMISSION_BUSY` |
| 필드 오류·알 수 없는 protocolType | 400 | `INVALID_REQUEST` |

- 응답은 즉시 온다(v1.0의 120초 타임아웃 불필요). 결과는 E8로 확인한다.
- `jobId`는 불투명 문자열이다.
- 요청 본문에 비밀번호가 있으므로 앱은 본문을 로그에 남기지 않는다. 서버도 남기지 않고, 작업 상태에도 넣지 않는다.

### E8. GET `/api/commission/{jobId}` — 등록 작업 상태 (신규)

**응답 200**
```json
{ "jobId": "c-20261117-0001", "state": "SUCCEEDED", "deviceId": "SN12345", "restored": false, "error": null }
```
| 필드 | 설명 |
|---|---|
| `state` | `PENDING` → `RUNNING` → `SUCCEEDED` 또는 `FAILED` |
| `deviceId` | 성공 시 등록된 기기 ID, 그 외 `null` |
| `restored` | 90일 안에 삭제했던 같은 기기를 다시 등록해 **이전 기록을 이어받았으면** `true` |
| `error` | 실패 시 `{"code": "...", "message": "..."}`, 그 외 `null`. `code`: `COMMISSION_FAILED`(BLE 기기 못 찾음·페어링 실패 등), `COMMISSION_TIMEOUT`, `CANCELLED`(서버 종료) |

| 오류 | HTTP |
|---|---|
| 없는 작업(서버 재시작으로 사라진 경우 포함) | 404 `JOB_NOT_FOUND` |

- 권장 폴링: 2초 간격, `SUCCEEDED`/`FAILED`까지. 서버 쪽 작업 제한 시간: TCP_DIY 약 25초(BLE 5초 + 접속 대기 15초 + 여유), MATTER 120초(**확인 필요**: 실측 후 조정). 앱은 180초가 지나도 끝나지 않으면 E1을 다시 불러 확인한다.
- 작업 상태는 서버 메모리에만 있다. 끝난 작업은 1시간 동안 조회할 수 있다.
- 404를 받으면 E1로 등록 여부를 확인한다.

### E4. POST `/api/devices/delete` — 삭제(숨김)

**요청**: `{ "deviceId": "SN12345" }`

**응답 200**
```json
{ "deviceId": "SN12345", "deletedAt": "2026-11-17T03:00:00Z", "restorableUntil": "2027-02-15T03:00:00Z", "unpaired": true }
```
| 필드 | 설명 |
|---|---|
| `deletedAt` | 숨긴 시각(UTC) |
| `restorableUntil` | 이 시각 전까지 같은 기기를 다시 등록(E3)하면 기록(누적 전력량·이력)을 이어받는다. 이후에는 기록까지 완전히 지워진다 |
| `unpaired` | 기기 연결 해제 성공 여부. Matter: 페어링 해제, TCP_DIY: 초기화 메시지 전송. `false`여도 삭제(숨김)는 완료된 것이다 |

| 오류 | HTTP |
|---|---|
| 없는 기기·이미 숨긴 기기 | 404 `DEVICE_NOT_FOUND` |
| JSON 오류 | 400 `INVALID_REQUEST` |
| 서버 내부 오류(삭제 안 됨) | 500 `INTERNAL_ERROR` |

- 숨긴 기기는 E1에 안 나오고 E2·E5·E6·E7은 404다. "되살리기" API는 없다. 복구는 다시 등록(E3)으로만 한다.
- Matter 기기는 페어링이 해제되므로 다시 쓰려면 다시 페어링해야 한다(같은 시리얼이면 `restored: true`).

### E5. GET `/api/devices/{deviceId}/state` — 상태 조회

쿼리 `isManualRefresh`(선택, `true`/그 외) — v1.0과 같다.

**응답 200**: `{ "deviceId": "SN12345", "state": "ON" }`

| 오류 | HTTP |
|---|---|
| 없는 기기·숨김 기기 | **404** `DEVICE_NOT_FOUND` (v1.0은 200 + `UNKNOWN`) |

- 기기가 응답하지 않으면 오류가 아니라 200 + `"state": "OFFLINE"`이다.

### E6. GET `/api/devices/{deviceId}/energy` — 현재 전력

**응답 200**
```json
{ "deviceId": "SN12345", "state": "ON", "activePower_mW": 15230, "totalEnergy_mWh": 1048576 }
```
| 필드 | 형식 | 단위 |
|---|---|---|
| `state` | 문자열 | §1.2 (v1.0의 `device_status`) |
| `activePower_mW` | 정수 | 밀리와트 |
| `totalEnergy_mWh` | 정수(64비트) | 밀리와트시, 서버가 보정한 누적값 |

| 오류 | HTTP |
|---|---|
| 없는 기기·숨김 기기 | 404 `DEVICE_NOT_FOUND` |
| TCP_DIY 기기 | 400 `UNSUPPORTED_OPERATION` |
| 측정값을 읽지 못함 | 502 `DEVICE_UNREACHABLE` (서버 통계는 바뀌지 않는다, REQ-03) |

### E7. GET `/api/devices/{deviceId}/energy/history` — 전력량 기록·집계

**쿼리 파라미터** (모두 선택)
| 이름 | 값 | 기본 | 설명 |
|---|---|---|---|
| `from` | `YYYY-MM-DD` (+09:00 날짜) | 없음 | 시작일(포함) |
| `to` | `YYYY-MM-DD` (+09:00 날짜) | 없음 | 끝일(포함) |
| `period` | `day` / `week` / `month` | 없음 | 있으면 **집계 모드**. 주는 월요일 시작 |
| `limit` | 정수 1 이상 | 50 | 원본 모드의 최대 건수. 1000을 넘으면 1000으로 제한. 집계 모드에서는 무시 |

규칙: `period`가 있으면 `from`·`to` 필수. `from ≤ to`. 범위는 최대 366일. 위반 시 400 `INVALID_REQUEST`. 기록은 90일만 보관되므로 그 이전 구간은 비어 있다.

**원본 모드**(`period` 없음): 범위 안(없으면 전체)의 **가장 최근 `limit`건을 오래된 것부터** 돌려준다.
```
GET /api/devices/SN12345/energy/history?limit=3
```
```json
{
  "deviceId": "SN12345",
  "state": "ON",
  "logs": [
    { "timestamp": "2026-11-16T00:20:00Z", "totalEnergy_mWh": 1040000 },
    { "timestamp": "2026-11-16T00:21:00Z", "totalEnergy_mWh": 1040150 },
    { "timestamp": "2026-11-16T00:22:00Z", "totalEnergy_mWh": 1040300 }
  ],
  "truncated": true
}
```
- `truncated`: 범위 안에 `limit`보다 많은 기록이 있어 오래된 쪽이 잘렸으면 `true`.
- `totalEnergy_mWh`는 그 시점 누적값이다.

**집계 모드**(`period` 있음): 범위 안의 모든 구간을 돌려준다(기록 없는 구간도 `sampleCount: 0`으로 포함).
```
GET /api/devices/SN12345/energy/history?from=2026-11-01&to=2026-11-17&period=week
```
```json
{
  "deviceId": "SN12345",
  "state": "ON",
  "period": "week",
  "utcOffset": "+09:00",
  "buckets": [
    { "start": "2026-11-01", "end": "2026-11-01", "energyUsed_mWh": 1200,  "sampleCount": 1440 },
    { "start": "2026-11-02", "end": "2026-11-08", "energyUsed_mWh": 9100,  "sampleCount": 10080 },
    { "start": "2026-11-09", "end": "2026-11-15", "energyUsed_mWh": 8800,  "sampleCount": 10080 },
    { "start": "2026-11-16", "end": "2026-11-17", "energyUsed_mWh": 2500,  "sampleCount": 2100 }
  ]
}
```
- `start`·`end`: 구간의 첫날·끝날(+09:00, 포함). 첫·마지막 구간은 `from`·`to`로 잘린다(예: 2026-11-01은 일요일이라 첫 주 구간이 하루).
- `energyUsed_mWh`: 그 구간 사용량 = 이웃한 기록 차이 중 양수의 합(뒤 기록이 속한 구간에 넣음). 음수 차이는 0. 구간 시작 직전 기록과의 차이도 포함한다.
- `period=day`면 구간이 하루, `month`면 달력 월.

| 오류 | HTTP |
|---|---|
| 없는 기기·숨김 기기 | **404** `DEVICE_NOT_FOUND` (v1.0은 200 + `error`) |
| TCP_DIY 기기 | 400 `UNSUPPORTED_OPERATION` |
| 파라미터 오류 | 400 `INVALID_REQUEST` |

---

## 4. 앱 구현 가이드 (v2.0 기준)

1. 성공 판정은 **HTTP 상태코드**로만 한다(2xx = 성공). E2의 202는 "접수됨, 나중에 실행"이다.
2. 오류는 `error.code`로 분기한다. 401이면 토큰 입력 화면으로 보낸다.
3. `deviceId`·`jobId`는 해석하지 않는다(형식이 바뀔 수 있다).
4. 응답에 처음 보는 필드가 있으면 무시한다.
5. 등록(E3)은 202를 받은 뒤 E8을 폴링한다. 화면을 떠나도 작업은 서버에서 계속된다.
6. 시각은 UTC ISO 8601로 오므로 표시할 때 현지 시간으로 바꾼다. E7 집계의 날짜는 이미 +09:00 기준이다.

## 5. 범위 밖 (v2.0에 없음)

- 서버 푸시(WebSocket·SSE). 상태는 앱이 E1·E5를 다시 불러 확인한다.
- 기기 이름·드라이버 변경, 숨김 기기 목록·되살리기 API.
- 여러 사용자 계정, 토큰 여러 개, 로그인 시도 횟수 제한.
- 집 Wi-Fi 평문 HTTP 직접 접속(비상 시 서버 설정으로만 가능, §7).
- 90일보다 오래된 기록.

## 6. MAJOR 변경 목록 (v1.0 → v2.0)

| # | 대상 | v1.0 | v2.0 | 앱 영향 | 근거 |
|---|---|---|---|---|---|
| 1 | 접속 주소 | `http://<IP>:8000` | `https://<서버 기기명>.<tailnet>.ts.net` | 설정에 전체 URL 입력, 휴대폰 Tailscale 필요 | REQ-14, I-06, ADR-005 |
| 2 | 오류 본문 | `{"result":"error","message"}` / `{"error":"..."}` | `{"error":{"code","message"}}` 한 가지 | 오류 파서 교체 | REQ-11, I-03 |
| 3 | 상태코드 | 실패도 200, 없는 기기 200/400 혼재 | 400/401/404/409/413/500/502 규약 | HTTP 코드로 판정 | REQ-11, I-01, I-02, I-05 |
| 4 | 성공 본문의 처리 결과 키 | E1 `status`, E2 `message`, E3·E4 `result`·`message` | 없음 | 해당 키 의존 코드 제거 | I-03 |
| 5 | 키 이름 | `id`, `type`, `total_count`, `device_status` | `deviceId`, `protocolType`, `totalCount`, `state` | 모델 필드명 변경 | I-03 |
| 6 | E2 결과 | 200 + `message`로 성공/실패 | 200 성공 / 202 지연 수락(`pendingCommand`, `delaySeconds`) / 409 / 502 | 결과 처리 분기, 202면 `delaySeconds` 뒤 E5 | REQ-07, REQ-08, N-01(신규) |
| 7 | E3 | 동기, 성공까지 대기(120초+) | 202 + `jobId`, E8 폴링 | 등록 화면 흐름 변경 | REQ-10, I-04 |
| 8 | E8 | 없음 | `GET /api/commission/{jobId}` | 신규 호출 | REQ-10 |
| 9 | E4 | 행 삭제, 실패 시 400 | 숨김, 응답 `deletedAt`·`restorableUntil`·`unpaired`, 없으면 404 | 완료 문구("90일 안 재등록 시 복구") | REQ-13, Q-03c |
| 10 | 기기 ID | TCP_DIY `ARD_<IP>` | `TCP_<MAC12>`, `ip` 필드 별도 | ID를 해석하지 않으면 영향 없음. 저장해 둔 ID는 첫 갱신 때 바뀜 | Q-03c, ADR-006 |
| 11 | E5 | 없는 기기 200 + `UNKNOWN` | 404 | 404 처리 | I-02 |
| 12 | E6 | 400 `{"error"}`, `device_status` | 404/400/502, `deviceId`·`state` | 오류·필드 처리 | REQ-04, I-02 |
| 13 | E7 시각 형식 | `"YYYY-MM-DD HH:MM:SS"`(UTC) | `"YYYY-MM-DDTHH:MM:SSZ"` | 파서 변경 | I-03 |
| 14 | E7 응답 | `device_status`, `logs` | `deviceId`, `state`, `logs`, `truncated` / 집계 `buckets` | 필드 처리, 앱 자체 구간 계산은 `period`로 대체 가능 | REQ-12, APP-REQ |
| 15 | 본문 크기 | 제한 없음 | 16KiB, 413 | 정상 사용 시 없음 | REQ-15 |

MAJOR가 아닌 변경(참고):
| 대상 | 내용 | 근거 |
|---|---|---|
| E7 파라미터 | `from`/`to`/`period`/`limit` 추가(없으면 기존처럼 최근 50건) | APP-REQ, Q-03b·d (MINOR) |
| E7 순서 설명 | v1.0 문서의 "최신 기록부터"는 오기. 실제·v2.0 모두 **최근 N건을 오래된 순**으로 반환 | N-02(신규) (설명 정정, 동작 불변) |
| POST 401 | 앱이 "unexpected end of stream" 대신 401 본문을 받음 | N-03(신규) (버그 수정) |
| 401 본문 | `{"error":"..."}` → 오류 본문 공통 모양 | #2에 포함 |

## 7. 앱 반영 순서 (서버 먼저, 앱 나중)

**결정: 이전 응답 호환 기간(v1·v2 동시 제공)은 두지 않는다.** 사용자 1명·앱 1개라 `/api/v2/` 경로를 따로 두면 서버 핸들러가 두 벌이 되는 비용이 이득보다 크다. 대신 깨지는 시간을 줄이도록 **호환되는 준비를 먼저** 하고, 전환은 한 번에 한다.

| 단계 | 누가 | 내용 | 깨짐 |
|---|---|---|---|
| 0 | 사용자 | 이 초안 승인 → v2.0 확정, 앱에 사본(`api-contract-v2.0.md`) 배치 | 없음 |
| 1 | 서버 | 1차 릴리스 배포(내부 재설계, REST는 v1.0 그대로) | 없음 |
| 2 | 앱 | v1.0에서도 문제없는 준비: 설정에서 **전체 Base URL(https 포함)** 입력 지원, `deviceId`를 해석하지 않음(확인 필요 C-11), 모르는 필드 무시 | 없음 |
| 3 | 서버·사용자 | Tailscale 설치·`tailscale serve` 켜기. 이 동안 서버 바인딩은 `0.0.0.0` 유지 → 앱 설정을 `https://...ts.net`으로 바꿔 동작 확인 → 서버 `HAC_HTTP_BIND=127.0.0.1`로 재시작 | 없음(주소만 바뀜) |
| 4 | 서버·앱 | 서버 2차 릴리스(v2.0)를 가짜 드라이버 테스트로 완성. 앱은 v2.0 사본 기준으로 별도 브랜치에서 개발(가짜 서버·단위 테스트) | 없음(배포 전) |
| 5 | 사용자 | **전환일**: ① DB·`registered_devices.txt` 백업 ② 서버 v2.0 배포(시작 시 마이그레이션 자동, 자동 백업 파일도 생김) ③ 바로 앱 v2.0 설치 | ②~③ 사이 몇 분 동안 구 앱 일부 기능 오동작 |
| 6 | 사용자 | 실기 확인: TCP 기기 재접속 시 ID가 `TCP_<MAC>`으로 바뀌는지, 목록·제어·등록·삭제·기록 | — |
| 7 | 총괄 | 변경 이력의 서버·앱 반영 칸 갱신 | — |

**되돌리기**: 서버를 이전 커밋으로 되돌리고 5-①의 백업 DB를 복원한 뒤 이전 앱을 설치한다(마이그레이션은 한 방향이라 백업 복원이 필요). 3단계의 바인딩은 환경 변수만 되돌리면 된다.

---

## 변경 이력

| 버전 | 날짜 | 변경 | 관련 ID | 서버 반영 | 앱 반영 |
|---|---|---|---|---|---|
| v1.0 | 2026-11-16 | 최초 작성. 현재 코드 동작(As-Is) | — | 해당 없음 | ⬜ 사본 배치 |
| v2.0 (초안) | 2026-11-17 | HTTPS·Tailscale 주소 | REQ-14, I-06 | ⬜ | ⬜ |
| | | 오류 본문 단일화·상태코드 규약·성공 본문 처리 결과 키 제거·키 표기 통일 | REQ-11, I-01, I-02, I-03, I-05 | ⬜ | ⬜ |
| | | E2 성공/지연 수락/실패 구분, 지연 중복 방지·취소 | REQ-07, REQ-08, N-01(신규) | ⬜ | ⬜ |
| | | E3 비동기 + E8 신규 | REQ-10, I-04 | ⬜ | ⬜ |
| | | E4 숨김 삭제(90일 복구), 기기 ID `TCP_<MAC>` + `ip` | REQ-13, Q-03c | ⬜ | ⬜ |
| | | E6 존재·프로토콜 확인, 실패 시 502 | REQ-03, REQ-04 | ⬜ | ⬜ |
| | | E7 `from/to/period/limit`, 집계, 시각 ISO 8601, 순서 설명 정정 | REQ-12, APP-REQ, N-02(신규) | ⬜ | ⬜ |
| | | 인증을 본문 읽은 뒤로(POST 401 전달), 본문 16KiB 제한 | REQ-15, N-03(신규) | ⬜ | ⬜ |
