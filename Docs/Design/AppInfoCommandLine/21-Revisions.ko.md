# 앱 정보와 명령줄 — 21. 변경 이력

[목차](README.ko.md)

## 21. 변경 이력

- **개정 1 초안(2026-11-18)**: UE 5.8 `FApp`·`FAppTime`·`FCommandLine`·`FParse`·`UCommandlet` 조사, Rocks 앱 정보·명령줄 현황(C++·C#), `AppInfo`·`FrameTiming`·`ProcessCommandLine`·빌드 정보 주입, 리플렉션 기반 C++ 명령줄 파서와 System.CommandLine 기반 C# 도구의 공통 규약, 결정 D1–D28, 단계 AP1–AP4·CL1–CL7.
- **개정 2(2026-11-18)**: 교차검증 1차를 반영하고 결정 D1–D30을 확정했다.
  - 바인딩(D19): ReflectionText 대입 방식은 쓸 수 없었다. 원소 추가 표기가 없고, 같은 경로 두 번이 오류이며, `\`로 끝나는 원소가 깨지고, Validate 이유를 전할 수 없고, enum이 접두 일치였다. 그래서 MetaCompiler 생성 필드별 바인더(Core `NumberText`·생성 enum 표)로 바꿨다. 검사 순서와 `ValidateCommand` 이유 통로를 정했다.
  - 출력(D21·D22): "Rocks 명령줄 출력 규약"을 직접 정의했다(14.4·14.5절). System.CommandLine은 파싱에만 쓰고 출력은 `RocksCli`가 그린다. 14.4절(자체 문구)과 16.2절(System.CommandLine 문구)의 모순을 없앴다. 현지화·묶음·`:` 구분자·bool 값 소비 차단 설정을 적었고, `RocksCli` 크기를 900줄로 다시 추정했다.
  - 지정자(D18): `DevelopmentOnly`(설정 전용, `Cheat`로 바뀔 예정)를 쓰지 않고 `NotInRelease`·`Remaining`·`Group`을 더했다. Command 키의 사용 위치를 검사한다.
  - identity(D30): Command struct는 identity 면제로 했다. 낮은 계층 도구도 ObjectRuntime 없이 옮긴다. 15절 비교에 실제 비용을 넣었다.
  - 알 수 없는 인자(D29): 도구는 오류, Game 대상 Release 호스트는 경고와 `<unparsed>` 가림으로 했고 `Remaining` 필드를 두었다.
  - 시간·실행 성격(D5·D7·D8·D9): UE와 같게 고쳤다. `--deterministic` 기본 1/30, `--use-fixed-time-step` 추가, `--fps`는 길이만, 고정 step 계열은 출시 빌드에서도 받고, 세션 인자는 비출시만 받는다. 9.1절의 잘못된 절 참조를 고쳤다.
  - 부트 순서: AppInfo 설치를 3단계(실행 성격 직후)로 옮겨 설정 등록 코드가 읽을 수 있게 했다.
  - 크기(D27): 도움말을 빼는 기준을 Game 대상 Release(`ROCKS_TARGET_KIND_GAME`)로 바꿨다.
  - commandlet(D24): config-dump 등록 이전과 ShaderSystem·AssetSystem 문서 반영을 단계에 넣었다.
  - C# 패키지(D26): NuGet 감사·빌드 증거·허용 목록 policy·라이선스 기록·별도 어셈블리 `Rocks.Tooling.Cli`·SDK 동봉 조건을 적었다.
  - 이름 제안: Core `EditDistance`를 CVar와 함께 쓰고, 문턱은 이름 붙은 상수로 했다.
  - 단계 표: AP2 선행에 CL2를 넣었고, AP1이 `RunTraits`를 채우는 방법, CL4의 HostConfig·실행 파일 등록 변경, Sandbox 자식의 `--deterministic`을 적었다.
  - 그 밖: Windows 인코딩(비 ASCII 인자가 깨진다, best-fit 보안 문제, `wmain`·직접 분해·`GetModuleFileNameW`), 대화상자 억제 공용 API, `TryFillSecureRandom` 이름, `AppInfoScope`와 `LogInstallScope`의 차이, 스키마 칸 형식, 묶음 설명, 도움말 예의 `--config`, CI branch, UE `UE-CmdLineArgs`, Developer 판정 문구, 명령줄 enum의 kebab-case 이름을 반영했다.
- **개정 3(2026-11-19)**: [크래시 처리·실행 진입점](../CrashHandlingAndLaunch.ko.md) LC3·LC4 구현을 반영했다.
  - 12절: `ProcessCommandLine`은 구현됐다(`Core/App/ProcessCommandLine.h`). 상한을 OS 한계에 맞춰 인자 32768개·텍스트 2 MiB·실행 파일 경로 96 KiB로 바꾸고 `IsCaptured`를 더했다. 캡처는 Launch 진입점이 한다.
  - 12절·D15: Windows `main(char**)`·`WinMain`의 `GetCommandLineW` 직접 분해를 지웠다(그 설계 D7). Launch 진입점은 늘 `wmain`·`wWinMain`(CRT `__wargv`)이다. 17절의 직접 분해 표 시험도 뺐다.
  - 7.1절 PlatformBase 행, 8.3절 0단계(Launch가 캡처, 실패 종료 코드는 `commandLineFailureExitCode`)를 맞췄다.
  - 18절 AP1: 크래시 처리 설계가 먼저 구현한 부분을 적고, 남은 일(`ApplicationKind` → `HostKind`, `suppressSystemErrorDialogs` ← `RunTraits::unattended`)을 적었다. 크래시 메타데이터에 가린 명령줄(CL2 뒤)·`--crash-*` 호스트 공용 옵션(CL4 뒤)·`BuildDescription`(AP2 뒤)을 더하는 일은 그 설계 21절에 있다.
- **개정 4(2026-11-22)**: [오디오 시스템](../AudioSystem/README.ko.md) 요구로 오디오 실행 성격을 열었다. `RunTraits::audioOutput`(부트가 Server 대상·commandlet·benchmark·`--no-sound`로 정함)·`audioMuted`(`--mute-audio`), 비출시 `--allow-commandlet-audio`. UE `-enablesound`는 두지 않는다. 볼륨 배수는 오디오 CVar로 넘겼다(8.6절).
