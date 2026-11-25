## 5. 결정 사항

| ID | 결정 | 상태 |
|---|---|---|
| MD1 | 모듈 수명 객체 `ModuleImplementation`(`Startup`·`PreUnload`·`Shutdown`·`SupportsAutomaticShutdown`·`SupportsUnload`). `Startup`은 `Result<void>`이고 실패 전에 부분 등록을 스스로 되돌린다. 문맥 `ModuleStartContext`(로더·단계·lease·`HostServices`). 선택 정적 함수 `RegisterSettings`. `PostLoadCallback`·`IsGameModule`은 두지 않는다 | 권장안 |
| MD2 | 진입점 매크로 `ROCKS_IMPLEMENT_MODULE(Class, Name)`은 상수 `ModuleEntry`(`entryVersion`·이름·`create`·타입 등록·설정 등록 — 타입 해제 칸은 ML2에서 뺐다)와 `RocksModuleEntry_<Name>()`을 정의한다. Monolithic은 빌드 생성 모듈 표(의존 순서·단계 포함), Modular는 export `RocksModuleEntry()` 하나와 강제 참조 링크. 전역 생성자를 쓰지 않는다. 이름 불일치는 `static_assert`. header-only·서드파티·Core 아래 모듈은 면제, 실행 파일 모듈은 포함(개정 2) | 권장안 |
| MD3 | 로더는 새 Core 런타임 모듈 `ModuleLoader`(의존 Core·PlatformBase)다. 타입 시스템을 모른다. 진입점 계약(수명 객체 기반·진입 기술·매크로·모듈 표 형식)은 모든 모듈이 쓰도록 Core에 둔다(개정 2) | 권장안 |
| MD4 | 로더는 프로세스에 하나이고 Launch `ApplicationContext`가 소유한다. 전역 접근자는 없다. 부트 모듈(Launch 의존 폐포)은 로더 생성 때 시작하고, 단계는 `EarliestPossible`로 강제하며, 해제할 수 없다 | 권장안 |
| MD5 | 기술자 `LoadingPhase`에 `PostEngineInit`·`None`을 더하고 엔진 모듈 규칙에도 `LoadingPhase`(기본 `Default`)를 둔다. 시작 화면·로딩 화면 단계는 두지 않는다 | 권장안 |
| MD6 | 의존 모듈의 단계는 같거나 더 이르거나 `None`이어야 한다(빌드 검사). `StartPhase`는 돌지 않은 앞 단계를 차례로 돌고, 이미 돈 단계는 성공으로 넘기며, 앞 묶음 단계 호출은 계약 위반이다. 단계 안 첫 실패에서 멈춘다 | 권장안 |
| MD7 | `Load`는 manifest 의존을 먼저 의존 순서로 시작한다. 시작 중인 모듈의 재적재는 `Cycle`이다. 적재·해제는 로더 스레드에서만, 조회는 어느 스레드에서든 `Started` 모듈만. `Failed`·실행 중 해제한 모듈은 다시 적재할 수 있다 | 권장안 |
| MD8 | 적재 결과는 `ModuleLoadReport`(코드·모듈·처음 실패한 모듈·`nativeError`·단계·문구), 해제 결과는 `ModuleUnloadReport`다. 로더는 기록하지 않고 실패를 확정하는 계층이 한 번 기록한다 | 권장안 |
| MD9 | 단계는 프로세스·호스트·엔진 묶음으로 나누고, 묶음마다 UE 절차(자동 종료 대상의 PreUnload 전부 → Shutdown 역순, 타입 해제, 공유 라이브러리는 해제하지 않음)로 내린다. 엔진 묶음은 `Engine::Shutdown`에서 TypeRegistry 종료 전, 호스트 묶음은 LogHost·HostSettings 정리 전, 프로세스 묶음은 Launch 정리에서 도구 로그 정리 전. 엔진 묶음은 Engine 수명에 묶이고 동시에 여럿이면 참조 수로 센다. 종료 절차에서는 `Changed`를 내지 않는다 | 권장안 |
| MD10 | 실행 중 해제는 먼저 `SupportsUnload`·부트 모듈·의존 모듈·타입 해제 가능 여부를 검사해 `Unsupported`·`Busy`로 거부한다(바뀌는 것 없음). 내린 뒤 lease가 남으면 공유 라이브러리 해제를 미룬다. 플러그인 끄기는 전체를 먼저 검사한다 | 권장안 |
| MD11 | 모듈 규칙 `DynamicModules`(UE `DynamicallyLoadedModuleNames`): Modular에서 링크하지 않고 manifest에만 싣는다. Monolithic은 링크한다. type 포함 규칙은 적용하고 단계 순서 검사는 적용하지 않는다. 순환 검사의 참조 사슬에는 넣지 않는다(UE와 같다 — 개정 4, 3.8절). `Dependencies`와 겹치거나 자기 자신이거나 모듈 표에 행이 없는 모듈이면 오류다. `DynamicModules`로만 표에 든 행은 단계 실행 대상이 아니다(UE `None`과 같다 — 개정 4 리뷰) | 권장안 |
| MD12 | Modular manifest `<Target>[-<Platform>-<Configuration>].modules.json`(`buildId`·`modules`의 `file`·`size`·`dependencies`·`loadingPhase`), 링크 성공 뒤 원자적으로 쓴다. 엔진·게임·플러그인 `Binaries/<Platform>/`의 manifest만 읽고 폴더를 훑지 않는다(엔진 저장소 플러그인 폴더는 MD23). 모르는 키는 오류 | 권장안 |
| MD13 | `ModuleBuildId`는 UE처럼 출력 폴더의 이전 값을 재사용하고 엔진 버전·툴체인 fingerprint가 바뀌면 새로 만든다. manifest에만 둔다(코드에 넣지 않음). 설치 SDK manifest에 실린다. 다르면 `Incompatible`, 파일 크기가 manifest와 다르면 `Incompatible` | 권장안 |
| MD14 | 의존을 먼저 적재하므로 OS 탐색은 같은 폴더만 쓴다. Linux는 `-soname`을 넣는다. POSIX `dlerror` 문구를 report로 넘기도록 `DynamicLibrary`를 보강한다 | 권장안 |
| MD15 | 타입 등록은 레지스트리 주인이 `ModuleTypeRegistrar`(`Register`·`CanUnregister`·`Unregister`)로 한다. 동시에 사는 Engine마다 등록기를 붙일 수 있다(최대 4개 — 개정 5, 3.11절). 엔진은 모듈마다 `ModuleRegistration`을 두고, 등록기를 `Engine::Initialize`에서 설정해 이미 시작한 모듈을 몰아서 등록하며 `Engine::Shutdown`에서 뗀다. 메타 컴파일러는 모듈 자신의 타입만 등록하는 함수를 더 낸다. HostSettings 레지스트리는 지금처럼 명시 등록 | 권장안 |
| MD16 | 로더가 모듈마다 실제 `ModuleLease`(원자적 수)를 발급하고, 생성 등록 함수와 모듈 코드를 가리키는 등록(CVar·trace 채널·delegate·서브시스템 클래스)이 그것을 잡는다. lease는 공유 라이브러리 해제 시점만 정한다 | 권장안 |
| MD17 | 모듈 이름은 대소문자를 구분해 비교하고, 대소문자만 다른 이름은 빌드가 거부한다 | 권장안 |
| MD18 | 형식 있는 조회는 `Find<T>(name)`이고 UE `GetModuleChecked<T>`처럼 검사 없는 `static_cast`다. `HostServices` 형식 키는 소유 모듈 cpp에 out-of-line으로 정의해 export한다. 키 형식·매크로는 Core에 둔다(개정 6 — Core의 `LogSystem`도 서비스가 되게) | 권장안 |
| MD19 | Rocks Studio는 프로젝트 기술자·플러그인 조건으로 켤 모듈을 정하고, 빌드가 게임 manifest에 실은 플러그인 목록과 다르면 `Incompatible`이다 | 권장안 |
| MD20 | 시험·도구 실행 파일도 모듈 표를 가지고 로더로 모듈을 시작한다. 시험 `TEST` 정적 등록은 예외를 유지하고, Modular에서 등록 목록 머리는 시험 호스트 모듈에 한 벌(BS8d5), 시험 묶음 공유 라이브러리는 해제하지 않는다 | 권장안 |
| MD21 | Modular 실행 파일은 부트 모듈만 import하고, 본문은 타깃의 호스트 모듈에 두어 로더가 적재한다. 부트 모듈 누락만 report 계약의 예외다 | 권장안 |
| MD22 | 로더는 시작 전에 manifest의 모든 모듈 바이너리를 준비(`Prepare`)한다. HostSettings는 설정 확정 전에 모든 모듈의 `registerSettings`를 의존 순서로 부른다 | 권장안 |
| MD23 | 켠 플러그인의 모듈 공유 라이브러리와 플러그인 manifest는 `<출력>/Plugins/<플러그인 상대 경로>/Binaries/<Platform>/`에 두고, 실행 파일 manifest가 플러그인 manifest 목록(`plugins`)을 싣는다. Modular 호스트는 켠 플러그인 모듈을 링크하지 않는다. 로더는 그 목록만 읽고 manifest 사이 `buildId`·엔진 버전·툴체인 지문이 다르면 실패하며, 합친 행을 빌드가 Monolithic 표 순서로 매긴 `order`로 정렬한다(3.15절). 버린 안: 원본 트리의 플러그인 `Binaries`, 엔진 `bin`에 함께 두기, 로더의 위상 정렬 | 권장안 |
