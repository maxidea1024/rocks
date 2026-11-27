# 앱 정보와 명령줄 — 6. 타입과 이름

[목차](README.ko.md)

## 6. 타입과 이름

Rocks 이름은 서술형이다. UE 이름은 이 표에만 둔다.

| UE 5.8 | Rocks | 한 줄 설명 |
|---|---|---|
| `FApp`(부트 값 부분) | `AppInfo` | 부트 때 한 번 채우는 프로세스 정보 불변 레코드 |
| (없음) | `AppInfoScope` | `AppInfo`를 프로세스에 설치·해제하는 범위 객체(`LogInstallScope`와 같은 방식) |
| `IsUnattended`·`IsBenchmarking`·`bUseFixedSeed`·`CanEverRender` 등 | `RunTraits` | 실행 성격 값 묶음 |
| `IsRunningCommandlet` 등 실행 종류 전역 | `HostKind` | `Game`·`Commandlet`·`Tool`·`TestRunner`(에디터·서버는 생길 때 더한다) |
| `GetBuildConfiguration`·`GetBuildTargetType`·`GetBranchName`·`GetBuildDate` 등 | `build::Description()` → `BuildDescription` | 빌드가 주입한 상수 레코드 |
| `EBuildConfiguration` | `build::BuildConfiguration` | `Debug`·`Development`·`Release` |
| `EBuildTargetType` | `build::TargetType` | `Game`·`Editor`·`Client`·`Server`·`Program`(빌드 규칙의 `TargetType`과 같은 이름·같은 값 — [빌드 타깃](../BuildTargets/README.ko.md) BD1) |
| `FAppTime` | `FrameTiming` | 한 프레임의 시간 값(double 초). 지금의 `EngineTime`을 대체한다 |
| `FAppTime::GetApproximateCurrentTime` | `LatestFrameIndex()` + `platform::MonotonicNanoseconds()` | 아무 스레드가 읽는 대략값 |
| `FCommandLine` | `ProcessCommandLine` | 진입점에서 캡처한 불변 UTF-8 인자 배열 |
| `FParse`·`UCommandlet::ParseCommandLine` | `ParseCommandLine<T>()` | 스키마 기반 파서(옵션 struct로 결과를 낸다) |
| `UCommandlet`의 `Help*` 칸 | `CommandLineSchema` | MetaCompiler가 만드는 명령줄 스키마(이름·별칭·도움말·예시)와 필드별 바인더 표 |
| `UCommandlet::ApplyCommandLineSwitches` | 생성 바인더(`GeneratedCommand<S>`) | 필드 하나에 값 텍스트 하나를 변환해 넣는 생성 함수 |
| `UCommandlet`·commandlet registry | `CommandSet` | 명령 묶음(하위 명령·공용 옵션·`help`·`--version`) |
| `-CmdLineFile=` | 응답 파일 `@file` | 도구 전용 |
| `FILTER_COMMANDLINE_LOGGING` | `Secret` 지정자 | 로그에서 값을 가릴 옵션 |
| `#if !UE_BUILD_SHIPPING`으로 감싼 명령줄 조회 | `NotInRelease` 지정자 | Release 구성에서 받지 않는 옵션 |
| (없음) | `ValidateCommand` hook | 옵션 사이 검증. 영어 이유 문자열을 진단으로 남긴다 |
| (없음) | `Remaining` 지정자 | 해석하지 않고 넘길 남은 인자 |
