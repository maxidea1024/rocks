## 1. 현재 상태

| 항목 | 현재 Rocks | 문제 |
|---|---|---|
| 모듈 수명 객체 | 없다. 모듈마다 시작·종료 함수가 없다 | 모듈이 자기 초기화를 둘 곳이 없어 호스트가 모듈별 등록 함수를 하나씩 부른다 |
| 시작 순서 | 실행 파일마다 `main`이 직접 조립한다. Sandbox 엔진 호스트: HostSettings → 크래시 처리 → LogHost → Trace → `Engine::Initialize`(`Programs/Sandbox/Private/EngineHost.cpp:301-391`) | 모듈이 늘면 호스트 코드가 계속 길어진다 |
| 리플렉션 등록 | 생성 집계 함수 `RegisterModuleTypes(ModuleRegistration&, const ModuleLease& = {})`(`Programs/Rocks.MetaCompiler/CppEmitter.cs:59-133`)를 호스트·래퍼가 명시 호출한다(`HostSettings.cpp:269-344`, `World.cpp:88-91`, `Templates/Game/Foundation/Source/Main.cpp:9-10`). 집계 함수는 import한 의존 모듈을 호출자의 lease로 재귀 등록한다 | 실제 `ModuleLease`를 넘기는 쪽이 없다. 모듈이 언제 등록되는지가 호출자마다 다르다 |
| 코드 수명 토큰 | `ModuleLease`(retain/release 토큰, 빈 lease는 정적 링크 코드 — `Core/Source/Runtime/Core/Public/Core/Lifetime/ModuleLease.h:7-93`)가 `RuntimeTypeOps.module`에 저장된다 | 발급자가 없다 |
| 공유 라이브러리 적재 | `platform::DynamicLibrary`(절대 경로만, `FileStatus{code, nativeError}` — `Core/Source/Runtime/PlatformBase/Public/Platform/DynamicLibrary.h:81-175`). 문서가 "엔진 모듈 로더가 아니다"라고 적는다 | POSIX는 `nativeError`가 0이고 `dlerror` 문구를 넘기지 않았다. ML0에서 `DynamicLibraryError`로 문구를 넘기도록 보강했다(3.5절) |
| 로딩 단계 | 기술자 `LoadingPhase { EarliestPossible, PostConfigInit, PreDefault, Default, PostDefault }`가 있다(`Programs/Rocks.Build/Descriptors.cs:35`). 엔진 모듈(`SourceModuleRules`)에는 단계가 없다 | 빌드가 읽기만 하고 실행 중 반영이 없다([저장소 구성](../../Guides/RepositoryLayout.ko.md) 게임 빌드 연동) |
| 설정 등록 | CVar·설정 struct는 모듈 등록 함수에서 `ConfigContext::Register`로 올린다([CVar](../CVarSystem.ko.md)). HostSettings는 `CompleteStartup` 직후 `ReportUnresolved`로 아무도 등록하지 않은 이름을 경고한다(`Core/Source/Runtime/HostConfig/Private/HostSettings.cpp:549-560`) | 모듈이 늦게 시작해 늦게 등록하면 설정 파일의 그 이름이 모두 오타 경고가 된다 |
| 빌드 식별 | `ToolchainFingerprint()`·`ToolBuildId()`(`Core/Public/Core/Build/BuildInfo.h`)는 Core가 선언하고 실행 파일마다 만드는 생성 TU가 정의한다 | 공유 라이브러리 사이 호환 식별자로 쓸 수 없다. Modular에서는 Core 공유 라이브러리에 미해결 기호가 생긴다 |
| 전역 생성자 | 금지가 원칙이다([설정 시스템](../ConfigSystem.ko.md) 129행, [CVar](../CVarSystem.ko.md) VD1). 예외는 시험 `TEST` 등록뿐이다(`TestSupport/Public/TestSupport/Test.h:13-33`) | UE식 정적 등록(전역 생성자)을 그대로 쓸 수 없다 |
| 실행 진입점 | 공통 진입점 `Launch` 모듈(`ROCKS_IMPLEMENT_APPLICATION`, 단계 실행기 `RunLaunchStages`)이 crash-launch 작업으로 진행 중이다(설계 `CrashHandlingAndLaunch.ko.md` 개정 3, 미병합) | 로더를 만들고 소유할 자리가 그 안에 있어야 한다 |
| 한 프로세스의 Engine 수 | 시험은 한 프로세스에서 `Engine`을 차례로 여러 번 만든다(`Engine/Source/Runtime/Engine/Tests/EngineTests.cpp`) | 엔진 단계 모듈의 시작·종료가 Engine 수명에 맞아야 한다 |
