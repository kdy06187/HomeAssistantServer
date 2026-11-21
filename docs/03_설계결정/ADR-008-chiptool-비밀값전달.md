# ADR-008: chip-tool 비밀값 전달 — `interactive start` + stdin, `ChipToolRunner` 인터페이스

- **날짜**: 2026-11-17
- **상태**: 채택(사용자 결정 Q-04, 2026-11-17). **실기 확인 C-6 뒤 확정**, 실패 시 후퇴안
- **관련 요구사항**: REQ-16, REQ-10, REQ-09, REQ-17, NFR-03, N-07

## 배경
- 페어링 때 Wi-Fi 비밀번호와 PIN이 chip-tool 인자에 들어가 `ps`(`/proc/<pid>/cmdline`)로 보인다(N-07).
- 조사(읽기 전용): chip-tool은 파일·환경변수 입력을 지원하지 않고, `interactive start`는 표준 입력에서 명령을 읽는다.
- 그 밖의 chip-tool 호출은 `popen`(셸)과 `timeout 5`를 써서 종료 시 강제 종료·테스트가 어렵다.

## 선택지
| 선택지 | 장점 | 단점 |
|---|---|---|
| A. `interactive start` 실행 후 stdin으로 `pairing ...` 한 줄과 `quit` 전달 | 비밀값이 argv에 없음, 변경이 페어링 함수 하나 | readline 프롬프트·출력 파싱·종료 감지가 바뀜, 공백·특수문자 따옴표 규칙 미확인 |
| B. 현 방식 유지 + 위험 문서화(+ `/proc` `hidepid=2` 권장) | 변경 없음 | REQ-16 미충족 → 범위 재협의 |
| C. `interactive server`(WebSocket) 상주 | 매 실행 비용 없음 | 수명·재접속 관리 추가, 구조 변경 큼 |

실행 계층:
| 선택지 | 장점 | 단점 |
|---|---|---|
| a. 기존 `popen`/`fork` 코드 그대로 | 변경 없음 | 셸 주입 위험 경로 잔존, 제한 시간·취소·가짜 교체 불가 |
| b. `ChipToolRunner` 인터페이스(셸 없이 fork+exec, stdin, 제한 시간, `cancelAll`) | 테스트에서 `FakeChipToolRunner`, 종료 시 자식 정리(REQ-09), 비동기 커미셔닝 취소(REQ-10) | 프로세스 관리 코드 직접 작성(파이프·waitpid·kill) |

## 결정
A + b. 모든 chip-tool 호출을 `ChipToolRunner::run(args, stdinText, timeout)`으로 바꾸고, 페어링만 `args={"interactive","start"}`, `stdinText="pairing ble-wifi <node> str:<ssid> str:<pw> <pin> 3830 --paa-trust-store-path <경로>\nquit\n"` 형태로 보낸다(정확한 문법은 C-6 확인 뒤). 제한 시간 초과·종료 시 SIGTERM → 1초 뒤 SIGKILL.
**보강(2026-11-18, 보안 점검 S-08·S-09)**:
- 입력 검증(S-09): stdin 한 줄에 값을 이어 붙이므로 개행이 들어가면 다른 chip-tool 명령이 실행될 수 있다. E3를 HTTP 계층에서 검증한다 — `ssid` 0~32바이트, `password` 0~63바이트, 모든 문자열 필드의 제어문자(`\0`~`\x1F`, `\x7F`, 개행·Tab 포함) 거부, MATTER `payload`는 숫자 11/21자리 또는 `MT:`+`[0-9A-Z.-]`, TCP_DIY `payload`는 IPv4 또는 빈 문자열, 위반 시 400 `INVALID_REQUEST`. 공백·따옴표·`\` 처리는 C-6 결과로 정하고, 안전하게 감쌀 수 없는 문자는 거부한다.
- 출력 마스킹(S-08): `run(..., secretsToMask)`으로 호출자가 비밀값(비밀번호·PIN·설정 코드) 목록을 넘기면, `ChipToolRunner`가 돌려주는 `output`에서 그 값을 `***`로 바꾼다(한 곳에서). chip-tool 출력은 오류 응답 `message`에 넣지 않는다(ADR-003 고정 문구).
- 작업 상태 표에는 요청을 보관하지 않는다(`jobId·state·deviceId·restored·error code`만). 요청은 `commission` 반환 뒤 폐기(가능하면 문자열을 0으로 덮어씀, 복사본까지 보장하지는 못함).
- C-6에 "stdin 모드에서 비밀값이 stdout·로그·파일(`/tmp` 등)에 남는지" 확인을 추가한다.

C-6에서 A가 동작하지 않으면 **B로 후퇴**: 인자 방식 유지, `docs/`에 위험(같은 PC 로컬 계정이 볼 수 있음, 1인 PC면 낮음)과 `hidepid=2` 설정을 문서화하고 REQ-16을 사용자와 재협의한다. 두 방식을 동시에 구현하지 않는다(YAGNI). 후퇴해도 출력 마스킹·입력 검증은 그대로 필요하다.

## 이유
- 비밀값을 커널이 공개하는 인자 목록에서 빼는 가장 작은 변경이다.
- `ChipToolRunner`는 REQ-09(종료 5초 안에 자식 정리)·REQ-10(작업 취소)·REQ-17(가짜 출력) 셋이 모두 필요로 하는 공통 지점이다.

## 감수하는 단점과 영향
- 실기 확인 항목(C-6): stdin 파이프(터미널 아님)에서 readline이 명령을 실행하는지, SSID·비밀번호의 공백·따옴표·`\` 처리, 완료·실패 감지 문자열, `quit` 뒤 정상 종료.
- 비밀값은 프로세스 메모리·파이프에만 있다. 로그에 stdinText를 남기지 않는다(코드 리뷰 항목).
- chip-tool 출력 문자열 의존(기존 위험)은 그대로다. `ChipToolOutputParser` 단위 테스트로 회귀만 잡는다.

## 학습 포인트
- 프로세스 인자 노출(`/proc/<pid>/cmdline`), fork/exec/pipe/waitpid, 셸 주입, Adapter 패턴(외부 프로그램을 인터페이스 뒤로).

## 2차 보완 — 확정안(사용자 결정 반영, 최종 승인 전) — 2026-11-21

### 0) 확정안 요약
| 항목 | 확정안 | 결정 |
|---|---|---|
| 전달 방식 | **A. 명령줄 인자 유지 + 위험 수용**(이 ADR의 원래 결정 "stdin"을 대체. 최종 승인 때 상태를 "대체됨 → 2차 보완 A"로 바꿈) | 결정: 사용자 11-21 |
| `hidepid=2` | **쓰지 않음**(실기 K-3 삭제) | 결정: 사용자 11-21 |
| REQ-16 완료 기준 | "1인 PC에서 수용한 위험(같은 계정의 `ps`에 보임), 계정이 늘면 재평가" — `01_요구사항.md` REQ-16 변경안 줄 | 결정: 사용자 11-21 |
| 마스킹 강화 | 아래 7) — SSID 추가, 빈 값 제외, 4바이트 미만 처리, 마스킹 후 자르기. 13번 단위 테스트 | 결정: 사용자 11-21(SSID·빈 값·순서) / 4바이트 기준은 추천안 그대로(최종 승인 때 확인) |
| 러너 걸린 시간 로그 | 13번에 넣음(4 ②) | 추천안 그대로(최종 승인 때 확인) |
| 기각 | B stdin — history 잔류·`Command:` 에코·종료 코드 0 고정(F3·F4·F6) 3개 새 위험이 줄이려는 위험보다 큼 / C interactive server — 수명·포트 관리가 요구 대비 과함 / `hidepid=2` — 같은 계정은 못 막고 polkit 등 시스템 부작용 가능, 1인 PC라 이득 없음 | — |

대상: 2차 13번(REQ-16, C-6·C-7). 근거: 실기 `docs/07_실기시험-1차.md` 5.2, 보안 SEC-1차-03·10, chip-tool 소스(읽기만, `examples/chip-tool/commands/interactive/InteractiveCommands.cpp`, `commands/common/Commands.cpp`·`Command.cpp`).

### 1) 실기·소스에서 새로 안 것
| # | 사실 | 근거 | 의미 |
|---|---|---|---|
| F1 | interactive는 따옴표를 무시하고 공백으로 나눈다 | 실기 5.2 ② | `str:` 값에 공백이 있으면 인자가 갈라짐 |
| F2 | 옥텟 문자열 인자는 `hex:` 접두사를 받는다(SSID·비밀번호는 옥텟 문자열 인자) | `Command.cpp` `OctetStringFromCharString`, `PairingCommand.h` `AddArgument("ssid"/"password")` | F1은 `hex:`로 피할 수 있음(interactive에서도 되는지는 실기 필요) |
| F3 | 입력 줄을 readline 기록에 넣고 매번 `write_history` | `InteractiveCommands.cpp` `GetCommand` | 비밀값이 파일에 남음. 파일 위치는 `--storage-directory`가 있으면 그 폴더, 없으면 `$TMPDIR` 또는 `/tmp`. **지금 서버는 `../config/chip-tool`(0700)을 넘기므로 서버 실행 시 `../config/chip-tool/chip_tool_history`(umask 077 → 0600)에 남는다**(실기는 저장소 옵션 없이 돌려 `/tmp`에 생김) |
| F4 | 실행 전 `Command: <전체 인자>`를 로그로 출력 | `Commands.cpp` `RunInteractive` | 비밀값이 stdout에 나옴(실기 "출력 2회"). 러너 마스킹이 필요하며 `hex:`를 쓰면 **hex 문자열도** 가릴 목록에 넣어야 함 |
| F5 | `quit` 없이 stdin이 닫히면 끝나지 않음 | 실기 5.2 ③ | 단, 실기 ①은 명령과 `quit`을 **한꺼번에** 보냈는데 명령을 끝까지 실행한 뒤 종료(exit 0, 2.4s). 명령은 동기로 돌므로 "완료 확인 뒤 quit"은 필요 없고 **끝에 `quit` 한 줄을 붙이면 된다**(원래 결정과 같음) |
| F6 | interactive 루프가 끝나면 `SetCommandExitStatus(CHIP_NO_ERROR)` | `InteractiveCommands.cpp` `InteractiveStartCommand::RunCommand` 끝 | **페어링이 실패해도 종료 코드가 0일 가능성**. 지금 성공 판정(종료 코드 0 + 성공 문구) 중 종료 코드 신호를 잃고 출력 문구에만 기댐 → 실기 필요 |
| F7 | chip-tool은 파일·환경 변수로 비밀값을 받지 않는다 | 이 ADR 배경(조사) | "0600 임시 파일"은 결국 stdin 리다이렉트와 같아 이점 없음 |
| F8 | 서버 PC 로그인 계정은 1개(`kdy`) | `08_검증-1차.md` 6절 D-7 | 다른 계정이 `ps`로 볼 위험은 지금 구성에서 낮음. 같은 계정은 어느 방식이든 `/proc/<pid>/mem`·파일을 읽을 수 있어 막을 수 없음 |

### 2) 대안
| 대안 | 내용 | 장점 | 단점 |
|---|---|---|---|
| **A. 인자 방식 유지 + `/proc` `hidepid=2`(운영 설정) + 위험 문서화** | 지금 코드 그대로(execv, `str:`). `06_배포.md`에 `hidepid=2` 마운트 안내. REQ-16 완료 기준을 "다른 계정에서 안 보임"으로 재협의 | 코드 변경 0. 공백·따옴표 문제 없음(execv는 인자를 나누지 않음). 종료 코드 판정 유지. 파일 잔류 없음 | 같은 계정·root에는 보임(어느 방식이든 같음). `hidepid`는 시스템 설정이라 일부 데몬(polkit 등)에 부작용 가능, 서버 코드로 보장 못 함 |
| B. interactive stdin + `hex:` + history 차단 | 페어링만 `interactive start`, SSID·비밀번호를 `hex:`로, 끝에 `quit`. history는 ① `chip_tool_history` 자리를 **빈 디렉터리로 미리 만들어** 쓰기 실패를 유도, 또는 ② 실행 뒤 파일 삭제 | argv에 비밀값 없음 → 원래 REQ-16 그대로 충족 | F4 출력 노출(마스킹 목록에 hex 추가), F6 종료 코드 신호 상실(출력 판정만), ①은 SDK 동작에 기대는 꼼수(readline 버전에 따라 다를 수 있음), ②는 서버가 죽으면 파일이 남고 삭제 전까지 디스크에 있음. 테스트·실기 항목 증가 |
| C. `interactive server`(WebSocket) 상주 | 프로세스 하나에 명령 전송 | 매번 실행 비용 없음 | 수명·재접속·포트 관리. 같은 LAN·계정 노출 문제는 새 포트로 옮겨 감. 요구 대비 과함(원래도 기각) |

### 3) 추천: **A** (B로 바꿀 조건을 함께 기록) — 확정(사용자 11-21), 단 2)의 A 중 `hidepid=2`는 빼고 위험 수용
- 근거: 노출 대상이 "같은 PC의 다른 계정"뿐인데 그런 계정이 없다(F8). B는 이 작은 위험을 줄이려고 **새 위험 3개**(디스크 잔류 F3, 출력 노출 F4, 실패 판정 약화 F6)를 들여온다. 실무에서도 "공격자 모델에 맞는 가장 단순한 통제"를 고르는 것이 원칙이다(Defense in depth는 비용 대비).
- 함께 할 것(어느 안이든): SEC-1차-10 — SSID도 마스킹 목록에, 실패 출력 로그는 끝 N줄(예 4KiB)만.
- B로 바꿀 조건: 서버 PC에 다른 계정이 생기거나 여러 사람이 쓰는 서버로 옮길 때.

### 4) C-7 페어링 시간 측정(코드 변경 없이 / 2차 13번에서)
| 방법 | 내용 | 장단점 |
|---|---|---|
| ① 로그 줄에 시각 붙이기(지금 가능) | `cd $FIELD/run && $REPO/build-field/HomeAssistantCore 2>&1 \| while IFS= read -r l; do printf '%s %s\n' "$(date +%T.%3N)" "$l"; done \| tee $FIELD/logs/server-c7.log` 후 `기기 등록 시도: NodeId=` → `커미셔닝 성공` 간격 | 코드 0. 시리얼 읽기(최대 10초)가 포함되므로 그만큼 빼서 본다 |
| ② 러너가 걸린 시간을 로그(2차 13번) | `ProcessChipToolRunner`가 실행마다 `[ChipToolRunner] <명령 이름> 종료 코드 N, X ms` 한 줄(인자·비밀값 없음) | 모든 명령에 재사용, 이후 제한 시간 조정 근거. 작은 코드 추가 |
| ③ 앱 화면 스톱워치 | 사람이 잼 | 오차 크고 반복 어려움 |
- 추천: 지금은 ①로 **5회**(가까운 거리·먼 거리 섞어) 측정, 2차 13번에 ② 추가. 제한 시간은 "최댓값 × 2, 상한 180초"로 정한다.

### 5) 실기 필요
- ~~(B를 고를 때만) `hex:` 해석·실패 시 종료 코드·history 선점~~ — A 확정으로 **필요 없음**(K-2 삭제).
- ~~(A) `hidepid=2` 부작용~~ — `hidepid=2` 안 씀(K-3 삭제, 결정: 사용자 11-21).
- C-7 측정 5회(방법 ①) — 남음(K-1).

### 6) 영향 번호 · 남은 결정
- 영향: **13번**(마스킹·러너 시간 로그), 10번(E3: 공백·따옴표 **허용**, 제어문자만 금지 — execv라 인자가 갈라지지 않음).
- 결정됨: ① A ② `hidepid=2` 안 씀 ③ REQ-16 재문구(사용자 11-21). 남음: ④ 측정 방법 ② — 추천안 그대로(최종 승인 때 확인).

### 7) 마스킹 규칙(13번, 단위 테스트로 고정)
현재 코드 확인(읽기만): 페어링 목록 `{wifiPassword, manualPincode, pinText}`(`MatterController.cpp:287`) — SSID 없음. `ProcessChipToolRunner::maskSecrets`는 긴 값부터 가리고 빈 문자열은 이미 건너뜀(`ProcessChipToolRunner.cpp:107~121`). 출력은 **수집 중 1MiB에서 자른 뒤 마스킹**(`:256~295`) → 잘린 끝에 비밀값 앞부분이 남을 수 있음. 실패 때 `MatterController.cpp:296`이 출력 전체를 로그에 씀.

| # | 규칙 | 근거 |
|---|---|---|
| M-1 | 목록 = `{wifiSsid, wifiPassword, manualPincode, pinText}` | SSID는 비밀은 아니지만 집 위치를 특정하는 개인정보(SEC-10) — 결정: 사용자 11-21 |
| M-2 | 빈 값은 목록에서 뺀다(호출자 쪽에서도, 러너의 기존 건너뜀은 유지) | 열린 Wi-Fi(비밀번호 `""`)에서 무한 반복 방지 — 결정: 사용자 11-21 |
| M-3 | **4바이트 미만** 값: SSID는 목록에서 빼고(공개 방송되는 값이라 노출 피해 작음), 비밀번호·PIN이 4바이트 미만이면 실패 출력 로그를 통째로 생략("출력 생략: 짧은 비밀값") | 1~3바이트 값을 `***`로 바꾸면 chip-tool 출력(16진 덤프·단어)이 대량으로 망가져 진단이 불가능하고, 반대로 안 가리면 노출. WPA2 비밀번호는 8~63자, PIN은 8자리, 수동 코드는 11/21자리라 정상 값은 해당 없음 — 추천안 그대로(최종 승인 때 확인) |
| M-4 | 순서: **(수집 상한에서 잘렸으면 끝에서 "가장 긴 비밀값 길이 − 1"바이트를 더 버림) → 마스킹 → 로그용 끝 4KiB 자르기** | 자른 뒤 마스킹하면 경계의 비밀값 조각이 남음(SEC2-02). 실패 로그는 끝부분만(SEC-10) — 결정: 사용자 11-21(순서) |
| M-5 | 러너는 인자 배열을 로그에 쓰지 않는다(명령 이름·종료 코드·걸린 시간만) | 인자에 비밀값이 있음 |

테스트(13번 `tests/unit`): M-1 목록 구성(가짜 러너가 받은 `secretsToMask` 확인), 빈 비밀번호, 3바이트 SSID, 1MiB 경계에 비밀값이 걸친 출력, 4KiB 경계에 비밀값이 걸친 출력, 긴 값·짧은 값이 겹친 경우(기존 정렬 유지).
