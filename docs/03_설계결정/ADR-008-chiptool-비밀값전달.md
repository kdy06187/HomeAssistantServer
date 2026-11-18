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
