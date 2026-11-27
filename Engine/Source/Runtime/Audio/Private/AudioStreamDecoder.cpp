#include <Audio/AudioStreamDecoder.h>

#include <Audio/AudioMixerTypes.h>
#include <Core/Assert.h>
#include <Core/Move.h>

// 스트림 생산자다(Docs/Design/AudioSystem 3.40.2절).
namespace rocks::audio {

Result<UniquePtr<AudioStreamDecoder>>
AudioStreamDecoder::TryCreate(Allocator& allocator, UniquePtr<AudioDecoder> decoder,
                              const AudioStreamDecoderDesc& desc) noexcept {
    using Made = Result<UniquePtr<AudioStreamDecoder>>;
    if (desc.view == nullptr || desc.chunks == nullptr || !decoder) {
        return Made::Failure(ErrorCode::InvalidArgument);
    }

    const AudioPlatformDataHeader& header = desc.view->Header();
    const uint64 frames = header.frames;
    const bool wholeLoop = desc.loopBegin == 0 && desc.loopEnd == 0;
    const uint64 loopEnd = wholeLoop ? frames : desc.loopEnd;
    if (header.channels == 0 || header.channels > MaxClipChannels || desc.startFrame >= frames ||
        (!wholeLoop && !(desc.loopBegin < desc.loopEnd && desc.loopEnd <= frames)) ||
        (desc.loop && desc.startFrame >= loopEnd)) {
        return Made::Failure(ErrorCode::InvalidArgument);
    }

    if (auto opened = decoder->Open(header, desc.view->CodecHeader()); !opened) {
        return Made::Failure(opened.Error());
    }

    auto made = UniquePtr<AudioStreamDecoder>::TryMake(allocator, allocator, Move(decoder), desc);
    if (!made) {
        return Made::Failure(made.Error());
    }

    AudioStreamDecoder& stream = *made.Value();
    const uint32 unitFrames = stream.decoder_->MaxFramesPerCall();
    if (auto resized = stream.scratch_.TryResize(static_cast<usize>(unitFrames) * header.channels);
        !resized) {
        return Made::Failure(resized.Error());
    }

    stream.unit_.channelCount = header.channels;
    stream.unit_.frameCount = unitFrames;
    for (uint32 channel = 0; channel < header.channels; ++channel) {
        stream.unit_.channels[channel] =
            stream.scratch_.Data() + static_cast<usize>(channel) * unitFrames;
    }

    return made;
}

AudioStreamDecoder::AudioStreamDecoder(Allocator& allocator, UniquePtr<AudioDecoder> decoder,
                                       const AudioStreamDecoderDesc& desc) noexcept
    : decoder_(Move(decoder)), desc_(desc), header_(desc.view->Header()), scratch_(allocator),
      effective_(desc.startFrame) {
    const bool wholeLoop = desc.loopBegin == 0 && desc.loopEnd == 0;
    loopBegin_ = wholeLoop ? 0 : desc.loopBegin;
    loopEnd_ = wholeLoop ? static_cast<uint32>(header_.frames) : desc.loopEnd;
}

AudioStreamDecoder::~AudioStreamDecoder() noexcept {
    ReleaseChunk();
}

uint64 AudioStreamDecoder::SegmentEnd() const noexcept {
    return desc_.loop ? loopEnd_ : header_.frames;
}

void AudioStreamDecoder::ReleaseChunk() noexcept {
    if (chunkOpen_) {
        desc_.chunks->Release(chunkIndex_);
        chunkOpen_ = false;
    }

    awaitingNext_ = false;
}

void AudioStreamDecoder::Fail(AudioStreamRing& ring) noexcept {
    ring.MarkFailed();
    finished_ = true;
    pendingFrames_ = 0;
    ReleaseChunk();
}

bool AudioStreamDecoder::OpenAtCursor(AudioStreamRing& ring) noexcept {
    const uint64 target = effective_ + header_.encoderDelay;
    const AudioPlatformDataView& view = *desc_.view;
    uint32 index = 0;
    while (index < header_.chunkCount) {
        const AudioChunkEntry chunk = view.Chunk(index);
        if (target >= chunk.firstFrame && target - chunk.firstFrame < chunk.frameCount) {
            break;
        }

        ++index;
    }

    // 청크 표가 codec frame 전체를 덮는다(Parse가 확인한다) — 못 찾으면 손상이다.
    if (index == header_.chunkCount) {
        Fail(ring);
        return false;
    }

    const AudioChunkAcquire got = desc_.chunks->TryAcquire(index);
    if (got.status == AudioChunkStatus::Pending) {
        return false;
    }

    if (got.status == AudioChunkStatus::Failed) {
        Fail(ring);
        return false;
    }

    chunkOpen_ = true;
    chunkIndex_ = index;
    if (auto begun = decoder_->BeginChunk(view.Chunk(index), got.payload, target); !begun) {
        Fail(ring);
        return false;
    }

    codecCursor_ = target;
    awaitingNext_ = false;
    if (index + 1 < header_.chunkCount) {
        desc_.chunks->Prefetch(index + 1);
    }

    return true;
}

bool AudioStreamDecoder::ContinueNextChunk(AudioStreamRing& ring) noexcept {
    const uint32 next = chunkIndex_ + 1;
    // 데이터가 구간 끝 전에 끝났다(청크 표·frame 수 불일치).
    if (next >= header_.chunkCount) {
        Fail(ring);
        return false;
    }

    const AudioChunkAcquire got = desc_.chunks->TryAcquire(next);
    if (got.status == AudioChunkStatus::Pending) {
        return false;
    }

    if (got.status == AudioChunkStatus::Failed) {
        Fail(ring);
        return false;
    }

    desc_.chunks->Release(chunkIndex_);
    chunkIndex_ = next;
    if (auto continued = decoder_->ContinueChunk(desc_.view->Chunk(next), got.payload);
        !continued) {
        Fail(ring);
        return false;
    }

    awaitingNext_ = false;
    if (next + 1 < header_.chunkCount) {
        desc_.chunks->Prefetch(next + 1);
    }

    return true;
}

bool AudioStreamDecoder::FlushPending(AudioStreamRing& ring) noexcept {
    const uint32 writable = ring.WritableFrames();
    const uint32 frames = pendingFrames_ < writable ? pendingFrames_ : writable;
    if (frames == 0) {
        return false;
    }

    PlanarConstView view;
    view.channelCount = header_.channels;
    view.frameCount = frames;
    for (uint32 channel = 0; channel < header_.channels; ++channel) {
        view.channels[channel] = unit_.channels[channel] + pendingOffset_;
    }

    // 비유한 표본이면 링이 스스로 Failed가 된다.
    if (auto written = ring.Write(view); !written) {
        Fail(ring);
        return false;
    }

    pendingOffset_ += frames;
    pendingFrames_ -= frames;
    effective_ += frames;
    return pendingFrames_ == 0;
}

void AudioStreamDecoder::Pump(AudioStreamRing& ring) noexcept {
    if (finished_) {
        return;
    }

    ROCKS_ASSERT(ring.ChannelCount() == header_.channels);
    while (true) {
        if (pendingFrames_ > 0 && !FlushPending(ring)) {
            return;
        }

        // 구간 끝: non-loop면 끝 상태, loop면 남은 단위 PCM을 버리고 loop 시작에서 다시 연다.
        if (effective_ >= SegmentEnd()) {
            if (!desc_.loop) {
                ring.MarkEnded();
                finished_ = true;
                ReleaseChunk();
                return;
            }

            ReleaseChunk();
            pendingFrames_ = 0;
            effective_ = loopBegin_;
        }

        if (ring.WritableFrames() == 0) {
            return;
        }

        if (!chunkOpen_ && !OpenAtCursor(ring)) {
            return;
        }

        if (awaitingNext_ && !ContinueNextChunk(ring)) {
            return;
        }

        auto written = decoder_->DecodeNext(unit_);
        if (!written) {
            Fail(ring);
            return;
        }

        const AudioChunkEntry chunk = desc_.view->Chunk(chunkIndex_);
        const uint64 chunkEnd = chunk.firstFrame + chunk.frameCount;
        const uint32 count = written.Value();
        if (count == 0) {
            // 디코더가 청크 frame 수만큼 정확히 내야 한다(DecodeAudioPlatformData와 같은 규칙).
            if (codecCursor_ != chunkEnd) {
                Fail(ring);
                return;
            }

            awaitingNext_ = true;
            continue;
        }

        if (count > chunkEnd - codecCursor_) {
            Fail(ring);
            return;
        }

        // codec frame [codecCursor_, +count)의 유효 구간 [effective_, 구간 끝)만 남긴다. 디코드는
        // 늘 다음에 쓸 유효 frame의 codec frame부터 열리므로 단위 첫 frame = effective_이다.
        if (codecCursor_ < header_.encoderDelay ||
            codecCursor_ - header_.encoderDelay != effective_) {
            Fail(ring);
            return;
        }

        codecCursor_ += count;

        const uint64 remaining = SegmentEnd() - effective_;
        pendingOffset_ = 0;
        pendingFrames_ = static_cast<uint32>(remaining < count ? remaining : count);
    }
}

AudioChunkAcquire ResidentAudioChunkSource::TryAcquire(uint32 index) noexcept {
    AudioChunkAcquire got;
    if (index >= view_.Header().chunkCount || !view_.HasAllPayload()) {
        got.status = AudioChunkStatus::Failed;
        got.error = ErrorCode::InvalidArgument;
        return got;
    }

    got.status = AudioChunkStatus::Ready;
    got.payload = view_.ChunkPayload(index);
    return got;
}

} // namespace rocks::audio
