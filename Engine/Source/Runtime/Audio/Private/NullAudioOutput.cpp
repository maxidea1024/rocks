#include <Audio/AudioMixerTypes.h>
#include <Audio/NullAudioOutput.h>

#include <Platform/Clock.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>

namespace rocks::audio {

namespace {

using platform::MemoryOrder;

constexpr uint64 NanosecondsPerSecond = 1000000000;
constexpr uint64 NanosecondsPerMillisecond = 1000000;
constexpr char NullOutputName[] = "Null";

} // namespace

NullAudioOutput::NullAudioOutput(Allocator& allocator, NullAudioOutputMode mode) noexcept
    : allocator_(allocator), mode_(mode) {
}

NullAudioOutput::~NullAudioOutput() noexcept {
    Close();
}

Result<AudioOutputFormat> NullAudioOutput::Open(const AudioOutputRequest& request,
                                                AudioOutputCallback& callback) noexcept {
    if (buffer_ != nullptr) {
        return Result<AudioOutputFormat>::Failure(ErrorCode::InvalidArgument);
    }

    AudioOutputFormat format;
    format.sampleRate = request.sampleRate != 0 ? request.sampleRate : DefaultMixSampleRate;
    format.layout = request.layout;
    format.callbackFrames =
        request.callbackFrames != 0 ? request.callbackFrames : DefaultMixBlockFrames;
    format.sampleFormat = AudioSampleFormat::Float32;
    const usize samples = static_cast<usize>(format.callbackFrames) * ChannelCount(format.layout);
    auto memory = allocator_.Allocate(samples * sizeof(float), alignof(float));
    if (!memory) {
        return Result<AudioOutputFormat>::Failure(memory.Error());
    }

    buffer_ = static_cast<float*>(memory.Value());
    callback_ = &callback;
    format_ = format;
    return Result<AudioOutputFormat>::Success(format);
}

Result<void> NullAudioOutput::Start() noexcept {
    if (buffer_ == nullptr || running_.Load<MemoryOrder::Acquire>() != 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    stopRequested_.Store<MemoryOrder::Relaxed>(0);
    if (mode_ == NullAudioOutputMode::Timer) {
        platform::ThreadOptions options;
        options.name = "AudioNullOutput";
        if (timer_.Start(&NullAudioOutput::TimerMain, this, options) !=
            platform::ThreadStatus::Success) {
            return Result<void>::Failure(ErrorCode::Unsupported);
        }
    }

    running_.Store<MemoryOrder::Release>(1);
    return Result<void>::Success();
}

// 돌아오면 콜백이 진행 중이지 않고 다시 불리지 않는다(타이머 스레드를 Join한다).
Result<void> NullAudioOutput::Stop() noexcept {
    if (running_.Load<MemoryOrder::Acquire>() == 0) {
        return Result<void>::Success();
    }

    stopRequested_.Store<MemoryOrder::Release>(1);
    if (mode_ == NullAudioOutputMode::Timer && timer_.Join() != platform::ThreadStatus::Success) {
        platform::Fatal("AudioNullOutput timer join failed");
    }

    running_.Store<MemoryOrder::Release>(0);
    return Result<void>::Success();
}

void NullAudioOutput::Close() noexcept {
    (void)Stop();
    if (buffer_ != nullptr) {
        if (!allocator_.Deallocate(buffer_)) {
            platform::Fatal("NullAudioOutput buffer deallocation was refused");
        }

        buffer_ = nullptr;
    }

    callback_ = nullptr;
}

Result<uint32> NullAudioOutput::EnumerateDevices(Span<AudioDeviceInfo> out) noexcept {
    (void)out;
    return Result<uint32>::Success(0);
}

const char* NullAudioOutput::Name() const noexcept {
    return NullOutputName;
}

int32 NullAudioOutput::LastNativeError() const noexcept {
    return 0;
}

void NullAudioOutput::Pump(uint32 frames) noexcept {
    if (mode_ != NullAudioOutputMode::Manual || running_.Load<MemoryOrder::Acquire>() == 0 ||
        frames == 0 || frames > format_.callbackFrames) {
        return;
    }

    Deliver(frames);
}

void NullAudioOutput::CaptureTo(Span<float> capture) noexcept {
    capture_ = capture;
    captured_ = 0;
}

void NullAudioOutput::Deliver(uint32 frames) noexcept {
    const uint32 channels = ChannelCount(format_.layout);
    AudioOutputView view;
    view.data = buffer_;
    view.frames = frames;
    view.channels = channels;
    view.format = AudioSampleFormat::Float32;
    callback_->Fill(view);
    const usize samples = static_cast<usize>(frames) * channels;
    if (captured_ < capture_.Size()) {
        const usize take =
            capture_.Size() - captured_ < samples ? capture_.Size() - captured_ : samples;
        platform::MemoryCopy(capture_.Data() + captured_, buffer_, take * sizeof(float));
        captured_ += take;
    }

    callbacks_.FetchAdd<MemoryOrder::Release>(1);
}

// 콜백 주기의 누적 시한(시작 + n × 주기)까지 남은 시간만 잔다. 늦게 깨도 다음 시한은 누적이라
// 드리프트가 쌓이지 않는다. 한참 늦었으면(절전 복귀 등) 시한을 지금으로 다시 맞춘다(밀린 콜백을
// 몰아서 부르지 않는다).
void NullAudioOutput::TimerMain(void* context) noexcept {
    auto& self = *static_cast<NullAudioOutput*>(context);
    const uint64 period =
        uint64{self.format_.callbackFrames} * NanosecondsPerSecond / self.format_.sampleRate;
    uint64 deadline = platform::MonotonicNanoseconds() + period;
    while (self.stopRequested_.Load<MemoryOrder::Acquire>() == 0) {
        const uint64 now = platform::MonotonicNanoseconds();
        if (now < deadline) {
            platform::SleepFor(static_cast<uint32>((deadline - now) / NanosecondsPerMillisecond));
            if (platform::MonotonicNanoseconds() < deadline) {
                platform::YieldThread();
                continue;
            }
        }

        self.Deliver(self.format_.callbackFrames);
        deadline += period;
        const uint64 after = platform::MonotonicNanoseconds();
        if (after > deadline + period) {
            deadline = after + period;
        }
    }
}

} // namespace rocks::audio
