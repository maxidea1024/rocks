# 앱 정보와 명령줄 — 2. 범위

[목차](README.ko.md)

## 2. 범위

**범위 안**
- UE `FApp`·`FAppTime`에 대응하는 프로세스 정보와 프레임 시간
- UE `FCommandLine`·`FParse`·`UCommandlet::ParseCommandLine`에 대응하는 명령줄 보관과 파서
- 명령줄 도구(RocksCommandlet, Sandbox, ShaderToolSpike, 벤치, FoundationTests)와 C# 도구(Rocks.Build.Cli, Rocks.MetaCompiler, Rocks.ToolTests)의 인자 처리 통일
- 빌드 정보 주입(Rocks.Build)

**범위 밖**
- UE `FGenericApplication`(OS 창·메시지·포커스 원천). [UI 툴킷](../UiToolkit.ko.md) 3절의 `ui::Platform` 계약과 PlatformWindow 모듈이 맡는다. 이 문서는 포커스 상태를 어디서 읽는지만 정한다(8.6절).
- UE `FEngineLoop` 구조 자체(PreInit·Init·Tick 순서). Rocks 엔진 루프는 [엔진 런타임](../EngineRuntime/README.ko.md)과 `Engine::RunFrame`이 정한다. 이 문서는 부트 순서에 끼우는 지점만 적는다(8.3절).
- UE Zen 저장소 프로젝트 ID(`GetZenStoreProjectId`). Rocks 파생 데이터 캐시는 에셋 너겟이고 Zen 대응이 없다.
- UE 원격 세션 도구의 사용자 허가 목록(`AuthorizeUser`·`DenyUser`). 대응하는 원격 세션 도구가 없다.
- 오디오 볼륨 배수와 VR 포커스. 오디오는 [오디오 시스템](../AudioSystem/README.ko.md)이 정했고(8.6절), XR은 그 설계 때 정한다.
