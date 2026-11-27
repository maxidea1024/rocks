# 앱 정보와 명령줄 — 20. 확인하지 못한 것

[목차](README.ko.md)

## 20. 확인하지 못한 것

- **System.CommandLine 판과 API.** 3.0.0(2026-11-10)·2.0.15라는 판 번호와 날짜, 3.0의 API는 확인하지 않았다. C# 예시는 2.0 API(`Option<T>{Description}`·`SetAction`·`GetValue`)를 따랐다. 착수 때 고정할 판의 API로 고친다.
- **System.CommandLine 설정이 16.2절 표를 모두 지원하는지.** `:` 구분자 끄기, bool arity 0, 응답 파일 끄기, 지시자 빼기, `TreatUnmatchedTokensAsErrors`가 채택 판에서 설정으로 되는지 확인하지 않았다. 안 되는 항목은 `RocksCli`가 토큰을 넘기기 전에 거부하거나 고쳐 넣는다(16.2절). 현지화 리소스(ko 포함)를 패키지가 싣는다는 것도 원본에서 확인하지 않았다. `SatelliteResourceLanguages`와 InvariantCulture 설정은 어느 경우든 둔다.
- **Rocks.Build가 같은 구성에서 모듈 object를 Game·Program 대상 사이에 공유하는지.** 공유하면 `ROCKS_TARGET_KIND_*` 정의를 모듈 전체에 줄 수 없다. 그때는 생성 스키마 TU만 대상별로 컴파일한다(7.2절). CL1 착수 때 확인한다.
- **Core `TryParseDouble`이 `nan`·`inf` 표기를 받는지.** 받든 받지 않든 바인더가 유한값만 통과시킨다. C# 사용자 파서를 같은 문법으로 맞추려고 CL2에서 확인한다.
- **생성 바인더의 크기.** Command struct 하나에 2 KiB 안팎으로 추정했다. CL2에서 측정해 7.2절 상한을 고친다.
- **Windows 직접 분해 규칙의 세부.** MSVC CRT(2008 이후) argv 규칙(역슬래시와 따옴표 짝, 따옴표 안 `""`)을 그대로 옮긴다. 실제 CRT 동작과 같은지 AP1의 표 시험과 `Process::Start` 왕복 시험으로 확인한다.
- **`activeCodePage` UTF-8 manifest의 영향.** 남은 ANSI API(CRT 출력 등)에 주는 영향과 지원 OS 하한을 확인하지 않았다. 보조 수단으로만 검토한다.
- **비 ASCII 인자 깨짐의 실측.** 코드상 ANSI argv를 UTF-8로 취급하므로 깨진다고 판단했다. 실행해 보지는 않았다. AP1 시험이 확인한다.
- **엔진 호스트가 worker 없이(`InlineExecutor`만으로) 돌 수 있는지.** `--single-thread`를 열지 정하는 근거다.
- **Android 호스트가 명령줄 대신 받는 인자 경로.**
- **UE 온라인 서버 판정 delegate**(`SetIsServerForOnlineSubsystemsDelegate`)를 엔진 어디서 설정하는지. `Runtime/Engine/Private`에서 찾지 못했다(플러그인으로 보인다). 범위 밖이라 더 찾지 않았다.
- **UE `FApp` 조회 함수의 읽는 스레드 분포.** 대표 호출(3절 표의 파일:줄)과 Runtime 아래 호출 파일 수만 셌고, 호출마다 스레드를 추적하지는 않았다.
