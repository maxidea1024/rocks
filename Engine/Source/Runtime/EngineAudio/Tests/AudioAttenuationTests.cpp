#include "AudioEngineTestSupport.h"

#include <Base/InitializerList.h>
#include <Core/Containers/Array.h>
#include <Core/Math/MathUtility.h>
#include <Core/Memory/SystemAllocator.h>
#include <EngineAudio/AudioAttenuation.h>
#include <EngineAudio/AudioSpatialization.h>
#include <TestSupport/Test.h>

// AU7d 감쇠 시험이다(Docs/Design/AudioSystem 3.42절): 모양 넷(회전 포함)·Custom 곡선·최대 가청
// 거리·공기 흡수·우선순위 거리 감쇠·focus 순수 함수는 UE 식을 손으로 계산한 값과 비교하고, 제어
// 쪽 연결(회전한 상자 emitter, focus 볼륨, 공기 흡수 HPF, 우선순위 감쇠와 cull)은 Inline 제어 +
// Manual 렌더로 출력·이벤트를 본다.
using namespace rocks;
namespace audio_attenuation_tests_file {

using namespace rocks::engine_audio_test;

namespace {

constexpr float Tolerance = 1.0e-5f;
constexpr float Falloff = 1000.0f;
// 재생이 시작해 출력이 고르게 될 때까지의 프레임 수다(Started 3프레임 + 믹서 ramp).
constexpr uint32 StartFrames = 6;
// mono 상수 0.2의 Direct 출력(0.5배)이다.
constexpr float DirectLevel = 0.1f;

bool Near(float actual, float expected, float tolerance = Tolerance) noexcept {
    return math::Abs(actual - expected) <= tolerance;
}

SoundAttenuation Linear(SoundAttenuationShape shape, const Vec3& extents) noexcept {
    SoundAttenuation attenuation;
    attenuation.shape = shape;
    attenuation.shapeExtents = extents;
    attenuation.distance.falloffDistance = Falloff;
    return attenuation;
}

Quatd Yaw(double degrees) noexcept {
    auto made = Quatd::TryFromAxisAngle(Vec3d(0.0, 0.0, 1.0), degrees * math::Pi<double> / 180.0);
    return made ? made.Value() : Quatd::Identity();
}

float Shape(const SoundAttenuation& attenuation, const Vec3d& listener,
            const Quatd& rotation = Quatd::Identity(), float scale = 1.0f) noexcept {
    return EvaluateShapeAttenuation(attenuation, listener, Vec3d(), rotation, scale);
}

// 키 (time, value)를 Linear 보간으로 넣은 곡선이다.
SharedPtr<const TangentCurve> MakeCurve(Allocator& allocator,
                                        std::initializer_list<Vec3> keys) noexcept {
    auto made = SharedPtr<TangentCurve>::TryMake(allocator, allocator);
    if (!made) {
        return {};
    }

    TangentCurve& curve = *made.Value().Get();
    for (const Vec3& key : keys) {
        auto added = curve.TryAddKey(key.x, key.y);
        if (added) {
            (void)curve.TrySetKeyInterp(added.Value(), CurveInterp::Linear);
        }
    }

    return made.Value();
}

// 위치 재생 사운드다(mono 상수 0.2 loop, spatialize 끔 — 출력 = 0.2 × 0.5(mono Direct) × 이득).
SoundSourceRef Sound(Allocator& allocator, const SharedPtr<const SoundPcm>& pcm,
                     const SoundAttenuation& attenuation,
                     SoundVirtualizationMode mode = SoundVirtualizationMode::Restart,
                     std::initializer_list<SoundConcurrencyEntry> entries = {}) noexcept {
    SoundPlayable playable;
    playable.pcm = pcm;
    playable.looping = true;
    playable.virtualizationMode = mode;
    playable.attenuation = attenuation;
    for (const SoundConcurrencyEntry& entry : entries) {
        playable.concurrency[playable.concurrencyCount++] = entry;
    }

    auto made = SoundSourceRef::TryCreate(allocator, playable);
    return made ? made.Value() : SoundSourceRef{};
}

// 시작 뒤 한 프레임 출력의 왼쪽 최대다.
float SteadyPeak(AudioEngine& fixture) noexcept {
    (void)fixture.Frames(StartFrames);
    Array<float> chunk(fixture.allocator);
    if (!chunk.TryResize(static_cast<usize>(PumpFrames) * 2)) {
        return -1.0f;
    }

    (void)CapturePeak(fixture, chunk);
    return CapturePeak(fixture, chunk).left;
}

float PlayedPeak(const SoundAttenuation& attenuation, const Vec3d& listener,
                 const Quatd& rotation = Quatd::Identity(),
                 const GlobalFocusSettings& focus = {}) noexcept {
    AudioEngine fixture(DeterministicOptions(true));
    if (!fixture.initialized || !fixture.audio->SetGlobalFocusSettings(focus)) {
        return -1.0f;
    }

    const AudioSceneId scene = fixture.audio->RegisterScene();
    fixture.audio->SetSceneListener(scene, listener, Quatd::Identity());
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.2f, TestRate);
    if (!fixture.audio->PlayAtInScene(scene, Sound(fixture.allocator, pcm, attenuation), Vec3d(),
                                      rotation)) {
        return -1.0f;
    }

    return SteadyPeak(fixture);
}

} // namespace

// Sphere·Box: 반경·반폭 밖의 거리로 Linear 감쇠, Box는 falloff 밖이면 거리 배수와 상관없이 0,
// 회전한 상자는 지역 축으로 잰다.
TEST(EngineAudio_AttenuationSphereAndBox) {
    const SoundAttenuation sphere = Linear(SoundAttenuationShape::Sphere, Vec3(400.0f, 0, 0));
    CHECK(Near(Shape(sphere, Vec3d(900.0, 0.0, 0.0)), 0.5f));
    CHECK(Near(Shape(sphere, Vec3d(100.0, 0.0, 0.0)), 1.0f));
    CHECK(Near(Shape(sphere, Vec3d(900.0, 0.0, 0.0), Quatd::Identity(), 0.5f), 0.75f));

    const SoundAttenuation box = Linear(SoundAttenuationShape::Box, Vec3(100.0f, 200.0f, 300.0f));
    CHECK(Near(Shape(box, Vec3d(600.0, 0.0, 0.0)), 0.5f));
    CHECK(Near(Shape(box, Vec3d(0.0, 0.0, 1400.0)), 0.0f));
    // 차단 경계에는 거리 배수가 들어가지 않는다(UE Attenuation.cpp:263).
    CHECK(Near(Shape(box, Vec3d(0.0, 0.0, 1400.0), Quatd::Identity(), 0.5f), 0.0f));
    CHECK(Near(Shape(box, Vec3d(0.0, 0.0, 1200.0), Quatd::Identity(), 0.5f), 0.55f));
    // NaturalSound Hold도 falloff 밖은 0이다.
    SoundAttenuation hold = box;
    hold.distance.model = audio::DistanceAttenuationModel::NaturalSound;
    hold.distance.naturalSoundFalloffMode = audio::NaturalSoundFalloffMode::Hold;
    CHECK(Near(Shape(hold, Vec3d(0.0, 0.0, 1400.0)), 0.0f));

    // 지역 X축으로만 긴 상자: 90° 돌리면 world Y축이 지역 X축이다.
    const SoundAttenuation thin = Linear(SoundAttenuationShape::Box, Vec3(100.0f, 0.0f, 0.0f));
    CHECK(Near(Shape(thin, Vec3d(0.0, 600.0, 0.0)), 0.4f));
    CHECK(Near(Shape(thin, Vec3d(0.0, 600.0, 0.0), Yaw(90.0)), 0.5f, 1.0e-4f));
}

// Capsule: 반높이 ≤ 반지름이면 구, 아니면 Z축 선분까지 거리 − 반지름(안쪽 음수 — Custom 곡선에는
// 그대로 간다).
TEST(EngineAudio_AttenuationCapsule) {
    SystemAllocator allocator;
    const SoundAttenuation capsule =
        Linear(SoundAttenuationShape::Capsule, Vec3(500.0f, 100.0f, 0.0f));
    CHECK(Near(Shape(capsule, Vec3d(0.0, 0.0, 300.0)), 1.0f));
    CHECK(Near(Shape(capsule, Vec3d(300.0, 0.0, 0.0)), 0.8f));
    CHECK(Near(Shape(capsule, Vec3d(0.0, 0.0, 700.0)), 0.8f));
    // yaw는 Z축을 바꾸지 않는다. Y축으로 90° 돌리면 축이 world X라 (300, 0, 0)은 선분 위다(−100 →
    // 1).
    CHECK(Near(Shape(capsule, Vec3d(300.0, 0.0, 0.0), Yaw(90.0)), 0.8f, 1.0e-4f));
    auto pitched = Quatd::TryFromAxisAngle(Vec3d(0.0, 1.0, 0.0), math::Pi<double> / 2.0);
    REQUIRE(pitched);
    CHECK(Near(Shape(capsule, Vec3d(300.0, 0.0, 0.0), pitched.Value()), 1.0f, 1.0e-4f));
    CHECK(Near(Shape(capsule, Vec3d(0.0, 0.0, 300.0), pitched.Value()), 0.8f, 1.0e-4f));

    const SoundAttenuation round =
        Linear(SoundAttenuationShape::Capsule, Vec3(100.0f, 200.0f, 0.0f));
    CHECK(Near(Shape(round, Vec3d(500.0, 0.0, 0.0)), 0.7f));

    SoundAttenuation custom = capsule;
    custom.distance.model = audio::DistanceAttenuationModel::Custom;
    custom.customDistanceCurve = MakeCurve(allocator, {Vec3(-1.0f, 0.2f, 0), Vec3(0.0f, 0.8f, 0)});
    REQUIRE(custom.customDistanceCurve.Get() != nullptr);
    // 안쪽 100 cm → 정규화 −0.1 → 0.2 + 0.6 × 0.9 = 0.74(0으로 자르면 0.8).
    CHECK(Near(Shape(custom, Vec3d(0.0, 0.0, 300.0)), 0.74f, 1.0e-4f));
}

// Cone: 반경·반각·각도 falloff, 구 부분, coneSphereRadius ≥ X, coneOffset, 원뿔 원점(θ = 0).
TEST(EngineAudio_AttenuationCone) {
    SoundAttenuation cone = Linear(SoundAttenuationShape::Cone, Vec3(1000.0f, 30.0f, 30.0f));
    CHECK(Near(Shape(cone, Vec3d(500.0, 0.0, 0.0)), 1.0f));
    const double diagonal = 500.0 / math::Sqrt(2.0);
    CHECK(Near(Shape(cone, Vec3d(diagonal, diagonal, 0.0)), 0.5f, 1.0e-4f));
    CHECK(Near(Shape(cone, Vec3d(2000.0, 0.0, 0.0)), 0.0f));
    CHECK(Near(Shape(cone, Vec3d(-400.0, 0.0, 0.0)), 0.0f));
    CHECK(Near(Shape(cone, Vec3d()), 1.0f));
    // 돌린 원뿔은 전방이 world Y다.
    CHECK(Near(Shape(cone, Vec3d(0.0, 500.0, 0.0), Yaw(90.0)), 1.0f, 1.0e-4f));
    CHECK(Near(Shape(cone, Vec3d(500.0, 0.0, 0.0), Yaw(90.0)), 0.0f, 1.0e-4f));

    SoundAttenuation sphere = cone;
    sphere.coneSphereRadius = 300.0f;
    sphere.coneSphereFalloffDistance = 200.0f;
    CHECK(Near(Shape(sphere, Vec3d(-400.0, 0.0, 0.0)), 0.5f));
    sphere.coneSphereRadius = 1000.0f;
    CHECK(Near(Shape(sphere, Vec3d(500.0, 0.0, 0.0)), 1.0f));
    CHECK(Near(Shape(sphere, Vec3d(1100.0, 0.0, 0.0)), 0.5f));

    SoundAttenuation offset = cone;
    offset.coneOffset = 200.0f;
    CHECK(Near(Shape(offset, Vec3d(-150.0, 0.0, 0.0)), 1.0f));
}

// 최대 가청 거리: 모양 반경, Box √2(자른 뒤), Cone은 구 부분을 보지 않음, Custom 마지막 키, focus
// 거리 배수로 나눔.
TEST(EngineAudio_AttenuationMaxAudibleDistance) {
    SystemAllocator allocator;
    SoundAttenuation box = Linear(SoundAttenuationShape::Box, Vec3(100.0f, 200.0f, 300.0f));
    box.distance.falloffDistance = 3600.0f;
    CHECK(MaxAudibleDistance(box) == 3900.0f);
    CHECK(IsWithinAudibleRange(box, Vec3d(), Vec3d(5500.0, 0.0, 0.0)));
    CHECK(!IsWithinAudibleRange(box, Vec3d(), Vec3d(5520.0, 0.0, 0.0)));
    SoundAttenuation capsule = Linear(SoundAttenuationShape::Capsule, Vec3(500.0f, 100.0f, 0.0f));
    capsule.distance.falloffDistance = 3600.0f;
    CHECK(MaxAudibleDistance(capsule) == 4100.0f);
    SoundAttenuation cone = Linear(SoundAttenuationShape::Cone, Vec3(400.0f, 30.0f, 30.0f));
    cone.distance.falloffDistance = 3600.0f;
    cone.coneSphereRadius = 5000.0f;
    CHECK(MaxAudibleDistance(cone) == 4000.0f);

    SoundAttenuation custom = Linear(SoundAttenuationShape::Sphere, Vec3(400.0f, 0, 0));
    custom.distance.falloffDistance = 3600.0f;
    custom.distance.model = audio::DistanceAttenuationModel::Custom;
    custom.customDistanceCurve =
        MakeCurve(allocator, {Vec3(0.0f, 1.0f, 0), Vec3(0.5f, 0.0005f, 0)});
    CHECK(MaxAudibleDistance(custom) == 2200.0f);
    custom.customDistanceCurve = MakeCurve(allocator, {Vec3(0.0f, 1.0f, 0), Vec3(0.5f, 0.5f, 0)});
    CHECK(MaxAudibleDistance(custom) == MaxAudioWorldDistance);
    custom.customDistanceCurve = MakeCurve(allocator, {});
    CHECK(MaxAudibleDistance(custom) == 400.0f);
    custom.customDistanceCurve = MakeCurve(allocator, {Vec3(0.0f, 1.0f, 0)});
    CHECK(MaxAudibleDistance(custom) == 400.0f);

    SoundAttenuation sphere = Linear(SoundAttenuationShape::Sphere, Vec3(400.0f, 0, 0));
    sphere.distance.falloffDistance = 3600.0f;
    CHECK(IsWithinAudibleRange(sphere, Vec3d(), Vec3d(7000.0, 0.0, 0.0), MaxAudioWorldDistance,
                               0.5f));
    CHECK(!IsWithinAudibleRange(sphere, Vec3d(), Vec3d(8500.0, 0.0, 0.0), MaxAudioWorldDistance,
                                0.5f));
    // world 최대 판정은 focus 배수로 나눈 뒤다(UE LocationIsAudible): Hold 상자는 √2배가 world
    // 최대를 넘어 늘 들리지만, focus 배수 2면 0.707배라 거리로 본다.
    SoundAttenuation hold = box;
    hold.distance.model = audio::DistanceAttenuationModel::NaturalSound;
    hold.distance.naturalSoundFalloffMode = audio::NaturalSoundFalloffMode::Hold;
    CHECK(Near(AudibleRange(hold), MaxAudioWorldDistance * 1.41421356f, 1.0f));
    CHECK(IsWithinAudibleRange(hold, Vec3d(), Vec3d(1.9e6, 0.0, 0.0)));
    CHECK(
        !IsWithinAudibleRange(hold, Vec3d(), Vec3d(1.9e6, 0.0, 0.0), MaxAudioWorldDistance, 2.0f));
    CHECK(IsWithinAudibleRange(hold, Vec3d(), Vec3d(1.4e6, 0.0, 0.0), MaxAudioWorldDistance, 2.0f));
}

// Custom 곡선: X축은 falloff로 정규화한 거리, 거리 배수 반영, [0, 1]로 자름, 키 범위 밖은 외삽,
// 키 없음은 defaultValue(없으면 0).
TEST(EngineAudio_AttenuationCustomCurve) {
    SystemAllocator allocator;
    SoundAttenuation custom = Linear(SoundAttenuationShape::Sphere, Vec3(0.0f, 0, 0));
    custom.distance.model = audio::DistanceAttenuationModel::Custom;
    custom.customDistanceCurve = MakeCurve(allocator, {Vec3(0.0f, 2.0f, 0), Vec3(1.0f, -1.0f, 0)});
    CHECK(Near(Shape(custom, Vec3d(100.0, 0.0, 0.0)), 1.0f));
    CHECK(Near(Shape(custom, Vec3d(500.0, 0.0, 0.0)), 0.5f));
    CHECK(Near(Shape(custom, Vec3d(250.0, 0.0, 0.0), Quatd::Identity(), 2.0f), 0.5f));
    CHECK(Near(Shape(custom, Vec3d(5000.0, 0.0, 0.0)), 0.0f));
    // 키 범위 밖은 곡선 외삽이다: 뒤 외삽 Linear면 (0, 1)·(1, 0.5) 곡선의 1.5는 0.25(Constant면
    // 0.5).
    {
        auto made = SharedPtr<TangentCurve>::TryMake(allocator, allocator);
        REQUIRE(made);
        TangentCurve& curve = *made.Value().Get();
        for (const Vec3& key : {Vec3(0.0f, 1.0f, 0), Vec3(1.0f, 0.5f, 0)}) {
            auto added = curve.TryAddKey(key.x, key.y);
            REQUIRE(added);
            REQUIRE(curve.TrySetKeyInterp(added.Value(), CurveInterp::Linear));
        }

        curve.postExtrapolation = CurveExtrapolation::Linear;
        custom.customDistanceCurve = made.Value();
        CHECK(Near(Shape(custom, Vec3d(1500.0, 0.0, 0.0)), 0.25f, 1.0e-4f));
        curve.postExtrapolation = CurveExtrapolation::Constant;
        CHECK(Near(Shape(custom, Vec3d(1500.0, 0.0, 0.0)), 0.5f, 1.0e-4f));
    }

    custom.customDistanceCurve = MakeCurve(allocator, {});
    CHECK(Near(Shape(custom, Vec3d(500.0, 0.0, 0.0)), 0.0f));
}

// 공기 흡수: 같은 주파수·계단·선형·로그·곡선(순서 없는 범위), [20, 20000].
TEST(EngineAudio_AttenuationAbsorption) {
    SystemAllocator allocator;
    SoundAttenuation air;
    air.attenuateWithLowPass = true;
    air.lowPassFrequencyAtMin = 20000.0f;
    air.lowPassFrequencyAtMax = 2000.0f;
    CHECK(Near(AbsorptionFrequency(air, 4500.0f, false), 11000.0f, 0.01f));
    CHECK(Near(AbsorptionFrequency(air, 1000.0f, false), 20000.0f));
    CHECK(Near(AbsorptionFrequency(air, 9000.0f, false), 2000.0f));
    air.logFrequencyScaling = true;
    CHECK(Near(AbsorptionFrequency(air, 4500.0f, false), 6324.555f, 0.05f));
    air.logFrequencyScaling = false;
    air.absorptionDistanceMax = air.absorptionDistanceMin;
    CHECK(Near(AbsorptionFrequency(air, 3001.0f, false), 2000.0f));
    CHECK(Near(AbsorptionFrequency(air, 3000.0f, false), 20000.0f));
    // 구간이 뒤집히면 스냅숏 검사가 거부한다(3.42.1절 — UE는 받고 로그 사상이 깨진다).
    air.absorptionDistanceMax = air.absorptionDistanceMin - 1.0f;
    CHECK(ValidateSoundAttenuation(air).Error() == ErrorCode::InvalidArgument);
    air.absorptionDistanceMax = DefaultAbsorptionDistanceMax;
    CHECK(ValidateSoundAttenuation(air));
    // HPF 기본(0 → 0)은 하한 20으로 자른다.
    CHECK(Near(AbsorptionFrequency(air, 4500.0f, true), 20.0f));
    air.highPassFrequencyAtMax = 1000.0f;
    CHECK(Near(AbsorptionFrequency(air, 4500.0f, true), 500.0f, 0.01f));
    air.absorptionMethod = SoundAbsorptionMethod::CustomCurve;
    air.customLowPassAbsorptionCurve =
        MakeCurve(allocator, {Vec3(0.0f, 0.0f, 0), Vec3(1.0f, 1.0f, 0)});
    CHECK(Near(AbsorptionFrequency(air, 4500.0f, false), 11000.0f, 0.01f));
    CHECK(Near(AbsorptionFrequency(air, 6000.0f, false), 20000.0f));
}

// 우선순위 거리 감쇠: Linear·Manual·곡선, 꺼짐 = 1.
TEST(EngineAudio_AttenuationPriorityScale) {
    SystemAllocator allocator;
    SoundAttenuation priority;
    CHECK(PriorityAttenuationScale(priority, 2200.0f) == 1.0f);
    priority.enablePriorityAttenuation = true;
    priority.priorityAttenuationMax = 0.2f;
    CHECK(Near(PriorityAttenuationScale(priority, 2200.0f), 0.6f));
    CHECK(Near(PriorityAttenuationScale(priority, 100.0f), 1.0f));
    CHECK(Near(PriorityAttenuationScale(priority, 9000.0f), 0.2f));
    priority.priorityAttenuationMethod = SoundPriorityAttenuationMethod::Manual;
    priority.manualPriorityAttenuation = 0.3f;
    CHECK(Near(PriorityAttenuationScale(priority, 9000.0f), 0.3f));
    priority.priorityAttenuationMethod = SoundPriorityAttenuationMethod::CustomCurve;
    priority.customPriorityAttenuationCurve =
        MakeCurve(allocator, {Vec3(0.0f, 3.0f, 0), Vec3(1.0f, 1.0f, 0)});
    // 위쪽은 자르지 않는다.
    CHECK(Near(PriorityAttenuationScale(priority, 2200.0f), 2.0f));
}

// focus: 방위각(같은 위치 90°), 계수 식(fa = nfa 포함)·전역 방위각 배수, 배수 lerp, 보간.
TEST(EngineAudio_AttenuationFocus) {
    CHECK(Near(FocusAzimuthDegrees(Vec3d(), Quatd::Identity(), Vec3d(0.0, 100.0, 0.0)), 90.0f));
    CHECK(Near(FocusAzimuthDegrees(Vec3d(), Quatd::Identity(), Vec3d(100.0, 0.0, 0.0)), 0.0f));
    CHECK(Near(FocusAzimuthDegrees(Vec3d(), Quatd::Identity(), Vec3d()), 90.0f));
    CHECK(Near(FocusAzimuthDegrees(Vec3d(), Yaw(90.0), Vec3d(0.0, 100.0, 0.0)), 0.0f, 1.0e-3f));

    SoundAttenuation focus;
    const GlobalFocusSettings global;
    CHECK(Near(FocusFactor(focus, global, 45.0f), 0.5f));
    CHECK(FocusFactor(focus, global, 10.0f) == 0.0f);
    CHECK(FocusFactor(focus, global, 90.0f) == 1.0f);
    GlobalFocusSettings wide;
    wide.focusAzimuthScale = 2.0f;
    wide.nonFocusAzimuthScale = 2.0f;
    CHECK(Near(FocusFactor(focus, wide, 90.0f), 0.5f));
    focus.nonFocusAzimuth = focus.focusAzimuth;
    CHECK(FocusFactor(focus, global, 30.0f) == 1.0f);
    CHECK(FocusFactor(focus, global, 29.0f) == 0.0f);

    SoundAttenuation scales;
    scales.nonFocusVolumeAttenuation = 0.5f;
    CHECK(Near(ComputeFocusScales(scales, global, 0.5f).volume, 0.75f));
    GlobalFocusSettings quiet;
    quiet.nonFocusVolumeScale = 0.5f;
    CHECK(Near(ComputeFocusScales(scales, quiet, 0.5f).volume, 0.625f));
    CHECK(ComputeFocusScales(scales, global, 1.0f).distance == 1.0f);

    CHECK(Near(InterpolateFocusFactor(1.0f, 0.0f, 0.1f, 5.0f, 1.0f), 0.5f));
    CHECK(Near(InterpolateFocusFactor(0.0f, 1.0f, 0.1f, 5.0f, 1.0f), 0.1f));
    CHECK(InterpolateFocusFactor(0.0f, 1.0f, 0.1f, 5.0f, 0.0f) == 1.0f);
    CHECK(InterpolateFocusFactor(0.5f, 0.50005f, 0.1f, 1.0f, 1.0f) == 0.50005f);
    CHECK(IsValidGlobalFocusSettings(global));
    GlobalFocusSettings negative;
    negative.focusVolumeScale = -1.0f;
    CHECK(!IsValidGlobalFocusSettings(negative));
}

// 제어 쪽: 고정 위치 재생의 회전이 상자 감쇠 축이 된다(지역 X로만 긴 상자, 리스너는 world Y 600).
TEST(EngineAudio_AttenuationRotatedBoxPlayback) {
    SoundAttenuation thin = Linear(SoundAttenuationShape::Box, Vec3(100.0f, 0.0f, 0.0f));
    thin.spatialize = false;
    CHECK(Near(PlayedPeak(thin, Vec3d(0.0, 600.0, 0.0)), DirectLevel * 0.4f, 1.0e-4f));
    CHECK(Near(PlayedPeak(thin, Vec3d(0.0, 600.0, 0.0), Yaw(90.0)), DirectLevel * 0.5f, 1.0e-4f));
}

// 제어 쪽 focus 볼륨: 뒤(방위각 180°, 계수 1)는 nonFocus 볼륨, 앞은 focus 볼륨, 전역 배수가
// 곱해진다. 전역 설정의 음수는 거부한다.
TEST(EngineAudio_AttenuationFocusVolumePlayback) {
    SoundAttenuation focus = Linear(SoundAttenuationShape::Sphere, Vec3(100000.0f, 0, 0));
    focus.spatialize = false;
    focus.enableListenerFocus = true;
    focus.nonFocusVolumeAttenuation = 0.5f;
    CHECK(Near(PlayedPeak(focus, Vec3d(500.0, 0.0, 0.0)), DirectLevel * 0.5f, 1.0e-4f));
    CHECK(Near(PlayedPeak(focus, Vec3d(-500.0, 0.0, 0.0)), DirectLevel, 1.0e-4f));
    GlobalFocusSettings quiet;
    quiet.nonFocusVolumeScale = 0.5f;
    CHECK(Near(PlayedPeak(focus, Vec3d(500.0, 0.0, 0.0), Quatd::Identity(), quiet),
               DirectLevel * 0.25f, 1.0e-4f));
    focus.enableListenerFocus = false;
    CHECK(Near(PlayedPeak(focus, Vec3d(500.0, 0.0, 0.0)), DirectLevel, 1.0e-4f));

    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    GlobalFocusSettings negative;
    negative.focusDistanceScale = -1.0f;
    CHECK(fixture.audio->SetGlobalFocusSettings(negative).Error() == ErrorCode::InvalidArgument);
    CHECK(fixture.audio->FocusSettings().focusDistanceScale == 1.0f);
}

// 제어 쪽 공기 흡수: HPF 2 kHz는 상수(DC) 출력을 없앤다. 기본값(흡수 끔)은 그대로다.
TEST(EngineAudio_AttenuationAbsorptionPlayback) {
    SoundAttenuation air = Linear(SoundAttenuationShape::Sphere, Vec3(100000.0f, 0, 0));
    air.spatialize = false;
    CHECK(Near(PlayedPeak(air, Vec3d(500.0, 0.0, 0.0)), DirectLevel, 1.0e-4f));
    air.attenuateWithLowPass = true;
    air.highPassFrequencyAtMin = 2000.0f;
    air.highPassFrequencyAtMax = 2000.0f;
    CHECK(PlayedPeak(air, Vec3d(500.0, 0.0, 0.0)) < 1.0e-3f);
}

// 제어 쪽 우선순위 감쇠: StopLowestPriority(최대 1) 그룹에서 먼 재생은 동시 발음 우선순위 P_c =
// clamp(max(1, P × pa) × fps, 0, 100) × P가 낮아 cull된다(P = 10: 가까움 100, 3900 cm는 pa 0.125
// → 12.5). 감쇠가 없으면 동률이라 오래된 쪽이 멈춘다.
TEST(EngineAudio_AttenuationPriorityCull) {
    for (const bool attenuated : {true, false}) {
        AudioEngine fixture(DeterministicOptions(true));
        REQUIRE(fixture.initialized);
        const AudioSceneId scene = fixture.audio->RegisterScene();
        const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.2f, TestRate);
        // 우선순위 감쇠가 동시 발음 우선순위에 닿는 것은 focus 블록(spatialize 또는 focus)이 돌
        // 때뿐이다 (UE ActiveSound.cpp:2105–2122).
        SoundAttenuation attenuation;
        attenuation.enablePriorityAttenuation = attenuated;
        attenuation.priorityAttenuationMax = 0.1f;
        SoundConcurrencyEntry lowest;
        lowest.key = 501;
        lowest.settings.resolutionRule = SoundConcurrencyRule::StopLowestPriority;
        lowest.settings.maxCount = 1;
        const SoundSourceRef sound =
            Sound(fixture.allocator, pcm, attenuation, SoundVirtualizationMode::Disabled, {lowest});
        EventLog log(fixture.allocator);
        PlayOptions nearOptions;
        nearOptions.priority = 10.0f;
        nearOptions.onEvent = log.Callback(fixture.allocator);
        auto nearPlay =
            fixture.audio->PlayAtInScene(scene, sound, Vec3d(100.0, 0.0, 0.0), Move(nearOptions));
        PlayOptions farOptions;
        farOptions.priority = 10.0f;
        farOptions.onEvent = log.Callback(fixture.allocator);
        auto farPlay =
            fixture.audio->PlayAtInScene(scene, sound, Vec3d(3900.0, 0.0, 0.0), Move(farOptions));
        REQUIRE(nearPlay && farPlay);
        REQUIRE(fixture.Frames(StartFrames));
        const PlaybackHandle stopped = attenuated ? farPlay.Value() : nearPlay.Value();
        const PlaybackHandle kept = attenuated ? nearPlay.Value() : farPlay.Value();
        CHECK(fixture.audio->Status(stopped) == PlaybackStatus::Finished);
        CHECK(fixture.audio->Status(kept) != PlaybackStatus::Finished);
        REQUIRE(fixture.audio->Stop(kept, 0));
        (void)fixture.RunUntilIdle();
    }
}

// stereo spread(3.42.6절): 좌우 방위각 = 방위각 ∓ deg(atan(0.5 × spread ÷ D)), 영거리는 L 270°·R
// 90°, spatialize 끔은 Direct, 비공간화 모드는 믹서 omni 배치로 간다.
TEST(EngineAudio_AttenuationStereoSpread) {
    SoundAttenuation stereo;
    stereo.shapeExtents.x = 100000.0f;
    SpatialParams params =
        ComputeSpatialParams(stereo, 2, Vec3d(), Quatd::Identity(), Vec3d(1000.0, 0.0, 0.0));
    CHECK(params.azimuthPan);
    CHECK(Near(params.azimuthDegrees, 354.28940f, 1.0e-3f));
    CHECK(Near(params.rightAzimuthDegrees, 5.71059f, 1.0e-3f));
    params = ComputeSpatialParams(stereo, 2, Vec3d(), Quatd::Identity(), Vec3d(0.0, 1000.0, 0.0));
    CHECK(Near(params.azimuthDegrees, 84.28941f, 1.0e-3f));
    CHECK(Near(params.rightAzimuthDegrees, 95.71059f, 1.0e-3f));
    params = ComputeSpatialParams(stereo, 2, Vec3d(), Quatd::Identity(), Vec3d());
    CHECK(params.azimuthDegrees == 270.0f && params.rightAzimuthDegrees == 90.0f);
    stereo.nonSpatializedMode = SoundNonSpatializedMode::Direct2D;
    params = ComputeSpatialParams(stereo, 1, Vec3d(), Quatd::Identity(), Vec3d(0.0, 1000.0, 0.0));
    CHECK(params.omniMode == audio::AudioOmniMode::Direct2D);
    CHECK(Near(params.azimuthDegrees, 90.0f, 1.0e-3f));
    stereo.spatialize = false;
    CHECK(!ComputeSpatialParams(stereo, 2, Vec3d(), Quatd::Identity(), Vec3d(1000.0, 0.0, 0.0))
               .azimuthPan);
}

} // namespace audio_attenuation_tests_file
