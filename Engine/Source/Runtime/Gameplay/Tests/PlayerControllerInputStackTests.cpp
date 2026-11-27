#include <Base/InitializerList.h>
#include <Core/Memory/SystemAllocator.h>
#include <Gameplay/Components/InputComponent.generated.h>
#include <Gameplay/Components/InputComponent.h>
#include <Gameplay/Controller.generated.h>
#include <Gameplay/Controller.h>
#include <Gameplay/GameInstance.generated.h>
#include <Gameplay/GameInstanceClient.h>
#include <Gameplay/LocalPlayer.generated.h>
#include <Gameplay/Pawn.generated.h>
#include <Gameplay/Pawn.h>
#include <Gameplay/PlayerController.generated.h>
#include <Gameplay/PlayerController.h>
#include <Input/InputEvent.h>
#include <TestSupport/Test.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/SceneComponent.generated.h>
#include <World/SceneComponent.h>
#include <stdio.h>
#include <string.h>

// PlayerController 입력 스택 시험이다(Docs/Design/InputActions 5.5절 — IA8): 쌓는 순서·priority
// 정렬·위층부터 실행, 층 사이 consumes, blockInput 아래층 미전달·값 0, Push 자리·중복 제거·Pop 값
// 0· 사라진 항목 정리, EnableInput 생성·재사용·조종 Pawn 켜기·다른 Controller Pawn 거부, Possess 때
// OnSetupPlayerInput·UnPossess 때 파괴, Pawn 입력 끄기.
namespace rocks::test {
ModuleLoader& ProcessModules() noexcept;
} // namespace rocks::test

using namespace rocks;
namespace gameplay_input_stack_tests_file {

namespace {

constexpr Guid JumpId{0x49413838, 1};
constexpr Guid ContextId{0x49413838, 100};
constexpr usize MaxCalls = 16;

// 실행 순서 기록이다.
struct CallLog {
    char tags[MaxCalls + 1] = {};
    usize count = 0;

    void Add(char tag) noexcept {
        if (count < MaxCalls) {
            tags[count++] = tag;
            tags[count] = '\0';
        }
    }

    bool Is(const char* expected) noexcept {
        const bool same = strcmp(tags, expected) == 0;
        if (!same) {
            printf("  calls '%s', expected '%s'\n", tags, expected);
        }

        count = 0;
        tags[0] = '\0';
        return same;
    }
};

CallLog calls;

// Possess되면 Jump를 바인딩하는 Pawn이다(UE SetupPlayerInputComponent).
class InputPawn final : public Pawn {
    ROCKS_NATIVE_TYPE(InputPawn, Pawn, 0x2a6d9e1f0c3b4875ull, 0xb7f40e9c1d52a683ull)

public:
    explicit InputPawn(Allocator& allocator) noexcept : Pawn(allocator) {
        autoPossessAi = AutoPossessAi::Disabled;
    }

    void OnSetupPlayerInput(InputComponent& input) noexcept override {
        ++setups;
        auto callback = input::InputActionBindings::Callback::TryCreate(
            GetAllocatorForTests(), [](const input::ActionEventData&) noexcept { calls.Add('w'); });
        if (callback) {
            (void)input.Bindings().BindAction(JumpId, input::ActionEvent::Started,
                                              Move(callback.Value()));
        }
    }

    static Allocator& GetAllocatorForTests() noexcept {
        static SystemAllocator allocator;
        return allocator;
    }

    static inline uint32 setups = 0;
};

// 입력 스택에 넣을 평범한 Actor다(Pawn이 아니다).
class PlainActor final : public Actor {
    ROCKS_NATIVE_TYPE(PlainActor, Actor, 0x6c1e08b4f9a25d37ull, 0x3d9a72e5b0c4f186ull)

public:
    explicit PlainActor(Allocator& allocator) noexcept : Actor(allocator) {
    }
};

Result<void> RegisterTestTypes(TypeRegistry& types) noexcept {
    auto pawn = RegisterNativeClass<InputPawn>(types);
    if (!pawn) {
        return Result<void>::Failure(pawn.Error());
    }

    auto plain = RegisterNativeClass<PlainActor>(types);
    return plain ? Result<void>::Success() : Result<void>::Failure(plain.Error());
}

struct Fixture {
    Fixture() noexcept {
        game.timeStep = {1.0 / 60.0, 0.25, 8};
        game.defaultMap.name = "Arena";
        config.modules = &test::ProcessModules();
        config.fixedFrameDeltaSeconds = 1.0 / 60.0;
        config.registerTypes = &RegisterTestTypes;
        calls.count = 0;
        calls.tags[0] = '\0';
        InputPawn::setups = 0;
    }

    Result<void> Start() noexcept {
        auto madeClient = UniquePtr<GameInstanceClient>::TryMake(allocator, allocator, game);
        if (!madeClient) {
            return Result<void>::Failure(madeClient.Error());
        }

        client = Move(madeClient.Value());
        config.client = client.Get();
        auto madeEngine = UniquePtr<Engine>::TryMake(allocator, allocator);
        if (!madeEngine) {
            return Result<void>::Failure(madeEngine.Error());
        }

        engine = Move(madeEngine.Value());
        if (auto initialized = engine->Initialize(config); !initialized) {
            return initialized;
        }

        // Jump(SpaceBar) 컨텍스트와 키보드 원천을 붙인다.
        const ObjectHandle player = Player();
        auto input = client->WithInstance([&](GameInstance& instance) noexcept {
            return instance.SetLocalPlayerInput(player, &keyboard);
        });
        if (!input) {
            return input;
        }

        input::InputContextBundle bundle(allocator);
        bundle.id = ContextId;
        input::InputActionData action(allocator);
        action.id = JumpId;
        input::InputBinding binding(allocator);
        binding.action = JumpId;
        if (!bundle.actions.TryPushBack(Move(action)) || !binding.key.TryAssign("SpaceBar") ||
            !bundle.data.bindings.TryPushBack(Move(binding))) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }

        auto added = Current().With<PlayerController>(
            ControllerHandle(), [&](PlayerController& value) noexcept {
                return value.ActionInput().AddContext(Move(bundle), 0);
            });
        if (!added) {
            return added;
        }

        return engine->RunFrame();
    }

    ~Fixture() {
        if (engine) {
            (void)client->WithInstance([&](GameInstance& instance) noexcept {
                return instance.SetLocalPlayerInput(Player(), nullptr);
            });
            CHECK(engine->Shutdown().IsOk());
        }

        engine.Reset();
        client.Reset();
    }

    World& Current() noexcept {
        auto world = client->WithInstance([](GameInstance& instance) noexcept {
            return reinterpret_cast<uptr>(instance.CurrentWorld());
        });
        return *reinterpret_cast<World*>(world.Value());
    }

    ObjectHandle Player() noexcept {
        auto player = client->WithInstance(
            [](GameInstance& instance) noexcept { return instance.LocalPlayerAt(0); });
        return player.Value();
    }

    ObjectHandle ControllerHandle() noexcept {
        const ObjectHandle player = Player();
        auto controller = client->WithInstance([player](GameInstance& instance) noexcept {
            return instance.WithLocalPlayer(
                player, [](LocalPlayer& local) noexcept { return local.PlayerControllerHandle(); });
        });
        return controller ? controller.Value() : ObjectHandle{};
    }

    template <class Fn>
    auto Controller(Fn&& fn) noexcept {
        return Current().With<PlayerController>(ControllerHandle(), Forward<Fn>(fn));
    }

    // tag를 기록하는 Started 바인딩을 bindings에 건다.
    bool Record(input::InputActionBindings& bindings, char tag, bool consumes = false) noexcept {
        auto callback = input::InputActionBindings::Callback::TryCreate(
            allocator, [tag](const input::ActionEventData&) noexcept { calls.Add(tag); });
        return callback && bindings
                               .BindAction(JumpId, input::ActionEvent::Started,
                                           Move(callback.Value()), consumes)
                               .IsOk();
    }

    bool RecordOn(ObjectHandle component, char tag, bool consumes = false) noexcept {
        auto done = Current().With<InputComponent>(component, [&](InputComponent& input) noexcept {
            return Record(input.Bindings(), tag, consumes);
        });
        return done && done.Value();
    }

    bool Configure(ObjectHandle component, int32 priority, bool block) noexcept {
        return Current()
            .With<InputComponent>(component,
                                  [&](InputComponent& input) noexcept {
                                      input.SetPriority(priority);
                                      input.SetBlockInput(block);
                                  })
            .IsOk();
    }

    ObjectHandle Spawn() noexcept {
        auto actor = Current().SpawnActor<PlainActor>();
        return actor ? actor.Value() : ObjectHandle{};
    }

    ObjectHandle SpawnPawn() noexcept {
        auto pawn = Current().SpawnActor<InputPawn>();
        return pawn ? pawn.Value() : ObjectHandle{};
    }

    ObjectHandle Enable(ObjectHandle actor) noexcept {
        auto enabled = Controller(
            [actor](PlayerController& value) noexcept { return value.EnableInput(actor); });
        return enabled ? enabled.Value() : ObjectHandle{};
    }

    // fn을 부르는 Started 바인딩을 component에 건다.
    template <class Fn>
    bool BindOn(ObjectHandle component, Fn&& fn) noexcept {
        auto callback = input::InputActionBindings::Callback::TryCreate(allocator, Forward<Fn>(fn));
        if (!callback) {
            return false;
        }

        auto done = Current().With<InputComponent>(component, [&](InputComponent& input) noexcept {
            return input.Bindings()
                .BindAction(JumpId, input::ActionEvent::Started, Move(callback.Value()))
                .IsOk();
        });
        return done && done.Value();
    }

    // component에 Jump 값 바인딩을 건다.
    input::ActionValueHandle BindValueOn(ObjectHandle component) noexcept {
        auto bound = Current().With<InputComponent>(component, [](InputComponent& input) noexcept {
            return input.Bindings().BindActionValue(JumpId);
        });
        return bound ? bound.Value() : input::ActionValueHandle{};
    }

    // component의 그 값 바인딩이 참인지다.
    bool ValueOn(ObjectHandle component, input::ActionValueHandle value) noexcept {
        auto read =
            Current().With<InputComponent>(component, [value](InputComponent& input) noexcept {
                return input.Bindings().ActionValue(value).AsBool();
            });
        return read && read.Value();
    }

    // 콜백 안에서 쓸 PlayerController 주소다(시험 전용 — 객체는 시험 동안 움직이지 않는다. With는
    // 포인터를 돌려주지 않으므로 정수로 옮긴다).
    PlayerController* ControllerPointer() noexcept {
        auto address = Controller(
            [](PlayerController& value) noexcept { return reinterpret_cast<uptr>(&value); });
        return address ? reinterpret_cast<PlayerController*>(address.Value()) : nullptr;
    }

    bool Down() noexcept {
        const input::InputEvent down[] = {input::InputEvent::KeyDown(input::Key::Space)};
        keyboard.BeginFrame(Span<const input::InputEvent>(down, 1));
        return engine->RunFrame().IsOk();
    }

    // 새 이벤트 없이(누른 채) 한 프레임 돌린다.
    bool Hold() noexcept {
        keyboard.BeginFrame(Span<const input::InputEvent>());
        return engine->RunFrame().IsOk();
    }

    bool Up() noexcept {
        const input::InputEvent up[] = {input::InputEvent::KeyUp(input::Key::Space)};
        keyboard.BeginFrame(Span<const input::InputEvent>(up, 1));
        return engine->RunFrame().IsOk();
    }

    // space를 누르고 한 프레임, 놓고 한 프레임 돌린다.
    bool Tap() noexcept {
        return Down() && Up();
    }

    SystemAllocator allocator;
    GameInstanceConfig game;
    EngineConfig config;
    input::InputState keyboard;
    UniquePtr<GameInstanceClient> client;
    UniquePtr<Engine> engine;
};

} // namespace

// 쌓기·정렬·위층부터 실행, 층 사이 consumes, blockInput 아래층 미전달과 값 0.
TEST(InputStack_OrderConsumptionAndBlocking) {
    Fixture fixture;
    REQUIRE(fixture.Start());
    const ObjectHandle low = fixture.Enable(fixture.Spawn());
    const ObjectHandle high = fixture.Enable(fixture.Spawn());
    REQUIRE(!low.IsNull() && !high.IsNull());
    REQUIRE(fixture.Configure(low, 1, false));
    REQUIRE(fixture.Configure(high, 2, false));
    input::ActionValueHandle value{};
    auto bound = fixture.Controller([&](PlayerController& controller) noexcept {
        auto valueHandle = controller.ActionBindings().BindActionValue(JumpId);
        if (valueHandle) {
            value = valueHandle.Value();
        }

        return fixture.Record(controller.ActionBindings(), 'p') && valueHandle.IsOk();
    });
    REQUIRE(bound && bound.Value());
    REQUIRE(fixture.RecordOn(low, 'a'));
    REQUIRE(fixture.RecordOn(high, 'b'));

    auto controllerValue = [&]() noexcept {
        auto read = fixture.Controller([&](PlayerController& controller) noexcept {
            return controller.ActionBindings().ActionValue(value).AsBool();
        });
        return read && read.Value();
    };

    // 위층(priority 2)부터: b → a → PlayerController 층. 누른 동안 PlayerController 값은 참이다.
    REQUIRE(fixture.Down());
    CHECK(calls.Is("bap"));
    CHECK(controllerValue());

    // 누른 채로 a 층이 막으면 PlayerController 층은 받지 않고 그 값 바인딩은 0이다.
    REQUIRE(fixture.Configure(low, 1, true));
    REQUIRE(fixture.Hold());
    CHECK(calls.Is(""));
    CHECK(!controllerValue());
    REQUIRE(fixture.Up());
    REQUIRE(fixture.Tap());
    CHECK(calls.Is("ba"));
    CHECK(!controllerValue());

    // 위층 consumes는 같은 층 뒤 바인딩과 아래층을 막는다(층 사이 소비). Started는 층 안에서 등록
    // 역순이라 c가 먼저고, c의 소비가 b·a·PlayerController 층을 막는다.
    REQUIRE(fixture.Configure(low, 1, false));
    REQUIRE(fixture.RecordOn(high, 'c', true));
    REQUIRE(fixture.Tap());
    CHECK(calls.Is("c"));
}

// Push 자리·중복 제거·Pop, EnableInput 재사용·DisableInput, 사라진 컴포넌트 정리, 형식 거부.
TEST(InputStack_PushPopAndEnableInput) {
    Fixture fixture;
    REQUIRE(fixture.Start());
    World& world = fixture.Current();
    const ObjectHandle actor = fixture.Spawn();
    const ObjectHandle component = fixture.Enable(actor);
    REQUIRE(!component.IsNull());
    CHECK(fixture.Enable(actor) == component);
    auto checks = fixture.Controller([&](PlayerController& controller) noexcept {
        bool ok = controller.IsInputComponentInStack(component);
        // 두 번 넣어도 하나다.
        ok = ok && controller.PushInputComponent(component).IsOk();
        ok = ok && controller.PopInputComponent(component);
        ok = ok && !controller.IsInputComponentInStack(component);
        ok = ok && !controller.PopInputComponent(component);
        // InputComponent가 아니면 TypeMismatch다.
        auto scene = world.AddComponent<SceneComponent>(actor);
        ok = ok && scene &&
             controller.PushInputComponent(scene.Value()).Error() == ErrorCode::TypeMismatch;
        // DisableInput은 그 Actor의 컴포넌트를 뺀다.
        ok = ok && controller.PushInputComponent(component).IsOk();
        ok = ok && controller.DisableInput(actor).IsOk();
        ok = ok && !controller.IsInputComponentInStack(component);
        ok = ok && controller.PushInputComponent(component).IsOk();
        return ok;
    });
    REQUIRE(checks);
    CHECK(checks.Value());

    // 파괴 대기는 Push가 거부하고, 다음 틱 스택 쌓기가 뺀다.
    REQUIRE(world.DestroyActor(actor));
    auto pending = fixture.Controller([&](PlayerController& controller) noexcept {
        return controller.PushInputComponent(component).Error();
    });
    REQUIRE(pending);
    CHECK(pending.Value() == ErrorCode::Destroyed);
    REQUIRE(fixture.engine->RunFrame());
    auto gone = fixture.Controller([&](PlayerController& controller) noexcept {
        return controller.IsInputComponentInStack(component);
    });
    REQUIRE(gone);
    CHECK(!gone.Value());
}

// 같은 priority는 나중에 Push한 것이 위다. PlayerController 층(같은 priority)은 Push 층보다 아래다.
// 다시 Push하면 맨 위로 간다.
TEST(InputStack_SamePriorityPushOrder) {
    Fixture fixture;
    REQUIRE(fixture.Start());
    const ObjectHandle first = fixture.Enable(fixture.Spawn());
    const ObjectHandle second = fixture.Enable(fixture.Spawn());
    REQUIRE(!first.IsNull() && !second.IsNull());
    auto bound = fixture.Controller([&](PlayerController& controller) noexcept {
        return fixture.Record(controller.ActionBindings(), 'p');
    });
    REQUIRE(bound && bound.Value());
    REQUIRE(fixture.RecordOn(first, 'x'));
    REQUIRE(fixture.RecordOn(second, 'y'));
    REQUIRE(fixture.Tap());
    CHECK(calls.Is("yxp"));

    auto pushed = fixture.Controller([first](PlayerController& controller) noexcept {
        return controller.PushInputComponent(first);
    });
    REQUIRE(pushed);
    REQUIRE(fixture.Tap());
    CHECK(calls.Is("xyp"));
}

// Pop·PlayerController 층 끄기는 값 바인딩을 0으로 하고, 꺼진 PlayerController 층은 전달받지
// 않는다(UE APlayerController::DisableInput). 켜면 돌아온다.
TEST(InputStack_PopAndDisableClearValues) {
    Fixture fixture;
    REQUIRE(fixture.Start());
    const ObjectHandle component = fixture.Enable(fixture.Spawn());
    REQUIRE(!component.IsNull());
    const input::ActionValueHandle componentValue = fixture.BindValueOn(component);
    input::ActionValueHandle value{};
    auto bound = fixture.Controller([&](PlayerController& controller) noexcept {
        auto valueHandle = controller.ActionBindings().BindActionValue(JumpId);
        if (valueHandle) {
            value = valueHandle.Value();
        }

        return fixture.Record(controller.ActionBindings(), 'p') && valueHandle.IsOk();
    });
    REQUIRE(bound && bound.Value());
    auto controllerValue = [&]() noexcept {
        auto read = fixture.Controller([&](PlayerController& controller) noexcept {
            return controller.ActionBindings().ActionValue(value).AsBool();
        });
        return read && read.Value();
    };

    REQUIRE(fixture.Down());
    CHECK(calls.Is("p"));
    CHECK(fixture.ValueOn(component, componentValue));
    CHECK(controllerValue());

    // Pop은 그 자리에서 값 0이다(누른 채라도).
    auto popped = fixture.Controller([component](PlayerController& controller) noexcept {
        return controller.PopInputComponent(component);
    });
    REQUIRE(popped);
    CHECK(popped.Value());
    CHECK(!fixture.ValueOn(component, componentValue));
    REQUIRE(fixture.Hold());
    CHECK(!fixture.ValueOn(component, componentValue));

    // 자기 층 끄기도 값 0이고, 다음 틱부터 전달하지 않는다.
    const ObjectHandle self = fixture.ControllerHandle();
    auto disabled = fixture.Controller([self](PlayerController& controller) noexcept {
        return controller.DisableInput(self).IsOk() && !controller.IsInputEnabled();
    });
    REQUIRE(disabled);
    CHECK(disabled.Value());
    CHECK(!controllerValue());
    REQUIRE(fixture.Hold());
    CHECK(!controllerValue());
    REQUIRE(fixture.Up());
    REQUIRE(fixture.Tap());
    CHECK(calls.Is(""));

    // 켜면 돌아온다(null handle).
    CHECK(fixture.Enable(self).IsNull());
    REQUIRE(fixture.Tap());
    CHECK(calls.Is("p"));
}

// 위층 콜백이 아래층을 Pop·파괴하면 그 틱 남은 전달도 받지 않는다. 콜백이 Push한 것은 다음
// 틱부터다.
TEST(InputStack_ReentrantChangesDuringDispatch) {
    Fixture fixture;
    REQUIRE(fixture.Start());
    World& world = fixture.Current();
    PlayerController* controller = fixture.ControllerPointer();
    REQUIRE(controller != nullptr);
    const ObjectHandle lowActor = fixture.Spawn();
    const ObjectHandle low = fixture.Enable(lowActor);
    const ObjectHandle high = fixture.Enable(fixture.Spawn());
    REQUIRE(!low.IsNull() && !high.IsNull());
    REQUIRE(fixture.Configure(low, 1, false));
    REQUIRE(fixture.Configure(high, 2, false));
    const input::ActionValueHandle lowValue = fixture.BindValueOn(low);
    REQUIRE(fixture.RecordOn(low, 'a'));

    // Pop: a는 불리지 않고 값도 다시 채워지지 않는다.
    REQUIRE(fixture.BindOn(high, [controller, low](const input::ActionEventData&) noexcept {
        calls.Add('b');
        (void)controller->PopInputComponent(low);
    }));
    REQUIRE(fixture.Down());
    CHECK(calls.Is("b"));
    CHECK(!controller->IsInputComponentInStack(low));
    CHECK(!fixture.ValueOn(low, lowValue));
    REQUIRE(fixture.Up());

    // 파괴: 다시 넣고 이번엔 위층 콜백이 아래층 Actor를 파괴한다.
    auto pushed = controller->PushInputComponent(low);
    REQUIRE(pushed);
    const ObjectHandle top = fixture.Enable(fixture.Spawn());
    REQUIRE(!top.IsNull());
    REQUIRE(fixture.Configure(top, 3, false));
    REQUIRE(fixture.BindOn(top, [&world, lowActor](const input::ActionEventData&) noexcept {
        calls.Add('t');
        (void)world.DestroyActor(lowActor);
    }));
    REQUIRE(fixture.Tap());
    // high는 Pop 콜백을 그대로 가지고 있다(b). low는 파괴 대기라 a가 불리지 않는다.
    CHECK(calls.Is("tb"));

    // 콜백이 Push한 컴포넌트는 그 틱에는 받지 않는다(스택은 틱 첫머리에 쌓는다).
    const ObjectHandle late = fixture.Spawn();
    REQUIRE(!late.IsNull());
    auto lateComponent = world.AddComponent<InputComponent>(late);
    REQUIRE(lateComponent);
    REQUIRE(fixture.RecordOn(lateComponent.Value(), 'l'));
    const ObjectHandle lateHandle = lateComponent.Value();
    REQUIRE(fixture.BindOn(top, [controller, lateHandle](const input::ActionEventData&) noexcept {
        (void)controller->PushInputComponent(lateHandle);
    }));
    REQUIRE(fixture.Tap());
    CHECK(calls.Is("tb"));
    REQUIRE(fixture.Tap());
    CHECK(calls.Is("tbl"));
}

// Pawn 층 순서(플레이어 입력 컴포넌트가 가장 아래 — UE "last dibs"), Push한 Pawn 컴포넌트는 한
// 번만, 위층 콜백의 UnPossess는 같은 틱 Pawn 층을 끊는다, 다른 Controller의 Pawn 거부, 다시
// Possess.
TEST(InputStack_PawnLayersAndPossession) {
    Fixture fixture;
    REQUIRE(fixture.Start());
    World& world = fixture.Current();
    PlayerController* controller = fixture.ControllerPointer();
    REQUIRE(controller != nullptr);
    const ObjectHandle pawn = fixture.SpawnPawn();
    REQUIRE(!pawn.IsNull());
    // Possess 전에 붙은 InputComponent(스크립트가 붙인 것 — UE components 순회 대상).
    auto extra = world.AddComponent<InputComponent>(pawn);
    REQUIRE(extra);
    REQUIRE(fixture.RecordOn(extra.Value(), 'q'));
    REQUIRE(controller->Possess(pawn));
    auto component =
        world.With<Pawn>(pawn, [](Pawn& value) noexcept { return value.PlayerInputComponent(); });
    REQUIRE(component && !component.Value().IsNull());
    const ObjectHandle playerComponent = component.Value();

    // 같은 priority 0: 나중에 쌓인 extra가 플레이어 입력 컴포넌트보다 위다.
    REQUIRE(fixture.Tap());
    CHECK(calls.Is("qw"));

    // Push한 플레이어 입력 컴포넌트는 Pawn 층에서 빠져 한 번만 받는다(스택 자리 — 맨 위).
    REQUIRE(controller->PushInputComponent(playerComponent));
    REQUIRE(fixture.Tap());
    CHECK(calls.Is("wq"));
    CHECK(controller->PopInputComponent(playerComponent));

    // 위층 콜백이 UnPossess하면 그 틱 Pawn 층은 받지 않는다.
    const ObjectHandle top = fixture.Enable(fixture.Spawn());
    REQUIRE(!top.IsNull());
    REQUIRE(fixture.Configure(top, 1, false));
    REQUIRE(fixture.BindOn(top, [controller](const input::ActionEventData&) noexcept {
        calls.Add('t');
        controller->UnPossess();
    }));
    REQUIRE(fixture.Tap());
    CHECK(calls.Is("t"));
    REQUIRE(controller->PopInputComponent(top));

    // 다른 Controller(AI)가 조종하는 Pawn은 InvalidArgument다.
    const ObjectHandle other = fixture.SpawnPawn();
    auto ai = world.SpawnActor<AiController>();
    REQUIRE(ai);
    auto aiPossessed = world.With<AiController>(
        ai.Value(), [other](AiController& value) noexcept { return value.Possess(other); });
    REQUIRE(aiPossessed);
    CHECK(controller->EnableInput(other).Error() == ErrorCode::InvalidArgument);
    CHECK(controller->DisableInput(other).Error() == ErrorCode::InvalidArgument);

    // 다시 Possess하면 hook을 다시 부르고 새 플레이어 입력 컴포넌트를 만든다.
    REQUIRE(controller->Possess(pawn));
    CHECK(InputPawn::setups == 2);
    auto renewed =
        world.With<Pawn>(pawn, [](Pawn& value) noexcept { return value.PlayerInputComponent(); });
    REQUIRE(renewed);
    CHECK(!renewed.Value().IsNull() && renewed.Value() != playerComponent);
    REQUIRE(fixture.Tap());
    CHECK(calls.Is("qw"));
}

// Possess 때 플레이어 입력 컴포넌트와 OnSetupPlayerInput, Pawn 입력 끄기·켜기, 다른 Controller의
// Pawn 거부, UnPossess 때 파괴.
TEST(InputStack_PawnPlayerInput) {
    Fixture fixture;
    REQUIRE(fixture.Start());
    World& world = fixture.Current();
    const ObjectHandle pawn = fixture.SpawnPawn();
    REQUIRE(!pawn.IsNull());
    auto possessed = fixture.Controller(
        [pawn](PlayerController& controller) noexcept { return controller.Possess(pawn); });
    REQUIRE(possessed);
    CHECK(InputPawn::setups == 1);
    auto component =
        world.With<Pawn>(pawn, [](Pawn& value) noexcept { return value.PlayerInputComponent(); });
    REQUIRE(component && !component.Value().IsNull());

    REQUIRE(fixture.Tap());
    CHECK(calls.Is("w"));

    // 입력을 끄면 Pawn 층은 쌓이지 않는다. 켜면 돌아오고 handle은 플레이어 입력 컴포넌트다.
    auto disabled = fixture.Controller(
        [pawn](PlayerController& controller) noexcept { return controller.DisableInput(pawn); });
    REQUIRE(disabled);
    REQUIRE(fixture.Tap());
    CHECK(calls.Is(""));
    CHECK(fixture.Enable(pawn) == component.Value());
    REQUIRE(fixture.Tap());
    CHECK(calls.Is("w"));

    // Controller가 없는(다른) Pawn은 InvalidArgument다.
    const ObjectHandle stray = fixture.SpawnPawn();
    auto rejected = fixture.Controller([stray](PlayerController& controller) noexcept {
        return controller.EnableInput(stray).Error();
    });
    REQUIRE(rejected);
    CHECK(rejected.Value() == ErrorCode::InvalidArgument);

    // UnPossess가 플레이어 입력 컴포넌트를 파괴한다.
    auto released =
        fixture.Controller([](PlayerController& controller) noexcept { controller.UnPossess(); });
    REQUIRE(released);
    auto after =
        world.With<Pawn>(pawn, [](Pawn& value) noexcept { return value.PlayerInputComponent(); });
    REQUIRE(after);
    CHECK(after.Value().IsNull());
    auto state = world.GetComponentState(component.Value());
    CHECK(!state || state.Value() == ComponentState::PendingDestroy);
    REQUIRE(fixture.Tap());
    CHECK(calls.Is(""));
}

} // namespace gameplay_input_stack_tests_file
