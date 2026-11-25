# Sparks 엔진 연동 요구 목록

기준일: 2026-11-25 · 개정 3 · 상태 확인 기준 커밋: `03c13ddb`(feature/engine-runtime)

이 문서는 [Sparks 언어](SparksLanguage/README.ko.md)(이하 SL)를 엔진에 붙일 때 엔진·도구 쪽에 있어야 하는 것을 모아 둔 목록이다. Sparks는 엔진 요소와 무관한 **언어 코어**를 먼저 구현하고(SL 18.2절 LC0–LC9), 엔진 쪽이 준비되는 대로 **연동 단계**를 하나씩 더한다(3절 EI1–EI9). 언어 코어 LC0–LC7과 LC9는 이 목록의 어느 항목도 기다리지 않는다. LC8(CLI·언어 서버)만 표준 입출력 API(2.6절 SE-B7)를 기다린다. LC9(AOT)는 미루므로(SL SD77) LC8 다음은 이 목록의 항목이 갖춰진 연동 단계다.

## 1. 운영 규칙

- 항목마다 요구, 현재 상태, 확인 근거(파일), 그 항목이 필요한 연동 단계, 설계 정본을 적는다. 상태는 코드로 확인한 것만 적고, 확인 기준 커밋은 머리말에 둔다.
- 언어 코어를 구현하다 엔진에 새로 필요한 것이 드러나면 코어 안에서 우회하지 않고 이 목록에 항목을 더한다. 코어는 host 경계(SL 18.1절) 뒤의 가짜 host로 계속 진행한다.
- 엔진 쪽 작업이 끝나면 상태를 "있음(커밋)"으로 바꾼다. 한 연동 단계의 항목이 모두 "있음"이 되면 그 단계를 착수할 수 있다.
- 각 요구의 설계 정본은 해당 시스템 문서다. 이 문서는 색인이며 설계를 다시 적지 않는다.
- 상태 표기: **있음**(코드에 있다), **일부**(일부만 있다 — 빠진 것을 적는다), **없음**(코드에 없다), **설계만**(설계 문서만 있다), **확인 필요**(이번에 코드로 확인하지 못했다).

## 2. 요구 목록

### 2.1 리플렉션·메타 컴파일러

| ID | 요구 | 상태 | 확인 근거 | 단계 | 정본 |
|---|---|---|---|---|---|
| SE-R1 | 함수 리플렉션(CU2): `ROCKS_FUNCTION` 매크로, 인자·반환 schema, FunctionId 발급, 호출 thunk 생성 | 없음 | `MetaSchema/Public/Meta/Annotations.h`의 매크로는 `ROCKS_CLASS`·`ROCKS_PROPERTY`·`ROCKS_ENUM`·`ROCKS_ENUMERATOR`·`ROCKS_STRUCT`·`ROCKS_NATIVE`·`ROCKS_GENERATED_BODY`뿐 | EI1·EI2 | UiToolkit CU2, SL 10절 |
| SE-R2 | 이벤트 필드(CU3): `MulticastDelegate` 필드를 `FieldKind::Event`로 분류, 인자 이름 `Params` 지정자 | 없음 | `MetaSchema/Public/Meta/TypeSchema.h`의 `FieldKind`는 `Value`·`ObjectReference` | EI1·EI3 | UiToolkit CU3, SL 10.1절 |
| SE-R3 | 스크립트 지정자: 클래스 `ScriptExtensible`, 필드 `ScriptName`·`ScriptWritable`, 함수의 lane·효과·자원 정책·파괴 지연·`NoFatalOnInput`·초당 값 인자, `TryX` → `X` 이름 규칙 | 없음 | `Programs/Rocks.MetaCompiler/Specifiers.cs`의 지정자 목록에 없다 | EI1 | SL 10.2·10.3절 |
| SE-R4 | binding schema 출력: 메타 컴파일러가 모듈마다 `.sparkbind.json`을 생성한다(형식은 SL 10.2절 "binding schema 파일 형식", LC2a 확정 — 함수의 `overridable`(재정의 가능 가상 함수, SD65)은 LC2b, 문맥 이름의 타입 `World`·`PlaySession`·`InputSystem`·`Connection`(SL §3.10)은 LC2c, 효과·lane·자원 정책 key는 LC3 이후 더한다) | 없음 | — | EI1 | SL 10.2절 |
| SE-R5 | interface 표(CU9): `TypeInfo`의 interface 목록·slot 표, C++ `ROCKS_INTERFACE` | 없음 | 위 매크로 목록에 없다 | EI2 | SL 10.3절 |
| SE-R6 | 편집 메타(CU1): 강제 범위(Min·Max), 편집 범위·단위·편집 조건 | 일부 | 필드 지정자 `Category`·`DisplayName`·`ReadOnly`·`MaxElements`는 있다(`Specifiers.cs`). 범위·단위·조건은 없다 | EI5 | UiToolkit CU1, SL 10.1절 |
| SE-R7 | 변경 통지(CU5): native 필드의 `Changed`, 계산 속성(getter·setter 쌍) | 없음 | ObjectRuntime·MetaSchema 공개 헤더에 통지 API가 없다 | EI3 | UiToolkit CU5, SL 5.3·10.2절 |
| SE-R8 | 안정 ID 공유: Sparks 컴파일러를 `.rocksids.json`의 두 번째 발급자로 인정(모듈 경로 이름 키, subobject·function·rpc kind) | 파서 확장 완료(LC4d) | `StableIdManifest.cs`가 `subobject`·`function`·`rpc`·`enumCase` kind와 Sparks 이름 키 `<패키지>/<경로>.<선언>[.<멤버>]`(handler `on(<경로>)`)를 받는다(시험 `StableIds.SparksNamesAndKinds`). 컴파일러는 manifest를 읽어 발급 대기 목록·S1005를 내고, 발급(GUID 쓰기)은 CLI `sparks ids`(LC8)가 한다 | EI2(엔진 타입과 공유) | SL 12.2절, SparksBytecode 6.1절 |
| SE-R9 | 엔진 공통 진단 코드 등록부(S·RMC·UI 코드 대역) | 있음(문서) | CompilerDiagnostics "진단 코드 등록부" 절(LC0에서 추가) | — | SL 8.1절 |

SE-R8의 파서 확장(새 kind와 Sparks 이름 키)은 LC4d에서 함께 했다. 이제 Sparks 패키지 ID manifest를 저장소에 둘 수 있다(CI의 `ids merge-check`가 받는다 — 저장소 전체에서 ID·이름이 겹치지 않아야 한다).

### 2.2 Core·ObjectRuntime

| ID | 요구 | 상태 | 확인 근거 | 단계 | 정본 |
|---|---|---|---|---|---|
| SE-C1 | Delegate C1: 생존 판정을 밖에서 주는 회수 가능 대상(객체 handle의 generation) | 없음 | `Core/Delegate.h`의 약한 바인딩은 `SharedPtr` 대상의 `CreateWeak`·`CreateWeakLambda`뿐 | EI3 | SL 5.5절 |
| SE-C2 | Delegate C2: `TryAdd`의 상한 compact(죽은 항목 ≤ 직전 compact 때 살아 있던 수 + 8) | 없음 | `MulticastDelegate::TryAdd`는 compact를 부르지 않는다 | EI3 | SL 5.5절 |
| SE-C3 | Delegate C3: 단일 pass compact | 없음 | `Compact()`가 항목을 지울 때마다 `index = 0`으로 되돌린다(최악 O(n²)) | EI3 | SL 5.5절 |
| SE-C4 | Delegate C4: `CompactDead`·`Stats` | 없음 | 같은 파일에 없다 | EI3 | SL 5.5절 |
| SE-C5 | NamePool 검색 전용 `TryFind` | 있음 | `Core/Text/NamePool.h` | — | SL 9.5절 |
| SE-O1 | 객체 회수 전 정확히 1회 실행되는 cleanup hook | 없음 | `Object/Object.h`에 가상 hook이 없다 | EI2 | SL 5.1절, ObjectFoundations |
| SE-O2 | 타입 revision: 같은 TypeId 아래 revision 공존, instance별 revision, 경계에서 교체, 마지막 lease 반환 때 retire | 없음 | `Object/TypeRegistry.h`에는 `Register(TypeSchema, RuntimeTypeOps)`·`Unregister(token)`만 있고 revision 개념이 없다 | EI2(등록)·EI6(reload) | RuntimeClassMetadata, SL 12.4절 |
| SE-O3 | 스크립트 클래스의 런타임 타입 등록: 런타임에 만든 TypeSchema, 필드 저장소가 객체 밖(VM instance 저장소)인 필드의 접근 경로, 기본값 레코드 합성 | 일부 | 런타임 `Register` API는 있다. `RuntimeFieldOps`의 `read`·`writeStaged`는 필드별 문맥 인자가 없는 함수 포인터라 객체 밖 저장소를 가리킬 수 없다(`Object/RuntimeTypeOps.h`) | EI2 | RuntimeClassMetadata, SL 10.1절 |
| SE-O4 | 약한 객체 참조 이름 `WeakObjectRef` | 있음 | 커밋 `e0e78953` | — | SL SD23 |
| SE-C6 | lane 사이 해제용 `IntrusiveMpscQueue`(실패하지 않는 되돌림) | 없음 | `Core/Containers`에는 `BoundedMpscQueue.h`만 있다 | EI7 | Synchronization 12.2절, SL 9.4절 |

### 2.3 World

| ID | 요구 | 상태 | 확인 근거 | 단계 | 정본 |
|---|---|---|---|---|---|
| SE-W1 | 스크립트 단계 hook: 입력 batch·재개·reload 경계를 실행할 프레임 지점 | 없음 | `WorldSubsystem`의 가상 함수는 `Initialize`·`OnWorldBeginPlay`·`Deinitialize`뿐이다. `TickPhase`는 PrePhysics·Physics·PostPhysics·LateUpdate·PreRender | EI3 | EngineRuntime 8절, SL 5.4절 |
| SE-W2 | Component용 시작 준비 hook(`IsReadyToStart`) | 없음 | `World/Actor.h`에만 있고 `World/Component.h`의 가상 함수 목록에 없다 | EI3 | EngineRuntime 7.2절, SL SD24 |
| SE-W3 | 파괴·활성 전환 bridge 큐 계약, 격리 bridge 계약, 프레임 끝 정리 반복 상한 | 없음(문서) | EngineRuntime 7절에 아직 없다 | EI3 | EngineRuntime 7.4절, SL SD25·8.3절 |
| SE-W4 | `ScriptableComponent`, `WorldService` 호스트(World마다 하나인 subsystem), `LevelScript` 타입 | 없음 | World 공개 헤더에 없다 | EI3 | SL 3.4절, LevelScript |
| SE-W5 | 시작 뒤 Tick 설정 변경(reload 때 `TickSettings` 재등록) | 일부 | `Actor::SetTickEnabled`는 있다. 설정은 `OnStart` 때 한 번 등록한다 | EI6 | SL 12.4절 |
| SE-W6 | UI 시계(`Clock.UI`) | 일부 | `Core/Time/GameClock.h`에 pause·배율과 무관한 `UnscaledTimeSeconds()`는 있다. 별도 UI 시계 타입은 없다 | EI5 | SL 6.1절 |
| SE-W7 | 스트리밍 셀, `Teleport`의 prefetch, data layer, 셀 사이 영속 Actor 참조(SoftEntityId) | 없음 | World 공개 헤더에 없다 | EI4 | 전체 계획 10절, SL 7.6절 |
| SE-W8 | 이벤트 slot 표: 이벤트 필드가 없는 확장 이벤트·native `Changed`·World 이벤트·Actor 물리 이벤트를 발행자 객체에 지연 할당해 저장 | 없음 | — | EI3 | SL 5.5절 |

### 2.4 에셋·Prefab

| ID | 요구 | 상태 | 확인 근거 | 단계 | 정본 |
|---|---|---|---|---|---|
| SE-A1 | 로드 요청·lease API(LoadTicket·AssetLease 대응)와 준비 mask | 일부 | `AssetRuntime` 공개 헤더에 `AssetLoader.h`·`PackageLoad.h`는 있다. `LoadTicket`·`AssetLease`·준비 mask 타입은 없다(AssetManager 단계 몫) | EI4 | AssetSystem, SL 7절 |
| SE-A2 | 클래스 기본값 레코드와 그 참조 의존 수집 | 없음 | 기본값 레코드 자체가 코드에 없다(`World/Actor.h`·`World/Component.h` 주석: "기본값 레코드와 차이 저장이 아직 없어") | EI4 | RuntimeClassMetadata, SL 7.2절 |
| SE-A3 | Prefab PF1(클래스 원형·default subobject), instancing, `World.Spawn(prefab, at)` | 설계만 | Prefab 개정 3 | EI4·EI6 | Prefab, SL 4.5·7.6절 |
| SE-A4 | 쿡: `.sparkc`의 패키지 포함, 하드 의존 예산, 소프트 대상 chunk 배정 | 없음 | — | EI9 | SL 12.3절 |
| SE-A5 | 에셋 사전(`Asset(...)` 리터럴 대조), `DataAsset`, 데이터 테이블(`Row<T>`·`@tableRow`) | 없음 | 해당 타입·사전이 코드에 없다 | EI4 | SL 3.5·15절 |

### 2.5 게임 시스템

| ID | 요구 | 상태 | 확인 근거 | 단계 | 정본 |
|---|---|---|---|---|---|
| SE-G1 | 입력 action과 phase 이벤트(Started·Triggered·Ongoing·Completed·Cancelled), action 사전 | 없음 | `Input` 공개 헤더는 `InputEvent.h`·`InputState.h`·`InputTypes.h`뿐 | EI5 | 입력 상세안 21절, SL 10.4절 |
| SE-G2 | Tag 사전과 `TagSet` | 없음 | Core·Engine 공개 헤더에 없다 | EI5 | SL 3.5절 |
| SE-G3 | 지역화 `Text`와 catalog, `Loc` 리터럴의 컴파일 시 대조용 catalog 사전 | 일부 | `Localization/Text.h`·`Catalog.h`는 있다. 컴파일러용 사전 내보내기는 없다 | EI5 | SL 3.5절 |
| SE-G4 | CVar 사전 내보내기와 스크립트 읽기 | 일부 | `Config` 모듈에 CVar 구현은 있다. 스크립트 노출은 함수 리플렉션 뒤(CVR8) | EI5 | CVarSystem 14.5절, SL 10.4절 |
| SE-G5 | 물리 이벤트·`Collider`·`Hit`·질의 | 설계만 | 물리 모듈이 없다(PhysicsJoltAdapter) | EI5 | SL 10.4절, LevelScript 4절 |
| SE-G6 | UI `WidgetController`·WidgetAsset schema | 설계만 | UiToolkit | EI5 | SL 10.4절 |
| SE-G7 | 유지 자원을 돌려주는 API(오디오 반복·입력 context·시퀀서 등) | 설계만 | 오디오는 [오디오 시스템](AudioSystem/03-Design/03.17-Sparks.ko.md) 3.17절이 정했다: `StartLoop`의 `LoopHandle?` null은 재생 자리를 얻지 못함, 서버·`--no-sound`는 `Ignored` 상태의 유효 핸들, 스키마에 `World.Audio.Play2D` 추가(AU3b). 입력 context·시퀀서는 해당 모듈이 없다 | EI5 | SL 7.1절 |
| SE-G8 | 네트워크 복제·RPC | 없음 | 네트워크 모듈이 없다 | EI7 | SL 14절 |
| SE-G9 | 세이브게임 저장 경로 | 일부 | `SaveGame` 필드만 고르는 저장 필터는 있다(`Object/FieldCodecPolicy.h`의 `includeFlags`). 세이브 슬롯·파일과 World 저장 API는 없다 | EI7 | SL 15절 |
| SE-G10 | Tryout(편집기 내 실행) session과 편집기 Undo transaction | 없음 | — | EI6 | 전체 계획 22절 |
| SE-G11 | `PlaySession`·`Session.Travel`·`Connection` | 없음 | 코드에 없다 | EI5·EI7 | 전체 계획 20.4·22.6절, SL 3.4·7.6절 |
| SE-G12 | 게임 프레임 타입 `Pawn`·`Character` | 없음 | 코드에 없다(EngineRuntime은 Pawn까지 설계) | EI5 | EngineRuntime 2절 |
| SE-G13 | 대형 월드 좌표 타입 `WorldPosition` | 없음 | Core·Engine에 타입이 없다 | EI5 | SL 4.2절 |

### 2.6 빌드·도구·플랫폼

| ID | 요구 | 상태 | 확인 근거 | 단계 | 정본 |
|---|---|---|---|---|---|
| SE-B1 | Rocks.Build의 Sparks compile action(저장 시 reload 경로, 증분) | 없음 | `Programs/Rocks.Build`에 Sparks 항목이 없다 | EI6 | SL SD6·12.3절 |
| SE-B2 | AOT 생성 C++의 빌드 편입(빌드한 생성기를 돌려 그 출력을 컴파일하는 action) | 일부 | FP 강제는 이미 있다(`Rocks.Build/ActionGraph.cs`: 전 모듈 `-ffp-contract=off`, 모듈별 FP 인자 거부). 생성 C++ 편입은 없다 — action 종류가 Compile·Pch·Archive·Link·Copy뿐이다 | EI8 | SL 12.5절 |
| SE-B3 | 플랫폼 간 비트가 같은 초월 함수(라디안 Sin·Cos·Tan·Asin·Acos·Atan·Atan2·Exp·Ln) | 일부 | `DeterministicMath`의 초월 함수는 `SinPi`·`CosPi`·`Exp2`·`Log2`·`Pow`다. 라디안 삼각함수·역삼각함수·`Exp`·`Ln`은 없다 | EI7 | SL 6.3절 |
| SE-B4 | 명령줄 파서(AppInfoCommandLine) | 설계만 | CLI는 `Core/Commandlet` registry로 시작한다 | — | AppInfoCommandLine |
| SE-B5 | JSON5 2단계(`.json5` 파일 이름 규칙) | 일부 | reader는 있다(`Core/Text/Json.h`의 Json5 문법) | — | SL 3.4절 |
| SE-B6 | 외부 모듈 서명 봉투 | 없음 | — | EI9 | ContentSigning, SL 12.7절 |
| SE-B7 | 표준 입출력 API: 이진 표준 입력 읽기와 표준 출력 쓰기(CLI 출력, 언어 서버의 stdio 통신) | 완료(2026-11-23, LC8a) | `Platform/StandardStream.h` — `WriteStandardStream`·`ReadStandardInput`(Windows 콘솔은 UTF-16, pipe·파일은 byte 그대로, CRT stdio 없음 — SparksTools T2). 시험 `StandardStream_PipesPassBytesUnchanged`(process probe `platform-cat`). Commandlet의 전용 구현은 이 API로 옮길 수 있다(그 프로그램의 일) | **LC8** | SL 13절 |
| SE-B8 | Trace 채널 `script` 등록 | 일부 | `TraceCapture` 모듈은 있다 | EI3 | TraceSystem, SL 11절 |
| SE-B9 | target 정책 상수 `ScriptEval`·`ExternalModules`·`ScriptingBackend`·`AotOptimization` | 없음 | `Rocks.Build.Abstractions/BuildRules.cs`와 BuildTargets 문서에 없다 | EI8·EI9 | SL 12.5·12.6절 |
| SE-B10 | 디버거(DAP) 전송: 소켓과 VM 디버거 연결 | 없음 | `PlatformBase` 공개 헤더에 소켓이 없다 | EI7 | SL 13절 |
| SE-B11 | (선택) Sparks 전용 시험 실행 파일 | 없음 | Engine 층 시험은 EngineTests에만 들어가고(Graphics·Vulkan까지 링크) CI는 Foundation·Commandlet만 돌린다 | — | UnityBuildPch·빌드 규칙 |

SE-B7은 Commandlet의 표준 출력 구현을 PlatformBase로 올리고 표준 입력 읽기를 더하는 작업이다. SE-B3이 없는 동안 `Math.Sin` 등은 Core의 플랫폼 수학 함수를 부른다. VM과 AOT는 같은 함수를 부르므로 backend 사이 결과는 같고, 플랫폼 사이 비트 동일성만 보장하지 않는다.

## 3. 연동 단계

연동 단계는 순서가 아니라 묶음이다. 필요한 항목이 모두 "있음"이 된 단계부터 더한다. 게임 시스템(EI5)은 시스템마다 따로 더한다.

| 단계 | 내용 | 필요한 항목 | 먼저 끝나야 하는 언어 코어 |
|---|---|---|---|
| EI1 binding 생성 | 실제 엔진 선언에서 `.sparkbind.json`을 생성하고 컴파일러가 실제 엔진 API로 타입을 검사한다 | SE-R1·R2·R3·R4 | LC2 |
| EI2 ScriptRuntime | 모듈 `ScriptRuntime`(ObjectRuntime 위의 host 구현): Ref bank와 객체 handle 연결, native call thunk, Sparks 클래스의 타입 등록 | SE-R1·R5·R8, SE-O1·O2(등록)·O3 | LC5 |
| EI3 World bridge | `ScriptableComponent`, 수명 hook, 스크립트 단계, 이벤트 연결, 파괴·활성 지연, 격리 | SE-R2·R7, SE-C1–C4, SE-W1–W4·W8, SE-B8 | LC6 |
| EI4 에셋·생성 | `Load`·`LoadAll`, 하드·소프트 참조, `World.Spawn(prefab, at)`, 데이터 에셋·테이블, 스트리밍 | SE-A1·A2·A3·A5, SE-W7 | LC6 |
| EI5 게임 시스템 | 입력·Tag·`Loc`·CVar·물리·UI·오디오·게임 프레임 | 시스템별로 SE-G1–G7·G11–G13, SE-R6, SE-W6 | LC6 |
| EI6 편집기 핫 리로드 | 저장 → compile action → 실행 중 World 적용, R3 instance 재생성 | SE-B1, SE-O2(reload), SE-A3, SE-W5, SE-G10 | LC7 |
| EI7 네트워크·저장·결정성·디버거 | `@net`·RPC·R4, `@save`, `@deterministic`, 디버거(DAP) | SE-G8·G9·G11, SE-B3·B10, SE-C6 | LC6·LC7 |
| EI8 AOT 빌드 편입 | 생성 C++을 target 빌드에 넣는다 | SE-B2·B9 | LC9 |
| EI9 출시 | 쿡·서명·eval 정책 | SE-A4, SE-B6·B9 | LC4 |

## 4. 개정 기록

- 개정 1(2026-11-20): 첫 목록. SL 초안 1.3의 17.2절에 있던 코드 쪽 후속 항목을 옮기고 현재 상태를 코드로 확인하였다. 리뷰어 1명이 상태 표기와 "언어 코어에 밖의 선행이 없다"는 주장을 코드와 대조하였다(상태 정정 8건, 항목 추가 9건, LC4의 ID manifest와 LC8의 표준 출력 반영). 리뷰어가 확인하지 못한 것: macOS·Android backend 세부, 로그 카테고리의 실행 중 등록 가능 여부.
- 개정 2(2026-11-22): SE-G7에 오디오 설계([오디오 시스템](AudioSystem/03-Design/03.17-Sparks.ko.md) 3.17절)를 반영했다.
- 개정 3(2026-11-25): LC9(AOT)를 미룬다는 사용자 결정(SL SD77)을 머리말에 반영했다. EI8은 그대로 LC9 뒤다.
