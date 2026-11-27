# 앱 정보와 명령줄 — 7. 모듈 배치와 코드 크기

[목차](README.ko.md)

## 7. 모듈 배치와 코드 크기

### 7.1 배치

Engine/Source/Runtime에는 핵심만 둔다. 게임 실행 파일이 쓰는 최소 부분과 도구·commandlet 전용 부분을 나눈다.

| 위치 | 담는 것 | 누가 링크하나 |
|---|---|---|
| Core 런타임 `Core`: `Core/App/AppInfo.h`, `Core/App/ProcessCommandLine.h`, `Core/Time/FrameTiming.h` | `AppInfo`·`RunTraits`·`HostKind`·`AppInfoScope`, `ProcessCommandLine`, `FrameTiming`·`LatestFrameIndex` | 모든 실행 파일 |
| Core 런타임 `Core`: `Core/Build/BuildInfo.h`(기존 헤더 확장) | `build::Description()` 선언. 정의는 Rocks.Build 생성 TU | Rocks.Build로 링크한 모든 실행 파일 |
| Core `PlatformBase` | 실행 파일 경로, 터미널 너비 조회, 대화상자 억제 정책(`platform::SuppressSystemErrorDialogs`, 크래시 수집 LC1b에서 TestSupport·ShaderToolSpike 복사본을 합쳤다). Windows 명령줄은 CRT가 나눈 넓은 문자 argv를 쓰므로 직접 분해는 두지 않는다(개정 3, 12절) | 모든 실행 파일 |
| Core 런타임 `Core`: `Core/Text/EditDistance.h`, Base `Guid` 텍스트 변환 | 이름 제안용 편집 거리(CVar 12절과 같은 함수), Guid 36자 소문자 텍스트 변환(ReflectionText도 이 함수를 쓰게 한다) | 공용 |
| 새 Core 런타임 모듈 `CommandLine`(의존: `Core`만) | `CommandLine/CommandLineSchema.h`(생성 스키마·바인더 표 형식), 토큰화·옵션 매칭·생성 바인더 호출·필수·범위·`ValidateCommand`·진단·이름 제안·비밀 가리기·짧은 사용법 줄 | 게임 호스트, Sandbox, 도구. Command struct를 선언한 모듈 |
| `HostConfig`(기존) | 호스트 공용 옵션 struct `HostCommandLineOptions`(`--set`·`--config`·`--trace`·`--project`·실행 성격)와 부트 도우미. 이 모듈에 Metadata 선언이 새로 생긴다(Command struct만 있으므로 identity 면제 — D30) | 게임 호스트, Sandbox, RocksCommandlet |
| 새 Core Developer 모듈 `CommandLineTools`(`Core/Source/Developer/` 아래 — Game 대상의 Release는 도달하면 policy 위반이고, Program 대상은 Release에서도 쓴다. `ReleaseExclusion.cs:28–33`) | 도움말 writer(너비·열·예시)·응답 파일·`CommandSet`·`--version` 출력 | 도구·commandlet, Game 대상의 Debug·Development(구성별 의존 — `Sandbox.Build.cs`의 `shaderCompiler` 분기와 같은 방식) |
| Engine 런타임 `Engine` | `FrameTiming` 갱신과 게시, `RunTraits` 소비(고정 step) | 엔진 호스트 |
| `Programs/Rocks.Build` | 생성 TU `<exe>.BuildStamp.cpp`, 대상 종류 정의 `ROCKS_TARGET_KIND_GAME`·`ROCKS_TARGET_KIND_PROGRAM` | — |
| 새 C# 어셈블리 `Programs/Rocks.Tooling.Cli` | `RocksCli`(System.CommandLine 파싱 위에 Rocks 출력 규약 렌더러). `Rocks.Tooling.Common`과 분리해, 설치 SDK의 게임 규칙 프로젝트(`InstalledSdk.cs:293–294`가 `Rocks.Tooling.Common.dll`을 참조)로 패키지 의존이 번지지 않게 한다 | C# 도구 |

지금 Core 런타임에 있는 `CommandletRegistry`(`Core/Source/Runtime/Core/Public/Core/Commandlet/Commandlet.h`)는 `CommandLineTools`의 `CommandSet`으로 대체하고 지운다(하위호환 없음, D24). 게임 실행 파일은 commandlet을 쓰지 않는다.
- Core 런타임 `Config`가 공개하는 `RegisterConfigDumpCommandlet(CommandletRegistry&)`(`Core/Source/Runtime/Config/Public/Config/ConfigDump.h:47–48`)는 Runtime 모듈이 Developer `CommandSet`을 볼 수 없으므로 옮긴다. 덤프 본체(`ConfigDump`)는 Config에 두고, 명령 등록과 `ConfigDumpOptions`는 `Programs/Commandlet`으로 옮긴다(CL4).
- [셰이더 시스템](../ShaderSystem/README.ko.md) 4·5.2절(나누기 전 96·147행, `shader-compile`)과 [에셋 시스템](../AssetSystem/README.ko.md) 37행(`asset-*`)은 `CommandletRegistry`를 전제로 썼다. CL7에서 `CommandSet`으로 고친다.

### 7.2 코드 크기 목표

외부 라이브러리를 쓰지 않은 이유가 크기이므로, 자체 구현에도 상한을 둔다. 단계 완료 때 측정해 보고한다.

| 부분 | 소스 상한(시험 제외) | Release x64 실행 파일 증가 상한 | 측정 |
|---|---|---|---|
| Core `App`·`Time`·`ProcessCommandLine`(PlatformBase 분해 포함) | 800줄 | 8 KiB | Release Sandbox 크기 비교 |
| `CommandLine`(런타임 파서) | 1,500줄 | 24 KiB | Release Sandbox 크기 비교. 의존은 Core뿐이다(수 변환은 이미 링크된 `NumberText`) |
| 생성 바인더 | 명령 하나에 필드 수에 비례 | Command struct 하나에 2 KiB 안팎(필드 10개 기준, CL2에서 측정해 상한을 고친다) | Release Sandbox(`HostCommandLineOptions` 하나)와 Release MathBench 크기 비교 |
| `CommandLineTools`(Developer) | 2,000줄 | Game 대상 Release 0(링크하지 않는다) | Game 대상 Release 링크 맵에 기호가 없음을 확인 |
| MetaCompiler 추가 | 1,200줄(C# — Command 생성 방식, 바인더·enum 표 생성, identity 면제, 검사) | — | — |
| 생성 스키마 문자열 | — | Game 대상 Release는 도움말·예시·값 이름 문자열 0 B | 생성 코드가 `ROCKS_BUILD_RELEASE && ROCKS_TARGET_KIND_GAME`이면 그 칸을 빈 값으로 낸다(CVar의 출시 빌드 치트 문자열 제거와 같은 방향 — [CVar](../CVarSystem.ko.md) 13.1절). Program 대상의 Release(RocksCommandlet·벤치·SDK 도구)는 도움말을 그대로 싣는다 |
| `Rocks.Tooling.Cli`(C#) | 900줄(도움말·오류 렌더러, 수·bool·enum 사용자 파서, 이름 제안, `help <명령>`, `--version`, 파서 설정) | — | — |

- **대상 종류 정의.** Rocks.Build가 Game 대상 모듈에 `ROCKS_TARGET_KIND_GAME=1`, Program 대상 모듈에 `ROCKS_TARGET_KIND_PROGRAM=1`을 넣는다. 같은 구성에서 한 모듈 object를 Game·Program 대상이 공유하면 정의가 달라 공유할 수 없다. 지금 Rocks.Build가 대상 사이 object를 공유하는지는 확인하지 못했다(20절). 공유한다면 생성 스키마 TU만 대상별로 컴파일하는 방식으로 좁힌다.
