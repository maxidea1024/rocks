#include <Core/Assert.h>
#include <Core/Math/MathUtility.h>
#include <AudioAssets/SoundPcm.h>

namespace rocks {

SoundPcm::SoundPcm(Allocator& allocator) noexcept : samples_(allocator) {
}

Result<SharedPtr<SoundPcm>> SoundPcm::TryAllocate(Allocator& allocator, uint32 channelCount,
                                                  uint32 frameCount, uint32 sampleRate) noexcept {
    using Created = Result<SharedPtr<SoundPcm>>;
    if (channelCount == 0 || channelCount > audio::MaxClipChannels || frameCount == 0 ||
        sampleRate < audio::MinClipSampleRate || sampleRate > audio::MaxClipSampleRate) {
        return Created::Failure(ErrorCode::InvalidArgument);
    }

    auto made = SharedPtr<SoundPcm>::TryMake(allocator, allocator);
    if (!made) {
        return Created::Failure(made.Error());
    }

    SoundPcm& pcm = *made.Value();
    const usize count = static_cast<usize>(channelCount) * frameCount;
    if (!pcm.samples_.TryResize(count)) {
        return Created::Failure(ErrorCode::OutOfMemory);
    }

    for (usize index = 0; index < count; ++index) {
        pcm.samples_[index] = 0.0f;
    }

    pcm.channelCount_ = channelCount;
    pcm.frameCount_ = frameCount;
    pcm.sampleRate_ = sampleRate;
    return Created::Success(Move(made.Value()));
}

Span<const float> SoundPcm::Channel(uint32 index) const noexcept {
    ROCKS_ASSERT(index < channelCount_);
    return Span<const float>(samples_.Data() + static_cast<usize>(index) * frameCount_,
                             frameCount_);
}

Span<float> SoundPcm::MutableChannel(uint32 index) noexcept {
    ROCKS_ASSERT(index < channelCount_);
    return Span<float>(samples_.Data() + static_cast<usize>(index) * frameCount_, frameCount_);
}

bool SoundPcm::IsFinite() const noexcept {
    for (const float sample : samples_) {
        if (!math::IsFinite(sample)) {
            return false;
        }
    }

    return true;
}

} // namespace rocks
