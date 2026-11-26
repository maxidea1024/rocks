#include "../../InputActions/Tests/InputAssetScene.h"

#include <Base/InitializerList.h>
#include <Core/Memory/SystemAllocator.h>
#include <Gameplay/Controller.generated.h>
#include <Gameplay/GameInstance.generated.h>
#include <Gameplay/GameInstanceClient.h>
#include <Gameplay/GameplayWorldSubsystem.h>
#include <Gameplay/LocalPlayer.generated.h>
#include <Gameplay/PlayerController.generated.h>
#include <Gameplay/PlayerController.h>
#include <Input/InputEvent.h>
#include <TestSupport/Test.h>
#include <World/Actor.generated.h>

// PlayerController 에셋 컨텍스트 시험이다(Docs/Design/InputActions 5.4절 — IA6b): 쥐기와 전달, 같은
// 에셋 다시 넣기(lease를 가져가 놓음), 다른 원천의 같은 id 거부, 등록 횟수 0에서 놓기, 데이터 변경
// 다시 모으기·id 변경·회수 lease, 평가기에서 직접 뺀 컨텍스트와 빼고 다시 넣은 같은 id(세대) 정리,
// EndPlay 해제, World 전체 다시 모으기.
// 에셋 장면(AssetManager)은 엔진보다 먼저 만들어 나중에 없앤다(엔진 종료가 lease를 놓는다).
namespace rocks::test {
ModuleLoader& ProcessModules() noexcept;
} // namespace rocks::test

using namespace rocks;
using namespace rocks::input_asset_test;
namespace gameplay_input_asset_tests_file {

namespace {

constexpr uint64 JumpPackage = 1;
constexpr uint64 ContextPackage = 2;
constexpr uint64 TwinPackage = 3;
constexpr uint64 CountedPackage = 4;
constexpr uint64 JumpLow = 10;
constexpr uint64 ContextLow = 20;
constexpr uint64 CountedLow = 21;
constexpr uint64 CodeContextLow = 22;
constexpr int32 RaisedPriority = 5;

struct Fixture {
    Fixture() noexcept {
        game.timeStep = {1.0 / 60.0, 0.25, 8};
        game.defaultMap.name = "Arena";
        config.modules = &test::ProcessModules();
        config.fixedFrameDeltaSeconds = 1.0 / 60.0;
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
        return engine->Initialize(config);
    }

    ~Fixture() {
        if (engine) {
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

    ObjectHandle Player(uint32 index) noexcept {
        auto player = client->WithInstance(
            [index](GameInstance& instance) noexcept { return instance.LocalPlayerAt(index); });
        return player.Value();
    }

    ObjectHandle PlayerControllerOf(uint32 index) noexcept {
        const ObjectHandle player = Player(index);
        auto controller = client->WithInstance([player](GameInstance& instance) noexcept {
            return instance.WithLocalPlayer(
                player, [](LocalPlayer& local) noexcept { return local.PlayerControllerHandle(); });
        });
        return controller ? controller.Value() : ObjectHandle{};
    }

    // index번째 PlayerController에서 fn을 부르고 그 결과를 돌려준다.
    template <class Fn>
    auto WithController(uint32 index, Fn&& fn) noexcept {
        return Current().With<PlayerController>(PlayerControllerOf(index), Forward<Fn>(fn));
    }

    SystemAllocator allocator;
    GameInstanceConfig game;
    EngineConfig config;
    UniquePtr<GameInstanceClient> client;
    UniquePtr<Engine> engine;
};

// 점프 액션(SpaceBar)과 그것을 쓰는 컨텍스트, 같은 id의 다른 컨텍스트, 등록 횟수를 세는 컨텍스트를
// 만든다.
bool BuildScene(InputAssetScene& scene) noexcept {
    const ObjectHandle jump =
        scene.AddAction(JumpPackage, Id(JumpLow), input::InputValueType::Boolean);
    const ObjectHandle context = scene.AddContext(ContextPackage, Id(ContextLow));
    const ObjectHandle twin = scene.AddContext(TwinPackage, Id(ContextLow));
    const ObjectHandle counted = scene.AddContext(CountedPackage, Id(CountedLow));
    if (jump.IsNull() || context.IsNull() || twin.IsNull() || counted.IsNull()) {
        return false;
    }

    input::InputContext* countedValue =
        asset_test::Pointer<input::InputContext>(scene.Source(), counted);
    if (countedValue == nullptr) {
        return false;
    }

    countedValue->data.registrationTracking =
        input::ContextRegistrationTracking::CountRegistrations;
    return scene.LinkAction(context, jump) && scene.LinkAction(twin, jump) &&
           scene.LinkAction(counted, jump) && scene.AddBinding(context, Id(JumpLow), "SpaceBar") &&
           scene.AddBinding(twin, Id(JumpLow), "SpaceBar") &&
           scene.AddBinding(counted, Id(JumpLow), "SpaceBar");
}

// 로드한 컨텍스트 에셋을 고친다(에셋 Registry 쓰기).
template <class Fn>
bool EditLoadedContext(InputAssetScene& scene, const AssetLease& lease, Fn&& fn) noexcept {
    bool edited = false;
    auto opened = scene.manager->With(lease, [&](ObjectHandle object) noexcept {
        ObjectRegistry& registry = scene.manager->Registry();
        auto writable =
            registry.ResolveForWrite<input::InputContext>(registry.OwnerAccess(), object);
        if (!writable) {
            return;
        }

        auto pointer = writable.Value().Get();
        if (pointer) {
            fn(*pointer.Value());
            edited = true;
        }
    });
    return opened.IsOk() && edited;
}

// 점프 콜백 횟수를 세는 바인딩을 건다.
bool BindJump(Fixture& fixture, uint32 index, Allocator& allocator, uint32& jumps) noexcept {
    auto callback = input::InputActionBindings::Callback::TryCreate(
        allocator, [&jumps](const input::ActionEventData&) noexcept { ++jumps; });
    if (!callback) {
        return false;
    }

    auto bound = fixture.WithController(index, [&](PlayerController& value) noexcept {
        return value.ActionBindings()
            .BindAction(Id(JumpLow), input::ActionEvent::Started, Move(callback.Value()))
            .IsOk();
    });
    return bound && bound.Value();
}

// key를 누르고 한 프레임, 놓고 한 프레임 돌린다.
bool Tap(Fixture& fixture, input::InputState& keyboard, input::Key key) noexcept {
    const input::InputEvent down[] = {input::InputEvent::KeyDown(key)};
    keyboard.BeginFrame(Span<const input::InputEvent>(down, 1));
    if (!fixture.engine->RunFrame()) {
        return false;
    }

    const input::InputEvent up[] = {input::InputEvent::KeyUp(key)};
    keyboard.BeginFrame(Span<const input::InputEvent>(up, 1));
    return fixture.engine->RunFrame().IsOk();
}

} // namespace

// 넣으면 lease를 쥐고 평가기가 그 바인딩을 전달한다. 같은 에셋을 다시 넣으면 받은 lease를 가져가
// 놓고(쥔 것은 처음 lease 하나) 우선순위만 바뀐다. 다른 원천이 같은 id를 넣었으면
// InvalidArgument이고 lease는 그대로다. 대표 에셋이 InputContext가 아니면 TypeMismatch다.
TEST(PlayerControllerInputAssets_AddHoldsLeaseAndRejectsOtherSources) {
    SystemAllocator allocator;
    asset_test::AssetEnv env;
    InputAssetScene scene(env);
    REQUIRE(scene.Ready() && BuildScene(scene) && scene.Start());
    {
        Fixture fixture;
        REQUIRE(fixture.Start());
        input::InputState keyboard;
        const ObjectHandle player = fixture.Player(0);
        REQUIRE(fixture.client->WithInstance([&](GameInstance& instance) noexcept {
            return instance.SetLocalPlayerInput(player, &keyboard);
        }));
        uint32 jumps = 0;
        REQUIRE(BindJump(fixture, 0, allocator, jumps));

        auto first = scene.Load(ContextPackage);
        auto again = scene.Load(ContextPackage);
        auto twin = scene.Load(TwinPackage);
        auto action = scene.Load(JumpPackage);
        REQUIRE(first && again && twin && action);
        const AssetId contextAsset = asset_test::FixedId(ContextPackage);
        auto added = fixture.WithController(0, [&](PlayerController& value) noexcept {
            CHECK(!value.HasInputContext(contextAsset));
            // 대표 에셋이 액션이면 TypeMismatch(lease 그대로).
            CHECK(value.AddInputContext(*scene.manager, Move(action.Value()), 0).Error() ==
                  ErrorCode::TypeMismatch);
            CHECK(action.Value().IsValid());
            const bool firstAdded =
                value.AddInputContext(*scene.manager, Move(first.Value()), 0).IsOk();
            CHECK(value.HasInputContext(contextAsset));
            // 같은 에셋 다시 넣기: 성공하고 받은 lease를 가져간다.
            const bool againAdded =
                value.AddInputContext(*scene.manager, Move(again.Value()), RaisedPriority).IsOk();
            // 같은 id의 다른 에셋은 거부한다.
            CHECK(value.AddInputContext(*scene.manager, Move(twin.Value()), 0).Error() ==
                  ErrorCode::InvalidArgument);
            CHECK(twin.Value().IsValid());
            CHECK(!value.HasInputContext(asset_test::FixedId(TwinPackage)));
            return firstAdded && againAdded;
        });
        REQUIRE(added && added.Value());
        CHECK(!first.Value().IsValid() && !again.Value().IsValid());
        // 쥔 것은 처음 lease 하나다(다시 넣은 lease는 놓였다).
        CHECK(scene.LeasesAfterPump(ContextPackage) == 1);

        // 넣은 뒤 빈 프레임 하나(다시 만들 때 눌린 키는 놓을 때까지 무시한다 — UE와 같다).
        REQUIRE(fixture.engine->RunFrame());
        REQUIRE(Tap(fixture, keyboard, input::Key::Space));
        CHECK(jumps == 1);

        // 직접 넣은 코드 bundle의 id와 같은 에셋도 거부한다.
        auto counted = scene.Load(CountedPackage);
        REQUIRE(counted);
        auto rejected = fixture.WithController(0, [&](PlayerController& value) noexcept {
            input::InputContextBundle code(allocator);
            code.id = Id(CountedLow);
            CHECK(value.ActionInput().AddContext(Move(code), 0).IsOk());
            return value.AddInputContext(*scene.manager, Move(counted.Value()), 0).Error();
        });
        REQUIRE(rejected);
        CHECK(rejected.Value() == ErrorCode::InvalidArgument);
        CHECK(counted.Value().IsValid());
        // 빈 lease는 StaleHandle이다.
        auto stale = fixture.WithController(0, [&](PlayerController& value) noexcept {
            return value.AddInputContext(*scene.manager, AssetLease(), 0).Error();
        });
        REQUIRE(stale);
        CHECK(stale.Value() == ErrorCode::StaleHandle);
        REQUIRE(fixture.client->WithInstance([&](GameInstance& instance) noexcept {
            return instance.SetLocalPlayerInput(player, nullptr);
        }));
    }

    // 엔진 종료(EndPlay)가 쥔 lease를 놓았다.
    CHECK(scene.LeasesAfterPump(ContextPackage) == 0);
}

// 등록 횟수를 세는 컨텍스트는 횟수가 0이 될 때 lease를 놓고, 쥐지 않은 에셋 제거는 NotFound다.
// 평가기에서 직접 뺀 컨텍스트는 Has가 거짓·Refresh가 NotFound이고 다음 틱에 lease를 놓는다.
TEST(PlayerControllerInputAssets_RemoveAndDirectRemovalRelease) {
    // 코드 bundle의 allocator는 PlayerController보다 오래 살아야 한다(Fixture보다 먼저 만든다).
    SystemAllocator allocator;
    asset_test::AssetEnv env;
    InputAssetScene scene(env);
    REQUIRE(scene.Ready() && BuildScene(scene) && scene.Start());
    Fixture fixture;
    REQUIRE(fixture.Start());
    const AssetId countedAsset = asset_test::FixedId(CountedPackage);
    const AssetId contextAsset = asset_test::FixedId(ContextPackage);
    {
        auto first = scene.Load(CountedPackage);
        auto second = scene.Load(CountedPackage);
        auto plain = scene.Load(ContextPackage);
        REQUIRE(first && second && plain);
        auto done = fixture.WithController(0, [&](PlayerController& value) noexcept {
            bool ok = value.AddInputContext(*scene.manager, Move(first.Value()), 0).IsOk() &&
                      value.AddInputContext(*scene.manager, Move(second.Value()), 0).IsOk() &&
                      value.AddInputContext(*scene.manager, Move(plain.Value()), 0).IsOk();
            // 두 번 넣었으니 한 번 빼도 남는다.
            ok = ok && value.RemoveInputContext(countedAsset).IsOk();
            CHECK(value.HasInputContext(countedAsset));
            ok = ok && value.RemoveInputContext(countedAsset).IsOk();
            CHECK(!value.HasInputContext(countedAsset));
            CHECK(value.RemoveInputContext(countedAsset).Error() == ErrorCode::NotFound);
            CHECK(value.RefreshInputContext(*scene.manager, countedAsset).Error() ==
                  ErrorCode::NotFound);
            return ok;
        });
        REQUIRE(done && done.Value());
        CHECK(!second.Value().IsValid());
    }

    CHECK(scene.LeasesAfterPump(CountedPackage) == 0);
    CHECK(scene.LeasesAfterPump(ContextPackage) == 1);
    auto cleared = fixture.WithController(0, [&](PlayerController& value) noexcept {
        value.ActionInput().ClearContexts();
        const bool has = value.HasInputContext(contextAsset);
        const ErrorCode refresh = value.RefreshInputContext(*scene.manager, contextAsset).Error();
        return !has && refresh == ErrorCode::NotFound;
    });
    REQUIRE(cleared);
    CHECK(cleared.Value());
    REQUIRE(fixture.engine->RunFrame());
    CHECK(scene.LeasesAfterPump(ContextPackage) == 0);

    // 빼고 같은 id를 코드 bundle로 다시 넣은 칸: 세대가 달라 끝난 칸이다. 코드 bundle은 그대로
    // 남고(Remove가 NotFound — 코드 컨텍스트를 빼지 않는다) lease는 놓인다.
    auto lease = scene.Load(ContextPackage);
    REQUIRE(lease);
    auto replaced = fixture.WithController(0, [&](PlayerController& value) noexcept {
        if (!value.AddInputContext(*scene.manager, Move(lease.Value()), 0)) {
            return false;
        }

        if (!value.ActionInput().RemoveContext(Id(ContextLow))) {
            return false;
        }

        input::InputContextBundle code(allocator);
        code.id = Id(ContextLow);
        if (!value.ActionInput().AddContext(Move(code), 0)) {
            return false;
        }

        const bool stale = !value.HasInputContext(contextAsset);
        const bool notFound = value.RemoveInputContext(contextAsset).Error() == ErrorCode::NotFound;
        return stale && notFound && value.ActionInput().HasContext(Id(ContextLow));
    });
    REQUIRE(replaced);
    CHECK(replaced.Value());
    CHECK(scene.LeasesAfterPump(ContextPackage) == 0);
}

// 에셋 데이터를 고친 뒤 Refresh가 평가기 사본을 바꾼다(SpaceBar → Enter). id를 바꾼 데이터는
// InvalidArgument이고 사본은 그대로다. 회수된 lease는 StaleHandle이다. World 전체 Refresh는 그
// 에셋을 쥔 플레이어만 다시 모으고, 쥔 플레이어가 없으면 성공한다.
TEST(PlayerControllerInputAssets_RefreshAndWorldRefresh) {
    SystemAllocator allocator;
    asset_test::AssetEnv env;
    InputAssetScene scene(env);
    REQUIRE(scene.Ready() && BuildScene(scene) && scene.Start());
    Fixture fixture;
    REQUIRE(fixture.Start());
    auto second = fixture.client->WithInstance(
        [](GameInstance& instance) noexcept { return instance.CreateLocalPlayer(1); });
    REQUIRE(second);
    input::InputState keyboard;
    const ObjectHandle player = fixture.Player(0);
    REQUIRE(fixture.client->WithInstance([&](GameInstance& instance) noexcept {
        return instance.SetLocalPlayerInput(player, &keyboard);
    }));
    uint32 jumps = 0;
    REQUIRE(BindJump(fixture, 0, allocator, jumps));
    const AssetId contextAsset = asset_test::FixedId(ContextPackage);
    auto edit = scene.Load(ContextPackage);
    REQUIRE(edit);
    {
        auto lease = scene.Load(ContextPackage);
        REQUIRE(lease);
        auto added = fixture.WithController(0, [&](PlayerController& value) noexcept {
            return value.AddInputContext(*scene.manager, Move(lease.Value()), 0).IsOk();
        });
        REQUIRE(added && added.Value());
    }

    // 넣은 뒤 빈 프레임 하나(다시 만들 때 눌린 키는 놓을 때까지 무시한다).
    REQUIRE(fixture.engine->RunFrame());
    // 쥔 플레이어가 없는 에셋은 성공(아무 일 없음).
    GameplayWorldSubsystem* gameplay = fixture.Current().GetSubsystem<GameplayWorldSubsystem>();
    REQUIRE(gameplay != nullptr);
    CHECK(gameplay
              ->RefreshInputContext(fixture.Current(), *scene.manager,
                                    asset_test::FixedId(TwinPackage))
              .IsOk());

    REQUIRE(EditLoadedContext(scene, edit.Value(), [](input::InputContext& context) noexcept {
        (void)context.data.bindings[0].key.TryAssign("Enter");
    }));
    // 다시 모으기 전에는 옛 사본(SpaceBar)이다.
    REQUIRE(Tap(fixture, keyboard, input::Key::Space));
    CHECK(jumps == 1);
    REQUIRE(gameplay->RefreshInputContext(fixture.Current(), *scene.manager, contextAsset, true));
    REQUIRE(Tap(fixture, keyboard, input::Key::Space));
    CHECK(jumps == 1);
    REQUIRE(Tap(fixture, keyboard, input::Key::Enter));
    CHECK(jumps == 2);
    // 두 번째 플레이어는 쥐지 않았다.
    auto other = fixture.WithController(
        1, [&](PlayerController& value) noexcept { return value.HasInputContext(contextAsset); });
    REQUIRE(other);
    CHECK(!other.Value());

    // id를 바꾼 데이터는 거부하고 사본은 그대로(Enter)다.
    REQUIRE(EditLoadedContext(scene, edit.Value(), [](input::InputContext& context) noexcept {
        context.id = Id(CodeContextLow);
    }));
    auto changed = fixture.WithController(0, [&](PlayerController& value) noexcept {
        return value.RefreshInputContext(*scene.manager, contextAsset).Error();
    });
    REQUIRE(changed);
    CHECK(changed.Value() == ErrorCode::InvalidArgument);
    CHECK(gameplay->RefreshInputContext(fixture.Current(), *scene.manager, contextAsset).Error() ==
          ErrorCode::InvalidArgument);
    REQUIRE(Tap(fixture, keyboard, input::Key::Enter));
    CHECK(jumps == 3);
    // 같은 에셋을 바뀐 id로 다시 넣는 것도 거부하고 받은 lease는 호출자 것 그대로다.
    auto readd = scene.Load(ContextPackage);
    REQUIRE(readd);
    auto readded = fixture.WithController(0, [&](PlayerController& value) noexcept {
        return value.AddInputContext(*scene.manager, Move(readd.Value()), 0).Error();
    });
    REQUIRE(readded);
    CHECK(readded.Value() == ErrorCode::InvalidArgument);
    CHECK(readd.Value().IsValid());
    readd.Value() = AssetLease();
    REQUIRE(EditLoadedContext(scene, edit.Value(), [](input::InputContext& context) noexcept {
        context.id = Id(ContextLow);
    }));

    // 회수된 lease: scope로 받은 lease를 넣고 scope를 Revoke한다.
    auto scope = scene.manager->CreateScope();
    REQUIRE(scope);
    AssetRequest request;
    request.id = asset_test::FixedId(CountedPackage);
    request.scope = &scope.Value();
    auto scoped = scene.manager->LoadBlocking(request, "PlayerControllerInputAssetTests.Scoped");
    REQUIRE(scoped);
    const AssetId countedAsset = asset_test::FixedId(CountedPackage);
    auto scopedAdded = fixture.WithController(0, [&](PlayerController& value) noexcept {
        return value.AddInputContext(*scene.manager, Move(scoped.Value()), 0).IsOk();
    });
    REQUIRE(scopedAdded && scopedAdded.Value());
    REQUIRE(scope.Value().Revoke());
    auto revoked = fixture.WithController(0, [&](PlayerController& value) noexcept {
        return value.RefreshInputContext(*scene.manager, countedAsset).Error();
    });
    REQUIRE(revoked);
    CHECK(revoked.Value() == ErrorCode::StaleHandle);
    // 같은 에셋을 새 lease로 다시 넣으면 쥔 lease가 새것으로 바뀌어 Refresh가 다시 된다.
    auto fresh = scene.Load(CountedPackage);
    REQUIRE(fresh);
    auto revived = fixture.WithController(0, [&](PlayerController& value) noexcept {
        return value.AddInputContext(*scene.manager, Move(fresh.Value()), 0).IsOk() &&
               value.RefreshInputContext(*scene.manager, countedAsset).IsOk();
    });
    REQUIRE(revived);
    CHECK(revived.Value());
    CHECK(!fresh.Value().IsValid());
    CHECK(scene.LeasesAfterPump(CountedPackage) == 1);
    REQUIRE(fixture.client->WithInstance([&](GameInstance& instance) noexcept {
        return instance.SetLocalPlayerInput(player, nullptr);
    }));
}

// EndPlay(엔진 종료)는 등록 횟수를 세는 컨텍스트를 두 번 넣었어도 0이 될 때까지 빼고 lease를 모두
// 놓는다. 끝난 칸(빼고 같은 id를 코드 bundle로 다시 넣은 것)은 lease만 놓이고 코드 컨텍스트는
// EndPlay 전까지 남는다.
TEST(PlayerControllerInputAssets_EndPlayReleasesEverything) {
    SystemAllocator allocator;
    asset_test::AssetEnv env;
    InputAssetScene scene(env);
    REQUIRE(scene.Ready() && BuildScene(scene) && scene.Start());
    {
        Fixture fixture;
        REQUIRE(fixture.Start());
        auto first = scene.Load(CountedPackage);
        auto second = scene.Load(CountedPackage);
        auto plain = scene.Load(ContextPackage);
        REQUIRE(first && second && plain);
        auto added = fixture.WithController(0, [&](PlayerController& value) noexcept {
            if (!value.AddInputContext(*scene.manager, Move(first.Value()), 0) ||
                !value.AddInputContext(*scene.manager, Move(second.Value()), 0) ||
                !value.AddInputContext(*scene.manager, Move(plain.Value()), 0)) {
                return false;
            }

            // plain 칸을 끝낸다: 빼고 같은 id를 코드 bundle로 넣는다.
            input::InputContextBundle code(allocator);
            code.id = Id(ContextLow);
            return value.ActionInput().RemoveContext(Id(ContextLow)).IsOk() &&
                   value.ActionInput().AddContext(Move(code), 0).IsOk() &&
                   value.HasInputContext(asset_test::FixedId(CountedPackage)) &&
                   !value.HasInputContext(asset_test::FixedId(ContextPackage));
        });
        REQUIRE(added);
        CHECK(added.Value());
        // 다음 틱이 끝난 칸의 lease를 놓는다. 코드 컨텍스트는 남는다.
        REQUIRE(fixture.engine->RunFrame());
        CHECK(scene.LeasesAfterPump(ContextPackage) == 0);
        CHECK(scene.LeasesAfterPump(CountedPackage) == 1);
        auto code = fixture.WithController(0, [](PlayerController& value) noexcept {
            return value.ActionInput().HasContext(Id(ContextLow)) &&
                   value.ActionInput().HasContext(Id(CountedLow));
        });
        REQUIRE(code);
        CHECK(code.Value());
    }

    // 엔진 종료의 EndPlay가 등록 두 번짜리 컨텍스트의 lease까지 놓았다.
    CHECK(scene.LeasesAfterPump(CountedPackage) == 0);
}

} // namespace gameplay_input_asset_tests_file
