#include <Audio/Adpcm.h>
#include <Audio/AudioDecoder.h>

#include <Base/NumericLimits.h>
#include <Core/Assert.h>
#include <Core/Move.h>

namespace rocks::audio {

namespace {

bool CodecInRange(AudioCodecId codec) noexcept {
    const uint32 value = static_cast<uint32>(codec);
    return value != 0 && value < MaxAudioCodecs;
}

// 내장 디코더(PCM16·ADPCM)가 받을 머리인지 본다(codec·채널·rate). 검증하지 않은 머리로 Open해도
// PlanarView 범위 밖을 쓰지 않게 채널 수를 여기서도 본다.
bool BuiltInHeaderValid(const AudioPlatformDataHeader& header, AudioCodecId codec) noexcept {
    return header.codec == codec && header.channels > 0 && header.channels <= MaxOutputChannels &&
           header.sampleRate >= MinPlatformSampleRate && header.sampleRate <= MaxPlatformSampleRate;
}

// out이 이 디코더에 맞는지 본다.
bool OutputFits(const PlanarView& out, uint32 channels, uint32 maxFrames) noexcept {
    return out.channelCount == channels && out.frameCount >= maxFrames;
}

// ContinueChunk 순서 규칙(3.30.2절 청크 경계 규칙): 열린 청크를 끝까지 냈고(DecodeNext가 0) 다음
// 청크가 그 끝에서 시작한다.
bool ContinuesChunk(bool drained, uint64 chunkEnd, const AudioChunkEntry& next) noexcept {
    return drained && next.firstFrame == chunkEnd;
}

// PCM16 디코더: 단위 = 1024 frame 교차 int16 LE(3.30.2절).
class Pcm16Decoder final : public AudioDecoder {
public:
    Result<void> Open(const AudioPlatformDataHeader& header,
                      Span<const uint8> codecHeader) noexcept override {
        // 실패하면 앞 상태(열린 청크)를 남기지 않는다.
        open_ = false;
        payload_ = Span<const uint8>();
        // 판은 codec 다음, 다른 규칙보다 먼저 본다(새 판의 규칙 변화를 손상으로 알리지 않게).
        if (header.codec == AudioCodecId::Pcm16 &&
            header.bitstreamVersion != Pcm16BitstreamVersion) {
            return Result<void>::Failure(ErrorCode::Unsupported);
        }

        if (!BuiltInHeaderValid(header, AudioCodecId::Pcm16) || codecHeader.Size() != 0) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        channels_ = header.channels;
        open_ = true;
        return Result<void>::Success();
    }

    Result<void> BeginChunk(const AudioChunkEntry& chunk, Span<const uint8> payload,
                            uint64 startCodecFrame) noexcept override {
        payload_ = Span<const uint8>();
        finished_ = false;
        const uint64 frameBytes = uint64{channels_} * Pcm16SampleBytes;
        if (!open_ || chunk.preRollFrames != 0 || payload.Size() != chunk.payloadBytes ||
            payload.Size() != uint64{chunk.frameCount} * frameBytes ||
            startCodecFrame < chunk.firstFrame ||
            startCodecFrame - chunk.firstFrame >= chunk.frameCount) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        payload_ = payload;
        frames_ = chunk.frameCount;
        chunkEnd_ = chunk.firstFrame + chunk.frameCount;
        position_ = static_cast<uint32>(startCodecFrame - chunk.firstFrame);
        return Result<void>::Success();
    }

    // 청크 사이 상태가 없으므로 이어 열기는 첫 frame부터의 BeginChunk다. 순서 규칙은 상태 있는
    // codec과 같게 지킨다.
    Result<void> ContinueChunk(const AudioChunkEntry& chunk,
                               Span<const uint8> payload) noexcept override {
        if (!ContinuesChunk(payload_.Size() != 0 && finished_, chunkEnd_, chunk)) {
            payload_ = Span<const uint8>();
            finished_ = false;
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        return BeginChunk(chunk, payload, chunk.firstFrame);
    }

    Result<uint32> DecodeNext(const PlanarView& out) noexcept override {
        if (payload_.Size() == 0 || !OutputFits(out, channels_, Pcm16UnitFrames)) {
            return Result<uint32>::Failure(ErrorCode::InvalidArgument);
        }

        // 단위 경계까지(시작 단위는 나머지, 마지막 단위는 남은 frame)다.
        const uint32 unitLeft = Pcm16UnitFrames - position_ % Pcm16UnitFrames;
        const uint32 count = frames_ - position_ < unitLeft ? frames_ - position_ : unitLeft;
        if (count == 0) {
            finished_ = true;
            return Result<uint32>::Success(0);
        }

        const uint8* source =
            payload_.Data() + static_cast<usize>(position_) * channels_ * Pcm16SampleBytes;
        for (uint32 frame = 0; frame < count; ++frame) {
            for (uint32 channel = 0; channel < channels_; ++channel) {
                const uint8* sample =
                    source + (static_cast<usize>(frame) * channels_ + channel) * Pcm16SampleBytes;
                const int16 value =
                    static_cast<int16>(static_cast<uint16>(sample[0] | (sample[1] << 8)));
                out.channels[channel][frame] = static_cast<float>(value) * Pcm16SampleScale;
            }
        }

        position_ += count;
        return Result<uint32>::Success(count);
    }

    uint32 MaxFramesPerCall() const noexcept override {
        return Pcm16UnitFrames;
    }

private:
    Span<const uint8> payload_;
    uint64 chunkEnd_ = 0;
    uint32 channels_ = 0;
    uint32 frames_ = 0;
    uint32 position_ = 0;
    bool open_ = false;
    bool finished_ = false;
};

// ADPCM 디코더: 단위 = 블록 묶음(채널 × 512 byte, 3.24.1절).
class AdpcmChunkDecoder final : public AudioDecoder {
public:
    Result<void> Open(const AudioPlatformDataHeader& header,
                      Span<const uint8> codecHeader) noexcept override {
        // 실패하면 앞 상태(열린 청크)를 남기지 않는다.
        open_ = false;
        payload_ = Span<const uint8>();
        // 판은 codec 다음, 다른 규칙보다 먼저 본다(새 판의 규칙 변화를 손상으로 알리지 않게).
        if (header.codec == AudioCodecId::Adpcm &&
            header.bitstreamVersion != AdpcmBitstreamVersion) {
            return Result<void>::Failure(ErrorCode::Unsupported);
        }

        if (!BuiltInHeaderValid(header, AudioCodecId::Adpcm) || codecHeader.Size() != 0) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        channels_ = header.channels;
        open_ = true;
        return Result<void>::Success();
    }

    Result<void> BeginChunk(const AudioChunkEntry& chunk, Span<const uint8> payload,
                            uint64 startCodecFrame) noexcept override {
        payload_ = Span<const uint8>();
        finished_ = false;
        if (!open_ || chunk.preRollFrames != 0 || payload.Size() != chunk.payloadBytes ||
            payload.Size() != AdpcmPayloadBytes(channels_, chunk.frameCount) ||
            startCodecFrame < chunk.firstFrame ||
            startCodecFrame - chunk.firstFrame >= chunk.frameCount) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        payload_ = payload;
        frames_ = chunk.frameCount;
        chunkEnd_ = chunk.firstFrame + chunk.frameCount;
        position_ = static_cast<uint32>(startCodecFrame - chunk.firstFrame);
        return Result<void>::Success();
    }

    // 청크 사이 상태가 없으므로 이어 열기는 첫 frame부터의 BeginChunk다. 순서 규칙은 상태 있는
    // codec과 같게 지킨다.
    Result<void> ContinueChunk(const AudioChunkEntry& chunk,
                               Span<const uint8> payload) noexcept override {
        if (!ContinuesChunk(payload_.Size() != 0 && finished_, chunkEnd_, chunk)) {
            payload_ = Span<const uint8>();
            finished_ = false;
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        return BeginChunk(chunk, payload, chunk.firstFrame);
    }

    Result<uint32> DecodeNext(const PlanarView& out) noexcept override {
        if (payload_.Size() == 0 || !OutputFits(out, channels_, AdpcmFramesPerBlock)) {
            return Result<uint32>::Failure(ErrorCode::InvalidArgument);
        }

        const uint32 unitLeft = AdpcmFramesPerBlock - position_ % AdpcmFramesPerBlock;
        const uint32 count = frames_ - position_ < unitLeft ? frames_ - position_ : unitLeft;
        if (count == 0) {
            finished_ = true;
            return Result<uint32>::Success(0);
        }

        if (auto decoded =
                DecodeAdpcmFrames(payload_, channels_, frames_, position_, out.Slice(0, count));
            !decoded) {
            return Result<uint32>::Failure(decoded.Error());
        }

        position_ += count;
        return Result<uint32>::Success(count);
    }

    uint32 MaxFramesPerCall() const noexcept override {
        return AdpcmFramesPerBlock;
    }

private:
    Span<const uint8> payload_;
    uint64 chunkEnd_ = 0;
    uint32 channels_ = 0;
    uint32 frames_ = 0;
    uint32 position_ = 0;
    bool open_ = false;
    bool finished_ = false;
};

template <typename Decoder>
Result<UniquePtr<AudioDecoder>> CreateDecoder(Allocator& allocator) noexcept {
    auto made = UniquePtr<Decoder>::TryMake(allocator);
    if (!made) {
        return Result<UniquePtr<AudioDecoder>>::Failure(made.Error());
    }

    return Result<UniquePtr<AudioDecoder>>::Success(UniquePtr<AudioDecoder>(Move(made.Value())));
}

} // namespace

Result<void> AudioDecoderTable::Register(AudioCodecId codec, AudioDecoderFactory factory) noexcept {
    if (!CodecInRange(codec) || factory == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    AudioDecoderFactory& slot = factories_[static_cast<uint32>(codec)];
    if (slot != nullptr) {
        return Result<void>::Failure(ErrorCode::AlreadyExists);
    }

    slot = factory;
    return Result<void>::Success();
}

Result<UniquePtr<AudioDecoder>> AudioDecoderTable::Create(AudioCodecId codec,
                                                          Allocator& allocator) const noexcept {
    if (!Contains(codec)) {
        return Result<UniquePtr<AudioDecoder>>::Failure(ErrorCode::NotFound);
    }

    return factories_[static_cast<uint32>(codec)](allocator);
}

bool AudioDecoderTable::Contains(AudioCodecId codec) const noexcept {
    return CodecInRange(codec) && factories_[static_cast<uint32>(codec)] != nullptr;
}

Result<void> RegisterBuiltInAudioDecoders(AudioDecoderTable& table) noexcept {
    if (auto pcm = table.Register(AudioCodecId::Pcm16, &CreateDecoder<Pcm16Decoder>); !pcm) {
        return pcm;
    }

    return table.Register(AudioCodecId::Adpcm, &CreateDecoder<AdpcmChunkDecoder>);
}

const float* DecodedAudioPcm::Channel(uint32 index) const noexcept {
    ROCKS_ASSERT(index < channels_);
    return samples_.Data() + static_cast<usize>(index) * frames_;
}

Result<float*> DecodedAudioPcm::TryAllocate(uint32 channels, uint32 sampleRate,
                                            uint64 frames) noexcept {
    // 머리의 frame 수는 64비트라 곱이 주소 공간을 넘을 수 있다.
    if (channels == 0 || frames > NumericLimits<usize>::Max() / sizeof(float) / channels) {
        return Result<float*>::Failure(ErrorCode::CapacityExceeded);
    }

    if (auto resized = samples_.TryResize(static_cast<usize>(frames * channels)); !resized) {
        return Result<float*>::Failure(resized.Error());
    }

    channels_ = channels;
    sampleRate_ = sampleRate;
    frames_ = frames;
    return Result<float*>::Success(samples_.Data());
}

void DecodedAudioPcm::Reset() noexcept {
    samples_.Clear();
    channels_ = 0;
    sampleRate_ = 0;
    frames_ = 0;
}

namespace {

Result<void> DecodeAllInto(const AudioPlatformDataView& view, const AudioDecoderTable& table,
                           Allocator& allocator, DecodedAudioPcm& out) noexcept {
    const AudioPlatformDataHeader& header = view.Header();
    if (!view.HasAllPayload()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto created = table.Create(header.codec, allocator);
    if (!created) {
        return Result<void>::Failure(created.Error());
    }

    AudioDecoder& decoder = *created.Value();
    if (auto opened = decoder.Open(header, view.CodecHeader()); !opened) {
        return opened;
    }

    // 단위 하나를 받을 scratch(채널 × MaxFramesPerCall)다.
    const uint32 unitFrames = decoder.MaxFramesPerCall();
    Array<float> scratch(allocator);
    if (auto resized = scratch.TryResize(static_cast<usize>(unitFrames) * header.channels);
        !resized) {
        return resized;
    }

    PlanarView unit;
    unit.channelCount = header.channels;
    unit.frameCount = unitFrames;
    for (uint32 channel = 0; channel < header.channels; ++channel) {
        unit.channels[channel] = scratch.Data() + static_cast<usize>(channel) * unitFrames;
    }

    // 할당 전에 모든 청크의 SHA와 단위 경계·크기를 먼저 본다(BeginChunk). 그래서 손상된 frame 수로
    // 큰 버퍼를 잡지 않는다 — PCM16·ADPCM은 이 검사가 frame 수를 payload byte에 묶는다.
    for (uint32 index = 0; index < header.chunkCount; ++index) {
        const AudioChunkEntry chunk = view.Chunk(index);
        const Span<const uint8> payload = view.ChunkPayload(index);
        if (auto verified = VerifyAudioChunk(chunk, payload); !verified) {
            return verified;
        }

        if (auto begun = decoder.BeginChunk(chunk, payload, chunk.firstFrame); !begun) {
            return begun;
        }
    }

    auto allocated = out.TryAllocate(header.channels, header.sampleRate, header.frames);
    if (!allocated) {
        return Result<void>::Failure(allocated.Error());
    }

    float* planar = allocated.Value();
    const uint64 validStart = header.encoderDelay;
    const uint64 validEnd = header.encoderDelay + header.frames;
    // 청크 0은 BeginChunk로 열고, 그 뒤 청크는 ContinueChunk로 상태를 이어 연다(처음부터 재생과
    // 같다 — 3.30.2절 청크 경계 규칙).
    for (uint32 index = 0; index < header.chunkCount; ++index) {
        const AudioChunkEntry chunk = view.Chunk(index);
        const Span<const uint8> payload = view.ChunkPayload(index);
        if (auto begun = index == 0 ? decoder.BeginChunk(chunk, payload, chunk.firstFrame)
                                    : decoder.ContinueChunk(chunk, payload);
            !begun) {
            return begun;
        }

        uint64 codecFrame = chunk.firstFrame;
        const uint64 chunkEnd = chunk.firstFrame + chunk.frameCount;
        while (true) {
            auto written = decoder.DecodeNext(unit);
            if (!written) {
                return Result<void>::Failure(written.Error());
            }

            const uint32 count = written.Value();
            if (count == 0) {
                break;
            }

            if (count > chunkEnd - codecFrame) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }

            // codec frame [codecFrame, codecFrame + count) 중 유효 구간만 옮긴다.
            for (uint32 frame = 0; frame < count; ++frame) {
                const uint64 at = codecFrame + frame;
                if (at < validStart || at >= validEnd) {
                    continue;
                }

                for (uint32 channel = 0; channel < header.channels; ++channel) {
                    planar[static_cast<usize>(channel) * header.frames + (at - validStart)] =
                        unit.channels[channel][frame];
                }
            }

            codecFrame += count;
        }

        // 디코더가 청크 frame 수만큼 정확히 내야 한다.
        if (codecFrame != chunkEnd) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    return Result<void>::Success();
}

} // namespace

Result<void> DecodeAudioPlatformData(const AudioPlatformDataView& view,
                                     const AudioDecoderTable& table, Allocator& allocator,
                                     DecodedAudioPcm& out) noexcept {
    out.Reset();
    Result<void> decoded = DecodeAllInto(view, table, allocator, out);
    if (!decoded) {
        out.Reset();
    }

    return decoded;
}

} // namespace rocks::audio
