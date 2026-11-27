# 앱 정보와 명령줄 — 18. 구현 단계

[목차](README.ko.md)

## 18. 구현 단계

단계 기호는 AP(앱 정보)와 CL(명령줄)이다. 기존 문서의 기호(CV·CU·CF·CVR·TR·LG·RB 등)와 겹치지 않는다. 교차검증은 단계마다 리뷰어 1명이 한다(구현 → Debug·format·policy·바뀐 부분 시험 → 리뷰 → 반영). 스펙·문서 관점 리뷰는 묶음 끝 CL7에서 한 번 한다. 착수 시점은 머리말대로 사용자가 정한다.

| 단계 | 범위 | 선행 | 규모 |
|---|---|---|---|
| AP1 | Core `AppInfo`·`RunTraits`·`HostKind`·`AppInfoScope`. 개정 3: `ProcessCommandLine`(Windows `wmain`·`wWinMain`·`GetModuleFileNameW`), PlatformBase 대화상자 억제 API(`SuppressSystemErrorDialogs`), Sandbox·RocksCommandlet·FoundationTests 진입점 연결은 크래시 처리 설계 LC3·LC4가 Launch 진입점으로 먼저 했다. AP1에 남는 일: Launch `ApplicationKind`를 `HostKind`로 합치고, `ApplicationSettings::suppressSystemErrorDialogs`를 `RunTraits::unattended`에서 정한다. 파서가 아직 없으므로 `RunTraits`는 호스트 기본값(시험 실행기는 `unattended = true`, 나머지는 모두 기본값)으로 채우고, 지금 손 파싱하는 인자는 그대로 둔다 | — | M |
| CL1 | MetaCompiler: `Command(...)`·`Group`·PROPERTY 키(13.3절)·문자열 목록 값, Command 생성 방식(스키마·필드별 바인더·기본값 출력·범위 검사·`ValidateCommand` 호출), 명령줄 enum 이름 표, identity 면제, Command 키 사용 위치 검사, descriptor 왕복, 실행 파일 모듈 `Metadata` 경로, RMC 진단. Rocks.Build 대상 종류 정의 `ROCKS_TARGET_KIND_*` | — | M |
| CL2 | Core 런타임 `CommandLine` 모듈: `CommandLineSchema.h`, 토큰화·매칭·생성 바인더 호출·필수·범위·`ValidateCommand`·진단·이름 제안·알 수 없는 인자 정책·`Remaining`·로그용 가리기·짧은 사용법 줄, Core `EditDistance`, Base `Guid` 텍스트 변환, 크기 측정 | CL1, AP1 | M |
| AP2 | Rocks.Build `<exe>.BuildStamp.cpp`·`build::Description()`·`ROCKS_BUILD_BRANCH`, `ToolBuildId` 입력에서 제외, 시작 로그 묶음(D14 — 가린 명령줄은 CL2의 `RedactedArguments`) | AP1, CL2 | M |
| CL3 | Developer `CommandLineTools`: 출력 규약 렌더러(도움말·오류)·응답 파일·`CommandSet`(`AddGroup` 포함)·`--version`, PlatformBase `TerminalColumns`, C++ 출력 규약 golden | CL2, AP2 | M |
| CL4 | 이전: HostConfig에 리플렉션 헤더(표지)와 `HostCommandLineOptions`(`--set`·`--config`·`--trace`·`--project`·실행 성격), 부트 순서(8.3절 — AppInfo 3단계), `HostSettings::ApplyCommandLine`·`TraceController::ApplyCommandLine` 입력 변경. RocksCommandlet 명령 넷과 묶음 `log`, `CommandletRegistry`·`CommandletOutput` 삭제, Config의 `RegisterConfigDumpCommandlet`을 `Programs/Commandlet`으로 옮김(덤프 본체는 Config에 남김). 실행 파일 등록 변경: Sandbox·ShaderToolSpike·벤치 넷·FoundationTests의 Build.cs에 `ModuleDirectory`와 리플렉션 헤더 폴더의 include 디렉터리(identity 면제 — 헤더는 빌드가 표지로 찾는다, [리플렉션·빌드 보강](../ReflectionFoundations.ko.md) 1.10절), FoundationTests `process-probe`는 `Remaining`. Sandbox 고정 delta 상수를 지우고 `--deterministic`으로, 자체 시험 자식에 `--deterministic`을 넘김 | CL3 | M |
| CL5 | C#: `Directory.Packages.props`·lock 파일·`NuGetAudit=false`·`Programs/ThirdPartyPackages.json`·허용 목록 policy·`BuildEvidence` 루트 목록, 새 어셈블리 `Rocks.Tooling.Cli`(`RocksCli` — 출력 규약 렌더러·파서 설정·사용자 파서), Rocks.Build.Cli·MetaCompiler(`ids` 포함)·ToolTests(`scaffold` 포함) 이전, 설치 SDK에 DLL 동봉 | — | M |
| CL6 | 양쪽 같음 golden 시험(17절) | CL3, CL5 | S |
| AP3 | `FrameTiming`(EngineTime 대체)·`LatestFrameIndex`, 렌더 packet 칸(RB1과 함께), worker 상속(CVar의 lane 묶음 장치에 `FrameTiming`을 넣는다), `Engine::IsForeground()` | AP1, RB1, CVR 묶음 장치 | M |
| AP4 | 실행 성격 소비처: 엔진 고정 step·시드, unattended 입력 장치 정책, `app.info` 콘솔 명령 | AP1, CL4 | S |
| CL7 | 문서 반영(설정 계층 5.4절 부트 순서, CVar 콘솔 영역 `app`과 12절 편집 거리 함수 이름, 로깅 시작 기록, 리플렉션 보강의 지정자 표와 identity 면제, [셰이더 시스템](../ShaderSystem/README.ko.md) 4·5.2절(나누기 전 96·147행)과 [에셋 시스템](../AssetSystem/README.ko.md) 37행의 `CommandletRegistry` → `CommandSet`, 구현 문서), 묶음 끝 리뷰 | 전부 | S |
