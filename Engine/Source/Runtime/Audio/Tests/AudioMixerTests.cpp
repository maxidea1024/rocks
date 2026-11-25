#include "AudioTestSupport.h"

#include <Audio/Decibels.h>
#include <Audio/OfflineAudioRender.h>
#include <Audio/Panner.h>
#include <Base/InitializerList.h>
#include <Core/Containers/Array.h>
#include <Core/Math/MathUtility.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/FloatingPointEnvironment.h>
#include <Platform/Memory.h>
#include <Platform/PlatformMath.h>
#include <TestSupport/MathTestSupport.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

using namespace rocks;
namespace audio_mixer_tests_file {

using namespace rocks::audio;
using platform::PlatformMath;

// AU1 믹서 코어 시험이다(Docs/Design/AudioSystem 3.22.10절). 모두 OfflineAudioRender로
// 결정적으로 렌더하고 frame·채널 값을 정답과 대조한다. 기본 rig는 limiter를 꺼서(clamp만 남는다)
// 정수·이진 소수 fixture가 정확히 일치한다.

namespace {

constexpr uint32 Rate = DefaultMixSampleRate;

AudioMixerConfig ConfigOf(uint32 blockFrames, bool limiter = false,
                          ChannelLayout layout = ChannelLayout::Stereo,
                          uint32 voices = 32) noexcept {
    AudioMixerConfig config;
    config.blockFrames = blockFrames;
    config.masterLimiterEnabled = limiter;
    config.layout = layout;
    config.maxVoices = voices;
    config.maxClips = 16;
    return config;
}

OfflineAudioRender MakeRig(const AudioMixerConfig& config, Allocator& allocator) noexcept {
    auto created = OfflineAudioRender::TryCreate(config, allocator);
    if (!created) {
        platform::Fatal("Audio mixer test configuration was rejected");
    }

    return Move(created.Value());
}

AudioClipId RegisterClip(AudioMixer& mixer, Span<const float* const> channels, uint32 frames,
                         uint32 rate = Rate, uint32 loopBegin = 0, uint32 loopEnd = 0) noexcept {
    AudioClipDesc desc;
    desc.channelCount = static_cast<uint32>(channels.Size());
    for (uint32 channel = 0; channel < desc.channelCount; ++channel) {
        desc.channels[channel] = channels[channel];
    }

    desc.frameCount = frames;
    desc.sampleRate = rate;
    desc.loopBegin = loopBegin;
    desc.loopEnd = loopEnd;
    auto registered = mixer.RegisterClip(desc);
    if (!registered) {
        platform::Fatal("Audio mixer test clip was rejected");
    }

    return registered.Value();
}

AudioClipId RegisterMono(AudioMixer& mixer, const float* samples, uint32 frames, uint32 rate = Rate,
                         uint32 loopBegin = 0, uint32 loopEnd = 0) noexcept {
    const float* channels[] = {samples};
    return RegisterClip(mixer, Span<const float* const>(channels), frames, rate, loopBegin,
                        loopEnd);
}

void SubmitOrDie(OfflineAudioRender& rig, const AudioRtCommand& command) noexcept {
    if (!rig.Submit(command)) {
        platform::Fatal("Audio mixer test command was rejected");
    }
}

// voice를 잡고 frame에 재생을 예약한다.
AudioVoiceHandle PlayAt(OfflineAudioRender& rig, AudioClipId clip, uint64 frame,
                        const AudioVoiceParams& params = AudioVoiceParams{}, bool loop = false,
                        uint32 startFrame = 0) noexcept {
    AudioMixer& mixer = rig.Mixer();
    auto voice = mixer.AcquireVoice();
    if (!voice) {
        platform::Fatal("Audio mixer test voice was not acquired");
    }

    auto play = mixer.PreparePlay(voice.Value(), clip, startFrame, loop, params);
    if (!play) {
        platform::Fatal("Audio mixer test play was rejected");
    }

    play.Value().mixFrame = frame;
    SubmitOrDie(rig, play.Value());
    return voice.Value();
}

AudioRtCommand StopAt(OfflineAudioRender& rig, AudioVoiceHandle voice, uint64 frame,
                      uint32 fadeFrames) noexcept {
    auto command = rig.Mixer().MakeStop(voice, frame, fadeFrames);
    if (!command) {
        platform::Fatal("Audio mixer test stop was rejected");
    }

    return command.Value();
}

AudioRtCommand ParamsAt(OfflineAudioRender& rig, AudioVoiceHandle voice, uint64 frame,
                        const AudioVoiceParams& params, uint32 rampFrames) noexcept {
    auto command = rig.Mixer().MakeSetParams(voice, frame, params, rampFrames);
    if (!command) {
        platform::Fatal("Audio mixer test parameters were rejected");
    }

    return command.Value();
}

AudioRtCommand MasterAt(OfflineAudioRender& rig, uint64 frame, float gain,
                        uint32 rampFrames) noexcept {
    auto command = rig.Mixer().MakeSetMasterGain(frame, gain, rampFrames);
    if (!command) {
        platform::Fatal("Audio mixer test master gain was rejected");
    }

    return command.Value();
}

// 제어 API를 거치지 않은 손 명령(RT의 stale·clamp·무시 경로 시험 전용)이다. 순번을 직접 준다.
AudioRtCommand RawCommand(AudioRtCommandType type, AudioVoiceHandle voice, uint64 frame,
                          uint64 sequence) noexcept {
    AudioRtCommand command;
    command.type = type;
    command.voice = voice;
    command.mixFrame = frame;
    command.sequence = sequence;
    return command;
}

// blocks 블록을 렌더해 교차 float 배열로 돌려준다.
Array<float> RenderBlocks(OfflineAudioRender& rig, uint32 blocks, Allocator& allocator) noexcept {
    Array<float> output(allocator);
    const usize floats = static_cast<usize>(blocks) * rig.Mixer().Config().blockFrames *
                         rig.Mixer().OutputChannelCount();
    if (!output.TryResize(floats) ||
        !rig.Render(Span<float>(output.Data(), output.Size()), blocks)) {
        platform::Fatal("Audio mixer test render failed");
    }

    return output;
}

float Sample(const Array<float>& output, uint32 channels, uint32 frame, uint32 channel) noexcept {
    return output[static_cast<usize>(frame) * channels + channel];
}

uint64 Ignored(const AudioMixer& mixer, AudioRtCommandType type) noexcept {
    return mixer.Counters().ignoredCommands[static_cast<uint32>(type)];
}

AudioVoiceStatus StatusOf(const AudioMixer& mixer, AudioVoiceHandle voice) noexcept {
    auto status = mixer.QueryVoice(voice);
    if (!status) {
        platform::Fatal("Audio mixer test voice query failed");
    }

    return status.Value();
}

} // namespace

TEST(Audio_Mixer_DirectChannelMapping) {
    SystemAllocator allocator;
    float ones[64];
    float left[64];
    float right[64];
    for (uint32 frame = 0; frame < 64; ++frame) {
        ones[frame] = 1.0f;
        left[frame] = 0.25f;
        right[frame] = -0.5f;
    }

    // mono → stereo는 양쪽 0.5(UE Linear 업믹스), 끝 frame = 시작 + 64.
    OfflineAudioRender rig = MakeRig(ConfigOf(256), allocator);
    const AudioClipId mono = RegisterMono(rig.Mixer(), ones, 64);
    const AudioVoiceHandle voice = PlayAt(rig, mono, 10);
    Array<float> output = RenderBlocks(rig, 1, allocator);
    for (uint32 frame = 0; frame < 256; ++frame) {
        const float expected = frame >= 10 && frame < 74 ? 0.5f : 0.0f;
        CHECK(Sample(output, 2, frame, 0) == expected && Sample(output, 2, frame, 1) == expected);
    }

    const AudioVoiceStatus status = StatusOf(rig.Mixer(), voice);
    CHECK(status.state == AudioVoiceState::Finished &&
          status.endReason == AudioVoiceEndReason::NaturalEnd);
    CHECK(status.startMixFrame == 10 && status.endMixFrame == 74);

    // stereo → stereo 직결.
    const float* stereo[] = {left, right};
    const AudioClipId pair = RegisterClip(rig.Mixer(), Span<const float* const>(stereo), 64);
    PlayAt(rig, pair, 256);
    output = RenderBlocks(rig, 1, allocator);
    CHECK(Sample(output, 2, 0, 0) == 0.25f && Sample(output, 2, 63, 1) == -0.5f &&
          Sample(output, 2, 64, 0) == 0.0f);

    // stereo → mono는 0.707씩, mono → mono는 1.
    OfflineAudioRender monoRig = MakeRig(ConfigOf(256, false, ChannelLayout::Mono), allocator);
    const AudioClipId monoPair =
        RegisterClip(monoRig.Mixer(), Span<const float* const>(stereo), 64);
    const AudioClipId monoOnes = RegisterMono(monoRig.Mixer(), ones, 64);
    PlayAt(monoRig, monoPair, 0);
    PlayAt(monoRig, monoOnes, 100);
    output = RenderBlocks(monoRig, 1, allocator);
    CHECK(test::Near(output[0], 0.707f * (0.25f - 0.5f), 1.0e-6f));
    CHECK(test::Near(output[100], 1.0f, 1.0e-7f));
}

TEST(Audio_Mixer_AzimuthPansMonoClip) {
    SystemAllocator allocator;
    float ones[32];
    for (float& value : ones) {
        value = 1.0f;
    }

    OfflineAudioRender rig = MakeRig(ConfigOf(64), allocator);
    const AudioClipId clip = RegisterMono(rig.Mixer(), ones, 32);
    AudioVoiceParams params;
    params.panMode = AudioPanMode::Azimuth;
    params.azimuthDegrees = 90.0f;
    PlayAt(rig, clip, 0, params);
    params.azimuthDegrees = 0.0f;
    params.nonSpatializedAmount = 1.0f;
    PlayAt(rig, clip, 40, params);
    const Array<float> output = RenderBlocks(rig, 2, allocator);
    // 정오른쪽: R만 1(equal-power).
    CHECK(test::Near(Sample(output, 2, 5, 0), 0.0f, 1.0e-6f) &&
          test::Near(Sample(output, 2, 5, 1), 1.0f, 1.0e-6f));
    // 비공간화 1은 1/채널 수 = 0.5로 Direct mono와 같다.
    CHECK(Sample(output, 2, 50, 0) == 0.5f && Sample(output, 2, 50, 1) == 0.5f);
}

TEST(Audio_Mixer_ScheduledOffsetsAcrossBlocks) {
    SystemAllocator allocator;
    const float impulse[] = {1.0f};
    for (const uint32 blockFrames : {64u, 256u, 512u}) {
        OfflineAudioRender rig = MakeRig(ConfigOf(blockFrames), allocator);
        const AudioClipId clip = RegisterMono(rig.Mixer(), impulse, 1);
        const uint64 starts[] = {0, 255, 256, 257, 700};
        for (const uint64 start : starts) {
            PlayAt(rig, clip, start);
        }

        const uint32 blocks = 1024 / blockFrames;
        const Array<float> output = RenderBlocks(rig, blocks, allocator);
        for (uint32 frame = 0; frame < 1024; ++frame) {
            bool expected = false;
            for (const uint64 start : starts) {
                expected = expected || frame == start;
            }

            CHECK(Sample(output, 2, frame, 0) == (expected ? 0.5f : 0.0f));
        }
    }
}

TEST(Audio_Mixer_SameFrameOrderAndLateCommands) {
    SystemAllocator allocator;
    float ones[64];
    for (float& value : ones) {
        value = 1.0f;
    }

    OfflineAudioRender rig = MakeRig(ConfigOf(256), allocator);
    const AudioClipId clip = RegisterMono(rig.Mixer(), ones, 64);
    // 같은 frame: 앞 sequence의 Stop이 Prepared voice를 바로 끝내고 뒤 Play는 무시된다.
    AudioMixer& mixer = rig.Mixer();
    const AudioVoiceHandle first = mixer.AcquireVoice().Value();
    auto play = mixer.PreparePlay(first, clip, 0, false, AudioVoiceParams{});
    REQUIRE(play);
    // 순번은 만든 순서다(Play가 먼저 만들어졌다). 시작 전 Stop은 앞 frame에 둔다.
    SubmitOrDie(rig, StopAt(rig, first, 20, DefaultStopFadeFrames));
    play.Value().mixFrame = 21;
    SubmitOrDie(rig, play.Value());
    // 같은 frame: Play 뒤 fade 0 Stop은 시작과 끝이 같은 frame이다.
    const AudioVoiceHandle second = PlayAt(rig, clip, 30);
    SubmitOrDie(rig, StopAt(rig, second, 30, 0));
    Array<float> output = RenderBlocks(rig, 1, allocator);
    for (uint32 frame = 0; frame < 256; ++frame) {
        CHECK(Sample(output, 2, frame, 0) == 0.0f);
    }

    AudioVoiceStatus status = StatusOf(mixer, first);
    CHECK(status.endReason == AudioVoiceEndReason::Stopped && status.endMixFrame == 20);
    CHECK(status.startMixFrame == UnsetMixFrame);
    status = StatusOf(mixer, second);
    CHECK(status.startMixFrame == 30 && status.endMixFrame == 30 &&
          status.endReason == AudioVoiceEndReason::Stopped);
    CHECK(Ignored(mixer, AudioRtCommandType::Play) == 1);

    // 지난 frame의 명령은 다음 블록 시작에 실행하고 늦은 frame을 센다.
    const AudioVoiceHandle late = PlayAt(rig, clip, 100);
    output = RenderBlocks(rig, 1, allocator);
    CHECK(StatusOf(mixer, late).startMixFrame == 256);
    CHECK(Sample(output, 2, 0, 0) == 0.5f);
    CHECK(mixer.Counters().lateCommands == 1 && mixer.Counters().lateFrames == 156);
}

TEST(Audio_Mixer_SegmentLimitDefersAndImmediateIsNotStuck) {
    SystemAllocator allocator;
    const float impulse[] = {1.0f};
    OfflineAudioRender rig = MakeRig(ConfigOf(256), allocator);
    const AudioClipId clip = RegisterMono(rig.Mixer(), impulse, 1);
    // 서로 다른 offset 18개: 0–15(16구간)만 이 블록에, 나머지 2개는 다음 블록 시작으로 미룬다.
    AudioVoiceHandle voices[18];
    for (uint32 index = 0; index < 18; ++index) {
        voices[index] = PlayAt(rig, clip, index);
    }

    Array<float> output = RenderBlocks(rig, 1, allocator);
    CHECK(rig.Mixer().Counters().deferredCommands == 2);
    for (uint32 frame = 0; frame < 16; ++frame) {
        CHECK(Sample(output, 2, frame, 0) == 0.5f);
    }

    CHECK(Sample(output, 2, 16, 0) == 0.0f);
    output = RenderBlocks(rig, 1, allocator);
    // 미룬 두 impulse가 다음 블록 시작(256)에 겹친다: 0.5 × 2.
    CHECK(Sample(output, 2, 0, 0) == 1.0f);
    CHECK(StatusOf(rig.Mixer(), voices[17]).startMixFrame == 256);
    CHECK(rig.Mixer().Counters().lateCommands == 2);

    // 먼 미래 명령 뒤에 낸 Immediate도 다음 블록에 실행된다.
    PlayAt(rig, clip, 100000);
    const AudioVoiceHandle now = PlayAt(rig, clip, ImmediateMixFrame);
    RenderBlocks(rig, 1, allocator);
    CHECK(StatusOf(rig.Mixer(), now).startMixFrame == 512);
    CHECK(rig.PendingCommands() == 1);
}

TEST(Audio_Mixer_SameOutputForEveryBlockSize) {
    SystemAllocator allocator;
    RandomStream random(0x5A5A);
    float noise[3000];
    for (float& value : noise) {
        value = random.NextFloatInRange(-0.3f, 0.3f);
    }

    float tone[700];
    for (uint32 frame = 0; frame < 700; ++frame) {
        tone[frame] = 0.2f * PlatformMath::Sin(static_cast<float>(frame) * 0.05f);
    }

    // 시각을 명시한 명령(Immediate·구간 미룸 없음)이면 Q와 무관하게 같은 PCM·끝 frame이다.
    constexpr uint32 TotalFrames = 4096;
    Array<float> reference(allocator);
    uint64 referenceEnds[3] = {};
    for (const uint32 blockFrames : {64u, 256u, 512u, 1024u}) {
        AudioMixerConfig config = ConfigOf(blockFrames, true);
        OfflineAudioRender rig = MakeRig(config, allocator);
        const AudioClipId noiseClip = RegisterMono(rig.Mixer(), noise, 3000, 44100);
        const AudioClipId toneClip = RegisterMono(rig.Mixer(), tone, 700, 32000, 100, 600);
        AudioVoiceParams params;
        params.pitch = 0.75f;
        params.lowPassCutoffHz = 3000.0f;
        const AudioVoiceHandle first = PlayAt(rig, noiseClip, 37, params);
        AudioVoiceParams panned;
        panned.panMode = AudioPanMode::Azimuth;
        panned.azimuthDegrees = 300.0f;
        panned.highPassCutoffHz = 400.0f;
        const AudioVoiceHandle second = PlayAt(rig, toneClip, 300, panned, true);
        const AudioVoiceHandle third =
            PlayAt(rig, noiseClip, 1500, AudioVoiceParams{}, false, 2000);
        params.gain = 2.5f;
        params.pitch = 1.6f;
        params.lowPassCutoffHz = 900.0f;
        SubmitOrDie(rig, ParamsAt(rig, first, 1111, params, 700));
        panned.azimuthDegrees = 45.0f;
        panned.gain = 0.5f;
        SubmitOrDie(rig, ParamsAt(rig, second, 1999, panned, 333));
        SubmitOrDie(rig, StopAt(rig, second, 3001, 900));
        SubmitOrDie(rig, MasterAt(rig, 2500, 1.5f, 600));

        const Array<float> output = RenderBlocks(rig, TotalFrames / blockFrames, allocator);
        const uint64 ends[3] = {StatusOf(rig.Mixer(), first).endMixFrame,
                                StatusOf(rig.Mixer(), second).endMixFrame,
                                StatusOf(rig.Mixer(), third).endMixFrame};
        if (reference.Size() == 0) {
            REQUIRE(reference.TryAppend(Span<const float>(output.Data(), output.Size())));
            for (uint32 index = 0; index < 3; ++index) {
                referenceEnds[index] = ends[index];
            }

            CHECK(ends[1] == 3001 + 900);
            continue;
        }

        CHECK(memcmp(reference.Data(), output.Data(), sizeof(float) * output.Size()) == 0);
        for (uint32 index = 0; index < 3; ++index) {
            CHECK(ends[index] == referenceEnds[index]);
        }
    }
}

TEST(Audio_Mixer_ResampleLoopAndEndFrame) {
    SystemAllocator allocator;
    // 44.1 kHz 선형 입력(i/1024)은 48 kHz 출력 n에서 n·44100/48000/1024다(선형 보간이 정확).
    float ramp[2048];
    for (uint32 frame = 0; frame < 2048; ++frame) {
        ramp[frame] = static_cast<float>(frame) / 1024.0f;
    }

    OfflineAudioRender rig = MakeRig(ConfigOf(256), allocator);
    const AudioClipId slow = RegisterMono(rig.Mixer(), ramp, 2048, 44100);
    const AudioVoiceHandle slowVoice = PlayAt(rig, slow, 0);
    SubmitOrDie(rig, StopAt(rig, slowVoice, 1024, 0));
    Array<float> output = RenderBlocks(rig, 4, allocator);
    for (uint32 frame = 0; frame < 1024; ++frame) {
        const double expected = 0.5 * static_cast<double>(frame) * 44100.0 / 48000.0 / 1024.0;
        CHECK(
            test::Near(static_cast<double>(Sample(output, 2, frame, 0)), expected, 2.0e-6, 1.0e-6));
    }

    // pitch 0.5: 4 frame clip [1,1,1,1]은 위치 0–3.5의 8 frame, 마지막은 x3·x4(0 꼬리)의 중간
    // 0.5다.
    const float four[] = {1.0f, 1.0f, 1.0f, 1.0f};
    const AudioClipId short4 = RegisterMono(rig.Mixer(), four, 4);
    AudioVoiceParams half;
    half.pitch = 0.5f;
    const AudioVoiceHandle halfVoice = PlayAt(rig, short4, 1024 + 10, half);
    // pitch 2: 위치 0, 2 → 2 frame.
    AudioVoiceParams twice;
    twice.pitch = 2.0f;
    const AudioVoiceHandle twiceVoice = PlayAt(rig, short4, 1024 + 100, twice);
    output = RenderBlocks(rig, 1, allocator);
    for (uint32 frame = 10; frame < 17; ++frame) {
        CHECK(Sample(output, 2, frame, 0) == 0.5f);
    }

    CHECK(Sample(output, 2, 17, 0) == 0.25f && Sample(output, 2, 18, 0) == 0.0f);
    CHECK(StatusOf(rig.Mixer(), halfVoice).endMixFrame == 1024 + 18);
    CHECK(Sample(output, 2, 100, 0) == 0.5f && Sample(output, 2, 101, 0) == 0.5f &&
          Sample(output, 2, 102, 0) == 0.0f);
    CHECK(StatusOf(rig.Mixer(), twiceVoice).endMixFrame == 1024 + 102);

    // loop: 구간 [2, 6)이면 0,1,2,3,4,5,2,3,4,5,…, 0/0이면 clip 전체를 되풀이한다.
    float indices[8];
    for (uint32 frame = 0; frame < 8; ++frame) {
        indices[frame] = static_cast<float>(frame) / 8.0f;
    }

    const AudioClipId partial = RegisterMono(rig.Mixer(), indices, 8, Rate, 2, 6);
    PlayAt(rig, partial, 1280, AudioVoiceParams{}, true);
    output = RenderBlocks(rig, 1, allocator);
    const uint32 partialOrder[] = {0, 1, 2, 3, 4, 5, 2, 3, 4, 5, 2, 3};
    for (uint32 frame = 0; frame < 12; ++frame) {
        CHECK(Sample(output, 2, frame, 0) == 0.5f * static_cast<float>(partialOrder[frame]) / 8.0f);
    }

    OfflineAudioRender loopRig = MakeRig(ConfigOf(64), allocator);
    const AudioClipId loopWhole = RegisterMono(loopRig.Mixer(), indices, 8);
    const AudioVoiceHandle looping = PlayAt(loopRig, loopWhole, 0, AudioVoiceParams{}, true, 3);
    output = RenderBlocks(loopRig, 2, allocator);
    for (uint32 frame = 0; frame < 128; ++frame) {
        CHECK(Sample(output, 2, frame, 1) == 0.5f * static_cast<float>((frame + 3) % 8) / 8.0f);
    }

    CHECK(StatusOf(loopRig.Mixer(), looping).state == AudioVoiceState::Playing);
}

TEST(Audio_Mixer_VoiceStateTable) {
    SystemAllocator allocator;
    float ones[2048];
    for (float& value : ones) {
        value = 1.0f;
    }

    OfflineAudioRender rig = MakeRig(ConfigOf(256), allocator);
    AudioMixer& mixer = rig.Mixer();
    const AudioClipId clip = RegisterMono(mixer, ones, 2048);

    // Reserved: Stop·SetParams는 무시된다.
    const AudioVoiceHandle reserved = mixer.AcquireVoice().Value();
    CHECK(mixer.MakeStop(reserved, 0, 0).Error() == ErrorCode::InvalidArgument);
    CHECK(mixer.MakeSetParams(reserved, 0, AudioVoiceParams{}, 0).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(mixer.PublishParams(reserved, AudioVoiceParams{}).Error() == ErrorCode::InvalidArgument);
    CHECK(mixer.RequestStop(reserved, 0).Error() == ErrorCode::InvalidArgument);

    // 순번은 제어 API가 만든 순서다. Play를 만든 뒤 만든 SetParams는 늘 Play보다 뒤 순번이라, 같은
    // frame이면 시작 뒤에 적용되고(Playing), 앞 frame이면 Prepared에 저장되어 Play 때 이긴다.
    AudioVoiceParams quarter;
    quarter.gain = 0.25f;
    const AudioVoiceHandle playWins = mixer.AcquireVoice().Value();
    auto playWinsCommand = mixer.PreparePlay(playWins, clip, 0, false, AudioVoiceParams{});
    REQUIRE(playWinsCommand);
    SubmitOrDie(rig, ParamsAt(rig, playWins, 10, quarter, 0));
    playWinsCommand.Value().mixFrame = 10;
    SubmitOrDie(rig, playWinsCommand.Value());
    const AudioVoiceHandle paramsWin = mixer.AcquireVoice().Value();
    auto paramsWinCommand = mixer.PreparePlay(paramsWin, clip, 0, false, AudioVoiceParams{});
    REQUIRE(paramsWinCommand);
    paramsWinCommand.Value().mixFrame = 100;
    SubmitOrDie(rig, paramsWinCommand.Value());
    SubmitOrDie(rig, ParamsAt(rig, paramsWin, 50, quarter, 0));
    Array<float> output = RenderBlocks(rig, 1, allocator);
    CHECK(Sample(output, 2, 20, 0) == 0.125f);
    CHECK(Sample(output, 2, 120, 0) == 0.25f);
    CHECK(Ignored(mixer, AudioRtCommandType::Stop) == 0 &&
          Ignored(mixer, AudioRtCommandType::SetParams) == 0);

    // Playing: Play는 무시, 반복 Stop은 더 일찍 끝날 때만(1000은 남은 90보다 길어 무시), fade 중
    // SetParams가 끝을 막지 못한다.
    SubmitOrDie(rig, paramsWinCommand.Value());
    SubmitOrDie(rig, StopAt(rig, playWins, 300, 100));
    SubmitOrDie(rig, StopAt(rig, playWins, 310, 1000));
    AudioVoiceParams loud;
    loud.gain = 2.0f;
    SubmitOrDie(rig, ParamsAt(rig, playWins, 320, loud, 10));
    output = RenderBlocks(rig, 1, allocator);
    AudioVoiceStatus status = StatusOf(mixer, playWins);
    CHECK(status.state == AudioVoiceState::Finished &&
          status.endReason == AudioVoiceEndReason::Stopped);
    CHECK(status.endMixFrame == 400);
    CHECK(Ignored(mixer, AudioRtCommandType::Play) == 1 &&
          Ignored(mixer, AudioRtCommandType::Stop) == 1);

    // Finished: 모든 명령 무시, terminal은 한 번만 수거된다. 수거 전 해제는 Busy.
    CHECK(mixer.ReleaseVoice(playWins).Error() == ErrorCode::Busy);
    SubmitOrDie(rig, StopAt(rig, playWins, 600, 0));
    SubmitOrDie(rig, ParamsAt(rig, playWins, 600, loud, 0));
    RenderBlocks(rig, 1, allocator);
    CHECK(Ignored(mixer, AudioRtCommandType::Stop) == 2 &&
          Ignored(mixer, AudioRtCommandType::SetParams) == 1);
    AudioVoiceTerminal terminals[8];
    const uint32 collected = mixer.CollectFinished(Span<AudioVoiceTerminal>(terminals));
    CHECK(collected == 1 && terminals[0].voice.index == playWins.index &&
          terminals[0].reason == AudioVoiceEndReason::Stopped);
    CHECK(mixer.CollectFinished(Span<AudioVoiceTerminal>(terminals)) == 0);
    CHECK(mixer.ReleaseVoice(playWins));
    // 놓은 handle은 무효다: 조회·명령 모두(제어 API가 거부하고, 손 명령은 RT가 stale로 센다).
    CHECK(mixer.QueryVoice(playWins).Error() == ErrorCode::StaleHandle);
    CHECK(mixer.MakeStop(playWins, 900, 0).Error() == ErrorCode::StaleHandle);
    // RT는 자기가 아는 generation보다 오래된 handle의 손 명령을 stale로 센다.
    SubmitOrDie(rig, RawCommand(AudioRtCommandType::Stop,
                                AudioVoiceHandle{playWins.index, playWins.generation - 1}, 900, 0));
    RenderBlocks(rig, 1, allocator);
    CHECK(mixer.Counters().staleCommands == 1);
    CHECK(mixer.ReleaseVoice(reserved));
}

TEST(Audio_Mixer_FadeAndParameterRamps) {
    SystemAllocator allocator;
    float ones[4096];
    for (float& value : ones) {
        value = 1.0f;
    }

    OfflineAudioRender rig = MakeRig(ConfigOf(256), allocator);
    const AudioClipId clip = RegisterMono(rig.Mixer(), ones, 4096);
    // SetParams ramp: gain 1 → 0을 100 frame에. k번째 frame은 0.5·(1 − k/100).
    const AudioVoiceHandle ramped = PlayAt(rig, clip, 0);
    AudioVoiceParams silent;
    silent.gain = 0.0f;
    SubmitOrDie(rig, ParamsAt(rig, ramped, 50, silent, 100));
    Array<float> output = RenderBlocks(rig, 1, allocator);
    for (uint32 k = 0; k < 100; ++k) {
        CHECK(test::Near(Sample(output, 2, 50 + k, 0),
                         0.5f * (1.0f - static_cast<float>(k) / 100.0f), 1.0e-6f));
    }

    CHECK(Sample(output, 2, 150, 0) == 0.0f);
    SubmitOrDie(rig, StopAt(rig, ramped, 256, 0));

    // Stop fade 512: 단조 감소, 끝 frame = Stop + 512.
    const AudioVoiceHandle faded = PlayAt(rig, clip, 256);
    SubmitOrDie(rig, StopAt(rig, faded, 300, DefaultStopFadeFrames));
    output = RenderBlocks(rig, 4, allocator);
    CHECK(Sample(output, 2, 43, 0) == 0.5f);
    for (uint32 frame = 45; frame < 44 + 512; ++frame) {
        CHECK(Sample(output, 2, frame, 0) < Sample(output, 2, frame - 1, 0));
    }

    CHECK(Sample(output, 2, 44 + 512, 0) == 0.0f);
    CHECK(StatusOf(rig.Mixer(), faded).endMixFrame == 300 + 512);
}

TEST(Audio_Mixer_MasterGainLimiterAndClamp) {
    SystemAllocator allocator;
    float ones[512];
    for (float& value : ones) {
        value = 1.0f;
    }

    AudioVoiceParams loud;
    loud.gain = MaxVoiceGain;
    // limiter 켬: 출력이 −1 dBFS를 넘지 않는다.
    OfflineAudioRender limited = MakeRig(ConfigOf(256, true), allocator);
    PlayAt(limited, RegisterMono(limited.Mixer(), ones, 512), 0, loud);
    Array<float> output = RenderBlocks(limited, 2, allocator);
    const float ceiling = DecibelsToLinear(-1.0f) * 1.0001f;
    for (uint32 frame = 0; frame < 512; ++frame) {
        CHECK(math::Abs(Sample(output, 2, frame, 0)) <= ceiling);
    }

    CHECK(limited.Mixer().Counters().clippedSamples == 0);

    // limiter 끔: ±1로 자르고 센다.
    OfflineAudioRender clamped = MakeRig(ConfigOf(256), allocator);
    PlayAt(clamped, RegisterMono(clamped.Mixer(), ones, 512), 0, loud);
    output = RenderBlocks(clamped, 1, allocator);
    CHECK(Sample(output, 2, 0, 0) == 1.0f && clamped.Mixer().Counters().clippedSamples == 512);

    // 마스터 gain: 0.5를 즉시.
    OfflineAudioRender master = MakeRig(ConfigOf(256), allocator);
    PlayAt(master, RegisterMono(master.Mixer(), ones, 512), 0);
    SubmitOrDie(master, MasterAt(master, 128, 0.5f, 0));
    output = RenderBlocks(master, 1, allocator);
    CHECK(Sample(output, 2, 127, 0) == 0.5f && Sample(output, 2, 128, 0) == 0.25f);
}

TEST(Audio_Mixer_LifetimeAndValidation) {
    SystemAllocator allocator;
    float ones[16];
    for (float& value : ones) {
        value = 1.0f;
    }

    OfflineAudioRender rig = MakeRig(ConfigOf(256, false, ChannelLayout::Stereo, 2), allocator);
    AudioMixer& mixer = rig.Mixer();
    const AudioClipId clip = RegisterMono(mixer, ones, 16);
    const AudioVoiceHandle a = PlayAt(rig, clip, 0);
    const AudioVoiceHandle b = mixer.AcquireVoice().Value();
    CHECK(mixer.AcquireVoice().Error() == ErrorCode::CapacityExceeded);
    // Prepared voice가 clip을 묶고 있으면 clip 해제는 Busy, Prepared·Playing voice 해제도 Busy.
    CHECK(mixer.UnregisterClip(clip).Error() == ErrorCode::Busy);
    CHECK(mixer.ReleaseVoice(a).Error() == ErrorCode::Busy);
    CHECK(mixer.PreparePlay(a, clip, 0, false, AudioVoiceParams{}).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(mixer.PreparePlay(b, clip, 16, false, AudioVoiceParams{}).Error() ==
          ErrorCode::InvalidArgument);
    AudioVoiceParams bad;
    bad.pitch = 3.0f;
    CHECK(mixer.PreparePlay(b, clip, 0, false, bad).Error() == ErrorCode::InvalidArgument);
    bad = AudioVoiceParams{};
    bad.gain = PlatformMath::QuietNaN<float>();
    CHECK(mixer.PreparePlay(b, clip, 0, false, bad).Error() == ErrorCode::InvalidArgument);
    CHECK(mixer
              .PreparePlay(AudioVoiceHandle{b.index, b.generation + 1}, clip, 0, false,
                           AudioVoiceParams{})
              .Error() == ErrorCode::StaleHandle);
    RenderBlocks(rig, 1, allocator);
    AudioVoiceTerminal terminals[2];
    CHECK(mixer.CollectFinished(Span<AudioVoiceTerminal>(terminals)) == 1);
    CHECK(mixer.ReleaseVoice(a) && mixer.ReleaseVoice(b));
    CHECK(mixer.UnregisterClip(clip));
    CHECK(mixer.UnregisterClip(clip).Error() == ErrorCode::StaleHandle);

    // clip 검사: 비유한 PCM, 채널 수, rate, loop 구간.
    float poisoned[4] = {0.0f, PlatformMath::Infinity<float>(), 0.0f, 0.0f};
    AudioClipDesc desc;
    desc.channels[0] = poisoned;
    desc.channelCount = 1;
    desc.frameCount = 4;
    desc.sampleRate = Rate;
    CHECK(mixer.RegisterClip(desc).Error() == ErrorCode::InvalidArgument);
    desc.channels[0] = ones;
    desc.sampleRate = MinClipSampleRate - 1;
    CHECK(mixer.RegisterClip(desc).Error() == ErrorCode::InvalidArgument);
    desc.sampleRate = Rate;
    desc.loopBegin = 3;
    desc.loopEnd = 3;
    CHECK(mixer.RegisterClip(desc).Error() == ErrorCode::InvalidArgument);
    desc.loopEnd = 5;
    CHECK(mixer.RegisterClip(desc).Error() == ErrorCode::InvalidArgument);
    desc.loopBegin = 0;
    desc.loopEnd = 0;
    desc.channelCount = 3;
    CHECK(mixer.RegisterClip(desc).Error() == ErrorCode::InvalidArgument);

    // 설정 검사.
    AudioMixerConfig config;
    config.blockFrames = 100;
    CHECK(AudioMixer::TryCreate(config, allocator).Error() == ErrorCode::InvalidArgument);
    config = AudioMixerConfig{};
    config.layout = ChannelLayout::Surround51;
    CHECK(AudioMixer::TryCreate(config, allocator).Error() == ErrorCode::InvalidArgument);
    config = AudioMixerConfig{};
    config.maxVoices = MaxMixerVoices + 1;
    CHECK(AudioMixer::TryCreate(config, allocator).Error() == ErrorCode::InvalidArgument);
    config = AudioMixerConfig{};
    config.masterLimiter.ratio = 0.0f;
    config.masterLimiter.mode = DynamicsMode::Compressor;
    CHECK(AudioMixer::TryCreate(config, allocator).Error() == ErrorCode::InvalidArgument);
}

TEST(Audio_Mixer_RenderDoesNotAllocateAndCreationRollsBack) {
    SystemAllocator system;
    test::NFailAllocator allocator(system);
    float ones[1024];
    for (float& value : ones) {
        value = 1.0f;
    }

    {
        OfflineAudioRender rig = MakeRig(ConfigOf(256, true), allocator);
        const AudioClipId clip = RegisterMono(rig.Mixer(), ones, 1024);
        for (uint32 index = 0; index < 8; ++index) {
            PlayAt(rig, clip, index * 100);
        }

        Array<float> output(system);
        REQUIRE(output.TryResize(256 * 2 * 4));
        const uint64 before = allocator.Attempts();
        REQUIRE(rig.Render(Span<float>(output.Data(), output.Size()), 4));
        CHECK(allocator.Attempts() == before);
    }

    // 생성 중 n번째 할당 실패마다 OutOfMemory이고 만든 것을 되돌린다(누수는 DebugAllocator 시험이
    // 본다).
    uint32 failures = 0;
    for (uint64 attempt = 1; attempt < 64; ++attempt) {
        allocator.FailOnAttempt(attempt);
        auto created = OfflineAudioRender::TryCreate(AudioMixerConfig{}, allocator);
        if (created) {
            break;
        }

        CHECK(created.Error() == ErrorCode::OutOfMemory);
        ++failures;
    }

    allocator.DisableFailure();
    CHECK(failures >= 5);
}

TEST(Audio_Mixer_RenderRunsWithFlushToZero) {
    SystemAllocator allocator;
    OfflineAudioRender rig = MakeRig(ConfigOf(64), allocator);
    // RenderBlock 진입 assert가 FTZ 환경을 확인한다. 렌더가 끝나면 원래 환경이다.
    RenderBlocks(rig, 1, allocator);
    CHECK(platform::IsDeterministicFloatingPointEnvironment());
    CHECK(!platform::IsRealtimeAudioFloatingPointEnvironment());
    CHECK(rig.Mixer().MixFrame() == 64);
}

TEST(Audio_Mixer_StateTableRemainingCells) {
    SystemAllocator allocator;
    float ones[600];
    for (float& value : ones) {
        value = 1.0f;
    }

    OfflineAudioRender rig = MakeRig(ConfigOf(256), allocator);
    AudioMixer& mixer = rig.Mixer();
    const AudioClipId clip = RegisterMono(mixer, ones, 600);
    const AudioClipId shortClip = RegisterMono(mixer, ones, 150);

    // Playing·Stopping은 해제할 수 없다. Stopping의 Play는 무시된다.
    const AudioVoiceHandle playing = PlayAt(rig, clip, 0);
    // Stopping 중 자연 끝: fade(1000)보다 clip(150)이 먼저 끝나면 NaturalEnd다.
    const AudioVoiceHandle naturalFirst = PlayAt(rig, shortClip, 0);
    SubmitOrDie(rig, StopAt(rig, naturalFirst, 50, 1000));
    // 같은 frame 동점: clip 끝(0 + 150)과 fade 끝(100 + 50)이 같으면 NaturalEnd가 이긴다.
    const AudioVoiceHandle tie = PlayAt(rig, shortClip, 0);
    SubmitOrDie(rig, StopAt(rig, tie, 100, 50));
    // fade가 먼저 끝나면 Stopped다.
    const AudioVoiceHandle fadeFirst = PlayAt(rig, shortClip, 0);
    SubmitOrDie(rig, StopAt(rig, fadeFirst, 100, 49));
    RenderBlocks(rig, 1, allocator);
    CHECK(mixer.ReleaseVoice(playing).Error() == ErrorCode::Busy);
    SubmitOrDie(rig, StopAt(rig, playing, 300, 400));
    RenderBlocks(rig, 1, allocator);
    // 제어 쪽은 RT의 Stopping을 따로 보지 못한다(재생 중 = Playing).
    CHECK(StatusOf(mixer, playing).state == AudioVoiceState::Playing);
    CHECK(mixer.ReleaseVoice(playing).Error() == ErrorCode::Busy);
    auto replay = mixer.PreparePlay(playing, clip, 0, false, AudioVoiceParams{});
    CHECK(replay.Error() == ErrorCode::InvalidArgument);
    AudioRtCommand play = RawCommand(AudioRtCommandType::Play, playing, 600, 0);
    SubmitOrDie(rig, play);
    RenderBlocks(rig, 2, allocator);
    CHECK(Ignored(mixer, AudioRtCommandType::Play) == 1);
    // Stopping 중이던 voice가 clip 끝(600)에 먼저 닿는다(fade 끝은 700).
    AudioVoiceStatus status = StatusOf(mixer, playing);
    CHECK(status.endReason == AudioVoiceEndReason::NaturalEnd && status.endMixFrame == 600);
    status = StatusOf(mixer, naturalFirst);
    CHECK(status.endReason == AudioVoiceEndReason::NaturalEnd && status.endMixFrame == 150);
    status = StatusOf(mixer, tie);
    CHECK(status.endReason == AudioVoiceEndReason::NaturalEnd && status.endMixFrame == 150);
    status = StatusOf(mixer, fadeFirst);
    CHECK(status.endReason == AudioVoiceEndReason::Stopped && status.endMixFrame == 149);

    // Finished의 Play도 무시된다.
    play.voice = fadeFirst;
    play.mixFrame = 1024;
    SubmitOrDie(rig, play);
    RenderBlocks(rig, 1, allocator);
    CHECK(Ignored(mixer, AudioRtCommandType::Play) == 2);

    // Prepared에서 sequence가 더 작은 SetParams는 버리고 센다.
    const AudioVoiceHandle prepared = mixer.AcquireVoice().Value();
    auto preparedPlay = mixer.PreparePlay(prepared, clip, 0, false, AudioVoiceParams{});
    REQUIRE(preparedPlay);
    AudioVoiceParams quiet;
    quiet.gain = 0.5f;
    // Play(seq 1) → SetParams 기본(seq 2, frame 1300) → SetParams quiet(seq 3, frame 1290) 순서로
    // 낸다. frame 순서로 quiet(3)을 저장하고, 기본(2)은 더 작은 sequence라 버리고, Play(1)는 저장된
    // quiet가 이긴다.
    preparedPlay.Value().mixFrame = 1310;
    SubmitOrDie(rig, preparedPlay.Value());
    SubmitOrDie(rig, ParamsAt(rig, prepared, 1300, AudioVoiceParams{}, 0));
    SubmitOrDie(rig, ParamsAt(rig, prepared, 1290, quiet, 0));
    const Array<float> output = RenderBlocks(rig, 1, allocator);
    CHECK(Ignored(mixer, AudioRtCommandType::SetParams) == 1);
    CHECK(Sample(output, 2, 1310 - 1280 + 5, 0) == 0.25f);
}

TEST(Audio_Mixer_ClampedParamsAreCounted) {
    SystemAllocator allocator;
    float ones[64];
    for (float& value : ones) {
        value = 1.0f;
    }

    // clip 1000 Hz · mix 192 kHz · pitch 0.4 → 비율 0.0021은 리샘플러 하한 0.01로 잘린다.
    AudioMixerConfig config = ConfigOf(256);
    config.sampleRate = MaxMixSampleRate;
    OfflineAudioRender rig = MakeRig(config, allocator);
    AudioMixer& mixer = rig.Mixer();
    const AudioClipId slow = RegisterMono(mixer, ones, 64, MinClipSampleRate);
    AudioVoiceParams low;
    low.pitch = MinPitch;
    const AudioVoiceHandle voice = PlayAt(rig, slow, 0, low);
    RenderBlocks(rig, 1, allocator);
    CHECK(mixer.Counters().clampedParams == 1);

    // 렌더 경로 SetParams의 범위 밖·비유한 값은 자르거나 기본값으로 바꾸고 센다.
    AudioVoiceParams wild;
    wild.gain = 10.0f;
    wild.pitch = PlatformMath::QuietNaN<float>();
    CHECK(mixer.MakeSetParams(voice, 256, wild, 0).Error() == ErrorCode::InvalidArgument);
    // 제어 API가 거르는 값을 손 명령으로 넣어 RT의 자르기 경로를 본다(순번은 Play보다 크게).
    AudioRtCommand raw = RawCommand(AudioRtCommandType::SetParams, voice, 256, 1000000);
    raw.params = wild;
    raw.rampFrames = 0;
    SubmitOrDie(rig, raw);
    RenderBlocks(rig, 1, allocator);
    CHECK(mixer.Counters().clampedParams == 1 + 2 + 1);

    // stereo clip의 방위각 패닝은 PreparePlay가 거부한다.
    const float* stereo[] = {ones, ones};
    const AudioClipId pair = RegisterClip(mixer, Span<const float* const>(stereo), 64);
    AudioVoiceParams azimuth;
    azimuth.panMode = AudioPanMode::Azimuth;
    const AudioVoiceHandle other = mixer.AcquireVoice().Value();
    CHECK(mixer.PreparePlay(other, pair, 0, false, azimuth).Error() == ErrorCode::InvalidArgument);
}

TEST(Audio_Mixer_SubnormalProductsFlushToZero) {
    SystemAllocator allocator;
    // 정규 수 1.5e-38 × mono 업믹스 0.5 = 7.5e-39(비정규). FTZ 렌더에서는 정확히 0이다.
    float tiny[64];
    for (float& value : tiny) {
        value = 1.5e-38f;
    }

    OfflineAudioRender rig = MakeRig(ConfigOf(64), allocator);
    const AudioVoiceHandle voice = PlayAt(rig, RegisterMono(rig.Mixer(), tiny, 64), 0);
    const Array<float> output = RenderBlocks(rig, 1, allocator);
    CHECK(StatusOf(rig.Mixer(), voice).endMixFrame == 64);
    for (uint32 frame = 0; frame < 64; ++frame) {
        CHECK(platform::BitCast<uint32>(Sample(output, 2, frame, 0)) == 0u);
    }
}

} // namespace audio_mixer_tests_file
