# 엔진 런타임·월드·그래픽 최소 경로 설계

기준일: 2026-11-12. 이 문서는 Core·Object 위에 처음 올리는 엔진 계층의 구조와 계약을 정한다. 범위는 **엔진 틱 → 월드 → Actor/Component → 텍스처 에셋 → D3D12·OpenGL로 화면에 그리기**까지의 수직 경로와, 그 위에 올릴 게임플레이 프레임워크(GameInstance·GameDirector·GameState·Controller·Pawn)의 골격이다. 전체 계획은 [엔진 구현 계획](EngineImplementationPlan.ko.md)을 따르며, 이 문서와 충돌하는 부분은 개정 1.8에서 이 문서 기준으로 바꾼다.

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
| S7–S14 | Tick 의존성 완성, Subsystem 의존성, Actor 수명 완성, GameInstance, GameDirector·GameState, Controller·Pawn, 초기화 흐름, 진단·문서 | 붙임 계획 24절의 14단계 시나리오와 Director 없는 World 시나리오 |

S4·S5는 S2·S3과 독립이다. 이번 범위에서 macOS는 창·Metal 구현이 없으므로 빌드·실행 검증을 하지 않는다.

## 2. 명칭

- 게임 규칙 조율 객체는 **GameDirector**, 진행 상태는 **GameState**, 제어 주체는 **Controller/PlayerController**, 제어 대상은 **Pawn**, 월드 배치 객체는 **Actor**다. 기존 계획 20.4·20.5절의 MatchRules·MatchState·PlayerAgent·ControlledEntity·Entity 명칭은 이 이름으로 대체한다.
- 그래픽 장치 추상화는 **Graphics**(`rocks::graphics`)다. 타입에 접두사를 붙이지 않는다(`graphics::Device`, `graphics::SwapChain`). 기존 계획의 "RHI" 표기는 Graphics로 바꾼다.
- 수명 hook은 계획 20.2절(2026-11-12 결정)을 따른다: `OnAwake`(Initialize), `OnEnable`, `OnStart`(BeginPlay), `OnTick`·`OnFixedTick`·`OnLateTick`, `OnDisable`, `OnDestroy`(EndPlay).
- Tick phase는 `PrePhysics`, `Physics`, `PostPhysics`, `LateUpdate`, `PreRender`(실행 순서)다. `Physics` phase만 고정 스텝으로 실행한다.

## 3. 모듈과 의존 방향

모든 엔진 모듈은 `Engine/Source/<Group>/<Module>/`에 두고 `EngineModuleRules`를 상속한다.

| 모듈 | 의존 | 책임 | 금지 |
|---|---|---|---|
| `Input` | Core | 장치 독립 입력 이벤트·프레임 입력 상태(키·마우스·게임패드), edge 판정 | 창·OS SDK 참조 |
| `PlatformWindow` | Input | 창 생성·이벤트 펌프·네이티브 핸들 노출, 입력 backend(Win32 메시지 키보드·마우스, XInput 게임패드) | Graphics·World 참조 |
| `GraphicsTypes` | Core | 그래픽 값 타입: 픽셀 형식·mip 크기 계산(`PixelFormat.h`), 필터·주소 모드·테두리 색·`SamplerDesc`(`Sampler.h`). 장치 계약·에셋·도구가 함께 쓴다 | 장치·창·World 참조 |
| `Graphics` | GraphicsTypes | 장치·스왑체인·버퍼·텍스처(mip·텍스처별 샘플러)·파이프라인·명령 기록 계약, backend 열거 | World·Object·창 모듈 참조 |
| `D3D12Graphics` | Graphics | D3D12 구현(Windows) | 다른 backend 참조 |
| `OpenGLGraphics` | Graphics | OpenGL 4.5 core 구현(Windows WGL) | 〃 |
| `VulkanGraphics`·`MetalGraphics` | Graphics | 자리만 둔다. 장치 생성은 `Unsupported`를 반환한다 | 동작하는 척하는 가짜 구현 |
| `Engine` | ObjectRuntime, World | 엔진 상태 기계·메인 루프·WorldContext·EngineSubsystem | Gameplay·Graphics·창 참조 |
| `World` | ObjectRuntime, CoreTypeBindings | World·Actor·Component·SceneComponent(WorldOnly 리플렉션 클래스 — 4.6절)·TickScheduler·WorldSubsystem | Engine·Graphics·Asset 참조 |
| `WorldTestFixtures`(Developer) | World | World 타입을 상속한 리플렉션 시험 타입(SaveActor·SaveComponent)과 그 identity 명세. EngineTests만 의존 | 제품 링크 |
| `Gameplay` | Engine, World | GameInstance·GameDirector·GameState·Controller·Pawn·LocalPlayer | Graphics 참조 |
| `AssetRuntime` | Serialization, Core | AssetId·에셋 파일 형식·AssetManager·TextureAsset | World·Graphics 참조 |
| `AssetTools`(Developer) | AssetRuntime | BMP 임포터·에셋 파일 기록 | Shipping 링크 |
| `Renderer` | Graphics | 내장 셰이더·RenderScene(평범한 데이터) 그리기 | World 순회 |
| `WorldIntegration` | World, Renderer, AssetRuntime | SpriteComponent 등 렌더 컴포넌트, RenderScene 추출, 텍스처 GPU 캐시 | Engine 내부 상태 변경 |

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
// 헤더가 필요하다.
#include <World/World.module.generated.h>
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
- World가 만든 목록에는 null이 없지만 로드된 목록에는 null 원소가 올 수 있다(`Required`는 로드에서 진단만 한다). 그래서 World의 Component 목록 순회(FindComponent·FinishSpawning·활성화·시작·DestroyActor 등)는 null을 끝 표지로 쓰지 않고 원소 수 기준으로 돌며 null을 건너뛴다.

### 4.6 리플렉션과 레벨 편입(A0-B B8b)

멤버 분류([리플렉션·빌드 보강 설계](ReflectionFoundations.ko.md) 9.1절, 결정 21):

| 타입 | PROPERTY(영속, 모두 ReadOnly — 도구 이름) | NATIVE(런타임) |
|---|---|---|
| Level | `actors`(ObjectRefArray<Actor>, Required) · `name` | World 포인터 · 영구 여부 · 파괴 대기 |
| Actor | `components`(ObjectRefArray<Component>, Required) · `rootComponent`(ObjectRef<SceneComponent>) · `enabled` | World 포인터 · 소속 Level(actors 포함 관계에서 유도) · Tick 설정·Tick 켜짐 |
| Component | `enabled` | World 포인터 · 소유 Actor · Tick 설정 · 수명 기록 |
| SceneComponent | `relativeTransform`(Transform — CoreTypeBindings 미러) | 월드 행렬 캐시 · dirty · parent · children(유도값) |

- **ReadOnly**: 경로·리플렉션 쓰기가 World 장부(ActorRecord·hook·MarkDirty·Tick 등록)를 우회하지 않게 World 필드는 모두 ReadOnly다(로드의 staged 쓰기는 허용). 값은 World API(`SetEnabled`·`SetRootComponent`·`SetRelativeTransform` 등)로 바꾼다. 그래서 CU5 setter hook 전에는 에디터에서 `relativeTransform`을 경로로 편집할 수 없다(UE `EditAnywhere`와 다른 제약).
- **Tick 설정은 NATIVE**다(UE `PrimaryActorTick`은 archetype 차분 직렬화 — Rocks에는 아직 클래스 기본값 레코드와 차이 저장이 없어 인스턴스 저장값이 클래스 기본값 변경을 가린다). 레코드와 struct 멤버 단위 비교가 생기는 A5에서 리플렉션 struct·enum으로 옮기며, 그래서 `TickSettings`를 단순 값 구조로 둔다([RuntimeClassMetadata](RuntimeClassMetadata.ko.md) '클래스 기본값 결정').
- **부착 계층은 NATIVE**다(parent는 다른 Actor의 Component에 붙을 수 있어 강한 참조로 두면 좀비와 그래프 끌어오기가 생긴다 — UE는 Outer·레벨 패키지·garbage elimination으로 푼다). 추적하지 않는 참조 `WeakObjectRef<T>`가 생긴 뒤 레벨·prefab 저장 전에 영속한다.
- **Level 그래프 저장은 구조 시험용이다**: graph v1로 Level 루트를 저장할 수 있다(그래프의 모든 타입이 생성 타입일 때 — native 파생 Actor·Component가 하나라도 있으면 Unsupported, Actor별 저장 제외는 레벨 패키지 단계). 부착 계층은 저장되지 않는다(root는 저장된다). 저장 안전 지점은 Tick 밖·파괴 대기 정리(FlushDestroys) 뒤다 — 정리 전에는 파괴 대기 Actor가 `actors`에 남아 함께 저장된다.
- **장부 밖 객체**: World API(SpawnActor·AddComponent·CreateLevel) 밖에서 만든 Actor·Component(그래프 로드, World registry에 직접 `NewObject`)는 `World*`가 없고 World에 수명 기록이 없다. 그 객체의 계층·World API는 Fatal 대신 `InvalidArgument`다(`SetRootComponent`·`Destroy`·`AttachTo`(그 객체를 부모로 쓰는 경우 포함)·`Detach`, `World::DestroyComponent`). 수명 기록이 필요한 World API(`DestroyActor`·`AddComponent`·`GetComponents`)는 `StaleHandle`이다. 상대 Transform 설정과 월드 행렬 계산은 부모가 없으므로 된다. `GetWorld()`는 참조를 돌려주므로 여전히 Fatal이며 `HasWorld()`로 먼저 확인한다.
- **레벨 편입 계약(레벨 패키지 단계 — 자리)**: 로드한 Level 그래프를 World에 넣는 단계는 (1) Actor마다 `ActorRecord`를 만들고 Level 레코드의 Actor 수를 맞추며, (2) Level·Actor·Component의 `World*`·Actor의 소속 Level·Component의 소유 Actor를 `actors`·`components` 포함 관계에서 연결하고, (3) 부착 계층이 영속된 뒤에는 parent에서 children을 다시 만들며, (4) `actors`·`components`의 null 원소를 정리하고 root가 `components` 밖이면 비우며, (5) `relativeTransform`을 `SetRelativeTransform`과 같은 규칙(유한·단위 quaternion)으로 검증한 뒤, (6) World 수명 단계(FinishSpawning과 같은 hook 순서)로 진행한다. 이 단계 전의 로드 객체는 별도 ObjectRegistry(같은 TypeRegistry)에 두고 World registry에 넣지 않는다.
- **World registry의 외부 생성 거부(정책 자리)**: World registry에 World API 밖에서 World 타입을 만드는 것(직접 `NewObject`·그래프 로드)은 계약 위반이다. 지금은 위 장부 밖 규칙으로 Fatal 없이 거부만 하고, A0-O의 에셋 Registry 정책(WorldOnly 타입의 에셋 Registry 생성 거부)의 거울로 World registry가 WorldOnly 타입의 외부 생성 경로를 막는 검사를 둘 자리를 남긴다.

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

- `Initialize(EngineConfig)`: TypeRegistry·ModuleRegistration → 타입 등록(World 리플렉션 타입 `RegisterWorldTypes` → `EngineConfig.registerModuleTypes(ModuleRegistration&)` — 게임 리플렉션 모듈, 같은 registration → `EngineConfig.registerTypes(TypeRegistry&)` — native 타입, 리플렉션 타입을 base로 둘 수 있으므로 마지막) → 엔진 Registry → EngineSubsystem(의존 순서) → 클라이언트(`EngineClient`, 예: GameInstance) 순. 어느 단계든 실패하면 앞 단계를 역순 정리하고 `Failed`.
- 종료(`Shutdown`과 초기화 실패 정리 모두): World 파괴 → Subsystem 역순 정리 → 엔진 Registry 정리 → ModuleRegistration 파괴(붙잡은 lease 해제) → `TypeRegistry::Shutdown`. Engine은 타입을 Unregister하지 않는다 — 게임이 토큰을 버린 native 파생이 리플렉션 base의 dependent로 남아 Unregister가 Busy로 멈추기 때문이다. `TypeRegistry::Shutdown`은 남은 lease만 검사하고 레코드를 dependent 순서로 반납한다. ModuleRegistration 멤버는 TypeRegistry 멤버 뒤에 선언해 먼저 소멸한다.
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
- `SpawnActor`는 `SpawnActorDeferred` + `FinishSpawning`이다. 같은 프레임에 FinishSpawning한 Actor들의 `OnAwake`가 모두 끝난 뒤에 `OnStart`를 시작한다.
- 의존성 준비(`IsReadyToStart`)는 매 프레임 확인하며 기다리느라 게임 스레드를 막지 않는다. 비동기 에셋 로딩은 이 지점에 연결한다.
- `OnAwake`가 실패하면 그 Actor는 즉시 파괴 경로로 들어가고 `FinishSpawning`이 오류를 반환한다.

### 7.3 Component

- `Component`는 Actor에 속하고 `OnRegister`/`OnUnregister`(엔진 내부·backend 등록)와 위 hook을 가진다. Actor의 `FinishSpawning`이 Component를 등록한다(OnRegister → Actor OnAwake → Component OnAwake → OnEnable). 이미 초기화된 Actor에 추가된 Component는 추가 즉시 OnRegister·OnAwake·OnEnable을 받고 OnStart는 다음 프레임 시작에 받는다(다른 객체의 hook 실행 중 OnStart 재진입 방지). 정리는 OnDisable(Component 역순 → Actor) → OnDestroy(Component 역순 → Actor) → OnUnregister 순이며, OnDestroy는 OnAwake가 성공한 객체만 받는다.
- `SceneComponent`는 부모·자식 계층, 상대 Transform, 월드 행렬(`Mat4d` affine)을 가진다. 월드 행렬 = 부모 월드 행렬 × 상대 TRS 행렬이며 dirty 플래그로 지연 계산한다. 행렬로 저장하므로 비균일 scale 부모 아래 회전한 자식(shear)도 정확하고, TRS가 필요할 때만 `TryWorldTransform`이 분해한다(shear면 Unsupported). 상대 Transform·부모 변경 시 하위 전체를 dirty로 표시한다. `AttachTo`는 순환(자기 자신·조상)과 장부 밖 객체를 `InvalidArgument`로 거부하며 같은 World 안에서만 붙인다. 부모가 파괴되면 자식은 루트가 된다. 영속 상태는 상대 Transform뿐이고 계층·월드 행렬은 저장하지 않는다(4.6절).

### 7.4 지연 작업과 안전 지점

- Tick 순회 중 파괴·Tick 등록 변경·Component 제거는 큐에 넣는다. 안전 지점은 **각 phase 경계**(Tick 등록 변경 반영)와 **World Tick 끝**(파괴 완료)이다.
- 같은 객체의 중복 파괴 요청은 성공으로 처리하고 한 번만 실행한다. 큐에 남은 핸들이 이미 무효이면 건너뛴다.
- World Tick 끝에 제한된 예산으로 증분 GC를 진행해 파괴된 객체 저장소를 회수한다.

## 8. Tick Scheduler

- 등록 기반이다. 등록 단위는 `(대상 핸들, 종류, hook, phase, priority, interval, pause 정책)`이며 `TickHandle`(slot+generation)로 식별한다.
- 단계 순서(열거 순서 = 실행 순서)는 `PrePhysics → Physics(고정 스텝 수만큼) → PostPhysics → LateUpdate → PreRender`다. LateUpdate(카메라 추적 등 후처리)가 PreRender(렌더 추출 준비)보다 먼저 와야 같은 프레임 렌더에 반영된다.
- phase마다 밀집 배열을 두고, 등록 집합이 바뀐 phase만 다시 정렬한다. 정렬은 (priority, 등록 순서) 최소 힙을 쓰는 위상 정렬이다. 선행 의존으로 묶이지 않은 항목끼리는 항상 (priority, 등록 순서)를 따른다. 순회 중에는 할당하지 않는다(실행 중 해제도 표시만 하고 단계 경계에서 지운다).
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
- Sandbox는 WASD·방향키로 스프라이트를 움직이고, 마우스 위치로 삼각형을 회전시키며, 좌클릭으로 색을 바꾸고, 게임패드 왼쪽 스틱도 같은 이동에 연결해 확인한다.

## 10. Graphics

### 10.1 계약

```cpp
namespace rocks::graphics {
enum class Backend : uint8 { D3D12, OpenGL, Vulkan, Metal };

Result<UniquePtr<Device>> CreateDevice(const DeviceDesc&);   // backend 모듈이 제공
class Device {
    Result<SwapChainHandle> CreateSwapChain(const SwapChainDesc&);   // 네이티브 창 핸들
    Result<BufferHandle> CreateBuffer(const BufferDesc&, Span<const uint8> initial);
    Result<TextureHandle> CreateTexture(const TextureDesc&, Span<const uint8> initial);
    Result<PipelineHandle> CreatePipeline(const PipelineDesc&);
    Result<CommandContext*> BeginFrame(SwapChainHandle, const ClearValue&);
    Result<void> EndFrame(SwapChainHandle);   // 제출·present
    void Destroy(Handle);                      // GPU 완료 후 회수
};
}
```

- 자원은 slot+generation 핸들이다. `Destroy`는 마지막으로 사용한 프레임의 GPU 완료(D3D12 fence) 이후에 실제 해제한다. OpenGL은 드라이버가 수명을 관리하므로 즉시 삭제한다.
- 셰이더는 backend별 원문을 `PipelineDesc`에 담는다(D3D12: HLSL을 런타임 `D3DCompile` SM 5.1, OpenGL: GLSL 4.50). 오프라인 셰이더 쿠킹과 DXC는 후속이다.
- 그리기 상수는 64바이트 행렬 + 16바이트 색으로 제한한다(D3D12 root constants, OpenGL uniform).
- 좌표 규약: 행렬은 열 우선 column vector이며 HLSL·GLSL 모두 열 우선으로 업로드한다. OpenGL은 `glClipControl`의 `GL_ZERO_TO_ONE`으로 D3D와 같은 [0,1] 깊이를 쓰고, 원점은 창에 그릴 때 `GL_LOWER_LEFT`, RenderTarget 텍스처에 그릴 때 `GL_UPPER_LEFT`로 바꿔 RenderTarget의 첫 행이 이미지 위쪽이 되게 한다(샘플링·readback 모두 D3D12와 같은 배치). 텍스처는 첫 행이 위인 배치로 올리고 UV (0,0)이 좌상단이다.
- D3D12는 프레임 2개를 동시에 진행하고 프레임 slot 재사용은 해당 fence 완료로 판정한다. 디버그 구성은 디버그 레이어를 켠다.

### 10.2 범위 밖

RenderGraph·Deferred/Forward 경로·깊이 버퍼·MSAA·멀티 큐·bindless·다른 backend 구현은 이번 범위가 아니다.

## 11. 에셋

- `AssetId`는 `Guid`다. 에셋 파일(`.rasset`)은 `magic "RAST" · 형식 버전 · 에셋 종류 · AssetId · 페이로드`를 Serialization `ArchiveWriter`로 기록한다.
- `TextureAsset`: 폭·높이·형식(RGBA8 sRGB)·픽셀. 행 순서는 위→아래.
- `AssetManager`: 루트 디렉터리를 마운트해 에셋 파일 헤더를 읽고 `AssetId ↔ 경로` 목록을 만든다. `Load(AssetId)`는 참조 계수 핸들을 반환한다. 마지막 핸들이 사라지면 언로드한다. 이번 단계의 로드는 동기이며 비동기 로딩(`AsyncReadFile`, 로드 상태 Pending/Ready/Failed)은 후속이다.
- `BitmapImporter`(AssetTools): 비압축 BMP(24·32비트, `BI_RGB`·`BI_BITFIELDS`, 상향·하향 행)를 RGBA8로 바꾼다. 압축 BMP·팔레트는 `Unsupported`다. Commandlet `import-texture <src.bmp> <dst.rasset>`로 실행한다.
- GPU 텍스처는 `WorldIntegration`의 텍스처 캐시가 AssetId별로 만든다. 에셋 데이터와 GPU 자원의 수명은 분리한다.

## 12. 실행 경로(S6 완료 시)

1. Sandbox가 창과 Graphics 장치(`--graphics=d3d12|opengl`)를 만든다.
2. Engine을 초기화하고 World를 하나 만든다.
3. AssetManager가 `Content/`를 마운트한다.
4. 삼각형 Actor(매 Tick 회전)와 SpriteComponent를 가진 Actor(텍스처 에셋)를 Spawn한다.
5. 매 프레임 Engine이 World를 Tick하고, `FrameEnd` hook에서 WorldIntegration이 RenderScene을 추출하고 Renderer가 그린다.
6. 창을 닫으면 `RequestExit` → World 파괴 → GPU idle 대기 → 장치·창 정리 → Engine 종료.

## 13. 시험

- `Engine` 타깃의 헤드리스 테스트: GameClock, Engine 상태 기계·실패 복구, World·Actor·Component 수명 순서, Tick phase·priority·interval·pause·지연 변경, 복수 World 격리, 지연 파괴, BMP 파싱, 에셋 파일 왕복, AssetManager 참조 계수.
- World 리플렉션(B8b — `WorldReflectionTests`·`WorldTests`·`EngineTests`): 네 타입의 WorldOnly·필드 형태와 멱등 등록, native 파생의 base 상속, GC(Spawn 생존·제거 회수·native 파생의 Component 생존·Marking 중 AddComponent의 newborn shade·Marking 중 참조 쓰기의 삽입 장벽·root 참조 방문과 파괴 시 비우기·우회 파괴 수리), Level 그래프 저장 → 별도 Registry 로드 → 필드·간선·root·소프트 참조 → 재저장 byte 동일(NATIVE 기본값, 계층 비저장), native 파생이 든 Level 저장 Unsupported, 장부 밖 객체 API 거부, null 원소 순회, 경로 읽기와 World 필드 쓰기 거부·fixture 쓰기와 옛 값, Engine 등록 순서와 native 파생을 둔 Shutdown·초기화 실패 정리.
- GPU 시험은 별도 실행(`Sandbox --self-test`): D3D12(WARP 허용)와 OpenGL에서 offscreen 또는 backbuffer readback으로 삼각형·텍스처 픽셀을 확인한다. GPU 없는 CI에서는 실행하지 않고 NOT RUN으로 보고한다.
- 각 단계는 Debug·ASan 테스트와 `clang-format` 검사를 통과한 상태로 커밋한다.

## 14. 알려진 제한

- World 타입의 Level 그래프 저장은 구조 시험용이다: 부착 계층은 저장되지 않고(WeakObjectRef 뒤), native 파생이 든 그래프는 저장할 수 없으며, 로드한 객체는 레벨 편입 단계 전까지 World 장부 밖이다(4.6절). World 필드는 ReadOnly라 CU5 setter hook 전에는 경로로 편집할 수 없다.
- Tick마다 Borrow를 얻는 잠금 비용이 있다. 일괄 접근은 측정 후 도입한다.
- 렌더 추출은 게임 스레드에서 동기 실행한다. 렌더 스레드·in-flight snapshot은 후속이다.
- macOS·Vulkan·Metal은 미구현이며 해당 경로는 `Unsupported`를 반환한다.
