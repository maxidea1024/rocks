## 2. 용어 대응

| UE 5.8 | Rocks | 비고 |
|---|---|---|
| `IModuleInterface` | `ModuleImplementation` | 모듈 수명 객체의 기반 클래스 |
| `FDefaultModuleImpl` | `DefaultModuleImplementation` | 빈 구현 |
| `IMPLEMENT_MODULE(Class, Name)` | `ROCKS_IMPLEMENT_MODULE(Class, Name)` | 모듈 진입점 매크로 |
| `FModuleManager` | `ModuleLoader` | 프로세스에 하나, Launch가 소유 |
| `FModuleInfo` | `ModuleRecord` | 모듈 하나의 상태 |
| `EModuleLoadResult` | `ModuleLoadCode` + `ModuleLoadReport` | 원래 OS 오류 코드를 담는 report |
| `*.modules`(`FModuleManifest`) | `<Target>.modules.json`(`ModuleManifest`) | 모듈 이름 → 파일·의존·단계 |
| `BuildId` | `ModuleBuildId` | 같은 빌드 계열의 바이너리인지 판정 |
| `ELoadingPhase` | `LoadingPhase` | 기술자에 이미 있다 |
| `OnModulesChanged` | `ModuleLoader::Changed` | Core Delegate |
| `ProcessLoadedObjectsCallback` | 타입 등록기(`ModuleTypeRegistrar`) | 3.6절 |
| `LoadModuleBinaryOnly`·`bPreloadedBinaries` | `ModuleLoader::Prepare` | 3.5절, 설정 등록에 쓴다 |
| `DynamicallyLoadedModuleNames` | 모듈 규칙 `DynamicModules` | 3.8절 |
