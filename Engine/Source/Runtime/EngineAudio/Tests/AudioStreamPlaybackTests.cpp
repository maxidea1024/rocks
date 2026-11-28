#include "AudioEngineTestSupport.h"

#include <Audio/AudioDecoder.h>
#include <Audio/AudioPlatformData.h>
#include <Audio/AudioStreamService.h>
#include <AudioAssets/SoundStreamData.h>
#include <AudioTools/AudioEncoder.h>
#include <AudioTools/AudioPlatformDataWriter.h>
#include <AudioTools/OpusAudioEncoder.h>
#include <Base/InitializerList.h>
#include <Core/Containers/Array.h>
#include <Core/Math/MathUtility.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/Atomics.h>
#include <Platform/Fatal.h>
#include <TestSupport/Test.h>

// AU6a3 엔진 스트리밍 재생 시험이다(Docs/Design/AudioSystem 3.40.3·3.40.4절): 스트림 재생이 같은
// 데이터를 통째로 푼 PCM 재생과 같은 출력을 낸다(2D·공간, 시작 시각·끝 사유·굶음 없음), Thread
// 디코드 서비스(두 단계 회수)도 Inline과 같다, 재생을 100번 다시 열어도 할당이 늘지 않는다(슬롯·링
// 재사용), 슬롯이 다 차면 VoiceLimit·streamSlotRejects, 재생 중 엔진 종료가 막히지 않는다. 렌더는
// Manual, Null은 Manual이다. Thread 방식은 FlushControl·서비스 Flush gate로만 순서를 만든다(시간
// 대기 없음).
using namespace rocks;
namespace audio_stream_playback_tests_file {

using namespace rocks::engine_audio_test;

namespace {

constexpr double TwoPi = 6.283185307179586;
constexpr double ToneAmplitude = 0.3;
// 청크를 여러 개로 나누는 목표 byte다.
constexpr uint32 SmallChunkBytes = 4096;
// 시험 프레임 상한(1–2초 clip을 끝까지 낸다).
constexpr uint32 MaxStreamFrames = 400;
// 공간 재생 위치(cm)다: 기본 감쇠 inner 반경 400 cm 안, 오른쪽.
constexpr double SpatialOffset = 300.0;

// 살아 있는 할당 수·그 최대값·가장 큰 요청 byte를 세는 래퍼다(엔진 쪽 스레드가 쓸 수 있어 원자
// 연산이다).
class CountingAllocator final : public Allocator {
public:
    explicit CountingAllocator(Allocator& allocator) noexcept : allocator_(allocator) {
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        auto made = allocator_.Allocate(bytes, alignment, tag);
        if (made) {
            const int64 live =
                platform::AtomicFetchAdd<platform::MemoryOrder::Relaxed>(&live_, int64{1}) + 1;
            RaiseTo(&peak_, live);
            RaiseTo(&largest_, static_cast<int64>(bytes));
        }

        return made;
    }

    Result<void> Deallocate(void* address) noexcept override {
        if (address != nullptr) {
            (void)platform::AtomicFetchSub<platform::MemoryOrder::Relaxed>(&live_, int64{1});
        }

        return allocator_.Deallocate(address);
    }

    void TrimMemory() noexcept override {
        allocator_.TrimMemory();
    }

    void MarkThreadIdle() noexcept override {
        allocator_.MarkThreadIdle();
    }

    void MarkThreadActive() noexcept override {
        allocator_.MarkThreadActive();
    }

    int64 Live() const noexcept {
        return platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&live_);
    }

    int64 Peak() const noexcept {
        return platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&peak_);
    }

    int64 Largest() const noexcept {
        return platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&largest_);
    }

    // 가장 큰 요청 기록을 지운다(준비 할당 뒤 재생 구간만 본다).
    void ResetLargest() noexcept {
        platform::AtomicStore<platform::MemoryOrder::Relaxed>(&largest_, int64{0});
    }

private:
    static void RaiseTo(int64* target, int64 value) noexcept {
        int64 seen = platform::AtomicLoad<platform::MemoryOrder::Relaxed>(target);
        while (seen < value &&
               !platform::AtomicCompareExchangeStrong<platform::MemoryOrder::Relaxed,
                                                      platform::MemoryOrder::Relaxed>(target, seen,
                                                                                      value)) {
        }
    }

    Allocator& allocator_;
    int64 live_ = 0;
    int64 peak_ = 0;
    int64 largest_ = 0;
};

// 엔진 묶음 시작 때 Opus 디코더가 표에 들어오게 디코더 등록부를 올린다(엔진보다 먼저 만들고 뒤에
// 지운다 — 3.37.1절).
class OpusRegistryScope {
public:
    OpusRegistryScope() noexcept {
        if (!test::ProcessModules().Services().Register(registry_).IsOk()) {
            platform::Fatal("Audio stream playback test registry was not registered");
        }
    }

    ~OpusRegistryScope() noexcept {
        (void)test::ProcessModules().Services().Unregister(registry_);
    }

    OpusRegistryScope(const OpusRegistryScope&) = delete;
    OpusRegistryScope& operator=(const OpusRegistryScope&) = delete;

private:
    audio::AudioDecoderRegistry registry_;
};

// mono Opus 플랫폼 데이터(순음) byte다.
Array<uint8> MakeStreamBytes(Allocator& allocator, uint32 frames) noexcept {
    Array<float> samples(allocator);
    if (!samples.TryResize(frames)) {
        platform::Fatal("Audio stream playback test signal allocation failed");
    }

    for (uint32 frame = 0; frame < frames; ++frame) {
        samples[frame] = static_cast<float>(
            ToneAmplitude * math::Sin(TwoPi * 440.0 * frame / audio::OpusPlatformSampleRate));
    }

    audio::PlanarConstView view;
    view.channelCount = 1;
    view.frameCount = frames;
    view.channels[0] = samples.Data();
    audio::OpusAudioEncoder encoder(allocator);
    audio::EncodedAudio encoded(allocator);
    Array<uint8> bytes(allocator);
    audio::AudioPlatformDataOptions options;
    options.targetChunkBytes = SmallChunkBytes;
    if (!encoder.Encode(view, audio::OpusPlatformSampleRate, audio::DefaultAudioChannelMask(1), {},
                        encoded) ||
        !audio::WriteAudioPlatformData(encoded, options, bytes)) {
        platform::Fatal("Audio stream playback test encode failed");
    }

    return bytes;
}

// 그 스트림 데이터다(상주 — 플랫폼 데이터 전체).
SharedPtr<const SoundStreamData> MakeStreamData(Allocator& allocator, uint32 frames) noexcept {
    const Array<uint8> bytes = MakeStreamBytes(allocator, frames);
    auto made = SoundStreamData::TryCreate(allocator, bytes.View());
    if (!made) {
        platform::Fatal("Audio stream playback test stream data was rejected");
    }

    return SharedPtr<const SoundStreamData>(Move(made.Value()));
}

// 같은 데이터를 통째로 푼 PCM이다.
SharedPtr<const SoundPcm> DecodePcm(Allocator& allocator, const SoundStreamData& data,
                                    const audio::AudioDecoderTable& decoders) noexcept {
    audio::DecodedAudioPcm decoded(allocator);
    if (!audio::DecodeAudioPlatformData(data.View(), decoders, allocator, decoded)) {
        platform::Fatal("Audio stream playback test decode failed");
    }

    auto pcm = SoundPcm::TryAllocate(allocator, 1, static_cast<uint32>(decoded.FrameCount()),
                                     decoded.SampleRate());
    if (!pcm) {
        platform::Fatal("Audio stream playback test PCM allocation failed");
    }

    Span<float> target = pcm.Value()->MutableChannel(0);
    for (uint32 frame = 0; frame < target.Size(); ++frame) {
        target[frame] = decoded.Channel(0)[frame];
    }

    return SharedPtr<const SoundPcm>(Move(pcm.Value()));
}

SoundSourceRef StreamSound(Allocator& allocator, const SharedPtr<const SoundStreamData>& data,
                           bool looping = false,
                           const Optional<SoundAttenuation>& attenuation = {}) noexcept {
    SoundPlayable playable;
    playable.stream = data;
    playable.looping = looping;
    playable.attenuation = attenuation;
    auto made = SoundSourceRef::TryCreate(allocator, playable);
    return made ? made.Value() : SoundSourceRef{};
}

// 프레임 하나다. Thread 제어 방식이면 제어 묶음 처리와 디코드 서비스 채우기를 gate로 기다린 뒤
// 렌더한다(채운 링은 한 프레임 소비량보다 커서 렌더 중 굶지 않는다).
bool StepFrame(AudioEngine& fixture, AudioControlMode mode) noexcept {
    if (mode == AudioControlMode::Inline) {
        return fixture.Frame();
    }

    if (!fixture.engine.RunFrame()) {
        return false;
    }

    fixture.audio->FlushControl();
    if (!fixture.audio->StreamService()->Flush()) {
        return false;
    }

    PumpDevice(fixture.audio, PumpFrames);
    return true;
}

// 끝날 때까지(또는 상한까지) 프레임을 돈다.
bool RunUntilEnded(AudioEngine& fixture, AudioControlMode mode, const EventLog& log,
                   uint32 ended = 1) noexcept {
    for (uint32 frame = 0; frame < MaxStreamFrames; ++frame) {
        if (log.Count(PlaybackEventKind::Ended) >= ended) {
            return true;
        }

        if (!StepFrame(fixture, mode)) {
            return false;
        }
    }

    return log.Count(PlaybackEventKind::Ended) >= ended;
}

// voice·clip·스트림 슬롯이 모두 풀릴 때까지 돈다(Thread 서비스면 Cancel 뒤 반납을 Flush로 본다).
bool RunUntilStreamsIdle(AudioEngine& fixture,
                         AudioControlMode mode = AudioControlMode::Inline) noexcept {
    for (uint32 frame = 0; frame < MaxTestFrames; ++frame) {
        const AudioControlStats stats = fixture.audio->ControlStats();
        if (stats.activeVoices == 0 && stats.stoppingVoices == 0 && stats.voicelessPlaybacks == 0 &&
            stats.virtualPlaybacks == 0 && stats.releasingVoices == 0 &&
            stats.registeredClips == 0 && stats.activeStreams == 0) {
            return true;
        }

        if (!StepFrame(fixture, mode)) {
            return false;
        }
    }

    return false;
}

// 살아 있는 voice가 expectedVoices이고 분리·놓기·스트림 회수가 끝날 때까지 돈다(voice 없는 재생은
// 남는다).
bool RunUntilStreamsIdleExcept(AudioEngine& fixture, uint32 expectedVoices) noexcept {
    for (uint32 frame = 0; frame < MaxTestFrames; ++frame) {
        const AudioControlStats stats = fixture.audio->ControlStats();
        if (stats.activeVoices == expectedVoices && stats.stoppingVoices == 0 &&
            stats.releasingVoices == 0 && stats.activeStreams == 0) {
            return true;
        }

        if (!fixture.Frame()) {
            return false;
        }
    }

    return false;
}

// 한 번의 재생 결과다.
struct PlaybackRun {
    explicit PlaybackRun(Allocator& allocator) noexcept : capture(allocator), events(allocator) {
    }

    Array<float> capture;
    Array<PlaybackEvent> events;
};

// 1 s mono 순음을 스트림(stream=true) 또는 통째 PCM으로 끝까지 재생하고 출력·이벤트를 모은다.
// spatial이면 scene의 오른쪽 위치에서 감쇠 재생(UpdateSpatial 경로)이다.
void RunPlayback(AudioControlMode mode, bool stream, bool spatial, PlaybackRun& out) noexcept {
    constexpr uint32 Frames = audio::OpusPlatformSampleRate;
    OpusRegistryScope opus;
    AudioEngineHostOptions options = DeterministicOptions(true);
    options.controlMode = mode;
    AudioEngine fixture(options);
    REQUIRE(fixture.initialized && fixture.audio->Device() != nullptr);
    REQUIRE(fixture.audio->StreamService() != nullptr);
    CHECK((fixture.audio->StreamService()->Mode() == audio::AudioStreamServiceMode::Inline) ==
          (mode == AudioControlMode::Inline));
    REQUIRE(out.capture.TryResize(static_cast<usize>(MaxStreamFrames) * PumpFrames * 2));
    fixture.audio->Device()->NullOutput()->CaptureTo(
        Span<float>(out.capture.Data(), out.capture.Size()));
    const SharedPtr<const SoundStreamData> data = MakeStreamData(fixture.allocator, Frames);
    Optional<SoundAttenuation> attenuation;
    if (spatial) {
        attenuation = SoundAttenuation{};
    }

    SoundSourceRef sound;
    if (stream) {
        sound = StreamSound(fixture.allocator, data, false, attenuation);
        CHECK(!sound.Get()->pcm && sound.Get()->FrameCount() == Frames);
    } else {
        sound = MakeSound(fixture.allocator,
                          DecodePcm(fixture.allocator, *data, fixture.audio->DecoderTable()), false,
                          0, 0, attenuation);
    }

    REQUIRE(sound.IsValid());
    EventLog log(fixture.allocator);
    PlayOptions play;
    play.onEvent = log.Callback(fixture.allocator);
    if (spatial) {
        const AudioSceneId scene = fixture.audio->RegisterScene();
        REQUIRE(
            fixture.audio->PlayAtInScene(scene, sound, Vec3d(0.0, SpatialOffset, 0.0), Move(play)));
    } else {
        REQUIRE(fixture.audio->Play2D(sound, Move(play)));
    }

    REQUIRE(RunUntilEnded(fixture, mode, log));
    for (const PlaybackEvent& event : log.events) {
        CHECK(out.events.TryPushBack(event));
    }

    CHECK(fixture.audio->Device()->Mixer().CountersSnapshot().streamStarvedFrames == 0);
    REQUIRE(RunUntilStreamsIdle(fixture, mode));
    CHECK(fixture.audio->ControlStats().activeStreams == 0);
    CHECK(fixture.audio->StreamService()->AcquiredSlots() == 0);
}

// 두 실행의 이벤트(시작·끝 시각·사유)와 출력이 bit까지 같다. 출력에 소리가 있다.
void CheckSamePlayback(const PlaybackRun& left, const PlaybackRun& right) noexcept {
    REQUIRE(left.events.Size() == 2 && right.events.Size() == 2);
    CHECK(left.events[0].kind == PlaybackEventKind::Started &&
          left.events[1].reason == PlaybackEndReason::NaturalEnd);
    CHECK(left.events[0].mixFrame == right.events[0].mixFrame &&
          left.events[1].mixFrame == right.events[1].mixFrame);
    CHECK(left.events[1].reason == right.events[1].reason);
    bool same = left.capture.Size() == right.capture.Size();
    uint32 loud = 0;
    for (usize index = 0; same && index < left.capture.Size(); ++index) {
        same = left.capture[index] == right.capture[index];
        loud += math::Abs(left.capture[index]) > 0.01f ? 1u : 0u;
    }

    CHECK(same);
    // 1 s 순음이 한 채널 이상에서 대부분 들린다(공간 재생은 오른쪽만).
    CHECK(loud > audio::OpusPlatformSampleRate / 2);
}

} // namespace

// 스트림 재생은 같은 데이터를 통째로 푼 PCM 재생과 같은 출력·시작·끝이다(제어 묶음이 펌프해 굶지
// 않는다). 재생 PCM은 만들지 않고(스냅숏에 pcm 없음), 끝나면 슬롯·clip이 모두 풀린다.
TEST(EngineAudio_StreamPlaybackMatchesDecodedPcm) {
    SystemAllocator allocator;
    PlaybackRun stream(allocator);
    PlaybackRun pcm(allocator);
    RunPlayback(AudioControlMode::Inline, true, false, stream);
    RunPlayback(AudioControlMode::Inline, false, false, pcm);
    CheckSamePlayback(stream, pcm);
}

// 공간 재생(감쇠·패닝 — UpdateSpatial이 스트림 원본의 채널 수를 쓴다)도 통째 PCM 재생과 같다.
TEST(EngineAudio_StreamSpatialPlaybackMatchesDecodedPcm) {
    SystemAllocator allocator;
    PlaybackRun stream(allocator);
    PlaybackRun pcm(allocator);
    RunPlayback(AudioControlMode::Inline, true, true, stream);
    RunPlayback(AudioControlMode::Inline, false, true, pcm);
    CheckSamePlayback(stream, pcm);
}

// Thread 제어 방식은 Thread 디코드 서비스를 쓴다: 디코드 스레드가 채운 출력이 Inline과 같고,
// voice가 끝나면 Cancel → 디코드 스레드 반납 → 슬롯 회수의 두 단계 회수가 끝난다(Flush gate).
TEST(EngineAudio_StreamThreadServiceMatchesInline) {
    SystemAllocator allocator;
    PlaybackRun threaded(allocator);
    PlaybackRun inlined(allocator);
    RunPlayback(AudioControlMode::Thread, true, false, threaded);
    RunPlayback(AudioControlMode::Inline, true, false, inlined);
    CheckSamePlayback(threaded, inlined);
}

// 재생을 100번(시작 frame을 바꿔 가며) 다시 열고 멈춰도 할당이 늘지 않는다 — 슬롯 링·서비스는
// 처음에 잡고, 재생마다 잡는 디코더·공급자는 회수 때 모두 놓는다(3.40.4절 "100회 seek 메모리
// 상한"). 살아 있는 할당 수와 그 최대값이 처음 두 재생 뒤와 같고, 재생 구간의 가장 큰 요청도 clip
// PCM 크기보다 작다(재생 PCM을 만들지 않는다).
TEST(EngineAudio_StreamReplayHasNoGrowth) {
    SystemAllocator system;
    CountingAllocator counting(system);
    OpusRegistryScope opus;
    AudioEngine fixture(DeterministicOptions(true), nullptr, &counting);
    REQUIRE(fixture.initialized);
    const SharedPtr<const SoundStreamData> data =
        MakeStreamData(fixture.allocator, audio::OpusPlatformSampleRate * 2);
    const SoundSourceRef sound = StreamSound(fixture.allocator, data);
    REQUIRE(sound.IsValid());
    const auto replay = [&](uint32 index) noexcept {
        PlayOptions options;
        options.startFrame = (index * 7919u) % (audio::OpusPlatformSampleRate * 2 - 1);
        auto played = fixture.audio->Play2D(sound, Move(options));
        REQUIRE(played);
        REQUIRE(fixture.Frames(2));
        REQUIRE(fixture.audio->Stop(played.Value(), 0));
        REQUIRE(RunUntilStreamsIdle(fixture));
    };

    replay(0);
    replay(1);
    const int64 live = counting.Live();
    const int64 peak = counting.Peak();
    counting.ResetLargest();
    for (uint32 index = 2; index < 102; ++index) {
        replay(index);
    }

    CHECK(counting.Live() == live);
    CHECK(counting.Peak() == peak);
    const int64 pcmBytes = int64{audio::OpusPlatformSampleRate} * 2 * int64{sizeof(float)};
    CHECK(counting.Largest() > 0 && counting.Largest() < pcmBytes);
    CHECK(fixture.audio->ControlStats().activeStreams == 0);
}

// 슬롯이 다 차면 새 스트림 one-shot은 VoiceLimit로 끝나고 streamSlotRejects가 오른다(Ignored는 장치
// 없음이라 따로 둔다). 새 스트림 loop는 끝나지 않고 voice 없이 남아 묶음마다 다시 시도하고
// (resumeFailures), 슬롯이 풀리면 시작한다(3.41.1절). voice를 먼저 잡았다가 놓으므로 voice 계수는
// 그대로다.
TEST(EngineAudio_StreamSlotsExhausted) {
    AudioEngineHostOptions options = DeterministicOptions(true);
    options.streamSlots = 1;
    OpusRegistryScope opus;
    AudioEngine fixture(options);
    REQUIRE(fixture.initialized);
    const SharedPtr<const SoundStreamData> data =
        MakeStreamData(fixture.allocator, audio::OpusPlatformSampleRate);
    const SoundSourceRef loop = StreamSound(fixture.allocator, data, true);
    const SoundSourceRef shot = StreamSound(fixture.allocator, data, false);
    EventLog log(fixture.allocator);
    // 긴 정지 fade 중인 PCM voice: 스트림 슬롯이 모자라도 이것을 끊지 않는다(끊어도 슬롯이 생기지
    // 않는다 — 3.41.1절 여유 부족).
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.01f, TestRate);
    auto fading = fixture.audio->Play2D(MakeSound(fixture.allocator, pcm, true));
    REQUIRE(fading);
    REQUIRE(fixture.Frames(2));
    REQUIRE(fixture.audio->Stop(fading.Value(), TestRate * 10));
    PlayOptions first;
    first.onEvent = log.Callback(fixture.allocator);
    auto playing = fixture.audio->Play2D(loop, Move(first));
    REQUIRE(playing);
    PlayOptions second;
    second.onEvent = log.Callback(fixture.allocator);
    REQUIRE(fixture.audio->Play2D(shot, Move(second)));
    PlayOptions third;
    third.onEvent = log.Callback(fixture.allocator);
    auto waiting = fixture.audio->Play2D(loop, Move(third));
    REQUIRE(waiting);
    REQUIRE(fixture.Frames(3));
    REQUIRE(log.Count(PlaybackEventKind::Ended) == 1);
    bool limited = false;
    for (const PlaybackEvent& event : log.events) {
        limited = limited || (event.kind == PlaybackEventKind::Ended &&
                              event.reason == PlaybackEndReason::VoiceLimit);
    }

    CHECK(limited);
    const AudioControlStats rejected = fixture.audio->ControlStats();
    CHECK(rejected.streamSlotRejects == 1);
    CHECK(rejected.voiceLimitRejects == 0 && rejected.activeVoices == 1);
    CHECK(rejected.voicelessPlaybacks == 1 && rejected.resumeFailures >= 1);
    CHECK(rejected.headroomCuts == 0 && rejected.stoppingVoices == 1);
    CHECK(fixture.audio->Status(waiting.Value()) == PlaybackStatus::Pending);
    REQUIRE(fixture.audio->Stop(playing.Value(), 0));
    for (uint32 frame = 0; frame < MaxTestFrames && log.Count(PlaybackEventKind::Started) < 2;
         ++frame) {
        REQUIRE(fixture.Frame());
    }

    CHECK(log.Count(PlaybackEventKind::Started) == 2);
    CHECK(fixture.audio->Status(waiting.Value()) == PlaybackStatus::Playing);
    CHECK(fixture.audio->ControlStats().voicelessPlaybacks == 0);
    CHECK(fixture.audio->ControlStats().streamSlotRejects == 1);
    REQUIRE(fixture.audio->Stop(waiting.Value(), 0));
    REQUIRE(fixture.audio->Stop(fading.Value(), 0));
    REQUIRE(RunUntilStreamsIdle(fixture));
}

// 상한에서 빼앗긴 스트림 loop는 스트림 슬롯을 돌려주고(activeStreams 0) voice 없이 남았다가, 자리가
// 나면 새 디코더로 다시 시작한다(Started는 한 번 — 3.41.1절).
TEST(EngineAudio_StreamLoopStolenReturnsSlot) {
    constexpr uint32 Fillers = 32;
    OpusRegistryScope opus;
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const SharedPtr<const SoundStreamData> data =
        MakeStreamData(fixture.allocator, audio::OpusPlatformSampleRate);
    EventLog log(fixture.allocator);
    PlayOptions streamOptions;
    streamOptions.onEvent = log.Callback(fixture.allocator);
    auto stream =
        fixture.audio->Play2D(StreamSound(fixture.allocator, data, true), Move(streamOptions));
    REQUIRE(stream);
    REQUIRE(fixture.Frames(3));
    REQUIRE(fixture.audio->ControlStats().activeStreams == 1);
    const SharedPtr<const SoundPcm> silent = MakeConstantPcm(fixture.allocator, 0.0f, TestRate);
    SoundPlayable fillerPlayable;
    fillerPlayable.pcm = silent;
    fillerPlayable.looping = true;
    fillerPlayable.priority = 10.0f;
    auto filler = SoundSourceRef::TryCreate(fixture.allocator, fillerPlayable);
    REQUIRE(filler);
    Array<PlaybackHandle> fillers(fixture.allocator);
    for (uint32 index = 0; index < Fillers; ++index) {
        auto played = fixture.audio->Play2D(filler.Value());
        REQUIRE(played);
        REQUIRE(fillers.TryPushBack(played.Value()));
    }

    REQUIRE(RunUntilStreamsIdleExcept(fixture, Fillers));
    CHECK(fixture.audio->ControlStats().voicelessPlaybacks == 1);
    CHECK(fixture.audio->ControlStats().activeStreams == 0);
    for (const PlaybackHandle handle : fillers) {
        REQUIRE(fixture.audio->Stop(handle, 0));
    }

    for (uint32 frame = 0;
         frame < MaxStreamFrames && fixture.audio->ControlStats().activeStreams == 0; ++frame) {
        REQUIRE(fixture.Frame());
    }

    REQUIRE(fixture.Frames(3));
    CHECK(fixture.audio->ControlStats().activeStreams == 1);
    CHECK(fixture.audio->ControlStats().voicelessPlaybacks == 0);
    CHECK(log.Count(PlaybackEventKind::Started) == 1);
    CHECK(log.Count(PlaybackEventKind::Ended) == 0);
    REQUIRE(fixture.audio->Stop(stream.Value(), 0));
    REQUIRE(RunUntilStreamsIdle(fixture));
}

// 재생 중(loop) 엔진을 내려도 종료 drain이 끝난다(서비스는 장치 Close 뒤 멈추고 활성 작업을
// 반납한다). Inline과 Thread 서비스 모두 스트림 데이터 참조를 모두 놓는다.
TEST(EngineAudio_StreamShutdownWhilePlaying) {
    SystemAllocator allocator;
    const SharedPtr<const SoundStreamData> data =
        MakeStreamData(allocator, audio::OpusPlatformSampleRate);
    for (AudioControlMode mode : {AudioControlMode::Inline, AudioControlMode::Thread}) {
        {
            OpusRegistryScope opus;
            AudioEngineHostOptions options = DeterministicOptions(true);
            options.controlMode = mode;
            AudioEngine fixture(options);
            REQUIRE(fixture.initialized);
            const SoundSourceRef sound = StreamSound(fixture.allocator, data, true);
            REQUIRE(fixture.audio->Play2D(sound));
            for (uint32 frame = 0; frame < 4; ++frame) {
                REQUIRE(StepFrame(fixture, mode));
            }

            CHECK(fixture.audio->ControlStats().activeStreams == 1);
        }

        // 엔진이 내려간 뒤 스트림 데이터 참조는 시험 것만 남는다.
        CHECK(data.UseCount() == 1);
    }
}

namespace {

// 읽지 않는 sidecar payload다(크기만 맞춘다 — 재생이 읽기 전에 거부되는지 본다).
class UnreadPayload final : public SoundStreamedPayload {
public:
    explicit UnreadPayload(uint64 bytes) noexcept : bytes_(bytes) {
    }

    uint64 Bytes() const noexcept override {
        return bytes_;
    }

    Result<SoundPayloadRead> Read(uint64, Span<uint8>, SoundReadPriority, SoundReadCallback,
                                  void*) noexcept override {
        ++reads;
        return Result<SoundPayloadRead>::Failure(ErrorCode::Unsupported);
    }

    uint32 reads = 0;

private:
    uint64 bytes_ = 0;
};

} // namespace

// sidecar 형태 스트림(청크 0만 상주 — 3.46.3절)은 청크 캐시(AU6b2) 전까지 재생하지 않는다: 시작
// 없이 Ended(Failed)이고 payload를 읽지 않으며 voice·슬롯이 남지 않는다.
TEST(EngineAudio_StreamSidecarFormRejectedUntilCache) {
    OpusRegistryScope opus;
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const Array<uint8> bytes = MakeStreamBytes(fixture.allocator, audio::OpusPlatformSampleRate);
    auto view = audio::AudioPlatformDataView::Parse(bytes.View());
    REQUIRE(view && view.Value().Header().chunkCount > 1);
    const audio::AudioChunkEntry chunk0 = view.Value().Chunk(0);
    const usize prefixBytes =
        view.Value().MetaBytes() + static_cast<usize>(chunk0.payloadOffset) + chunk0.payloadBytes;
    auto payload =
        SharedPtr<UnreadPayload>::TryMake(fixture.allocator, view.Value().Header().payloadBytes);
    REQUIRE(payload);
    auto made = SoundStreamData::TryCreateStreamed(
        fixture.allocator, bytes.View().Slice(0, prefixBytes), payload.Value());
    REQUIRE(made);
    REQUIRE(!made.Value()->HasAllPayload());
    const SoundSourceRef sound =
        StreamSound(fixture.allocator, SharedPtr<const SoundStreamData>(Move(made.Value())));
    REQUIRE(sound.IsValid());
    EventLog log(fixture.allocator);
    PlayOptions play;
    play.onEvent = log.Callback(fixture.allocator);
    REQUIRE(fixture.audio->Play2D(sound, Move(play)));
    REQUIRE(RunUntilEnded(fixture, AudioControlMode::Inline, log));
    REQUIRE(log.events.Size() == 1);
    CHECK(log.events[0].kind == PlaybackEventKind::Ended &&
          log.events[0].reason == PlaybackEndReason::Failed);
    CHECK(payload.Value()->reads == 0);
    REQUIRE(RunUntilStreamsIdle(fixture));
    CHECK(fixture.audio->StreamService()->AcquiredSlots() == 0);
}

} // namespace audio_stream_playback_tests_file
