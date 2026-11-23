## 6. 단계

각 단계는 Debug 빌드, format, policy, 바뀐 부분의 시험(`Rocks.Build.Tests`·`Rocks.Build.IntegrationTests`·`Rocks.ToolTests`)으로 검증한다.

| 단계 | 내용 | 완료 기준 |
|---|---|---|
| BS0 | 의존 두 목록(3.5절): `NativeModuleInput`·규칙 기반 클래스·ActionGraph 전파·순환 검사·Closures, MetadataRunner·MetadataLayout 범위, SDK manifest(`SdkModule`·`Reachable`·검증), DescriptorModules·ReleaseExclusion 도달 검사. 기존 모듈은 모두 공개 의존으로 옮긴다 | 모든 타깃의 빌드 결과(object 인자)가 전환 전과 같다 |
| BS1 | BS1a: 모듈별 공개 헤더 검사(BD9 — 검사 모듈 `PublicHeaderChecks_<M>`). BS1b: include 소유 모듈 검사(BD11, depfile — UB0 파서), 시험 소스 묶음(BD22 개정 27), 쓰지 않는 공개 의존 진단 | 공개 헤더가 의존하지 않은 모듈의 헤더를 include하면 검사 컴파일이 실패한다. 소스가 보이지 않는 모듈의 헤더를 include하면 빌드가 실패한다(시험 소스는 대상 모듈 환경 밖 헤더). 쓰지 않는 공개 의존 알림 |
| BS2 | 타깃 종류 다섯 값·플랫폼 표·`build::TargetType`·네 매크로와 주입 위치(3.2절 목록)·`Base/Compiler.h` 검사·`CompileWithEditor`·`WithEditorData`·모듈 type ServerOnly·ClientOnly와 `SourceModuleRules.ModuleType`·SDK manifest type, Release 배제 정책 확장과 타깃 종류별 평가 플랫폼(BD20), type 사이 의존 규칙(BD25), 메타 컴파일러 어휘 스캔·두 조합 비교·조건부 생성·합집합 descriptor·`EditorOnly` FieldId 목록(BD6), 에디터 패키지 저장 거부(BD26), 기존 fixture의 `EditorOnly` 필드 이동 | 타깃 종류별 모듈 포함 표와 매크로 값을 시험이 확인한다. 허용되지 않은 조건 블록 안 리플렉션 선언, 블록 안 타입 선언, 블록 밖 `EditorOnly`를 거부한다. `ROCKS_WITH_EDITOR_DATA=0` 빌드의 에디터 패키지 저장이 Unsupported다. `ROCKS_WITH_EDITOR_DATA` 0·1 조합 시험 빌드가 통과한다 |
| BS3 | 서드파티 모듈 전환(BD21), 기존 모듈의 공개 의존을 비공개로 내리기 | 쓰지 않는 공개 의존 진단 0, `Consumer*` 우회 0, 빌드 통과 |
| BS4 | 플러그인 활성 조건(3.6절): 기술자 읽기·값 검사, 처리 순서 분리(BD24), 문맥별 해석, 플러그인 `platforms` | 조건 조합 시험(타깃·플랫폼·구성), `enabledByDefault` 플러그인의 Editor 전용 켜기, 미지원 플랫폼 조용히 빼기와 명시 오류 시험 |
| BS5 | Editor 타깃 첫 실행 파일 — Rocks Studio 착수 시점. 선행: BS8, 모듈 로더 스펙과 구현(BD30). 설치 SDK Editor 변형(`Modular`), 게임 Editor 타깃의 공유 빌드 환경 검사 | Rocks Studio 실행 파일이 엔진 모듈 공유 라이브러리와 게임·플러그인 모듈을 적재해 부트·종료한다(Windows·macOS) |
| BS6 | Linux x64 빌드 플랫폼(3.7절 2): `NativePlatform.Linux`, clang++·lld 툴체인 탐색과 fingerprint, 플랫폼 매크로, `Private/Linux/` 규칙, 정책 검사의 플랫폼 폴더 목록, Linux에서 허용 타깃 제한 | Core 모듈과 FoundationTests가 Linux x64에서 빌드·통과(전체 계획 M3 "Core CPU/headless suite") |
| BS7 | Server 타깃 첫 실행 파일(3.7절 1·3·4) — 네트워크 단계: 게임 템플릿의 Server 타깃 규칙, 그래픽 backend ClientOnly 표시와 비링크 검사, Android Server 타깃 거부, headless 부트·종료 | Windows·macOS·Linux에서 Server 실행 파일이 부트·종료한다(전체 계획 M4 "server service boot/shutdown") |
| BS8 | 모듈식 링크 기반(3.8절) — 선행: BS0. `LinkType`·`ROCKS_MONOLITHIC`·Android 거부·파일 이름, 모듈 API 매크로 주입과 `ROCKS_DLLEXPORT`·`ROCKS_DLLIMPORT`, 숨김 가시성, 예약 매크로 검사, 가시성·`-fPIC`·rpath·install name, 생성 `.cpp`와 `NativeTypeKey` 교체(BD28), 상속 export 진단과 모듈별 export 수 진단, 기존 모듈의 모듈 사이 API에 매크로 붙이기, 정책 `public-header-static-storage`와 지금 위반 정리, `HasGlobalState` 표시와 단일 소비 검사, 시험용 내부 export와 `Modular` FoundationTests 검사 체인(BD31 개정 22), 실행 파일 생성 TU가 정의하던 빌드 정보(`ToolchainFingerprint`·`ToolBuildId`)의 Modular 정의 위치(Core 공유 라이브러리에 미해결 기호가 남지 않게) | `Modular` FoundationTests가 Windows·macOS에서 빌드·통과한다. `Monolithic` 빌드 결과(object 인자)는 매크로 값 외에 바뀌지 않는다. 공개 헤더의 `thread_local`·쓰기 가능한 inline 정적 저장소를 정책이 거부한다 |
| BS9a | 엔진 플러그인 해석(3.9절): `ResolveEnabled`·`Resolve` 소유자 일반화, `targetType` 인자, BD34·BD35, `TargetRules.Plugins`, `BuildContext.Plugins`, RuleLoader 탐색 | 기술자 시험(엔진 소유자 해석, 타깃 참조 우선, Program 기본 활성 미적용, Developer의 Program Release 포함·Game Release 제외, 없는 참조·끈 플러그인 요구·순환 오류), 게임 빌드 동작 불변(Program Release의 Developer만 바뀜) |
| BS9b | `PluginModuleRules`, `NativeModuleInput.Type`, Release 배제 정책 전환, BD39 계층 검사, BD42 정책 | 규칙 시험(디렉터리 일치, 선언 없는 규칙·규칙 없는 선언·대소문자 이름 오류, 끈 플러그인 규칙 Configure 성공), 계층 위반 시험, 플러그인 Developer 모듈을 Release 정책이 잡는 음성 대조 |
| BS9c | 엔진 타깃 연결(CLI), BD36 링크·모듈 표 행(게임 빌드 포함), BD41 거부 | 의존 없는 주 실행 파일에 켠 플러그인 모듈이 링크되고 기술자 단계로 시작, probe에는 없음, 끈 플러그인 모듈은 빌드되지 않음, 실행 파일 없는 타깃은 빌드만, SDK export·Modular 거부 시험 |
| BS9d | 첫 엔진 플러그인 `Engine/Plugins/Importers/UnrealExchange`(기술자와 진입점만 있는 모듈), Commandlet·EngineTests가 켬, 플러그인 시험 수집(BD40) | EngineTests가 플러그인 Tests를 실행, 다른 타깃에는 모듈 없음 — Unreal 교환 UX1a 선행 조건 충족 |

순서: BS0 → BS1(UB0 depfile 파서 뒤) → BS3. BS2는 BS0 뒤가 쉽다(규칙 기반 클래스를 한 번에 고친다). BS4는 BS2 뒤다(`targets` 값 검증과 문맥별 해석에 다섯 값 `TargetType`이 필요하다). BS6은 다른 단계와 독립이다. BS7은 BS2·BS6 뒤이고, 렌더·오디오·입력 backend를 서버에서 빼는 범위는 BS7 전에 네트워크 단계 스펙이 정한다. BS9a → BS9b → BS9c → BS9d는 BS2·BS4와 독립이다(타깃 종류 두 값, 프로젝트 없는 엔진 타깃만으로 동작). BS2·BS4가 나중에 종류 표와 조건 필드를 넓힌다.
