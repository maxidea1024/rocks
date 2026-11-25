#pragma once

#include <Core/Text/String.h>
#include <Engine/Engine.h>
#include <Gameplay/LocalPlayer.h>
#include <Object/ObjectRefArray.h>

namespace rocks {

class GameInstance;

// GameInstance와 수명을 같이하는 Subsystem의 기반이다(UE UGameInstanceSubsystem, Docs/Design/
// EngineRuntime 16.3절 ERD11). GameInstance OnInit 뒤 마지막에 초기화하고, 종료 때 LocalPlayer를
// 모두 지운 뒤 역순으로 정리한다. 게임 World 안에서는 GetGameInstanceSubsystem<T>(world)로 찾는다.
// Initialize·Deinitialize가 받는 GameInstance는 그 호출 동안만 빌린 것이다(포인터를 남기지 않는다).
//
// 사용 예:
//   class SaveSlots final : public GameInstanceSubsystem {
//   public:
//       Result<void> Initialize(GameInstance&) noexcept override { return Load(); }
//   };
//   const GameInstanceSubsystemType types[] = {
//       MakeSubsystemType<GameInstanceSubsystem, SaveSlots>("SaveSlots")};
//   // GameInstanceConfig::subsystems = types;
class GameInstanceSubsystem {
public:
    virtual ~GameInstanceSubsystem() = default;

    virtual Result<void> Initialize(GameInstance&) noexcept {
        return Result<void>::Success();
    }

    virtual void Deinitialize(GameInstance&) noexcept {
    }
};

using GameInstanceSubsystemType = SubsystemType<GameInstanceSubsystem>;
using GameInstanceSubsystems = SubsystemCollection<GameInstanceSubsystem, GameInstance>;

namespace detail {

// GameInstance Subsystem 묶음과 World 바깥 조회 창의 context다. GameInstanceClient가 소유한다 —
// 엔진 Registry 객체(GameInstance) 밖에 둬서 World의 조회 창이 GameInstance를 빌리지 않고 읽게
// 하고, GameInstance를 정리한 뒤에도(Editor·Inactive World는 Shutdown 때까지 창이 열려 있다) 창이
// 닫힌 묶음만 보게 한다(Docs/Design/EngineRuntime 18.6절).
struct GameInstanceServices {
    explicit GameInstanceServices(Allocator& allocator) noexcept
        : subsystems(allocator), worldSubsystems(allocator) {
    }

    Engine* engine = nullptr;
    GameInstanceSubsystems subsystems;
    // OpenWorld가 만드는 World의 Subsystem 목록이다(설정 목록 + GameplayWorldSubsystem). World가
    // Shutdown까지 Span을 쥐므로 client가 소유한다.
    Array<WorldSubsystemType> worldSubsystems;
    // GameInstance Subsystem을 조회 창에 보이는지다(초기화 뒤 참, 정리 전에 거짓).
    bool visible = false;
};

} // namespace detail

// GameInstanceClient의 설정이다(18.6절). Span·포인터가 가리키는 것은 client보다 오래 살아야 한다.
struct GameInstanceConfig {
    // 만들 GameInstance·LocalPlayer 클래스다(비면 기본 클래스). 엔진 TypeRegistry에 등록된
    // 타입이어야 한다(native 파생은 EngineConfig::registerTypes).
    TypeId gameInstanceType;
    TypeId localPlayerType;
    // 맵이 GameDirector 타입을 정하지 않을 때의 프로젝트 기본이다(비면 GameDirector — 19.3절).
    TypeId defaultDirectorType;
    NetMode netMode = NetMode::Standalone;
    Span<const GameInstanceSubsystemType> subsystems;
    Span<const LocalPlayerSubsystemType> localPlayerSubsystems;
    // OpenWorld가 만드는 World의 WorldSubsystem 목록과 시간 정책이다.
    Span<const WorldSubsystemType> worldSubsystems;
    TimeStepPolicy timeStep;
    // StartGameInstance가 여는 기본 맵이다(이름이 비면 시작이 InvalidArgument — UE도 기본 맵이
    // 없으면 실패한다). 첫 LocalPlayer의 입력 장치 번호는 0이다.
    MapDesc defaultMap;
};

// 게임 세션 하나다(UE UGameInstance, 18.2절). 엔진 Registry의 리플렉션 객체로 Engine 수명 동안
// 살며 LocalPlayer 목록·GameInstance Subsystem 묶음·현재 게임 World를 가진다. World 전환
// (OpenWorld)을 넘어 유지된다. 게임은 파생 클래스로 hook을 재정의하고
// GameInstanceConfig::gameInstanceType으로 지정한다. GameInstanceClient가 만들고 hook을 부른다.
//
// 엔진 Registry 객체라 Borrow 범위에서만 쓴다: hook 안, Subsystem Initialize·Deinitialize 안,
// GameInstanceClient::WithInstance 안.
//
// 사용 예:
//   class MyGame final : public GameInstance {
//       ROCKS_NATIVE_TYPE(MyGame, GameInstance, 0x..., 0x...)
//   public:
//       explicit MyGame(Allocator& allocator) noexcept : GameInstance(allocator) {}
//   protected:
//       Result<void> OnPostInit() noexcept override {
//           auto* saves = GetSubsystem<SaveSlots>();   // Subsystem은 OnPostInit부터 있다
//           return saves != nullptr ? Result<void>::Success()
//                                   : Result<void>::Failure(ErrorCode::NotFound);
//       }
//   };
//
// 스레드: 엔진 게임 스레드 전용이다.
ROCKS_CLASS()
class ROCKS_GAMEPLAY_API GameInstance : public Object {
    ROCKS_GENERATED_BODY()

public:
    // allocator는 Registry allocator다(Object 생성 시 주입). 파생 클래스 생성자도 받아서 넘긴다.
    explicit GameInstance(Allocator& allocator) noexcept;
    virtual ~GameInstance() noexcept;

    Engine& GetEngine() const noexcept {
        return *engine_;
    }

    NetMode GetNetMode() const noexcept {
        return netMode_;
    }

    bool IsDedicatedServer() const noexcept {
        return netMode_ == NetMode::DedicatedServer;
    }

    // T 종류의 GameInstance Subsystem이다. OnPostInit부터 OnShutdown 뒤 정리 전까지 있다(없으면
    // nullptr). 포인터는 묶음이 정리될 때까지 유효하다.
    template <class T>
    T* GetSubsystem() const noexcept {
        return services_->subsystems.template Get<T>();
    }

    // 시작 때 생략한 GameInstance Subsystem이다(진단용).
    Span<const SkippedSubsystem> SkippedSubsystems() const noexcept {
        return services_->subsystems.Skipped();
    }

    // --- LocalPlayer(18.4절) ---

    // LocalPlayer를 만들어 목록 끝에 더하고 LocalPlayer Subsystem을 초기화한다(UE CreateLocalPlayer
    // → PlayerAdded). spawnPlayerController이고 현재 World가 Playing이며 GameDirector가 있으면 바로
    // Login한다(21.2절 8–9단계). 오류: DedicatedServer는 InvalidArgument(UE도 거부), 같은
    // controllerId가 있으면 AlreadyExists, 생성·Subsystem 초기화·Login 실패는 그 오류(만들지 않은
    // 것으로 되돌린다).
    //
    // 사용 예:
    //   auto second = CreateLocalPlayer(1);    // 두 번째 패드
    Result<ObjectHandle> CreateLocalPlayer(int32 controllerId,
                                           bool spawnPlayerController = true) noexcept;

    // LocalPlayer의 입력 원천을 바꾸고 현재 PlayerController에도 건다(20.8절). input은 다시 바꿀
    // 때까지 살아 있어야 한다(null이면 뗀다). 없는 handle은 NotFound다.
    Result<void> SetLocalPlayerInput(ObjectHandle player, const input::InputState* input) noexcept;

    // LocalPlayer를 목록에서 빼고 LocalPlayer Subsystem을 역순으로 정리한다(UE RemoveLocalPlayer →
    // PlayerRemoved). 뒤 LocalPlayer의 Index가 앞당겨진다. 없는 handle은 NotFound다.
    Result<void> RemoveLocalPlayer(ObjectHandle player) noexcept;

    uint32 LocalPlayerCount() const noexcept {
        return static_cast<uint32>(localPlayers_.View().Size());
    }

    // index번째 LocalPlayer handle이다. 범위 밖이면 null handle이다.
    ObjectHandle LocalPlayerAt(uint32 index) const noexcept {
        const Span<const ObjectHandle> players = localPlayers_.View();
        return index < players.Size() ? players[index] : ObjectHandle{};
    }

    // LocalPlayer(또는 T 파생)를 빌려 function을 부른다(World::With와 같은 반환 규칙).
    template <class T = LocalPlayer, class Function>
    auto WithLocalPlayer(ObjectHandle player, Function&& function) noexcept {
        return detail::WithEngineObject<T>(engine_->Objects(), player,
                                           static_cast<Function&&>(function));
    }

    // --- World(18.5절) ---

    // 맵을 연다(UE LoadMap): 현재 게임 World를 EndPlay(LevelTransition)로 끝내고 바로 파괴한 뒤 새
    // World를 만들어 map.populate → BeginPlay한다. 엔진 프레임 안(OnEngineTick 밖 — World Tick·loop
    // hook)에서 부르면 요청을 기록하고 다음 OnEngineTick에서 실행한다(마지막 요청만 남고 덮어쓰면
    // Warning). 오류: 이름이 비거나 Engine이 Running이 아니면 InvalidArgument, OpenWorld 실행
    // 중(정리 hook 안)이면 Busy, 새 World 생성·populate·BeginPlay 실패는 그 오류(새 World를
    // 정리하고 현재 World는 없음이 된다 — 이전 World는 이미 정리했다).
    Result<void> OpenWorld(const MapDesc& map) noexcept;

    // 현재 게임 World다(없으면 null). OpenWorld·종료까지, 또는 누가 Engine::DestroyWorld로 지울
    // 때까지 유효하다(파괴되면 EngineClient::OnWorldDestroyed가 비운다).
    World* CurrentWorld() const noexcept {
        return currentWorld_;
    }

    // 다음 OnEngineTick에서 실행할 OpenWorld 요청이 있는지다.
    bool HasPendingOpen() const noexcept {
        return hasPendingOpen_;
    }

    // OpenWorld가 만드는 World에 넣는 바깥 Subsystem 조회 창이다: GameInstance Subsystem을 먼저
    // 보고 없으면 EngineSubsystem으로 넘긴다(16.3절 ERD10).
    OuterSubsystemLookup SubsystemLookup() const noexcept;

protected:
    // GameInstance Subsystem 초기화 전이다(UE ReceiveInit 자리). 실패하면 엔진 시작이 실패한다.
    virtual Result<void> OnInit() noexcept {
        return Result<void>::Success();
    }

    // GameInstance Subsystem 초기화 뒤, 첫 LocalPlayer 전이다(C++ 파생이 Super::Init() 뒤에 두던
    // 코드 자리). 실패하면 Subsystem을 정리하고 엔진 시작이 실패한다.
    virtual Result<void> OnPostInit() noexcept {
        return Result<void>::Success();
    }

    // GameDirector 타입을 마지막으로 바꾼다(UE OverrideGameModeClass — 19.3절). 받은 타입은 맵 →
    // 프로젝트 기본 → GameDirector 순으로 정한 것이다. 기본은 그대로 돌려준다.
    virtual TypeId OverrideDirectorType(TypeId type) noexcept {
        return type;
    }

    // 기본 맵을 연 뒤다(UE OnStart).
    virtual void OnStart() noexcept {
    }

    // 종료 때 현재 World를 EndPlay(Quit)로 끝낸 뒤, LocalPlayer·Subsystem 정리 전이다(UE Shutdown의
    // ReceiveShutdown 자리).
    virtual void OnShutdown() noexcept {
    }

private:
    friend class GameInstanceClient;

    // 미룬 요청을 지금 실행한다(OnEngineTick).
    Result<void> RunPendingOpen() noexcept;
    Result<void> OpenWorldNow(const MapDesc& map) noexcept;
    // 조건(19.3절)이 맞으면 GameDirector를 Spawn한다(InitGame → FinishSpawning). stage는 실패한
    // 단계 이름을 받는다.
    Result<void> SpawnDirector(World& world, const MapDesc& map, const char*& stage) noexcept;
    // 현재 World를 reason으로 끝내고 파괴를 요청한다(없으면 아무것도 하지 않는다). 21.3절 정리를
    // 먼저 한다.
    void CloseCurrentWorld(EndPlayReason reason) noexcept;
    // 21.3절: LocalPlayer마다 Spawn된 Pawn → PlayerController를 파괴 대기에 넣는다(EndPlay가 그
    // 순서로 정리한다 — Logout 포함). 배치 Pawn은 남는다.
    void CleanupPlayers(World& world) noexcept;
    // LocalPlayer 하나를 World에 Login시킨다(Director가 없으면 아무것도 하지 않는다).
    Result<void> LoginPlayer(World& world, ObjectHandle player) noexcept;
    // LocalPlayer를 모두 역순으로 지운다(종료·시작 실패 되돌림).
    void RemoveAllLocalPlayers() noexcept;
    Result<void> RemoveLocalPlayerAt(usize index) noexcept;

    ROCKS_NATIVE() Engine* engine_ = nullptr;
    ROCKS_NATIVE() const GameInstanceConfig* config_ = nullptr;
    ROCKS_NATIVE() detail::GameInstanceServices* services_ = nullptr;
    // LocalPlayer 목록이다(강한 GC 간선 — UE LocalPlayers 배열). Registry API로만 고친다.
    ROCKS_PROPERTY(ReadOnly, Name = "localPlayers") ObjectRefArray<LocalPlayer> localPlayers_;
    ROCKS_NATIVE() World* currentWorld_ = nullptr;
    ROCKS_NATIVE() String pendingName_;
    ROCKS_NATIVE() String pendingOptions_;
    ROCKS_NATIVE() MapDesc pendingMap_;
    ROCKS_NATIVE() NetMode netMode_ = NetMode::Standalone;
    ROCKS_NATIVE() bool hasPendingOpen_ = false;
    ROCKS_NATIVE() bool inEngineTick_ = false;
    ROCKS_NATIVE() bool opening_ = false;
};

// 게임 World 안(WorldSubsystem·Actor 등)에서 GameInstance Subsystem을 찾는다(World의 바깥 조회 창 —
// 16.3절 ERD10). GameInstance 없이 만든 World, 조회 창이 닫힌 뒤(EndPlay 끝·PreDeinitialize·
// Deinitialize), 없는 Subsystem이면 nullptr이다.
//
// 사용 예:
//   auto* saves = GetGameInstanceSubsystem<SaveSlots>(world);
template <class T>
T* GetGameInstanceSubsystem(const World& world) noexcept {
    static_assert(IsBaseOfV<GameInstanceSubsystem, T>);
    return static_cast<T*>(
        static_cast<GameInstanceSubsystem*>(world.FindOuterSubsystem(SubsystemKeyOf<T>())));
}

} // namespace rocks
