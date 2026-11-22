#include <Audio/AudioBuffer.h>

#include <Core/Assert.h>
#include <Core/Move.h>

namespace rocks::audio {

namespace {

// 정렬 단위 하나에 들어가는 float 수다. 채널 간격을 이 배수로 올려 모든 채널 시작을 정렬한다.
constexpr uint32 FloatsPerAlignment = static_cast<uint32>(AudioBufferAlignment / sizeof(float));

} // namespace

PlanarView PlanarView::Slice(uint32 frameOffset, uint32 frames) const noexcept {
    ROCKS_ASSERT(frameOffset <= frameCount && frames <= frameCount - frameOffset);
    PlanarView slice;
    slice.channelCount = channelCount;
    slice.frameCount = frames;
    for (uint32 channel = 0; channel < channelCount; ++channel) {
        slice.channels[channel] = channels[channel] + frameOffset;
    }

    return slice;
}

PlanarConstView::PlanarConstView(const PlanarView& view) noexcept
    : channelCount(view.channelCount), frameCount(view.frameCount) {
    for (uint32 channel = 0; channel < channelCount; ++channel) {
        channels[channel] = view.channels[channel];
    }
}

PlanarConstView PlanarConstView::Slice(uint32 frameOffset, uint32 frames) const noexcept {
    ROCKS_ASSERT(frameOffset <= frameCount && frames <= frameCount - frameOffset);
    PlanarConstView slice;
    slice.channelCount = channelCount;
    slice.frameCount = frames;
    for (uint32 channel = 0; channel < channelCount; ++channel) {
        slice.channels[channel] = channels[channel] + frameOffset;
    }

    return slice;
}

Result<PlanarBuffer> PlanarBuffer::TryCreate(uint32 channels, uint32 frameCapacity,
                                             Allocator& allocator) noexcept {
    if (channels == 0 || channels > MaxOutputChannels || frameCapacity == 0 ||
        frameCapacity > MaxBlockFrames) {
        return Result<PlanarBuffer>::Failure(ErrorCode::InvalidArgument);
    }

    // MaxBlockFrames(4096)를 정렬 단위로 올려도 4096이고 채널 8개면 32,768 float라 곱이 넘치지
    // 않는다.
    const uint32 stride =
        (frameCapacity + FloatsPerAlignment - 1) / FloatsPerAlignment * FloatsPerAlignment;
    const usize bytes = static_cast<usize>(stride) * channels * sizeof(float);
    auto allocated = allocator.Allocate(bytes, AudioBufferAlignment);
    if (!allocated) {
        return Result<PlanarBuffer>::Failure(allocated.Error());
    }

    PlanarBuffer buffer;
    buffer.allocator_ = &allocator;
    buffer.data_ = static_cast<float*>(allocated.Value());
    buffer.channelCount_ = channels;
    buffer.frameCapacity_ = frameCapacity;
    buffer.channelStride_ = stride;
    for (usize index = 0; index < static_cast<usize>(stride) * channels; ++index) {
        buffer.data_[index] = 0.0f;
    }

    return Result<PlanarBuffer>::Success(Move(buffer));
}

PlanarBuffer::PlanarBuffer(PlanarBuffer&& other) noexcept
    : allocator_(other.allocator_), data_(other.data_), channelCount_(other.channelCount_),
      frameCapacity_(other.frameCapacity_), channelStride_(other.channelStride_) {
    other.allocator_ = nullptr;
    other.data_ = nullptr;
    other.channelCount_ = 0;
    other.frameCapacity_ = 0;
    other.channelStride_ = 0;
}

PlanarBuffer& PlanarBuffer::operator=(PlanarBuffer&& other) noexcept {
    if (this != &other) {
        Release();
        allocator_ = other.allocator_;
        data_ = other.data_;
        channelCount_ = other.channelCount_;
        frameCapacity_ = other.frameCapacity_;
        channelStride_ = other.channelStride_;
        other.allocator_ = nullptr;
        other.data_ = nullptr;
        other.channelCount_ = 0;
        other.frameCapacity_ = 0;
        other.channelStride_ = 0;
    }

    return *this;
}

PlanarBuffer::~PlanarBuffer() noexcept {
    Release();
}

void PlanarBuffer::Release() noexcept {
    if (data_ != nullptr) {
        // 이 버퍼가 할당한 주소를 돌려주므로 실패는 allocator 계약 위반이다.
        const Result<void> released = allocator_->Deallocate(data_);
        ROCKS_ASSERT(released);
        (void)released;
        data_ = nullptr;
    }
}

float* PlanarBuffer::Channel(uint32 index) const noexcept {
    ROCKS_ASSERT(index < channelCount_);
    return data_ + static_cast<usize>(index) * channelStride_;
}

PlanarView PlanarBuffer::View(uint32 frames) const noexcept {
    ROCKS_ASSERT(frames <= frameCapacity_);
    PlanarView view;
    view.channelCount = channelCount_;
    view.frameCount = frames;
    for (uint32 channel = 0; channel < channelCount_; ++channel) {
        view.channels[channel] = data_ + static_cast<usize>(channel) * channelStride_;
    }

    return view;
}

void PlanarBuffer::Clear() noexcept {
    for (usize index = 0; index < static_cast<usize>(channelStride_) * channelCount_; ++index) {
        data_[index] = 0.0f;
    }
}

void Clear(const PlanarView& destination) noexcept {
    for (uint32 channel = 0; channel < destination.channelCount; ++channel) {
        float* samples = destination.channels[channel];
        for (uint32 frame = 0; frame < destination.frameCount; ++frame) {
            samples[frame] = 0.0f;
        }
    }
}

void MixAdd(const PlanarView& destination, const PlanarConstView& source, float gain) noexcept {
    ROCKS_ASSERT(destination.channelCount == source.channelCount &&
                 destination.frameCount == source.frameCount);
    for (uint32 channel = 0; channel < destination.channelCount; ++channel) {
        float* output = destination.channels[channel];
        const float* input = source.channels[channel];
        for (uint32 frame = 0; frame < destination.frameCount; ++frame) {
            output[frame] += input[frame] * gain;
        }
    }
}

void MixAddRamp(const PlanarView& destination, const PlanarConstView& source, float startGain,
                float endGain) noexcept {
    ROCKS_ASSERT(destination.channelCount == source.channelCount &&
                 destination.frameCount == source.frameCount);
    const uint32 frames = destination.frameCount;
    if (frames == 0) {
        return;
    }

    // n번째 이득을 매번 식으로 계산한다(누적 덧셈 오차가 블록 끝에 쌓이지 않게).
    const float delta = endGain - startGain;
    const float inverseFrames = 1.0f / static_cast<float>(frames);
    for (uint32 channel = 0; channel < destination.channelCount; ++channel) {
        float* output = destination.channels[channel];
        const float* input = source.channels[channel];
        for (uint32 frame = 0; frame < frames; ++frame) {
            const float gain = startGain + delta * (static_cast<float>(frame) * inverseFrames);
            output[frame] += input[frame] * gain;
        }
    }
}

void Scale(const PlanarView& destination, float gain) noexcept {
    for (uint32 channel = 0; channel < destination.channelCount; ++channel) {
        float* samples = destination.channels[channel];
        for (uint32 frame = 0; frame < destination.frameCount; ++frame) {
            samples[frame] *= gain;
        }
    }
}

} // namespace rocks::audio
