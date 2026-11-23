# 엔진 런타임·월드·그래픽 최소 경로 설계

기준일: 2026-11-12. 2026-11-23 갱신: 2절 명칭에 PlayerState([컴포넌트 도입](ComponentCatalog/09-Plugins.ko.md) CR48), 3절 `Gameplay` 행 상태, 7.3절 Component 보강 예정 항목, 8절 Tick 단계의 UE 대응 결정(물리 결과 반영·skeletal mesh 물리 마무리 엔진 단계, [컴포넌트 도입](ComponentCatalog/04-Prerequisites.ko.md) CR1·CR2·CR8a·CR26·CD11). 2026-11-22 갱신: 6절 종료 게이트([오디오 시스템](AudioSystem.ko.md) AD23). 2026-11-20 갱신: 4.6절 Level `levelScript` 필드·레벨 편입 순서·레벨 저장 검증, 7.2절 Level 파괴 순서와 LevelScript 파괴 규칙([레벨 스크립트](LevelScript.ko.md) LD4–LD8·LD16·LD17). 2026-11-19 갱신: 7.4절 우회 파괴 복구 비용 측정, GC 일정(STW 병렬 전체 수집 요청·주기, 회수 예산, 엔진 소유 GC scheduler — GC 설계 17절). 2026-11-18 갱신: 4.5절 위치 교체 제거, 7.2·7.4절 Level 배열·우회 파괴 복구(매 프레임, Level·Component 포함, 근거 정정)·encode 고정 계약·시작 큐, 4.6절 편입의 배열 위치·Component 색인, 7.5절 World 소스 파일 구성, 8절 증분 틱 순서(GC 실험 [16절](../Implementation/GcScaleExperiment.ko.md) 선형 검색 점검). 이 문서는 Core·Object 위에 처음 올리는 엔진 계층의 구조와 계약을 정한다. 범위는 **엔진 틱 → 월드 → Actor/Component → 텍스처 에셋 → D3D12·OpenGL로 화면에 그리기**까지의 수직 경로와, 그 위에 올릴 게임플레이 프레임워크(GameInstance·GameDirector·GameState·Controller·Pawn)의 골격이다. 전체 계획은 [엔진 구현 계획](EngineImplementationPlan.ko.md)을 따르며, 이 문서와 충돌하는 부분은 개정 1.8에서 이 문서 기준으로 바꾼다.

## 1. 범위와 단계

| 단계 | 내용 | 완료 기준 |
|---|---|---|
| S0 | 엔진 모듈 빌드 규칙, 테스트 타깃(`Engine`), 실행 타깃(`Sandbox`) | 빈 Engine 모듈이 빌드되고 테스트 러너가 실행됨 |
| S1 | Core 보강: 게임 시계·스텝 계획, `Resolve<T>` 상속 허용, 네이티브 클래스 등록 | 시간 TK01–TK03 상당 테스트, 상속 Resolve 테스트 |
| S2 | `Engine` 상태 기계·메인 루프·EngineSubsystem | 헤드리스 N프레임 실행, 초기화 실패 역순 정리, 중복 종료 |
| S3 | World·Actor·Component·SceneComponent·지연 파괴·Tick 등록 | Spawn·Tick 중 Destroy·Transform 계층·복수 World 격리 |
| S4 | `PlatformWindow`(Win32 창·이벤트·키보드·마우스·XInput 게임패드)와 `Input`(프레임 입력 상태) | 창 닫기로 `RequestExit` 정상 종료, 헤드리스 입력 주입 테스트, Sandbox에서 마우스·키보드 반응 확인 |
| S5 | `Graphics` 계약과 `D3D12Graphics`·`OpenGLGraphics`, Vulkan·Metal 자리 | 삼각형 렌더, D3D12 offscreen readback 검사 |
| S6 | `AssetRuntime`·`AssetTools`(BMP 임포트)·텍스처 그리기 | BMP → 에셋 파일 → GPU 텍스처 → 화면의 사각형 |
| S7–S14 | Tick 의존성 완성, Subsystem 의존성, Actor 수명 완성, GameInstance, GameDirector·GameState·PlayerState, Controller·Pawn, 초기화 흐름, 진단·문서 | 붙임 계획 24절의 14단계 시나리오와 Director 없는 World 시나리오 |

S4·S5는 S2·S3과 독립이다. 이번 범위에서 macOS는 창·Metal 구현이 없으므로 빌드·실행 검증을 하지 않는다.

## 2. 명칭

- 게임 규칙 조율 객체는 **GameDirector**, 진행 상태는 **GameState**, 플레이어별 상태는 **PlayerState**, 제어 주체는 **Controller/PlayerController**, 제어 대상은 **Pawn**, 월드 배치 객체는 **Actor**다. 기존 계획 20.4·20.5절의 MatchRules·MatchState·PlayerAgent·ControlledEntity·Entity 명칭은 이 이름으로 대체한다. PlayerState는 계획 20.5절과 같은 이름이다(UE `APlayerState` 대응 — possession·재접속을 넘어 유지되는 플레이어 상태, 모든 참여자에게 복제).
- 그래픽 장치 추상화는 **Graphics**(`rocks::graphics`)다. 타입에 접두사를 붙이지 않는다(`graphics::Device`, `graphics::SwapChain`). 기존 계획의 "RHI" 표기는 Graphics로 바꾼다.
- 수명 hook은 계획 20.2절(2026-11-12 결정)을 따른다: `OnAwake`(Initialize), `OnEnable`, `OnStart`(BeginPlay), `OnTick`·`OnFixedTick`·`OnLateTick`, `OnDisable`, `OnDestroy`(EndPlay).
- Tick phase는 `PrePhysics`, `Physics`, `PostPhysics`, `LateUpdate`, `PreRender`(실행 순서)다. `Physics` phase만 고정 스텝으로 실행한다.
- 편집 World를 복제해 에디터 안에서 실행하는 기능은 **Tryout**이다(UE PIE 대응, 2026-11-19 결정). 뜻·표기 규칙·변형 이름(Tryout · Simulate, Tryout · Standalone)은 계획 22.6절을 따른다.

## 3. 모듈과 의존 방향

모든 엔진 모듈은 `Engine/Source/<Group>/<Module>/`에 두고 `EngineModuleRules`를 상속한다.

| 모듈 | 의존 | 책임 | 금지 |
|---|---|---|---|
| `Input` | Core | 장치 독립 입력 이벤트·프레임 입력 상태(키·마우스·게임패드), edge 판정 | 창·OS SDK 참조 |
| `PlatformWindow` | Input | 창 생성·이벤트 펌프·네이티브 핸들 노출, 입력 backend(Win32 메시지 키보드·마우스, XInput 게임패드) | Graphics·World 참조 |
| `GraphicsTypes` | Core | 그래픽 값 타입: 픽셀 형식·mip 크기 계산(`PixelFormat.h`), 필터·주소 모드·테두리 색·`SamplerDesc`(`Sampler.h`). 장치 계약·에셋·도구가 함께 쓴다 | 장치·창·World 참조 |
| `Graphics` | GraphicsTypes | 장치·스왑체인·버퍼·텍스처(mip·텍스처별 샘플러)·파이프라인·명령 기록 계약, backend 열거 | World·Object·창 모듈 참조 |
| `DxgiGraphics` | Core | D3D11·D3D12가 함께 쓰는 DXGI 어댑터 도우미(HDR 출력 판정·D3D 보장 자원 크기, GX1). 어댑터 질의는 Windows 빌드에만 있다 | Graphics·backend 참조 |
| `D3D12Graphics` | Graphics, DxgiGraphics | D3D12 구현(Windows) | 다른 backend 참조 |
| `D3D11Graphics` | Graphics, DxgiGraphics | D3D11 구현(Windows, feature level 11_0 이상, 10.2절) | 〃 |
| `OpenGLGraphics` | Graphics | OpenGL ES 3.2 구현(Windows `GLES31_PC` 미리보기 WGL, Android EGL, [OpenGL ES backend](OpenGLESGraphics.ko.md)) | 〃 |
| `VulkanGraphics` | Graphics | Vulkan 1.1 구현(Windows·Android, 10.3절·[VulkanGraphics](VulkanGraphics.ko.md)) | 다른 backend 참조 |
| `MetalGraphics` | Graphics | 자리만 둔다. 장치 생성은 `Unsupported`를 반환한다 | 동작하는 척하는 가짜 구현 |
| `Engine` | ObjectRuntime, World | 엔진 상태 기계·메인 루프·WorldContext·EngineSubsystem | Gameplay·Graphics·창 참조 |
| `World` | ObjectRuntime, CoreTypeBindings | World·Actor·Component·SceneComponent(WorldOnly 리플렉션 클래스 — 4.6절)·TickScheduler·WorldSubsystem | Engine·Graphics·Asset 참조 |
| `WorldTestFixtures`(Developer) | World | World 타입을 상속한 리플렉션 시험 타입(SaveActor·SaveComponent)과 그 identity 명세. EngineTests만 의존 | 제품 링크 |
| `Gameplay` | Engine, World | GameInstance·GameDirector·GameState·PlayerState·Controller·Pawn·LocalPlayer. 아직 없다(S10–S12). PlayerState는 모듈식 게임플레이 `PlayerStateComponent`의 소유 클래스다([컴포넌트 도입](ComponentCatalog/09-Plugins.ko.md) CR48). PlayerController의 입력 컴포넌트 스택·햅틱 효과 관리가 입력·force feedback 컴포넌트의 선행이다([컴포넌트 도입](ComponentCatalog/04-Prerequisites.ko.md) CR26) | Graphics 참조 |
| `AssetRuntime` | Serialization, Core | AssetId·에셋 파일 형식·AssetManager·TextureAsset | World·Graphics 참조 |
| `AssetTools`(Developer) | AssetRuntime | BMP 임포터·에셋 파일 기록 | Shipping 링크 |
| `RenderBase` | Graphics, Task, MetaSchema | 렌더 lane·프레임 slot·프레임 packet 전달·완료 통지·`RenderHandle` 예약([렌더 기반](RenderBase.ko.md) RB1) | ObjectRuntime·World 참조 |
| `WorldIntegration` | World, RenderBase, AssetRuntime | SpriteComponent 등 렌더 컴포넌트, RenderScene 추출, 텍스처 GPU 캐시 | Engine 내부 상태 변경 |

정식 렌더러 모듈(`Renderer`, Forward+·Deferred, 계획 11절)은 아직 없다. 내장 메시·원문 셰이더로 그리던 최소 렌더러는 엔진 핵심이 아니라 시험 샘플이므로 `Programs/Sandbox/Private/SampleRenderer`로 옮겼다(2026-11-17). `Renderer` 이름은 정식 렌더러가 다시 쓴다.

Engine은 창·그래픽·게임플레이를 모른다. 이벤트 펌프와 렌더는 `EngineLoopHooks`의 확장 지점으로 실행 파일(Sandbox)이 연결한다. 헤드리스 서버·테스트는 같은 Engine을 hook 없이 실행한다.

## 4. Object 시스템과의 결합

### 4.1 Registry 구성

- `TypeRegistry`는 엔진 전체에 하나다. 엔진 초기화가 엔진·월드·게임플레이 타입을 등록한다. 리플렉션 타입은 Engine이 소유하는 `ModuleRegistration` 하나로 모듈 단위 등록한다(World 타입 → 게임 리플렉션 모듈 → native 타입 순서, 6절).
- `ObjectRegistry`는 **World마다 하나**이며 World마다 고유 `ContextId`를 쓴다. Actor·Component는 소속 World의 Registry에서 생성한다. 다른 World의 객체는 핸들 교환만 가능하고 `ObjectAccessScope`가 문맥 오용을 거부한다. World 파괴는 그 Registry의 Shutdown으로 끝나므로 월드 단위 누수가 바로 드러난다.
- 엔진 수준 Registry가 하나 더 있으며 GameInstance처럼 World보다 오래 사는 객체를 둔다. World 객체를 강한 간선으로 참조하지 않는다.

### 4.2 소유권

- 소유 사슬은 **World → Level → Actor → Component**다(UE의 UWorld → ULevel → AActor → UActorComponent와 같은 형태).
  - World는 Level마다 `RootRef<Level>`을 보유한다. World가 가진 객체 루트는 이것뿐이다. 시작할 때 영구 Level을 만들고, 서브 Level은 `CreateLevel`로 추가한다.
  - Level은 소속 Actor를, Actor는 자기 Component를 **GC 추적 강한 간선 목록 `ObjectRefArray<T>`**로 소유한다. 이 목록은 영속 리플렉션 필드(`actors`·`components`)이고 소유자의 생성 Trace가 방문하므로 GC 도달성이 소유 관계를 그대로 따른다.
  - World는 Actor마다 수명 기록(`ActorRecord`: 상태·Tick 핸들·소속 Level)만 `SlotArray`에 둔다. Component의 수명 기록은 Component 객체 자신이 갖는다.
- 파괴는 **명시적**이다. World가 hook을 부른 뒤 소유자의 간선에서 빼고 `RequestDestroy`한다. GC는 그 뒤 저장소를 회수하는 수단이다. Level 파괴는 소속 Actor를 모두 정리한 뒤 Level 루트를 반환한다.
- `ObjectRefArray`는 이번 단계에서 Core(ObjectRuntime)에 추가했다(4.5절). 추가할 때 대상 타입·생존·같은 Registry를 검증하고, 증분 GC Marking 중이면 대상을 shade한 뒤 게시한다(삽입 장벽). 제거에는 장벽이 필요 없다.
- Actor의 root component는 **영속 강한 참조 `ObjectRef<SceneComponent>`**다(UE `AActor::RootComponent`와 같다). root는 늘 자기 `components` 안의 객체라 새 생존 간선을 만들지 않는다. 쓰기는 Registry `SetReference`로만 하고(Marking 중 삽입 장벽), root Component를 파괴하는 모든 경로(정상 정리·Registry 우회 파괴의 수리)에서 비운다 — 강한 참조는 파괴된 대상을 null로 만들지 않아 비우지 않으면 저장소가 좀비로 남는다.
- 소유가 아닌 그 밖의 객체 사이 참조(부모·자식 SceneComponent, Controller↔Pawn)는 `ObjectHandle`이다. 대상 파괴 후 접근은 핸들 generation 검사로 실패한다. 부착 계층(parent·children)은 저장하지 않는 런타임 상태다(4.6절).
- Actor·Component·Level은 소속 `World*`를 `ROCKS_NATIVE` 멤버로 보관한다. World는 C++ 객체이며 자신의 Registry와 모든 객체보다 오래 산다.
- Level·Actor·Component는 `ROCKS_CLASS(WorldOnly)`, SceneComponent는 `ROCKS_CLASS()`(WorldOnly 상속)인 리플렉션 클래스다(A0-B B8b, [리플렉션·빌드 보강 설계](ReflectionFoundations.ko.md) 9.1절). 메타컴파일러가 컨테이너(B5)와 `ROCKS_NATIVE`(B2)를 지원하게 되어 이전의 수동 네이티브 등록 이유가 사라졌다. 멤버 분류는 4.6절이다.

### 4.3 접근

- 객체 포인터는 `Borrow` 안에서만 쓴다. TickScheduler와 World는 매 호출마다 Borrow를 얻는다(잠금 비용은 진단으로 측정하고, 일괄 접근 API는 후속 최적화로 둔다).
- **`Resolve<T>`는 상속을 허용**하도록 바꾼다. 등록된 native 기반 체인에 `T`가 있으면 성공하고, 포인터는 `Object*`에서 `static_cast`로 내려 얻는다(단일 비가상 상속만 지원하므로 안전하다). `SetReference`·`TryPin`·생성 트랜잭션의 타입 검사도 같은 규칙을 쓴다. 정확 타입만 허용하던 기존 의미와 그 테스트는 바꾼다.

### 4.4 네이티브 클래스 등록

World 타입은 리플렉션 클래스이므로 `RegisterWorldTypes(ModuleRegistration&)`(생성 집계 함수 `rocks::generated::World::RegisterModuleTypes` — 의존 모듈 CoreTypeBindings의 struct부터, base 먼저)로 등록한다. 같은 registration에 다시 부르거나 다른 registration이 이미 등록했으면 lease를 붙잡고 성공한다(멱등). 영속 상태가 없는 런타임 전용 파생(.cpp 안의 시험 타입 등)은 네이티브 클래스로 등록한다:

```cpp
class SpinningActor final : public rocks::Actor {
public:
    ROCKS_NATIVE_TYPE(SpinningActor, rocks::Actor, 0x5b1c..., 0x9e02...)
    // ...
};

// 엔진 초기화(EngineConfig::registerTypes) 또는 게임 모듈 초기화에서. 리플렉션 base(Actor)의 생성
// 헤더가 필요하다(모듈 생성 헤더 World.module.generated.h는 등록 함수의 선언만 담는다).
#include <World/Actor.generated.h>
rocks::RegisterNativeClass<SpinningActor>(types);
```

매크로는 `ThisClass`·`Super` 별칭, 정적 `TypeId`, 이름을 정의한다. `NativeClassType` concept은 매크로를 빠뜨려 기반의 ID를 물려받은 클래스를 컴파일 시 거부한다. `RegisterNativeClass<T>`는 `baseType = ObjectTypeIdOf<T::Super>()`와 `MakeRuntimeTypeOps<T>`(`nativeBaseType` 설정)를 등록하고, Super 사슬 위에 리플렉션 조상 R(World 타입)이 있으면 R의 스키마 필드·생성 함수표를 빌리고 R의 WorldOnly를 물려받으며 GC Trace는 native 손 Trace를 부르다가 R의 생성 Trace에서 멈춘다(B8a). native 파생은 자기 C++ 상태를 모르므로 `completeFieldLayout = false`이고 그 객체가 든 그래프의 저장은 Unsupported다. 기반 타입을 먼저 등록해야 한다(TypeRegistry 규칙 — native 등록은 리플렉션 등록 뒤). `nativeBaseType`은 상속 Resolve의 포인터 변환 근거이므로 C++ 상속을 `static_assert`로 보증한 코드(RegisterNativeClass, 메타 생성 코드)만 설정하고, TypeRegistry는 기반 레코드 일치와 크기·정렬 하한을 검사한다. `World::SpawnActor<T>`·`AddComponent<T>`는 `ObjectTypeIdOf<T>()`를 쓴다 — native T는 `T::StaticTypeId`만 쓰고, 리플렉션 World 타입을 템플릿 인자로 쓰는 TU는 생성 헤더를 include한다(World.h는 생성 헤더를 include하지 않는다 — Engine.h 소비자 전체로 컴파일 비용이 번지지 않게).

### 4.5 ObjectRefArray

```cpp
ROCKS_CLASS(WorldOnly)
class Actor : public Object {
    ROCKS_GENERATED_BODY()
public:
    explicit Actor(Allocator& allocator) noexcept : components_(allocator) {}
private:
    // 생성 Trace가 방문한다(손 Trace 없음).
    ROCKS_PROPERTY(ReadOnly, Required, Name = "components") ObjectRefArray<Component> components_;
};

registry.AddReference(access, actor, actorPointer->components_, component);
registry.RemoveReference(access, actor, actorPointer->components_, component);
```

- 원소 버퍼는 소유 객체의 allocator(Registry allocator)에서 할당하고, 확장은 Registry 잠금 밖에서 한다. 원소를 바꾸는 쪽은 소유 스레드뿐이고 GC trace는 slice 안에서만 원소를 읽으므로, 확장과 trace가 겹치지 않는다.
- 배열 객체는 소유자 저장소 안에 직접 있어야 한다(`SetReference`의 필드 위치 검사와 같은 규칙). 추가는 게시된 생존 소유자만 받고, 정리용 제거는 파괴 요청된 소유자에서도 허용한다.
- 끝 원소와 바꿔 떼기 `RemoveReferenceAtSwap(access, owner, array, index)`: index 원소를 지우고 끝 원소를 그 자리로 옮긴다(순서를 지키지 않는다 — O(1), 앞에서 찾거나 뒤를 당기지 않는다). 계약:
  - 지운 원소는 `RemoveReferenceAt`과 같은 규칙이다(장부·관찰자 기록, 삭제 간선은 삽입 장벽이 필요 없다).
  - 옮기는 원소는 같은 소유자·같은 컨테이너에 이미 있는 간선이다. 대상의 생존·cutoff·세대를 다시 묻지 않는다 — 대상이 DestroyRequested(World API 밖 파괴)이거나 cutoff white여도 실패하지 않는다. 간선 다중도가 그대로라 장부·관찰자 기록도 없다.
  - owner 규칙은 `RemoveReferenceAt`과 같다(Alive 또는 정리 중인 DestroyRequested, Borrow 보유, 컨테이너가 owner 저장소 안). index ≥ Size()는 InvalidArgument다. 할당하지 않는다(관찰자 Registry는 지운 원소 하나의 예약 — 공통 규칙).
  - 배경: 처음 구현(1913624)은 `SetReferenceAt(index, 끝 원소)` + 끝 `RemoveReferenceAt`이었는데, `SetReferenceAt`은 대상 검증을 거쳐 끝 원소가 DestroyRequested면 StaleHandle로 실패했고 실패를 버려 지울 Actor가 Level 배열에 좀비로 남았다(리뷰 지적). 같은 컨테이너 안 이동은 대상 검증 대상이 아니다.
- World가 만든 목록에는 null이 없지만 로드된 목록에는 null 원소가 올 수 있다(`Required`는 로드에서 진단만 한다). 그래서 World의 Component 목록 순회(FindComponent·FinishSpawning·활성화·시작·DestroyActor 등)는 null을 끝 표지로 쓰지 않고 원소 수 기준으로 돌며 null을 건너뛴다.

### 4.6 리플렉션과 레벨 편입(A0-B B8b)

멤버 분류([리플렉션·빌드 보강 설계](ReflectionFoundations.ko.md) 9.1절, 결정 21):

| 타입 | PROPERTY(영속, 모두 ReadOnly — 도구 이름) | NATIVE(런타임) |
|---|---|---|
| Level | `actors`(ObjectRefArray<Actor>, Required) · `name` · `levelScript`(ObjectRef<LevelScript>, null 허용 — `actors` 안의 레벨 스크립트 Actor, [LevelScript](LevelScript.ko.md) LD4) | World 포인터 · 영구 여부 · 파괴 대기 |
| Actor | `components`(ObjectRefArray<Component>, Required) · `rootComponent`(ObjectRef<SceneComponent>) · `enabled` | World 포인터 · 소속 Level(actors 포함 관계에서 유도) · Tick 설정·Tick 켜짐 |
| Component | `enabled` | World 포인터 · 소유 Actor · Tick 설정 · 수명 기록 |
| SceneComponent | `relativeTransform`(Transform — CoreTypeBindings 미러) | 월드 행렬 캐시 · dirty · parent · children(유도값) |

- **ReadOnly**: 경로·리플렉션 쓰기가 World 장부(ActorRecord·hook·MarkDirty·Tick 등록)를 우회하지 않게 World 필드는 모두 ReadOnly다(로드의 staged 쓰기는 허용). 값은 World API(`SetEnabled`·`SetRootComponent`·`SetRelativeTransform`·`SetLevelScript`(편집 World 전용) 등)로 바꾼다. 그래서 CU5 setter hook 전에는 에디터에서 `relativeTransform`을 경로로 편집할 수 없다(UE `EditAnywhere`와 다른 제약).
- **Tick 설정은 NATIVE**다(UE `PrimaryActorTick`은 archetype 차분 직렬화 — Rocks에는 아직 클래스 기본값 레코드와 차이 저장이 없어 인스턴스 저장값이 클래스 기본값 변경을 가린다). 레코드와 struct 멤버 단위 비교가 생기는 A5에서 리플렉션 struct·enum으로 옮기며, 그래서 `TickSettings`를 단순 값 구조로 둔다([RuntimeClassMetadata](RuntimeClassMetadata.ko.md) '클래스 기본값 결정').
- **부착 계층은 NATIVE**다(parent는 다른 Actor의 Component에 붙을 수 있어 강한 참조로 두면 좀비와 그래프 끌어오기가 생긴다 — UE는 Outer·레벨 패키지·garbage elimination으로 푼다). 추적하지 않는 참조 `WeakObjectRef<T>`가 생긴 뒤 레벨·prefab 저장 전에 영속한다. 리플렉션 B9가 `WeakObjectRef<T>` PROPERTY(같은 그래프 안 대상만 보존)를 가능하게 하며, 부착 계층을 리플렉션으로 옮길지는 World 단계가 정한다([리플렉션 B9](ReflectionExtensions.ko.md) 5.1절).
- **Level 그래프 저장은 구조 시험용이다**: graph v2 편의 API `SaveObjectGraph(root)`로 Level 루트를 저장할 수 있다(A0-Od1부터 — [ObjectFoundations](ObjectFoundations.ko.md) 5.2절, 그 전에는 graph v1)(그래프의 모든 타입이 생성 타입일 때 — native 파생 Actor·Component가 하나라도 있으면 Unsupported, Actor별 저장 제외는 레벨 패키지 단계). 부착 계층은 저장되지 않는다(root는 저장된다). 저장 안전 지점은 Tick 밖·파괴 대기 정리(FlushDestroys) 뒤다 — 정리 전에는 파괴 대기 Actor가 `actors`에 남아 함께 저장된다.
- **레벨 저장 검증(레벨 패키지 단계 — 자리)**: 레벨 저장·쿡은 (a) `levelScript`가 null이 아니면 `actors`의 원소이고 그 밖에 LevelScript 파생 Actor가 없는지(`LevelScriptNotInLevel`·`DuplicateLevelScript` — cooked 로드도 다시 검사해 `CorruptPackage`, 출시 빌드도 끄지 않는다 — LevelScript LD5), (b) 레벨에 저장하는 weak 참조가 같은 Level의 객체만 가리키는지(다른 Level은 `CrossLevelReference`로 거부, 셀 사이는 영속 Entity 참조가 생기기 전까지 거부 — LD6), (c) prefab 배치가 펼친 노드를 가리키는 참조를 Record에서 null로 두고 (배치 `instanceGuid` + 노드 주소)로 따로 저장하는지([Prefab](Prefab.ko.md) D22, LD17)를 검사한다. 진단 이름은 [에셋 시스템](AssetSystem.ko.md) 4.6절이다.
- **장부 밖 객체**: World API(SpawnActor·AddComponent·CreateLevel) 밖에서 만든 Actor·Component(그래프 로드, World registry에 직접 `NewObject`)는 `World*`가 없고 World에 수명 기록이 없다. 그 객체의 계층·World API는 Fatal 대신 `InvalidArgument`다(`SetRootComponent`·`Destroy`·`AttachTo`(그 객체를 부모로 쓰는 경우 포함)·`Detach`, `World::DestroyComponent`). 수명 기록이 필요한 World API(`DestroyActor`·`AddComponent`·`GetComponents`)는 `StaleHandle`이다. 상대 Transform 설정과 월드 행렬 계산은 부모가 없으므로 된다. `GetWorld()`는 참조를 돌려주므로 여전히 Fatal이며 `HasWorld()`로 먼저 확인한다.
- **레벨 편입 계약(레벨 패키지 단계 — 자리)**: 로드한 Level 그래프를 World에 넣는 단계는 (1) Actor마다 `ActorRecord`를 만들고 Level 레코드의 Actor 수를 맞추며 Actor의 `actors` 배열 위치를 레코드(`levelPosition` — 7.4절 바꿔 떼기의 근거)에 기록하고(아래 (4)의 null 정리·압축 뒤 다시 매긴다), Component마다 World의 Component 색인(소유 Actor·Tick handle — 7.4절 우회 파괴 복구)에 넣으며, (2) Level·Actor·Component의 `World*`·Actor의 소속 Level·Component의 소유 Actor를 `actors`·`components` 포함 관계에서 연결하고, (3) 부착 계층이 영속된 뒤에는 parent에서 children을 다시 만들며, (4) `actors`·`components`의 null 원소를 정리하고 root가 `components` 밖이면 비우며, (5) `relativeTransform`을 `SetRelativeTransform`과 같은 규칙(유한·단위 quaternion)으로 검증한 뒤, (6) World 수명 단계(FinishSpawning과 같은 hook 순서)로 진행한다. Actor마다 FinishSpawning하되 `levelScript`는 그 Level의 다른 모든 Actor(prefab 배치에서 펼친 Actor 포함)의 FinishSpawning 뒤에 한다 — 저장 때 꺼져 있던 Actor는 "이미 활성" 보장에서 빠지고, Start는 시작 큐 순서대로 같은 프레임에 준비된 활성 Actor 뒤에 시작하며 꺼진 채 기다리거나 준비가 안 된 Actor는 기다리지 않는다([LevelScript](LevelScript.ko.md) LD8). 이 단계 전의 로드 객체는 별도 ObjectRegistry(같은 TypeRegistry)에 두고 World registry에 넣지 않는다.
- **World registry의 외부 생성 거부(정책 자리)**: World registry에 World API 밖에서 World 타입을 만드는 것(직접 `NewObject`·그래프 로드)은 계약 위반이다. 지금은 위 장부 밖 규칙으로 Fatal 없이 거부만 하고, A0-O의 에셋 Registry 정책(WorldOnly 타입의 에셋 Registry 생성 거부)의 거울로 World registry가 WorldOnly 타입의 외부 생성 경로를 막는 검사를 둘 자리를 남긴다. 그 검사는 `RegistryPolicy::worldOnlyCreationKey`(World API만 가진 키를 함께 줄 때만 WorldOnly 타입 생성 허용)로 넣는다([ObjectFoundations](ObjectFoundations.ko.md) 1.3절 후속 자리).

## 5. 시간

Core에 `Core/Time/GameClock.h`를 둔다(계획 8.3절의 StepPlanner).

- `TimeStepPolicy{fixedStepSeconds, maxFrameDeltaSeconds, maxSubsteps}`.
- `GameClock::Advance(realDeltaSeconds) → StepPlan{unscaledDelta, delta, fixedSteps, fixedStep, alpha, droppedSeconds}`.
- 실제 시간 delta는 `maxFrameDeltaSeconds`로 자른다. 게임 delta = 잘린 실제 delta × scale이며 pause면 0이다. 고정 accumulator에는 게임 delta만 더하므로 scale·pause가 같은 값을 두 번 적용하지 않는다.
- 한 프레임의 고정 스텝 수는 `maxSubsteps`를 넘지 않으며, 넘는 누적은 버리고 `droppedSeconds`로 보고한다.
- pause 중 `RequestSingleStep(debugDelta)`은 단일 스텝을 요청하고, 다음 `Advance`가 이를 소비해 고정 스텝 정확히 한 번과 디버그 delta(생략 시 fixedStep)를 계획한다(`StepPlan::singleStep`). 실제·unscaled 시간은 측정값 그대로 흐르고, World는 그 프레임을 pause가 아닌 것처럼 실행한다.
- scale은 [0, 1000] 범위의 유한한 값이다. 0은 게임 시간을 멈추지만 pause 상태가 아니다(단일 스텝 요청 거부, `TickWhenPaused` 미실행). 음수·NaN은 `InvalidArgument`다. scale과 pause는 별도 상태라 pause 해제 시 이전 scale이 그대로 적용된다.
- 고정 스텝은 [10µs, 1s], 프레임당 고정 스텝 상한은 [1, 1024], 프레임 delta 상한은 (0, 60s]다. 고정 시간은 스텝 길이가 바뀐 시점을 기준점으로 누적한다. 극단값에서도 보간 비율과 잔여 누적을 [0, 1)·[0, step)으로 고정한다.
- Engine은 실제 시간과 프레임 번호를, World는 자기 `GameClock`을 가진다. World별 pause·scale이 독립이다.

## 6. Engine

```text
Uninitialized → Initializing → Running → Stopping → Stopped
                     └──(실패)──→ Failed(이미 초기화한 단계를 역순 정리)
```

- 모듈 로더([모듈 로더](ModuleLoader.ko.md) 3.11절, ML1): `EngineConfig::modules`(프로세스 로더, 필수)에 엔진 `TypeRegistry`의 타입 등록기를 붙이고(이미 시작한 모듈의 타입을 몰아서 등록) 엔진 묶음(`PreDefault`–`PostDefault`, 동시에 여러 Engine이면 참조 수)을 시작한다. 엔진 단계 모듈(World·Engine 등)의 타입은 그 모듈 `Startup` 직전에 등록된다. 클라이언트 시작 뒤 `PostEngineInit` 단계를 돈다. 종료는 ObjectRegistry 정리·최종 GC 뒤 엔진 묶음 종료 → 등록기 떼기 → `TypeRegistry::Shutdown`이다.
- `Initialize(EngineConfig)`: TypeRegistry·ModuleRegistration → 타입 등록(모듈 로더 등록기와 엔진 묶음 — World 리플렉션 타입 등 → `EngineConfig.registerModuleTypes(ModuleRegistration&)` — 게임 리플렉션 모듈, 같은 registration → `EngineConfig.registerTypes(TypeRegistry&)` — native 타입, 리플렉션 타입을 base로 둘 수 있으므로 마지막) → 엔진 Registry → EngineSubsystem(의존 순서) → 클라이언트(`EngineClient`, 예: GameInstance) 순. 어느 단계든 실패하면 앞 단계를 역순 정리하고 `Failed`.
- 종료(`Shutdown`과 초기화 실패 정리 모두): World 파괴 → Subsystem 역순 정리 → 엔진 Registry 정리 → ModuleRegistration 파괴(붙잡은 lease 해제) → `TypeRegistry::Shutdown`. Engine은 타입을 Unregister하지 않는다 — 게임이 토큰을 버린 native 파생이 리플렉션 base의 dependent로 남아 Unregister가 Busy로 멈추기 때문이다. `TypeRegistry::Shutdown`은 남은 lease만 검사하고 레코드를 dependent 순서로 반납한다. ModuleRegistration 멤버는 TypeRegistry 멤버 뒤에 선언해 먼저 소멸한다.
- 종료 게이트(2026-11-22 설계, [오디오 시스템](AudioSystem.ko.md) AD23): 외부 실행 문맥(장치 콜백·전용 스레드)이 Subsystem 소유 메모리를 쥐고 있을 수 있는 Subsystem은 `CompleteInFlightWork`에서 그 문맥이 멈췄음을 확인하지 못하면 `Busy`를 돌려준다. Engine은 `Busy`를 다른 실패처럼 누적하지 않고 그 자리에서 `Shutdown`을 멈추고 `Busy`를 돌려준다(Subsystem 해제·Registry 정리로 넘어가지 않는다 — 해제 뒤 접근 방지). `Shutdown`을 다시 부르면 아직 완료하지 않은 Subsystem부터 `CompleteInFlightWork`를 이어 부른다(끝낸 것은 다시 부르지 않는다). 엔진 호스트는 `MaxShutdownGateRetries`(= 3)번까지 다시 부르고, 그래도 `Busy`면 `platform::Fatal`로 끝낸다(소유자를 풀지 않고 끝내는 것이 해제 뒤 접근보다 안전하다 — [크래시 처리·실행](CrashHandlingAndLaunch.ko.md) 20절). 대기 시간 상한은 Subsystem 안에 있다(오디오 `ShutdownDrainTimeoutMs`). 지금 코드는 모든 실패를 `KeepFirst`로 누적하고 해제로 진행한다(`Engine/Source/Runtime/Engine/Private/Engine.cpp:767–794`) — 오디오 AU3에서 바꾼다.
- `RunFrame()`: ① hook `PumpEvents` ② 시간 갱신 ③ hook `FrameBegin` ④ WorldContext마다 `World::Tick` ⑤ 엔진 지연 작업 ⑥ hook `FrameEnd`(렌더 추출·제출).
- `Run()`: `RequestExit()`까지 `RunFrame` 반복. `Shutdown()`은 상태와 무관하게 여러 번 호출해도 안전하다.
- `EngineClient`는 Engine이 Gameplay를 모르게 하는 경계다. GameInstance가 이를 구현한다.

## 7. World·Actor·Component

### 7.1 World 상태

`Created → Initialized(WorldSubsystem 초기화) → Playing(BeginPlay 이후) → TearingDown → Destroyed`. 초기화와 게임플레이 시작을 분리한다. TearingDown 이후 Spawn·Tick 등록은 `ShuttingDown`으로 거부한다.

### 7.2 Actor 수명

| 상태 | 뜻 | 진입 |
|---|---|---|
| `Constructed` | 존재함. Component 추가 가능. Tick 없음 | `SpawnActorDeferred` |
| `Initialized` | Component 등록·`OnAwake` 완료 | `FinishSpawning` |
| `Playing` | 의존성 준비 완료, `OnStart` 호출, Tick 참여 | World가 Playing이고 `IsReadyToStart()`가 참일 때 프레임 시작 처리 |
| `PendingDestroy` | 파괴 요청됨. Tick 중지. 일반 API 거부 | `DestroyActor` |
| (레코드 제거) | `OnDestroy`·Component 해제·`RequestDestroy`·루트 반환 | 프레임의 안전 지점 |

- 활성(`enabled`)은 상태와 별개 플래그이며 `OnEnable`/`OnDisable`을 부른다.
- Level의 `actors_`(`Level::Actors()`)는 순서를 보장하지 않는다. Actor 레코드가 배열 위치를 기억하고, 파괴는 `RemoveReferenceAtSwap`으로 끝 원소와 바꿔 O(1)로 뗀 뒤 옮긴 Actor의 위치를 고친다. 위치와 배열이 어긋나면 Fatal이다(실패를 버리지 않는다). 측정(실험 16절): Actor 하나의 파괴가 Actor 10K → 100K에서 5.0 → 43.7us이던 것이 0.5–1.0us로 같다.
- 꺼진(`enabled`가 거짓) 채 시작을 기다리는 Actor는 시작 큐에서 빼고 `startDeferred`로 표시한다. 활성화(`EnableActor`)가 표시를 보고 큐에 다시 넣는다(hook 안 활성화 포함 — 큐 용량은 Spawn에서 Actor마다 확보했다). 프레임마다 꺼진 Actor를 다시 훑지 않는다. 시작 순서는 다시 넣은 순서다.
- `DestroyLevel`은 그 Level의 `actors_`만 훑는다(World 전체 Actor가 아니라 — 작은 Level 파괴 21 → 716us이던 것이 1.3–1.5us).
- `DestroyLevel`(World 종료·travel 포함)은 Level의 `levelScript`를 먼저 처리한다: 간선을 끊고 LevelScript를 다른 Actor보다 먼저 파괴 대기에 넣어 프레임 끝 정리에서 LevelScript의 Disable·Destroy가 그 Level의 다른 Actor보다 먼저 실행된다([LevelScript](LevelScript.ko.md) LD8). LevelScript는 `DestroyActor`로 파괴할 수 없다(`InvalidArgument`, 진단 `LevelScriptDestroyRejected` — LD16). 간선을 끊고 파괴하는 길은 `DestroyLevel`과 편집 World의 `SetLevelScript`, 엔진 내부 예외(OnAwake 실패, 우회 파괴 복구 — 계약 위반으로 Error 기록, 핫 리로드 R3 재생성)뿐이고 모두 간선을 먼저 끊으므로, 강한 간선이 파괴 요청된 Actor를 붙잡아 회수되지 않는 일(7.4절)이 없다. fault 격리는 파괴가 아니므로 간선을 유지한다. `SpawnActor`는 LevelScript 파생 클래스를 `InvalidArgument`(`LevelScriptSpawnRejected`)로 거부한다(레벨당 0 또는 1 — LD5).
- `SpawnActor`는 `SpawnActorDeferred` + `FinishSpawning`이다. 같은 프레임에 FinishSpawning한 Actor들의 `OnAwake`가 모두 끝난 뒤에 `OnStart`를 시작한다.
- 의존성 준비(`IsReadyToStart`)는 매 프레임 확인하며 기다리느라 게임 스레드를 막지 않는다. 비동기 에셋 로딩은 이 지점에 연결한다.
- `OnAwake`가 실패하면 그 Actor는 즉시 파괴 경로로 들어가고 `FinishSpawning`이 오류를 반환한다.

### 7.3 Component

- `Component`는 Actor에 속하고 `OnRegister`/`OnUnregister`(엔진 내부·backend 등록)와 위 hook을 가진다. Actor의 `FinishSpawning`이 Component를 등록한다(OnRegister → Actor OnAwake → Component OnAwake → OnEnable). 이미 초기화된 Actor에 추가된 Component는 추가 즉시 OnRegister·OnAwake·OnEnable을 받고 OnStart는 다음 프레임 시작에 받는다(다른 객체의 hook 실행 중 OnStart 재진입 방지). 정리는 OnDisable(Component 역순 → Actor) → OnDestroy(Component 역순 → Actor) → OnUnregister 순이며, OnDestroy는 OnAwake가 성공한 객체만 받는다.
- `SceneComponent`는 부모·자식 계층, 상대 Transform, 월드 행렬(`Mat4d` affine)을 가진다. 좌표·단위는 UE 5.8과 같다(왼손 +X 전방·+Y 오른쪽·+Z 위, 위치 cm, 회전 단위 quaternion — 도 단위 `Rotator`는 `ToQuaternion`으로 바꾼다, scale 무차원). 월드 행렬 = 상대 TRS 행렬 × 부모 월드 행렬(행 벡터 — UE `NewRelativeTransform * ParentToWorld`, 2026-11-17 전에는 column vector의 부모 × 상대)이며 dirty 플래그로 지연 계산한다. 행렬로 저장하므로 비균일 scale 부모 아래 회전한 자식(shear)도 정확하고, TRS가 필요할 때만 `TryWorldTransform`이 분해한다(shear면 Unsupported). 상대 Transform·부모 변경 시 하위 전체를 dirty로 표시한다. `AttachTo`는 순환(자기 자신·조상)과 장부 밖 객체를 `InvalidArgument`로 거부하며 같은 World 안에서만 붙인다. 부모가 파괴되면 자식은 루트가 된다. 영속 상태는 상대 Transform뿐이고 계층·월드 행렬은 저장하지 않는다(4.6절).
- **보강 예정(설계만, 미구현).** UE 5.8 엔진 컴포넌트를 들이기 전에 이 절을 넓힌다. 요구의 정본은 [컴포넌트 도입](ComponentCatalog/04-Prerequisites.ko.md) 4.1절이고 착수는 그 문서 CM0(화면 마일스톤 첫 단계)이다.
  - CR1 시스템 상태 수명: 등록 안에서 렌더·물리·오디오 상태를 만들고 지우는 hook과 "다시 보내기" 표시(UE `CreateRenderState`·`MarkRenderStateDirty`·`CreatePhysicsState` 대응), 등록 실패 시 부분 등록 해제, 대량 등록 묶음. 지금은 `OnRegister`·`OnUnregister` 하나뿐이다.
  - CR2 SceneComponent: 부착 규칙(KeepRelative·KeepWorld·SnapToTarget), socket 부착과 socket Transform, 절대 location·rotation·scale, Mobility(Static·Stationary·Movable — Static 이동은 진단과 함께 거부), 가시성(자식 전파·게임 중 숨김), 속도, bounds 계산과 갱신, Transform 변경 통지(구독형 — 이벤트 slot 표).
  - 그 밖에 Component 시작 준비 hook(CR4 — SE-W2), EditorOnly 컴포넌트와 태그(CR5), 컴포넌트 타입 규칙(CR6)이다.

### 7.4 지연 작업과 안전 지점

- Tick 순회 중 파괴·Tick 등록 변경·Component 제거는 큐에 넣는다. 안전 지점은 **각 phase 경계**(Tick 등록 변경 반영)와 **World Tick 끝**(파괴 완료)이다.
- 같은 객체의 중복 파괴 요청은 성공으로 처리하고 한 번만 실행한다. 큐에 남은 핸들이 이미 무효이면 건너뛴다.
- World Tick 끝에 GC 일정(`WorldDesc::gcSchedule`)에 따라 파괴된 객체 저장소를 회수한다. 기본은 제한된 예산의 증분 GC이고, `RequestFullCollection`(레벨 전환·로딩·메모리 압박)이나 `WorldGcMode::FullCollection`(주기)은 STW 병렬 전체 수집 뒤 회수를 프레임마다 시간 예산으로 나눈다. 표식 병렬 scheduler는 엔진이 소유해 World에 빌려준다(GC 설계 17.3·17.4절).
- World API 밖에서 파괴(`RequestDestroy`·`Object::Destroy`)된 Actor·Component·Level은 **매 프레임 GC step 전**(World Tick 끝, 파괴 정리 뒤 — `RepairBypassedObjects`)에 Registry의 파괴 요청 목록(`ObjectRegistry::TakeDestroyRequests` — 지난 호출 뒤 DestroyRequested가 된 객체, GC 설계 14절)에서 찾아 World 파괴로 돌린다. Actor는 `DestroyActor`, Component는 파괴 큐(정리 단계가 소유 Actor 목록·Tick 등록에서 뗀다), Level은 `DestroyLevel`(소속 Actor 전부 정리 후 회수)이다. 영구 Level의 우회 파괴는 복구할 수 없는 계약 위반이라 Fatal이다(World API로도 파괴할 수 없고 Spawn 기본 대상이다). 목록의 객체가 World 레코드·색인에 없거나 이미 PendingDestroy면(World가 요청한 파괴) 건너뛴다. 가려내기는 모두 색인으로 한다: Actor는 Actor 레코드 색인, Level은 슬롯 번호 집합(`levelSlots_`), Component는 Component 색인(`componentRecords_` — 슬롯 번호 → Component handle·소유 Actor·Tick handle 사본, AddComponent가 넣고 World 정리가 회수 요청 직전에 뺀다). Component 색인은 정리 단계가 우회 파괴된 Component(Borrow로 읽을 수 없다)의 소유 Actor와 Tick 등록을 찾는 데도 쓴다 — 처음 구현은 그때 모든 Actor를 Borrow로 훑었다(Actor 수 × Borrow). 비용은 그 사이 파괴 수에 비례하고(Actor 수가 아니다) 목록이 비면 잠금 한 번이다. 복구한 수는 `WorldStats::repairedBypasses`다.
  - Component 단독 우회 파괴는 처음 구현에서 복구되지 않았다(리뷰 지적, 시험 `World_BypassedComponentIsRepaired`로 확인 — 소유 Actor 목록·Actor 수에 남고 회수되지 않았다).
  - 복구가 늦어도 안전한 이유: World 객체의 회수는 World 루트(Level 레코드)·Level 배열·Actor의 `components` 간선에서 끊긴 뒤에만 일어나고(DestroyRequested여도 강한 간선이 붙잡으면 회수되지 않는다), 그 간선을 끊는 것은 World 정리뿐이다. 그래서 우회 파괴가 언제 일어나든(Tick 밖, Tick hook·OnDestroy hook 안 포함) 다음 복구 전에 회수되지 않는다. 매 프레임인 이유는 cycle 시작 전에만 보던 처음 방식이 World 쪽 파괴가 없으면 GC가 돌지 않아 우회 파괴된 Actor·Level을 영영 정리하지 않았기 때문이다(2026-11-18 리뷰 반영 시험 `World_BypassedLevelDestroyIsRepaired`가 드러냄 — Level 우회 파괴는 아예 복구하지 않았다). 앞 개정의 "cycle 도중 우회 파괴가 정리 없이 회수된다"는 근거는 틀렸다(리뷰 지적).
  - 처음 구현은 cycle마다 모든 Actor를 `InspectLifecycle`로 조회했다(cycle 시작 프레임 Actor 10K → 100K: 207 → 2,390us, 파괴 요청 목록으로 2–13us).
  - 비용(2026-11-19, `WorldBench` — GC 실험 23절): 우회 파괴 하나의 복구·정리(Registry `RequestDestroy` 200개 뒤 한 프레임)는 Actor 10K·100K 모두 Actor당 0.30–0.34us(World API `DestroyActor`는 0.42–0.51us)이고, 변화 없는 프레임은 0.3us(목록이 비면 잠금 한 번)다. Actor 수와 무관하다.
  - Level 배열에서 뗄 때(`DetachFromLevel`)는 `RemoveReferenceAtSwap`(4.5절)을 쓴다. 옮겨 오는 끝 Actor도 우회 파괴로 DestroyRequested일 수 있는데, 위치 교체 `SetReferenceAt`은 그 원소를 새로 쓰는 것이라 재검증에서 실패하고 그 실패를 버리면 지운 Actor가 배열에 좀비로 남았다(리뷰 지적). 이 경로의 실패와 Actor 레코드 위치·배열 불일치는 일어날 수 없으므로 Fatal이다. 단 공개 API로 닿는 실패 하나가 있다: Level을 `PinForEncode`로 고정한 채 Tick하면 그 프레임의 Actor 정리에서 떼기가 Busy다. World 계약은 Borrow와 같이 encode 고정도 Tick·Shutdown 전에 반환하는 것이고(World.h 소유·수명 규약), 위반하면 그 계약을 밝히는 메시지로 Fatal이다(떼기 시점의 Busy로만 판정 — 매 프레임 Level을 훑지 않는다). 미뤄 두는 대기 목록은 버렸다: 고정이 풀릴 때 알 길이 없어 매 프레임 다시 시도해야 하고, 그동안 Level 배열과 Actor 레코드가 어긋난 채 남는다. 예외는 Level 객체 자체가 우회 파괴돼(DestroyRequested — Registry가 빌려 주지 않는다) 복구 전인 경우다. 그 배열은 Level과 함께 사라지므로 뗄 것이 없고, 복구의 `DestroyLevel`은 Level 배열 대신 World의 Actor 레코드에서 소속 Actor를 찾는다(이 계약 위반의 복구에서만 Actor 수에 비례).

### 7.5 소스 파일 구성

`World` 클래스 하나의 정의를 기능별 파일로 나눈다(`Engine/Source/Runtime/World/Private/`, 2026-11-18 — 한 파일 약 2,000줄을 동작 불변 이동으로 나눔). 공개 선언은 `World/World.h` 하나이고, 파일 사이 공용 helper가 필요하면 `Private/` 내부 헤더에 둔다(지금은 없다).

| 파일 | 담당 | 새 기능을 넣는 곳 |
|---|---|---|
| `World.cpp` | 생성·소멸, `Initialize`·`BeginPlay`·`Shutdown`(Subsystem 초기화·종료 포함), 상태 이름·Tick 설정 검사·타입 등록, 레코드 조회(`FindActorRecord`·`Get*`·`HasActor`), `Stats` | World 수명 단계, 조회 API, 통계 |
| `WorldLevels.cpp` | Level 생성·파괴·회수(`CreateLevel`·`DestroyLevel`·`ReleaseLevel`·`FinishLevelDestroys`), Level 배열에서 떼기(`DetachFromLevel` — 바꿔 떼기) | Level 스트리밍·Level 단위 기능 |
| `WorldActors.cpp` | Actor Spawn(`SpawnActorDeferred`·`FinishSpawning`), `DestroyActor` 요청, 클러스터 가입 조건(`JoinsCluster`) | Actor 생성 방식·Spawn 인자 |
| `WorldComponents.cpp` | Component 추가·진행(`AddComponent`·`AdvanceComponent`)·`DestroyComponent` 요청 | Component 종류별 등록 |
| `WorldActivation.cpp` | 활성·Tick 활성 전환(`SetActorEnabled`·`EnableActor` 등, 시작 큐 재등록) | 활성 정책 |
| `WorldStarts.cpp` | 시작 큐와 `OnStart` 처리(`ProcessStarts`·`StartComponent`) | 시작 순서·준비 조건 |
| `WorldTicks.cpp` | Tick 등록·해제(`RegisterTicks` 등)와 프레임 실행(`ExecuteTick`·`Tick`) | 프레임 단계·Tick 연동 |
| `WorldDestroys.cpp` | 안전 지점의 파괴 정리(`FlushDestroys`·`DestroyActorNow`·`DestroyComponentNow`·root 비우기) | 파괴 hook·정리 순서 |
| `WorldGarbageCollection.cpp` | GC step(`StepGarbageCollection`), 소유 객체 파괴 요청, 우회 파괴 복구(`RepairBypassedObjects`·Component 색인) | GC 일정·Registry 연동 |

- 파일은 수백 줄 안쪽을 유지한다. 한 파일이 천 줄 가까이 커지면 기능 경계로 다시 나눈다(나눔은 동작 불변 이동 커밋).
- `TickScheduler.cpp`(약 590줄)는 한 클래스의 한 기능이라 나누지 않는다.

## 8. Tick Scheduler

- **UE 단계 대응(2026-11-23 결정, 권장안 — [컴포넌트 도입](ComponentCatalog/04-Prerequisites.ko.md) CR8a·CD11).** 단계를 더하지 않는다. UE `ETickingGroup`(`Engine/Classes/Engine/EngineBaseTypes.h:86–104`)과 계획 20.2절의 옛 그룹 이름(PhysicsSync·AnimationFinalize)은 아래처럼 대응한다. UE 경로는 `Engine/Source/Runtime/Engine/` 기준이다.

  | Rocks | UE 5.8 | 근거·규칙 |
  |---|---|---|
  | PrePhysics | `TG_PrePhysics` | Actor 기본(`Private/Actor.cpp:276`), skeletal mesh 기본(`Private/Components/SkeletalMeshComponent.cpp:441`). UE 컴포넌트 기본은 `TG_DuringPhysics`(`Private/Components/ActorComponent.cpp:548`)이나 Rocks에는 그 단계가 없으므로 컴포넌트 기본도 PrePhysics다. DuringPhysics 항목은 그 프레임 물리 결과를 보지 못하므로 PrePhysics와 관찰 결과가 같다 |
  | Physics(고정 스텝) | `TG_StartPhysics`·`TG_EndPhysics`(UE도 Hidden — 엔진 전용) | 사용자 등록 단계는 그대로이고, phase 끝에 엔진 단계 둘을 이 순서로 둔다. (1) **물리 결과 반영**(옛 PhysicsSync): body transform을 컴포넌트에 쓰고 고정 스텝 동안 쌓인 충돌·trigger 통지를 스텝 순서대로 발행한다. 고정 스텝이 여러 번이어도 반영·발행은 프레임에 한 번이다(UE `FinishPhysicsSim` — `Private/PhysicsEngine/PhysLevel.cpp:170, 225–234`, 충돌 통지가 EndPhysics에서 나간다 — `Private/LevelTick.cpp:1767`). (2) **skeletal mesh 물리 마무리**(옛 AnimationFinalize): ragdoll·물리 blend 결과를 본 transform에 반영한다(UE `EndPhysicsTickFunction` = `TG_EndPhysics` — `SkeletalMeshComponent.cpp:452`, `Private/SkeletalMeshComponentPhysics.cpp:3419`) |
  | PostPhysics | `TG_PostPhysics` | 물리 결과가 필요한 항목(UE spring arm — `Private/GameFramework/SpringArmComponent.cpp:22`) |
  | LateUpdate | 카메라 갱신 + `TG_PostUpdateWork` | UE는 PostPhysics 뒤 타이머·tickable → 카메라 갱신 → PostUpdateWork 순이다(`Private/LevelTick.cpp:1835, 1877`). LateUpdate 시작에 엔진의 카메라 갱신(PlayerController 카메라 관리, S12)을 두고 등록 항목은 그 뒤에 실행한다 |
  | PreRender | end-of-frame 갱신(렌더 상태 전송, `Private/LevelTick.cpp:1826`) | 렌더 추출 준비 |
  | (없음) | `TG_LastDemotable`·`TG_NewlySpawned` | 같은 프레임에 새로 생긴 항목은 시작 큐 규칙(7.2절)대로 다음 프레임이다 |

  - 애니메이션 pose 평가는 skeletal mesh의 PrePhysics Tick에서 한다. 병렬 평가(병렬 Tick과 함께, 후속)를 들이면 물리 body가 있는 mesh는 PrePhysics 끝에서 합류하고, 없는 mesh는 PostPhysics 끝까지 합류를 늦출 수 있다(UE end tick group — `SkeletalMeshComponent.cpp:2084–2086`).
  - cloth는 스펙이 없다. UE는 PrePhysics에서 시작해 PostPhysics에서 끝나는 병렬 작업이다(`SkeletalMeshComponent.cpp:457–458`). cloth 스펙이 같은 자리로 정한다.
  - UE DuringPhysics의 "물리와 병렬" 등록 종류는 물리를 비동기(Jolt job)로 돌리고 병렬 Tick을 들일 때 다시 본다.
- 등록 기반이다. 등록 단위는 `(대상 핸들, 종류, hook, phase, priority, interval, pause 정책)`이며 `TickHandle`(slot+generation)로 식별한다.
- 단계 순서(열거 순서 = 실행 순서)는 `PrePhysics → Physics(고정 스텝 수만큼) → PostPhysics → LateUpdate → PreRender`다. LateUpdate(카메라 추적 등 후처리)가 PreRender(렌더 추출 준비)보다 먼저 와야 같은 프레임 렌더에 반영된다.
- phase마다 밀집 배열을 두고, 등록 집합이 바뀐 phase만 다시 정렬한다. 정렬은 (priority, 등록 순서) 최소 힙을 쓰는 위상 정렬이다. 선행 의존으로 묶이지 않은 항목끼리는 항상 (priority, 등록 순서)를 따른다. 순회 중에는 할당하지 않는다(실행 중 해제도 표시만 하고 단계 경계에서 지운다).
- 순서 배열은 흔한 변경에서 다시 만들지 않고 고친다(2026-11-18 — 재구성이 모든 틱에 비례해 틱 Actor 하나를 만든 프레임마다 Actor 10K에서 1.6ms, 100K에서 22ms가 더 들었다). 결과 순서는 처음부터 재구성한 순서와 같다:
  - 새 등록은 선행이 없고 등록 순서가 가장 늦다. priority가 그 단계 순서 배열의 최댓값 이상이면 재구성해도 맨 끝이므로 끝에 붙인다. 실행 중에는 배열을 바꾸지 않는다(다음 경계의 재구성).
  - 해제는 남은 항목의 순서를 바꾸지 않으므로 순서 배열에 남겨 실행이 건너뛴다(세대가 다른 handle). 남은 칸이 배열의 절반을 넘거나, 해제한 항목을 선행으로 둔 항목이 있으면(항목마다 `dependents` 계수) 재구성한다 — 풀린 의존이 순서를 바꾸기 때문이다.
  - 선행 추가는 지금 순서에서 이미 선행이 앞이면 재구성하지 않는다(그 간선이 막는 선택은 원래 순서에서도 일어나지 않았다). 선행 제거는 재구성한다.
  - 사이클 검사(`Reaches`)는 호출 번호 표시로 방문한 칸만 본다(호출마다 표시 배열을 비우지 않는다 — 번호가 한 바퀴 돌 때만 비운다). 실행 중 미룬 해제는 목록으로 지운다(전 항목을 훑지 않는다 — 목록 용량은 등록 때 확보).
  - 한계: priority가 최댓값보다 낮은 등록과 선행 제거는 지금처럼 단계 전체를 다시 만든다.
- 선행 의존은 같은 phase 안에서만 허용한다. 사이클은 `AddPrerequisite` 호출 시점에 방문 표시 탐색(선형 시간)으로 검출해 거부한다. 다른 phase 간 순서는 phase 순서로 보장된다.
- Component Tick은 같은 hook의 소유 Actor Tick 뒤에 오도록 World가 선행 의존을 자동으로 건다(UE의 기본 순서와 같다).
- interval은 대상별 누적으로 판정하고 상대 허용오차(1e-9)로 합산 오차를 흡수한다. 남은 누적은 다음 주기로 이월해 평균 주기를 interval에 맞추되 한 번에 여러 주기를 따라잡지 않는다. delta는 마지막 실행 이후 흐른 시간이다.
- pause 정책: 기본은 World pause 시 정지, `TickWhenPaused`는 unscaled delta로 계속 실행한다. 디버그 단일 스텝 프레임은 pause가 아닌 것처럼 실행한다.
- 병렬 Tick은 후속이다. 등록 단위가 읽기·쓰기 선언을 추가할 수 있도록 구조를 남긴다.

## 9. 입력

- `Input` 모듈은 OS와 무관한 `InputEvent`(키 down/up·문자·마우스 이동·버튼·휠·게임패드 연결/해제)와 `InputState`를 정의한다. `InputState`는 한 프레임 동안 변하지 않는 스냅샷이다: 키·마우스 버튼·게임패드 버튼의 현재 눌림과 이번 프레임 edge(`WasPressed`/`WasReleased`), 마우스 위치·이동량·휠, 게임패드 최대 4개의 스틱·트리거(데드존 적용)와 버튼.
- `PlatformWindow`의 입력 backend가 창 메시지(키보드·마우스)와 XInput 폴링(게임패드)을 `InputEvent`로 바꿔 `InputQueue`에 넣는다. 키 식별은 물리 위치 기준 `Key` 열거(스캔코드 기반)이며 문자 입력은 별도 `Text` 이벤트다(IME 텍스트와 키 바인딩 분리).
- 프레임 순서는 계획 8.2절대로 입력 수집이 먼저다: hook `PumpEvents`가 메시지를 펌프하고 게임패드를 폴링한 뒤 `InputState::BeginFrame(queue)`가 큐를 소비해 새 스냅샷을 만든다. edge는 그 프레임에만 유효하다.
- pause 중에도 펌프와 스냅샷 갱신은 계속한다. 게임플레이가 소비하지 못한 edge는 다음 프레임으로 넘기지 않으므로 resume 시 과거 edge를 재발사하지 않는다. 창이 포커스를 잃으면 눌린 키를 모두 해제 이벤트로 정리한다.
- 액션 매핑·리바인딩·컨텍스트 우선순위는 이번 범위가 아니다. PlayerController(S12)가 LocalPlayer의 `InputState`를 읽는 경계까지만 만든다.
- 입력을 월드 이동·시점으로 바꾸는 매핑은 아직 없다(계획). 만들 때는 UE 5.8과 같은 축을 쓴다: 앞 입력은 전방(+X, `GetForwardVector`), 오른쪽 입력(D 키·왼쪽 스틱 +X)은 오른쪽(+Y, `GetRightVector`), 이동 속도는 cm/s, 시점 회전은 `Rotator` 도이며 yaw +는 위에서 볼 때 시계 방향(전방이 오른쪽으로 돈다)이다([수학 구현](../Implementation/Math.ko.md) 2절).
- Sandbox는 WASD·방향키로 스프라이트를 움직이고, 마우스 위치로 삼각형을 회전시키며, 좌클릭으로 색을 바꾸고, 게임패드 왼쪽 스틱도 같은 이동에 연결해 확인한다.

## 10. Graphics

### 10.1 계약

```cpp
namespace rocks::graphics {
enum class Backend : uint8 { D3D12, OpenGL, Vulkan, Metal };

Result<UniquePtr<Device>> CreateDevice(const DeviceDesc&);   // backend 모듈이 제공
class Device {
    Result<SwapChainHandle> CreateSwapChain(const SwapChainDesc&);   // 네이티브 창 핸들
    Result<BufferHandle> CreateBuffer(const BufferDesc&, Span<const uint8> initial);   // 정점·index
    Result<TextureHandle> CreateTexture(const TextureDesc&, Span<const uint8> initial);
    Result<PipelineHandle> CreatePipeline(const PipelineDesc&);      // 정점 배치·셰이더 입력 대조
    Result<CommandContext*> BeginFrame(SwapChainHandle, const ClearColor&);
    Result<GpuFencePoint> EndFrame();          // 제출·present, 완료 지점
    void Retire(Handle);                       // 마지막 사용 fence 뒤 회수
    void ReclaimCompleted();
    // 스레드 안전: LastSubmittedFence·IsFenceComplete·WaitFence·NotifyFence·IsLost
    // 소유권: IsOwnerThread·ReleaseOwnership·AcquireOwnership
};
}
```

- 자원은 slot+generation 핸들이다. `Retire`는 마지막으로 사용한 제출의 fence(`GpuFencePoint`: device epoch·queue·value)가 끝난 뒤 자원과 descriptor를 해제한다. OpenGL도 `glFenceSync`로 같은 규칙을 따른다(즉시 삭제하지 않는다). fence 완료는 다른 스레드에서 기다리거나 통지·작업 이벤트(`Graphics/GpuFenceEvent.h`)로 받는다. 장치 손실은 기다리는 쪽을 `Destroyed`로 깨운다([렌더 기반](RenderBase.ko.md) GX7).
- 장치 스레드 모델([렌더 기반](RenderBase.ko.md) 5.4절 GX11·GX10): 소유 스레드 하나가 제출·present·상태 변경·생성(지금은 생성도 — `DeviceInfo::concurrentResourceCreation` false)을 하고, 소유권은 `ReleaseOwnership`·`AcquireOwnership`으로 넘긴다. 소유 스레드 밖 호출은 Debug·Development에서 `WrongThread`다. 엔진 실행 때는 렌더 lane(RenderBase)이 장치를 소유한다.
- 정점 배치([렌더 기반](RenderBase.ko.md) 10.4절 GX3a): 정점 원소 형식 20종(`GraphicsTypes/VertexFormat.h`), 속성(location·형식·stream·offset)·stream(stride·정점/인스턴스), index 버퍼(UInt16·UInt32), `Draw`·`DrawIndexed`(인스턴스 수), 정점 버퍼 없는 그리기. 셰이더 정점 입력 시맨틱은 `ATTRIBUTEn`이다.
- 셰이더는 backend별 원문을 `PipelineDesc`에 담는다(D3D12: HLSL을 런타임 `D3DCompile` SM 5.1, D3D11: 같은 HLSL을 SM 5.0, OpenGL ES: GLSL ES 3.20, 쿠킹된 `GLES31_PC` 출력과 같은 모양, Vulkan: 같은 HLSL을 개발용 컴파일 훅 `DeviceDesc::shaderCompiler`가 DXC로 SPIR-V로, 10.3절). 오프라인 셰이더 쿠킹은 후속이다.
- 그리기 상수는 64바이트 행렬 + 16바이트 색으로 제한한다(D3D12 root constants, OpenGL ES 루트 상수 에뮬레이션 배열 `vu_h`·`pu_h`, Vulkan push constant).
- 좌표 규약: 2026-11-17 사용자 결정으로 좌표계·단위·행렬 규약을 UE 5.8과 같게 바꾼다(행 벡터 `p·M`·행 우선 저장, 셰이더 `mul(v, M)`·행 우선 packing, 앞면은 화면 반시계, [셰이더 시스템](ShaderSystem.ko.md) 7.2절 6·8항). 메모리 배치는 전과 같아 GPU로 가는 바이트는 바뀌지 않는다. 렌더 코드는 C7에서 반영했다(2026-11-17): `DrawConstants::transform`은 행 우선, 샘플 렌더러 셰이더는 `mul(float4(position, 1), transform)`, D3D12는 `D3DCOMPILE_PACK_MATRIX_ROW_MAJOR`로 컴파일하고 OpenGL ES는 행 우선 byte를 vec4 배열로 그대로 올려 GLSL ES가 `mat4(vu_h[0..3]) * v`로 쓴다(P11). `PipelineDesc::cullMode`(None·Back·Front, 기본 None)가 컬을 정하고 앞면은 화면 반시계다(D3D12 `FrontCounterClockwise = TRUE`, OpenGL ES는 클립 공간 규약에 맞춘 `glFrontFace`를 장치가 정한다). 샘플 렌더러는 뒤면을 컬하고, 물체 world·view 행렬식 가운데 하나만 음수면 앞면 컬 파이프라인으로 바꾼다. OpenGL ES Windows 미리보기는 모든 패스를 `glClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE)`으로 그리고 스왑체인도 offscreen 텍스처에 그린 뒤 present가 Y를 뒤집어 창에 blit한다(P11, 2026-11-17). 그래서 RenderTarget·스왑체인 모두 첫 행이 이미지 위쪽이다(샘플링·readback 모두 D3D12와 같은 배치). Android는 셰이더가 클립 공간을 보정하고 앞면이 `GL_CW`다([OpenGL ES backend](OpenGLESGraphics.ko.md) 5.3절). 텍스처는 첫 행이 위인 배치로 올리고 UV (0,0)이 좌상단이다.
- 동시에 진행하는 프레임 수는 렌더 lane 프레임 slot(2–3개)이 정하고, slot 재사용은 그 프레임의 모든 제출 fence 완료로 판정한다. D3D12는 끝난 제출의 기록 저장소를 다시 쓴다. 디버그 구성은 디버그 레이어를 켠다.

### 10.2 D3D11Graphics(P15 D3D11 부분)

D3D11은 필수 backend다(2026-11-13 사용자 결정). D3D12의 SM 6.6 기준을 받지 못하는 구형 GPU·드라이버를 위한 Windows 대체 경로이고 셰이더 형식은 `D3D11_SM5`(FXC → DXBC SM 5.0)다. 2026-11-17 사용자 결정으로 SH5보다 앞당겨 구현했다. SH2 전이므로 셰이더는 D3D12와 같이 `PipelineDesc`의 HLSL 원문을 실행 중 `D3DCompile`(`vs_5_0`·`ps_5_0`)로 받고, 쿠킹된 셰이더는 SH2·SH5에서 잇는다. 근거는 UE 5.8 `Runtime/Windows/D3D11RHI`다(아래 경로는 그 아래 기준).

**장치.**
- feature level은 11_1·11_0을 요청하고 11_0을 최소로 둔다(UE `Private/Windows/WindowsD3D11Device.cpp:247–259`).
- 어댑터: `IDXGIFactory6::EnumAdapterByGpuPreference(HIGH_PERFORMANCE)`로 소프트웨어 어댑터를 빼고 차례로 만든다(UE `:701–710`). 하드웨어 후보가 모두 실패하거나 `preferSoftware`이면 WARP(`EnumWarpAdapter`)다. D3D12와 같은 규칙이다.
- 필수 인터페이스는 `ID3D11Device5`·`ID3D11DeviceContext4`와 `ID3D11Fence` 생성이다(아래 "GPU 완료"). 없으면 다음 후보로 넘어가고 WARP까지 없으면 `Unsupported`다(Windows 10 1703 이상, WDDM 2.0 드라이버).
- `D3D11_CREATE_DEVICE_SINGLETHREADED`를 쓰지 않는다. 장치(생성 함수)는 runtime이 직렬화하는 free-threaded 객체가 된다. UE도 비동기 자원 생성을 끄지 않는 한 이 플래그를 빼고(`:1130`), 드라이버 동시 생성 지원은 `D3D11_FEATURE_THREADING`으로 따로 읽는다(`:1466–1469`).
- 검증: `enableValidation`이면 `D3D11_CREATE_DEVICE_DEBUG`와 DXGI debug factory를 켠다. SDK layer가 없으면(`DXGI_ERROR_SDK_COMPONENT_MISSING`) 끄고 다시 만든다. `ID3D11InfoQueue`와 DXGI info queue에 ERROR·CORRUPTION만 저장하는 filter를 걸고 `ValidationErrorCount`가 센다(D3D12와 같다). UE는 INFO·WARNING을 deny list로 거르고 특정 ID를 뺀다(`:1407–1449`).
- 생성 실패는 `DeviceCreateReport`(LoggingSystem 13절 모양: `code`·`nativeCode`(HRESULT)·`stage`·`detail`)로 돌려준다. 장치 안 실패는 `LastError`에 단계와 HRESULT를 함께 적는다.

**스왑체인.** flip model(`DXGI_SWAP_EFFECT_FLIP_DISCARD`, 버퍼 2개, `R8G8B8A8_UNORM` 버퍼에 sRGB RTV)이다. D3D11 runtime이 flip 버퍼를 돌리므로 버퍼 0과 RTV 하나만 쥔다. UE는 tearing을 지원할 때만 flip model이고 아니면 `DXGI_SWAP_EFFECT_DISCARD`다(`Private/Windows/WindowsD3D11Viewport.cpp:75–94`). Rocks는 Windows 10 이상만 대상으로 하므로 늘 flip model이다. `ResizeBuffers` 전에 버퍼·RTV 참조를 놓고 immediate context의 상태를 비운다.

**context 모델.**
- 소유 스레드 하나가 immediate context로 제출한다(업로드 실행, 프레임 실행, present, fence signal, query 회수).
- 프레임 기록은 deferred context 하나에 한다. `EndFrame`이 `FinishCommandList`로 닫고, 쌓인 업로드를 immediate context에서 실행한 뒤 `ExecuteCommandList`로 프레임을 실행한다.
- 이렇게 해야 GX8 순서 계약(쌓인 업로드는 그 제출보다 먼저 실행되고, 기록 중에 갱신하면 그 프레임 전체가 새 내용을 본다)이 D3D12와 같다. 기록 객체를 deferred context로 두는 모양은 병렬 기록(GX4·RG3)에서 기록 스레드마다 하나로 늘리면 된다.
- UE는 deferred context를 쓰지 않고 immediate context 하나를 RHI 스레드가 쓴다. 드라이버가 command list를 지원하지 않으면(`DriverCommandLists` 거짓) runtime이 기록·재생을 흉내 낸다. 이때 비용만 늘고 동작은 같다.

**GPU 완료(GX7).**
- `ID3D11Fence`를 쓴다. `ID3D11DeviceContext4::Signal` 바로 뒤 `Flush`해 신호가 GPU에 닿게 한다. fence 관찰 스레드가 `SetEventOnCompletion`으로 기다리는 `FenceTimeline` 구조와 장치 손실 판정(완료 값 `UINT64_MAX`, `GetDeviceRemovedReason`)은 D3D12와 같다.
- UE는 event query(`D3D11_QUERY_EVENT`)를 RHI 스레드가 polling한다(`Private/D3D11Commands.cpp:1648–1690`). 그러나 `GetData`는 immediate context 전용이어서 GX7의 "어느 스레드에서나 기다린다"를 지킬 수 없다. 그래서 fence가 없는 장치는 지원하지 않는다(위 필수 인터페이스).
- 회수 규칙(`Retire` → 마지막 사용 fence 뒤 해제, 기록 중이면 그 프레임의 fence)은 D3D12와 같다. D3D11 runtime이 쓰는 중인 객체를 스스로 늦게 지우더라도, 계약(`PendingRetireCount`, 해제 시점)이 backend마다 같도록 장부를 그대로 둔다.

**업로드·readback(GX8).**
- D3D11에는 계속 map해 둔 채 GPU가 복사할 수 있는 업로드 heap이 없다. map은 immediate context 전용이고, map 중인 자원은 복사 원본이 될 수 없다.
- 그래서 업로드 ring(`UploadRingAllocator`, 기본 4 MiB)과 스트리밍 staging pool은 시스템 메모리다. 갱신(`UpdateBuffer`·`UpdateTexture`·`Upload*FromStaging`)은 "ring·staging에 쓰기 → 복사 기록"으로 쌓인다. 소유 스레드의 다음 제출이 immediate context에서 `UpdateSubresource`로 실행한다.
- `UpdateSubresource`는 원본을 호출 안에서 드라이버 내부 staging으로 복사한다. 그래서 GPU 복사가 한 번 늘지만 ring 구간은 그 묶음의 fence 뒤에 회수한다(D3D12와 같은 보수적 규칙, 결과가 완료 시점에 좌우되지 않는다).
- UE도 정적 버퍼 쓰기 lock은 CPU 메모리를 잡았다가 unlock 때 올리고(`Private/D3D11Buffer.cpp:278`, `:320`), 텍스처 갱신은 `UpdateSubresource`다(`Private/D3D11Texture.cpp:1188,1220`).
- 생성 초기 데이터는 `D3D11_SUBRESOURCE_DATA`로 생성 호출 안에서 올린다(장치 호출이라 스레드 안전, 생성 즉시 내용이 정해진다).
- staging에서 올리는 경로는 행 정렬 요구가 없어 언제나 그대로 올린다. D3D12처럼 ring으로 다시 옮기는 경로가 없다.
- readback: `D3D11_USAGE_STAGING` 텍스처로 `CopyResource`하고 fence가 끝난 뒤에만 `Map(READ)`으로 읽는다(UE `Private/D3D11RenderTarget.cpp:520–563`과 같은 staging 경로). `D3D11_MAP_FLAG_DO_NOT_WAIT`은 쓰지 않는다. 하드웨어 드라이버(GTX 1060)에서 runtime의 자원 사용 추적이 fence보다 늦게 풀려, 끝난 복사에도 `DXGI_ERROR_WAS_STILL_DRAWING`을 돌려주는 것을 확인했다. fence 뒤의 map은 GPU를 기다리지 않는다.
- 그리기별 상수(D3D12 루트 상수 대응, RenderBase 결정 R7·SX2)는 128 byte 동적 상수 버퍼(b0, VS·PS·CS) 하나다. 파이프라인이 선언한 크기(`drawConstantBytes`)만 셰이더가 읽고, `SetDrawConstants`마다 CPU 사본 전체를 deferred context에서 `Map(WRITE_DISCARD)`한다(UE uniform buffer와 같다, `Private/D3D11UniformBuffer.cpp:161`). `NO_OVERWRITE`는 deferred context의 상수 버퍼에 11.1 기능(`MapNoOverwriteOnDynamicConstantBuffer`)이 필요해 쓰지 않는다. SH2·P2의 b13 그리기 상수(셰이더 시스템 7.2절 4항)로 옮기는 것은 SH5다.

**장치 능력·버퍼(GX1·GX2, 2026-11-18).**
- `DeviceCaps`는 d3d11.h 상수와 feature level(FL 11.0은 UAV 8·VS UAV 없음, 11.1 이상은 UAV 64·모든 단계 UAV), `D3D11_FEATURE_D3D11_OPTIONS3`(VS 층 출력), DXGI 출력 HDR 색 공간이다. 64 bit atomic·multi draw indirect는 거짓이다. MSAA 표본 수는 `CheckMultisampleQualityLevels`다.
- 버퍼 용도는 bind·misc flag로 옮긴다(UE `Private/D3D11Buffer.cpp:38–83`). raw SRV는 `D3D11_SRV_DIMENSION_BUFFEREX` + RAW다. 버퍼 view slot n은 VS·PS `t(n + 1)`(`VSSetShaderResources`·`PSSetShaderResources`).
- 상수 버퍼는 `UpdateSubresource`에 대상 구간을 줄 수 없어(debug layer 288) 전체 갱신만 받고 부분 갱신은 `Unsupported`다. WARP가 `RGB9E5Ufloat` 버퍼 SRV 생성에서 멈추므로 WARP에서만 그 형식의 texel buffer 능력을 공개하지 않는다.
- `ReadBuffer`는 staging 버퍼로 구간 `CopySubresourceRegion`(원본 box) 뒤 fence를 기다려 `Map(READ)`이다.

**파이프라인·명령 기록 확장(GX3·GX4, 2026-11-18).**
- 파이프라인마다 blend(타깃별, `IndependentBlendEnable`, alpha-to-coverage)·래스터(컬·깊이 bias·clamp, scissor는 늘 켬)·깊이·스텐실 상태 객체와 기본도형을 만든다. compute는 `cs_5_0`이고 CS 상수는 그리기 상수 버퍼 slot 0을 같이 쓴다.
- `BeginPass`는 RTV 0–8과 DSV(읽기 전용 깊이는 깊이 텍스처마다 만든 `D3D11_DSV_READ_ONLY_*` view)를 묶고 load 동작을 `ClearRenderTargetView`·`ClearDepthStencilView`로 한다. 스텐실 기준은 깊이·스텐실 상태와 함께 건다.
- dispatch는 CS에 읽기 SRV(t1–t4)·UAV(u0–u3)를 묶고 끝나면 풀며, 그래픽 SRV는 그리기 직전에 묶는다(같은 버퍼의 SRV·UAV가 겹쳐 런타임이 binding을 풀지 않게). dispatch 뒤 첫 그리기는 정점·index 버퍼를 다시 묶는다. 간접 명령은 `DrawInstancedIndirect`·`DrawIndexedInstancedIndirect`·`DispatchIndirect`, 복사는 `CopySubresourceRegion`, UAV clear는 `ClearUnorderedAccessViewUint`다.

**GPU 계측(GX9).**
- timestamp: query 객체(slot 4 × 256개)를 deferred context에서 `End`로 기록한다. 프레임 실행(`ExecuteCommandList`) 앞뒤를 slot별 `D3D11_QUERY_TIMESTAMP_DISJOINT`로 감싼다.
- 회수는 fence 완료 뒤 소유 스레드가 `GetData(DONOTFLUSH)`로 한다. 아직 값이 없으면 미완료로 남긴다.
- 주파수는 장치를 만들 때 disjoint query 한 번으로 얻어 `DeviceInfo::timestampFrequency`로 공개한다. disjoint 표시가 섰거나 주파수가 공개값과 다른 slot의 결과는 모두 disjoint다. 그래서 `timestampDisjointDetection`은 참이다.
- calibration: D3D11에는 `GetClockCalibration`이 없다. `CalibrateClocks`는 제출한 GPU 작업이 끝나기를 기다린 뒤 CPU 시각 t0을 읽는다. 이어서 timestamp(disjoint로 감쌈)와 event query를 넣고 `Flush`한 뒤, event가 끝날 때까지 `GetData`를 돌며 끝을 본 CPU 시각 t1을 읽는다.
  - timestamp는 t0과 t1 사이에 찍혔으므로 `cpuTicks`는 가운데, `uncertaintyNanoseconds`는 (t1 − t0)/2다. disjoint면 몇 번 다시 하고, 그래도 안 되면 실패다.
  - GPU가 쉬기를 기다리므로 비싸다. GpuTrace는 `calibrationIntervalFrames`(기본 60)마다만 부른다.
  - UE는 장치를 만들 때 한 번 같은 방식으로 재고 끝을 본 polling 시각을 CPU 시각으로 둔다(`Private/D3D11Query.cpp:389–498`).
- marker: deferred context에서 얻은 `ID3DUserDefinedAnnotation`의 `BeginEvent`·`EndEvent`(UTF-16)다. 얻지 못하면 `debugMarkers`가 거짓이다.

**스레드 모델(GX10·GX11).** `concurrentResourceCreation`은 참이다. 생성은 free-threaded 장치 호출이고, 갱신·staging 업로드는 내부 잠금 아래 CPU 쪽 업로드 기록에 쌓인다. immediate·deferred context는 소유 스레드만 만진다. context는 스레드에 묶이지 않으므로 소유권 이관은 검사만 바꾼다.

**파이프라인·정점 배치(GX3a).**
- 입력 layout은 파이프라인마다 VS 바이트코드로 만든다. 시맨틱은 `ATTRIBUTEn`, `InputSlot`은 stream이고, 인스턴스 stream은 `D3D11_INPUT_PER_INSTANCE_DATA`·간격 1이다(UE `Private/D3D11VertexDeclaration.cpp:57–62`). 속성이 없으면 입력 layout을 묶지 않는다.
- 셰이더 입력 대조는 `ID3D11ShaderReflection`으로 D3D12와 같은 규칙을 적용한다. `Bgra8Unorm`은 `CheckFormatSupport`의 IA 정점 버퍼 지원이 없으면 `Unsupported`다.
- 래스터: `FrontCounterClockwise = TRUE`(UE `Private/D3D11State.cpp:211`, D3D12와 같다), 깊이 clip 켬, scissor 끔. 컬 모드(`PipelineDesc::cullMode`)마다 래스터 상태를 하나씩 만들어 파이프라인이 고른다. HLSL은 `D3DCOMPILE_PACK_MATRIX_ROW_MAJOR`로 컴파일한다(D3D12와 같은 행 우선 규약).
- `CopyPassTarget`은 deferred context의 `CopyResource`다(백버퍼 `R8G8B8A8_UNORM`과 RenderTarget sRGB는 같은 typeless 묶음).
- 블렌드·래스터·깊이 상태 객체는 장치가 한 번 만들어 파이프라인끼리 공유한다.
- 자원 전이(GX5)는 없다. D3D11 runtime이 hazard를 추적한다.

**공개 제약 요약.** fence(`ID3D11Fence`) 없는 장치는 `Unsupported`, 업로드는 드라이버 복사가 한 번 더 있다, calibration은 GPU 대기를 포함한다, timestamp 주파수가 바뀌면 그 구간을 disjoint로 버린다(추정값을 만들지 않는다).

**self-test.** Sandbox `--self-test`가 D3D11 하드웨어와 WARP에서 D3D12와 같은 검사를 한다. 검사 항목은 픽셀, 제출·완료·회수, 소유권, 스왑체인, 정점 형식 20종, 업로드·readback, 계측이다. 그 결과를 D3D12와 픽셀 대조한다. 검증 계층 집계는 index 용도가 아닌 버퍼를 index 버퍼로 묶는 의도적 오류로 확인한다. 이 오류는 WARP 장치를 제거하므로 검사용 장치와 따로 만든 단기 장치에서 한다.

**UE 대응표.**

| Rocks | UE 5.8 D3D11RHI |
|---|---|
| `D3D11Graphics` 모듈, `CreateD3D11Device` | `D3D11RHI`, `FD3D11DynamicRHIModule::CreateRHI` |
| 프레임 기록 deferred context | (없음, immediate context + RHI 스레드) |
| fence 관찰 스레드(`ID3D11Fence`) | `FD3D11Sync`(event query polling) |
| 업로드 ring(시스템 메모리) + `UpdateSubresource` | `FD3D11LockedData` + `UpdateSubresource` |
| 그리기 상수 동적 버퍼 | `FWinD3D11ConstantBuffer`·uniform buffer `Map(WRITE_DISCARD)` |
| `CalibrateClocks` | `FD3D11DynamicRHI::CalibrateTimers` |
| `PushMarker`·`PopMarker` | `Direct3DDeviceIMAnnotation->BeginEvent`·`EndEvent` |

### 10.3 VulkanGraphics(P9)

Vulkan은 모바일 주력이자 필수 backend다. ShaderSystem P9의 산출물로 SH2 전에 구현했다(2026-11-17 사용자 지시 "Vulkan까지는 작업을 미리하자"). 설계와 UE 5.8 VulkanRHI 조사, 계약별 대응은 [VulkanGraphics](VulkanGraphics.ko.md)에 있고 여기에는 요지만 둔다.
- 기준은 Vulkan 1.1(Android)이다. 로더는 시스템 `vulkan-1.dll`·`libvulkan.so`를 P10 `OpenSystem`으로 연다. 선택 확장은 swapchain·calibrated timestamps·debug utils뿐이고 없으면 `Unsupported`·false로 공개한다.
- 완료 추적은 제출마다 `VkFence`이고, render pass는 `VkRenderPass`, 메모리는 자체 페이지 할당기(Graphics `GpuPageAllocator`)다.
- 창 출력은 중간 RGBA8 이미지에 그리고 `EndFrame`에서 swapchain 이미지에 blit한다(UE 기본 경로와 같다).
- 앞면은 음수 viewport 높이 + `VK_FRONT_FACE_COUNTER_CLOCKWISE`·`Back → BACK_BIT`다. UE 매핑(`CLOCKWISE`·`CM_CW → FRONT_BIT`)은 `SV_IsFrontFace`가 D3D12와 반대라 따르지 않는다.
- 셰이더는 실행 중 컴파일러가 없으므로 개발 프로그램이 Developer `ShaderFormatVulkan`(DXC `VULKAN_SM5` 옵션과 SPIR-V 재작성)을 `DeviceDesc::shaderCompiler`로 넘긴다. Release 실행 파일에는 컴파일러가 없다(SH2 전에는 Vulkan 파이프라인이 `Unsupported`).
- 확인: Sandbox self-test 전체가 GTX 1060과 lavapipe(소프트웨어 ICD, 측정 S4)에서 통과하고 D3D12와 픽셀 대조가 차이 0이다(VulkanGraphics 17절).

### 10.4 범위 밖

RenderGraph·Deferred/Forward 경로·깊이 버퍼·MSAA·멀티 큐·bindless·Metal backend 구현은 이번 범위가 아니다.

## 11. 에셋

- `AssetId`는 `Guid`다. 에셋 파일(`.rasset`)은 `magic "RAST" · 형식 버전 · 에셋 종류 · AssetId · 페이로드`를 Serialization `ArchiveWriter`로 기록한다.
- `TextureAsset`: 폭·높이·형식(RGBA8 sRGB)·픽셀. 행 순서는 위→아래.
- `AssetManager`: 루트 디렉터리를 마운트해 에셋 파일 헤더를 읽고 `AssetId ↔ 경로` 목록을 만든다. `Load(AssetId)`는 참조 계수 핸들을 반환한다. 마지막 핸들이 사라지면 언로드한다. 이번 단계의 로드는 동기이며 비동기 로딩(`AsyncReadFile`, 로드 상태 Pending/Ready/Failed)은 후속이다.
- `BitmapImporter`(AssetTools): 비압축 BMP(24·32비트, `BI_RGB`·`BI_BITFIELDS`, 상향·하향 행)를 RGBA8로 바꾼다. 압축 BMP·팔레트는 `Unsupported`다. Commandlet `import-texture <src.bmp> <dst.rasset>`로 실행한다.
- GPU 텍스처는 `WorldIntegration`의 텍스처 캐시가 AssetId별로 만든다. 에셋 데이터와 GPU 자원의 수명은 분리한다.

## 12. 실행 경로(S6 완료 시)

1. Sandbox가 창과 Graphics 장치(`--graphics=d3d12|opengl`)를 만든다.
2. 렌더 lane(RenderBase `RenderLane`)을 시작해 장치 소유권을 lane으로 넘긴다. lane이 스왑체인·렌더러 자원을 만든다(표면 작업·`RenderLaneProcessor::OnLaneStart`).
3. Engine을 초기화하고 World를 하나 만든다. AssetManager가 `Content/`를 마운트한다.
4. 삼각형 Actor(매 Tick 회전)와 SpriteComponent를 가진 Actor(텍스처 에셋)를 Spawn한다. 텍스처는 `RenderHandle`을 예약하고 lane의 자원 작업이 만든다.
5. 매 프레임 `FrameEnd` hook이 프레임 slot을 확보하고(빈 slot이 없으면 가장 오래된 slot의 GPU 완료를 창 메시지를 펌프하며 기다린다), World에서 추출한 그리기 항목을 slot 프레임 arena에 담아 프레임 packet으로 넘긴다. 렌더러는 렌더 lane 안에서 그린다(지금은 Sandbox 샘플 렌더러, 이후 정식 Renderer).
6. 창을 닫으면 `RequestExit` → Engine 종료 hook(CP6)이 렌더 lane 종료 순서를 실행한다([렌더 기반](RenderBase.ko.md) 5.5절): 새 프레임 차단 → World 파괴 → 진행 중 프레임 GPU 완료 → 최종 GC와 Retire drain 반복 → 내장 자원·스왑체인 Retire와 closure 확인 → lane Drain·장치 소유권 반납·Join → 장치 `WaitIdle`·소멸 → 창 파괴.

## 13. 시험

- `Engine` 타깃의 헤드리스 테스트: GameClock, Engine 상태 기계·실패 복구, World·Actor·Component 수명 순서, Tick phase·priority·interval·pause·지연 변경, 복수 World 격리, 지연 파괴, BMP 파싱, 에셋 파일 왕복, AssetManager 참조 계수.
- World 리플렉션(B8b — `WorldReflectionTests`·`WorldTests`·`EngineTests`): 네 타입의 WorldOnly·필드 형태와 멱등 등록, native 파생의 base 상속, GC(Spawn 생존·제거 회수·native 파생의 Component 생존·Marking 중 AddComponent의 newborn shade·Marking 중 참조 쓰기의 삽입 장벽·root 참조 방문과 파괴 시 비우기·우회 파괴 수리), Level 그래프 저장 → 별도 Registry 로드 → 필드·간선·root·소프트 참조 → 재저장 byte 동일(NATIVE 기본값, 계층 비저장), native 파생이 든 Level 저장 Unsupported, 장부 밖 객체 API 거부, null 원소 순회, 경로 읽기와 World 필드 쓰기 거부·fixture 쓰기와 옛 값, Engine 등록 순서와 native 파생을 둔 Shutdown·초기화 실패 정리.
- 규모 경로(2026-11-18 — 실험 16절): 같은 Level에서 끝 원소를 포함한 여러 Actor의 World API 밖 파괴, 우회 파괴와 `DestroyActor`가 같은 프레임에 겹치는 경우(둘 다 `Level::Actors()`에 남지 않고 회수된다), `RemoveReferenceAtSwap`(끝 원소가 DestroyRequested여도 성공, 장부·관찰자), `TakeDestroyRequests`(꺼낸 뒤 비고, 회수된 요청은 빠지고, 묶음이 작으면 나눠 꺼낸다), Level 우회 파괴(GC idle 중·cycle 도중 — 소속 Actor 전부 정리·회수), Component 단독 우회 파괴(틱 있음·없음), Tick hook·OnDestroy hook 안의 우회 파괴, Level encode 고정 위반의 Fatal 메시지(판정 함수), hook 안에서 활성화된 꺼진 Actor의 시작, TickScheduler의 끝에 붙이기·이미 지켜지는 선행 추가의 재구성 생략·의존받는 항목의 미룬 해제·도달 검사 번호 한 바퀴(모듈 Private의 `TickSchedulerTestAccess.h` — 시험 실행 파일은 엔진 계층에도 `Engine/Source/Runtime`을 include 경로로 둬 `<Module/Private/...>`로 읽는다). 각 시험은 고친 지점마다 음성 대조(옛 구현·잘못된 조건을 넣으면 실패)로 확인한다.
- GPU 시험은 별도 실행(`Sandbox --self-test`): D3D12(WARP 허용)와 OpenGL에서 offscreen 또는 backbuffer readback으로 삼각형·텍스처 픽셀을 확인한다. GPU 없는 CI에서는 실행하지 않고 NOT RUN으로 보고한다.
- 각 단계는 Debug·ASan 테스트와 `clang-format` 검사를 통과한 상태로 커밋한다.

## 14. 알려진 제한

- World 타입의 Level 그래프 저장은 구조 시험용이다: 부착 계층은 저장되지 않고(리플렉션 B9의 WeakObjectRef PROPERTY와 World 단계의 결정 뒤), native 파생이 든 그래프는 저장할 수 없으며, 로드한 객체는 레벨 편입 단계 전까지 World 장부 밖이다(4.6절). World 필드는 ReadOnly라 CU5 setter hook 전에는 경로로 편집할 수 없다.
- Tick마다 Borrow를 얻는 잠금 비용이 있다. 일괄 접근은 측정 후 도입한다.
- 렌더 lane(RB1)과 프레임 slot은 구현했다. 렌더 설정 snapshot(CP4), 대체 자원(RB2), 장치 재생성은 후속이다. 지금 Sandbox의 엔진 루프 경로는 self-test(`--self-test`)의 헤드리스 장면이며, World 추출(WorldIntegration)과 창 실행 경로는 화면 마일스톤에서 잇는다.
- macOS·Metal은 미구현이며 해당 경로는 `Unsupported`를 반환한다. Vulkan은 SH2 전까지 개발용 컴파일 훅이 있어야 파이프라인을 만든다(10.3절).
