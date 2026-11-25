#include <Base/InitializerList.h>
#include <Collision/BodySettings.generated.h>
#include <Collision/CollisionSettings.generated.h>
#include <Collision/CollisionSettings.h>
#include <Collision/CollisionTypes.generated.h>
#include <Config/Tests/ConfigTestSupport.h>
#include <Core/Diagnostics/LogSystem.h>
#include <Core/Diagnostics/RuntimeLogSink.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Memory/UniquePtr.h>
#include <TestSupport/Test.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Lights/LightingChannels.generated.h>
#include <World/Primitives/BoxComponent.generated.h>
#include <World/Primitives/BoxComponent.h>
#include <World/Primitives/PrimitiveComponent.generated.h>
#include <World/Primitives/PrimitiveTypes.generated.h>
#include <World/Primitives/ShapeComponent.generated.h>
#include <World/Primitives/SphereComponent.generated.h>
#include <World/Primitives/SphereComponent.h>
#include <World/SceneComponent.generated.h>
#include <World/World.h>
#include <string.h>

// PrimitiveComponent 충돌 해석 시험이다(Docs/Design/CollisionData.ko.md 9절 CD2): World profile 표,
// 등록·이름 setter의 profile 값 복사, "Custom"으로 바꾸는 setter, 없는 profile(마지막 해석 값·경고
// 한 번), 설정 문맥이 없는 World.
using namespace rocks;
namespace world_primitive_collision_tests_file {

namespace {

// 시험용 엔진 층이다: Shape 기본(OverlapAllDynamic)과 BlockAll·Pawnlike 셋.
constexpr const char* CollisionTestLayer = R"({ "Collision": { "profiles": [
    { "name": "OverlapAllDynamic", "collisionEnabled": "QueryOnly", "objectTypeName": "WorldDynamic",
      "customResponses": [ { "channel": "Pawn", "response": "Overlap" },
                           { "channel": "WorldStatic", "response": "Overlap" } ] },
    { "name": "BlockAll", "collisionEnabled": "QueryAndPhysics", "objectTypeName": "WorldStatic" },
    { "name": "Pawnlike", "collisionEnabled": "QueryAndPhysics", "objectTypeName": "Pawn",
      "customResponses": [ { "channel": "Visibility", "response": "Ignore" } ] },
    { "name": "", "collisionEnabled": "QueryOnly" } ],
  "profileRedirects": [ { "oldName": "OldPawn", "newName": "Pawnlike" } ] } })";

// 충돌 진단 경고 수를 센다(CollisionProfileMissing·CollisionProfileEntryDropped).
class MissingProfileCounter final : public RuntimeLogSink {
public:
    Result<void> Write(const LogEvent& event) noexcept override {
        if (Contains(event.message, "(CollisionProfileMissing)")) {
            ++missing;
        }

        if (Contains(event.message, "(CollisionProfileEntryDropped)")) {
            ++dropped;
        }

        return Result<void>::Success();
    }

    uint32 missing = 0;
    uint32 dropped = 0;

private:
    static bool Contains(StringView text, const char* needle) noexcept {
        const usize needleBytes = strlen(needle);
        for (usize start = 0; start + needleBytes <= text.ByteCount(); ++start) {
            if (memcmp(text.Data() + start, needle, needleBytes) == 0) {
                return true;
            }
        }

        return false;
    }
};

class CollisionTestActor final : public Actor {
    ROCKS_NATIVE_TYPE(CollisionTestActor, Actor, 0x7d21c0e9a43f5b86ull, 0x1b6e94f2c80a7d35ull)

public:
    explicit CollisionTestActor(Allocator& allocator) noexcept : Actor(allocator) {
    }
};

struct CollisionWorldFixture {
    explicit CollisionWorldFixture(bool withSettings) noexcept
        : types(allocator), modules(types, allocator) {
        registered = RegisterWorldTypes(modules).IsOk() && RegisterActor();
        bool configured = true;
        if (withSettings) {
            configured = RegisterCollisionTypes(settingsFixture.registration).IsOk() &&
                         settingsFixture.Context().RegisterSettings<CollisionSettings>().IsOk() &&
                         settingsFixture.Context()
                             .AddLayerText(config::ConfigLayer::Engine, "Engine.json", "Engine",
                                           "<engine>", StringView(CollisionTestLayer))
                             .IsOk() &&
                         settingsFixture.Context().Sync().IsOk();
        }

        registered = registered && configured;
        WorldDesc desc;
        desc.name = "Collision";
        desc.context = {0x434f4c4cull, 1};
        desc.timeStep = {1.0 / 60.0, 0.25, 8};
        desc.settings = withSettings ? &settingsFixture.Context() : nullptr;
        auto created = UniquePtr<World>::TryMake(allocator, allocator, types, desc);
        if (created) {
            world = Move(created.Value());
        }
    }

    ~CollisionWorldFixture() {
        world.Reset();
        if (token) {
            CHECK(types.Unregister(*token).IsOk());
        }

        CHECK(modules.Unregister().IsOk());
    }

    bool RegisterActor() noexcept {
        auto registeredToken = RegisterNativeClass<CollisionTestActor>(types);
        if (!registeredToken) {
            return false;
        }

        token = registeredToken.Value();
        return true;
    }

    template <class T>
    ObjectHandle Add(ObjectHandle actor) noexcept {
        auto added = world->AddComponent<T>(actor);
        return added ? added.Value() : ObjectHandle{};
    }

    ObjectHandle Spawn() noexcept {
        auto spawned = world->SpawnActor<CollisionTestActor>();
        return spawned ? spawned.Value() : ObjectHandle{};
    }

    // 설정 문맥은 World보다 오래 산다(먼저 선언).
    test::config::ConfigFixture settingsFixture;
    SystemAllocator allocator;
    TypeRegistry types;
    ModuleRegistration modules;
    Optional<TypeRegistrationToken> token;
    bool registered = false;
    UniquePtr<World> world;
};

bool HasCustomResponse(const BodySettings& body, StringView channel,
                       CollisionResponse response) noexcept {
    for (const ChannelResponse& entry : body.customResponses) {
        if (entry.channel.View() == channel && entry.response == response) {
            return true;
        }
    }

    return false;
}

} // namespace

// World가 설정 문맥에서 표를 만들고, 등록 때 클래스 기본 profile 값을 body에 옮긴다.
TEST(PrimitiveCollision_RegistrationCopiesProfile) {
    CollisionWorldFixture fixture(true);
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    CHECK(world.CollisionProfiles().Profiles().Size() == 3);
    const ObjectHandle sphere = fixture.Add<SphereComponent>(fixture.Spawn());
    REQUIRE(!sphere.IsNull());
    CHECK(world.With<SphereComponent>(sphere, [](SphereComponent& shape) noexcept {
        auto resolved = shape.TryResolveCollision();
        REQUIRE(resolved);
        CHECK(resolved.Value().fromProfile && !resolved.Value().profileMissing);
        CHECK(resolved.Value().profileName == StringView("OverlapAllDynamic"));
        CHECK(resolved.Value().collisionEnabled == CollisionEnabled::QueryOnly);
        CHECK(resolved.Value().objectType == CollisionChannel::WorldDynamic);
        CHECK(resolved.Value().responses.Get(CollisionChannel::Pawn) == CollisionResponse::Overlap);
        CHECK(resolved.Value().responses.Get(CollisionChannel::Camera) == CollisionResponse::Block);
        // 등록 때 profile 값이 body 사용자 지정 칸에 들어갔다(UE ReadConfig 복사).
        const BodySettings& body = shape.GetBodySettings();
        CHECK(body.collisionEnabled == CollisionEnabled::QueryOnly);
        CHECK(body.objectType == CollisionChannel::WorldDynamic);
        CHECK(body.customResponses.Size() == 2);
        CHECK(HasCustomResponse(body, "Pawn", CollisionResponse::Overlap));
        CHECK(HasCustomResponse(body, "WorldStatic", CollisionResponse::Overlap));

        // 이름 setter도 profile 값을 옮긴다.
        REQUIRE(shape.SetCollisionProfileName("Pawnlike"));
        CHECK(body.objectType == CollisionChannel::Pawn);
        CHECK(body.collisionEnabled == CollisionEnabled::QueryAndPhysics);
        CHECK(body.customResponses.Size() == 1);
        CHECK(HasCustomResponse(body, "Visibility", CollisionResponse::Ignore));

        // 표에 없는 이름: body는 마지막 해석 값(Pawnlike) 그대로이고 그 값으로 해석한다.
        REQUIRE(shape.SetCollisionProfileName("NoSuchProfile"));
        CHECK(shape.CollisionProfileName() == StringView("NoSuchProfile"));
        auto missing = shape.TryResolveCollision();
        REQUIRE(missing);
        CHECK(missing.Value().profileMissing && !missing.Value().fromProfile);
        CHECK(missing.Value().objectType == CollisionChannel::Pawn);
        CHECK(missing.Value().responses.Get(CollisionChannel::Visibility) ==
              CollisionResponse::Ignore);

        // redirect 이름은 새 profile 값을 옮긴다(이름은 철자 그대로).
        REQUIRE(shape.SetCollisionProfileName("BlockAll"));
        REQUIRE(shape.SetCollisionProfileName("OldPawn"));
        CHECK(shape.CollisionProfileName() == StringView("OldPawn"));
        CHECK(body.objectType == CollisionChannel::Pawn);
        auto redirected = shape.TryResolveCollision();
        REQUIRE(redirected);
        CHECK(redirected.Value().fromProfile &&
              redirected.Value().profileName == StringView("Pawnlike"));

        // 클래스 기본으로(대소문자 무시) 돌아가면 빈 값으로 두고 그 profile 값을 옮긴다.
        REQUIRE(shape.SetCollisionProfileName("overlapalldynamic"));
        CHECK(shape.CollisionProfileName() == StringView("OverlapAllDynamic"));
        CHECK(body.objectType == CollisionChannel::WorldDynamic);
        CHECK(body.collisionEnabled == CollisionEnabled::QueryOnly);
        CHECK(body.customResponses.Size() == 2);
    }));
    REQUIRE(world.Shutdown());
}

// "Custom"으로 바꾸는 setter: 값이 바뀔 때만, 나머지 칸은 직전 profile 값, 범위 밖 enum 거부.
TEST(PrimitiveCollision_CustomSetters) {
    CollisionWorldFixture fixture(true);
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    const ObjectHandle box = fixture.Add<BoxComponent>(fixture.Spawn());
    REQUIRE(!box.IsNull());
    CHECK(world.With<BoxComponent>(box, [](BoxComponent& shape) noexcept {
        // Enabled·응답은 같은 값이면 이름을 바꾸지 않는다(UE: 값이 바뀔 때만
        // InvalidateCollisionProfileName).
        REQUIRE(shape.SetCollisionResponseToChannel(CollisionChannel::Pawn,
                                                    CollisionResponse::Overlap));
        REQUIRE(shape.SetCollisionEnabled(CollisionEnabled::QueryOnly));
        CHECK(shape.CollisionProfileName() == StringView("OverlapAllDynamic"));

        REQUIRE(shape.SetCollisionResponseToChannel(CollisionChannel::Camera,
                                                    CollisionResponse::Ignore));
        CHECK(shape.CollisionProfileName() == CustomCollisionProfileName);
        auto custom = shape.TryResolveCollision();
        REQUIRE(custom);
        CHECK(!custom.Value().fromProfile && !custom.Value().profileMissing);
        CHECK(custom.Value().responses.Get(CollisionChannel::Camera) == CollisionResponse::Ignore);
        CHECK(custom.Value().responses.Get(CollisionChannel::Pawn) == CollisionResponse::Overlap);
        CHECK(custom.Value().objectType == CollisionChannel::WorldDynamic);
        CHECK(HasCustomResponse(shape.GetBodySettings(), "Camera", CollisionResponse::Ignore));

        REQUIRE(shape.SetCollisionObjectType(CollisionChannel::PhysicsBody));
        REQUIRE(shape.SetCollisionEnabled(CollisionEnabled::QueryAndProbe));
        auto edited = shape.TryResolveCollision();
        REQUIRE(edited);
        CHECK(edited.Value().objectType == CollisionChannel::PhysicsBody);
        CHECK(edited.Value().collisionEnabled == CollisionEnabled::QueryAndProbe);
        CHECK(edited.Value().responses.Get(CollisionChannel::Camera) == CollisionResponse::Ignore);

        REQUIRE(shape.SetCollisionResponseToAllChannels(CollisionResponse::Block));
        auto all = shape.TryResolveCollision();
        REQUIRE(all);
        CHECK(all.Value().responses == CollisionResponses(CollisionResponse::Block));

        // 범위 밖 enum은 거부하고 바꾸지 않는다.
        CHECK(!shape.SetCollisionEnabled(static_cast<CollisionEnabled>(CollisionEnabledCount)));
        CHECK(!shape.SetCollisionObjectType(static_cast<CollisionChannel>(CollisionChannelCount)));
        CHECK(!shape.SetCollisionResponseToChannel(
            CollisionChannel::Pawn, static_cast<CollisionResponse>(CollisionResponseCount)));
        CHECK(!shape.SetCollisionResponseToAllChannels(
            static_cast<CollisionResponse>(CollisionResponseCount)));
        CHECK(shape.GetBodySettings().objectType == CollisionChannel::PhysicsBody);

        // profile 이름으로 돌아가면 그 값을 다시 옮긴다.
        REQUIRE(shape.SetCollisionProfileName("BlockAll"));
        CHECK(shape.GetBodySettings().objectType == CollisionChannel::WorldStatic);
        CHECK(shape.GetBodySettings().customResponses.IsEmpty());

        // object type은 UE처럼 같은 값이어도 Custom으로 바꾼다(BodyInstance.cpp:675).
        REQUIRE(shape.SetCollisionObjectType(CollisionChannel::WorldStatic));
        CHECK(shape.CollisionProfileName() == CustomCollisionProfileName);
        CHECK(shape.GetBodySettings().objectType == CollisionChannel::WorldStatic);
    }));
    REQUIRE(world.Shutdown());
}

// 등록 때 표에 없는 profile이면 경고를 한 번 남긴다(지연 Spawn으로 등록 전에 이름을 정한다).
TEST(PrimitiveCollision_MissingProfileWarnsOnce) {
    MissingProfileCounter counter;
    SystemAllocator logAllocator;
    LogSystemConfig logConfig;
    logConfig.mode = LogMode::Manual;
    logConfig.globalMinimum = LogLevel::Debug;
    LogSystem logs(logAllocator, logConfig);
    REQUIRE(logs.Start() && logs.AddSink(counter));
    LogInstallScope installed(logs);
    CollisionWorldFixture fixture(true);
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    auto deferred = world.SpawnActorDeferred<CollisionTestActor>();
    REQUIRE(deferred);
    const ObjectHandle sphere = fixture.Add<SphereComponent>(deferred.Value());
    const ObjectHandle known = fixture.Add<SphereComponent>(deferred.Value());
    REQUIRE(!sphere.IsNull() && !known.IsNull());
    REQUIRE(world.With<SphereComponent>(sphere, [](SphereComponent& shape) noexcept {
        return shape.SetCollisionProfileName("Nope");
    }));
    REQUIRE(world.FinishSpawning(deferred.Value()));
    (void)logs.Pump();
    CHECK(counter.missing == 1u);
    // 표의 잘못된 항목(빈 이름 profile)은 Initialize 때 항목마다 경고했다.
    CHECK(counter.dropped == 1u);
    // 등록 뒤 setter·해석은 다시 경고하지 않는다.
    REQUIRE(world.With<SphereComponent>(known, [](SphereComponent& shape) noexcept {
        return shape.SetCollisionProfileName("AlsoMissing");
    }));
    REQUIRE(world.With<SphereComponent>(
        known, [](SphereComponent& shape) noexcept { return shape.TryResolveCollision(); }));
    (void)logs.Pump();
    CHECK(counter.missing == 1u);
    // body는 기본 사용자 지정 값(QueryAndPhysics·WorldStatic)으로 해석된다.
    CHECK(world.With<SphereComponent>(sphere, [](SphereComponent& shape) noexcept {
        auto resolved = shape.TryResolveCollision();
        REQUIRE(resolved);
        CHECK(resolved.Value().profileMissing);
        CHECK(resolved.Value().collisionEnabled == CollisionEnabled::QueryAndPhysics);
        CHECK(resolved.Value().objectType == CollisionChannel::WorldStatic);
    }));
    REQUIRE(world.Shutdown());
}

// 설정 문맥이 없는 World는 빈 표다: 모든 이름이 profileMissing이고 setter는 그대로 동작한다.
TEST(PrimitiveCollision_NoSettingsContext) {
    CollisionWorldFixture fixture(false);
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    CHECK(world.CollisionProfiles().Profiles().IsEmpty());
    const ObjectHandle sphere = fixture.Add<SphereComponent>(fixture.Spawn());
    REQUIRE(!sphere.IsNull());
    CHECK(world.With<SphereComponent>(sphere, [](SphereComponent& shape) noexcept {
        auto resolved = shape.TryResolveCollision();
        REQUIRE(resolved);
        CHECK(resolved.Value().profileMissing);
        CHECK(resolved.Value().objectType == CollisionChannel::WorldStatic);
        REQUIRE(shape.SetCollisionObjectType(CollisionChannel::Pawn));
        CHECK(shape.CollisionProfileName() == CustomCollisionProfileName);
        auto custom = shape.TryResolveCollision();
        REQUIRE(custom);
        CHECK(!custom.Value().profileMissing &&
              custom.Value().objectType == CollisionChannel::Pawn);
    }));
    REQUIRE(world.Shutdown());
}

} // namespace world_primitive_collision_tests_file
