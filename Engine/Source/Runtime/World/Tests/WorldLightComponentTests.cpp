#include <Base/InitializerList.h>
#include <Core/Diagnostics/LogSystem.h>
#include <Core/Diagnostics/RuntimeLogSink.h>
#include <Core/Math/Rotator.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Memory/UniquePtr.h>
#include <Meta/TypeSchema.h>
#include <ObjectSerialization/ObjectGraph.h>
#include <TestSupport/Test.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Lights/DirectionalLightComponent.generated.h>
#include <World/Lights/DirectionalLightComponent.h>
#include <World/Level.generated.h>
#include <World/Lights/Light.generated.h>
#include <World/Lights/PointLight.generated.h>
#include <World/Lights/SpotLight.generated.h>
#include <World/Lights/RectLight.generated.h>
#include <World/Lights/DirectionalLight.generated.h>
#include <World/Lights/SkyLight.generated.h>
#include <World/Lights/Light.h>
#include <World/Lights/PointLight.h>
#include <World/Lights/SpotLight.h>
#include <World/Lights/RectLight.h>
#include <World/Lights/DirectionalLight.h>
#include <World/Lights/SkyLight.h>
#include <World/Lights/LightUnits.generated.h>
#include <World/Lights/LightComponentBase.generated.h>
#include <World/Lights/LightComponent.generated.h>
#include <World/Lights/LocalLightComponent.generated.h>
#include <World/Lights/LightUnits.h>
#include <World/Lights/LightComponentBase.h>
#include <World/Lights/LightComponent.h>
#include <World/Lights/LocalLightComponent.h>
#include <World/Lights/PointLightComponent.generated.h>
#include <World/Lights/SpotLightComponent.generated.h>
#include <World/Lights/PointLightComponent.h>
#include <World/Lights/SpotLightComponent.h>
#include <World/Lights/RectLightComponent.generated.h>
#include <World/Lights/RectLightComponent.h>
#include <World/SceneComponent.generated.h>
#include <World/Lights/SkyLightComponent.generated.h>
#include <World/Lights/SkyLightComponent.h>
#include <World/World.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// 컴포넌트 데이터 먼저 D1 광원 시험이다(Docs/Design/EngineRuntime 24.2.7절): UE 기본값, setter
// 검증, Mobility 검사, 단위·밝기·원뿔 계산, 색, 광원 Actor root, 리플렉션·저장 왕복.
using namespace rocks;
namespace world_light_component_tests_file {

namespace {

constexpr double VectorTolerance = 1.0e-6;
constexpr float RelativeTolerance = 1.0e-5f;

// 상대 오차로 비교한다(0이면 절대 오차).
bool NearFloat(float value, float expected) noexcept {
    const float scale = fabsf(expected) > 1.0f ? fabsf(expected) : 1.0f;
    const bool same = fabsf(value - expected) <= RelativeTolerance * scale;
    if (!same) {
        printf("  value %.9g, expected %.9g\n", static_cast<double>(value),
               static_cast<double>(expected));
    }

    return same;
}

bool NearVector(const Vec3d& value, double x, double y, double z) noexcept {
    const bool same = fabs(value.x - x) <= VectorTolerance &&
                      fabs(value.y - y) <= VectorTolerance && fabs(value.z - z) <= VectorTolerance;
    if (!same) {
        printf("  vector (%g, %g, %g), expected (%g, %g, %g)\n", value.x, value.y, value.z, x, y,
               z);
    }

    return same;
}

ErrorCode ErrorOf(const Result<void>& result) noexcept {
    return result ? ErrorCode::None : result.Error();
}

template <class T>
T* Get(Borrow<T>& borrow) noexcept {
    auto pointer = borrow.Get();
    return pointer ? pointer.Value() : nullptr;
}

// 진단 코드가 든 Warning 수를 센다(Manual 모드 LogSystem에 붙여 Pump로 받는다).
class DiagnosticCounter final : public RuntimeLogSink {
public:
    Result<void> Write(const LogEvent& event) noexcept override {
        if (event.level != LogLevel::Warning) {
            return Result<void>::Success();
        }

        if (Contains(event.message, "(StaticComponentDataChanged)")) {
            ++dataChanged;
        }

        if (Contains(event.message, "(ComponentDataLockedInGame)")) {
            ++locked;
        }

        return Result<void>::Success();
    }

    uint32 dataChanged = 0;
    uint32 locked = 0;

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

class EmptyActor final : public Actor {
    ROCKS_NATIVE_TYPE(EmptyActor, Actor, 0x6a1e3c9f0b7d2548ull, 0xd3f84b2e9c1a0576ull)

public:
    explicit EmptyActor(Allocator& allocator) noexcept : Actor(allocator) {
    }
};

// OnAwake 안에서 Static 점 광원을 만들고 세기를 바꾸는 Actor다(초기화 전 변경은 된다).
class StaticLampActor final : public Actor {
    ROCKS_NATIVE_TYPE(StaticLampActor, Actor, 0x0f5c7e2a9d3b1864ull, 0x7b29e0d4a6c3f158ull)

public:
    explicit StaticLampActor(Allocator& allocator) noexcept : Actor(allocator) {
    }

    Result<void> OnAwake() noexcept override {
        auto lamp = GetWorld().AddComponent<PointLightComponent>(Handle());
        if (!lamp) {
            return Result<void>::Failure(lamp.Error());
        }

        auto set = GetWorld().With<PointLightComponent>(
            lamp.Value(), [](PointLightComponent& light) noexcept {
                light.SetMobility(ComponentMobility::Static);
                return light.SetIntensity(1234.0f);
            });
        if (!set) {
            return set;
        }

        return SetRootComponent(lamp.Value());
    }
};

struct Fixture {
    explicit Fixture(WorldKind kind = WorldKind::Game) noexcept
        : types(allocator), modules(types, allocator) {
        registered = RegisterWorldTypes(modules).IsOk() && Register<EmptyActor>() &&
                     Register<StaticLampActor>();
        WorldDesc desc;
        desc.name = "Lights";
        desc.context = {0x4c49474854ull, 1};
        desc.timeStep = {1.0 / 60.0, 0.25, 8};
        desc.kind = kind;
        auto created = UniquePtr<World>::TryMake(allocator, allocator, types, desc);
        if (created) {
            world = Move(created.Value());
        }
    }

    ~Fixture() {
        world.Reset();
        while (tokenCount != 0) {
            CHECK(types.Unregister(tokens[--tokenCount]).IsOk());
        }

        CHECK(modules.Unregister().IsOk());
    }

    template <class T>
    bool Register() noexcept {
        auto token = RegisterNativeClass<T>(types);
        if (!token || tokenCount == MaxTokens) {
            return false;
        }

        tokens[tokenCount++] = token.Value();
        return true;
    }

    // 초기화한 World에 빈 Actor를 놓고 T를 붙인다(Actor는 Initialized — 컴포넌트는 바로 등록).
    template <class T>
    ObjectHandle AddLight(ObjectHandle& actor) noexcept {
        if (actor.IsNull()) {
            auto spawned = world->SpawnActor<EmptyActor>();
            if (!spawned) {
                return {};
            }

            actor = spawned.Value();
        }

        auto added = world->AddComponent<T>(actor);
        return added ? added.Value() : ObjectHandle{};
    }

    ObjectHandle RootOf(ObjectHandle actor) noexcept {
        auto root =
            world->With<Actor>(actor, [](Actor& value) noexcept { return value.RootComponent(); });
        return root ? root.Value() : ObjectHandle{};
    }

    static constexpr usize MaxTokens = 4;
    SystemAllocator allocator;
    TypeRegistry types;
    ModuleRegistration modules;
    TypeRegistrationToken tokens[MaxTokens];
    usize tokenCount = 0;
    bool registered = false;
    UniquePtr<World> world;
};

} // namespace

// 24.2.2절: 클래스마다 UE 생성자 기본값. 추상 타입은 만들 수 없다.
TEST(Light_ComponentDefaultsMatchUe) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    ObjectHandle actor;
    const ObjectHandle point = fixture.AddLight<PointLightComponent>(actor);
    const ObjectHandle spot = fixture.AddLight<SpotLightComponent>(actor);
    const ObjectHandle rect = fixture.AddLight<RectLightComponent>(actor);
    const ObjectHandle directional = fixture.AddLight<DirectionalLightComponent>(actor);
    const ObjectHandle sky = fixture.AddLight<SkyLightComponent>(actor);
    REQUIRE(!point.IsNull() && !spot.IsNull() && !rect.IsNull() && !directional.IsNull() &&
            !sky.IsNull());

    auto pointChecked =
        world.With<PointLightComponent>(point, [](PointLightComponent& light) noexcept {
            // LightComponentBase·LightComponent 공통.
            CHECK(light.Intensity() == 5000.0f);
            CHECK(light.LightColor() == Color::White());
            CHECK(light.AffectsWorld() && light.CastsShadows() && light.CastsStaticShadows() &&
                  light.CastsDynamicShadows() && light.AffectsTranslucentLighting());
            CHECK(!light.HasTransmission() && !light.CastsVolumetricShadow());
            CHECK(light.IndirectLightingIntensity() == 1.0f);
            CHECK(light.VolumetricScatteringIntensity() == 1.0f);
            CHECK(light.Temperature() == 6500.0f && !light.UsesTemperature());
            CHECK(light.MaxDrawDistance() == 0.0f && light.MaxDistanceFadeRange() == 0.0f);
            CHECK(light.SpecularScale() == 1.0f && light.DiffuseScale() == 1.0f);
            CHECK(light.ShadowResolutionScale() == 1.0f);
            CHECK(light.ShadowBias() == 0.5f && light.ShadowSlopeBias() == 0.5f);
            CHECK(light.ShadowSharpen() == 0.0f && light.ContactShadowLength() == 0.0f);
            CHECK(!light.ContactShadowLengthInWorldSpace() && !light.CastsTranslucentShadows());
            CHECK(light.LightingChannels() == LightingChannels{});
            CHECK(light.LightingChannels().channel0 && !light.LightingChannels().channel1 &&
                  !light.LightingChannels().channel2);
            // LocalLight·Point.
            CHECK(light.IntensityUnits() == LightUnits::Unitless);
            CHECK(light.AttenuationRadius() == 1000.0f && light.InverseExposureBlend() == 0.0f);
            CHECK(light.UsesInverseSquaredFalloff() && light.LightFalloffExponent() == 8.0f);
            CHECK(light.SourceRadius() == 0.0f && light.SoftSourceRadius() == 0.0f &&
                  light.SourceLength() == 0.0f);
            // 컴포넌트 기본 Mobility는 Movable이다(UE USceneComponent — Actor가 Stationary로
            // 바꾼다).
            CHECK(light.Mobility() == ComponentMobility::Movable);
        });
    CHECK(pointChecked);
    auto spotChecked = world.With<SpotLightComponent>(spot, [](SpotLightComponent& light) noexcept {
        CHECK(light.InnerConeAngle() == 0.0f && light.OuterConeAngle() == 44.0f);
        CHECK(light.Intensity() == 5000.0f);
    });
    CHECK(spotChecked);
    auto rectChecked = world.With<RectLightComponent>(rect, [](RectLightComponent& light) noexcept {
        CHECK(light.SourceWidth() == 64.0f && light.SourceHeight() == 64.0f);
        CHECK(light.BarnDoorAngle() == 88.0f && light.BarnDoorLength() == 20.0f);
        CHECK(light.Intensity() == 5000.0f && light.AttenuationRadius() == 1000.0f);
    });
    CHECK(rectChecked);
    auto directionalChecked = world.With<DirectionalLightComponent>(
        directional, [](DirectionalLightComponent& light) noexcept {
            CHECK(light.Intensity() == 10.0f);
            CHECK(light.CastsTranslucentShadows() && light.CastsVolumetricShadow());
            CHECK(light.LightSourceAngle() == 0.5357f && light.LightSourceSoftAngle() == 0.0f);
            CHECK(light.ShadowAmount() == 1.0f);
            CHECK(light.DynamicShadowDistanceMovableLight() == 40000.0f);
            CHECK(light.DynamicShadowDistanceStationaryLight() == 0.0f);
            CHECK(light.DynamicShadowCascades() == 4);
            CHECK(light.CascadeDistributionExponent() == 3.0f);
            CHECK(light.CascadeTransitionFraction() == 0.1f);
            CHECK(light.ShadowDistanceFadeoutFraction() == 0.1f);
            CHECK(light.FarShadowCascadeCount() == 0 && light.FarShadowDistance() == 300000.0f);
            CHECK(light.ForwardShadingPriority() == 0);
            CHECK(light.IsAtmosphereSunLight() && light.AtmosphereSunLightIndex() == 0);
            CHECK(light.AtmosphereSunDiskColorScale() == LinearColor::White());
            CHECK(light.IntensityUnits() == LightUnits::Unitless);
        });
    CHECK(directionalChecked);
    auto skyChecked = world.With<SkyLightComponent>(sky, [](SkyLightComponent& light) noexcept {
        CHECK(light.Intensity() == 1.0f);
        CHECK(light.Mobility() == ComponentMobility::Stationary);
        CHECK(light.CastsVolumetricShadow());
        // UE는 ULightComponent 생성자만 반투명 조명을 켠다(하늘광은 ULightComponentBase 파생).
        CHECK(!light.AffectsTranslucentLighting());
        CHECK(!light.UsesRealTimeCapture());
        CHECK(light.SourceType() == SkyLightSourceType::CapturedScene);
        CHECK(light.SourceCubemapAngle() == 0.0f && light.CubemapResolution() == 128);
        CHECK(light.SkyDistanceThreshold() == 150000.0f);
        CHECK(!light.CapturesEmissiveOnly() && light.IsLowerHemisphereBlack());
        CHECK(light.LowerHemisphereColor() == LinearColor::Black());
    });
    CHECK(skyChecked);

    // 추상 타입은 만들 수 없다.
    CHECK(!world.AddComponent(actor, ObjectTypeIdOf<LightComponentBase>()));
    CHECK(!world.AddComponent(actor, ObjectTypeIdOf<LightComponent>()));
    CHECK(!world.AddComponent(actor, ObjectTypeIdOf<LocalLightComponent>()));
    CHECK(!world.SpawnActor(ObjectTypeIdOf<Light>()));
    REQUIRE(world.Shutdown());
}

// 24.2.3절: 값 검증(비유한·범위 밖 거부, 값 유지), EV 음수, 1/r² 끄기의 단위 규칙.
TEST(Light_SettersValidateValues) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    ObjectHandle actor;
    const ObjectHandle point = fixture.AddLight<PointLightComponent>(actor);
    const ObjectHandle rect = fixture.AddLight<RectLightComponent>(actor);
    const ObjectHandle directional = fixture.AddLight<DirectionalLightComponent>(actor);
    const ObjectHandle sky = fixture.AddLight<SkyLightComponent>(actor);
    REQUIRE(!point.IsNull() && !rect.IsNull() && !directional.IsNull() && !sky.IsNull());
    const float nan = NAN;
    const float infinity = INFINITY;

    auto pointChecked =
        world.With<PointLightComponent>(point, [&](PointLightComponent& light) noexcept {
            CHECK(ErrorOf(light.SetIntensity(-1.0f)) == ErrorCode::InvalidArgument);
            CHECK(ErrorOf(light.SetIntensity(nan)) == ErrorCode::InvalidArgument);
            CHECK(ErrorOf(light.SetIntensity(infinity)) == ErrorCode::InvalidArgument);
            CHECK(light.Intensity() == 5000.0f);
            CHECK(light.SetIntensity(0.0f));
            // EV만 음수를 받는다.
            CHECK(light.SetIntensityUnits(LightUnits::EV));
            CHECK(light.SetIntensity(-2.0f));
            CHECK(light.Intensity() == -2.0f);
            CHECK(ErrorOf(light.SetIntensityUnits(static_cast<LightUnits>(9))) ==
                  ErrorCode::InvalidArgument);
            CHECK(light.IntensityUnits() == LightUnits::EV);
            // 1/r²를 끄면 Unitless로 돌아가고 다른 단위를 거부한다. EV의 음수 세기는 0이 된다(UE
            // PostEditChangeProperty).
            CHECK(light.SetUseInverseSquaredFalloff(false));
            CHECK(light.IntensityUnits() == LightUnits::Unitless);
            CHECK(light.Intensity() == 0.0f);
            CHECK(ErrorOf(light.SetIntensityUnits(LightUnits::Candelas)) ==
                  ErrorCode::InvalidArgument);
            CHECK(light.SetIntensityUnits(LightUnits::Unitless));
            CHECK(light.SetUseInverseSquaredFalloff(true));
            CHECK(light.SetIntensityUnits(LightUnits::EV) && light.SetIntensity(-3.0f));
            CHECK(light.SetIntensityUnits(LightUnits::Candelas));
            CHECK(light.Intensity() == 0.0f);
            // 범위(거부하면 값 유지).
            CHECK(ErrorOf(light.SetSpecularScale(1.5f)) == ErrorCode::InvalidArgument);
            CHECK(ErrorOf(light.SetDiffuseScale(-0.1f)) == ErrorCode::InvalidArgument);
            CHECK(light.SetSpecularScale(0.25f) && light.SpecularScale() == 0.25f);
            CHECK(ErrorOf(light.SetSourceRadius(10001.0f)) == ErrorCode::InvalidArgument);
            CHECK(ErrorOf(light.SetSourceLength(-1.0f)) == ErrorCode::InvalidArgument);
            CHECK(light.SourceRadius() == 0.0f && light.SourceLength() == 0.0f);
            CHECK(light.SetSourceRadius(10000.0f));
            CHECK(ErrorOf(light.SetLightFalloffExponent(0.0f)) == ErrorCode::InvalidArgument);
            CHECK(light.LightFalloffExponent() == 8.0f);
            CHECK(ErrorOf(light.SetInverseExposureBlend(1.01f)) == ErrorCode::InvalidArgument);
            CHECK(ErrorOf(light.SetShadowResolutionScale(-1.0f)) == ErrorCode::InvalidArgument);
            CHECK(ErrorOf(light.SetContactShadowLength(-0.5f)) == ErrorCode::InvalidArgument);
            CHECK(ErrorOf(light.SetTemperature(nan)) == ErrorCode::InvalidArgument);
            CHECK(ErrorOf(light.SetAttenuationRadius(infinity)) == ErrorCode::InvalidArgument);
            CHECK(ErrorOf(light.SetAttenuationRadius(-1.0f)) == ErrorCode::InvalidArgument);
            CHECK(light.AttenuationRadius() == 1000.0f);
            CHECK(light.SetAttenuationRadius(250.0f) && light.AttenuationRadius() == 250.0f);
            CHECK(light.SpecularScale() == 0.25f && light.DiffuseScale() == 1.0f);
            CHECK(light.InverseExposureBlend() == 0.0f && light.ShadowResolutionScale() == 1.0f);
            CHECK(light.ContactShadowLength() == 0.0f && light.Temperature() == 6500.0f);
            // 점 광원 그림자 편향은 유한성만 본다(방향광만 [0, 10]).
            CHECK(light.SetShadowBias(10.5f) && light.SetShadowSlopeBias(-1.0f));
            // 선형 색의 비유한 성분은 거부한다.
            CHECK(ErrorOf(light.SetLightColor(LinearColor(nan, 0.0f, 0.0f))) ==
                  ErrorCode::InvalidArgument);
            CHECK(light.LightColor() == Color::White());
            LightingChannels channels;
            channels.channel0 = false;
            channels.channel2 = true;
            CHECK(light.SetLightingChannels(channels));
            CHECK(light.LightingChannels() == channels);
        });
    CHECK(pointChecked);
    auto rectChecked = world.With<RectLightComponent>(rect, [](RectLightComponent& light) noexcept {
        CHECK(ErrorOf(light.SetSourceWidth(0.5f)) == ErrorCode::InvalidArgument);
        CHECK(ErrorOf(light.SetSourceHeight(100001.0f)) == ErrorCode::InvalidArgument);
        CHECK(light.SetSourceWidth(1.0f) && light.SourceWidth() == 1.0f);
        CHECK(ErrorOf(light.SetBarnDoorAngle(88.5f)) == ErrorCode::InvalidArgument);
        CHECK(ErrorOf(light.SetBarnDoorLength(-1.0f)) == ErrorCode::InvalidArgument);
        CHECK(ErrorOf(light.SetBarnDoorLength(0.05f)) == ErrorCode::InvalidArgument);
        CHECK(light.SourceHeight() == 64.0f && light.BarnDoorAngle() == 88.0f &&
              light.BarnDoorLength() == 20.0f);
        CHECK(light.SetBarnDoorLength(0.1f));
        CHECK(light.SetBarnDoorAngle(0.0f));
    });
    CHECK(rectChecked);
    auto directionalChecked = world.With<DirectionalLightComponent>(
        directional, [&](DirectionalLightComponent& light) noexcept {
            CHECK(ErrorOf(light.SetDynamicShadowCascades(11)) == ErrorCode::InvalidArgument);
            CHECK(ErrorOf(light.SetDynamicShadowCascades(-1)) == ErrorCode::InvalidArgument);
            CHECK(light.SetDynamicShadowCascades(10));
            CHECK(ErrorOf(light.SetFarShadowCascadeCount(11)) == ErrorCode::InvalidArgument);
            CHECK(ErrorOf(light.SetCascadeTransitionFraction(0.0f)) == ErrorCode::InvalidArgument);
            CHECK(ErrorOf(light.SetCascadeTransitionFraction(0.31f)) == ErrorCode::InvalidArgument);
            CHECK(ErrorOf(light.SetCascadeDistributionExponent(0.05f)) ==
                  ErrorCode::InvalidArgument);
            CHECK(ErrorOf(light.SetShadowAmount(2.0f)) == ErrorCode::InvalidArgument);
            CHECK(ErrorOf(light.SetAtmosphereSunLightIndex(2)) == ErrorCode::InvalidArgument);
            CHECK(light.SetAtmosphereSunLightIndex(1));
            CHECK(ErrorOf(light.SetForwardShadingPriority(-1)) == ErrorCode::InvalidArgument);
            CHECK(ErrorOf(light.SetLightSourceAngle(-0.1f)) == ErrorCode::InvalidArgument);
            CHECK(ErrorOf(light.SetDynamicShadowDistanceMovableLight(-1.0f)) ==
                  ErrorCode::InvalidArgument);
            CHECK(ErrorOf(light.SetAtmosphereSunDiskColorScale(LinearColor(nan, 1.0f, 1.0f))) ==
                  ErrorCode::InvalidArgument);
            CHECK(light.AtmosphereSunDiskColorScale() == LinearColor::White());
            CHECK(light.FarShadowCascadeCount() == 0 && light.CascadeTransitionFraction() == 0.1f);
            CHECK(light.CascadeDistributionExponent() == 3.0f && light.ShadowAmount() == 1.0f);
            // 방향광 그림자 편향은 [0, 10]이다(UE DirectionalLightComponent.cpp:1107).
            CHECK(ErrorOf(light.SetShadowBias(10.5f)) == ErrorCode::InvalidArgument);
            CHECK(ErrorOf(light.SetShadowSlopeBias(-0.1f)) == ErrorCode::InvalidArgument);
            CHECK(light.ShadowBias() == 0.5f && light.ShadowSlopeBias() == 0.5f);
            CHECK(light.SetShadowBias(10.0f));
        });
    CHECK(directionalChecked);
    auto skyChecked = world.With<SkyLightComponent>(sky, [&](SkyLightComponent& light) noexcept {
        // UE처럼 회전 각은 자르지 않고 유한성만 본다.
        CHECK(ErrorOf(light.SetSourceCubemapAngle(nan)) == ErrorCode::InvalidArgument);
        CHECK(light.SetSourceCubemapAngle(361.0f) && light.SourceCubemapAngle() == 361.0f);
        CHECK(ErrorOf(light.SetSourceType(static_cast<SkyLightSourceType>(5))) ==
              ErrorCode::InvalidArgument);
        CHECK(light.SetSourceType(SkyLightSourceType::SpecifiedCubemap));
        CHECK(ErrorOf(light.SetSkyDistanceThreshold(-1.0f)) == ErrorCode::InvalidArgument);
        CHECK(ErrorOf(light.SetLowerHemisphereColor(LinearColor(0.0f, infinity, 0.0f))) ==
              ErrorCode::InvalidArgument);
        // 하늘광 세기는 Unitless라 음수를 거부한다.
        CHECK(ErrorOf(light.SetIntensity(-1.0f)) == ErrorCode::InvalidArgument);
    });
    CHECK(skyChecked);
    REQUIRE(world.Shutdown());
}

// 24.2.3절 Mobility: 게임 World 초기화 뒤 Static은 거부, Stationary는 세기 되고 반경·원뿔 각
// 거부, affectsWorld는 늘 거부(진단 코드와 함께). 편집 World와 OnAwake 안은 막지 않는다.
TEST(Light_MobilityGatesDataChanges) {
    for (WorldKind kind : {WorldKind::Game, WorldKind::Tryout, WorldKind::Editor}) {
        // sink는 LogSystem보다 먼저 선언한다(LogSystem이 먼저 소멸해야 한다).
        DiagnosticCounter diagnostics;
        SystemAllocator logAllocator;
        LogSystemConfig logConfig;
        logConfig.mode = LogMode::Manual;
        logConfig.globalMinimum = LogLevel::Debug;
        LogSystem logs(logAllocator, logConfig);
        REQUIRE(logs.Start() && logs.AddSink(diagnostics));
        LogInstallScope installed(logs);
        Fixture fixture(kind);
        REQUIRE(fixture.registered);
        World& world = *fixture.world;
        REQUIRE(world.Initialize());
        const bool game = kind != WorldKind::Editor;
        const ErrorCode blocked = game ? ErrorCode::InvalidArgument : ErrorCode::None;
        ObjectHandle actor;
        const ObjectHandle spot = fixture.AddLight<SpotLightComponent>(actor);
        REQUIRE(!spot.IsNull());

        auto checked =
            world.With<SpotLightComponent>(spot, [&](SpotLightComponent& light) noexcept {
                // Movable은 다 된다(affectsWorld 제외).
                CHECK(light.SetIntensity(10.0f));
                CHECK(light.SetAttenuationRadius(500.0f));
                CHECK(light.SetOuterConeAngle(30.0f));
                CHECK(ErrorOf(light.SetAffectsWorld(false)) == blocked);

                light.SetMobility(ComponentMobility::Stationary);
                CHECK(light.SetIntensity(20.0f));
                CHECK(light.SetLightColor(Color(255, 0, 0)));
                CHECK(ErrorOf(light.SetAttenuationRadius(600.0f)) == blocked);
                CHECK(ErrorOf(light.SetInnerConeAngle(10.0f)) == blocked);
                CHECK(ErrorOf(light.SetOuterConeAngle(40.0f)) == blocked);

                light.SetMobility(ComponentMobility::Static);
                CHECK(ErrorOf(light.SetIntensity(30.0f)) == blocked);
                CHECK(ErrorOf(light.SetCastShadows(false)) == blocked);
                CHECK(ErrorOf(light.SetIntensityUnits(LightUnits::Lumens)) == blocked);
                CHECK(ErrorOf(light.SetUseInverseSquaredFalloff(false)) == blocked);
                CHECK(light.Intensity() == (game ? 20.0f : 30.0f));
                CHECK(light.AttenuationRadius() == (game ? 500.0f : 600.0f));
                CHECK(light.UsesInverseSquaredFalloff() == game);
                // 값 검증은 Mobility보다 먼저다(어느 World든 InvalidArgument).
                CHECK(ErrorOf(light.SetIntensity(nan(""))) == ErrorCode::InvalidArgument);
                // 광원 채널은 UE처럼 Mobility를 보지 않는다.
                LightingChannels channels;
                channels.channel1 = true;
                CHECK(light.SetLightingChannels(channels));
            });
        CHECK(checked);
        (void)logs.Pump();
        // 게임 World: Stationary 3건 + Static 4건, affectsWorld 1건.
        CHECK(diagnostics.dataChanged == (game ? 7u : 0u));
        CHECK(diagnostics.locked == (game ? 1u : 0u));

        // 광원 Actor(Stationary root): 세기는 되고 반경은 게임 World에서 막힌다.
        auto lamp = world.SpawnActor<PointLight>();
        REQUIRE(lamp);
        auto lampChecked = world.With<PointLightComponent>(
            fixture.RootOf(lamp.Value()), [&](PointLightComponent& light) noexcept {
                CHECK(light.SetIntensity(42.0f));
                CHECK(ErrorOf(light.SetAttenuationRadius(10.0f)) == blocked);
            });
        CHECK(lampChecked);
        (void)logs.Pump();
        CHECK(diagnostics.dataChanged == (game ? 8u : 0u));

        // OnAwake 안의 Static 변경은 된다(Actor 초기화 전).
        auto awake = world.SpawnActor<StaticLampActor>();
        REQUIRE(awake);
        auto awakeChecked = world.With<PointLightComponent>(
            fixture.RootOf(awake.Value()), [](PointLightComponent& light) noexcept {
                CHECK(light.Mobility() == ComponentMobility::Static);
                CHECK(light.Intensity() == 1234.0f);
                CHECK(light.HasStaticLighting() && light.HasStaticShadowing());
            });
        CHECK(awakeChecked);
        REQUIRE(world.Shutdown());
    }
}

// 24.2.4절: 단위 변환 계수(UE GetUnitsConversionFactor)와 Point·Spot·Rect 밝기.
TEST(Light_UnitsAndBrightnessMatchUe) {
    const float pi = math::Pi<float>;
    using Local = LocalLightComponent;
    // 같은 단위는 1, 계수는 원 단위 → 내부(Candelas·EV 1e4, Lumens 1e4/(2π(1−cos)), Nits 1,
    // Unitless 16) × 내부 → 대상.
    // 5×5 전체: 계수 = 원 단위 내부 배율 ÷ 대상 단위 내부 배율(구 — cos −1, 2π(1 − cos) = 4π).
    // 순서는 LightUnits(Unitless·Candelas·Lumens·EV·Nits)다.
    const float toInternal[] = {16.0f, 1.0e4f, 1.0e4f / (4.0f * pi), 1.0e4f, 1.0f};
    for (uint8 from = 0; from <= static_cast<uint8>(LightUnits::Nits); ++from) {
        for (uint8 to = 0; to <= static_cast<uint8>(LightUnits::Nits); ++to) {
            const float factor = Local::UnitsConversionFactor(static_cast<LightUnits>(from),
                                                              static_cast<LightUnits>(to));
            if (from == to) {
                CHECK(factor == 1.0f);
            } else {
                CHECK(NearFloat(factor, toInternal[from] / toInternal[to]));
            }
        }
    }

    CHECK(NearFloat(Local::UnitsConversionFactor(LightUnits::Candelas, LightUnits::Unitless),
                    625.0f));
    CHECK(NearFloat(Local::UnitsConversionFactor(LightUnits::Unitless, LightUnits::Candelas),
                    0.0016f));
    CHECK(NearFloat(Local::UnitsConversionFactor(LightUnits::Lumens, LightUnits::Candelas),
                    1.0f / (4.0f * pi)));
    CHECK(NearFloat(Local::UnitsConversionFactor(LightUnits::Candelas, LightUnits::Lumens),
                    4.0f * pi));
    CHECK(NearFloat(Local::UnitsConversionFactor(LightUnits::Nits, LightUnits::Candelas), 1.0e-4f));
    CHECK(NearFloat(Local::UnitsConversionFactor(LightUnits::Candelas, LightUnits::Nits), 1.0e4f));
    CHECK(NearFloat(Local::UnitsConversionFactor(LightUnits::EV, LightUnits::Candelas), 1.0f));
    CHECK(NearFloat(Local::UnitsConversionFactor(LightUnits::Unitless, LightUnits::Nits), 16.0f));
    // 원뿔: cos 0.5 → 2π(1 − 0.5) = π, cos 1은 1 − 1e-4로 자른다, -2는 -1로.
    CHECK(NearFloat(Local::UnitsConversionFactor(LightUnits::Lumens, LightUnits::Candelas, 0.5f),
                    1.0f / pi));
    CHECK(NearFloat(Local::UnitsConversionFactor(LightUnits::Lumens, LightUnits::Candelas, 1.0f),
                    1.0f / (2.0f * pi * (1.0f - (1.0f - 1.0e-4f)))));
    CHECK(NearFloat(Local::UnitsConversionFactor(LightUnits::Lumens, LightUnits::Candelas, -2.0f),
                    1.0f / (4.0f * pi)));

    Fixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    ObjectHandle actor;
    const ObjectHandle point = fixture.AddLight<PointLightComponent>(actor);
    const ObjectHandle spot = fixture.AddLight<SpotLightComponent>(actor);
    const ObjectHandle rect = fixture.AddLight<RectLightComponent>(actor);
    REQUIRE(!point.IsNull() && !spot.IsNull() && !rect.IsNull());
    auto pointChecked =
        world.With<PointLightComponent>(point, [&](PointLightComponent& light) noexcept {
            // Unitless: 5000 × 16.
            CHECK(NearFloat(light.ComputeLightBrightness(), 80000.0f));
            CHECK(light.SetIntensityUnits(LightUnits::Candelas) && light.SetIntensity(8.0f));
            CHECK(NearFloat(light.ComputeLightBrightness(), 80000.0f));
            // Nits: 캡슐 면적 4π·R·(R + L/2) = 4π·10·20 = 800π.
            CHECK(light.SetIntensityUnits(LightUnits::Nits) && light.SetIntensity(2.0f));
            CHECK(light.SetSourceRadius(10.0f) && light.SetSourceLength(20.0f));
            CHECK(NearFloat(light.ComputeLightBrightness(), 1600.0f * pi));
            // Lumens: 4π lm → 1e4.
            CHECK(light.SetIntensityUnits(LightUnits::Lumens) && light.SetIntensity(4.0f * pi));
            CHECK(NearFloat(light.ComputeLightBrightness(), 1.0e4f));
            // EV 3 → 2^3 × 1e4, EV −1 → 0.5 × 1e4.
            CHECK(light.SetIntensityUnits(LightUnits::EV) && light.SetIntensity(3.0f));
            CHECK(NearFloat(light.ComputeLightBrightness(), 8.0e4f));
            CHECK(light.SetIntensity(-1.0f));
            CHECK(NearFloat(light.ComputeLightBrightness(), 5.0e3f));
            // 1/r²가 꺼지면 intensity 그대로다.
            CHECK(light.SetUseInverseSquaredFalloff(false) && light.SetIntensity(5000.0f));
            CHECK(light.ComputeLightBrightness() == 5000.0f);
        });
    CHECK(pointChecked);
    auto spotChecked =
        world.With<SpotLightComponent>(spot, [&](SpotLightComponent& light) noexcept {
            // 바깥 반각 60° → cos 0.5 → Lumens 배율 1e4/π.
            CHECK(light.SetOuterConeAngle(60.0f));
            CHECK(NearFloat(light.CosHalfConeAngle(), 0.5f));
            CHECK(light.SetIntensityUnits(LightUnits::Lumens) && light.SetIntensity(pi));
            CHECK(NearFloat(light.ComputeLightBrightness(), 1.0e4f));
            CHECK(light.SetIntensityUnits(LightUnits::Candelas) && light.SetIntensity(1.0f));
            CHECK(NearFloat(light.ComputeLightBrightness(), 1.0e4f));
            // Nits는 Point와 같은 캡슐 면적, EV·Unitless도 Point와 같다.
            CHECK(light.SetIntensityUnits(LightUnits::Nits) && light.SetIntensity(1.0f));
            CHECK(light.SetSourceRadius(5.0f) && light.SetSourceLength(0.0f));
            CHECK(NearFloat(light.ComputeLightBrightness(), 100.0f * pi));
            CHECK(light.SetIntensityUnits(LightUnits::EV) && light.SetIntensity(2.0f));
            CHECK(NearFloat(light.ComputeLightBrightness(), 4.0e4f));
            CHECK(light.SetIntensityUnits(LightUnits::Unitless) && light.SetIntensity(10.0f));
            CHECK(NearFloat(light.ComputeLightBrightness(), 160.0f));
            // 1/r²가 꺼지면 intensity 그대로다.
            CHECK(light.SetUseInverseSquaredFalloff(false));
            CHECK(light.ComputeLightBrightness() == 10.0f);
        });
    CHECK(spotChecked);
    auto rectChecked =
        world.With<RectLightComponent>(rect, [&](RectLightComponent& light) noexcept {
            CHECK(NearFloat(light.ComputeLightBrightness(), 80000.0f));
            CHECK(light.SetIntensityUnits(LightUnits::Candelas) && light.SetIntensity(1.0f));
            CHECK(NearFloat(light.ComputeLightBrightness(), 1.0e4f));
            CHECK(light.SetIntensityUnits(LightUnits::Nits));
            CHECK(NearFloat(light.ComputeLightBrightness(), 64.0f * 64.0f));
            CHECK(light.SetIntensityUnits(LightUnits::Lumens) && light.SetIntensity(pi));
            CHECK(NearFloat(light.ComputeLightBrightness(), 1.0e4f));
            CHECK(light.SetIntensityUnits(LightUnits::EV) && light.SetIntensity(0.0f));
            CHECK(NearFloat(light.ComputeLightBrightness(), 1.0e4f));
        });
    CHECK(rectChecked);
    REQUIRE(world.Shutdown());
}

// 24.2.4절: 원뿔 각 clamp, cubemap 해상도 정리, 경계 구·AffectsBounds, Direction, 정적 판정.
TEST(Light_GeometryAndSanitizing) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    ObjectHandle actor;
    const ObjectHandle spot = fixture.AddLight<SpotLightComponent>(actor);
    const ObjectHandle sky = fixture.AddLight<SkyLightComponent>(actor);
    const ObjectHandle point = fixture.AddLight<PointLightComponent>(actor);
    REQUIRE(!spot.IsNull() && !sky.IsNull() && !point.IsNull());
    const float maxRadians = math::DegreesToRadians(SpotLightComponent::MaxConeAngle);
    const float gap = SpotLightComponent::MinConeAngleGapRadians;

    auto spotChecked =
        world.With<SpotLightComponent>(spot, [&](SpotLightComponent& light) noexcept {
            SpotLightComponent::ConeAngles angles = light.ClampedConeAngles();
            CHECK(angles.innerRadians == 0.0f);
            CHECK(NearFloat(angles.outerRadians, math::DegreesToRadians(44.0f)));
            CHECK(NearFloat(light.HalfConeAngle(), math::DegreesToRadians(44.0f)));
            // inner > outer: outer는 inner + 0.001 rad.
            CHECK(light.SetInnerConeAngle(50.0f) && light.SetOuterConeAngle(30.0f));
            angles = light.ClampedConeAngles();
            CHECK(NearFloat(angles.innerRadians, math::DegreesToRadians(50.0f)));
            CHECK(NearFloat(angles.outerRadians, math::DegreesToRadians(50.0f) + gap));
            // 음수 inner는 0, 너무 큰 각은 88.9°(+0.001)로 자른다.
            CHECK(light.SetInnerConeAngle(-5.0f) && light.SetOuterConeAngle(120.0f));
            angles = light.ClampedConeAngles();
            CHECK(angles.innerRadians == 0.0f);
            CHECK(NearFloat(angles.outerRadians, maxRadians + gap));
            CHECK(light.SetInnerConeAngle(100.0f));
            angles = light.ClampedConeAngles();
            CHECK(NearFloat(angles.innerRadians, maxRadians));
            CHECK(NearFloat(angles.outerRadians, maxRadians + gap));
            // setter는 저장만 한다.
            CHECK(light.InnerConeAngle() == 100.0f && light.OuterConeAngle() == 120.0f);
        });
    CHECK(spotChecked);

    auto skyChecked = world.With<SkyLightComponent>(sky, [](SkyLightComponent& light) noexcept {
        struct Case {
            int32 input;
            int32 expected;
        };

        const Case cases[] = {{1, 8},         {8, 8},         {9, 16},
                              {100, 128},     {128, 128},     {16384, 16384},
                              {16385, 16384}, {20000, 16384}, {INT32_MAX, 16384}};
        for (const Case& value : cases) {
            CHECK(light.SetCubemapResolution(value.input));
            CHECK(light.CubemapResolution() == value.expected);
        }

        CHECK(ErrorOf(light.SetCubemapResolution(0)) == ErrorCode::InvalidArgument);
        CHECK(ErrorOf(light.SetCubemapResolution(-8)) == ErrorCode::InvalidArgument);
        CHECK(light.CubemapResolution() == 16384);
    });
    CHECK(skyChecked);

    auto pointChecked =
        world.With<PointLightComponent>(point, [](PointLightComponent& light) noexcept {
            CHECK(light.SetRelativeLocation(Vec3d(100.0, 0.0, 0.0)));
            const Sphere<double> sphere = light.BoundingSphere();
            CHECK(NearVector(sphere.center, 100.0, 0.0, 0.0));
            CHECK(sphere.radius == 1000.0);
            ComponentBounds bounds;
            bounds.origin = Vec3d(1200.0, 0.0, 0.0);
            bounds.sphereRadius = 150.0;
            CHECK(light.AffectsBounds(bounds));
            bounds.sphereRadius = 50.0;
            CHECK(!light.AffectsBounds(bounds));
            // 방향은 월드 +X 축이다(yaw 90 → +Y, 비균일 scale이어도 단위 벡터).
            CHECK(NearVector(light.Direction(), 1.0, 0.0, 0.0));
            CHECK(light.SetRelativeRotation(Rotator<double>(0.0, 90.0, 0.0).ToQuaternion()));
            CHECK(light.SetRelativeScale(Vec3d(2.0, 3.0, 1.0)));
            CHECK(NearVector(light.Direction(), 0.0, 1.0, 0.0));
            // 정적 판정(UE HasStaticLighting·HasStaticShadowing).
            CHECK(!light.HasStaticLighting() && !light.HasStaticShadowing());
            light.SetMobility(ComponentMobility::Stationary);
            CHECK(!light.HasStaticLighting() && light.HasStaticShadowing());
            light.SetMobility(ComponentMobility::Static);
            CHECK(light.HasStaticLighting() && light.HasStaticShadowing());
        });
    CHECK(pointChecked);
    REQUIRE(world.Shutdown());
}

// 24.2.3절 색: sRGB byte 그대로, 선형 색은 sRGB 인코딩, srgb 거짓은 양자화만.
TEST(Light_ColorEncoding) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    ObjectHandle actor;
    const ObjectHandle point = fixture.AddLight<PointLightComponent>(actor);
    REQUIRE(!point.IsNull());
    auto checked = world.With<PointLightComponent>(point, [](PointLightComponent& light) noexcept {
        CHECK(light.SetLightColor(Color(10, 20, 30, 40)));
        CHECK(light.LightColor() == Color(10, 20, 30, 40));
        CHECK(light.LinearLightColor() == LinearColor::FromSRGB(Color(10, 20, 30, 40)));
        const LinearColor linear(1.0f, 0.5f, 0.25f, 1.0f);
        CHECK(light.SetLightColor(linear));
        CHECK(light.LightColor() == linear.ToSRGB());
        // sRGB 0.5 선형은 byte 188 근처다(인코딩) — 양자화만 하면 128(0.5 × 255 = 127.5 → 올림).
        CHECK(light.LightColor().g > 180);
        CHECK(light.SetLightColor(linear, false));
        CHECK(light.LightColor() == Color(255, 128, 64, 255));
    });
    CHECK(checked);
    REQUIRE(world.Shutdown());
}

// 24.2.5절: 광원 Actor가 Stationary root를 만들고 Spot·Directional 회전, Spawn Transform 곱,
// 켜고 끄기, 이미 있는 root 재사용.
TEST(Light_ActorsCreateStationaryRoots) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());

    auto point = world.SpawnActor<PointLight>();
    auto rect = world.SpawnActor<RectLight>();
    auto sky = world.SpawnActor<SkyLight>();
    auto spot = world.SpawnActor<SpotLight>();
    // Spawn yaw 90°: root 상대 pitch −46° × Spawn → 방향 (0, cos 46°, −sin 46°).
    const Transform yawed(Rotator<double>(0.0, 90.0, 0.0).ToQuaternion(), Vec3d(0.0, 0.0, 500.0),
                          Vec3d::One());
    auto sun = world.SpawnActor<DirectionalLight>(SpawnParams::At(yawed));
    REQUIRE(point && rect && sky && spot && sun);

    const auto mobilityOf = [&](ObjectHandle actor) noexcept {
        auto mobility = world.With<SceneComponent>(
            fixture.RootOf(actor), [](SceneComponent& root) noexcept { return root.Mobility(); });
        return mobility ? mobility.Value() : ComponentMobility::Movable;
    };

    CHECK(world.With<PointLightComponent>(fixture.RootOf(point.Value()),
                                          [](PointLightComponent&) noexcept {}));
    CHECK(world.With<RectLightComponent>(fixture.RootOf(rect.Value()),
                                         [](RectLightComponent&) noexcept {}));
    CHECK(world.With<SkyLightComponent>(fixture.RootOf(sky.Value()),
                                        [](SkyLightComponent&) noexcept {}));
    for (ObjectHandle actor :
         {point.Value(), rect.Value(), sky.Value(), spot.Value(), sun.Value()}) {
        CHECK(mobilityOf(actor) == ComponentMobility::Stationary);
        ObjectHandle components[2];
        auto count = world.GetComponents(actor, Span<ObjectHandle>(components, 2));
        CHECK(count && count.Value() == 1);
        auto light = world.With<Light>(actor, [](Light& value) noexcept {
            return value.LightComponentHandle() == value.RootComponent();
        });
        CHECK(light && light.Value());
    }

    auto spotChecked = world.With<SpotLightComponent>(
        fixture.RootOf(spot.Value()), [](SpotLightComponent& light) noexcept {
            CHECK(NearVector(light.Direction(), 0.0, 0.0, -1.0));
        });
    CHECK(spotChecked);
    const double pitch = math::DegreesToRadians(46.0);
    auto sunChecked = world.With<DirectionalLightComponent>(
        fixture.RootOf(sun.Value()), [&](DirectionalLightComponent& light) noexcept {
            CHECK(NearVector(light.Direction(), 0.0, cos(pitch), -sin(pitch)));
            CHECK(NearVector(light.WorldLocation(), 0.0, 0.0, 500.0));
        });
    CHECK(sunChecked);

    // 켜고 끄기는 root visible이다.
    auto toggled = world.With<Light>(point.Value(), [&](Light& light) noexcept {
        CHECK(light.IsLightEnabled());
        CHECK(light.SetLightEnabled(false));
        CHECK(!light.IsLightEnabled());
        CHECK(light.SetLightEnabled(true));
        CHECK(light.IsLightEnabled());
    });
    CHECK(toggled);

    // root가 이미 그 타입(파생 포함)이면 재사용한다 — 레벨 로드처럼 Spawn 전에 붙여 둔다.
    auto deferred = world.SpawnActorDeferred<PointLight>();
    REQUIRE(deferred);
    auto preset = world.AddComponent<SpotLightComponent>(deferred.Value());
    REQUIRE(preset);
    REQUIRE(world.With<Actor>(deferred.Value(), [&](Actor& actor) noexcept {
        return actor.SetRootComponent(preset.Value());
    }));
    REQUIRE(world.FinishSpawning(deferred.Value()));
    CHECK(fixture.RootOf(deferred.Value()) == preset.Value());
    ObjectHandle components[2];
    auto count = world.GetComponents(deferred.Value(), Span<ObjectHandle>(components, 2));
    CHECK(count && count.Value() == 1);
    // 재사용한 root는 다시 설정하지 않는다(Mobility 그대로).
    CHECK(mobilityOf(deferred.Value()) == ComponentMobility::Movable);

    // 다른 타입 root(SceneComponent)면 Spawn이 InvalidArgument로 실패한다(옛 root를 떼어 두지
    // 않는다).
    auto mismatched = world.SpawnActorDeferred<RectLight>();
    REQUIRE(mismatched);
    auto plain = world.AddComponent<SceneComponent>(mismatched.Value());
    REQUIRE(plain);
    REQUIRE(world.With<Actor>(mismatched.Value(), [&](Actor& actor) noexcept {
        return actor.SetRootComponent(plain.Value());
    }));
    CHECK(ErrorOf(world.FinishSpawning(mismatched.Value())) == ErrorCode::InvalidArgument);
    REQUIRE(world.Shutdown());
}

// 24.2.7절 6: 리플렉션 형태(Abstract·base·필드 수)와 Level 그래프 저장 왕복.
TEST(Light_ReflectionAndSaveRoundTrip) {
    Fixture fixture(WorldKind::Editor);
    REQUIRE(fixture.registered);
    TypeRegistry& types = fixture.types;
    struct Expected {
        TypeId id;
        TypeId base;
        usize fields;
        bool abstract;
    };

    // 필드 수는 상속분을 포함한다(SceneComponent 12, Actor 5 — 태그 ER 31.4절 포함). 광원 클래스가
    // 더하는 수는 LightComponentBase 11·LightComponent 14·LocalLight 3·Point 5·Spot 2·Rect
    // 4·Directional 15·Sky 8, 광원 Actor 0이다.
    const Expected expected[] = {
        {ObjectTypeIdOf<LightComponentBase>(), ObjectTypeIdOf<SceneComponent>(), 23, true},
        {ObjectTypeIdOf<LightComponent>(), ObjectTypeIdOf<LightComponentBase>(), 37, true},
        {ObjectTypeIdOf<LocalLightComponent>(), ObjectTypeIdOf<LightComponent>(), 40, true},
        {ObjectTypeIdOf<PointLightComponent>(), ObjectTypeIdOf<LocalLightComponent>(), 45, false},
        {ObjectTypeIdOf<SpotLightComponent>(), ObjectTypeIdOf<PointLightComponent>(), 47, false},
        {ObjectTypeIdOf<RectLightComponent>(), ObjectTypeIdOf<LocalLightComponent>(), 44, false},
        {ObjectTypeIdOf<DirectionalLightComponent>(), ObjectTypeIdOf<LightComponent>(), 52, false},
        {ObjectTypeIdOf<SkyLightComponent>(), ObjectTypeIdOf<LightComponentBase>(), 31, false},
        {ObjectTypeIdOf<Light>(), ObjectTypeIdOf<Actor>(), 5, true},
        {ObjectTypeIdOf<PointLight>(), ObjectTypeIdOf<Light>(), 5, false},
        {ObjectTypeIdOf<SkyLight>(), ObjectTypeIdOf<Light>(), 5, false},
    };
    for (const Expected& entry : expected) {
        auto lease = types.Acquire(entry.id);
        REQUIRE(lease);
        const TypeSchema& schema = lease.Value().Schema();
        CHECK(HasFlag(schema.flags, TypeFlags::WorldOnly));
        CHECK(HasFlag(schema.flags, TypeFlags::Abstract) == entry.abstract);
        CHECK(schema.baseType == entry.base);
        CHECK(schema.fields.Size() == entry.fields);
    }

    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    auto level = world.CreateLevel("Lit");
    REQUIRE(level);
    SpawnParams params;
    params.level = level.Value();
    auto lamp = world.SpawnActor<SpotLight>(params);
    REQUIRE(lamp);
    LightingChannels channels;
    channels.channel0 = false;
    channels.channel1 = true;
    auto edited = world.With<SpotLightComponent>(
        fixture.RootOf(lamp.Value()), [&](SpotLightComponent& light) noexcept {
            if (auto set = light.SetIntensityUnits(LightUnits::Candelas); !set) {
                return set;
            }

            if (auto set = light.SetIntensity(1234.5f); !set) {
                return set;
            }

            if (auto set = light.SetLightColor(Color(1, 2, 3, 4)); !set) {
                return set;
            }

            if (auto set = light.SetLightingChannels(channels); !set) {
                return set;
            }

            if (auto set = light.SetAttenuationRadius(777.0f); !set) {
                return set;
            }

            return light.SetOuterConeAngle(33.0f);
        });
    REQUIRE(edited);
    auto sun = world.SpawnActor<DirectionalLight>(params);
    auto sky = world.SpawnActor<SkyLight>(params);
    REQUIRE(sun && sky);
    const LinearColor disk(0.5f, 0.25f, 2.0f, 1.0f);
    auto sunEdited = world.With<DirectionalLightComponent>(
        fixture.RootOf(sun.Value()), [&](DirectionalLightComponent& light) noexcept {
            if (auto set = light.SetDynamicShadowCascades(7); !set) {
                return set;
            }

            if (auto set = light.SetAtmosphereSunDiskColorScale(disk); !set) {
                return set;
            }

            return light.SetForwardShadingPriority(3);
        });
    REQUIRE(sunEdited);
    const LinearColor ground(0.1f, 0.2f, 0.3f, 1.0f);
    auto skyEdited = world.With<SkyLightComponent>(
        fixture.RootOf(sky.Value()), [&](SkyLightComponent& light) noexcept {
            if (auto set = light.SetSourceType(SkyLightSourceType::SpecifiedCubemap); !set) {
                return set;
            }

            if (auto set = light.SetCubemapResolution(512); !set) {
                return set;
            }

            return light.SetLowerHemisphereColor(ground);
        });
    REQUIRE(skyEdited);

    Array<uint8> saved(fixture.allocator);
    {
        auto bytes =
            SaveObjectGraph(fixture.allocator, world.Objects(), world.Access(), level.Value());
        REQUIRE(bytes);
        saved = Move(bytes.Value());
    }

    {
        ObjectRegistry loaded(fixture.allocator, fixture.types, {0x4c4954ull, 2});
        const auto access = loaded.OwnerAccess();
        auto root = LoadObjectGraph(fixture.allocator, loaded, fixture.types, access, saved.View());
        REQUIRE(root);
        // Level actors 순서는 보장하지 않는다(7.2절) — 타입으로 고른다.
        ObjectHandle loadedLamp;
        ObjectHandle loadedSun;
        ObjectHandle loadedSky;
        {
            auto borrowed = loaded.Resolve<Level>(access, root.Value().Handle());
            REQUIRE(borrowed);
            Level* pointer = Get(borrowed.Value());
            REQUIRE(pointer != nullptr && pointer->Actors().Size() == 3);
            for (usize index = 0; index < 3; ++index) {
                const ObjectHandle actor = pointer->Actors()[index];
                if (loaded.Resolve<SpotLight>(access, actor)) {
                    loadedLamp = actor;
                } else if (loaded.Resolve<DirectionalLight>(access, actor)) {
                    loadedSun = actor;
                } else if (loaded.Resolve<SkyLight>(access, actor)) {
                    loadedSky = actor;
                }
            }
        }

        REQUIRE(!loadedLamp.IsNull() && !loadedSun.IsNull() && !loadedSky.IsNull());
        const auto rootOf = [&](ObjectHandle actor) noexcept {
            auto borrowed = loaded.Resolve<Light>(access, actor);
            Light* pointer = borrowed ? Get(borrowed.Value()) : nullptr;
            return pointer != nullptr ? pointer->RootComponent() : ObjectHandle{};
        };

        {
            auto borrowed = loaded.Resolve<DirectionalLightComponent>(access, rootOf(loadedSun));
            REQUIRE(borrowed);
            DirectionalLightComponent* light = Get(borrowed.Value());
            REQUIRE(light != nullptr);
            CHECK(light->DynamicShadowCascades() == 7);
            CHECK(light->ForwardShadingPriority() == 3);
            CHECK(light->AtmosphereSunDiskColorScale() == disk);
            CHECK(light->Intensity() == 10.0f);
        }

        {
            auto borrowed = loaded.Resolve<SkyLightComponent>(access, rootOf(loadedSky));
            REQUIRE(borrowed);
            SkyLightComponent* light = Get(borrowed.Value());
            REQUIRE(light != nullptr);
            CHECK(light->SourceType() == SkyLightSourceType::SpecifiedCubemap);
            CHECK(light->CubemapResolution() == 512);
            CHECK(light->LowerHemisphereColor() == ground);
        }

        ObjectHandle loadedLight;
        {
            auto borrowed = loaded.Resolve<SpotLight>(access, loadedLamp);
            REQUIRE(borrowed);
            SpotLight* pointer = Get(borrowed.Value());
            REQUIRE(pointer != nullptr && pointer->Components().Size() == 1);
            loadedLight = pointer->RootComponent();
        }

        auto borrowed = loaded.Resolve<SpotLightComponent>(access, loadedLight);
        REQUIRE(borrowed);
        SpotLightComponent* light = Get(borrowed.Value());
        REQUIRE(light != nullptr);
        CHECK(light->IntensityUnits() == LightUnits::Candelas);
        CHECK(light->Intensity() == 1234.5f);
        CHECK(light->LightColor() == Color(1, 2, 3, 4));
        CHECK(light->LightingChannels() == channels);
        CHECK(light->AttenuationRadius() == 777.0f);
        CHECK(light->OuterConeAngle() == 33.0f);
        CHECK(light->Mobility() == ComponentMobility::Stationary);
    }

    REQUIRE(world.Shutdown());
}

} // namespace world_light_component_tests_file
