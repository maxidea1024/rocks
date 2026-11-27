#include <Audio/AudioStreamRing.h>

#include <Audio/AudioMixerTypes.h>
#include <Core/Assert.h>
#include <Core/Math/MathUtility.h>
#include <Platform/Atomics.h>

// 스트림 PCM 링이다(Docs/Design/AudioSystem 3.40.1절).
namespace rocks::audio {

using platform::AtomicLoad;
using platform::AtomicStore;
using platform::MemoryOrder;

namespace {

// 쓰기 word의 상태 bit 자리와 위치 mask다.
constexpr uint32 StreamStateShift = 62;
constexpr uint64 StreamPositionMask = (uint64{1} << StreamStateShift) - 1;

uint64 PositionOf(uint64 word) noexcept {
    return word & StreamPositionMask;
}

AudioStreamState StateOf(uint64 word) noexcept {
    return static_cast<AudioStreamState>(word >> StreamStateShift);
}

uint64 MakeWord(uint64 position, AudioStreamState state) noexcept {
    return position | (static_cast<uint64>(state) << StreamStateShift);
}

} // namespace

Result<UniquePtr<AudioStreamRing>> AudioStreamRing::TryCreate(Allocator& allocator, uint32 channels,
                                                              uint32 capacityFrames) noexcept {
    using Made = Result<UniquePtr<AudioStreamRing>>;
    if (channels == 0 || channels > MaxClipChannels || capacityFrames < StreamRingMinFrames ||
        capacityFrames > StreamRingMaxFrames) {
        return Made::Failure(ErrorCode::InvalidArgument);
    }

    auto made = UniquePtr<AudioStreamRing>::TryMake(allocator, allocator, channels, capacityFrames);
    if (!made) {
        return Made::Failure(made.Error());
    }

    if (auto resized =
            made.Value()->samples_.TryResize(static_cast<usize>(channels) * capacityFrames);
        !resized) {
        return Made::Failure(resized.Error());
    }

    return made;
}

AudioStreamRing::AudioStreamRing(Allocator& allocator, uint32 channels,
                                 uint32 capacityFrames) noexcept
    : samples_(allocator), channels_(channels), maxChannels_(channels), capacity_(capacityFrames) {
}

uint32 AudioStreamRing::WritableFrames() const noexcept {
    // 생산자 자신의 쓰기 word는 Relaxed로 충분하다. 읽기 위치는 소비자가 Release로 올린다.
    const uint64 word = AtomicLoad<MemoryOrder::Relaxed>(&write_.word);
    if (StateOf(word) != AudioStreamState::Running) {
        return 0;
    }

    const uint64 read = AtomicLoad<MemoryOrder::Acquire>(&read_.position);
    return static_cast<uint32>(capacity_ - (PositionOf(word) - read));
}

Result<void> AudioStreamRing::Write(const PlanarConstView& pcm) noexcept {
    if (pcm.channelCount != channels_ || pcm.frameCount > WritableFrames()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    for (uint32 channel = 0; channel < channels_; ++channel) {
        const float* source = pcm.channels[channel];
        for (uint32 frame = 0; frame < pcm.frameCount; ++frame) {
            if (!math::IsFinite(source[frame])) {
                MarkFailed();
                return Result<void>::Failure(ErrorCode::CorruptData);
            }
        }
    }

    const uint64 word = AtomicLoad<MemoryOrder::Relaxed>(&write_.word);
    const uint64 position = PositionOf(word);
    for (uint32 channel = 0; channel < channels_; ++channel) {
        float* base = samples_.Data() + static_cast<usize>(channel) * capacity_;
        const float* source = pcm.channels[channel];
        for (uint32 frame = 0; frame < pcm.frameCount; ++frame) {
            base[(position + frame) % capacity_] = source[frame];
        }
    }

    // PCM을 쓴 뒤 위치를 Release로 출판한다(소비자는 Acquire로 읽고 PCM을 본다).
    AtomicStore<MemoryOrder::Release>(&write_.word,
                                      MakeWord(position + pcm.frameCount, StateOf(word)));
    return Result<void>::Success();
}

void AudioStreamRing::MarkEnded() noexcept {
    const uint64 word = AtomicLoad<MemoryOrder::Relaxed>(&write_.word);
    if (StateOf(word) == AudioStreamState::Running) {
        AtomicStore<MemoryOrder::Release>(&write_.word,
                                          MakeWord(PositionOf(word), AudioStreamState::Ended));
    }
}

void AudioStreamRing::MarkFailed() noexcept {
    const uint64 word = AtomicLoad<MemoryOrder::Relaxed>(&write_.word);
    if (StateOf(word) == AudioStreamState::Running) {
        AtomicStore<MemoryOrder::Release>(&write_.word,
                                          MakeWord(PositionOf(word), AudioStreamState::Failed));
    }
}

uint32 AudioStreamRing::ReadableFrames() const noexcept {
    const uint64 word = AtomicLoad<MemoryOrder::Acquire>(&write_.word);
    const uint64 read = AtomicLoad<MemoryOrder::Relaxed>(&read_.position);
    return static_cast<uint32>(PositionOf(word) - read);
}

AudioStreamState AudioStreamRing::State() const noexcept {
    return StateOf(AtomicLoad<MemoryOrder::Acquire>(&write_.word));
}

AudioStreamSnapshot AudioStreamRing::Snapshot() const noexcept {
    const uint64 word = AtomicLoad<MemoryOrder::Acquire>(&write_.word);
    const uint64 read = AtomicLoad<MemoryOrder::Relaxed>(&read_.position);
    AudioStreamSnapshot snapshot;
    snapshot.readable = static_cast<uint32>(PositionOf(word) - read);
    snapshot.state = StateOf(word);
    return snapshot;
}

void AudioStreamRing::Peek(uint32 frames, const PlanarView& out) const noexcept {
    ROCKS_ASSERT(frames <= ReadableFrames() && out.channelCount == channels_ &&
                 out.frameCount >= frames);
    const uint64 read = AtomicLoad<MemoryOrder::Relaxed>(&read_.position);
    for (uint32 channel = 0; channel < channels_; ++channel) {
        const float* base = samples_.Data() + static_cast<usize>(channel) * capacity_;
        float* target = out.channels[channel];
        for (uint32 frame = 0; frame < frames; ++frame) {
            target[frame] = base[(read + frame) % capacity_];
        }
    }
}

void AudioStreamRing::Consume(uint32 frames) noexcept {
    ROCKS_ASSERT(frames <= ReadableFrames());
    const uint64 read = AtomicLoad<MemoryOrder::Relaxed>(&read_.position);
    // 읽기를 마친 뒤 위치를 Release로 출판한다(생산자는 그 칸을 덮어쓸 수 있다).
    AtomicStore<MemoryOrder::Release>(&read_.position, read + frames);
}

void AudioStreamRing::Reset(uint32 channels) noexcept {
    ROCKS_ASSERT(channels != 0 && channels <= maxChannels_);
    channels_ = channels;
    AtomicStore<MemoryOrder::Relaxed>(&read_.position, uint64{0});
    AtomicStore<MemoryOrder::Release>(&write_.word, uint64{0});
}

} // namespace rocks::audio
