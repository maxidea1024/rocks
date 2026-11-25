#include "AudioTestSupport.h"

#include <Audio/AudioOutputDevice.h>
#include <Audio/OfflineAudioRender.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/Clock.h>
#include <Platform/FloatingPointEnvironment.h>
#include <Platform/Thread.h>
#include <TestSupport/Test.h>

#include <string.h>

using namespace rocks;
namespace audio_output_tests_file {

using namespace rocks::audio;

// AU2a 출력 장치 시험이다(Docs/Design/AudioSystem 3.33.7절). 링·pending·장치는 수동 Null(또는 시험
// 가짜 backend)로 결정적으로 소비한다. 실제 스레드는 AudioRT 깨우기·타이머 Null 두 곳뿐이고 둘 다
// 조건이 될 때까지(넉넉한 시한 안에서) 기다린 뒤 단언한다.

namespace {

constexpr uint32 Rate = DefaultMixSampleRate;
constexpr uint32 Block = 256;
// 다른 스레드의 진행을 기다리는 상한이다. 넘으면 실패로 본다(시간 운이 아니라 멈춤 검출용).
constexpr uint64 ProgressTimeoutNs = 10ull * 1000 * 1000 * 1000;

AudioMixerConfig ConfigOf(uint32 blockFrames = Block,
                          ChannelLayout layout = ChannelLayout::Stereo) noexcept {
    AudioMixerConfig config;
    config.blockFrames = blockFrames;
    config.masterLimiterEnabled = false;
    config.layout = layout;
    config.maxVoices = 16;
    config.maxClips = 8;
    return config;
}

AudioOutputRequest RequestOf(uint32 callbackFrames, uint32 rate = Rate,
                             ChannelLayout layout = ChannelLayout::Stereo) noexcept {
    AudioOutputRequest request;
    request.sampleRate = rate;
    request.layout = layout;
    request.callbackFrames = callbackFrames;
    return request;
}

AudioOutputDeviceOptions ManualOptions(uint32 requestedBufferCount = 0) noexcept {
    AudioOutputDeviceOptions options;
    options.renderMode = AudioRenderMode::Manual;
    options.nullMode = NullAudioOutputMode::Manual;
    options.requestedBufferCount = requestedBufferCount;
    return options;
}

// 가짜 backend가 남기는 호출 기록이다(backend는 장치가 소유하므로 기록은 밖에 둔다).
struct FakeBackendLog {
    char calls[32] = {};
    uint32 count = 0;
    AudioOutputCallback* callback = nullptr;
    AudioOutputFormat format;

    void Add(char call) noexcept {
        if (count + 1 < sizeof(calls)) {
            calls[count++] = call;
        }
    }

    bool CallsAre(const char* expected) const noexcept {
        return strcmp(calls, expected) == 0;
    }
};

struct FakeBackendConfig {
    ErrorCode openError = ErrorCode::None;
    ErrorCode startError = ErrorCode::None;
    int32 nativeError = 0;
    // 0이면 요청값을 그대로 협상한다.
    uint32 sampleRate = 0;
    bool overrideLayout = false;
    ChannelLayout layout = ChannelLayout::Stereo;
};

// 호출 순서를 'O'pen·'S'tart·s'T'op·'C'lose로 기록하는 시험 backend다. 콜백은 시험이
// log.callback으로 직접 부른다.
class FakeBackend final : public AudioOutputBackend {
public:
    FakeBackend(FakeBackendLog& log, const FakeBackendConfig& config) noexcept
        : log_(log), config_(config) {
    }

    Result<AudioOutputFormat> Open(const AudioOutputRequest& request,
                                   AudioOutputCallback& callback) noexcept override {
        log_.Add('O');
        if (config_.openError != ErrorCode::None) {
            native_ = config_.nativeError;
            return Result<AudioOutputFormat>::Failure(config_.openError);
        }

        AudioOutputFormat format;
        format.sampleRate = config_.sampleRate != 0 ? config_.sampleRate : request.sampleRate;
        format.layout = config_.overrideLayout ? config_.layout : request.layout;
        format.callbackFrames = request.callbackFrames;
        format.sampleFormat = AudioSampleFormat::Float32;
        log_.callback = &callback;
        log_.format = format;
        return Result<AudioOutputFormat>::Success(format);
    }

    Result<void> Start() noexcept override {
        log_.Add('S');
        if (config_.startError != ErrorCode::None) {
            native_ = config_.nativeError;
            return Result<void>::Failure(config_.startError);
        }

        return Result<void>::Success();
    }

    Result<void> Stop() noexcept override {
        log_.Add('T');
        return Result<void>::Success();
    }

    void Close() noexcept override {
        log_.Add('C');
        log_.callback = nullptr;
    }

    Result<uint32> EnumerateDevices(Span<AudioDeviceInfo> out) noexcept override {
        (void)out;
        return Result<uint32>::Success(0);
    }

    const char* Name() const noexcept override {
        return "Fake";
    }

    int32 LastNativeError() const noexcept override {
        return native_;
    }

private:
    FakeBackendLog& log_;
    FakeBackendConfig config_;
    int32 native_ = 0;
};

UniquePtr<AudioOutputBackend> MakeFake(Allocator& allocator, FakeBackendLog& log,
                                       const FakeBackendConfig& config = {}) noexcept {
    auto made = UniquePtr<FakeBackend>::TryMake(allocator, log, config);
    if (!made) {
        platform::Fatal("Audio output test backend was not allocated");
    }

    return UniquePtr<AudioOutputBackend>(Move(made.Value()));
}

UniquePtr<AudioOutputDevice> OpenOrDie(const AudioMixerConfig& config,
                                       UniquePtr<AudioOutputBackend> backend,
                                       const AudioOutputRequest& request,
                                       const AudioOutputDeviceOptions& options,
                                       Allocator& allocator) noexcept {
    auto opened = AudioOutputDevice::TryOpen(config, Move(backend), request, options, allocator);
    if (!opened) {
        platform::Fatal("Audio output test device did not open");
    }

    return Move(opened.Value());
}

AudioClipId RegisterMono(AudioMixer& mixer, const float* samples, uint32 frames) noexcept {
    AudioClipDesc desc;
    desc.channelCount = 1;
    desc.channels[0] = samples;
    desc.frameCount = frames;
    desc.sampleRate = Rate;
    auto registered = mixer.RegisterClip(desc);
    if (!registered) {
        platform::Fatal("Audio output test clip was rejected");
    }

    return registered.Value();
}

// voice를 잡고 frame에 시작하는 Play 명령을 만든다(제출은 호출자가 한다).
AudioRtCommand PlayCommand(AudioMixer& mixer, AudioClipId clip, uint64 frame,
                           AudioVoiceHandle* voiceOut = nullptr,
                           const AudioVoiceParams& params = AudioVoiceParams{}) noexcept {
    auto voice = mixer.AcquireVoice();
    if (!voice) {
        platform::Fatal("Audio output test voice was not acquired");
    }

    auto play = mixer.PreparePlay(voice.Value(), clip, 0, false, params);
    if (!play) {
        platform::Fatal("Audio output test play was rejected");
    }

    play.Value().mixFrame = frame;
    if (voiceOut != nullptr) {
        *voiceOut = voice.Value();
    }

    return play.Value();
}

void SubmitOrDie(AudioOutputDevice& device, const AudioRtCommand& command) noexcept {
    if (!device.Submit(command)) {
        platform::Fatal("Audio output test command was rejected");
    }
}

void SubmitOrDie(OfflineAudioRender& rig, const AudioRtCommand& command) noexcept {
    if (!rig.Submit(command)) {
        platform::Fatal("Audio output test offline command was rejected");
    }
}

OfflineAudioRender MakeRig(const AudioMixerConfig& config, Allocator& allocator) noexcept {
    auto created = OfflineAudioRender::TryCreate(config, allocator);
    if (!created) {
        platform::Fatal("Audio output test offline configuration was rejected");
    }

    return Move(created.Value());
}

Array<float> RenderOffline(OfflineAudioRender& rig, uint32 blocks, Allocator& allocator) noexcept {
    Array<float> output(allocator);
    const usize floats = static_cast<usize>(blocks) * rig.Mixer().Config().blockFrames *
                         rig.Mixer().OutputChannelCount();
    if (!output.TryResize(floats) ||
        !rig.Render(Span<float>(output.Data(), output.Size()), blocks)) {
        platform::Fatal("Audio output test offline render failed");
    }

    return output;
}

// 수동 Null 장치를 callbackFrames씩 Pump하고 다시 채우기를 반복해 frames frame을 캡처한다.
Array<float> CaptureManual(AudioOutputDevice& device, uint32 frames, uint32 callbackFrames,
                           Allocator& allocator) noexcept {
    NullAudioOutput* output = device.NullOutput();
    if (output == nullptr) {
        platform::Fatal("Audio output test device is not using Null output");
    }

    const uint32 channels = device.Mixer().OutputChannelCount();
    Array<float> captured(allocator);
    if (!captured.TryResize(static_cast<usize>(frames) * channels)) {
        platform::Fatal("Audio output test capture allocation failed");
    }

    output->CaptureTo(Span<float>(captured.Data(), captured.Size()));
    for (uint32 delivered = 0; delivered < frames; delivered += callbackFrames) {
        output->Pump(callbackFrames);
        device.RenderAvailable();
    }

    return captured;
}

bool SamplesEqual(const Array<float>& left, const Array<float>& right) noexcept {
    return left.Size() == right.Size() &&
           memcmp(left.Data(), right.Data(), left.Size() * sizeof(float)) == 0;
}

// AudioRenderThread::RenderAvailable을 실시간 FP 환경 안에서 한 번 돈다(수동 방식).
void RenderManual(AudioRenderThread& render) noexcept {
    platform::RunWithRealtimeAudioFloatingPoint(
        [](void* context) noexcept { static_cast<AudioRenderThread*>(context)->RenderAvailable(); },
        &render);
}

// 생산자 쪽에서 빈 칸마다 다음 값(이어지는 정수)으로 블록을 채운다.
void FillCounting(AudioOutputRing& ring, float& next) noexcept {
    while (ring.HasFreeSlot()) {
        Span<float> slot = ring.BeginWrite();
        for (usize index = 0; index < slot.Size(); ++index) {
            slot[index] = next;
            next += 1.0f;
        }

        ring.CommitWrite();
    }
}

AudioOutputView FloatView(float* data, uint32 frames, uint32 channels) noexcept {
    AudioOutputView view;
    view.data = data;
    view.frames = frames;
    view.channels = channels;
    view.format = AudioSampleFormat::Float32;
    return view;
}

// 다른 스레드가 조건을 만들 때까지 양보하며 기다린다. 시한을 넘으면 false다.
template <typename Predicate>
bool WaitForProgress(Predicate&& predicate) noexcept {
    const uint64 deadline = platform::MonotonicNanoseconds() + ProgressTimeoutNs;
    while (!predicate()) {
        if (platform::MonotonicNanoseconds() > deadline) {
            return false;
        }

        platform::YieldThread();
    }

    return true;
}

} // namespace

TEST(Audio_Output_BufferCountTable) {
    // max(2, ceil(cb / Q) + 1, requested), 상한 16.
    CHECK(ChooseOutputBufferCount(480, 256, 0) == 3);
    CHECK(ChooseOutputBufferCount(256, 256, 0) == 2);
    CHECK(ChooseOutputBufferCount(257, 256, 0) == 3);
    CHECK(ChooseOutputBufferCount(1024, 256, 0) == 5);
    CHECK(ChooseOutputBufferCount(100, 256, 0) == 2);
    CHECK(ChooseOutputBufferCount(0, 256, 0) == 2);
    CHECK(ChooseOutputBufferCount(480, 256, 8) == 8);
    CHECK(ChooseOutputBufferCount(480, 256, 1) == 3);
    CHECK(ChooseOutputBufferCount(480, 256, 100) == MaxOutputBufferCount);
    CHECK(ChooseOutputBufferCount(100000, 256, 0) == MaxOutputBufferCount);
    CHECK(ChooseOutputBufferCount(0xFFFFFFFFu, 1, 0) == MaxOutputBufferCount);
    CHECK(ChooseOutputBufferCount(480, 0, 0) == MinOutputBufferCount);
}

TEST(Audio_Output_RingRejectsInvalidArguments) {
    SystemAllocator allocator;
    AudioOutputRing ring;
    CHECK(ring.Initialize(allocator, 1, 8, 2, nullptr).Error() == ErrorCode::InvalidArgument);
    CHECK(ring.Initialize(allocator, MaxOutputBufferCount + 1, 8, 2, nullptr).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(ring.Initialize(allocator, 2, 0, 2, nullptr).Error() == ErrorCode::InvalidArgument);
    CHECK(ring.Initialize(allocator, 2, 8, 0, nullptr).Error() == ErrorCode::InvalidArgument);
    CHECK(ring.Initialize(allocator, 2, 8, 2, nullptr));
    CHECK(ring.Initialize(allocator, 2, 8, 2, nullptr).Error() == ErrorCode::InvalidArgument);
    CHECK(ring.BufferCount() == 2 && ring.BlockFrames() == 8 && ring.Channels() == 2);

    // 채널 수가 다른 보기는 전부 무음·underrun이고 블록을 소비하지 않는다.
    float next = 1.0f;
    FillCounting(ring, next);
    float mono[8];
    for (float& sample : mono) {
        sample = 7.0f;
    }

    ring.Read(FloatView(mono, 8, 1));
    for (float sample : mono) {
        CHECK(sample == 0.0f);
    }

    const AudioOutputRingCounters counters = ring.CountersSnapshot();
    CHECK(counters.underrunFrames == 8 && counters.underrunCallbacks == 1);
    CHECK(ring.ReadBlocks() == 0);
    ring.Shutdown();
    ring.Shutdown();
}

TEST(Audio_Output_RingSplitReadsFollowBlockOrder) {
    // Q = 8, 2채널, 4칸. 블록 경계와 다른 크기로 읽어도 이어진 값 순서가 그대로다.
    SystemAllocator allocator;
    AudioOutputRing ring;
    CHECK(ring.Initialize(allocator, 4, 8, 2, nullptr));
    float next = 0.0f;
    float expected = 0.0f;
    const uint32 sizes[] = {3, 5, 13, 1, 8, 7, 16, 11};
    float buffer[16 * 2];
    uint64 consumed = 0;
    for (uint32 round = 0; round < 40; ++round) {
        FillCounting(ring, next);
        const uint32 frames = sizes[round % (sizeof(sizes) / sizeof(sizes[0]))];
        ring.Read(FloatView(buffer, frames, 2));
        consumed += frames;
        for (uint32 index = 0; index < frames * 2; ++index) {
            CHECK(buffer[index] == expected);
            expected += 1.0f;
        }
    }

    const AudioOutputRingCounters counters = ring.CountersSnapshot();
    CHECK(counters.underrunFrames == 0 && counters.underrunCallbacks == 0);
    CHECK(counters.consumedFrames == consumed);
    CHECK(ring.ReadBlocks() == consumed / 8);
}

TEST(Audio_Output_RingUnderrunWritesSilence) {
    SystemAllocator allocator;
    AudioOutputRing ring;
    CHECK(ring.Initialize(allocator, 2, 4, 1, nullptr));
    Span<float> slot = ring.BeginWrite();
    for (usize index = 0; index < slot.Size(); ++index) {
        slot[index] = static_cast<float>(index + 1);
    }

    ring.CommitWrite();
    float buffer[6] = {9.0f, 9.0f, 9.0f, 9.0f, 9.0f, 9.0f};
    ring.Read(FloatView(buffer, 6, 1));
    CHECK(buffer[0] == 1.0f && buffer[1] == 2.0f && buffer[2] == 3.0f && buffer[3] == 4.0f);
    CHECK(buffer[4] == 0.0f && buffer[5] == 0.0f);
    AudioOutputRingCounters counters = ring.CountersSnapshot();
    CHECK(counters.underrunFrames == 2 && counters.underrunCallbacks == 1);
    CHECK(counters.consumedFrames == 6);

    // 빈 링: 전부 무음. 0 frame 읽기는 아무것도 세지 않는다.
    ring.Read(FloatView(buffer, 3, 1));
    ring.Read(FloatView(buffer, 0, 1));
    counters = ring.CountersSnapshot();
    CHECK(buffer[0] == 0.0f && buffer[1] == 0.0f && buffer[2] == 0.0f);
    CHECK(counters.underrunFrames == 5 && counters.underrunCallbacks == 2);
    CHECK(counters.consumedFrames == 9);
}

TEST(Audio_Output_RingInt16Conversion) {
    SystemAllocator allocator;
    AudioOutputRing ring;
    CHECK(ring.Initialize(allocator, 2, 8, 1, nullptr));
    const float input[8] = {0.0f, 1.0f, -1.0f, 1.5f, -2.0f, 0.5f, -0.5f, 1.0f / 32767.0f * 0.49f};
    Span<float> slot = ring.BeginWrite();
    for (usize index = 0; index < slot.Size(); ++index) {
        slot[index] = input[index];
    }

    ring.CommitWrite();
    int16 output[8] = {};
    AudioOutputView view;
    view.data = output;
    view.frames = 8;
    view.channels = 1;
    view.format = AudioSampleFormat::Int16;
    ring.Read(view);
    // [-1, 1]로 자르고 ×32767 뒤 0에서 먼 쪽으로 반올림한다(16383.5 → 16384).
    CHECK(output[0] == 0);
    CHECK(output[1] == 32767);
    CHECK(output[2] == -32767);
    CHECK(output[3] == 32767);
    CHECK(output[4] == -32767);
    CHECK(output[5] == 16384);
    CHECK(output[6] == -16384);
    CHECK(output[7] == 0);

    // Int16 underrun도 무음이다.
    int16 empty[4] = {5, 5, 5, 5};
    view.data = empty;
    view.frames = 4;
    ring.Read(view);
    CHECK(empty[0] == 0 && empty[1] == 0 && empty[2] == 0 && empty[3] == 0);
}

TEST(Audio_Output_RingSignalsOnlyWhenBlockFreed) {
    SystemAllocator allocator;
    platform::RealtimeWakeSignal wake;
    CHECK(wake.Initialize());
    AudioOutputRing ring;
    CHECK(ring.Initialize(allocator, 3, 8, 1, &wake));
    float next = 0.0f;
    FillCounting(ring, next);
    float buffer[24];

    // 블록 안에서만 읽음 → 신호 없음.
    ring.Read(FloatView(buffer, 5, 1));
    CHECK(wake.Wait(0) == platform::WaitResult::TimedOut);
    // 블록 하나를 끝냄 → 신호 한 번.
    ring.Read(FloatView(buffer, 3, 1));
    CHECK(wake.Wait(0) == platform::WaitResult::Signaled);
    CHECK(wake.Wait(0) == platform::WaitResult::TimedOut);
    // 블록 두 개를 끝내도 신호는 한 번(소비 쪽은 한 번 깨우면 빈 칸을 모두 채운다).
    ring.Read(FloatView(buffer, 16, 1));
    CHECK(wake.Wait(0) == platform::WaitResult::Signaled);
    CHECK(wake.Wait(0) == platform::WaitResult::TimedOut);
    // 빈 링 underrun → 신호 없음.
    ring.Read(FloatView(buffer, 4, 1));
    CHECK(wake.Wait(0) == platform::WaitResult::TimedOut);
    CHECK(ring.HasFreeSlot());
}

TEST(Audio_Output_DeviceMatchesOfflineRender) {
    // 순서를 뒤섞어 제출한 미래 명령이 정렬되어 Inline 렌더와 bit 단위로 같은 PCM이 나온다. 콜백
    // 480 frame, Q 256(칸 3)으로 블록 경계와 콜백 경계가 어긋난다.
    SystemAllocator allocator;
    float ones[300];
    float ramp[300];
    for (uint32 frame = 0; frame < 300; ++frame) {
        ones[frame] = 1.0f;
        ramp[frame] = static_cast<float>(frame) / 300.0f;
    }

    auto device = OpenOrDie(ConfigOf(), nullptr, RequestOf(480), ManualOptions(), allocator);
    CHECK(device->Info().usingNull && device->Info().bufferCount == 3);
    OfflineAudioRender rig = MakeRig(ConfigOf(), allocator);

    // 채워 둔 3블록(0..767) 뒤 frame에 예약한다. 두 믹서에 같은 순서로 만들어 순번이 같다.
    const AudioClipId deviceOnes = RegisterMono(device->Mixer(), ones, 300);
    const AudioClipId deviceRamp = RegisterMono(device->Mixer(), ramp, 300);
    const AudioClipId rigOnes = RegisterMono(rig.Mixer(), ones, 300);
    const AudioClipId rigRamp = RegisterMono(rig.Mixer(), ramp, 300);
    const AudioRtCommand deviceFirst = PlayCommand(device->Mixer(), deviceOnes, 1000);
    const AudioRtCommand deviceSecond = PlayCommand(device->Mixer(), deviceRamp, 1700);
    const AudioRtCommand deviceThird = PlayCommand(device->Mixer(), deviceOnes, 800);
    SubmitOrDie(rig, PlayCommand(rig.Mixer(), rigOnes, 1000));
    SubmitOrDie(rig, PlayCommand(rig.Mixer(), rigRamp, 1700));
    SubmitOrDie(rig, PlayCommand(rig.Mixer(), rigOnes, 800));
    SubmitOrDie(*device, deviceSecond);
    SubmitOrDie(*device, deviceThird);
    SubmitOrDie(*device, deviceFirst);

    constexpr uint32 Blocks = 12;
    Array<float> expected = RenderOffline(rig, Blocks, allocator);
    Array<float> captured = CaptureManual(*device, Blocks * Block, 480, allocator);
    CHECK(SamplesEqual(captured, expected));
    CHECK(expected[(800 + 10) * 2] == 0.5f);
    CHECK(device->RingCountersSnapshot().underrunFrames == 0);
    CHECK(device->RenderCountersSnapshot().pendingFullStalls == 0);
}

TEST(Audio_Output_PendingSameFrameOrdersBySequence) {
    // 같은 mixFrame의 Play(순번 앞)와 SetParams(순번 뒤)를 거꾸로 제출해도 Play가 먼저 실행된다.
    SystemAllocator allocator;
    float ones[256];
    for (float& sample : ones) {
        sample = 1.0f;
    }

    auto device = OpenOrDie(ConfigOf(), nullptr, RequestOf(Block), ManualOptions(), allocator);
    OfflineAudioRender rig = MakeRig(ConfigOf(), allocator);
    AudioVoiceParams half;
    half.gain = 0.5f;

    const AudioClipId deviceClip = RegisterMono(device->Mixer(), ones, 256);
    AudioVoiceHandle deviceVoice;
    const AudioRtCommand devicePlay = PlayCommand(device->Mixer(), deviceClip, 600, &deviceVoice);
    auto deviceParams = device->Mixer().MakeSetParams(deviceVoice, 600, half, 0);
    CHECK(deviceParams);

    const AudioClipId rigClip = RegisterMono(rig.Mixer(), ones, 256);
    AudioVoiceHandle rigVoice;
    SubmitOrDie(rig, PlayCommand(rig.Mixer(), rigClip, 600, &rigVoice));
    auto rigParams = rig.Mixer().MakeSetParams(rigVoice, 600, half, 0);
    CHECK(rigParams);
    SubmitOrDie(rig, rigParams.Value());

    SubmitOrDie(*device, deviceParams.Value());
    SubmitOrDie(*device, devicePlay);
    constexpr uint32 Blocks = 6;
    Array<float> expected = RenderOffline(rig, Blocks, allocator);
    Array<float> captured = CaptureManual(*device, Blocks * Block, Block, allocator);
    CHECK(SamplesEqual(captured, expected));
    CHECK(captured[(600 + 1) * 2] == 0.25f);
    CHECK(device->Mixer()
              .Counters()
              .ignoredCommands[static_cast<uint32>(AudioRtCommandType::SetParams)] == 0);
}

TEST(Audio_Output_ImmediateStartsAtDrainBlock) {
    // 콜백 = Q = 256이면 칸 2, 연 직후 0..511이 채워져 있다. Immediate Play는 링에서 꺼낼 때의 블록
    // 시작 (512)에서 시작한다.
    SystemAllocator allocator;
    float ones[256];
    for (float& sample : ones) {
        sample = 1.0f;
    }

    auto device = OpenOrDie(ConfigOf(), nullptr, RequestOf(Block), ManualOptions(), allocator);
    CHECK(device->Info().bufferCount == 2);
    const AudioClipId clip = RegisterMono(device->Mixer(), ones, 256);
    SubmitOrDie(*device, PlayCommand(device->Mixer(), clip, ImmediateMixFrame));
    Array<float> captured = CaptureManual(*device, 4 * Block, Block, allocator);
    for (uint32 frame = 0; frame < 4 * Block; ++frame) {
        const float value = frame >= 512 && frame < 768 ? 0.5f : 0.0f;
        CHECK(captured[frame * 2] == value && captured[frame * 2 + 1] == value);
    }
}

TEST(Audio_Output_PendingFullLeavesCommandsQueued) {
    // pending 배열(1024)을 미래 명령으로 채우면 그다음 명령은 명령 링에 남고 블록마다 계수가
    // 오른다. 시각이 되면 pending이 비고 남은 명령이 이어 소비된다.
    SystemAllocator allocator;
    auto mixer = AudioMixer::TryCreate(ConfigOf(), allocator);
    CHECK(mixer);
    AudioOutputRing ring;
    AudioRenderThread render;
    CHECK(ring.Initialize(allocator, 2, Block, 2, &render.WakeSignal()));
    CHECK(render.Initialize(allocator, mixer.Value(), ring));
    constexpr uint64 Due = 4 * Block;
    for (uint32 index = 0; index < AudioRtPendingCapacity; ++index) {
        auto command = mixer.Value().MakeSetMasterGain(Due, 0.5f, 0);
        CHECK(command && render.Submit(command.Value()));
    }

    RenderManual(render);
    CHECK(render.PendingCount() == AudioRtPendingCapacity);
    CHECK(render.CountersSnapshot().pendingFullStalls == 0);
    auto extra = mixer.Value().MakeSetMasterGain(Due, 0.25f, 0);
    CHECK(extra && render.Submit(extra.Value()));

    float scratch[2 * Block * 2];
    ring.Read(FloatView(scratch, 2 * Block, 2));
    RenderManual(render);
    // 블록 2·3: pending이 차 있어 매 블록 한 번씩 멈춘다.
    CHECK(render.PendingCount() == AudioRtPendingCapacity);
    CHECK(render.CountersSnapshot().pendingFullStalls == 2);

    for (uint32 round = 0; round < 16; ++round) {
        ring.Read(FloatView(scratch, 2 * Block, 2));
        RenderManual(render);
    }

    CHECK(render.PendingCount() == 0);
    CHECK(render.CountersSnapshot().renderedBlocks == 2 + 2 + 32);
    render.Shutdown();
    ring.Shutdown();
}

TEST(Audio_Output_SubmitBusyWhenQueueFull) {
    SystemAllocator allocator;
    auto mixer = AudioMixer::TryCreate(ConfigOf(), allocator);
    CHECK(mixer);
    AudioOutputRing ring;
    AudioRenderThread render;
    auto early = mixer.Value().MakeSetMasterGain(0, 1.0f, 0);
    CHECK(early && render.Submit(early.Value()).Error() == ErrorCode::InvalidArgument);
    CHECK(ring.Initialize(allocator, 2, Block, 2, &render.WakeSignal()));
    CHECK(render.Initialize(allocator, mixer.Value(), ring));
    CHECK(render.Initialize(allocator, mixer.Value(), ring).Error() == ErrorCode::InvalidArgument);
    for (uint32 index = 0; index < AudioRtCommandCapacity; ++index) {
        auto command = mixer.Value().MakeSetMasterGain(ImmediateMixFrame, 1.0f, 0);
        CHECK(command && render.Submit(command.Value()));
    }

    auto overflow = mixer.Value().MakeSetMasterGain(ImmediateMixFrame, 1.0f, 0);
    CHECK(overflow && render.Submit(overflow.Value()).Error() == ErrorCode::Busy);
    render.Shutdown();
    ring.Shutdown();
}

TEST(Audio_Output_NoBackendUsesNull) {
    SystemAllocator allocator;
    auto device = OpenOrDie(ConfigOf(), nullptr, RequestOf(0, 0), ManualOptions(), allocator);
    const AudioOutputDeviceInfo& info = device->Info();
    CHECK(info.usingNull && strcmp(info.backendName, "Null") == 0);
    CHECK(info.backendError == ErrorCode::None && info.backendNativeError == 0);
    // 요청 rate 0은 기본 rate, callbackFrames 0은 믹서 블록 크기다.
    CHECK(info.format.sampleRate == DefaultMixSampleRate);
    CHECK(info.format.callbackFrames == Block && info.bufferCount == 2);
    CHECK(device->NullOutput() != nullptr);
    CHECK(device->Ring().WrittenBlocks() == 2);
    device->Close();
    device->Close();
}

TEST(Audio_Output_OpenFailureFallsBackToNull) {
    SystemAllocator allocator;
    FakeBackendLog log;
    FakeBackendConfig config;
    config.openError = ErrorCode::NotFound;
    config.nativeError = static_cast<int32>(0x88890004u);
    auto device = OpenOrDie(ConfigOf(), MakeFake(allocator, log, config), RequestOf(480),
                            ManualOptions(), allocator);
    const AudioOutputDeviceInfo& info = device->Info();
    CHECK(info.usingNull && strcmp(info.backendName, "Null") == 0);
    CHECK(info.backendError == ErrorCode::NotFound);
    CHECK(info.backendNativeError == static_cast<int32>(0x88890004u));
    CHECK(log.CallsAre("OC"));
    // Null로도 소리가 이어진다(시간 진행).
    Array<float> captured = CaptureManual(*device, 960, 480, allocator);
    CHECK(device->RingCountersSnapshot().underrunFrames == 0);
}

TEST(Audio_Output_StartFailureFallsBackToNull) {
    SystemAllocator allocator;
    FakeBackendLog log;
    FakeBackendConfig config;
    config.startError = ErrorCode::Busy;
    config.nativeError = 17;
    auto device = OpenOrDie(ConfigOf(), MakeFake(allocator, log, config), RequestOf(480),
                            ManualOptions(), allocator);
    CHECK(device->Info().usingNull);
    CHECK(device->Info().backendError == ErrorCode::Busy &&
          device->Info().backendNativeError == 17);
    CHECK(log.CallsAre("OSC"));
    CHECK(device->Ring().WrittenBlocks() == 3);
}

TEST(Audio_Output_InvalidArgumentFromBackendBecomesUnsupported) {
    // backend의 InvalidArgument는 장치 설정 오류가 아니므로 Unsupported로 바꿔 Null로 간다.
    SystemAllocator allocator;
    FakeBackendLog log;
    FakeBackendConfig config;
    config.openError = ErrorCode::InvalidArgument;
    auto device = OpenOrDie(ConfigOf(), MakeFake(allocator, log, config), RequestOf(480),
                            ManualOptions(), allocator);
    CHECK(device->Info().usingNull && device->Info().backendError == ErrorCode::Unsupported);
}

TEST(Audio_Output_DeviceRateDrivesMixer) {
    SystemAllocator allocator;
    FakeBackendLog log;
    FakeBackendConfig config;
    config.sampleRate = 44100;
    auto device = OpenOrDie(ConfigOf(), MakeFake(allocator, log, config), RequestOf(441),
                            ManualOptions(), allocator);
    const AudioOutputDeviceInfo& info = device->Info();
    CHECK(!info.usingNull && strcmp(info.backendName, "Fake") == 0);
    CHECK(info.format.sampleRate == 44100 && device->Mixer().Config().sampleRate == 44100);
    CHECK(info.bufferCount == 3 && device->NullOutput() == nullptr);
    CHECK(log.CallsAre("OS"));

    // 가짜 콜백을 직접 불러 링을 소비한다(441 frame).
    float buffer[441 * 2];
    log.callback->Fill(FloatView(buffer, 441, 2));
    CHECK(device->RingCountersSnapshot().consumedFrames == 441);
    CHECK(device->RingCountersSnapshot().underrunFrames == 0);
    device->Close();
    CHECK(log.CallsAre("OSTC"));
}

TEST(Audio_Output_UnsupportedLayoutFallsBackToNull) {
    SystemAllocator allocator;
    FakeBackendLog log;
    FakeBackendConfig config;
    config.overrideLayout = true;
    config.layout = ChannelLayout::Surround51;
    auto device = OpenOrDie(ConfigOf(), MakeFake(allocator, log, config), RequestOf(480),
                            ManualOptions(), allocator);
    CHECK(device->Info().usingNull && device->Info().backendError == ErrorCode::Unsupported);
    CHECK(device->Info().format.layout == ChannelLayout::Stereo);
    CHECK(log.CallsAre("OC"));
}

TEST(Audio_Output_UnsupportedRateFallsBackToNull) {
    SystemAllocator allocator;
    FakeBackendLog log;
    FakeBackendConfig config;
    config.sampleRate = 1000;
    auto device = OpenOrDie(ConfigOf(), MakeFake(allocator, log, config), RequestOf(480),
                            ManualOptions(), allocator);
    CHECK(device->Info().usingNull && device->Info().backendError == ErrorCode::Unsupported);
    CHECK(log.CallsAre("OC"));
}

TEST(Audio_Output_InvalidMixerConfigFails) {
    SystemAllocator allocator;
    FakeBackendLog log;
    AudioMixerConfig config = ConfigOf();
    config.blockFrames = 0;
    auto opened = AudioOutputDevice::TryOpen(config, MakeFake(allocator, log), RequestOf(480),
                                             ManualOptions(), allocator);
    CHECK(!opened && opened.Error() == ErrorCode::InvalidArgument);
    auto withoutBackend =
        AudioOutputDevice::TryOpen(config, nullptr, RequestOf(480), ManualOptions(), allocator);
    CHECK(!withoutBackend && withoutBackend.Error() == ErrorCode::InvalidArgument);
}

TEST(Audio_Output_MismatchedCallbackHasNoUnderrun) {
    // Q = 256, 콜백 480·441·1000 frame: 칸 수 계산 덕에 수동 Pump 동안 underrun이 없다.
    SystemAllocator allocator;
    const uint32 callbacks[] = {480, 441, 1000, 128};
    for (uint32 callbackFrames : callbacks) {
        auto device =
            OpenOrDie(ConfigOf(), nullptr, RequestOf(callbackFrames), ManualOptions(), allocator);
        CHECK(device->Info().bufferCount == ChooseOutputBufferCount(callbackFrames, Block, 0));
        Array<float> captured =
            CaptureManual(*device, 64 * callbackFrames, callbackFrames, allocator);
        const AudioOutputRingCounters counters = device->RingCountersSnapshot();
        CHECK(counters.underrunFrames == 0 && counters.underrunCallbacks == 0);
        CHECK(counters.consumedFrames == uint64{64} * callbackFrames);
    }

    // 요청 칸 수가 계산값보다 크면 요청값이다.
    auto requested = OpenOrDie(ConfigOf(), nullptr, RequestOf(480), ManualOptions(6), allocator);
    CHECK(requested->Info().bufferCount == 6 && requested->Ring().WrittenBlocks() == 6);
}

TEST(Audio_Output_RenderThreadRefillsAfterCallback) {
    // 스레드 방식: Start는 링을 다 채운 뒤 돌아오고, 콜백이 칸을 비우면 AudioRT가 깨어 다시 채운다.
    SystemAllocator allocator;
    FakeBackendLog log;
    auto device = OpenOrDie(ConfigOf(), MakeFake(allocator, log), RequestOf(Block),
                            AudioOutputDeviceOptions{}, allocator);
    const AudioOutputRing& ring = device->Ring();
    CHECK(!device->Info().usingNull && device->Info().bufferCount == 2);
    CHECK(ring.WrittenBlocks() == 2);
    CHECK(device->Info().priorityKnown);

    float buffer[Block * 2];
    for (uint64 round = 1; round <= 8; ++round) {
        log.callback->Fill(FloatView(buffer, Block, 2));
        CHECK(WaitForProgress(
            [&ring, round]() noexcept { return ring.WrittenBlocks() == 2 + round; }));
    }

    CHECK(device->RingCountersSnapshot().underrunFrames == 0);
    device->Close();
    CHECK(log.CallsAre("OSTC"));
    CHECK(device->RenderCountersSnapshot().renderedBlocks == 10);
}

TEST(Audio_Output_TimerNullDeliversThenStops) {
    // 타이머 Null: 적어도 한 번 콜백이 불리고, Close 뒤에는 더 불리지 않는다.
    SystemAllocator allocator;
    auto device =
        OpenOrDie(ConfigOf(), nullptr, RequestOf(Block), AudioOutputDeviceOptions{}, allocator);
    NullAudioOutput* output = device->NullOutput();
    CHECK(output != nullptr);
    CHECK(WaitForProgress([output]() noexcept { return output->CallbackCount() >= 2; }));
    device->Close();
    const uint64 stopped = output->CallbackCount();
    for (uint32 index = 0; index < 64; ++index) {
        platform::YieldThread();
    }

    CHECK(output->CallbackCount() == stopped);
}

} // namespace audio_output_tests_file
