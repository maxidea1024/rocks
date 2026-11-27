#include "AudioEngineTestSupport.h"

#include <Config/Tests/ConfigTestSupport.h>
#include <Core/Containers/Array.h>
#include <Core/Math/MathUtility.h>
#include <EngineAudio/AudioEngineSettings.h>
#include <EngineAudio/AudioOcclusionQuery.h>
#include <TestSupport/Test.h>

// AU7d 차폐 목표값 입력 시험이다(Docs/Design/AudioSystem 3.42.7절, 물리 질의 PR16 전 fixture): Play
// 때 첫 검사는 바로(보간 없음), 이후 간격마다 제어 쪽 요청 → 게임 쪽 공급자 → 명령 왕복, 차폐가
// 풀리면 보간해 돌아옴, 공급자 없음은 차폐되지 않음, audio.occlusion.disable. Inline 제어 + Manual
// 렌더로 프레임·Pump만으로 순서를 만든다.
using namespace rocks;
namespace audio_occlusion_tests_file {

using namespace rocks::engine_audio_test;

namespace {

// 재생이 시작해 출력이 고르게 될 때까지의 프레임 수와 mono 상수 0.2의 Direct 출력(0.5배)이다.
constexpr uint32 StartFrames = 6;
constexpr float DirectLevel = 0.1f;
// 검사 간격(0.1 s = 4800 frame)을 넉넉히 넘기는 프레임 수다(묶음 512 frame).
constexpr uint32 IntervalFrames = 12;

class FixtureQuery final : public AudioOcclusionQuery {
public:
    bool IsOccluded(const Vec3d& source, const Vec3d& listener, uint64 owner) noexcept override {
        (void)source;
        (void)listener;
        (void)owner;
        ++calls;
        return occluded;
    }

    bool occluded = false;
    uint32 calls = 0;
};

SoundAttenuation Occluding() noexcept {
    SoundAttenuation attenuation;
    attenuation.spatialize = false;
    attenuation.shapeExtents.x = 100000.0f;
    attenuation.enableOcclusion = true;
    attenuation.occlusionVolumeAttenuation = 0.5f;
    return attenuation;
}

SoundSourceRef Sound(Allocator& allocator, const SharedPtr<const SoundPcm>& pcm,
                     const SoundAttenuation& attenuation) noexcept {
    SoundPlayable playable;
    playable.pcm = pcm;
    playable.looping = true;
    playable.attenuation = attenuation;
    auto made = SoundSourceRef::TryCreate(allocator, playable);
    return made ? made.Value() : SoundSourceRef{};
}

float Peak(AudioEngine& fixture) noexcept {
    Array<float> chunk(fixture.allocator);
    if (!chunk.TryResize(static_cast<usize>(PumpFrames) * 2)) {
        return -1.0f;
    }

    return CapturePeak(fixture, chunk).left;
}

bool Near(float actual, float expected) noexcept {
    return math::Abs(actual - expected) <= 1.0e-4f;
}

} // namespace

// Play 때 첫 검사는 동기이고 보간 없이 바로 쓴다(UE 첫 검사 — ActiveSound.cpp:1600–1615). 이후
// 간격마다 요청이 왕복하고, 차폐가 풀리면 보간해 1로 돌아온다.
TEST(EngineAudio_OcclusionFirstCheckAndInterval) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const AudioSceneId scene = fixture.audio->RegisterScene();
    FixtureQuery query;
    query.occluded = true;
    fixture.audio->SetSceneOcclusionQuery(scene, &query);
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.2f, TestRate);
    auto played = fixture.audio->PlayAtInScene(scene, Sound(fixture.allocator, pcm, Occluding()),
                                               Vec3d(500.0, 0.0, 0.0));
    REQUIRE(played);
    CHECK(query.calls == 1);
    REQUIRE(fixture.Frames(StartFrames));
    CHECK(Near(Peak(fixture), DirectLevel * 0.5f));

    // 간격 0.1 s = 4800 frame: 묶음 512 frame이면 수락 뒤 10번째 묶음에서 처음 넘는다(> — 9번째는
    // 4608). 그 요청은 다음 프레임 시작에 답한다. 수락에서 18 프레임 뒤에는 Play 때 1 + 요청 1이다.
    REQUIRE(fixture.Frames(IntervalFrames));
    CHECK(query.calls == 2);
    CHECK(fixture.audio->ControlStats().occlusionRequests == 1);
    query.occluded = false;
    // 다음 요청(간격) + 왕복 + 보간 0.1 s 뒤에는 1이다. 그 사이에 중간값을 지난다.
    bool between = false;
    float last = 0.0f;
    for (uint32 frame = 0; frame < IntervalFrames * 3; ++frame) {
        last = Peak(fixture);
        between = between || (last > DirectLevel * 0.5f + 1.0e-3f && last < DirectLevel - 1.0e-3f);
    }

    CHECK(between);
    CHECK(Near(last, DirectLevel));
    REQUIRE(fixture.audio->Stop(played.Value(), 0));
    (void)fixture.RunUntilIdle();
}

// 공급자가 없으면 차폐되지 않고(요청은 차폐되지 않음으로 답한다), 차폐를 쓰지 않는 감쇠는 요청하지
// 않는다.
TEST(EngineAudio_OcclusionWithoutProvider) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const AudioSceneId scene = fixture.audio->RegisterScene();
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.2f, TestRate);
    auto played = fixture.audio->PlayAtInScene(scene, Sound(fixture.allocator, pcm, Occluding()),
                                               Vec3d(500.0, 0.0, 0.0));
    REQUIRE(played);
    REQUIRE(fixture.Frames(StartFrames));
    CHECK(Near(Peak(fixture), DirectLevel));
    CHECK(fixture.audio->ControlStats().occlusionRequests >= 1);
    REQUIRE(fixture.audio->Stop(played.Value(), 0));
    (void)fixture.RunUntilIdle();

    SoundAttenuation plain = Occluding();
    plain.enableOcclusion = false;
    const uint64 before = fixture.audio->ControlStats().occlusionRequests;
    auto other = fixture.audio->PlayAtInScene(scene, Sound(fixture.allocator, pcm, plain),
                                              Vec3d(500.0, 0.0, 0.0));
    REQUIRE(other);
    REQUIRE(fixture.Frames(IntervalFrames));
    CHECK(fixture.audio->ControlStats().occlusionRequests == before);
    REQUIRE(fixture.audio->Stop(other.Value(), 0));
    (void)fixture.RunUntilIdle();
}

// focus 볼륨이 0이면 제어 쪽은 요청하지 않는다(UE ActiveSound.cpp:2178 — 첫 검사는 Play 때라 본다).
TEST(EngineAudio_OcclusionSkipsSilentFocus) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const AudioSceneId scene = fixture.audio->RegisterScene();
    FixtureQuery query;
    fixture.audio->SetSceneOcclusionQuery(scene, &query);
    SoundAttenuation silent = Occluding();
    silent.enableListenerFocus = true;
    silent.focusVolumeAttenuation = 0.0f;
    silent.nonFocusVolumeAttenuation = 0.0f;
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.2f, TestRate);
    auto played = fixture.audio->PlayAtInScene(scene, Sound(fixture.allocator, pcm, silent),
                                               Vec3d(500.0, 0.0, 0.0));
    REQUIRE(played);
    REQUIRE(fixture.Frames(IntervalFrames * 2));
    CHECK(query.calls == 1);
    CHECK(fixture.audio->ControlStats().occlusionRequests == 0);
    REQUIRE(fixture.audio->Stop(played.Value(), 0));
    (void)fixture.RunUntilIdle();
}

// audio.occlusion.disable = 1이면 질의하지 않고 차폐되지 않은 것으로 본다(UE au.DisableOcclusion).
TEST(EngineAudio_OcclusionDisableCVar) {
    test::config::ConfigFixture settings(test::config::DevelopmentRules());
    config::ConfigContext& context = settings.Context();
    REQUIRE(RegisterEngineAudioSettings(context));
    REQUIRE(context.AddAssignment(config::ConfigLayer::Session, "test", "audio.occlusion.disable",
                                  "1"));
    REQUIRE(context.Sync());
    REQUIRE(context.CompleteStartup());
    AudioEngine fixture(DeterministicOptions(true), &context);
    REQUIRE(fixture.initialized);
    const AudioSceneId scene = fixture.audio->RegisterScene();
    FixtureQuery query;
    query.occluded = true;
    fixture.audio->SetSceneOcclusionQuery(scene, &query);
    // 첫 프레임 끝에 설정을 읽는다(그 전 Play의 첫 검사는 기본 허용 — 프레임 하나를 먼저 돈다).
    REQUIRE(fixture.Frame());
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.2f, TestRate);
    auto played = fixture.audio->PlayAtInScene(scene, Sound(fixture.allocator, pcm, Occluding()),
                                               Vec3d(500.0, 0.0, 0.0));
    REQUIRE(played);
    REQUIRE(fixture.Frames(IntervalFrames));
    CHECK(query.calls == 0);
    CHECK(Near(Peak(fixture), DirectLevel));
    REQUIRE(fixture.audio->Stop(played.Value(), 0));
    (void)fixture.RunUntilIdle();
}

// audio.occlusion.checkinterval이 음수면 첫 검사(Play 때)만 한다.
TEST(EngineAudio_OcclusionNegativeIntervalChecksOnce) {
    test::config::ConfigFixture settings(test::config::DevelopmentRules());
    config::ConfigContext& context = settings.Context();
    REQUIRE(RegisterEngineAudioSettings(context));
    REQUIRE(context.AddAssignment(config::ConfigLayer::Session, "test",
                                  "audio.occlusion.checkinterval", "-1"));
    REQUIRE(context.Sync());
    REQUIRE(context.CompleteStartup());
    AudioEngine fixture(DeterministicOptions(true), &context);
    REQUIRE(fixture.initialized);
    const AudioSceneId scene = fixture.audio->RegisterScene();
    FixtureQuery query;
    query.occluded = true;
    fixture.audio->SetSceneOcclusionQuery(scene, &query);
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.2f, TestRate);
    auto played = fixture.audio->PlayAtInScene(scene, Sound(fixture.allocator, pcm, Occluding()),
                                               Vec3d(500.0, 0.0, 0.0));
    REQUIRE(played);
    REQUIRE(fixture.Frames(IntervalFrames * 3));
    CHECK(query.calls == 1);
    CHECK(fixture.audio->ControlStats().occlusionRequests == 0);
    CHECK(Near(Peak(fixture), DirectLevel * 0.5f));
    REQUIRE(fixture.audio->Stop(played.Value(), 0));
    (void)fixture.RunUntilIdle();
}

} // namespace audio_occlusion_tests_file
