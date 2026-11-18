# HomeAssistant REST API 계약서 v2.0 (초안)

| 항목 | 값 |
|---|---|
| 상태 | **초안(승인 전)**. 2026-11-17 작성, 2026-11-18 수정(LAN 우선·입력 제한·고정 문구, 압축 전 사본 `docs/archive/`) |
| 기준 | `docs/02_설계.md`, ADR-003·006·007·009·011 (코드는 아직 v1.0) |
| 이전 버전 | `docs/api-contract.md` v1.0 (승인 2026-11-16, As-Is) |
| 원본·규칙 | 승인되면 서버 `docs/api-contract.md`를 v2.0으로 교체(v1.0은 이력). 앱은 버전 붙은 사본. 서버 먼저, 앱 나중. 바꿀 때마다 버전을 올리고 변경 이력에 적는다 |

**지금 상태 / 다음 할 일**
- 서버 2차 릴리스(REQ-08~13·15·16)에서 **한 번에** 적용할 목표 계약. 1차 릴리스까지는 v1.0 유효.
- 11-18: 접속은 v1.0처럼 **LAN `http://<서버 IP>:8000`**. HTTPS·외부 접속은 보류. 입력 제한(E3·E7)과 고정 오류 문구 추가.
- 다음: 사용자 승인 → 앱 팀에 사본 전달(§7 단계 0). 변경 전체는 §6, 전환은 §7.

## 1. 공통 규칙

| 항목 | 내용 | 근거 |
|---|---|---|
| Base URL | `http://<서버 LAN IP>:8000` (v1.0과 같음). 집 Wi-Fi 안에서만 | ADR-011 |
| 전송 | HTTP 평문(LAN). HTTPS·외부 접속은 **보류**(나중에 붙여도 경로·본문은 같고 주소만 바뀜) | REQ-14·I-06 보류 |
| 형식 | 요청·응답 JSON, UTF-8, `Content-Type: application/json` | |
| 인증 | 모든 경로(없는 경로 포함) `Authorization: Bearer <토큰>` | ADR-009 |
| 본문 크기 | 최대 16KiB, 넘으면 413 | REQ-15 |
| 키 표기 | lowerCamelCase. `_mW`(밀리와트)·`_mWh`(밀리와트시)는 유지 | ADR-003 |
| 시각 | 응답은 ISO 8601 UTC(`2026-11-16T00:30:00Z`). `from`/`to`는 +09:00 날짜 `YYYY-MM-DD` | ADR-007 |
| 성공 판정 | **HTTP 상태코드만**. 성공 본문에 `result`/`status`/`message` 없음 | I-01, I-03 |
| `{deviceId}` 경로 | `[A-Za-z0-9_-]{1,64}`만. 아니면 404 `NOT_FOUND` | S-15 |
| 포트 8080 | DIY 기기 전용 TCP. 앱은 안 씀 | |

### 1.1 오류 본문 (공통, 한 가지 모양)

`{ "error": { "code": "DEVICE_NOT_FOUND", "message": "등록되지 않은 기기입니다." } }`

- 앱은 `error.code`로 분기. `message`는 **코드별 고정 문구**(아래 표)이며 표시용. 서버 내부 상세(SQL·경로 등)는 담지 않는다(S-11).
- 모르는 `code`는 HTTP 상태 기준 일반 오류로 처리(코드 추가는 MINOR).

| HTTP | `code` | `message` | 언제 |
|---|---|---|---|
| 400 | `INVALID_REQUEST` | 요청 형식이 올바르지 않습니다. | JSON 오류, 필수 필드 누락, 허용 안 된 값, E3 입력 제한·E7 파라미터 위반 |
| 400 | `UNSUPPORTED_OPERATION` | 이 기기에서 지원하지 않는 요청입니다. | TCP_DIY 기기에 E6·E7 |
| 401 | `UNAUTHORIZED` | 인증에 실패했습니다. | 토큰 없음·틀림. `WWW-Authenticate: Bearer`. POST도 본문 정상 전달(N-03(신규) 수정) |
| 404 | `DEVICE_NOT_FOUND` | 등록되지 않은 기기입니다. | 없는·삭제(숨김)된 기기 |
| 404 | `JOB_NOT_FOUND` | 없는 등록 작업입니다. | 없는 작업(서버 재시작 후 포함) |
| 404 | `NOT_FOUND` | 없는 경로입니다. | 없는 경로, 형식이 틀린 `{deviceId}` |
| 409 | `COMMAND_BLOCKED` | 기기 드라이버가 명령을 막았습니다. | E2 |
| 409 | `COMMISSION_BUSY` | 다른 기기 등록이 진행 중입니다. | E3 (동시 1개) |
| 413 | `PAYLOAD_TOO_LARGE` | — | 16KiB 초과. **확인 필요**: 본문 모양이 다를 수 있음 → 앱은 본문 없이도 처리 |
| 500 | `INTERNAL_ERROR` | 서버 내부 오류입니다. | |
| 502 | `DEVICE_UNREACHABLE` | 기기가 응답하지 않습니다. | 기기 또는 Matter 제어 도구 실패 |

### 1.2 공통 값

| 이름 | 값 |
|---|---|
| `protocolType` | `"MATTER"`, `"TCP_DIY"` (목록에 `"UNKNOWN"` 가능) |
| `state` | `"ON"`, `"OFF"`, `"OFFLINE"`, `"UNKNOWN"`. 그 외는 "알 수 없음"으로 표시 |
| `command` | `"ON"`, `"OFF"` |
| `driverTypes` 원소 | `1`=PC_DRIVER, `2`=TV_DRIVER, `3`=TIMER_DRIVER (`0`은 안 보냄) |
| `deviceId` | **불투명 문자열**(해석 금지). 현재 Matter = 시리얼, TCP_DIY = `TCP_` + Wi-Fi MAC 16진 12자(예 `TCP_A1B2C3D4E5F6`), IP가 바뀌어도 같음(ADR-006) |
| `ip` | TCP_DIY 마지막 접속 IP, Matter는 `null` (표시용) |

## 2. 엔드포인트 요약

| # | 메서드 | 경로 | 하는 일 | v1.0 대비 |
|---|---|---|---|---|
| E1 | GET | `/api/devices` | 기기 목록 | 키 변경 |
| E2 | POST | `/api/control` | 켜기/끄기 | 200/202/오류 |
| E3 | POST | `/api/commission` | 등록 작업 시작 | **비동기**(202 + 작업 ID), 입력 제한 |
| E8 | GET | `/api/commission/{jobId}` | 등록 작업 상태 | **신규** |
| E4 | POST | `/api/devices/delete` | 삭제(숨김) | 의미·응답 변경 |
| E5 | GET | `/api/devices/{deviceId}/state` | 상태 1건 | 없는 기기 404 |
| E6 | GET | `/api/devices/{deviceId}/energy` | 현재 전력·누적 | 키·상태코드 변경 |
| E7 | GET | `/api/devices/{deviceId}/energy/history` | 기록·집계 | 파라미터·키·시각 형식 |

경로는 v1.0과 같다(E8만 추가). `DELETE /api/devices/{id}` 안은 앱 변경만 늘어 채택 안 함.

## 3. 엔드포인트 상세

### E1. GET `/api/devices`

**200** `{"totalCount": 2, "devices": [{"deviceId": "TCP_A1B2C3D4E5F6", "name": "거실 전등", "protocolType": "TCP_DIY", "state": "ON", "ip": "192.168.0.50"}, {"deviceId": "SN12345", "name": "스마트 플러그", "protocolType": "MATTER", "state": "OFF", "ip": null}]}`
- 숨김 기기는 안 나온다. `state`는 저장된 마지막 상태(최신은 E5). 재시작 직후 `OFFLINE`이 `ON`/`OFF`로 보일 수 있다(헬스체크 60초).

### E2. POST `/api/control`

**요청** `{"deviceId": "TCP_A1B2C3D4E5F6", "command": "OFF"}`

| 경우 | HTTP | 본문 |
|---|---|---|
| 즉시 성공 | 200 | `{"deviceId", "state": "OFF"}` |
| 지연 수락(예: PC 드라이버 OFF) | **202** | `{"deviceId", "state": "ON", "pendingCommand": "OFF", "delaySeconds": 45}` |
| 같은 명령 대기 중 | 202 | 같은 모양, `delaySeconds`는 남은 초. 종료 메시지 재전송 없음 |
| 드라이버가 막음 | 409 | `COMMAND_BLOCKED` |
| 기기 응답 없음·실패 | 502 | `DEVICE_UNREACHABLE` (서버는 `OFFLINE` 기록) |
| 없는·숨김 기기 | 404 | `DEVICE_NOT_FOUND` |
| 허용 안 된 명령, JSON 오류 | 400 | `INVALID_REQUEST` |

- 202의 `state`는 바뀌기 전 상태. 앱은 `delaySeconds` 뒤 E5로 확인(지연 결과는 응답으로 안 옴).
- 지연 OFF 대기 중 `ON`을 보내면 대기 OFF 취소. 서버 종료 시 대기 명령은 실행 안 하고 취소(Q-06).

### E3. POST `/api/commission` (비동기)

**요청**(필드는 v1.0과 같음) `{"protocolType": "MATTER", "deviceName": "스마트 플러그", "payload": "34970112332", "ssid": "MyWiFi", "password": "********", "driverTypes": [1]}`

| 필드 | 필수 | 설명 · **입력 제한**(S-09, 위반 시 400 `INVALID_REQUEST`) |
|---|---|---|
| `protocolType` | 예 | `"MATTER"` / `"TCP_DIY"` |
| `deviceName` | 예 | 표시 이름. 1~64자 |
| `payload` | 예 | MATTER: 수동 코드 **숫자만 11 또는 21자리**(앱이 `-`·공백 제거) 또는 QR 내용 `MT:` + `0-9 A-Z . -`. TCP_DIY: 기기가 접속할 서버 LAN IP(IPv4 점 표기) 또는 `""`(서버 기본값) |
| `ssid` | 아니오 | 0~32바이트(UTF-8), 빠지면 `""` |
| `password` | 아니오 | 0~63바이트, 빠지면 `""` |
| `driverTypes` | 아니오 | §1.2의 값만 |

- 공통: 모든 문자열 필드에 제어문자(개행 `\r` `\n`, Tab, `\0` 등) 금지. SSID·비밀번호의 공백·따옴표 허용 여부는 **확인 필요**(실기 C-6 뒤 확정, 안전하게 처리 못 하는 문자는 400).

| 경우 | HTTP | 본문 |
|---|---|---|
| 접수 | **202** | `{"jobId": "c-20261117-0001", "state": "PENDING"}` |
| 다른 등록 실행 중 | 409 | `COMMISSION_BUSY` |
| 필드 오류·입력 제한 위반 | 400 | `INVALID_REQUEST` |

- 응답은 즉시(v1.0의 120초 대기 불필요). 결과는 E8. `jobId`는 불투명 문자열.
- 앱·서버 모두 요청 본문을 로그에 남기지 않는다. 서버는 작업 상태에 요청을 보관하지 않는다.

### E8. GET `/api/commission/{jobId}` (신규)

**200** `{"jobId": "c-20261117-0001", "state": "SUCCEEDED", "deviceId": "SN12345", "restored": false, "error": null}`

| 필드 | 설명 |
|---|---|
| `state` | `PENDING` → `RUNNING` → `SUCCEEDED` / `FAILED` |
| `deviceId` | 성공 시 기기 ID, 그 외 `null` |
| `restored` | 90일 안에 삭제한 같은 기기를 다시 등록해 **기록을 이어받았으면** `true`. 앱은 사용자에게 알린다 |
| `error` | 실패 시 `{"code", "message"}`(고정 문구), 그 외 `null` |

| `error.code` | `message` | 언제 |
|---|---|---|
| `COMMISSION_FAILED` | 기기 등록에 실패했습니다. | BLE 기기 못 찾음·페어링 실패 등 |
| `COMMISSION_TIMEOUT` | 기기 등록 시간이 초과되었습니다. | 작업 제한 시간 |
| `CANCELLED` | 서버 종료로 취소되었습니다. | 서버 종료 |
| `ALREADY_REGISTERED` | 이미 등록된 기기입니다. | 이미 활성으로 등록된 기기(덮어쓰지 않음, S-05) |

- 없는 작업 → 404 `JOB_NOT_FOUND`(E1로 등록 여부 확인).
- 폴링 2초 간격. 서버 제한 시간: TCP_DIY 약 25초, MATTER 120초(**확인 필요** C-7). 앱은 180초가 지나면 E1로 확인.
- 작업 상태는 서버 메모리에만, 끝난 작업은 1시간 조회 가능.

### E4. POST `/api/devices/delete` (숨김)

**요청** `{"deviceId": "SN12345"}`
**200** `{"deviceId": "SN12345", "deletedAt": "2026-11-17T03:00:00Z", "restorableUntil": "2027-02-15T03:00:00Z", "unpaired": true}`

| 필드 | 설명 |
|---|---|
| `deletedAt` | 숨긴 시각(UTC) |
| `restorableUntil` | 이 전에 같은 기기를 E3로 다시 등록하면 기록(누적·이력)을 이어받음. 이후 기록까지 완전 삭제 |
| `unpaired` | 연결 해제 성공 여부(Matter: 페어링 해제, TCP_DIY: 초기화 메시지). `false`여도 숨김은 완료. **앱은 `false`면 "기기 쪽 해제 실패, 기기 초기화 필요" 경고를 보인다**(S-16) |

| 오류 | HTTP |
|---|---|
| 없는·이미 숨긴 기기 | 404 `DEVICE_NOT_FOUND` |
| JSON 오류 | 400 `INVALID_REQUEST` |
| 내부 오류(삭제 안 됨) | 500 `INTERNAL_ERROR` |

- 숨김 기기는 E1에 없고 E2·E5·E6·E7은 404. 되살리기 API 없음(복구는 E3 재등록만). Matter는 재페어링 필요(같은 시리얼이면 `restored: true`).

### E5. GET `/api/devices/{deviceId}/state`

쿼리 `isManualRefresh`(선택, `true`/그 외, v1.0과 같음). **200** `{"deviceId": "SN12345", "state": "ON"}`
- 없는·숨김 기기 **404** `DEVICE_NOT_FOUND`(v1.0은 200 + `UNKNOWN`). 기기가 응답하지 않으면 200 + `"OFFLINE"`.

### E6. GET `/api/devices/{deviceId}/energy`

**200** `{"deviceId": "SN12345", "state": "ON", "activePower_mW": 15230, "totalEnergy_mWh": 1048576}`
- `state`(v1.0 `device_status`), `activePower_mW` 정수 밀리와트, `totalEnergy_mWh` 64비트 정수 밀리와트시(서버 보정 누적).

| 오류 | HTTP |
|---|---|
| 없는·숨김 기기 | 404 `DEVICE_NOT_FOUND` |
| TCP_DIY 기기 | 400 `UNSUPPORTED_OPERATION` |
| 측정값 못 읽음 | 502 `DEVICE_UNREACHABLE` (서버 통계 불변, REQ-03) |

### E7. GET `/api/devices/{deviceId}/energy/history`

| 쿼리(모두 선택) | 값 | 기본 | 설명 |
|---|---|---|---|
| `from` / `to` | `YYYY-MM-DD`(+09:00) | 없음 | 시작일·끝일(포함) |
| `period` | `day` / `week` / `month` | 없음 | 있으면 **집계 모드**, 주는 월요일 시작 |
| `limit` | 정수 1 이상 | 50 | 원본 모드 최대 건수. 1000 초과는 1000으로. 집계 모드에서는 무시 |

**400 `INVALID_REQUEST` 조건**(S-10, 서버는 예외 없이 400으로 응답): 날짜가 `YYYY-MM-DD` 형식이 아니거나 없는 날짜(`2026-02-30`, `2026-13-01`) / `period`가 세 값 외 / `limit`이 정수가 아님·1 미만·64비트 범위 초과 / `period`가 있는데 `from`·`to` 중 하나라도 없음 / `from > to` / 범위가 366일 초과(366일까지 허용). 기록은 90일만 보관되므로 그 이전 구간은 비어 있다.

**원본 모드**(`period` 없음): 범위 안(없으면 전체)의 **가장 최근 `limit`건을 오래된 것부터**.
`GET .../energy/history?limit=3` → `{"deviceId": "SN12345", "state": "ON", "logs": [{"timestamp": "2026-11-16T00:20:00Z", "totalEnergy_mWh": 1040000}, {"timestamp": "2026-11-16T00:21:00Z", "totalEnergy_mWh": 1040150}, {"timestamp": "2026-11-16T00:22:00Z", "totalEnergy_mWh": 1040300}], "truncated": true}`
- `truncated`: 범위 안 기록이 `limit`보다 많아 오래된 쪽이 잘렸으면 `true`. `totalEnergy_mWh`는 그 시점 누적값.

**집계 모드**(`period` 있음): 범위 안 모든 구간(기록 없는 구간은 `sampleCount: 0`).
`GET .../energy/history?from=2026-11-01&to=2026-11-17&period=week` →
```json
{ "deviceId": "SN12345", "state": "ON", "period": "week", "utcOffset": "+09:00",
  "buckets": [
    { "start": "2026-11-01", "end": "2026-11-01", "energyUsed_mWh": 1200, "sampleCount": 1440 },
    { "start": "2026-11-02", "end": "2026-11-08", "energyUsed_mWh": 9100, "sampleCount": 10080 },
    { "start": "2026-11-09", "end": "2026-11-15", "energyUsed_mWh": 8800, "sampleCount": 10080 },
    { "start": "2026-11-16", "end": "2026-11-17", "energyUsed_mWh": 2500, "sampleCount": 2100 } ] }
```
- `start`·`end`: 구간 첫날·끝날(+09:00, 포함). 첫·끝 구간은 `from`·`to`로 잘림(2026-11-01은 일요일이라 첫 주가 하루).
- `energyUsed_mWh` = 이웃 기록 차이 중 양수의 합(뒤 기록이 속한 구간에), 음수는 0, 구간 시작 직전 기록과의 차이 포함. `day`=하루, `month`=달력 월.

| 오류 | HTTP |
|---|---|
| 없는·숨김 기기 | **404** `DEVICE_NOT_FOUND` (v1.0은 200 + `error`) |
| TCP_DIY 기기 | 400 `UNSUPPORTED_OPERATION` |
| 파라미터 오류(위 조건) | 400 `INVALID_REQUEST` |

## 4. 앱 구현 가이드

1. 성공 판정은 HTTP 상태코드(2xx). E2의 202는 "접수, 나중에 실행".
2. 오류는 `error.code`로 분기. 401이면 토큰 입력 화면.
3. `deviceId`·`jobId`는 해석하지 않는다. 처음 보는 필드는 무시.
4. E3은 보내기 전에 §3 E3 입력 제한을 앱에서도 검사(수동 코드의 `-`·공백 제거). 202 뒤 E8 폴링, 화면을 떠나도 작업은 계속.
5. 시각은 UTC로 오므로 표시 때 현지 시간으로. E7 집계 날짜는 이미 +09:00.

## 5. 범위 밖 (v2.0에 없음)

서버 푸시(WebSocket·SSE) / 기기 이름·드라이버 변경, 숨김 기기 목록·되살리기·영구 삭제 / 여러 계정·토큰, 시도 횟수 제한 / **HTTPS·집 밖 접속(보류, 나중에 주소만 바뀌도록 설계)** / 90일보다 오래된 기록.

## 6. MAJOR 변경 목록 (v1.0 → v2.0)

| # | 대상 | v1.0 | v2.0 | 앱 영향 | 근거 |
|---|---|---|---|---|---|
| 1 | ~~접속 주소~~ | `http://<IP>:8000` | **변경 없음(보류, 11-18)** | 없음 | ADR-011 |
| 2 | 오류 본문 | `{"result":"error","message"}` / `{"error":"..."}` | `{"error":{"code","message"}}`, 고정 문구 | 오류 파서 교체 | REQ-11, I-03, S-11 |
| 3 | 상태코드 | 실패도 200, 없는 기기 200/400 혼재 | 400/401/404/409/413/500/502 | HTTP 코드로 판정 | REQ-11, I-01·02·05 |
| 4 | 성공 본문 결과 키 | E1 `status`, E2 `message`, E3·E4 `result`·`message` | 없음 | 해당 키 의존 제거 | I-03 |
| 5 | 키 이름 | `id`, `type`, `total_count`, `device_status` | `deviceId`, `protocolType`, `totalCount`, `state` | 모델 필드명 | I-03 |
| 6 | E2 결과 | 200 + `message` | 200 / 202(`pendingCommand`, `delaySeconds`) / 409 / 502 | 분기, 202면 뒤에 E5 | REQ-07·08, N-01(신규) |
| 7 | E3 | 동기(120초+) | 202 + `jobId`, E8 폴링 | 등록 화면 흐름 | REQ-10, I-04 |
| 8 | E8 | 없음 | `GET /api/commission/{jobId}` | 신규 호출 | REQ-10 |
| 9 | E4 | 행 삭제, 실패 시 400 | 숨김, `deletedAt`·`restorableUntil`·`unpaired`, 없으면 404 | 완료 문구, `unpaired:false` 경고 | REQ-13, Q-03c |
| 10 | 기기 ID | TCP_DIY `ARD_<IP>` | `TCP_<MAC12>` + `ip` 필드 | 해석 안 하면 없음. 저장된 ID는 첫 갱신 때 바뀜 | ADR-006 |
| 11 | E5 | 없는 기기 200 + `UNKNOWN` | 404 | 404 처리 | I-02 |
| 12 | E6 | 400 `{"error"}`, `device_status` | 404/400/502, `deviceId`·`state` | 오류·필드 | REQ-04, I-02 |
| 13 | E7 시각 | `"YYYY-MM-DD HH:MM:SS"` | `"YYYY-MM-DDTHH:MM:SSZ"` | 파서 | I-03 |
| 14 | E7 응답 | `device_status`, `logs` | `deviceId`, `state`, `logs`, `truncated` / `buckets` | 필드, 앱 구간 계산은 `period`로 대체 가능 | REQ-12, APP-REQ |
| 15 | 본문 크기 | 제한 없음 | 16KiB, 413 | 정상 사용 시 없음 | REQ-15 |
| 16 | E3 입력 제한 | 검사 없음 | 길이·제어문자·`payload` 형식, 위반 시 400 | 수동 코드 `-` 제거, 앱 쪽 검사 | S-09, REQ-16 |

MAJOR 아님: E7 `from/to/period/limit` 추가(MINOR, APP-REQ) / E7 순서 설명 정정 — 실제·v2.0 모두 최근 N건을 오래된 순(N-02(신규)) / POST 401 본문 전달(N-03(신규) 버그 수정) / 형식이 틀린 `{deviceId}` 404 `NOT_FOUND`(현재 ID 형식은 모두 통과, S-15).

## 7. 앱 반영 순서 (서버 먼저, 앱 나중)

**v1·v2 동시 제공 기간은 두지 않는다**(1인·앱 1개라 핸들러 두 벌 비용이 더 큼). 호환되는 준비를 먼저 하고 전환은 한 번에.

| 단계 | 누가 | 내용 | 깨짐 |
|---|---|---|---|
| 0 | 사용자 | 이 초안 승인 → v2.0 확정, 앱에 사본(`api-contract-v2.0.md`) | 없음 |
| 1 | 서버 | 1차 릴리스(내부 재설계, REST v1.0 그대로) | 없음 |
| 2 | 앱 | v1.0에서도 문제없는 준비: `deviceId` 해석 안 함(C-11), 모르는 필드 무시, E3 입력 사전 검사 | 없음 |
| 3 | — | ~~Tailscale·HTTPS 전환~~ **보류(11-18)**. 주소는 `http://<서버 IP>:8000` 그대로 | — |
| 4 | 서버·앱 | 서버 2차(v2.0)를 가짜 드라이버 테스트로 완성. 앱은 v2.0 사본으로 별도 브랜치 개발 | 없음(배포 전) |
| 5 | 사용자 | **전환일**: ① DB·`registered_devices.txt` 백업 ② 서버 v2.0 배포(마이그레이션·자동 백업) ③ 바로 앱 v2.0 설치 | ②~③ 몇 분 구 앱 일부 오동작 |
| 6 | 사용자 | 실기: TCP 기기 재접속 시 ID `TCP_<MAC>`, 목록·제어·등록·삭제·기록 | — |
| 7 | 총괄 | 변경 이력의 반영 칸 갱신 | — |

**되돌리기**: 서버를 이전 커밋으로, 5-① 백업 DB 복원, 이전 앱 설치(마이그레이션은 한 방향).

## 변경 이력

| 버전 | 날짜 | 변경 | 관련 ID | 서버 | 앱 |
|---|---|---|---|---|---|
| v1.0 | 2026-11-16 | 최초 작성(As-Is) | — | 해당 없음 | ⬜ 사본 |
| v2.0 (초안) | 2026-11-17 | ~~HTTPS·Tailscale 주소~~ → 11-18 보류 | REQ-14, I-06 | — | — |
| | | 오류 본문 단일화·상태코드 규약·결과 키 제거·키 표기 | REQ-11, I-01~03·05 | ⬜ | ⬜ |
| | | E2 성공/지연 수락/실패, 지연 중복 방지·취소 | REQ-07·08, N-01(신규) | ⬜ | ⬜ |
| | | E3 비동기 + E8 | REQ-10, I-04 | ⬜ | ⬜ |
| | | E4 숨김 삭제(90일 복구), 기기 ID `TCP_<MAC>` + `ip` | REQ-13, Q-03c | ⬜ | ⬜ |
| | | E6 존재·프로토콜 확인, 실패 502 | REQ-03·04 | ⬜ | ⬜ |
| | | E7 파라미터·집계·ISO 8601·순서 설명 정정 | REQ-12, APP-REQ, N-02(신규) | ⬜ | ⬜ |
| | | 인증을 본문 읽은 뒤로, 본문 16KiB | REQ-15, N-03(신규) | ⬜ | ⬜ |
| | 2026-11-18 | 접속 LAN 유지(HTTPS 보류), E3 입력 제한(#16), E7 400 조건 명시, 고정 오류 문구, `ALREADY_REGISTERED`, `{deviceId}` 형식, `unpaired:false` 경고 | ADR-011, S-05·09·10·11·15·16 | ⬜ | ⬜ |
