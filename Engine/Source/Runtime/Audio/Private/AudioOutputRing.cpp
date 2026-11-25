#include <Audio/AudioOutputRing.h>

#include <Platform/Fatal.h>
#include <Platform/Memory.h>

namespace rocks::audio {

namespace {

using platform::MemoryOrder;

constexpr float Int16Scale = 32767.0f;

int16 ToInt16(float sample) noexcept {
    const float clamped = sample > 1.0f ? 1.0f : (sample < -1.0f ? -1.0f : sample);
    const float scaled = clamped * Int16Scale;
    return static_cast<int16>(scaled >= 0.0f ? scaled + 0.5f : scaled - 0.5f);
}

} // namespace

uint32 ChooseOutputBufferCount(uint32 callbackFrames, uint32 blockFrames,
                               uint32 requested) noexcept {
    uint32 count = MinOutputBufferCount;
    if (blockFrames != 0) {
        const uint64 covering = (uint64{callbackFrames} + blockFrames - 1) / blockFrames + 1;
        if (covering > count) {
            count = covering > MaxOutputBufferCount ? MaxOutputBufferCount
                                                    : static_cast<uint32>(covering);
        }
    }

    if (requested > count) {
        count = requested;
    }

    return count > MaxOutputBufferCount ? MaxOutputBufferCount : count;
}

Result<void> AudioOutputRing::Initialize(Allocator& allocator, uint32 bufferCount,
                                         uint32 blockFrames, uint32 channels,
                                         platform::RealtimeWakeSignal* wake) noexcept {
    if (samples_ != nullptr || bufferCount < MinOutputBufferCount ||
        bufferCount > MaxOutputBufferCount || blockFrames == 0 || channels == 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const uint64 floats = uint64{bufferCount} * blockFrames * channels;
    if (floats > MaxSize / sizeof(float)) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    auto memory = allocator.Allocate(static_cast<usize>(floats) * sizeof(float), alignof(float));
    if (!memory) {
        return Result<void>::Failure(memory.Error());
    }

    samples_ = static_cast<float*>(memory.Value());
    platform::MemoryZero(samples_, static_cast<usize>(floats) * sizeof(float));
    allocator_ = &allocator;
    wake_ = wake;
    bufferCount_ = bufferCount;
    blockFrames_ = blockFrames;
    channels_ = channels;
    readOffset_ = 0;
    written_.Store<MemoryOrder::Relaxed>(0);
    read_.Store<MemoryOrder::Relaxed>(0);
    underrunFrames_.Store<MemoryOrder::Relaxed>(0);
    underrunCallbacks_.Store<MemoryOrder::Relaxed>(0);
    consumedFrames_.Store<MemoryOrder::Relaxed>(0);
    return Result<void>::Success();
}

void AudioOutputRing::Shutdown() noexcept {
    if (samples_ == nullptr) {
        return;
    }

    if (!allocator_->Deallocate(samples_)) {
        platform::Fatal("AudioOutputRing storage deallocation was refused");
    }

    samples_ = nullptr;
    bufferCount_ = 0;
}

bool AudioOutputRing::HasFreeSlot() const noexcept {
    return written_.Load<MemoryOrder::Relaxed>() - read_.Load<MemoryOrder::Acquire>() <
           bufferCount_;
}

Span<float> AudioOutputRing::BeginWrite() noexcept {
    const uint64 slot = written_.Load<MemoryOrder::Relaxed>() % bufferCount_;
    const usize blockFloats = static_cast<usize>(blockFrames_) * channels_;
    return Span<float>(samples_ + static_cast<usize>(slot) * blockFloats, blockFloats);
}

void AudioOutputRing::CommitWrite() noexcept {
    written_.Store<MemoryOrder::Release>(written_.Load<MemoryOrder::Relaxed>() + 1);
}

void AudioOutputRing::WriteSilence(const AudioOutputView& view, uint32 firstFrame) noexcept {
    const usize samples = static_cast<usize>(view.frames - firstFrame) * view.channels;
    const usize offset = static_cast<usize>(firstFrame) * view.channels;
    if (view.format == AudioSampleFormat::Float32) {
        platform::MemoryZero(static_cast<float*>(view.data) + offset, samples * sizeof(float));
    } else {
        platform::MemoryZero(static_cast<int16*>(view.data) + offset, samples * sizeof(int16));
    }
}

void AudioOutputRing::Read(const AudioOutputView& view) noexcept {
    if (view.frames == 0) {
        return;
    }

    if (samples_ == nullptr || view.channels != channels_) {
        WriteSilence(view, 0);
        underrunFrames_.FetchAdd<MemoryOrder::Relaxed>(view.frames);
        underrunCallbacks_.FetchAdd<MemoryOrder::Relaxed>(1);
        return;
    }

    const usize blockFloats = static_cast<usize>(blockFrames_) * channels_;
    uint64 read = read_.Load<MemoryOrder::Relaxed>();
    const uint64 written = written_.Load<MemoryOrder::Acquire>();
    bool freedBlock = false;
    uint32 frame = 0;
    while (frame < view.frames && read != written) {
        const float* block = samples_ + static_cast<usize>(read % bufferCount_) * blockFloats;
        const uint32 take = view.frames - frame < blockFrames_ - readOffset_
                                ? view.frames - frame
                                : blockFrames_ - readOffset_;
        const float* source = block + static_cast<usize>(readOffset_) * channels_;
        const usize count = static_cast<usize>(take) * channels_;
        const usize offset = static_cast<usize>(frame) * channels_;
        if (view.format == AudioSampleFormat::Float32) {
            platform::MemoryCopy(static_cast<float*>(view.data) + offset, source,
                                 count * sizeof(float));
        } else {
            int16* target = static_cast<int16*>(view.data) + offset;
            for (usize index = 0; index < count; ++index) {
                target[index] = ToInt16(source[index]);
            }
        }

        frame += take;
        readOffset_ += take;
        if (readOffset_ == blockFrames_) {
            readOffset_ = 0;
            ++read;
            // 칸을 다 읽었다: 생산자가 이 칸을 다시 쓸 수 있다.
            read_.Store<MemoryOrder::Release>(read);
            freedBlock = true;
        }
    }

    consumedFrames_.FetchAdd<MemoryOrder::Relaxed>(view.frames);
    if (frame < view.frames) {
        WriteSilence(view, frame);
        underrunFrames_.FetchAdd<MemoryOrder::Relaxed>(view.frames - frame);
        underrunCallbacks_.FetchAdd<MemoryOrder::Relaxed>(1);
    }

    if (freedBlock && wake_ != nullptr) {
        wake_->Signal();
    }
}

AudioOutputRingCounters AudioOutputRing::CountersSnapshot() const noexcept {
    AudioOutputRingCounters counters;
    counters.underrunFrames = underrunFrames_.Load<MemoryOrder::Relaxed>();
    counters.underrunCallbacks = underrunCallbacks_.Load<MemoryOrder::Relaxed>();
    counters.consumedFrames = consumedFrames_.Load<MemoryOrder::Relaxed>();
    return counters;
}

} // namespace rocks::audio
