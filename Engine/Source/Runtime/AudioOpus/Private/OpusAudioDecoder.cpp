#include <AudioOpus/OpusAudioDecoder.h>

#include <Audio/OpusCodecHeader.h>
#include <Audio/VorbisChannelOrder.h>
#include <Base/Endian.h>
#include <Core/Move.h>

#include <opus_multistream.h>

namespace rocks::audio {

namespace {

// libopus 상태 정렬이다(구조체에 포인터·float 배열이 있다).
constexpr usize OpusDecoderStateAlignment = 16;

// 시작·seek(BeginChunk) 때 시작 패킷 앞에서 디코드해 버리는 패킷 수다(= 청크 복제 pre-roll).
constexpr uint64 OpusPreRollPackets = OpusPreRollFrames / OpusPacketFrames;

// Opus 청크 디코더다(3.32.4절).
class OpusChunkDecoder final : public AudioDecoder {
public:
    explicit OpusChunkDecoder(Allocator& allocator) noexcept
        : allocator_(allocator), scratch_(allocator) {
    }

    ~OpusChunkDecoder() override {
        ReleaseState();
    }

    OpusChunkDecoder(const OpusChunkDecoder&) = delete;
    OpusChunkDecoder& operator=(const OpusChunkDecoder&) = delete;

    Result<void> Open(const AudioPlatformDataHeader& header,
                      Span<const uint8> codecHeader) noexcept override {
        // 실패하면 앞 상태(열린 청크)를 남기지 않는다.
        open_ = false;
        ClearChunk();
        if (header.codec != AudioCodecId::Opus) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        // 판을 다른 규칙보다 먼저 본다. 새 판이 codec 머리·rate 규칙을 바꿔도 손상이 아니라
        // 모르는 판(Unsupported)으로 알린다.
        if (header.bitstreamVersion != OpusBitstreamVersion) {
            return Result<void>::Failure(ErrorCode::Unsupported);
        }

        if (header.channels == 0 || header.channels > MaxOutputChannels ||
            header.sampleRate != OpusPlatformSampleRate ||
            header.channelMask != DefaultAudioChannelMask(header.channels)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        auto parsed = ParseOpusCodecHeader(codecHeader, header);
        if (!parsed) {
            return Result<void>::Failure(parsed.Error());
        }

        const OpusCodecHeader& opus = parsed.Value();
        const opus_int32 stateBytes =
            opus_multistream_decoder_get_size(opus.streams, opus.coupledStreams);
        if (stateBytes <= 0) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        if (static_cast<usize>(stateBytes) > stateBytes_) {
            ReleaseState();
            auto allocated =
                allocator_.Allocate(static_cast<usize>(stateBytes), OpusDecoderStateAlignment);
            if (!allocated) {
                return Result<void>::Failure(allocated.Error());
            }

            state_ = static_cast<OpusMSDecoder*>(allocated.Value());
            stateBytes_ = static_cast<usize>(stateBytes);
        }

        if (auto resized =
                scratch_.TryResize(static_cast<usize>(OpusPacketFrames) * header.channels);
            !resized) {
            return resized;
        }

        unsigned char mapping[MaxOutputChannels] = {};
        for (uint32 channel = 0; channel < header.channels; ++channel) {
            mapping[channel] = opus.mapping[channel];
        }

        if (opus_multistream_decoder_init(state_, static_cast<opus_int32>(OpusPlatformSampleRate),
                                          static_cast<int>(header.channels), opus.streams,
                                          opus.coupledStreams, mapping) != OPUS_OK) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        channels_ = header.channels;
        open_ = true;
        return Result<void>::Success();
    }

    Result<void> BeginChunk(const AudioChunkEntry& chunk, Span<const uint8> payload,
                            uint64 startCodecFrame) noexcept override {
        ClearChunk();
        if (startCodecFrame < chunk.firstFrame ||
            startCodecFrame - chunk.firstFrame >= chunk.frameCount) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        if (auto checked = CheckChunk(chunk, payload); !checked) {
            return checked;
        }

        if (opus_multistream_decoder_ctl(state_, OPUS_RESET_STATE) != OPUS_OK) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        // 시작 패킷보다 pre-roll(OpusPreRollPackets) 앞부터 디코드해 버린다(3.32.4절). 그 지점이
        // payload 머리 앞이면 머리(복제 pre-roll 포함)부터다. 시작 패킷은 scratch에 두고 나머지를
        // DecodeNext가 낸다.
        StartChunk(chunk, payload);
        const uint64 skip = chunk.preRollFrames + (startCodecFrame - chunk.firstFrame);
        const uint64 startPacket = skip / OpusPacketFrames;
        const uint64 firstPacket =
            startPacket >= OpusPreRollPackets ? startPacket - OpusPreRollPackets : 0;
        SkipPackets(firstPacket);
        for (uint64 packet = firstPacket; packet <= startPacket; ++packet) {
            if (auto decoded = DecodePacket(); !decoded) {
                ClearChunk();
                return decoded;
            }
        }

        pendingOffset_ = static_cast<uint32>(skip % OpusPacketFrames);
        return Result<void>::Success();
    }

    Result<void> ContinueChunk(const AudioChunkEntry& chunk,
                               Span<const uint8> payload) noexcept override {
        // 앞 청크를 끝까지 냈고(DecodeNext가 0) 이 청크가 바로 이어져야 libopus 상태를 이어 쓸 수
        // 있다.
        const bool follows = active_ && finished_ && chunk.firstFrame == chunkEnd_;
        ClearChunk();
        if (!follows) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        if (auto checked = CheckChunk(chunk, payload); !checked) {
            return checked;
        }

        // 상태를 비우지 않고 복제 pre-roll 패킷은 디코드하지 않고 건너뛴다. 그래서 처음부터 순차로
        // 낸 결과는 청크 하나로 디코드한 결과(연속 디코드)와 bit까지 같다.
        StartChunk(chunk, payload);
        SkipPackets(chunk.preRollFrames / OpusPacketFrames);
        return Result<void>::Success();
    }

    Result<uint32> DecodeNext(const PlanarView& out) noexcept override {
        if (!active_ || out.channelCount != channels_ || out.frameCount < OpusPacketFrames) {
            return Result<uint32>::Failure(ErrorCode::InvalidArgument);
        }

        if (pendingOffset_ == OpusPacketFrames) {
            if (position_ == payload_.Size()) {
                finished_ = true;
                return Result<uint32>::Success(0);
            }

            // 실패하면 청크를 닫는다(손상된 libopus 상태로 다음 패킷을 이어 디코드하지 않는다).
            if (auto decoded = DecodePacket(); !decoded) {
                ClearChunk();
                return Result<uint32>::Failure(decoded.Error());
            }

            pendingOffset_ = 0;
        }

        // Vorbis 순서 교차 → 내부 순서 planar(3.32.2절).
        const uint32 count = OpusPacketFrames - pendingOffset_;
        const uint8* order = VorbisChannelOrder[channels_ - 1];
        const float* source = scratch_.Data() + static_cast<usize>(pendingOffset_) * channels_;
        for (uint32 frame = 0; frame < count; ++frame) {
            for (uint32 channel = 0; channel < channels_; ++channel) {
                out.channels[channel][frame] =
                    source[static_cast<usize>(frame) * channels_ + order[channel]];
            }
        }

        pendingOffset_ = OpusPacketFrames;
        return Result<uint32>::Success(count);
    }

    uint32 MaxFramesPerCall() const noexcept override {
        return OpusPacketFrames;
    }

private:
    // 청크 표 값과 payload 단위를 검사한다: 길이 접두를 끝까지 훑어 길이 1 이상, 합이 payload와
    // 정확히 같고, 패킷마다 첫 stream TOC가 나타내는 길이가 960 frame이며, 패킷 수 × 960 = pre-roll
    // + codec frame 수. 이미 낸 PCM이 voice에 나간 뒤 DecodeNext가 길이 때문에 실패하지 않게 여기서
    // 거른다(3.30.2절).
    Result<void> CheckChunk(const AudioChunkEntry& chunk,
                            Span<const uint8> payload) const noexcept {
        if (!open_ || payload.Size() != chunk.payloadBytes || chunk.frameCount == 0 ||
            chunk.frameCount % OpusPacketFrames != 0 ||
            chunk.preRollFrames % OpusPacketFrames != 0) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        uint64 packets = 0;
        for (usize offset = 0; offset < payload.Size();) {
            if (payload.Size() - offset < OpusLengthPrefixBytes) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }

            const usize length = LoadLittleEndian<uint16>(payload.Data() + offset);
            if (length == 0 || length > payload.Size() - offset - OpusLengthPrefixBytes) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }

            const unsigned char* packet = payload.Data() + offset + OpusLengthPrefixBytes;
            if (opus_packet_get_nb_samples(packet, static_cast<opus_int32>(length),
                                           static_cast<opus_int32>(OpusPlatformSampleRate)) !=
                static_cast<int>(OpusPacketFrames)) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }

            offset += OpusLengthPrefixBytes + length;
            ++packets;
        }

        if (packets * OpusPacketFrames != uint64{chunk.preRollFrames} + chunk.frameCount) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        return Result<void>::Success();
    }

    // 검사를 마친 청크를 연다(아직 낸 것 없음).
    void StartChunk(const AudioChunkEntry& chunk, Span<const uint8> payload) noexcept {
        payload_ = payload;
        position_ = 0;
        pendingOffset_ = OpusPacketFrames;
        chunkEnd_ = chunk.firstFrame + chunk.frameCount;
        active_ = true;
        finished_ = false;
    }

    // 패킷 count개를 디코드하지 않고 건너뛴다(CheckChunk가 단위 경계를 확인했다).
    void SkipPackets(uint64 count) noexcept {
        for (uint64 packet = 0; packet < count; ++packet) {
            position_ +=
                OpusLengthPrefixBytes + LoadLittleEndian<uint16>(payload_.Data() + position_);
        }
    }

    // position_의 패킷을 scratch에 디코드한다. 960 frame이 아니면 손상이다.
    Result<void> DecodePacket() noexcept {
        const usize length = LoadLittleEndian<uint16>(payload_.Data() + position_);
        const unsigned char* packet = payload_.Data() + position_ + OpusLengthPrefixBytes;
        const int decoded =
            opus_multistream_decode_float(state_, packet, static_cast<opus_int32>(length),
                                          scratch_.Data(), static_cast<int>(OpusPacketFrames), 0);
        if (decoded != static_cast<int>(OpusPacketFrames)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        position_ += OpusLengthPrefixBytes + length;
        return Result<void>::Success();
    }

    void ClearChunk() noexcept {
        payload_ = Span<const uint8>();
        position_ = 0;
        pendingOffset_ = OpusPacketFrames;
        chunkEnd_ = 0;
        active_ = false;
        finished_ = false;
    }

    void ReleaseState() noexcept {
        if (state_ != nullptr) {
            (void)allocator_.Deallocate(state_);
            state_ = nullptr;
            stateBytes_ = 0;
        }
    }

    Allocator& allocator_;
    OpusMSDecoder* state_ = nullptr;
    usize stateBytes_ = 0;
    Array<float> scratch_;
    Span<const uint8> payload_;
    usize position_ = 0;
    uint32 pendingOffset_ = OpusPacketFrames;
    uint32 channels_ = 0;
    // 열린 청크의 끝 codec frame이다(ContinueChunk가 다음 청크가 이어지는지 본다).
    uint64 chunkEnd_ = 0;
    bool open_ = false;
    bool active_ = false;
    // 열린 청크를 끝까지 냈다(DecodeNext가 0을 돌려줬다).
    bool finished_ = false;
};

Result<UniquePtr<AudioDecoder>> CreateOpusDecoder(Allocator& allocator) noexcept {
    auto made = UniquePtr<OpusChunkDecoder>::TryMake(allocator, allocator);
    if (!made) {
        return Result<UniquePtr<AudioDecoder>>::Failure(made.Error());
    }

    return Result<UniquePtr<AudioDecoder>>::Success(UniquePtr<AudioDecoder>(Move(made.Value())));
}

} // namespace

Result<void> RegisterOpusAudioDecoder(AudioDecoderTable& table) noexcept {
    return table.Register(AudioCodecId::Opus, &CreateOpusDecoder);
}

} // namespace rocks::audio
