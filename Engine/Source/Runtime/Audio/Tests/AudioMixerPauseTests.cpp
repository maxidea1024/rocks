#include <Audio/OfflineAudioRender.h>
#include <Core/Containers/Array.h>
#include <Core/Math/MathUtility.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/Fatal.h>
#include <TestSupport/Test.h>

// AU7e2 믹서 일시정지 시험이다(Docs/Design/AudioSystem 3.44.5·3.44.8절): 64 frame fade·원본 멈춤·
// 이어 내기, fade 중 되돌림, 시작 미룸과 최신 파라미터, 일시정지 중 정지(꼬리 없음), fade 중 자연
// 끝, 읽지 않은 누적 게시, seqlock의 블록 안 켜짐·꺼짐. ramp clip(값 = frame / RampScale)이라
// 출력에서 원본 위치를 읽는다. mono Direct 패닝은 양쪽 0.5다.
using namespace rocks;
namespace audio_mixer_pause_tests_file {

using namespace rocks::audio;

namespace {

constexpr uint32 Block = 256;
constexpr uint32 RampFrames = 8192;
constexpr float RampScale = 8192.0f;
constexpr float DirectGain = 0.5f;
constexpr float Tolerance = 1.0e-6f;

AudioMixerConfig ConfigOf() noexcept {
    AudioMixerConfig config;
    config.blockFrames = Block;
    config.masterLimiterEnabled = false;
    config.maxVoices = 8;
    config.maxClips = 4;
    return config;
}

OfflineAudioRender MakeRig(Allocator& allocator) noexcept {
    auto created = OfflineAudioRender::TryCreate(ConfigOf(), allocator);
    if (!created) {
        platform::Fatal("Audio pause test configuration was rejected");
    }

    return Move(created.Value());
}

struct Ramp {
    Ramp() noexcept {
        for (uint32 frame = 0; frame < RampFrames; ++frame) {
            values[frame] = static_cast<float>(frame) / RampScale;
        }
    }

    float values[RampFrames];
};

AudioClipId RegisterRamp(AudioMixer& mixer, const Ramp& ramp, uint32 frames = RampFrames) noexcept {
    AudioClipDesc desc;
    desc.channels[0] = ramp.values;
    desc.channelCount = 1;
    desc.frameCount = frames;
    desc.sampleRate = DefaultMixSampleRate;
    auto registered = mixer.RegisterClip(desc);
    if (!registered) {
        platform::Fatal("Audio pause test clip was rejected");
    }

    return registered.Value();
}

void SubmitOrDie(OfflineAudioRender& rig, const AudioRtCommand& command) noexcept {
    if (!rig.Submit(command)) {
        platform::Fatal("Audio pause test command was rejected");
    }
}

AudioVoiceParams Paused(bool paused = true) noexcept {
    AudioVoiceParams params;
    params.paused = paused;
    return params;
}

AudioVoiceHandle PlayAt(OfflineAudioRender& rig, AudioClipId clip, uint64 frame,
                        const AudioVoiceParams& params = AudioVoiceParams{}) noexcept {
    auto voice = rig.Mixer().AcquireVoice();
    if (!voice) {
        platform::Fatal("Audio pause test voice was not acquired");
    }

    auto play = rig.Mixer().PreparePlay(voice.Value(), clip, 0, false, params);
    if (!play) {
        platform::Fatal("Audio pause test play was rejected");
    }

    play.Value().mixFrame = frame;
    SubmitOrDie(rig, play.Value());
    return voice.Value();
}

void ParamsAt(OfflineAudioRender& rig, AudioVoiceHandle voice, uint64 frame,
              const AudioVoiceParams& params, uint32 rampFrames = 0) noexcept {
    auto command = rig.Mixer().MakeSetParams(voice, frame, params, rampFrames);
    if (!command) {
        platform::Fatal("Audio pause test parameters were rejected");
    }

    SubmitOrDie(rig, command.Value());
}

void StopAt(OfflineAudioRender& rig, AudioVoiceHandle voice, uint64 frame,
            uint32 fadeFrames) noexcept {
    auto command = rig.Mixer().MakeStop(voice, frame, fadeFrames);
    if (!command) {
        platform::Fatal("Audio pause test stop was rejected");
    }

    SubmitOrDie(rig, command.Value());
}

// blocks 블록을 렌더해 왼쪽 채널을 out 뒤에 붙인다(색인 = mix frame).
void Render(OfflineAudioRender& rig, uint32 blocks, Array<float>& out) noexcept {
    float block[Block * 2] = {};
    for (uint32 index = 0; index < blocks; ++index) {
        REQUIRE(rig.Render(Span<float>(block, Block * 2), 1));
        for (uint32 frame = 0; frame < Block; ++frame) {
            CHECK(out.TryPushBack(block[frame * 2]));
        }
    }
}

AudioVoiceStatus StatusOf(const AudioMixer& mixer, AudioVoiceHandle voice) noexcept {
    auto status = mixer.QueryVoice(voice);
    if (!status) {
        platform::Fatal("Audio pause test voice query failed");
    }

    return status.Value();
}

// 원본 위치 position을 gain으로 낸 출력 값이다.
float Expected(uint64 position, float gain) noexcept {
    return DirectGain * (static_cast<float>(position) / RampScale) * gain;
}

bool Near(float actual, float expected) noexcept {
    return math::Abs(actual - expected) <= Tolerance;
}

} // namespace

// 켜지면 64 frame 동안 내리고(원본은 읽는다) 원본을 멈춘다. 풀면 멈춘 위치에서 64 frame 동안
// 올린다. 읽지 않은 누적·Paused 표지·pausedVoices가 게시된다.
TEST(Audio_MixerPause_FadeHoldResume) {
    SystemAllocator allocator;
    static const Ramp ramp;
    OfflineAudioRender rig = MakeRig(allocator);
    const AudioClipId clip = RegisterRamp(rig.Mixer(), ramp);
    const AudioVoiceHandle voice = PlayAt(rig, clip, 0);
    ParamsAt(rig, voice, 100, Paused());
    Array<float> out(allocator);
    Render(rig, 2, out);
    for (uint32 frame = 0; frame < 100; ++frame) {
        CHECK(Near(out[frame], Expected(frame, 1.0f)));
    }

    for (uint32 k = 0; k < PauseFadeFrames; ++k) {
        CHECK(
            Near(out[100 + k], Expected(100 + k, 1.0f - static_cast<float>(k) / PauseFadeFrames)));
    }

    for (uint32 frame = 100 + PauseFadeFrames; frame < 2 * Block; ++frame) {
        CHECK(out[frame] == 0.0f);
    }

    const uint32 pausedAt = 100 + PauseFadeFrames;
    AudioVoiceStatus status = StatusOf(rig.Mixer(), voice);
    CHECK(status.pausedAtAsOf);
    CHECK(status.unreadAsOfFrame == 2 * Block);
    CHECK(status.unreadFrames == 2 * Block - pausedAt);
    CHECK(rig.Mixer().CountersSnapshot().pausedVoices == 1);

    ParamsAt(rig, voice, 1000, Paused(false));
    Render(rig, 6, out);
    for (uint32 frame = 2 * Block; frame < 1000; ++frame) {
        CHECK(out[frame] == 0.0f);
    }

    for (uint32 k = 0; k < PauseFadeFrames; ++k) {
        CHECK(Near(out[1000 + k], Expected(pausedAt + k, static_cast<float>(k) / PauseFadeFrames)));
    }

    const uint64 unread = 1000 - pausedAt;
    for (uint32 frame = 1000 + PauseFadeFrames; frame < 8 * Block; ++frame) {
        CHECK(Near(out[frame], Expected(frame - unread, 1.0f)));
    }

    status = StatusOf(rig.Mixer(), voice);
    CHECK(!status.pausedAtAsOf);
    CHECK(status.unreadFrames == unread);
    CHECK(rig.Mixer().CountersSnapshot().pausedVoices == 0);
}

// fade 중 되돌림: PausingOut 중 해제는 지금 gain에서 올리고(원본을 멈춘 적이 없다), ResumingIn 중
// 다시 켜면 지금 gain에서 내린다.
TEST(Audio_MixerPause_ReversesMidFade) {
    SystemAllocator allocator;
    static const Ramp ramp;
    OfflineAudioRender rig = MakeRig(allocator);
    const AudioClipId clip = RegisterRamp(rig.Mixer(), ramp);
    const AudioVoiceHandle voice = PlayAt(rig, clip, 0);
    ParamsAt(rig, voice, 100, Paused());
    ParamsAt(rig, voice, 132, Paused(false));
    Array<float> out(allocator);
    Render(rig, 1, out);
    // 132에서 gain 0.5 → 1을 32 frame에.
    for (uint32 k = 0; k < 32; ++k) {
        CHECK(Near(out[132 + k], Expected(132 + k, 0.5f + 0.5f * static_cast<float>(k) / 32.0f)));
    }

    for (uint32 frame = 164; frame < Block; ++frame) {
        CHECK(Near(out[frame], Expected(frame, 1.0f)));
    }

    CHECK(StatusOf(rig.Mixer(), voice).unreadFrames == 0);

    // 300 켜짐 → 316 꺼짐(gain 0.75) → 324 켜짐(gain 0.875에서 56 frame) → 380 Paused → 500 해제.
    ParamsAt(rig, voice, 300, Paused());
    ParamsAt(rig, voice, 316, Paused(false));
    ParamsAt(rig, voice, 324, Paused());
    ParamsAt(rig, voice, 500, Paused(false));
    Render(rig, 3, out);
    CHECK(Near(out[323], Expected(323, 0.75f + 0.25f * 7.0f / 16.0f)));
    CHECK(Near(out[324], Expected(324, 0.875f)));
    for (uint32 frame = 380; frame < 500; ++frame) {
        CHECK(out[frame] == 0.0f);
    }

    for (uint32 k = 0; k < PauseFadeFrames; ++k) {
        CHECK(Near(out[500 + k], Expected(380 + k, static_cast<float>(k) / PauseFadeFrames)));
    }

    CHECK(Near(out[4 * Block - 1], Expected(4 * Block - 1 - 120, 1.0f)));
    CHECK(StatusOf(rig.Mixer(), voice).unreadFrames == 120);
}

// 일시정지된 Play는 시작을 미룬다(시작 시각·출력 없음). 풀린 frame에서 최신 파라미터를 ramp 없이
// 맞추고 원본 처음부터 시작한다. 미룬 voice의 Stop은 바로 끝난다.
TEST(Audio_MixerPause_DefersStart) {
    SystemAllocator allocator;
    static const Ramp ramp;
    OfflineAudioRender rig = MakeRig(allocator);
    const AudioClipId clip = RegisterRamp(rig.Mixer(), ramp);
    const AudioVoiceHandle voice = PlayAt(rig, clip, 0, Paused());
    Array<float> out(allocator);
    Render(rig, 1, out);
    AudioVoiceStatus status = StatusOf(rig.Mixer(), voice);
    CHECK(status.state == AudioVoiceState::Prepared);
    CHECK(status.startMixFrame == UnsetMixFrame);
    CHECK(rig.Mixer().CountersSnapshot().pausedVoices == 1);

    AudioVoiceParams half;
    half.gain = 0.5f;
    ParamsAt(rig, voice, 300, half, DefaultParamRampFrames);
    Render(rig, 1, out);
    for (uint32 frame = 0; frame < 300; ++frame) {
        CHECK(out[frame] == 0.0f);
    }

    for (uint32 frame = 300; frame < 2 * Block; ++frame) {
        CHECK(Near(out[frame], Expected(frame - 300, 0.5f)));
    }

    status = StatusOf(rig.Mixer(), voice);
    CHECK(status.state == AudioVoiceState::Playing);
    CHECK(status.startMixFrame == 300);

    const AudioVoiceHandle stopped = PlayAt(rig, clip, 2 * Block, Paused());
    StopAt(rig, stopped, 2 * Block + 10, DefaultStopFadeFrames);
    Render(rig, 1, out);
    status = StatusOf(rig.Mixer(), stopped);
    CHECK(status.state == AudioVoiceState::Finished);
    CHECK(status.endReason == AudioVoiceEndReason::Stopped);
    CHECK(status.endMixFrame == 2 * Block + 10);
    CHECK(status.startMixFrame == UnsetMixFrame);
}

// 일시정지 중 정지: Paused voice의 Stop은 원본을 읽지 않고 fade 길이 뒤에 끝난다(해제가 와도 다시
// 읽지 않는다). PausingOut 중 Stop은 두 gain을 곱한다. 정지 중 voice도 일시정지되면 소리 없이
// fade를 마친다.
TEST(Audio_MixerPause_StopWhilePaused) {
    SystemAllocator allocator;
    static const Ramp ramp;
    {
        OfflineAudioRender rig = MakeRig(allocator);
        const AudioClipId clip = RegisterRamp(rig.Mixer(), ramp);
        const AudioVoiceHandle voice = PlayAt(rig, clip, 0);
        ParamsAt(rig, voice, 100, Paused());
        StopAt(rig, voice, 300, DefaultStopFadeFrames);
        ParamsAt(rig, voice, 400, Paused(false));
        Array<float> out(allocator);
        Render(rig, 4, out);
        for (uint32 frame = 100 + PauseFadeFrames; frame < 4 * Block; ++frame) {
            CHECK(out[frame] == 0.0f);
        }

        const AudioVoiceStatus status = StatusOf(rig.Mixer(), voice);
        CHECK(status.state == AudioVoiceState::Finished);
        CHECK(status.endReason == AudioVoiceEndReason::Stopped);
        CHECK(status.endMixFrame == 300 + DefaultStopFadeFrames);
    }

    {
        OfflineAudioRender rig = MakeRig(allocator);
        const AudioClipId clip = RegisterRamp(rig.Mixer(), ramp);
        const AudioVoiceHandle voice = PlayAt(rig, clip, 0);
        ParamsAt(rig, voice, 100, Paused());
        StopAt(rig, voice, 120, DefaultStopFadeFrames);
        Array<float> out(allocator);
        Render(rig, 3, out);
        for (uint32 frame = 120; frame < 100 + PauseFadeFrames; ++frame) {
            const float pause = 1.0f - static_cast<float>(frame - 100) / PauseFadeFrames;
            const float fade = 1.0f - static_cast<float>(frame - 120) / DefaultStopFadeFrames;
            CHECK(Near(out[frame], Expected(frame, pause * fade)));
        }

        for (uint32 frame = 100 + PauseFadeFrames; frame < 3 * Block; ++frame) {
            CHECK(out[frame] == 0.0f);
        }

        CHECK(StatusOf(rig.Mixer(), voice).endMixFrame == 120 + DefaultStopFadeFrames);
    }

    {
        OfflineAudioRender rig = MakeRig(allocator);
        const AudioClipId clip = RegisterRamp(rig.Mixer(), ramp);
        const AudioVoiceHandle voice = PlayAt(rig, clip, 0);
        StopAt(rig, voice, 100, DefaultStopFadeFrames);
        ParamsAt(rig, voice, 200, Paused());
        Array<float> out(allocator);
        Render(rig, 3, out);
        CHECK(out[199] > 0.0f);
        for (uint32 frame = 200 + PauseFadeFrames; frame < 3 * Block; ++frame) {
            CHECK(out[frame] == 0.0f);
        }

        CHECK(StatusOf(rig.Mixer(), voice).endMixFrame == 100 + DefaultStopFadeFrames);
    }
}

// fade 중 자연 끝: PausingOut 중 원본이 끝나면 지금처럼 NaturalEnd다. ResumingIn 중도 같다.
TEST(Audio_MixerPause_NaturalEndDuringFade) {
    SystemAllocator allocator;
    static const Ramp ramp;
    OfflineAudioRender rig = MakeRig(allocator);
    const AudioClipId shortClip = RegisterRamp(rig.Mixer(), ramp, 130);
    const AudioVoiceHandle pausing = PlayAt(rig, shortClip, 0);
    ParamsAt(rig, pausing, 100, Paused());
    const AudioClipId longerClip = RegisterRamp(rig.Mixer(), ramp, 150);
    const AudioVoiceHandle resuming = PlayAt(rig, longerClip, 0);
    ParamsAt(rig, resuming, 50, Paused());
    ParamsAt(rig, resuming, 300, Paused(false));
    Array<float> out(allocator);
    Render(rig, 2, out);
    AudioVoiceStatus status = StatusOf(rig.Mixer(), pausing);
    CHECK(status.endReason == AudioVoiceEndReason::NaturalEnd);
    CHECK(status.endMixFrame == 130);
    // 50 + 64 = 114에서 멈췄다. 300에서 이어 36 frame 남은 원본이 ResumingIn(64 frame) 안에서
    // 끝난다.
    status = StatusOf(rig.Mixer(), resuming);
    CHECK(status.endReason == AudioVoiceEndReason::NaturalEnd);
    CHECK(status.endMixFrame == 300 + (150 - 114));
}

// seqlock(PublishParams)은 블록 시작에 한 번 읽는다: 블록 사이에 켜고 끄면 마지막 값만 보여 멈추지
// 않는다. 켜진 값만 남으면 블록 시작부터 내린다.
TEST(Audio_MixerPause_SeqlockLastValueWins) {
    SystemAllocator allocator;
    static const Ramp ramp;
    OfflineAudioRender rig = MakeRig(allocator);
    const AudioClipId clip = RegisterRamp(rig.Mixer(), ramp);
    const AudioVoiceHandle voice = PlayAt(rig, clip, 0);
    Array<float> out(allocator);
    Render(rig, 1, out);
    REQUIRE(rig.Mixer().PublishParams(voice, Paused()));
    REQUIRE(rig.Mixer().PublishParams(voice, Paused(false)));
    Render(rig, 1, out);
    for (uint32 frame = Block; frame < 2 * Block; ++frame) {
        CHECK(Near(out[frame], Expected(frame, 1.0f)));
    }

    REQUIRE(rig.Mixer().PublishParams(voice, Paused()));
    Render(rig, 1, out);
    CHECK(Near(out[2 * Block + 1], Expected(2 * Block + 1, 1.0f - 1.0f / PauseFadeFrames)));
    CHECK(out[2 * Block + PauseFadeFrames] == 0.0f);
    CHECK(rig.Mixer().CountersSnapshot().pausedVoices == 1);
    CHECK(StatusOf(rig.Mixer(), voice).unreadFrames == Block - PauseFadeFrames);
}

} // namespace audio_mixer_pause_tests_file
