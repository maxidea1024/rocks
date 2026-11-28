#include "AudioTestSupport.h"

#include <Audio/OfflineAudioRender.h>
#include <Base/InitializerList.h>
#include <Core/Containers/Array.h>
#include <Core/Math/MathUtility.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/Atomics.h>
#include <Platform/Fatal.h>
#include <Platform/FloatingPointEnvironment.h>
#include <Platform/Thread.h>
#include <TestSupport/MathTestSupport.h>
#include <TestSupport/Test.h>

// Mixer2 스레드 경계 시험이 믹서 내부 원자값을 주입·관찰하는 접근점이다(시험 전용 정의).
namespace rocks::audio {

class AudioMixerTestAccess {
public:
    static uint32 SeqlockVersion(AudioMixer& mixer, uint32 voice) noexcept {
        return platform::AtomicLoad(&mixer.sharedVoices_[voice].version);
    }

    static void SetSeqlockVersion(AudioMixer& mixer, uint32 voice, uint32 version) noexcept {
        platform::AtomicStore(&mixer.sharedVoices_[voice].version, version);
    }

    static void SetSeqlockReadHook(AudioMixer& mixer,
                                   void (*hook)(AudioMixer&, uint32) noexcept) noexcept {
        mixer.seqlockReadHook_ = hook;
    }

    // 읽지 않은 누적 seqlock version이다(홀수 = 쓰는 중으로 보인다).
    static void SetUnreadVersion(AudioMixer& mixer, uint32 voice, uint32 version) noexcept {
        platform::AtomicStore(&mixer.sharedVoices_[voice].unreadVersion, version);
    }

    static void SetMailbox(AudioMixer& mixer, uint32 voice, uint64 value) noexcept {
        platform::AtomicStore(&mixer.sharedVoices_[voice].mailbox, value);
    }

    static void SetCompletedMixFrame(AudioMixer& mixer, uint64 frame) noexcept {
        platform::AtomicStore(&mixer.completedMixFrame_, frame);
    }

    // terminal word {generation:32, reason:8}다(원자 읽기).
    static uint64 TerminalWord(AudioMixer& mixer, uint32 voice) noexcept {
        return platform::AtomicLoad(&mixer.sharedVoices_[voice].terminal);
    }

    // RT 슬롯에 적용된 파라미터다. RT 스레드에서만 부른다.
    static AudioVoiceParams AppliedParams(const AudioMixer& mixer, uint32 voice) noexcept {
        return mixer.voices_[voice].params;
    }
};

} // namespace rocks::audio

using namespace rocks;
namespace audio_mixer_boundary_tests_file {

using namespace rocks::audio;

// AU1 Mixer2 스레드 경계 시험이다(Docs/Design/AudioSystem 3.23.8절). 경계 상태(쓰는 중
// seqlock, 늦게 보이는 mailbox, 낡은 ACK)는 Inline에서 시험 접근점으로 주입해 결정적으로 단언한다.

namespace {

AudioMixerConfig ConfigOf(uint32 voices = 8) noexcept {
    AudioMixerConfig config;
    config.blockFrames = 256;
    config.masterLimiterEnabled = false;
    config.maxVoices = voices;
    config.maxClips = 4;
    return config;
}

OfflineAudioRender MakeRig(const AudioMixerConfig& config, Allocator& allocator) noexcept {
    auto created = OfflineAudioRender::TryCreate(config, allocator);
    if (!created) {
        platform::Fatal("Audio boundary test configuration was rejected");
    }

    return Move(created.Value());
}

AudioClipId RegisterOnes(AudioMixer& mixer, const float* samples, uint32 frames) noexcept {
    AudioClipDesc desc;
    desc.channels[0] = samples;
    desc.channelCount = 1;
    desc.frameCount = frames;
    desc.sampleRate = DefaultMixSampleRate;
    auto registered = mixer.RegisterClip(desc);
    if (!registered) {
        platform::Fatal("Audio boundary test clip was rejected");
    }

    return registered.Value();
}

void SubmitOrDie(OfflineAudioRender& rig, const AudioRtCommand& command) noexcept {
    if (!rig.Submit(command)) {
        platform::Fatal("Audio boundary test command was rejected");
    }
}

AudioVoiceHandle PlayAt(OfflineAudioRender& rig, AudioClipId clip, uint64 frame,
                        const AudioVoiceParams& params = AudioVoiceParams{}) noexcept {
    auto voice = rig.Mixer().AcquireVoice();
    if (!voice) {
        platform::Fatal("Audio boundary test voice was not acquired");
    }

    auto play = rig.Mixer().PreparePlay(voice.Value(), clip, 0, false, params);
    if (!play) {
        platform::Fatal("Audio boundary test play was rejected");
    }

    play.Value().mixFrame = frame;
    SubmitOrDie(rig, play.Value());
    return voice.Value();
}

Array<float> RenderBlocks(OfflineAudioRender& rig, uint32 blocks, Allocator& allocator) noexcept {
    Array<float> output(allocator);
    const usize floats = static_cast<usize>(blocks) * rig.Mixer().Config().blockFrames * 2;
    if (!output.TryResize(floats) ||
        !rig.Render(Span<float>(output.Data(), output.Size()), blocks)) {
        platform::Fatal("Audio boundary test render failed");
    }

    return output;
}

float Left(const Array<float>& output, uint32 frame) noexcept {
    return output[static_cast<usize>(frame) * 2];
}

AudioVoiceStatus StatusOf(const AudioMixer& mixer, AudioVoiceHandle voice) noexcept {
    auto status = mixer.QueryVoice(voice);
    if (!status) {
        platform::Fatal("Audio boundary test voice query failed");
    }

    return status.Value();
}

AudioVoiceParams Gain(float gain) noexcept {
    AudioVoiceParams params;
    params.gain = gain;
    return params;
}

uint64 Ignored(const AudioMixer& mixer, AudioRtCommandType type) noexcept {
    return mixer.Counters().ignoredCommands[static_cast<uint32>(type)];
}

} // namespace

TEST(Audio_Mixer2_SeqlockPublishRampsNextBlock) {
    SystemAllocator allocator;
    float ones[4096];
    for (float& value : ones) {
        value = 1.0f;
    }

    OfflineAudioRender rig = MakeRig(ConfigOf(), allocator);
    AudioMixer& mixer = rig.Mixer();
    const AudioVoiceHandle voice = PlayAt(rig, RegisterOnes(mixer, ones, 4096), 0);
    RenderBlocks(rig, 1, allocator);
    REQUIRE(mixer.PublishParams(voice, Gain(0.0f)));
    // 다음 블록 시작에서 DefaultParamRampFrames(256)로 1 → 0.
    const Array<float> output = RenderBlocks(rig, 2, allocator);
    for (uint32 k = 0; k < DefaultParamRampFrames; ++k) {
        CHECK(test::Near(Left(output, k), 0.5f * (1.0f - static_cast<float>(k) / 256.0f), 1.0e-6f));
    }

    CHECK(Left(output, 256) == 0.0f);
}

TEST(Audio_Mixer2_SeqlockOddAndTornReadsKeepPreviousValue) {
    SystemAllocator allocator;
    float ones[8192];
    for (float& value : ones) {
        value = 1.0f;
    }

    OfflineAudioRender rig = MakeRig(ConfigOf(), allocator);
    AudioMixer& mixer = rig.Mixer();
    const AudioVoiceHandle voice = PlayAt(rig, RegisterOnes(mixer, ones, 8192), 0);
    RenderBlocks(rig, 1, allocator);

    // 쓰는 중(홀수 version): 이번 블록은 이전 값(이득 1)을 유지하고 건너뛴 수를 센다.
    REQUIRE(mixer.PublishParams(voice, Gain(0.5f)));
    const uint32 version = AudioMixerTestAccess::SeqlockVersion(mixer, voice.index);
    AudioMixerTestAccess::SetSeqlockVersion(mixer, voice.index, version + 1);
    Array<float> output = RenderBlocks(rig, 1, allocator);
    CHECK(Left(output, 255) == 0.5f && mixer.Counters().skippedSeqlockReads == 1);

    // 찢어진 읽기(내용을 읽는 사이 version이 바뀜): 역시 유지한다.
    AudioMixerTestAccess::SetSeqlockVersion(mixer, voice.index, version);
    AudioMixerTestAccess::SetSeqlockReadHook(mixer, [](AudioMixer& target, uint32 index) noexcept {
        AudioMixerTestAccess::SetSeqlockVersion(
            target, index, AudioMixerTestAccess::SeqlockVersion(target, index) + 2);
    });
    output = RenderBlocks(rig, 1, allocator);
    CHECK(Left(output, 255) == 0.5f && mixer.Counters().skippedSeqlockReads == 2);

    // hook을 걷으면 다음 블록에 적용된다(건너뛴 읽기는 적용 순번을 바꾸지 않았다).
    AudioMixerTestAccess::SetSeqlockReadHook(mixer, nullptr);
    output = RenderBlocks(rig, 2, allocator);
    CHECK(Left(output, 300) == 0.25f);

    // 같은 순번은 다시 적용하지 않는다: 시각 지정 SetParams(더 뒤 순번)가 바꾼 값이 다음 블록에도
    // 남는다.
    auto setParams = mixer.MakeSetParams(voice, mixer.CompletedMixFrame(), Gain(1.0f), 0);
    REQUIRE(setParams);
    SubmitOrDie(rig, setParams.Value());
    output = RenderBlocks(rig, 2, allocator);
    CHECK(Left(output, 10) == 0.5f && Left(output, 400) == 0.5f);
}

TEST(Audio_Mixer2_SeqlockGenerationAndOrdering) {
    SystemAllocator allocator;
    float ones[4096];
    for (float& value : ones) {
        value = 1.0f;
    }

    // 한 voice 슬롯을 다시 쓰는 rig: 이전 수명에 출판한 값이 새 수명에 적용되지 않는다.
    OfflineAudioRender rig = MakeRig(ConfigOf(1), allocator);
    AudioMixer& mixer = rig.Mixer();
    const AudioClipId clip = RegisterOnes(mixer, ones, 300);
    const AudioVoiceHandle first = PlayAt(rig, clip, 0);
    REQUIRE(mixer.PublishParams(first, Gain(0.25f)));
    RenderBlocks(rig, 2, allocator);
    AudioVoiceTerminal terminals[2];
    REQUIRE(mixer.CollectFinished(Span<AudioVoiceTerminal>(terminals)) == 1);
    REQUIRE(mixer.ReleaseVoice(first));
    const AudioVoiceHandle second = PlayAt(rig, clip, 512);
    CHECK(second.index == first.index && second.generation != first.generation);
    Array<float> output = RenderBlocks(rig, 1, allocator);
    CHECK(Left(output, 0) == 0.5f);

    // Prepared 중 출판: Play가 적용될 때 바로 반영된다(뒤 순번).
    REQUIRE(mixer.CollectFinished(Span<AudioVoiceTerminal>(terminals)) == 0);
    OfflineAudioRender pending = MakeRig(ConfigOf(), allocator);
    AudioMixer& pendingMixer = pending.Mixer();
    const AudioClipId pendingClip = RegisterOnes(pendingMixer, ones, 4096);
    const AudioVoiceHandle prepared = PlayAt(pending, pendingClip, 100);
    REQUIRE(pendingMixer.PublishParams(prepared, Gain(0.5f)));
    output = RenderBlocks(pending, 1, allocator);
    CHECK(Left(output, 100) == 0.25f);

    // seqlock A → 미래 SetParams B → seqlock C: C가 블록 시작에 적용되고 B(더 앞 순번)는 무시된다.
    const AudioVoiceHandle ordered = PlayAt(pending, pendingClip, 256);
    RenderBlocks(pending, 1, allocator);
    REQUIRE(pendingMixer.PublishParams(ordered, Gain(0.5f)));
    auto older = pendingMixer.MakeSetParams(ordered, 700, Gain(2.0f), 0);
    REQUIRE(older);
    SubmitOrDie(pending, older.Value());
    REQUIRE(pendingMixer.PublishParams(ordered, Gain(0.25f)));
    output = RenderBlocks(pending, 2, allocator);
    // C의 ramp(512–767)가 끝난 frame 800에서 두 voice 합 = prepared 0.25 + ordered 0.5 × 0.25.
    CHECK(test::Near(Left(output, 800 - 512), 0.25f + 0.125f, 1.0e-6f));
    CHECK(Ignored(pendingMixer, AudioRtCommandType::SetParams) == 1);
}

TEST(Audio_Mixer2_StopMailbox) {
    SystemAllocator allocator;
    float ones[8192];
    for (float& value : ones) {
        value = 1.0f;
    }

    OfflineAudioRender rig = MakeRig(ConfigOf(), allocator);
    AudioMixer& mixer = rig.Mixer();
    const AudioClipId clip = RegisterOnes(mixer, ones, 8192);

    // Playing: 다음 블록 시작에서 fade.
    const AudioVoiceHandle playing = PlayAt(rig, clip, 0);
    RenderBlocks(rig, 1, allocator);
    REQUIRE(mixer.RequestStop(playing, 100));
    // Play가 아직 대기 중(미래 frame)인데 stop이 먼저 보이면 그 블록 시작에서 바로 끝나고 Play는
    // 무시된다.
    const AudioVoiceHandle waiting = PlayAt(rig, clip, 1000);
    REQUIRE(mixer.RequestStop(waiting, 300));
    RenderBlocks(rig, 4, allocator);
    AudioVoiceStatus status = StatusOf(mixer, playing);
    CHECK(status.endReason == AudioVoiceEndReason::Stopped && status.endMixFrame == 256 + 100);
    status = StatusOf(mixer, waiting);
    CHECK(status.endReason == AudioVoiceEndReason::Stopped && status.endMixFrame == 256);
    CHECK(status.startMixFrame == UnsetMixFrame && Ignored(mixer, AudioRtCommandType::Play) == 1);

    // 반복 stop: Stopping 중 남은 fade보다 긴 fade는 무시하고, 짧은 fade(0 = steal)는 받아들인다.
    const uint64 repeatedStart = mixer.MixFrame();
    const AudioVoiceHandle repeated = PlayAt(rig, clip, repeatedStart);
    RenderBlocks(rig, 1, allocator);
    REQUIRE(mixer.RequestStop(repeated, 600));
    RenderBlocks(rig, 1, allocator);
    REQUIRE(mixer.RequestStop(repeated, 500));
    RenderBlocks(rig, 2, allocator);
    CHECK(Ignored(mixer, AudioRtCommandType::Stop) == 1);
    CHECK(StatusOf(mixer, repeated).endMixFrame == repeatedStart + 256 + 600);
    const uint64 stolenStart = mixer.MixFrame();
    const AudioVoiceHandle stolen = PlayAt(rig, clip, stolenStart);
    RenderBlocks(rig, 1, allocator);
    REQUIRE(mixer.RequestStop(stolen, 1000));
    RenderBlocks(rig, 1, allocator);
    REQUIRE(mixer.RequestStop(stolen, 0));
    RenderBlocks(rig, 1, allocator);
    CHECK(StatusOf(mixer, stolen).endMixFrame == stolenStart + 512);

    // RT가 아는 것보다 오래된 generation의 mailbox는 stale로 센다.
    const uint64 staleBefore = mixer.Counters().staleCommands;
    constexpr uint32 GenerationShift = 32;
    constexpr uint64 SomeFade = 7;
    const uint64 oldValue =
        (static_cast<uint64>(stolen.generation - 1) << GenerationShift) | SomeFade;
    AudioMixerTestAccess::SetMailbox(mixer, stolen.index, oldValue);
    RenderBlocks(rig, 1, allocator);
    CHECK(mixer.Counters().staleCommands == staleBefore + 1);
}

TEST(Audio_Mixer2_StopAllEpoch) {
    SystemAllocator allocator;
    float ones[8192];
    for (float& value : ones) {
        value = 1.0f;
    }

    OfflineAudioRender rig = MakeRig(ConfigOf(), allocator);
    AudioMixer& mixer = rig.Mixer();
    const AudioClipId clip = RegisterOnes(mixer, ones, 8192);
    const AudioVoiceHandle a = PlayAt(rig, clip, 0);
    const AudioVoiceHandle b = PlayAt(rig, clip, 10);
    RenderBlocks(rig, 1, allocator);
    // StopAll 전에 만든 미래 Play(epoch가 작다)는 그 frame에 StoppedAll로 끝난다.
    const AudioVoiceHandle early = PlayAt(rig, clip, 600);
    mixer.StopAll(64);
    // StopAll 뒤에 만든 Play는 영향이 없다.
    const AudioVoiceHandle late = PlayAt(rig, clip, 700);
    RenderBlocks(rig, 3, allocator);
    for (const AudioVoiceHandle voice : {a, b}) {
        const AudioVoiceStatus status = StatusOf(mixer, voice);
        CHECK(status.endReason == AudioVoiceEndReason::StoppedAll &&
              status.endMixFrame == 256 + 64);
    }

    AudioVoiceStatus status = StatusOf(mixer, early);
    CHECK(status.endReason == AudioVoiceEndReason::StoppedAll && status.endMixFrame == 600);
    CHECK(status.startMixFrame == UnsetMixFrame);
    status = StatusOf(mixer, late);
    CHECK(status.state == AudioVoiceState::Playing && status.startMixFrame == 700);
}

TEST(Audio_Mixer2_TerminalAckAndControlRejection) {
    SystemAllocator allocator;
    float ones[600];
    for (float& value : ones) {
        value = 1.0f;
    }

    OfflineAudioRender rig = MakeRig(ConfigOf(2), allocator);
    AudioMixer& mixer = rig.Mixer();
    const AudioClipId clip = RegisterOnes(mixer, ones, 600);
    // 제어 거부: Reserved·stale.
    const AudioVoiceHandle reserved = mixer.AcquireVoice().Value();
    CHECK(mixer.PublishParams(reserved, Gain(1.0f)).Error() == ErrorCode::InvalidArgument);
    CHECK(mixer.RequestStop(reserved, 0).Error() == ErrorCode::InvalidArgument);
    CHECK(mixer.MakeStop(reserved, 0, 0).Error() == ErrorCode::InvalidArgument);
    const AudioVoiceHandle stale{reserved.index, reserved.generation + 7};
    CHECK(mixer.RequestStop(stale, 0).Error() == ErrorCode::StaleHandle);
    CHECK(mixer.MakeSetMasterGain(0, MaxMasterGain + 1.0f, 0).Error() ==
          ErrorCode::InvalidArgument);

    // 블록 시작 frame에서 즉시 끝난 voice도 그 블록이 끝나기 전(ACK < releaseMixFrame)에는 놓을 수
    // 없다.
    const AudioVoiceHandle voice = PlayAt(rig, clip, 0);
    auto stop = mixer.MakeStop(voice, 256, 0);
    REQUIRE(stop);
    SubmitOrDie(rig, stop.Value());
    RenderBlocks(rig, 2, allocator);
    AudioVoiceTerminal terminals[2];
    REQUIRE(mixer.CollectFinished(Span<AudioVoiceTerminal>(terminals)) == 1);
    CHECK(terminals[0].endMixFrame == 256);
    const AudioVoiceStatus status = StatusOf(mixer, voice);
    CHECK(status.releaseMixFrame == 512);
    AudioMixerTestAccess::SetCompletedMixFrame(mixer, 256);
    CHECK(mixer.ReleaseVoice(voice).Error() == ErrorCode::Busy);
    CHECK(mixer.UnregisterClip(clip).Error() == ErrorCode::Busy);
    AudioMixerTestAccess::SetCompletedMixFrame(mixer, 512);
    CHECK(mixer.ReleaseVoice(voice));
    CHECK(mixer.CollectFinished(Span<AudioVoiceTerminal>(terminals)) == 0);

    // 새 수명은 이전 terminal을 보지 않는다(generation 묶음). 손 Play의 잘못된 clip 묶음은
    // StartFailed다.
    REQUIRE(mixer.ReleaseVoice(reserved));
    const AudioVoiceHandle next = mixer.AcquireVoice().Value();
    REQUIRE(mixer.PreparePlay(next, clip, 0, false, AudioVoiceParams{}));
    CHECK(StatusOf(mixer, next).state == AudioVoiceState::Prepared);
    AudioRtCommand broken;
    broken.type = AudioRtCommandType::Play;
    broken.voice = next;
    broken.mixFrame = 600;
    broken.clipIndex = 99;
    SubmitOrDie(rig, broken);
    RenderBlocks(rig, 2, allocator);
    CHECK(StatusOf(mixer, next).endReason == AudioVoiceEndReason::StartFailed);

    // 계수 스냅샷은 블록 끝에 게시된다.
    CHECK(mixer.CountersSnapshot().staleCommands == mixer.Counters().staleCommands);
    CHECK(mixer.CompletedMixFrame() == mixer.MixFrame());
}

namespace {

constexpr uint32 TwoThreadVoices = 4;
constexpr uint32 TwoThreadBlocks = 400;
constexpr uint32 BlockFloats = 256 * 2;

// 두 스레드 시험의 공유 상태다. 반복 수가 정해져 있고 단계는 원자 gate와 ACK(CompletedMixFrame)로
// 맞춘다 (시간·sleep에 기대지 않는다).
struct TwoThreadRig {
    AudioMixer* mixer = nullptr;
    AudioVoiceHandle voices[TwoThreadVoices];
    uint32 gate = 0;
    // 제어 스레드가 출판을 멈추고 stop을 내는 RT 진행 frame이다.
    uint64 stopAtFrame = 0;
    // RT가 본 적용 파라미터 중 한 출판의 튜플이 아닌 것의 수, voice마다 terminal word가 바뀐 수다.
    uint32 tornTuples = 0;
    uint32 terminalChanges[TwoThreadVoices] = {};
    uint64 lastTerminal[TwoThreadVoices] = {};
    float output[BlockFloats] = {};
};

// 출판 k의 튜플: gain = k/1024, LPF = 1000 + k, HPF = 20 + k. 셋이 같은 k에서 나와야 한다.
AudioVoiceParams Tuple(uint32 k) noexcept {
    AudioVoiceParams params;
    params.gain = static_cast<float>(k) / 1024.0f;
    params.lowPassCutoffHz = 1000.0f + static_cast<float>(k);
    params.highPassCutoffHz = 20.0f + static_cast<float>(k);
    return params;
}

void ControlThread(void* context) noexcept {
    auto& rig = *static_cast<TwoThreadRig*>(context);
    while (platform::AtomicLoad<platform::MemoryOrder::Acquire>(&rig.gate) == 0) {
        platform::YieldThread();
    }

    // RT가 stopAtFrame에 닿을 때까지 출판한다. RT는 정해진 블록 수를 돌므로 이 루프는 반드시
    // 끝나고, 그 동안 두 스레드가 실제로 겹친다.
    for (uint32 k = 1; rig.mixer->CompletedMixFrame() < rig.stopAtFrame; ++k) {
        for (const AudioVoiceHandle voice : rig.voices) {
            (void)rig.mixer->PublishParams(voice, Tuple(k % 1024));
        }
    }

    (void)rig.mixer->RequestStop(rig.voices[0], 64);
    rig.mixer->StopAll(0);
}

void RenderThreadBody(void* context) noexcept {
    auto& rig = *static_cast<TwoThreadRig*>(context);
    for (uint32 block = 0; block < TwoThreadBlocks; ++block) {
        rig.mixer->RenderBlock(Span<const AudioRtCommand>(), Span<float>(rig.output));
        for (uint32 index = 0; index < TwoThreadVoices; ++index) {
            const uint32 voice = rig.voices[index].index;
            const AudioVoiceParams applied = AudioMixerTestAccess::AppliedParams(*rig.mixer, voice);
            const float k = applied.gain * 1024.0f;
            if (applied.lowPassCutoffHz - 1000.0f != k || applied.highPassCutoffHz - 20.0f != k) {
                ++rig.tornTuples;
            }

            const uint64 terminal = AudioMixerTestAccess::TerminalWord(*rig.mixer, voice);
            if (terminal != rig.lastTerminal[index]) {
                ++rig.terminalChanges[index];
                rig.lastTerminal[index] = terminal;
            }
        }
    }
}

void RenderThread(void* context) noexcept {
    auto& rig = *static_cast<TwoThreadRig*>(context);
    platform::AtomicStore<platform::MemoryOrder::Release>(&rig.gate, 1u);
    platform::RunWithRealtimeAudioFloatingPoint(&RenderThreadBody, context);
}

} // namespace

TEST(Audio_Mixer2_TwoThreadInvariants) {
    // 경계 상태의 정확성은 위 Inline 주입 시험이 맡는다. 여기서는 제어·RT 스레드를 실제로 겹쳐 돌려
    // 인터리빙과 무관한 불변식만 단언한다: 적용 파라미터는 늘 한 출판의 튜플이고, voice마다
    // terminal은 한 번 게시되며 사유가 맞다. Windows에는 TSan이 없고 x64는 TSO라 이 시험이 경쟁
    // 부재를 증명하지는 않는다.
    SystemAllocator allocator;
    float ones[64];
    for (float& value : ones) {
        value = 1.0f;
    }

    OfflineAudioRender offline = MakeRig(ConfigOf(), allocator);
    AudioMixer& mixer = offline.Mixer();
    const AudioClipId clip = RegisterOnes(mixer, ones, 64);
    TwoThreadRig rig;
    rig.mixer = &mixer;
    for (AudioVoiceHandle& voice : rig.voices) {
        voice = mixer.AcquireVoice().Value();
        auto play = mixer.PreparePlay(voice, clip, 0, true, Tuple(0));
        REQUIRE(play);
        play.Value().mixFrame = 0;
        SubmitOrDie(offline, play.Value());
    }

    RenderBlocks(offline, 1, allocator);
    rig.stopAtFrame = mixer.CompletedMixFrame() + 200 * 256;
    platform::Thread control;
    platform::Thread render;
    REQUIRE(control.Start(&ControlThread, &rig) == platform::ThreadStatus::Success);
    REQUIRE(render.Start(&RenderThread, &rig) == platform::ThreadStatus::Success);
    REQUIRE(render.Join() == platform::ThreadStatus::Success);
    REQUIRE(control.Join() == platform::ThreadStatus::Success);

    // RT가 마지막 stop을 못 봤을 수 있으니 한 블록을 더 돌린다.
    RenderBlocks(offline, 1, allocator);
    CHECK(rig.tornTuples == 0);
    for (uint32 index = 0; index < TwoThreadVoices; ++index) {
        CHECK(rig.terminalChanges[index] <= 1);
    }

    AudioVoiceTerminal terminals[8];
    CHECK(mixer.CollectFinished(Span<AudioVoiceTerminal>(terminals)) == TwoThreadVoices);
    for (uint32 index = 1; index < TwoThreadVoices; ++index) {
        CHECK(StatusOf(mixer, rig.voices[index]).endReason == AudioVoiceEndReason::StoppedAll);
    }

    const AudioVoiceEndReason first = StatusOf(mixer, rig.voices[0]).endReason;
    CHECK(first == AudioVoiceEndReason::Stopped || first == AudioVoiceEndReason::StoppedAll);
}

TEST(Audio_Mixer2_StopOrderingAndMinimumFade) {
    SystemAllocator allocator;
    float ones[8192];
    for (float& value : ones) {
        value = 1.0f;
    }

    OfflineAudioRender rig = MakeRig(ConfigOf(), allocator);
    AudioMixer& mixer = rig.Mixer();
    const AudioClipId clip = RegisterOnes(mixer, ones, 8192);
    const AudioVoiceHandle a = PlayAt(rig, clip, 0);
    const AudioVoiceHandle b = PlayAt(rig, clip, 0);
    const AudioVoiceHandle c = PlayAt(rig, clip, 0);
    // Prepared(앞 frame의 SetParams로 RT가 수명을 연 슬롯)도 stop-all에 즉시 끝난다.
    const AudioVoiceHandle prepared = PlayAt(rig, clip, 2000);
    auto early = mixer.MakeSetParams(prepared, 100, Gain(0.5f), 0);
    REQUIRE(early);
    SubmitOrDie(rig, early.Value());
    RenderBlocks(rig, 1, allocator);

    // b를 Stopping(fade 1000)으로 만든다. c는 fade 100.
    REQUIRE(mixer.RequestStop(b, 1000));
    REQUIRE(mixer.RequestStop(c, 100));
    REQUIRE(mixer.RequestStop(a, 100));
    // mailbox는 줄어든 값만 쓴다: c에 더 긴 512를 내도 100이 남는다.
    REQUIRE(mixer.RequestStop(c, 512));
    RenderBlocks(rig, 1, allocator);
    // 다음 블록: StopAll(0)이 Stopping b(남은 744)를 즉시 끝낸다(더 짧은 fade 수용).
    mixer.StopAll(0);
    // 아직 처리되지 않은 StopAll(0) 뒤의 StopAll(1000)은 0을 지우지 않는다.
    mixer.StopAll(1000);
    RenderBlocks(rig, 2, allocator);
    AudioVoiceStatus status = StatusOf(mixer, a);
    CHECK(status.endReason == AudioVoiceEndReason::Stopped && status.endMixFrame == 256 + 100);
    status = StatusOf(mixer, c);
    CHECK(status.endReason == AudioVoiceEndReason::Stopped && status.endMixFrame == 256 + 100);
    status = StatusOf(mixer, b);
    CHECK(status.endReason == AudioVoiceEndReason::StoppedAll && status.endMixFrame == 512);
    status = StatusOf(mixer, prepared);
    CHECK(status.endReason == AudioVoiceEndReason::StoppedAll && status.endMixFrame == 512);
    CHECK(status.startMixFrame == UnsetMixFrame);
    // stop-all이 건너뛴 voice(이미 끝남)는 무시 계수에 들어가지 않는다.
    CHECK(Ignored(mixer, AudioRtCommandType::Stop) == 0);

    // 같은 블록 처리 순서: StopAll(0) → mailbox. 새 voice에 둘을 함께 내면 StoppedAll로 끝난다.
    const AudioVoiceHandle both = PlayAt(rig, clip, mixer.MixFrame());
    RenderBlocks(rig, 1, allocator);
    REQUIRE(mixer.RequestStop(both, 100));
    mixer.StopAll(0);
    const uint64 bothFrame = mixer.MixFrame();
    RenderBlocks(rig, 1, allocator);
    status = StatusOf(mixer, both);
    CHECK(status.endReason == AudioVoiceEndReason::StoppedAll && status.endMixFrame == bothFrame);

    // mailbox의 0이 아닌 더 짧은 fade도 받아들인다.
    const AudioVoiceHandle shorter = PlayAt(rig, clip, mixer.MixFrame());
    RenderBlocks(rig, 1, allocator);
    REQUIRE(mixer.RequestStop(shorter, 600));
    const uint64 fadeStart = mixer.MixFrame();
    RenderBlocks(rig, 1, allocator);
    REQUIRE(mixer.RequestStop(shorter, 10));
    RenderBlocks(rig, 1, allocator);
    CHECK(StatusOf(mixer, shorter).endMixFrame == fadeStart + 256 + 10);
}

TEST(Audio_Mixer2_ControlRejectsAndClipRelease) {
    SystemAllocator allocator;
    float ones[64];
    for (float& value : ones) {
        value = 1.0f;
    }

    OfflineAudioRender rig = MakeRig(ConfigOf(2), allocator);
    AudioMixer& mixer = rig.Mixer();
    const AudioClipId clip = RegisterOnes(mixer, ones, 64);
    const AudioVoiceHandle reserved = mixer.AcquireVoice().Value();
    CHECK(mixer.MakeSetParams(reserved, 0, Gain(1.0f), 0).Error() == ErrorCode::InvalidArgument);
    const AudioVoiceHandle played = PlayAt(rig, clip, 0);
    const AudioVoiceHandle stale{played.index, played.generation + 1};
    CHECK(mixer.MakeStop(stale, 0, 0).Error() == ErrorCode::StaleHandle);
    CHECK(mixer.MakeSetParams(stale, 0, Gain(1.0f), 0).Error() == ErrorCode::StaleHandle);
    CHECK(mixer.PublishParams(stale, Gain(1.0f)).Error() == ErrorCode::StaleHandle);
    RenderBlocks(rig, 1, allocator);
    AudioVoiceTerminal terminals[2];
    REQUIRE(mixer.CollectFinished(Span<AudioVoiceTerminal>(terminals)) == 1);
    REQUIRE(mixer.ReleaseVoice(played));
    REQUIRE(mixer.ReleaseVoice(reserved));
    // 묶은 voice가 모두 풀리면 clip을 해제할 수 있다.
    CHECK(mixer.UnregisterClip(clip));
}

// 보내지 않은 Play 버리기(3.41.1절 — 명령 링이 차서 미뤘던 Play를 제어 쪽이 거둘 때): voice는
// Reserved로 돌아가 바로 놓이고 clip 사용 수가 내려가 해제된다. Reserved·정지 요청 뒤·낡은 handle은
// 거부한다. 버린 Play의 순번 틈은 다른 voice 재생에 영향이 없다.
// 읽지 않은 누적 seqlock이 계속 쓰는 중으로 보이면 조회는 읽기 실패를 알리고 값을 0으로 두지
// 않는다(제어 쪽이 커서를 진행하지 않게 — 3.44.6절). 다시 짝수면 정상으로 읽는다.
TEST(Audio_Mixer2_UnreadTornReadReported) {
    constexpr uint32 OddVersion = 1;
    constexpr uint32 EvenVersion = 2;
    SystemAllocator allocator;
    OfflineAudioRender rig = MakeRig(ConfigOf(), allocator);
    const float ones[1024] = {1.0f};
    const AudioClipId clip = RegisterOnes(rig.Mixer(), ones, 1024);
    AudioVoiceParams paused;
    paused.paused = true;
    const AudioVoiceHandle voice = PlayAt(rig, clip, 0);
    (void)RenderBlocks(rig, 1, allocator);
    REQUIRE(rig.Mixer().PublishParams(voice, paused));
    (void)RenderBlocks(rig, 1, allocator);
    const AudioVoiceStatus published = StatusOf(rig.Mixer(), voice);
    CHECK(!published.unreadReadFailed);
    REQUIRE(published.unreadAsOfFrame != UnsetMixFrame);

    AudioMixerTestAccess::SetUnreadVersion(rig.Mixer(), voice.index, OddVersion);
    const AudioVoiceStatus torn = StatusOf(rig.Mixer(), voice);
    CHECK(torn.unreadReadFailed);
    CHECK(torn.unreadAsOfFrame == UnsetMixFrame);

    AudioMixerTestAccess::SetUnreadVersion(rig.Mixer(), voice.index, EvenVersion);
    CHECK(!StatusOf(rig.Mixer(), voice).unreadReadFailed);
}

TEST(Audio_Mixer2_AbandonPreparedPlay) {
    SystemAllocator allocator;
    float ones[2048];
    for (float& value : ones) {
        value = 1.0f;
    }

    OfflineAudioRender rig = MakeRig(ConfigOf(), allocator);
    AudioMixer& mixer = rig.Mixer();
    const AudioClipId clip = RegisterOnes(mixer, ones, 2048);
    const AudioClipId abandonedClip = RegisterOnes(mixer, ones, 1024);
    auto reserved = mixer.AcquireVoice();
    REQUIRE(reserved);
    CHECK(mixer.AbandonPreparedPlay(reserved.Value()).Error() == ErrorCode::InvalidArgument);
    REQUIRE(mixer.PreparePlay(reserved.Value(), abandonedClip, 0, false, AudioVoiceParams{}));
    CHECK(mixer.ReleaseVoice(reserved.Value()).Error() == ErrorCode::Busy);
    CHECK(mixer.UnregisterClip(abandonedClip).Error() == ErrorCode::Busy);
    REQUIRE(mixer.AbandonPreparedPlay(reserved.Value()));
    REQUIRE(mixer.ReleaseVoice(reserved.Value()));
    // clip 사용 수가 내려가 해제된다.
    CHECK(mixer.UnregisterClip(abandonedClip));
    CHECK(mixer.AbandonPreparedPlay(reserved.Value()).Error() == ErrorCode::StaleHandle);
    // 정지 요청을 낸 voice는 이미 보낸 것으로 본다.
    auto stopped = mixer.AcquireVoice();
    REQUIRE(stopped);
    REQUIRE(mixer.PreparePlay(stopped.Value(), clip, 0, false, AudioVoiceParams{}));
    REQUIRE(mixer.RequestStop(stopped.Value(), 0));
    CHECK(mixer.AbandonPreparedPlay(stopped.Value()).Error() == ErrorCode::InvalidArgument);
    // 다른 voice의 재생은 틈과 상관없이 시작한다.
    const AudioVoiceHandle playing = PlayAt(rig, clip, 0);
    RenderBlocks(rig, 2, allocator);
    CHECK(StatusOf(mixer, playing).startMixFrame == 0);
}

} // namespace audio_mixer_boundary_tests_file
