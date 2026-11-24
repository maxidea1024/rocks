## 9. 범위 밖과 채택하지 않은 UE 요소

### 9.1 채택하지 않은 UE 요소

| UE 5.8 요소 | 판단 |
|---|---|
| `PostLoadCallback`, `LoadModuleWithCallback`, `UnloadOrAbandonModuleWithCallback`, `AbandonModule` | hot reload용이다(9.2절) |
| `IsGameModule`, `IMPLEMENT_GAME_MODULE`, `IMPLEMENT_PRIMARY_GAME_MODULE` | 게임 모듈 표시와 게임 이름 등록이다. Rocks는 프로젝트 이름을 기술자·빌드 정보로 안다 |
| `UE_MERGED_MODULES`(여러 모듈을 라이브러리 하나로) | 실험 기능이다. 지연 해제 방식만 3.7절에서 빌렸다 |
| `PER_MODULE_BOILERPLATE`의 `operator new` 교체 | Rocks는 전역 `operator new`를 바꾸지 않는다([빌드 타깃](../BuildTargets/README.ko.md) BD29) |
| `-ModulesBootstrap=`(경로 캐시 직렬화) | 폴더를 훑지 않으므로 필요 없다 |
| 시작 화면·로딩 화면 단계(`PostSplashScreen`·`PreEarlyLoadingScreen`·`PreLoadingScreen`) | 대응하는 시작 흐름이 생기면 다시 본다 |
| `IsLoadedInCurrentConfiguration`(실행 중 host type 필터) | 빌드가 타깃에 들어갈 모듈을 이미 거른다([빌드 타깃](../BuildTargets/README.ko.md) BD4·BD25). manifest에 있는 모듈은 그 타깃에서 쓸 수 있다 |
| 콘솔 명령 `Module Load/Unload/Reload` | 편집기 도구가 생기면 다시 본다 |

### 9.2 범위 밖

- 코드 다시 불러오기(UE Live Coding·hot reload): Modular 적재(ML4)가 생긴 뒤 별도 스펙.
- Rocks Studio의 다시 빌드 안내 UI: Rocks Studio 설계.
- HostSettings와 엔진 레지스트리의 통합: [설정 시스템](../ConfigSystem.ko.md)·[엔진 런타임](../EngineRuntime/README.ko.md).
- 플러그인 C 함수 표 ABI([전체 계획](../EngineImplementationPlan.ko.md) 2426행 "versioned C function table"): 바이너리 배포 플러그인용이다. 이 문서는 같은 빌드 계열에서 나온 모듈만 다룬다(`ModuleBuildId` 일치).
