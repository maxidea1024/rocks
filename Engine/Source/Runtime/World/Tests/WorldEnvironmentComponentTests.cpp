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
#include <World/Environment/BoxReflectionCapture.generated.h>
#include <World/Environment/BoxReflectionCapture.h>
#include <World/Environment/BoxReflectionCaptureComponent.generated.h>
#include <World/Environment/BoxReflectionCaptureComponent.h>
#include <World/Environment/DecalActor.generated.h>
#include <World/Environment/DecalActor.h>
#include <World/Environment/DecalComponent.generated.h>
#include <World/Environment/DecalComponent.h>
#include <World/Environment/HeightFog.generated.h>
#include <World/Environment/HeightFog.h>
#include <World/Environment/HeightFogComponent.generated.h>
#include <World/Environment/HeightFogComponent.h>
#include <World/Environment/HeightFogSettings.generated.h>
#include <World/Environment/HeightFogSettings.h>
#include <World/Environment/PlaneReflectionCapture.generated.h>
#include <World/Environment/PlaneReflectionCapture.h>
#include <World/Environment/PlaneReflectionCaptureComponent.generated.h>
#include <World/Environment/PlaneReflectionCaptureComponent.h>
#include <World/Environment/ReflectionCapture.generated.h>
#include <World/Environment/ReflectionCapture.h>
#include <World/Environment/ReflectionCaptureComponent.generated.h>
#include <World/Environment/ReflectionCaptureComponent.h>
#include <World/Environment/ReflectionSourceType.generated.h>
#include <World/Environment/SkyAtmosphere.generated.h>
#include <World/Environment/SkyAtmosphere.h>
#include <World/Environment/SkyAtmosphereComponent.generated.h>
#include <World/Environment/SkyAtmosphereComponent.h>
#include <World/Environment/SkyAtmosphereSettings.generated.h>
#include <World/Environment/SkyAtmosphereSettings.h>
#include <World/Environment/SphereReflectionCapture.generated.h>
#include <World/Environment/SphereReflectionCapture.h>
#include <World/Environment/SphereReflectionCaptureComponent.generated.h>
#include <World/Environment/SphereReflectionCaptureComponent.h>
#include <World/Level.generated.h>
#include <World/PostProcess/PostProcessBlending.h>
#include <World/PostProcess/PostProcessComponent.generated.h>
#include <World/PostProcess/PostProcessComponent.h>
#include <World/PostProcess/PostProcessSettings.generated.h>
#include <World/PostProcess/PostProcessSettings.h>
#include <World/PostProcess/PostProcessTypes.generated.h>
#include <World/SceneComponent.generated.h>
#include <World/World.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// 컴포넌트 데이터 먼저 D5 환경 시험이다(Docs/Design/EngineRuntime 27.8절): UE 기본값, 설정 범위
// 거부, Mobility, 행성 중심·안개 높이, decal bounds·수명, 후처리 덮어쓰기, 반사 캡처 반경, Actor
// root·켜고 끄기, 리플렉션·저장 왕복.
using namespace rocks;
namespace world_environment_component_tests_file {

namespace {

constexpr double EnvironmentTolerance = 1.0e-6;
constexpr float EnvironmentRelativeTolerance = 1.0e-5f;
// decal 수명 시험의 한 Tick(초 — 2진 정확값)이다.
constexpr double DecalTickSeconds = 0.125;

bool NearEnvironmentFloat(float value, float expected) noexcept {
    const float scale = fabsf(expected) > 1.0f ? fabsf(expected) : 1.0f;
    const bool same = fabsf(value - expected) <= EnvironmentRelativeTolerance * scale;
    if (!same) {
        printf("  value %.9g, expected %.9g\n", static_cast<double>(value),
               static_cast<double>(expected));
    }

    return same;
}

bool NearEnvironmentDouble(double value, double expected) noexcept {
    const bool same = fabs(value - expected) <= EnvironmentTolerance;
    if (!same) {
        printf("  value %.12g, expected %.12g\n", value, expected);
    }

    return same;
}

bool NearEnvironmentVector(const Vec3d& value, double x, double y, double z) noexcept {
    const bool same = fabs(value.x - x) <= EnvironmentTolerance &&
                      fabs(value.y - y) <= EnvironmentTolerance &&
                      fabs(value.z - z) <= EnvironmentTolerance;
    if (!same) {
        printf("  vector (%.9g, %.9g, %.9g), expected (%.9g, %.9g, %.9g)\n", value.x, value.y,
               value.z, x, y, z);
    }

    return same;
}

ErrorCode EnvironmentErrorOf(const Result<void>& result) noexcept {
    return result ? ErrorCode::None : result.Error();
}

template <class T>
T* EnvironmentGet(Borrow<T>& borrow) noexcept {
    auto pointer = borrow.Get();
    return pointer ? pointer.Value() : nullptr;
}

// StaticComponentDataChanged 진단 수를 센다(Manual 모드 LogSystem에 붙여 Pump로 받는다).
class EnvironmentDiagnosticCounter final : public RuntimeLogSink {
public:
    Result<void> Write(const LogEvent& event) noexcept override {
        if (event.level == LogLevel::Warning &&
            Contains(event.message, "(StaticComponentDataChanged)")) {
            ++dataChanged;
        }

        return Result<void>::Success();
    }

    uint32 dataChanged = 0;

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

class EnvironmentEmptyActor final : public Actor {
    ROCKS_NATIVE_TYPE(EnvironmentEmptyActor, Actor, 0x3e8b51c27a904d6full, 0x9c2d74f1b05e8a13ull)

public:
    explicit EnvironmentEmptyActor(Allocator& allocator) noexcept : Actor(allocator) {
    }
};

struct EnvironmentFixture {
    explicit EnvironmentFixture(WorldKind kind = WorldKind::Game) noexcept
        : types(allocator), modules(types, allocator) {
        registered = RegisterWorldTypes(modules).IsOk() && Register<EnvironmentEmptyActor>();
        WorldDesc desc;
        desc.name = "Environment";
        desc.context = {0x454e5649524full, 1};
        desc.timeStep = {1.0 / 60.0, 0.25, 8};
        desc.kind = kind;
        auto created = UniquePtr<World>::TryMake(allocator, allocator, types, desc);
        if (created) {
            world = Move(created.Value());
        }
    }

    ~EnvironmentFixture() {
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

    // 빈 Actor(없으면 만든다)에 T를 붙인다. root가 없으면 T를 root로 둔다.
    template <class T>
    ObjectHandle Add(ObjectHandle& actor, SpawnParams params = {}) noexcept {
        if (actor.IsNull()) {
            auto spawned = world->SpawnActor<EnvironmentEmptyActor>(params);
            if (!spawned) {
                return {};
            }

            actor = spawned.Value();
        }

        auto added = world->AddComponent<T>(actor);
        if (!added) {
            return {};
        }

        if (RootOf(actor).IsNull()) {
            (void)world->With<Actor>(actor, [&](Actor& value) noexcept {
                return value.SetRootComponent(added.Value());
            });
        }

        return added.Value();
    }

    ObjectHandle RootOf(ObjectHandle actor) noexcept {
        auto root =
            world->With<Actor>(actor, [](Actor& value) noexcept { return value.RootComponent(); });
        return root ? root.Value() : ObjectHandle{};
    }

    // 파괴 요청됐거나 이미 정리됐다.
    bool ComponentGone(ObjectHandle component) noexcept {
        auto state = world->GetComponentState(component);
        return !state || state.Value() == ComponentState::PendingDestroy;
    }

    bool ActorGone(ObjectHandle actor) noexcept {
        auto state = world->GetActorState(actor);
        return !state || state.Value() == ActorState::PendingDestroy;
    }

    bool TickTimes(int count) noexcept {
        for (int index = 0; index < count; ++index) {
            if (!world->Tick(DecalTickSeconds)) {
                return false;
            }
        }

        return true;
    }

    static constexpr usize MaxTokens = 2;
    SystemAllocator allocator;
    TypeRegistry types;
    ModuleRegistration modules;
    TypeRegistrationToken tokens[MaxTokens];
    usize tokenCount = 0;
    bool registered = false;
    UniquePtr<World> world;
};

} // namespace

// 27.2–27.6절: UE 생성자 기본값. 추상 타입은 만들 수 없다.
TEST(Environment_DefaultsMatchUe) {
    EnvironmentFixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());

    // 대기(UE SkyAtmosphereComponent.cpp:91–139).
    const SkyAtmosphereSettings sky;
    CHECK(sky.transformMode == SkyAtmosphereTransformMode::PlanetTopAtAbsoluteWorldOrigin);
    CHECK(sky.bottomRadius == 6360.0f && NearEnvironmentFloat(sky.atmosphereHeight, 60.0f));
    CHECK(sky.groundAlbedo == Color(170, 170, 170));
    CHECK(sky.multiScatteringFactor == 1.0f && sky.traceSampleCountScale == 1.0f);
    CHECK(sky.rayleighScatteringScale == 0.0331f);
    CHECK(NearEnvironmentFloat(sky.rayleighScattering.r, 0.005802f / 0.0331f));
    CHECK(NearEnvironmentFloat(sky.rayleighScattering.g, 0.013558f / 0.0331f));
    CHECK(NearEnvironmentFloat(sky.rayleighScattering.b, 1.0f));
    CHECK(NearEnvironmentFloat(sky.rayleighScattering.a, 1.0f / 0.0331f));
    CHECK(sky.rayleighExponentialDistribution == 8.0f);
    CHECK(sky.mieScatteringScale == 0.003996f && sky.mieScattering == LinearColor::White());
    CHECK(sky.mieAbsorptionScale == 0.000444f && sky.mieAbsorption == LinearColor::White());
    CHECK(sky.mieAnisotropy == 0.8f && sky.mieExponentialDistribution == 1.2f);
    CHECK(sky.otherAbsorptionScale == 0.001881f);
    CHECK(NearEnvironmentFloat(sky.otherAbsorption.r, 0.00065f / 0.001881f));
    CHECK(NearEnvironmentFloat(sky.otherAbsorption.g, 1.0f));
    CHECK(NearEnvironmentFloat(sky.otherAbsorption.b, 0.000085f / 0.001881f));
    CHECK(sky.otherTentDistribution.tipAltitude == 25.0f &&
          sky.otherTentDistribution.tipValue == 1.0f && sky.otherTentDistribution.width == 15.0f);
    CHECK(sky.skyLuminanceFactor == LinearColor::White() &&
          sky.skyAndAerialPerspectiveLuminanceFactor == LinearColor::White());
    CHECK(sky.aerialPerspectiveViewDistanceScale == 1.0f && sky.heightFogContribution == 1.0f);
    CHECK(sky.transmittanceMinLightElevationAngle == -90.0f &&
          sky.aerialPerspectiveStartDepth == 0.1f);
    CHECK(!sky.holdout && sky.renderInMainPass);
    CHECK(IsValidSkyAtmosphereSettings(sky));

    // 높이 안개(UE ExponentialHeightFogComponent.cpp:72–119).
    const HeightFogSettings fog;
    CHECK(fog.fogDensity == 0.02f && fog.fogHeightFalloff == 0.2f);
    CHECK(fog.secondFogData.fogDensity == 0.0f && fog.secondFogData.fogHeightFalloff == 0.2f &&
          fog.secondFogData.fogHeightOffset == 0.0f);
    CHECK(fog.fogInscatteringLuminance == LinearColor::Black());
    CHECK(fog.skyAtmosphereAmbientContributionColorScale == LinearColor::White());
    CHECK(fog.skyLightCaptureAffectsHeightFogStrength == 0.0f &&
          fog.skyLightCaptureAffectsHeightFogRoughness == 0.15f);
    CHECK(fog.inscatteringColorCubemapAngle == 0.0f &&
          fog.inscatteringTextureTint == LinearColor::White());
    CHECK(fog.fullyDirectionalInscatteringColorDistance == 100000.0f &&
          fog.nonDirectionalInscatteringColorDistance == 1000.0f);
    CHECK(fog.directionalInscatteringExponent == 4.0f &&
          fog.directionalInscatteringStartDistance == 10000.0f &&
          fog.directionalInscatteringLuminance == LinearColor::Black());
    CHECK(fog.fogMaxOpacity == 1.0f && fog.startDistance == 0.0f && fog.endDistance == 0.0f &&
          fog.fogCutoffDistance == 0.0f);
    CHECK(!fog.enableVolumetricFog && fog.volumetricFogScatteringDistribution == 0.2f);
    CHECK(fog.volumetricFogAlbedo == Color::White() &&
          fog.volumetricFogEmissive == LinearColor::Black());
    CHECK(fog.volumetricFogExtinctionScale == 1.0f && fog.volumetricFogDistance == 6000.0f);
    CHECK(fog.volumetricFogStartDistance == 0.0f && fog.volumetricFogNearFadeInDistance == 0.0f);
    CHECK(fog.volumetricFogStaticLightingScatteringIntensity == 1.0f);
    CHECK(!fog.overrideLightColorsWithFogInscatteringColors && !fog.holdout);
    CHECK(fog.renderInMainPass && fog.visibleInReflectionCaptures &&
          fog.visibleInRealTimeSkyCaptures);
    CHECK(IsValidHeightFogSettings(fog));

    // 후처리(UE Scene.cpp:398) — override 플래그는 모두 거짓.
    const PostProcessSettings post;
    CHECK(post.temperatureType == TemperatureMethod::WhiteBalance && post.whiteTemp == 6500.0f &&
          post.whiteTint == 0.0f);
    CHECK(post.colorSaturation == Vec4d(1.0, 1.0, 1.0, 1.0) &&
          post.colorContrast == Vec4d(1.0, 1.0, 1.0, 1.0));
    CHECK(post.colorGamma == Vec4d(1.0, 1.0, 1.0, 1.0) &&
          post.colorGain == Vec4d(1.0, 1.0, 1.0, 1.0) &&
          post.colorOffset == Vec4d(0.0, 0.0, 0.0, 0.0));
    CHECK(post.tonemappingMethod == TonemappingMethod::Filmic && post.filmSlope == 0.88f &&
          post.filmToe == 0.55f && post.filmShoulder == 0.26f);
    CHECK(post.filmBlackClip == 0.0f && post.filmWhiteClip == 0.04f);
    CHECK(post.sceneFringeIntensity == 0.0f && post.bloomMethod == BloomMethod::Standard);
    CHECK(post.bloomIntensity == 0.675f && post.bloomThreshold == -1.0f &&
          post.bloomSizeScale == 4.0f);
    CHECK(post.autoExposureMethod == AutoExposureMethod::Histogram &&
          post.autoExposureBias == 1.0f);
    // UE5 기본 확장 범위(EV100 — Scene.cpp:500).
    CHECK(post.autoExposureMinBrightness == -10.0f && post.autoExposureMaxBrightness == 20.0f);
    CHECK(post.autoExposureSpeedUp == 3.0f && post.autoExposureSpeedDown == 1.0f);
    CHECK(post.autoExposureLowPercent == 10.0f && post.autoExposureHighPercent == 90.0f);
    CHECK(post.autoExposureApplyPhysicalCameraExposure && post.cameraShutterSpeed == 60.0f &&
          post.cameraIso == 100.0f);
    CHECK(post.lensFlareIntensity == 1.0f && post.vignetteIntensity == 0.4f &&
          post.filmGrainIntensity == 0.0f);
    CHECK(post.ambientOcclusionIntensity == 0.5f && post.ambientOcclusionRadius == 200.0f);
    CHECK(post.depthOfFieldFocalDistance == 0.0f && post.depthOfFieldFstop == 4.0f &&
          post.depthOfFieldSensorWidth == 24.576f);
    CHECK(post.motionBlurAmount == 0.5f && post.motionBlurMax == 5.0f &&
          post.motionBlurTargetFps == 30 && post.motionBlurPerObjectSize == 0.0f);
    CHECK(!post.overrideTemperatureType && !post.overrideBloomIntensity &&
          !post.overrideAutoExposureBias && !post.overrideMotionBlurTargetFps &&
          !post.overrideColorGain);
    CHECK(IsValidPostProcessSettings(post));

    ObjectHandle actor;
    const ObjectHandle skyComponent = fixture.Add<SkyAtmosphereComponent>(actor);
    const ObjectHandle fogComponent = fixture.Add<HeightFogComponent>(actor);
    const ObjectHandle decal = fixture.Add<DecalComponent>(actor);
    const ObjectHandle volume = fixture.Add<PostProcessComponent>(actor);
    const ObjectHandle sphere = fixture.Add<SphereReflectionCaptureComponent>(actor);
    const ObjectHandle box = fixture.Add<BoxReflectionCaptureComponent>(actor);
    const ObjectHandle plane = fixture.Add<PlaneReflectionCaptureComponent>(actor);
    REQUIRE(!skyComponent.IsNull() && !fogComponent.IsNull() && !decal.IsNull() &&
            !volume.IsNull() && !sphere.IsNull() && !box.IsNull() && !plane.IsNull());

    CHECK(world.With<SkyAtmosphereComponent>(skyComponent, [](SkyAtmosphereComponent& c) noexcept {
        CHECK(c.Settings() == SkyAtmosphereSettings{});
        CHECK(c.Mobility() == ComponentMobility::Movable);
    }));
    CHECK(world.With<HeightFogComponent>(fogComponent, [](HeightFogComponent& c) noexcept {
        CHECK(c.Settings() == HeightFogSettings{});
        CHECK(c.Mobility() == ComponentMobility::Movable);
    }));
    // decal(UE DecalComponent.cpp:233–241).
    CHECK(world.With<DecalComponent>(decal, [](DecalComponent& c) noexcept {
        CHECK(c.SortOrder() == 0 && c.FadeScreenSize() == 0.01f);
        CHECK(c.FadeStartDelay() == 0.0f && c.FadeDuration() == 0.0f);
        CHECK(c.FadeInStartDelay() == 0.0f && c.FadeInDuration() == 0.0f);
        CHECK(c.DestroysOwnerAfterFade());
        CHECK(NearEnvironmentVector(c.DecalSize(), 128.0, 256.0, 256.0));
        CHECK(c.DecalColor() == LinearColor::White());
        CHECK(c.RemainingLifeSpan() == 0.0);
        CHECK(c.Mobility() == ComponentMobility::Movable);
    }));
    // 후처리 볼륨(UE PostProcessComponent.cpp:16–24).
    CHECK(world.With<PostProcessComponent>(volume, [](PostProcessComponent& c) noexcept {
        CHECK(c.Settings() == PostProcessSettings{});
        CHECK(c.Priority() == 0.0f && c.BlendRadius() == 100.0f && c.BlendWeight() == 1.0f);
        CHECK(c.IsPostProcessEnabled() && c.IsUnbound());
    }));
    // 반사 캡처(UE ReflectionCaptureComponent.cpp:768–780·1321·1351·1385) — 생성자 Static.
    CHECK(world.With<SphereReflectionCaptureComponent>(
        sphere, [](SphereReflectionCaptureComponent& c) noexcept {
            CHECK(c.SourceType() == ReflectionSourceType::CapturedScene);
            CHECK(c.SourceCubemapAngle() == 0.0f && c.Brightness() == 1.0f);
            CHECK(NearEnvironmentVector(c.CaptureOffset(), 0.0, 0.0, 0.0));
            CHECK(c.RuntimeSkylightScale() == LinearColor(0.1f, 0.1f, 0.1f, 1.0f));
            CHECK(!c.UsesRuntimeCapture() && c.MaxViewDistance() == 0.0f && !c.HasFiniteFarPlane());
            CHECK(c.InfluenceRadius() == 3000.0f);
            CHECK(c.Mobility() == ComponentMobility::Static);
        }));
    CHECK(world.With<BoxReflectionCaptureComponent>(
        box, [](BoxReflectionCaptureComponent& c) noexcept {
            CHECK(c.BoxTransitionDistance() == 100.0f);
            CHECK(c.Mobility() == ComponentMobility::Static);
        }));
    CHECK(world.With<PlaneReflectionCaptureComponent>(
        plane, [](PlaneReflectionCaptureComponent& c) noexcept {
            CHECK(c.InfluenceRadiusScale() == 2.0f);
            CHECK(c.Mobility() == ComponentMobility::Static);
        }));

    // 추상 타입은 만들 수 없다.
    CHECK(!world.AddComponent(actor, ObjectTypeIdOf<ReflectionCaptureComponent>()));
    CHECK(!world.SpawnActor(ObjectTypeIdOf<ReflectionCapture>()));
    REQUIRE(world.Shutdown());
}

// 27.1절: SetSettings·필드 setter는 범위 밖·비유한 값을 InvalidArgument로 거부하고 값을 유지한다.
TEST(Environment_SettersRejectOutOfRange) {
    EnvironmentFixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    ObjectHandle actor;
    const ObjectHandle skyComponent = fixture.Add<SkyAtmosphereComponent>(actor);
    const ObjectHandle fogComponent = fixture.Add<HeightFogComponent>(actor);
    const ObjectHandle volume = fixture.Add<PostProcessComponent>(actor);
    const ObjectHandle decal = fixture.Add<DecalComponent>(actor);
    const ObjectHandle sphere = fixture.Add<SphereReflectionCaptureComponent>(actor);
    const ObjectHandle box = fixture.Add<BoxReflectionCaptureComponent>(actor);
    const ObjectHandle plane = fixture.Add<PlaneReflectionCaptureComponent>(actor);
    REQUIRE(!plane.IsNull());
    const float notANumber = nanf("");
    const float infinity = HUGE_VALF;

    CHECK(world.With<SkyAtmosphereComponent>(skyComponent, [&](SkyAtmosphereComponent& c) noexcept {
        const auto rejects = [&](void (*edit)(SkyAtmosphereSettings&)) noexcept {
            SkyAtmosphereSettings settings;
            edit(settings);
            CHECK(!IsValidSkyAtmosphereSettings(settings));
            CHECK(EnvironmentErrorOf(c.SetSettings(settings)) == ErrorCode::InvalidArgument);
            CHECK(c.Settings() == SkyAtmosphereSettings{});
        };
        rejects([](SkyAtmosphereSettings& s) noexcept { s.bottomRadius = 0.05f; });
        rejects([](SkyAtmosphereSettings& s) noexcept { s.bottomRadius = 10000.5f; });
        rejects([](SkyAtmosphereSettings& s) noexcept { s.atmosphereHeight = 0.05f; });
        rejects([](SkyAtmosphereSettings& s) noexcept { s.multiScatteringFactor = 100.5f; });
        rejects([](SkyAtmosphereSettings& s) noexcept { s.traceSampleCountScale = 0.2f; });
        rejects([](SkyAtmosphereSettings& s) noexcept { s.mieAnisotropy = 1.0f; });
        rejects([](SkyAtmosphereSettings& s) noexcept { s.rayleighScattering.g = nanf(""); });
        rejects([](SkyAtmosphereSettings& s) noexcept { s.otherTentDistribution.width = -1.0f; });
        rejects([](SkyAtmosphereSettings& s) noexcept {
            s.transmittanceMinLightElevationAngle = 90.5f;
        });
        rejects([](SkyAtmosphereSettings& s) noexcept { s.aerialPerspectiveStartDepth = 0.0f; });
        rejects([](SkyAtmosphereSettings& s) noexcept {
            s.transformMode = static_cast<SkyAtmosphereTransformMode>(3);
        });
        // 경계값은 받는다.
        SkyAtmosphereSettings edge;
        edge.bottomRadius = 10000.0f;
        edge.mieAnisotropy = 0.999f;
        edge.transmittanceMinLightElevationAngle = 90.0f;
        CHECK(c.SetSettings(edge));
        CHECK(c.Settings() == edge);
    }));

    CHECK(world.With<HeightFogComponent>(fogComponent, [&](HeightFogComponent& c) noexcept {
        const auto rejects = [&](void (*edit)(HeightFogSettings&)) noexcept {
            HeightFogSettings settings;
            edit(settings);
            CHECK(!IsValidHeightFogSettings(settings));
            CHECK(EnvironmentErrorOf(c.SetSettings(settings)) == ErrorCode::InvalidArgument);
            CHECK(c.Settings() == HeightFogSettings{});
        };
        rejects([](HeightFogSettings& s) noexcept { s.fogDensity = 10.5f; });
        rejects([](HeightFogSettings& s) noexcept { s.fogHeightFalloff = -0.1f; });
        rejects([](HeightFogSettings& s) noexcept { s.secondFogData.fogDensity = 11.0f; });
        rejects([](HeightFogSettings& s) noexcept { s.secondFogData.fogHeightOffset = HUGE_VALF; });
        rejects([](HeightFogSettings& s) noexcept {
            s.nonDirectionalInscatteringColorDistance = 200000.0f;
        });
        rejects([](HeightFogSettings& s) noexcept { s.startDistance = 1.0e13f; });
        rejects([](HeightFogSettings& s) noexcept {
            s.fullyDirectionalInscatteringColorDistance = -1.0f;
        });
        rejects([](HeightFogSettings& s) noexcept { s.volumetricFogDistance = HUGE_VALF; });
        rejects([](HeightFogSettings& s) noexcept { s.fogMaxOpacity = 1.1f; });
        rejects([](HeightFogSettings& s) noexcept { s.inscatteringColorCubemapAngle = 361.0f; });
        rejects([](HeightFogSettings& s) noexcept { s.endDistance = -1.0f; });
        rejects([](HeightFogSettings& s) noexcept { s.volumetricFogEmissive.b = nanf(""); });
        HeightFogSettings edge;
        edge.fogDensity = 10.0f;
        // 볼륨 안개 값은 UE에 UIMin뿐이라 슬라이더 밖도 받는다.
        edge.volumetricFogScatteringDistribution = 0.95f;
        edge.volumetricFogDistance = -5.0f;
        edge.endDistance = 1.0e13f;
        edge.nonDirectionalInscatteringColorDistance =
            edge.fullyDirectionalInscatteringColorDistance;
        CHECK(c.SetSettings(edge));
        CHECK(c.Settings() == edge);
    }));

    CHECK(world.With<PostProcessComponent>(volume, [&](PostProcessComponent& c) noexcept {
        const auto rejects = [&](void (*edit)(PostProcessSettings&)) noexcept {
            PostProcessSettings settings;
            edit(settings);
            CHECK(!IsValidPostProcessSettings(settings));
            CHECK(EnvironmentErrorOf(c.SetSettings(settings)) == ErrorCode::InvalidArgument);
            CHECK(c.Settings() == PostProcessSettings{});
        };
        rejects([](PostProcessSettings& s) noexcept { s.ambientOcclusionIntensity = 1.5f; });
        rejects([](PostProcessSettings& s) noexcept { s.bloomThreshold = -2.0f; });
        rejects([](PostProcessSettings& s) noexcept { s.autoExposureLowPercent = 101.0f; });
        rejects([](PostProcessSettings& s) noexcept { s.autoExposureSpeedUp = 0.01f; });
        rejects([](PostProcessSettings& s) noexcept { s.cameraIso = 0.5f; });
        rejects([](PostProcessSettings& s) noexcept { s.cameraShutterSpeed = 0.0f; });
        rejects([](PostProcessSettings& s) noexcept { s.cameraShutterSpeed = 8000.5f; });
        rejects([](PostProcessSettings& s) noexcept { s.autoExposureMinBrightness = -10.5f; });
        rejects([](PostProcessSettings& s) noexcept { s.vignetteIntensity = nanf(""); });
        rejects([](PostProcessSettings& s) noexcept { s.depthOfFieldFstop = 0.5f; });
        rejects([](PostProcessSettings& s) noexcept { s.motionBlurTargetFps = 121; });
        rejects([](PostProcessSettings& s) noexcept { s.motionBlurTargetFps = -1; });
        rejects([](PostProcessSettings& s) noexcept { s.colorGain.w = nan(""); });
        rejects([](PostProcessSettings& s) noexcept {
            s.tonemappingMethod = static_cast<TonemappingMethod>(2);
        });
        rejects([](PostProcessSettings& s) noexcept {
            s.autoExposureMethod = static_cast<AutoExposureMethod>(3);
        });
        PostProcessSettings edge;
        edge.depthOfFieldFstop = 32.0f;
        edge.motionBlurTargetFps = 120;
        edge.overrideBloomIntensity = true;
        edge.bloomIntensity = 8.0f;
        edge.cameraShutterSpeed = 0.01f;
        // UIMin뿐인 값은 음수도 받는다.
        edge.vignetteIntensity = -1.0f;
        edge.sceneFringeIntensity = 10.0f;
        CHECK(c.SetSettings(edge));
        CHECK(c.Settings() == edge);
        edge.cameraShutterSpeed = 8000.0f;
        CHECK(c.SetSettings(edge));
        CHECK(c.Settings() == edge);

        CHECK(EnvironmentErrorOf(c.SetPriority(infinity)) == ErrorCode::InvalidArgument);
        CHECK(EnvironmentErrorOf(c.SetBlendRadius(-1.0f)) == ErrorCode::InvalidArgument);
        CHECK(EnvironmentErrorOf(c.SetBlendWeight(notANumber)) == ErrorCode::InvalidArgument);
        CHECK(c.Priority() == 0.0f && c.BlendRadius() == 100.0f && c.BlendWeight() == 1.0f);
        // blendWeight는 UE에 UIMin뿐이다(혼합이 1로 자른다).
        CHECK(c.SetPriority(-5.0f) && c.SetBlendRadius(0.0f) && c.SetBlendWeight(1.5f));
        CHECK(c.Priority() == -5.0f && c.BlendRadius() == 0.0f && c.BlendWeight() == 1.5f);
        c.SetPostProcessEnabled(false);
        c.SetUnbound(false);
        CHECK(!c.IsPostProcessEnabled() && !c.IsUnbound());
        // Component 활성과는 따로다.
        CHECK(c.IsEnabled());
    }));

    CHECK(world.With<DecalComponent>(decal, [&](DecalComponent& c) noexcept {
        CHECK(EnvironmentErrorOf(c.SetFadeScreenSize(notANumber)) == ErrorCode::InvalidArgument);
        CHECK(EnvironmentErrorOf(c.SetFadeOut(1.0f, infinity)) == ErrorCode::InvalidArgument);
        // 둘 다 유한이어도 합(수명)이 넘치면 거부하고 아무것도 바꾸지 않는다.
        CHECK(EnvironmentErrorOf(c.SetFadeOut(FLT_MAX, FLT_MAX, false)) ==
              ErrorCode::InvalidArgument);
        CHECK(c.DestroysOwnerAfterFade());
        CHECK(EnvironmentErrorOf(c.SetFadeIn(1.0f, notANumber)) == ErrorCode::InvalidArgument);
        CHECK(EnvironmentErrorOf(c.SetDecalSize(Vec3d(1.0, nan(""), 1.0))) ==
              ErrorCode::InvalidArgument);
        CHECK(EnvironmentErrorOf(c.SetDecalColor(LinearColor(notANumber, 0.0f, 0.0f, 1.0f))) ==
              ErrorCode::InvalidArgument);
        CHECK(EnvironmentErrorOf(c.SetLifeSpan(infinity)) == ErrorCode::InvalidArgument);
        CHECK(c.FadeScreenSize() == 0.01f && c.FadeStartDelay() == 0.0f &&
              c.FadeDuration() == 0.0f && c.FadeInDuration() == 0.0f);
        CHECK(NearEnvironmentVector(c.DecalSize(), 128.0, 256.0, 256.0));
        CHECK(c.DecalColor() == LinearColor::White() && c.RemainingLifeSpan() == 0.0);
        c.SetSortOrder(-3);
        CHECK(c.SortOrder() == -3);
        CHECK(c.SetFadeIn(0.5f, 2.0f));
        CHECK(c.FadeInStartDelay() == 0.5f && c.FadeInDuration() == 2.0f);
        CHECK(c.SetDecalSize(Vec3d(0.0, 0.0, 0.0)) && c.SetDecalColor(LinearColor::Black()));
        // UE decal 속성에는 범위가 없어 음수도 받는다.
        CHECK(c.SetFadeScreenSize(-0.5f) && c.FadeScreenSize() == -0.5f);
        CHECK(c.SetDecalSize(Vec3d(-1.0, 1.0, 1.0)));
        CHECK(c.SetFadeOut(-1.0f, 0.5f));
        CHECK(c.FadeStartDelay() == -1.0f && c.RemainingLifeSpan() == 0.0);
    }));

    CHECK(world.With<SphereReflectionCaptureComponent>(
        sphere, [&](SphereReflectionCaptureComponent& c) noexcept {
            CHECK(EnvironmentErrorOf(c.SetSourceCubemapAngle(notANumber)) ==
                  ErrorCode::InvalidArgument);
            CHECK(EnvironmentErrorOf(c.SetBrightness(infinity)) == ErrorCode::InvalidArgument);
            CHECK(EnvironmentErrorOf(c.SetCaptureOffset(Vec3d(0.0, HUGE_VAL, 0.0))) ==
                  ErrorCode::InvalidArgument);
            CHECK(EnvironmentErrorOf(c.SetMaxViewDistance(notANumber)) ==
                  ErrorCode::InvalidArgument);
            CHECK(EnvironmentErrorOf(c.SetSourceType(static_cast<ReflectionSourceType>(2))) ==
                  ErrorCode::InvalidArgument);
            CHECK(EnvironmentErrorOf(c.SetInfluenceRadius(-infinity)) ==
                  ErrorCode::InvalidArgument);
            CHECK(EnvironmentErrorOf(c.SetRuntimeSkylightScale(
                      LinearColor(0.1f, notANumber, 0.1f, 1.0f))) == ErrorCode::InvalidArgument);
            CHECK(c.SourceCubemapAngle() == 0.0f && c.Brightness() == 1.0f &&
                  c.MaxViewDistance() == 0.0f && c.InfluenceRadius() == 3000.0f);
            CHECK(c.SourceType() == ReflectionSourceType::CapturedScene);
            CHECK(c.RuntimeSkylightScale() == LinearColor(0.1f, 0.1f, 0.1f, 1.0f));
            // UE에 UIMin뿐인 값은 슬라이더 밖·음수도 받는다.
            CHECK(c.SetSourceCubemapAngle(400.0f) && c.SetBrightness(-1.0f));
            CHECK(c.SetMaxViewDistance(-1.0f) && c.SetInfluenceRadius(-2.0f));
            CHECK(c.SourceCubemapAngle() == 400.0f && c.Brightness() == -1.0f &&
                  c.MaxViewDistance() == -1.0f && c.InfluenceRadius() == -2.0f);
            CHECK(c.SetSourceType(ReflectionSourceType::SpecifiedCubemap));
            CHECK(c.SetCaptureOffset(Vec3d(1.0, 2.0, 3.0)) && c.SetMaxViewDistance(5000.0f));
            const LinearColor skylight(0.5f, 0.25f, 2.0f, 1.0f);
            CHECK(c.SetRuntimeSkylightScale(skylight) && c.RuntimeSkylightScale() == skylight);
            c.SetFiniteFarPlane(true);
            CHECK(c.HasFiniteFarPlane());
            c.SetRuntimeCapture(true);
            CHECK(c.UsesRuntimeCapture());
        }));
    CHECK(world.With<BoxReflectionCaptureComponent>(
        box, [](BoxReflectionCaptureComponent& c) noexcept {
            CHECK(EnvironmentErrorOf(c.SetBoxTransitionDistance(nanf(""))) ==
                  ErrorCode::InvalidArgument);
            CHECK(c.BoxTransitionDistance() == 100.0f);
            CHECK(c.SetBoxTransitionDistance(-1.0f) && c.BoxTransitionDistance() == -1.0f);
        }));
    CHECK(world.With<PlaneReflectionCaptureComponent>(
        plane, [](PlaneReflectionCaptureComponent& c) noexcept {
            CHECK(EnvironmentErrorOf(c.SetInfluenceRadiusScale(nanf(""))) ==
                  ErrorCode::InvalidArgument);
            CHECK(c.InfluenceRadiusScale() == 2.0f);
        }));
    REQUIRE(world.Shutdown());
}

// 27.2·27.3·27.6절: 대기 SetSettings만 Static을 막고(게임 World 초기화 뒤), 안개·후처리·반사 캡처
// setter는 Mobility를 보지 않는다(UE와 같다).
TEST(Environment_MobilityRules) {
    for (WorldKind kind : {WorldKind::Game, WorldKind::Tryout, WorldKind::Editor}) {
        EnvironmentDiagnosticCounter diagnostics;
        SystemAllocator logAllocator;
        LogSystemConfig logConfig;
        logConfig.mode = LogMode::Manual;
        logConfig.globalMinimum = LogLevel::Debug;
        LogSystem logs(logAllocator, logConfig);
        REQUIRE(logs.Start() && logs.AddSink(diagnostics));
        LogInstallScope installed(logs);
        EnvironmentFixture fixture(kind);
        REQUIRE(fixture.registered);
        World& world = *fixture.world;
        REQUIRE(world.Initialize());
        const bool game = kind != WorldKind::Editor;
        ObjectHandle actor;
        const ObjectHandle skyComponent = fixture.Add<SkyAtmosphereComponent>(actor);
        const ObjectHandle fogComponent = fixture.Add<HeightFogComponent>(actor);
        const ObjectHandle volume = fixture.Add<PostProcessComponent>(actor);
        const ObjectHandle sphere = fixture.Add<SphereReflectionCaptureComponent>(actor);
        REQUIRE(!sphere.IsNull());

        SkyAtmosphereSettings sky;
        sky.bottomRadius = 1000.0f;
        CHECK(world.With<SkyAtmosphereComponent>(
            skyComponent, [&](SkyAtmosphereComponent& c) noexcept {
                CHECK(c.SetSettings(sky));
                c.SetMobility(ComponentMobility::Stationary);
                sky.bottomRadius = 2000.0f;
                CHECK(c.SetSettings(sky));
                c.SetMobility(ComponentMobility::Static);
                sky.bottomRadius = 3000.0f;
                CHECK(EnvironmentErrorOf(c.SetSettings(sky)) ==
                      (game ? ErrorCode::InvalidArgument : ErrorCode::None));
                CHECK(c.Settings().bottomRadius == (game ? 2000.0f : 3000.0f));
                // 값 검증이 먼저다(어느 World든 InvalidArgument, 진단 없음).
                sky.bottomRadius = -1.0f;
                CHECK(EnvironmentErrorOf(c.SetSettings(sky)) == ErrorCode::InvalidArgument);
            }));
        CHECK(world.With<HeightFogComponent>(fogComponent, [](HeightFogComponent& c) noexcept {
            c.SetMobility(ComponentMobility::Static);
            HeightFogSettings fog;
            fog.fogDensity = 0.5f;
            CHECK(c.SetSettings(fog));
            CHECK(c.Settings().fogDensity == 0.5f);
        }));
        CHECK(world.With<PostProcessComponent>(volume, [](PostProcessComponent& c) noexcept {
            c.SetMobility(ComponentMobility::Static);
            PostProcessSettings post;
            post.overrideVignetteIntensity = true;
            post.vignetteIntensity = 1.0f;
            CHECK(c.SetSettings(post) && c.SetBlendWeight(0.5f));
        }));
        CHECK(world.With<SphereReflectionCaptureComponent>(
            sphere, [](SphereReflectionCaptureComponent& c) noexcept {
                CHECK(c.Mobility() == ComponentMobility::Static);
                CHECK(c.SetBrightness(2.0f) && c.SetInfluenceRadius(10.0f));
            }));
        (void)logs.Pump();
        CHECK(diagnostics.dataChanged == (game ? 1u : 0u));
        REQUIRE(world.Shutdown());
    }
}

// 27.2·27.3절: 행성 중심 세 모드(km)와 안개 보임·높이(cm).
TEST(Environment_PlanetCenterAndFogHeight) {
    EnvironmentFixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    ObjectHandle actor;
    const ObjectHandle skyComponent = fixture.Add<SkyAtmosphereComponent>(actor);
    REQUIRE(!skyComponent.IsNull());
    CHECK(world.With<SkyAtmosphereComponent>(skyComponent, [](SkyAtmosphereComponent& c) noexcept {
        REQUIRE(c.SetRelativeLocation(Vec3d(100000.0, -200000.0, 50000.0)));
        CHECK(NearEnvironmentVector(c.PlanetCenterKm(), 0.0, 0.0, -6360.0));
        SkyAtmosphereSettings settings;
        settings.transformMode = SkyAtmosphereTransformMode::PlanetTopAtComponentTransform;
        REQUIRE(c.SetSettings(settings));
        CHECK(NearEnvironmentVector(c.PlanetCenterKm(), 1.0, -2.0, 0.5 - 6360.0));
        settings.transformMode = SkyAtmosphereTransformMode::PlanetCenterAtComponentTransform;
        settings.bottomRadius = 100.0f;
        REQUIRE(c.SetSettings(settings));
        CHECK(NearEnvironmentVector(c.PlanetCenterKm(), 1.0, -2.0, 0.5));
        settings.transformMode = SkyAtmosphereTransformMode::PlanetTopAtComponentTransform;
        REQUIRE(c.SetSettings(settings));
        CHECK(NearEnvironmentVector(c.PlanetCenterKm(), 1.0, -2.0, 0.5 - 100.0));
    }));

    ObjectHandle parentActor;
    const ObjectHandle parent = fixture.Add<SceneComponent>(parentActor);
    const ObjectHandle fogComponent = fixture.Add<HeightFogComponent>(parentActor);
    REQUIRE(!parent.IsNull() && !fogComponent.IsNull());
    REQUIRE(world.With<SceneComponent>(parent, [](SceneComponent& c) noexcept {
        return c.SetRelativeLocation(Vec3d(0, 0, 300));
    }));
    CHECK(world.With<HeightFogComponent>(fogComponent, [&](HeightFogComponent& c) noexcept {
        REQUIRE(c.AttachTo(parent));
        REQUIRE(c.SetRelativeLocation(Vec3d(10.0, 20.0, -50.0)));
        // 월드 Z다(부모 300 + 상대 −50).
        CHECK(NearEnvironmentDouble(c.FogHeight(), 250.0));
        CHECK(NearEnvironmentDouble(c.SecondFogHeight(), 250.0));
        CHECK(c.IsFogVisible());
        HeightFogSettings settings;
        settings.secondFogData.fogHeightOffset = 75.0f;
        REQUIRE(c.SetSettings(settings));
        CHECK(NearEnvironmentDouble(c.SecondFogHeight(), 325.0));
        // 밀도 합 × 1000 ≤ UE_DELTA(1e-5)면 보이지 않는다(UE AddFogIfNeeded — float 비교).
        settings.fogDensity = 0.0f;
        REQUIRE(c.SetSettings(settings));
        CHECK(!c.IsFogVisible());
        settings.secondFogData.fogDensity = 2.0e-8f;
        REQUIRE(c.SetSettings(settings));
        CHECK(c.IsFogVisible());
        settings.secondFogData.fogDensity = 0.5e-8f;
        REQUIRE(c.SetSettings(settings));
        CHECK(!c.IsFogVisible());
        // 최대 불투명도도 1e-5보다 커야 한다(같으면 보이지 않는다).
        settings.fogDensity = 0.02f;
        settings.fogMaxOpacity = 1.0e-5f;
        REQUIRE(c.SetSettings(settings));
        CHECK(!c.IsFogVisible());
        settings.fogMaxOpacity = 2.0e-5f;
        REQUIRE(c.SetSettings(settings));
        CHECK(c.IsFogVisible());
        settings.fogMaxOpacity = 0.0f;
        REQUIRE(c.SetSettings(settings));
        CHECK(!c.IsFogVisible());
    }));
    REQUIRE(world.Shutdown());
}

// 27.4절: decal bounds(TransformedSphere)와 수명(파괴 시점·소유 Actor 파괴·취소·fade).
TEST(Environment_DecalBoundsAndLifeSpan) {
    EnvironmentFixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());

    // bounds: scale (2, 1, 1), 위치 (100, 0, 0) — extent (256, 256, 256), 구는 min(2 × |size|,
    // |extent|)다.
    ObjectHandle boundsActor;
    const ObjectHandle scaled = fixture.Add<DecalComponent>(boundsActor);
    REQUIRE(!scaled.IsNull());
    CHECK(world.With<DecalComponent>(scaled, [](DecalComponent& c) noexcept {
        REQUIRE(c.SetRelativeLocation(Vec3d(100.0, 0.0, 0.0)));
        REQUIRE(c.SetRelativeScale(Vec3d(2.0, 1.0, 1.0)));
        const ComponentBounds bounds = c.Bounds();
        CHECK(NearEnvironmentVector(bounds.origin, 100.0, 0.0, 0.0));
        CHECK(NearEnvironmentVector(bounds.boxExtent, 256.0, 256.0, 256.0));
        CHECK(NearEnvironmentDouble(bounds.sphereRadius, sqrt(3.0 * 256.0 * 256.0)));
        // 크기를 바꾸면 bounds가 다시 계산된다.
        REQUIRE(c.SetDecalSize(Vec3d(10.0, 20.0, 30.0)));
        CHECK(NearEnvironmentVector(c.Bounds().boxExtent, 20.0, 20.0, 30.0));
        CHECK(NearEnvironmentDouble(c.Bounds().sphereRadius,
                                    sqrt(20.0 * 20.0 + 20.0 * 20.0 + 30.0 * 30.0)));
    }));

    // DecalActor: pitch −90° root — 지역 X가 아래(−Z)라 extent는 (256, 256, 128)이다.
    auto decalActor = world.SpawnActor<DecalActor>();
    REQUIRE(decalActor);
    CHECK(world.With<DecalComponent>(
        fixture.RootOf(decalActor.Value()), [](DecalComponent& c) noexcept {
            const ComponentBounds bounds = c.Bounds();
            CHECK(NearEnvironmentVector(bounds.boxExtent, 256.0, 256.0, 128.0));
            CHECK(NearEnvironmentDouble(bounds.sphereRadius, 384.0));
        }));

    // 수명 a: fade 0.25 + 0.25, 소유 Actor도 파괴.
    ObjectHandle fadingActor;
    const ObjectHandle fading = fixture.Add<DecalComponent>(fadingActor);
    // 수명 b: 같은 fade, 소유 Actor는 남긴다.
    ObjectHandle keptActor;
    const ObjectHandle kept = fixture.Add<DecalComponent>(keptActor);
    // 수명 c: fade 없이 SetLifeSpan — 컴포넌트만 파괴(UE: fade 시간이 없으면 Actor를 남긴다).
    ObjectHandle plainActor;
    const ObjectHandle plain = fixture.Add<DecalComponent>(plainActor);
    // 수명 d: 정했다가 취소.
    ObjectHandle cancelActor;
    const ObjectHandle cancelled = fixture.Add<DecalComponent>(cancelActor);
    // 수명 e: 기본(수명 없음).
    ObjectHandle foreverActor;
    const ObjectHandle forever = fixture.Add<DecalComponent>(foreverActor);
    REQUIRE(!fading.IsNull() && !kept.IsNull() && !plain.IsNull() && !cancelled.IsNull() &&
            !forever.IsNull());
    REQUIRE(world.With<DecalComponent>(
        fading, [](DecalComponent& c) noexcept { return c.SetFadeOut(0.25f, 0.25f); }));
    REQUIRE(world.With<DecalComponent>(
        kept, [](DecalComponent& c) noexcept { return c.SetFadeOut(0.25f, 0.25f, false); }));
    CHECK(world.With<DecalComponent>(fading, [](DecalComponent& c) noexcept {
        CHECK(c.FadeStartDelay() == 0.25f && c.FadeDuration() == 0.25f);
        CHECK(c.DestroysOwnerAfterFade() && c.RemainingLifeSpan() == 0.5);
        // 시작 전에 수명을 취소해 두면 OnStart만 수명을 다시 정할 수 있다.
        CHECK(c.SetLifeSpan(0.0f) && c.RemainingLifeSpan() == 0.0);
    }));
    REQUIRE(world.BeginPlay());
    // OnStart가 fade 합으로 수명을 다시 정한다(UE BeginPlay).
    CHECK(world.With<DecalComponent>(
        fading, [](DecalComponent& c) noexcept { CHECK(c.RemainingLifeSpan() == 0.5); }));
    CHECK(world.With<DecalComponent>(
        forever, [](DecalComponent& c) noexcept { CHECK(c.RemainingLifeSpan() == 0.0); }));
    REQUIRE(world.With<DecalComponent>(
        plain, [](DecalComponent& c) noexcept { return c.SetLifeSpan(0.25f); }));
    REQUIRE(world.With<DecalComponent>(cancelled, [](DecalComponent& c) noexcept {
        if (auto set = c.SetLifeSpan(0.25f); !set) {
            return set;
        }

        return c.SetLifeSpan(-1.0f);
    }));
    CHECK(world.With<DecalComponent>(
        cancelled, [](DecalComponent& c) noexcept { CHECK(c.RemainingLifeSpan() == 0.0); }));

    REQUIRE(fixture.TickTimes(1));
    CHECK(world.With<DecalComponent>(
        fading, [](DecalComponent& c) noexcept { CHECK(c.RemainingLifeSpan() == 0.375); }));
    REQUIRE(fixture.TickTimes(1));
    // c: 0.25초가 지나 컴포넌트만 파괴.
    CHECK(fixture.ComponentGone(plain));
    CHECK(!fixture.ActorGone(plainActor));
    CHECK(!fixture.ComponentGone(fading) && !fixture.ComponentGone(kept));
    REQUIRE(fixture.TickTimes(1));
    CHECK(!fixture.ComponentGone(fading) && !fixture.ComponentGone(kept));
    REQUIRE(fixture.TickTimes(1));
    // a·b: 0.5초에 파괴, a만 소유 Actor도.
    CHECK(fixture.ComponentGone(fading) && fixture.ActorGone(fadingActor));
    CHECK(fixture.ComponentGone(kept) && !fixture.ActorGone(keptActor));
    REQUIRE(fixture.TickTimes(8));
    CHECK(!fixture.ComponentGone(cancelled) && !fixture.ComponentGone(forever));
    CHECK(!fixture.ActorGone(cancelActor) && !fixture.ActorGone(foreverActor));
    REQUIRE(world.Shutdown());
}

// 27.5절: 덮어쓰기 — override 켜진 값만, 연속 값 Lerp·이산 값 그대로, weight clamp·0·비유한.
TEST(Environment_PostProcessOverride) {
    PostProcessSettings source;
    source.overrideBloomIntensity = true;
    source.bloomIntensity = 2.675f;
    source.overrideTonemappingMethod = true;
    source.tonemappingMethod = TonemappingMethod::StandardAces;
    source.overrideColorGain = true;
    source.colorGain = Vec4d(3.0, 3.0, 3.0, 5.0);
    source.overrideMotionBlurTargetFps = true;
    source.motionBlurTargetFps = 60;
    source.overrideAutoExposureApplyPhysicalCameraExposure = true;
    source.autoExposureApplyPhysicalCameraExposure = false;
    // 플래그가 꺼진 값은 쓰지 않는다.
    source.filmSlope = 0.1f;
    source.vignetteIntensity = 3.0f;

    PostProcessSettings half;
    OverridePostProcessSettings(half, source, 0.5f);
    CHECK(NearEnvironmentFloat(half.bloomIntensity, 1.675f));
    CHECK(half.tonemappingMethod == TonemappingMethod::StandardAces);
    CHECK(half.colorGain == Vec4d(2.0, 2.0, 2.0, 3.0));
    CHECK(half.motionBlurTargetFps == 60 && !half.autoExposureApplyPhysicalCameraExposure);
    CHECK(half.filmSlope == 0.88f && half.vignetteIntensity == 0.4f);
    // destination의 override 플래그는 그대로다.
    CHECK(!half.overrideBloomIntensity && !half.overrideTonemappingMethod);

    // 층을 차례로 덮는다: 둘째 층이 bloom만 다시 덮는다.
    PostProcessSettings second;
    second.overrideBloomIntensity = true;
    second.bloomIntensity = 0.0f;
    OverridePostProcessSettings(half, second, 0.25f);
    CHECK(NearEnvironmentFloat(half.bloomIntensity, 1.675f * 0.75f));
    CHECK(half.tonemappingMethod == TonemappingMethod::StandardAces);

    // weight > 1은 1로 자른다(+inf 포함 — UE SceneView.cpp:1638).
    for (float weight : {2.0f, HUGE_VALF}) {
        PostProcessSettings full;
        OverridePostProcessSettings(full, source, weight);
        CHECK(full.bloomIntensity == 2.675f && full.colorGain == source.colorGain);
        CHECK(full.tonemappingMethod == TonemappingMethod::StandardAces);
    }

    // weight 0 이하·NaN이면 아무것도 바꾸지 않는다(이산 값도).
    for (float weight : {0.0f, -1.0f, -HUGE_VALF, nanf("")}) {
        PostProcessSettings untouched;
        OverridePostProcessSettings(untouched, source, weight);
        CHECK(untouched == PostProcessSettings{});
    }

    // 아주 작은 양의 weight도 이산 값은 그대로 쓴다(UE SET_PP).
    PostProcessSettings tiny;
    OverridePostProcessSettings(tiny, source, 1.0e-6f);
    CHECK(tiny.tonemappingMethod == TonemappingMethod::StandardAces &&
          tiny.motionBlurTargetFps == 60);
    CHECK(NearEnvironmentFloat(tiny.bloomIntensity, 0.675f));

    // 초점 거리: 어느 쪽이 0("DOF 끔")이면 보간하지 않고 src를 쓴다(UE SceneView.cpp:1780).
    PostProcessSettings focus;
    focus.overrideDepthOfFieldFocalDistance = true;
    focus.depthOfFieldFocalDistance = 1000.0f;
    PostProcessSettings fromOff;
    OverridePostProcessSettings(fromOff, focus, 0.5f);
    CHECK(fromOff.depthOfFieldFocalDistance == 1000.0f);
    PostProcessSettings both;
    both.depthOfFieldFocalDistance = 200.0f;
    OverridePostProcessSettings(both, focus, 0.5f);
    CHECK(NearEnvironmentFloat(both.depthOfFieldFocalDistance, 600.0f));
    PostProcessSettings off;
    off.overrideDepthOfFieldFocalDistance = true;
    OverridePostProcessSettings(both, off, 0.25f);
    CHECK(both.depthOfFieldFocalDistance == 0.0f);
    // override가 꺼져 있으면 0이어도 바꾸지 않는다.
    PostProcessSettings ignored;
    ignored.depthOfFieldFocalDistance = 500.0f;
    OverridePostProcessSettings(ignored, PostProcessSettings{}, 1.0f);
    CHECK(ignored.depthOfFieldFocalDistance == 500.0f);
}

// 27.6절: 영향 반경 — Sphere는 반지름, Box는 |월드 scale|, Plane은 |(sy, sz)| × 배율.
TEST(Environment_ReflectionCaptureRadius) {
    EnvironmentFixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    const Transform scaled(Quatd::Identity(), Vec3d(0.0, 0.0, 0.0), Vec3d(2.0, 2.0, 2.0));
    auto sphere = world.SpawnActor<SphereReflectionCapture>(SpawnParams::At(scaled));
    auto box = world.SpawnActor<BoxReflectionCapture>();
    auto plane = world.SpawnActor<PlaneReflectionCapture>();
    auto scaledBox = world.SpawnActor<BoxReflectionCapture>(SpawnParams::At(scaled));
    REQUIRE(sphere && box && plane && scaledBox);

    CHECK(world.With<SphereReflectionCaptureComponent>(
        fixture.RootOf(sphere.Value()), [](SphereReflectionCaptureComponent& c) noexcept {
            // scale과 무관하다(UE GetInfluenceBoundingRadius).
            CHECK(c.InfluenceBoundingRadius() == 3000.0);
            REQUIRE(c.SetInfluenceRadius(500.0f));
            CHECK(c.InfluenceBoundingRadius() == 500.0);
        }));
    CHECK(world.With<BoxReflectionCaptureComponent>(
        fixture.RootOf(box.Value()), [](BoxReflectionCaptureComponent& c) noexcept {
            CHECK(NearEnvironmentDouble(c.InfluenceBoundingRadius(),
                                        sqrt(1000.0 * 1000.0 * 2.0 + 400.0 * 400.0)));
        }));
    CHECK(world.With<BoxReflectionCaptureComponent>(
        fixture.RootOf(scaledBox.Value()), [](BoxReflectionCaptureComponent& c) noexcept {
            // Spawn scale × root 상대 scale.
            CHECK(NearEnvironmentDouble(c.InfluenceBoundingRadius(),
                                        2.0 * sqrt(1000.0 * 1000.0 * 2.0 + 400.0 * 400.0)));
        }));
    CHECK(world.With<PlaneReflectionCaptureComponent>(
        fixture.RootOf(plane.Value()), [](PlaneReflectionCaptureComponent& c) noexcept {
            CHECK(NearEnvironmentDouble(c.InfluenceBoundingRadius(), sqrt(2.0e6) * 2.0));
            REQUIRE(c.SetInfluenceRadiusScale(0.5f));
            CHECK(NearEnvironmentDouble(c.InfluenceBoundingRadius(), sqrt(2.0e6) * 0.5));
        }));
    REQUIRE(world.Shutdown());
}

// 27.1·27.4·27.6절: 환경 Actor root(타입·회전·scale·Static), HeightFog 켜고 끄기, root 재사용·거부.
TEST(Environment_ActorRoots) {
    EnvironmentFixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    auto sky = world.SpawnActor<SkyAtmosphere>();
    auto fog = world.SpawnActor<HeightFog>();
    auto decal = world.SpawnActor<DecalActor>();
    auto sphere = world.SpawnActor<SphereReflectionCapture>();
    auto box = world.SpawnActor<BoxReflectionCapture>();
    auto plane = world.SpawnActor<PlaneReflectionCapture>();
    REQUIRE(sky && fog && decal && sphere && box && plane);

    const auto mobilityOf = [&](ObjectHandle actor) noexcept {
        auto mobility = world.With<SceneComponent>(
            fixture.RootOf(actor), [](SceneComponent& root) noexcept { return root.Mobility(); });
        return mobility ? mobility.Value() : ComponentMobility::Movable;
    };
    const auto relativeOf = [&](ObjectHandle actor) noexcept {
        auto relative =
            world.With<SceneComponent>(fixture.RootOf(actor), [](SceneComponent& root) noexcept {
                return root.RelativeTransform();
            });
        return relative ? relative.Value() : Transform();
    };

    CHECK(world.With<SkyAtmosphereComponent>(fixture.RootOf(sky.Value()),
                                             [](SkyAtmosphereComponent&) noexcept {}));
    CHECK(world.With<HeightFogComponent>(fixture.RootOf(fog.Value()),
                                         [](HeightFogComponent&) noexcept {}));
    CHECK(
        world.With<DecalComponent>(fixture.RootOf(decal.Value()), [](DecalComponent&) noexcept {}));
    for (ObjectHandle actor :
         {sky.Value(), fog.Value(), decal.Value(), sphere.Value(), box.Value(), plane.Value()}) {
        ObjectHandle components[2];
        auto count = world.GetComponents(actor, Span<ObjectHandle>(components, 2));
        CHECK(count && count.Value() == 1);
    }

    // 컴포넌트 기본 Mobility를 그대로 둔다: 대기·안개·decal Movable, 반사 캡처 Static.
    CHECK(mobilityOf(sky.Value()) == ComponentMobility::Movable);
    CHECK(mobilityOf(fog.Value()) == ComponentMobility::Movable);
    CHECK(mobilityOf(decal.Value()) == ComponentMobility::Movable);
    CHECK(mobilityOf(sphere.Value()) == ComponentMobility::Static);
    CHECK(mobilityOf(box.Value()) == ComponentMobility::Static);
    CHECK(mobilityOf(plane.Value()) == ComponentMobility::Static);

    // decal pitch −90°: 지역 X가 월드 −Z다.
    const Transform decalRelative = relativeOf(decal.Value());
    CHECK(NearEnvironmentVector(decalRelative.rotation.RotateVector(Vec3d(1.0, 0.0, 0.0)), 0.0, 0.0,
                                -1.0));
    CHECK(NearEnvironmentVector(relativeOf(box.Value()).scale, 1000.0, 1000.0, 400.0));
    CHECK(NearEnvironmentVector(relativeOf(plane.Value()).scale, 1.0, 1000.0, 1000.0));
    CHECK(NearEnvironmentVector(relativeOf(sphere.Value()).scale, 1.0, 1.0, 1.0));
    CHECK(world.With<ReflectionCapture>(box.Value(), [](ReflectionCapture& actor) noexcept {
        CHECK(actor.CaptureComponentHandle() == actor.RootComponent());
    }));

    // 켜고 끄기는 HeightFog에만 있다(root visible).
    CHECK(world.With<HeightFog>(fog.Value(), [](HeightFog& actor) noexcept {
        CHECK(actor.IsFogEnabled());
        CHECK(actor.SetFogEnabled(false));
        CHECK(!actor.IsFogEnabled());
        CHECK(actor.SetFogEnabled(true));
        CHECK(actor.IsFogEnabled());
    }));
    CHECK(world.With<HeightFogComponent>(
        fixture.RootOf(fog.Value()),
        [](HeightFogComponent& c) noexcept { CHECK(c.GetVisibleFlag()); }));

    // root가 없으면(Spawn 전) 켜고 끄기는 InvalidArgument다.
    auto deferredFog = world.SpawnActorDeferred<HeightFog>();
    REQUIRE(deferredFog);
    CHECK(world.With<HeightFog>(deferredFog.Value(), [](HeightFog& actor) noexcept {
        CHECK(EnvironmentErrorOf(actor.SetFogEnabled(false)) == ErrorCode::InvalidArgument);
        CHECK(!actor.IsFogEnabled());
    }));
    REQUIRE(world.FinishSpawning(deferredFog.Value()));

    // 같은 타입 root는 재사용한다(다시 설정하지 않는다 — scale 그대로).
    auto deferredBox = world.SpawnActorDeferred<BoxReflectionCapture>();
    REQUIRE(deferredBox);
    auto preset = world.AddComponent<BoxReflectionCaptureComponent>(deferredBox.Value());
    REQUIRE(preset);
    REQUIRE(world.With<Actor>(deferredBox.Value(), [&](Actor& actor) noexcept {
        return actor.SetRootComponent(preset.Value());
    }));
    REQUIRE(world.FinishSpawning(deferredBox.Value()));
    CHECK(fixture.RootOf(deferredBox.Value()) == preset.Value());
    CHECK(NearEnvironmentVector(relativeOf(deferredBox.Value()).scale, 1.0, 1.0, 1.0));

    // 다른 타입 root(Sphere 캡처를 Plane Actor에)면 Spawn이 InvalidArgument다.
    auto mismatched = world.SpawnActorDeferred<PlaneReflectionCapture>();
    REQUIRE(mismatched);
    auto wrong = world.AddComponent<SphereReflectionCaptureComponent>(mismatched.Value());
    REQUIRE(wrong);
    REQUIRE(world.With<Actor>(mismatched.Value(), [&](Actor& actor) noexcept {
        return actor.SetRootComponent(wrong.Value());
    }));
    CHECK(EnvironmentErrorOf(world.FinishSpawning(mismatched.Value())) ==
          ErrorCode::InvalidArgument);
    REQUIRE(world.Shutdown());
}

// 27.8절: 리플렉션 형태(Abstract·base·필드 수)와 Level 그래프 저장 왕복.
TEST(Environment_ReflectionAndSaveRoundTrip) {
    EnvironmentFixture fixture(WorldKind::Editor);
    REQUIRE(fixture.registered);
    TypeRegistry& types = fixture.types;
    struct Expected {
        TypeId id;
        TypeId base;
        usize fields;
        bool abstract;
    };

    // 필드 수는 상속분을 포함한다(SceneComponent 11, Actor 4). 더하는 수는 대기·안개 1, decal 9,
    // 후처리 6, 반사 캡처 기반 8, Sphere·Box·Plane 1, 환경 Actor 0이다.
    const Expected expected[] = {
        {ObjectTypeIdOf<SkyAtmosphereComponent>(), ObjectTypeIdOf<SceneComponent>(), 12, false},
        {ObjectTypeIdOf<HeightFogComponent>(), ObjectTypeIdOf<SceneComponent>(), 12, false},
        {ObjectTypeIdOf<DecalComponent>(), ObjectTypeIdOf<SceneComponent>(), 20, false},
        {ObjectTypeIdOf<PostProcessComponent>(), ObjectTypeIdOf<SceneComponent>(), 17, false},
        {ObjectTypeIdOf<ReflectionCaptureComponent>(), ObjectTypeIdOf<SceneComponent>(), 19, true},
        {ObjectTypeIdOf<SphereReflectionCaptureComponent>(),
         ObjectTypeIdOf<ReflectionCaptureComponent>(), 20, false},
        {ObjectTypeIdOf<BoxReflectionCaptureComponent>(),
         ObjectTypeIdOf<ReflectionCaptureComponent>(), 20, false},
        {ObjectTypeIdOf<PlaneReflectionCaptureComponent>(),
         ObjectTypeIdOf<ReflectionCaptureComponent>(), 20, false},
        {ObjectTypeIdOf<SkyAtmosphere>(), ObjectTypeIdOf<Actor>(), 4, false},
        {ObjectTypeIdOf<HeightFog>(), ObjectTypeIdOf<Actor>(), 4, false},
        {ObjectTypeIdOf<DecalActor>(), ObjectTypeIdOf<Actor>(), 4, false},
        {ObjectTypeIdOf<ReflectionCapture>(), ObjectTypeIdOf<Actor>(), 4, true},
        {ObjectTypeIdOf<SphereReflectionCapture>(), ObjectTypeIdOf<ReflectionCapture>(), 4, false},
        {ObjectTypeIdOf<BoxReflectionCapture>(), ObjectTypeIdOf<ReflectionCapture>(), 4, false},
        {ObjectTypeIdOf<PlaneReflectionCapture>(), ObjectTypeIdOf<ReflectionCapture>(), 4, false},
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

    // 설정 구조체 필드 수(후처리는 값 42 + override 플래그 42).
    CHECK(GeneratedType<SkyAtmosphereSettings>::Schema().fields.Size() == 26);
    CHECK(GeneratedType<SkyAtmosphereTentDistribution>::Schema().fields.Size() == 3);
    CHECK(GeneratedType<HeightFogSettings>::Schema().fields.Size() == 32);
    CHECK(GeneratedType<HeightFogData>::Schema().fields.Size() == 3);
    CHECK(GeneratedType<PostProcessSettings>::Schema().fields.Size() == 84);

    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    auto level = world.CreateLevel("Environment");
    REQUIRE(level);
    SpawnParams params;
    params.level = level.Value();
    auto sky = world.SpawnActor<SkyAtmosphere>(params);
    auto fog = world.SpawnActor<HeightFog>(params);
    auto decal = world.SpawnActor<DecalActor>(params);
    auto box = world.SpawnActor<BoxReflectionCapture>(params);
    REQUIRE(sky && fog && decal && box);
    // 후처리 볼륨은 대기 Actor의 둘째 컴포넌트로 둔다(시험 전용 native Actor는 그래프 저장 대상이
    // 아니다).
    auto addedVolume = world.AddComponent<PostProcessComponent>(sky.Value());
    REQUIRE(addedVolume);
    const ObjectHandle volume = addedVolume.Value();

    SkyAtmosphereSettings skySettings;
    skySettings.transformMode = SkyAtmosphereTransformMode::PlanetCenterAtComponentTransform;
    skySettings.bottomRadius = 1234.5f;
    skySettings.groundAlbedo = Color(1, 2, 3, 4);
    skySettings.otherTentDistribution.width = 7.0f;
    skySettings.mieAbsorption = LinearColor(0.5f, 0.25f, 0.125f, 1.0f);
    skySettings.holdout = true;
    REQUIRE(world.With<SkyAtmosphereComponent>(
        fixture.RootOf(sky.Value()),
        [&](SkyAtmosphereComponent& c) noexcept { return c.SetSettings(skySettings); }));
    HeightFogSettings fogSettings;
    fogSettings.fogDensity = 0.5f;
    fogSettings.secondFogData.fogDensity = 0.25f;
    fogSettings.secondFogData.fogHeightOffset = -40.0f;
    fogSettings.volumetricFogAlbedo = Color(10, 20, 30);
    fogSettings.enableVolumetricFog = true;
    REQUIRE(world.With<HeightFogComponent>(
        fixture.RootOf(fog.Value()),
        [&](HeightFogComponent& c) noexcept { return c.SetSettings(fogSettings); }));
    REQUIRE(
        world.With<DecalComponent>(fixture.RootOf(decal.Value()), [](DecalComponent& c) noexcept {
            c.SetSortOrder(4);
            if (auto set = c.SetDecalSize(Vec3d(1.0, 2.0, 3.0)); !set) {
                return set;
            }

            if (auto set = c.SetDecalColor(LinearColor(0.1f, 0.2f, 0.3f, 0.4f)); !set) {
                return set;
            }

            if (auto set = c.SetFadeIn(1.5f, 2.5f); !set) {
                return set;
            }

            return c.SetFadeOut(3.0f, 4.0f, false);
        }));
    REQUIRE(world.With<BoxReflectionCaptureComponent>(
        fixture.RootOf(box.Value()), [](BoxReflectionCaptureComponent& c) noexcept {
            if (auto set = c.SetSourceType(ReflectionSourceType::SpecifiedCubemap); !set) {
                return set;
            }

            if (auto set = c.SetBrightness(2.5f); !set) {
                return set;
            }

            if (auto set = c.SetCaptureOffset(Vec3d(5.0, 6.0, 7.0)); !set) {
                return set;
            }

            return c.SetBoxTransitionDistance(42.0f);
        }));
    PostProcessSettings postSettings;
    postSettings.overrideBloomIntensity = true;
    postSettings.bloomIntensity = 3.5f;
    postSettings.overrideBloomMethod = true;
    postSettings.bloomMethod = BloomMethod::Convolution;
    postSettings.colorGamma = Vec4d(0.5, 0.6, 0.7, 0.8);
    postSettings.motionBlurTargetFps = 90;
    REQUIRE(world.With<PostProcessComponent>(volume, [&](PostProcessComponent& c) noexcept {
        c.SetPostProcessEnabled(false);
        c.SetUnbound(false);
        if (auto set = c.SetPriority(2.0f); !set) {
            return set;
        }

        if (auto set = c.SetBlendRadius(250.0f); !set) {
            return set;
        }

        if (auto set = c.SetBlendWeight(0.75f); !set) {
            return set;
        }

        return c.SetSettings(postSettings);
    }));

    Array<uint8> saved(fixture.allocator);
    {
        auto bytes =
            SaveObjectGraph(fixture.allocator, world.Objects(), world.Access(), level.Value());
        REQUIRE(bytes);
        saved = Move(bytes.Value());
    }

    {
        ObjectRegistry loaded(fixture.allocator, fixture.types, {0x454e56ull, 2});
        const auto access = loaded.OwnerAccess();
        auto root = LoadObjectGraph(fixture.allocator, loaded, fixture.types, access, saved.View());
        REQUIRE(root);
        // Level actors 순서는 보장하지 않는다(7.2절) — 타입으로 고른다.
        ObjectHandle loadedSky;
        ObjectHandle loadedFog;
        ObjectHandle loadedDecal;
        ObjectHandle loadedBox;
        {
            auto borrowed = loaded.Resolve<Level>(access, root.Value().Handle());
            REQUIRE(borrowed);
            Level* pointer = EnvironmentGet(borrowed.Value());
            REQUIRE(pointer != nullptr && pointer->Actors().Size() == 4);
            for (usize index = 0; index < 4; ++index) {
                const ObjectHandle actor = pointer->Actors()[index];
                if (loaded.Resolve<SkyAtmosphere>(access, actor)) {
                    loadedSky = actor;
                } else if (loaded.Resolve<HeightFog>(access, actor)) {
                    loadedFog = actor;
                } else if (loaded.Resolve<DecalActor>(access, actor)) {
                    loadedDecal = actor;
                } else if (loaded.Resolve<BoxReflectionCapture>(access, actor)) {
                    loadedBox = actor;
                }
            }
        }

        REQUIRE(!loadedSky.IsNull() && !loadedFog.IsNull() && !loadedDecal.IsNull() &&
                !loadedBox.IsNull());
        const auto rootOf = [&](ObjectHandle actor) noexcept {
            auto borrowed = loaded.Resolve<Actor>(access, actor);
            Actor* pointer = borrowed ? EnvironmentGet(borrowed.Value()) : nullptr;
            return pointer != nullptr ? pointer->RootComponent() : ObjectHandle{};
        };

        {
            auto borrowed = loaded.Resolve<SkyAtmosphereComponent>(access, rootOf(loadedSky));
            REQUIRE(borrowed);
            SkyAtmosphereComponent* component = EnvironmentGet(borrowed.Value());
            REQUIRE(component != nullptr);
            CHECK(component->Settings() == skySettings);
        }

        {
            auto borrowed = loaded.Resolve<HeightFogComponent>(access, rootOf(loadedFog));
            REQUIRE(borrowed);
            HeightFogComponent* component = EnvironmentGet(borrowed.Value());
            REQUIRE(component != nullptr);
            CHECK(component->Settings() == fogSettings);
        }

        {
            auto borrowed = loaded.Resolve<DecalComponent>(access, rootOf(loadedDecal));
            REQUIRE(borrowed);
            DecalComponent* component = EnvironmentGet(borrowed.Value());
            REQUIRE(component != nullptr);
            CHECK(component->SortOrder() == 4);
            CHECK(NearEnvironmentVector(component->DecalSize(), 1.0, 2.0, 3.0));
            CHECK(component->DecalColor() == LinearColor(0.1f, 0.2f, 0.3f, 0.4f));
            CHECK(component->FadeInStartDelay() == 1.5f && component->FadeInDuration() == 2.5f);
            CHECK(component->FadeStartDelay() == 3.0f && component->FadeDuration() == 4.0f);
            CHECK(!component->DestroysOwnerAfterFade());
            // 남은 수명은 저장하지 않는다(OnStart가 fade로 다시 정한다).
            CHECK(component->RemainingLifeSpan() == 0.0);
        }

        {
            auto borrowed =
                loaded.Resolve<BoxReflectionCaptureComponent>(access, rootOf(loadedBox));
            REQUIRE(borrowed);
            BoxReflectionCaptureComponent* component = EnvironmentGet(borrowed.Value());
            REQUIRE(component != nullptr);
            CHECK(component->SourceType() == ReflectionSourceType::SpecifiedCubemap);
            CHECK(component->Brightness() == 2.5f && component->BoxTransitionDistance() == 42.0f);
            CHECK(NearEnvironmentVector(component->CaptureOffset(), 5.0, 6.0, 7.0));
            CHECK(component->Mobility() == ComponentMobility::Static);
        }

        ObjectHandle loadedVolume;
        {
            auto actor = loaded.Resolve<SkyAtmosphere>(access, loadedSky);
            REQUIRE(actor);
            SkyAtmosphere* pointer = EnvironmentGet(actor.Value());
            REQUIRE(pointer != nullptr && pointer->Components().Size() == 2);
            for (usize index = 0; index < 2; ++index) {
                if (loaded.Resolve<PostProcessComponent>(access, pointer->Components()[index])) {
                    loadedVolume = pointer->Components()[index];
                }
            }
        }

        auto borrowed = loaded.Resolve<PostProcessComponent>(access, loadedVolume);
        REQUIRE(borrowed);
        PostProcessComponent* component = EnvironmentGet(borrowed.Value());
        REQUIRE(component != nullptr);
        CHECK(component->Settings() == postSettings);
        CHECK(!component->IsPostProcessEnabled() && !component->IsUnbound());
        CHECK(component->Priority() == 2.0f && component->BlendRadius() == 250.0f &&
              component->BlendWeight() == 0.75f);
    }

    REQUIRE(world.Shutdown());
}

} // namespace world_environment_component_tests_file
