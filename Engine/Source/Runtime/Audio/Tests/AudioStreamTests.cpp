#include "AudioTestSupport.h"

#include <Audio/AudioStreamRing.h>
#include <Audio/LinearResampler.h>
#include <Audio/OfflineAudioRender.h>
#include <Base/InitializerList.h>
#include <Core/Containers/Array.h>
#include <Core/Math/MathUtility.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/Fatal.h>
#include <Platform/PlatformMath.h>
#include <Platform/Sync/RealtimeWakeSignal.h>
#include <TestSupport/Test.h>

// AU6a1 스트림 원본 시험이다(Docs/Design/AudioSystem 3.40.1·3.40.4절): 링 프로토콜(감기·끝 상태·
// 비유한 거부·초기화), 리샘플러 OutputFramesFor, 손으로 채운 링이 상주 clip과 같은 PCM을 내는지,
// 굶음(낼 수 있는 만큼 + 무음, 0 구간만 끼운 열), 시작 대기(수위·짧은 clip), 굶은 Stopping voice의
// fade 끝, 끝·실패 사유, loop 감기, 등록·PreparePlay 규칙, refill 신호. 모두 OfflineAudioRender로
// 블록마다 직접 채워 결정적으로 렌더한다.
using namespace rocks;
namespace audio_stream_tests_file {

using namespace rocks::audio;

namespace {

constexpr uint32 Rate = DefaultMixSampleRate;
constexpr uint32 Rate44 = 44100;
constexpr uint32 Block = 256;
constexpr uint32 Capacity = StreamRingMinFrames;

AudioMixerConfig ConfigOf(platform::RealtimeWakeSignal* refill = nullptr) noexcept {
    AudioMixerConfig config;
    config.blockFrames = Block;
    config.masterLimiterEnabled = false;
    config.maxVoices = 8;
    config.maxClips = 8;
    config.streamRefillSignal = refill;
    return config;
}

OfflineAudioRender MakeRig(Allocator& allocator,
                           platform::RealtimeWakeSignal* refill = nullptr) noexcept {
    auto created = OfflineAudioRender::TryCreate(ConfigOf(refill), allocator);
    if (!created) {
        platform::Fatal("Audio stream test configuration was rejected");
    }

    return Move(created.Value());
}

// 0이 없는 결정적 신호다(굶은 무음 구간을 신호와 헷갈리지 않게).
Array<float> Signal(Allocator& allocator, uint32 frames) noexcept {
    Array<float> out(allocator);
    CHECK(out.TryResize(frames));
    for (uint32 frame = 0; frame < frames; ++frame) {
        out[frame] = 0.25f + 0.5f * static_cast<float>((frame * 37u) % 101u) / 101.0f;
    }

    return out;
}

UniquePtr<AudioStreamRing> MakeRing(Allocator& allocator, uint32 capacity = Capacity) noexcept {
    auto made = AudioStreamRing::TryCreate(allocator, 1, capacity);
    if (!made) {
        platform::Fatal("Audio stream test ring was not created");
    }

    return Move(made.Value());
}

// 링에 source[cursor..)를 빈 칸만큼(또는 limit까지) 쓴다. loop면 [loopBegin, loopEnd)를 감는다.
struct Feeder {
    const Array<float>* source = nullptr;
    uint64 cursor = 0;
    bool loop = false;
    uint32 loopBegin = 0;
    uint32 loopEnd = 0;

    uint32 Feed(AudioStreamRing& ring, uint32 limit = NumericLimits<uint32>::Max()) noexcept {
        uint32 written = 0;
        const uint32 end = loop ? loopEnd : static_cast<uint32>(source->Size());
        while (written < limit && ring.WritableFrames() > 0) {
            if (cursor >= end) {
                if (!loop) {
                    break;
                }

                cursor = loopBegin;
            }

            uint32 frames = static_cast<uint32>(end - cursor);
            const uint32 writable = ring.WritableFrames();
            frames = frames < writable ? frames : writable;
            frames = frames < limit - written ? frames : limit - written;
            PlanarConstView view;
            view.channelCount = 1;
            view.frameCount = frames;
            view.channels[0] = source->Data() + cursor;
            CHECK(ring.Write(view));
            cursor += frames;
            written += frames;
        }

        return written;
    }
};

AudioClipId RegisterStream(AudioMixer& mixer, AudioStreamRing& ring, uint32 frames,
                           uint32 rate = Rate, uint32 startFrame = 0, bool loop = false,
                           uint32 loopBegin = 0, uint32 loopEnd = 0) noexcept {
    AudioStreamClipDesc desc;
    desc.ring = &ring;
    desc.channelCount = 1;
    desc.frameCount = frames;
    desc.sampleRate = rate;
    desc.startFrame = startFrame;
    desc.loop = loop;
    desc.loopBegin = loopBegin;
    desc.loopEnd = loopEnd;
    auto registered = mixer.RegisterStreamClip(desc);
    if (!registered) {
        platform::Fatal("Audio stream test clip was rejected");
    }

    return registered.Value();
}

AudioClipId RegisterResident(AudioMixer& mixer, const Array<float>& source, uint32 rate = Rate,
                             uint32 loopBegin = 0, uint32 loopEnd = 0) noexcept {
    AudioClipDesc desc;
    desc.channelCount = 1;
    desc.channels[0] = source.Data();
    desc.frameCount = static_cast<uint32>(source.Size());
    desc.sampleRate = rate;
    desc.loopBegin = loopBegin;
    desc.loopEnd = loopEnd;
    auto registered = mixer.RegisterClip(desc);
    if (!registered) {
        platform::Fatal("Audio stream test resident clip was rejected");
    }

    return registered.Value();
}

AudioVoiceHandle PlayAt(OfflineAudioRender& rig, AudioClipId clip, uint64 frame,
                        uint32 startFrame = 0, bool loop = false) noexcept {
    auto voice = rig.Mixer().AcquireVoice();
    if (!voice) {
        platform::Fatal("Audio stream test voice was not acquired");
    }

    AudioVoiceParams params;
    auto play = rig.Mixer().PreparePlay(voice.Value(), clip, startFrame, loop, params);
    if (!play) {
        platform::Fatal("Audio stream test play was rejected");
    }

    play.Value().mixFrame = frame;
    if (!rig.Submit(play.Value())) {
        platform::Fatal("Audio stream test play was not submitted");
    }

    return voice.Value();
}

// 한 블록을 렌더해 왼쪽 채널을 out 뒤에 붙인다.
void RenderBlock(OfflineAudioRender& rig, Array<float>& out) noexcept {
    float block[Block * 2] = {};
    REQUIRE(rig.Render(Span<float>(block, Block * 2), 1));
    for (uint32 frame = 0; frame < Block; ++frame) {
        CHECK(out.TryPushBack(block[frame * 2]));
    }
}

AudioVoiceStatus StatusOf(const AudioMixer& mixer, AudioVoiceHandle voice) noexcept {
    auto status = mixer.QueryVoice(voice);
    if (!status) {
        platform::Fatal("Audio stream test voice query failed");
    }

    return status.Value();
}

// 상주 clip을 blocks 블록 렌더한 왼쪽 채널이다.
Array<float> RenderResident(Allocator& allocator, const Array<float>& source, uint32 rate,
                            uint32 blocks, bool loop = false, uint32 loopBegin = 0,
                            uint32 loopEnd = 0) noexcept {
    OfflineAudioRender rig = MakeRig(allocator);
    const AudioClipId clip = RegisterResident(rig.Mixer(), source, rate, loopBegin, loopEnd);
    (void)PlayAt(rig, clip, 0, 0, loop);
    Array<float> out(allocator);
    for (uint32 block = 0; block < blocks; ++block) {
        RenderBlock(rig, out);
    }

    return out;
}

} // namespace

// 링 프로토콜: 감기·Peek·Consume, 끝 상태 뒤 쓰기 거부, 비유한 거부(Failed), 초기화, 만들기 범위.
TEST(Audio_Stream_RingProtocol) {
    SystemAllocator allocator;
    CHECK(AudioStreamRing::TryCreate(allocator, 0, Capacity).Error() == ErrorCode::InvalidArgument);
    CHECK(AudioStreamRing::TryCreate(allocator, 3, Capacity).Error() == ErrorCode::InvalidArgument);
    CHECK(AudioStreamRing::TryCreate(allocator, 1, Capacity - 1).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(AudioStreamRing::TryCreate(allocator, 1, StreamRingMaxFrames + 1).Error() ==
          ErrorCode::InvalidArgument);
    UniquePtr<AudioStreamRing> ring = MakeRing(allocator);
    const Array<float> source = Signal(allocator, Capacity * 3);
    CHECK(ring->WritableFrames() == Capacity && ring->ReadableFrames() == 0);
    Feeder feeder;
    feeder.source = &source;
    CHECK(feeder.Feed(*ring) == Capacity);
    CHECK(ring->WritableFrames() == 0 && ring->ReadableFrames() == Capacity);
    // 감기: 일부 읽고 다시 채우면 앞에서부터 원본 순서다.
    Array<float> scratch(allocator);
    REQUIRE(scratch.TryResize(Capacity));
    PlanarView out;
    out.channelCount = 1;
    out.frameCount = Capacity;
    out.channels[0] = scratch.Data();
    ring->Consume(Capacity - 100);
    CHECK(feeder.Feed(*ring) == Capacity - 100);
    ring->Peek(Capacity, out);
    bool same = true;
    for (uint32 frame = 0; frame < Capacity; ++frame) {
        same = same && scratch[frame] == source[Capacity - 100 + frame];
    }

    CHECK(same);
    CHECK(ring->State() == AudioStreamState::Running);
    ring->MarkEnded();
    CHECK(ring->State() == AudioStreamState::Ended && ring->WritableFrames() == 0);
    ring->MarkFailed();
    CHECK(ring->State() == AudioStreamState::Ended);
    PlanarConstView one;
    one.channelCount = 1;
    one.frameCount = 0;
    one.channels[0] = source.Data();
    CHECK(ring->Write(one).IsOk());
    one.frameCount = 1;
    CHECK(ring->Write(one).Error() == ErrorCode::InvalidArgument);

    // 비유한 표본은 쓰지 않고 Failed다.
    ring->Reset(1);
    CHECK(ring->State() == AudioStreamState::Running && ring->ReadableFrames() == 0);
    const float bad[2] = {0.5f, platform::PlatformMath::QuietNaN<float>()};
    PlanarConstView badView;
    badView.channelCount = 1;
    badView.frameCount = 2;
    badView.channels[0] = bad;
    CHECK(ring->Write(badView).Error() == ErrorCode::CorruptData);
    CHECK(ring->State() == AudioStreamState::Failed && ring->ReadableFrames() == 0);
    PlanarConstView stereo = badView;
    stereo.channelCount = 2;
    ring->Reset(1);
    CHECK(ring->Write(stereo).Error() == ErrorCode::InvalidArgument);
}

// OutputFramesFor는 InputFramesNeeded의 역이다: 낼 수 있는 가장 큰 출력 frame 수.
TEST(Audio_Stream_OutputFramesFor) {
    for (const double ratio : {1.0, 44100.0 / 48000.0, 0.5, 1.75, 2.0}) {
        auto created = LinearResampler::TryCreate(1, ratio);
        REQUIRE(created);
        const LinearResampler& resampler = created.Value();
        for (uint32 outputs = 1; outputs <= 600; outputs += 37) {
            const uint32 needed = resampler.InputFramesNeeded(outputs).Value();
            CHECK(resampler.OutputFramesFor(needed, outputs) == outputs);
            CHECK(resampler.OutputFramesFor(needed, MaxBlockFrames) >= outputs);
            if (needed > 0) {
                CHECK(resampler.OutputFramesFor(needed - 1, MaxBlockFrames) < outputs);
            }
        }

        CHECK(resampler.OutputFramesFor(0, 10) == 0 ||
              resampler.InputFramesNeeded(resampler.OutputFramesFor(0, 10)).Value() == 0);
    }
}

// 링을 늘 채워 두면 상주 clip과 같은 PCM이다(44.1 kHz — 리샘플 경로, 시작 시각 같음).
TEST(Audio_Stream_FullRingMatchesResidentClip) {
    SystemAllocator allocator;
    const Array<float> source = Signal(allocator, 30000);
    constexpr uint32 Blocks = 140;
    const Array<float> expected = RenderResident(allocator, source, Rate44, Blocks);
    OfflineAudioRender rig = MakeRig(allocator);
    UniquePtr<AudioStreamRing> ring = MakeRing(allocator);
    Feeder feeder;
    feeder.source = &source;
    const AudioClipId clip = RegisterStream(rig.Mixer(), *ring, 30000, Rate44);
    const AudioVoiceHandle voice = PlayAt(rig, clip, 0);
    Array<float> actual(allocator);
    for (uint32 block = 0; block < Blocks; ++block) {
        (void)feeder.Feed(*ring);
        if (feeder.cursor >= source.Size()) {
            ring->MarkEnded();
        }

        RenderBlock(rig, actual);
    }

    REQUIRE(actual.Size() == expected.Size());
    bool same = true;
    for (usize index = 0; index < actual.Size(); ++index) {
        same = same && actual[index] == expected[index];
    }

    CHECK(same);
    const AudioVoiceStatus status = StatusOf(rig.Mixer(), voice);
    CHECK(status.startMixFrame == 0 && status.state == AudioVoiceState::Finished &&
          status.endReason == AudioVoiceEndReason::NaturalEnd);
    CHECK(rig.Mixer().Counters().streamStarvedFrames == 0);
}

// 굶음: 낼 수 있는 만큼 내고 나머지는 무음, 이어서 같은 자리부터다 — 출력은 상주 출력에 0 구간만
// 끼운 열과 bit까지 같다(param 일정). 무음 frame 수를 센다.
TEST(Audio_Stream_StarvationInsertsSilence) {
    SystemAllocator allocator;
    const Array<float> source = Signal(allocator, 30000);
    // 기대 출력은 스트림 렌더(최대 320 블록)보다 길게 만든다(비교가 넘지 않게).
    const Array<float> expected = RenderResident(allocator, source, Rate44, 400);
    OfflineAudioRender rig = MakeRig(allocator);
    UniquePtr<AudioStreamRing> ring = MakeRing(allocator);
    Feeder feeder;
    feeder.source = &source;
    const AudioClipId clip = RegisterStream(rig.Mixer(), *ring, 30000, Rate44);
    (void)PlayAt(rig, clip, 0);
    Array<float> actual(allocator);
    // 처음 5000 frame만 주고(시작 수위 4096을 넘긴다) 링이 빌 때까지 렌더한 뒤 세 블록을 더 굶긴다.
    (void)feeder.Feed(*ring, 5000);
    uint32 dryBlocks = 0;
    for (uint32 block = 0; block < 200 && dryBlocks < 3; ++block) {
        RenderBlock(rig, actual);
        if (rig.Mixer().Counters().streamStarvedFrames > 0) {
            ++dryBlocks;
        }
    }

    const uint64 starved = rig.Mixer().Counters().streamStarvedFrames;
    CHECK(starved > 0);
    for (uint32 block = 0; block < 120; ++block) {
        (void)feeder.Feed(*ring);
        if (feeder.cursor >= source.Size()) {
            ring->MarkEnded();
        }

        RenderBlock(rig, actual);
    }

    // 같은 앞부분 k frame → 무음 starved frame → expected[k..] 순이다.
    usize prefix = 0;
    REQUIRE(actual.Size() <= expected.Size());
    while (prefix < actual.Size() && actual[prefix] == expected[prefix]) {
        ++prefix;
    }

    REQUIRE(prefix > 0 && prefix + starved <= actual.Size());
    bool silent = true;
    for (usize index = prefix; index < prefix + starved; ++index) {
        silent = silent && actual[index] == 0.0f;
    }

    CHECK(silent);
    bool resumed = true;
    for (usize index = prefix + starved; index < actual.Size(); ++index) {
        resumed = resumed && actual[index] == expected[index - starved];
    }

    CHECK(resumed);
    CHECK(rig.Mixer().Counters().streamStarvedFrames == starved);
}

// 시작 대기: 링이 수위(StreamStartFrames)를 채울 때까지 소리·cursor가 없고 Prepared다. 채운
// 블록에서 시작하고 기다린 frame을 센다. 짧은 clip은 남은 frame 전체가 수위다. 기다리는 중 Stop은
// 바로 끝난다.
TEST(Audio_Stream_StartWaitsForWatermark) {
    SystemAllocator allocator;
    const Array<float> source = Signal(allocator, 20000);
    OfflineAudioRender rig = MakeRig(allocator);
    UniquePtr<AudioStreamRing> ring = MakeRing(allocator);
    Feeder feeder;
    feeder.source = &source;
    const AudioClipId clip = RegisterStream(rig.Mixer(), *ring, 20000);
    const AudioVoiceHandle voice = PlayAt(rig, clip, 0);
    Array<float> out(allocator);
    (void)feeder.Feed(*ring, StreamStartFrames - 1);
    RenderBlock(rig, out);
    RenderBlock(rig, out);
    CHECK(StatusOf(rig.Mixer(), voice).state == AudioVoiceState::Prepared);
    CHECK(rig.Mixer().Counters().streamStartDelayFrames == 2 * Block);
    bool silent = true;
    for (const float value : out) {
        silent = silent && value == 0.0f;
    }

    CHECK(silent);
    (void)feeder.Feed(*ring, 1);
    RenderBlock(rig, out);
    const AudioVoiceStatus started = StatusOf(rig.Mixer(), voice);
    CHECK(started.state == AudioVoiceState::Playing && started.startMixFrame == 2 * Block);
    // mono Direct 패닝은 양쪽 0.5다.
    CHECK(out[2 * Block] == 0.5f * source[0]);

    // 짧은 clip(1000 frame)은 1000 frame이면 시작한다.
    UniquePtr<AudioStreamRing> shortRing = MakeRing(allocator);
    Feeder shortFeeder;
    shortFeeder.source = &source;
    const AudioClipId shortClip = RegisterStream(rig.Mixer(), *shortRing, 1000);
    const AudioVoiceHandle shortVoice = PlayAt(rig, shortClip, rig.Mixer().MixFrame());
    (void)shortFeeder.Feed(*shortRing, 1000);
    RenderBlock(rig, out);
    CHECK(StatusOf(rig.Mixer(), shortVoice).state != AudioVoiceState::Prepared);

    // 기다리는 중 Stop(fade 있음)은 낸 소리가 없으므로 바로 끝난다.
    UniquePtr<AudioStreamRing> waitRing = MakeRing(allocator);
    const AudioClipId waitClip = RegisterStream(rig.Mixer(), *waitRing, 20000);
    const AudioVoiceHandle waiting = PlayAt(rig, waitClip, rig.Mixer().MixFrame());
    RenderBlock(rig, out);
    auto stop = rig.Mixer().MakeStop(waiting, rig.Mixer().MixFrame(), 512);
    REQUIRE(stop);
    REQUIRE(rig.Submit(stop.Value()));
    RenderBlock(rig, out);
    const AudioVoiceStatus stopped = StatusOf(rig.Mixer(), waiting);
    CHECK(stopped.state == AudioVoiceState::Finished &&
          stopped.endReason == AudioVoiceEndReason::Stopped);
}

// 굶은 Stopping voice도 fade 길이 뒤에 끝난다(생산자가 멈춰도 Stop·drain이 끝난다).
TEST(Audio_Stream_StopWhileStarvedFinishes) {
    SystemAllocator allocator;
    const Array<float> source = Signal(allocator, 30000);
    OfflineAudioRender rig = MakeRig(allocator);
    UniquePtr<AudioStreamRing> ring = MakeRing(allocator);
    Feeder feeder;
    feeder.source = &source;
    const AudioClipId clip = RegisterStream(rig.Mixer(), *ring, 30000);
    const AudioVoiceHandle voice = PlayAt(rig, clip, 0);
    (void)feeder.Feed(*ring, StreamStartFrames);
    Array<float> out(allocator);
    // 링을 다 쓰고 굶을 때까지 렌더한다.
    for (uint32 block = 0; block < 40 && rig.Mixer().Counters().streamStarvedFrames == 0; ++block) {
        RenderBlock(rig, out);
    }

    REQUIRE(rig.Mixer().Counters().streamStarvedFrames > 0);
    constexpr uint32 Fade = 600;
    const uint64 stopFrame = rig.Mixer().MixFrame();
    auto stop = rig.Mixer().MakeStop(voice, stopFrame, Fade);
    REQUIRE(stop);
    REQUIRE(rig.Submit(stop.Value()));
    for (uint32 block = 0; block < 4; ++block) {
        RenderBlock(rig, out);
    }

    const AudioVoiceStatus status = StatusOf(rig.Mixer(), voice);
    CHECK(status.state == AudioVoiceState::Finished &&
          status.endReason == AudioVoiceEndReason::Stopped &&
          status.endMixFrame == stopFrame + Fade);
}

// 끝·실패: Ended 뒤 남은 PCM을 다 내면 자연 끝(상주와 같은 frame), Ended인데 모자라면 StreamFailed,
// Failed면 남은 PCM을 낸 뒤 StreamFailed다.
TEST(Audio_Stream_EndAndFailure) {
    SystemAllocator allocator;
    const Array<float> source = Signal(allocator, 6000);
    OfflineAudioRender resident = MakeRig(allocator);
    const AudioClipId residentClip = RegisterResident(resident.Mixer(), source);
    const AudioVoiceHandle residentVoice = PlayAt(resident, residentClip, 0);
    Array<float> residentOut(allocator);
    for (uint32 block = 0; block < 30; ++block) {
        RenderBlock(resident, residentOut);
    }

    const uint64 naturalEnd = StatusOf(resident.Mixer(), residentVoice).endMixFrame;

    const auto run = [&](uint32 feed, bool failed) noexcept {
        OfflineAudioRender rig = MakeRig(allocator);
        UniquePtr<AudioStreamRing> ring = MakeRing(allocator);
        Feeder feeder;
        feeder.source = &source;
        const AudioClipId clip = RegisterStream(rig.Mixer(), *ring, 6000);
        const AudioVoiceHandle voice = PlayAt(rig, clip, 0);
        (void)feeder.Feed(*ring, feed);
        if (failed) {
            ring->MarkFailed();
        } else {
            ring->MarkEnded();
        }

        Array<float> out(allocator);
        for (uint32 block = 0; block < 30; ++block) {
            RenderBlock(rig, out);
        }

        return StatusOf(rig.Mixer(), voice);
    };

    const AudioVoiceStatus ended = run(6000, false);
    CHECK(ended.endReason == AudioVoiceEndReason::NaturalEnd && ended.endMixFrame == naturalEnd);
    const AudioVoiceStatus short_ = run(5000, false);
    CHECK(short_.endReason == AudioVoiceEndReason::StreamFailed);
    const AudioVoiceStatus failed = run(5000, true);
    CHECK(failed.endReason == AudioVoiceEndReason::StreamFailed && failed.endMixFrame >= 4900 &&
          failed.endMixFrame < naturalEnd);
}

// loop: 생산자가 loop 구간을 감아 쓰면 상주 loop clip과 같은 PCM이다(여러 바퀴, 시작 frame ≠ 0).
TEST(Audio_Stream_LoopWraps) {
    SystemAllocator allocator;
    const Array<float> source = Signal(allocator, 9000);
    constexpr uint32 LoopBegin = 2000;
    constexpr uint32 LoopEnd = 5000;
    constexpr uint32 Blocks = 120;
    OfflineAudioRender resident = MakeRig(allocator);
    const AudioClipId residentClip =
        RegisterResident(resident.Mixer(), source, Rate44, LoopBegin, LoopEnd);
    (void)PlayAt(resident, residentClip, 0, 100, true);
    Array<float> expected(allocator);
    for (uint32 block = 0; block < Blocks; ++block) {
        RenderBlock(resident, expected);
    }

    OfflineAudioRender rig = MakeRig(allocator);
    UniquePtr<AudioStreamRing> ring = MakeRing(allocator);
    Feeder feeder;
    feeder.source = &source;
    feeder.cursor = 100;
    feeder.loop = true;
    feeder.loopBegin = LoopBegin;
    feeder.loopEnd = LoopEnd;
    const AudioClipId clip =
        RegisterStream(rig.Mixer(), *ring, 9000, Rate44, 100, true, LoopBegin, LoopEnd);
    (void)PlayAt(rig, clip, 0, 100, true);
    Array<float> actual(allocator);
    for (uint32 block = 0; block < Blocks; ++block) {
        (void)feeder.Feed(*ring);
        RenderBlock(rig, actual);
    }

    bool same = actual.Size() == expected.Size();
    for (usize index = 0; same && index < actual.Size(); ++index) {
        same = actual[index] == expected[index];
    }

    CHECK(same);
}

// 등록·PreparePlay 규칙: 링 채널 불일치·시작 frame 범위 밖은 등록 거부, 등록 값과 다른 시작·loop와
// 두 번째 voice는 InvalidArgument, 묶인 동안 해제는 Busy.
TEST(Audio_Stream_RegistrationRules) {
    SystemAllocator allocator;
    OfflineAudioRender rig = MakeRig(allocator);
    AudioMixer& mixer = rig.Mixer();
    UniquePtr<AudioStreamRing> ring = MakeRing(allocator);
    AudioStreamClipDesc desc;
    desc.ring = ring.Get();
    desc.channelCount = 2;
    desc.frameCount = 1000;
    desc.sampleRate = Rate;
    CHECK(mixer.RegisterStreamClip(desc).Error() == ErrorCode::InvalidArgument);
    desc.channelCount = 1;
    desc.startFrame = 1000;
    CHECK(mixer.RegisterStreamClip(desc).Error() == ErrorCode::InvalidArgument);
    desc.startFrame = 10;
    desc.loop = true;
    desc.loopBegin = 0;
    desc.loopEnd = 10;
    CHECK(mixer.RegisterStreamClip(desc).Error() == ErrorCode::InvalidArgument);
    desc.loop = false;
    desc.loopEnd = 0;
    desc.ring = nullptr;
    CHECK(mixer.RegisterStreamClip(desc).Error() == ErrorCode::InvalidArgument);
    desc.ring = ring.Get();
    auto clip = mixer.RegisterStreamClip(desc);
    REQUIRE(clip);
    auto voice = mixer.AcquireVoice();
    REQUIRE(voice);
    AudioVoiceParams params;
    CHECK(mixer.PreparePlay(voice.Value(), clip.Value(), 0, false, params).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(mixer.PreparePlay(voice.Value(), clip.Value(), 10, true, params).Error() ==
          ErrorCode::InvalidArgument);
    REQUIRE(mixer.PreparePlay(voice.Value(), clip.Value(), 10, false, params));
    auto second = mixer.AcquireVoice();
    REQUIRE(second);
    CHECK(mixer.PreparePlay(second.Value(), clip.Value(), 10, false, params).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(mixer.UnregisterClip(clip.Value()).Error() == ErrorCode::Busy);
}

// refill 신호: Running 링이 수위(용량의 절반) 아래일 때만 블록 끝에 울린다. 끝 상태 링은 울리지
// 않는다.
TEST(Audio_Stream_RefillSignal) {
    SystemAllocator allocator;
    platform::RealtimeWakeSignal refill;
    REQUIRE(refill.Initialize());
    const Array<float> source = Signal(allocator, 40000);
    OfflineAudioRender rig = MakeRig(allocator, &refill);
    UniquePtr<AudioStreamRing> ring = MakeRing(allocator);
    Feeder feeder;
    feeder.source = &source;
    const AudioClipId clip = RegisterStream(rig.Mixer(), *ring, 40000);
    (void)PlayAt(rig, clip, 0);
    (void)feeder.Feed(*ring);
    Array<float> out(allocator);
    RenderBlock(rig, out);
    // 가득 찬 링(수위 위)은 울리지 않는다.
    CHECK(refill.Wait(0) == platform::WaitResult::TimedOut);
    // 수위 아래까지 읽히면 울린다.
    while (ring->ReadableFrames() >= Capacity / 2) {
        RenderBlock(rig, out);
    }

    CHECK(refill.Wait(0) == platform::WaitResult::Signaled);
    ring->MarkEnded();
    (void)refill.Wait(0);
    RenderBlock(rig, out);
    CHECK(refill.Wait(0) == platform::WaitResult::TimedOut);
}

} // namespace audio_stream_tests_file
