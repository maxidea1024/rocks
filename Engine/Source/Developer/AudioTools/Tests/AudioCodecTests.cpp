#include <Audio/Adpcm.h>
#include <Audio/AudioDecoder.h>
#include <Audio/AudioPlatformData.h>
#include <AudioTools/AdpcmEncoder.h>
#include <AudioTools/AudioEncoder.h>
#include <AudioTools/AudioPlatformDataWriter.h>
#include <Base/Endian.h>
#include <Base/InitializerList.h>
#include <Base/NumericLimits.h>
#include <Core/Containers/Array.h>
#include <Core/Hash/Sha256.h>
#include <Core/Math/MathUtility.h>
#include <Core/Math/RandomStream.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Move.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>
#include <TestSupport/Test.h>

using namespace rocks;
namespace audio_codec_tests_file {

using namespace rocks::audio;

// 코덱 계층 C1 시험이다(Docs/Design/AudioSystem 3.30.4절): 플랫폼 데이터 쓰기·검증·디코드,
// 청크 나누기·pre-roll, 디코더 표·계약.

namespace {

constexpr uint32 Rate = 48000;

// 시험 신호(톱니파): frame × step mod 주기를 [−0.5, 0.5)로 옮긴다. 채널마다 step이 다르다.
constexpr uint32 SawPeriod = 2000;
constexpr uint32 SawStepBase = 37;
constexpr uint32 SawStepPerChannel = 11;

// 시험 전용 codec: 단위 = TestUnitFrames frame 교차 int16이다. 디코더 출력은
// y[t] = x[t] + … + x[t − N + 1](N = TestMemoryFrames, stream 앞은 0)이라 상태 기억이 정확히
// N frame이다. pre-roll 요구도 N이다.
constexpr AudioCodecId TestCodec = static_cast<AudioCodecId>(7);
constexpr uint32 TestUnitFrames = 100;
constexpr uint32 TestMemoryFrames = 150;
constexpr uint32 TestBitstreamVersion = 1;
constexpr usize TestCodecHeaderBytes = 8;
constexpr float TestOutputScale = 1.0f / (32768.0f * TestMemoryFrames);

// 머리 칸 위치(형식 시험용, 3.30.1절 표)다.
constexpr usize CodecOffset = 6;
constexpr usize ChannelsOffset = 7;
constexpr usize RateOffset = 12;
constexpr usize FramesOffset = 16;
constexpr usize LoopStartOffset = 24;
constexpr usize LoopEndOffset = 32;
constexpr usize DelayOffset = 40;
constexpr usize PaddingOffset = 44;
constexpr usize ChunkCountOffset = 48;
constexpr usize CodecHeaderBytesOffset = 52;
constexpr usize ChannelMaskOffset = 56;
constexpr usize Reserved32Offset = 60;
constexpr usize Reserved64Offset = 72;
constexpr usize ChunkOffsetField = 0;
constexpr usize ChunkFlagsField = 12;
constexpr usize ChunkFirstFrameField = 16;
constexpr usize ChunkFrameCountField = 24;
constexpr usize ChunkPreRollField = 28;

// 시험 신호(채널마다 step이 다른 톱니파)를 담는 planar 배열이다.
class Signal {
public:
    Signal(Allocator& allocator, uint32 channels, uint32 frames) noexcept : samples_(allocator) {
        if (!samples_.TryResize(static_cast<usize>(channels) * frames)) {
            platform::Fatal("Audio codec test signal allocation failed");
        }

        view_.channelCount = channels;
        view_.frameCount = frames;
        for (uint32 channel = 0; channel < channels; ++channel) {
            float* data = samples_.Data() + static_cast<usize>(channel) * frames;
            view_.channels[channel] = data;
            // 정수로 만든 톱니파라 libm에 기대지 않는다(golden byte가 플랫폼마다 같다).
            const uint32 step = SawStepBase + SawStepPerChannel * channel;
            for (uint32 frame = 0; frame < frames; ++frame) {
                const int32 phase = static_cast<int32>(frame * step % SawPeriod) -
                                    static_cast<int32>(SawPeriod / 2);
                data[frame] = static_cast<float>(phase) / static_cast<float>(SawPeriod);
            }
        }
    }

    const PlanarConstView& View() const noexcept {
        return view_;
    }

private:
    Array<float> samples_;
    PlanarConstView view_;
};

// 시험 codec 인코드: x = QuantizePcm16(입력), 앞에 delay 무음, 뒤에 padding 무음을 붙여 단위로
// 나눈다.
void EncodeTestCodec(const PlanarConstView& pcm, uint32 delay, uint32 padding, EncodedAudio& out,
                     bool withCodecHeader = false) noexcept {
    out.Reset();
    if (withCodecHeader) {
        const uint8 codecHeader[TestCodecHeaderBytes] = {1, 2, 3, 4, 5, 6, 7, 8};
        if (!out.codecHeader.TryAppend(Span<const uint8>(codecHeader))) {
            platform::Fatal("Audio codec test codec header allocation failed");
        }
    }

    out.codec = TestCodec;
    out.bitstreamVersion = TestBitstreamVersion;
    out.encoderVersion = 1;
    out.channels = pcm.channelCount;
    out.channelMask = DefaultAudioChannelMask(pcm.channelCount);
    out.sampleRate = Rate;
    out.frames = pcm.frameCount;
    out.encoderDelay = delay;
    out.endPadding = padding;
    out.preRollFrames = TestMemoryFrames;
    const uint32 total = delay + pcm.frameCount + padding;
    for (uint32 frame = 0; frame < total; ++frame) {
        for (uint32 channel = 0; channel < pcm.channelCount; ++channel) {
            const bool valid = frame >= delay && frame < delay + pcm.frameCount;
            const int16 value =
                valid ? QuantizePcm16(pcm.channels[channel][frame - delay]) : int16{0};
            uint8 bytes[Pcm16SampleBytes];
            StoreLittleEndian<uint16>(bytes, static_cast<uint16>(value));
            if (!out.payload.TryAppend(Span<const uint8>(bytes))) {
                platform::Fatal("Audio codec test payload allocation failed");
            }
        }
    }

    const usize frameBytes = static_cast<usize>(pcm.channelCount) * Pcm16SampleBytes;
    for (uint32 first = 0; first < total; first += TestUnitFrames) {
        const uint32 frames = total - first < TestUnitFrames ? total - first : TestUnitFrames;
        if (!out.units.TryPushBack(EncodedAudioUnit{
                first * frameBytes, static_cast<uint32>(frames * frameBytes), frames})) {
            platform::Fatal("Audio codec test unit allocation failed");
        }
    }
}

// 시험 codec 디코더다.
class TestCodecDecoder final : public AudioDecoder {
public:
    Result<void> Open(const AudioPlatformDataHeader& header,
                      Span<const uint8> codecHeader) noexcept override {
        if (header.codec != TestCodec || header.bitstreamVersion != TestBitstreamVersion ||
            (codecHeader.Size() != 0 && codecHeader.Size() != TestCodecHeaderBytes)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        channels_ = header.channels;
        return Result<void>::Success();
    }

    Result<void> BeginChunk(const AudioChunkEntry& chunk, Span<const uint8> payload,
                            uint64 startCodecFrame) noexcept override {
        finished_ = false;
        const uint64 frameBytes = uint64{channels_} * Pcm16SampleBytes;
        if (payload.Size() != (uint64{chunk.preRollFrames} + chunk.frameCount) * frameBytes ||
            startCodecFrame < chunk.firstFrame ||
            startCodecFrame >= chunk.firstFrame + chunk.frameCount) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        payload_ = payload;
        chunkEnd_ = chunk.firstFrame + chunk.frameCount;
        payloadFirst_ = chunk.firstFrame - chunk.preRollFrames;
        for (uint32 channel = 0; channel < channels_; ++channel) {
            for (uint32 index = 0; index < TestMemoryFrames; ++index) {
                window_[channel][index] = 0;
            }
        }

        // pre-roll과 시작 앞 frame을 디코드해 버린다.
        position_ = payloadFirst_;
        while (position_ < startCodecFrame) {
            Step(nullptr, 0);
            ++position_;
        }

        return Result<void>::Success();
    }

    // 상태(window)를 이어 쓰고 복제 pre-roll은 건너뛴다. 그래서 전체 디코드는 청크 경계와 무관하게
    // 기준(연속 디코드)과 같다.
    Result<void> ContinueChunk(const AudioChunkEntry& chunk,
                               Span<const uint8> payload) noexcept override {
        const uint64 frameBytes = uint64{channels_} * Pcm16SampleBytes;
        if (!finished_ || chunk.firstFrame != chunkEnd_ ||
            payload.Size() != (uint64{chunk.preRollFrames} + chunk.frameCount) * frameBytes) {
            payload_ = Span<const uint8>();
            finished_ = false;
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        finished_ = false;
        payload_ = payload;
        chunkEnd_ = chunk.firstFrame + chunk.frameCount;
        payloadFirst_ = chunk.firstFrame - chunk.preRollFrames;
        return Result<void>::Success();
    }

    Result<uint32> DecodeNext(const PlanarView& out) noexcept override {
        if (payload_.Size() == 0 || out.channelCount != channels_ ||
            out.frameCount < TestUnitFrames) {
            return Result<uint32>::Failure(ErrorCode::InvalidArgument);
        }

        const uint64 unitEnd = (position_ / TestUnitFrames + 1) * TestUnitFrames;
        const uint64 end = unitEnd < chunkEnd_ ? unitEnd : chunkEnd_;
        uint32 count = 0;
        for (; position_ < end; ++position_, ++count) {
            Step(&out, count);
        }

        finished_ = count == 0;
        return Result<uint32>::Success(count);
    }

    uint32 MaxFramesPerCall() const noexcept override {
        return TestUnitFrames;
    }

private:
    void Step(const PlanarView* out, uint32 index) noexcept {
        const uint8* frame =
            payload_.Data() + (position_ - payloadFirst_) * channels_ * Pcm16SampleBytes;
        for (uint32 channel = 0; channel < channels_; ++channel) {
            const int16 value =
                static_cast<int16>(LoadLittleEndian<uint16>(frame + channel * Pcm16SampleBytes));
            window_[channel][position_ % TestMemoryFrames] = value;
            if (out != nullptr) {
                int64 sum = 0;
                for (uint32 tap = 0; tap < TestMemoryFrames; ++tap) {
                    sum += window_[channel][tap];
                }

                out->channels[channel][index] = static_cast<float>(sum) * TestOutputScale;
            }
        }
    }

    Span<const uint8> payload_;
    uint32 channels_ = 0;
    uint64 chunkEnd_ = 0;
    uint64 payloadFirst_ = 0;
    uint64 position_ = 0;
    int32 window_[MaxOutputChannels][TestMemoryFrames] = {};
    bool finished_ = false;
};

Result<UniquePtr<AudioDecoder>> CreateTestCodecDecoder(Allocator& allocator) noexcept {
    auto made = UniquePtr<TestCodecDecoder>::TryMake(allocator);
    if (!made) {
        return Result<UniquePtr<AudioDecoder>>::Failure(made.Error());
    }

    return Result<UniquePtr<AudioDecoder>>::Success(UniquePtr<AudioDecoder>(Move(made.Value())));
}

Result<UniquePtr<AudioDecoder>> FailingFactory(Allocator&) noexcept {
    return Result<UniquePtr<AudioDecoder>>::Failure(ErrorCode::OutOfMemory);
}

// 시험 codec의 기준(연속, 상태 기억 N frame) 출력이다. codec frame t의 값.
float TestReference(const PlanarConstView& pcm, uint32 delay, uint32 channel,
                    uint64 codecFrame) noexcept {
    int64 sum = 0;
    for (uint32 tap = 0; tap < TestMemoryFrames && tap <= codecFrame; ++tap) {
        const uint64 at = codecFrame - tap;
        if (at >= delay && at < delay + pcm.frameCount) {
            sum += QuantizePcm16(pcm.channels[channel][at - delay]);
        }
    }

    return static_cast<float>(sum) * TestOutputScale;
}

AudioDecoderTable MakeTable() noexcept {
    AudioDecoderTable table;
    if (!RegisterBuiltInAudioDecoders(table) ||
        !table.Register(TestCodec, &CreateTestCodecDecoder)) {
        platform::Fatal("Audio codec test table setup failed");
    }

    return table;
}

// 채널마다 [start, start + frames) 구간을 디코더로 읽어 out(채널 × frames)에 쓴다. 청크를 넘으면
// 다음 청크로 간다.
Result<void> ReadRange(const AudioPlatformDataView& view, const AudioDecoderTable& table,
                       Allocator& allocator, uint64 startCodecFrame, uint64 frames,
                       Array<float>& out) noexcept {
    auto created = table.Create(view.Header().codec, allocator);
    if (!created) {
        return Result<void>::Failure(created.Error());
    }

    AudioDecoder& decoder = *created.Value();
    if (auto opened = decoder.Open(view.Header(), view.CodecHeader()); !opened) {
        return opened;
    }

    const uint32 channels = view.Header().channels;
    const uint32 unitFrames = decoder.MaxFramesPerCall();
    Array<float> scratch(allocator);
    if (!scratch.TryResize(static_cast<usize>(unitFrames) * channels) ||
        !out.TryResize(static_cast<usize>(frames) * channels)) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    PlanarView unit;
    unit.channelCount = channels;
    unit.frameCount = unitFrames;
    for (uint32 channel = 0; channel < channels; ++channel) {
        unit.channels[channel] = scratch.Data() + static_cast<usize>(channel) * unitFrames;
    }

    uint64 position = startCodecFrame;
    uint64 written = 0;
    for (uint32 index = 0; index < view.Header().chunkCount && written < frames; ++index) {
        const AudioChunkEntry chunk = view.Chunk(index);
        if (position >= chunk.firstFrame + chunk.frameCount) {
            continue;
        }

        if (auto begun = decoder.BeginChunk(chunk, view.ChunkPayload(index), position); !begun) {
            return begun;
        }

        while (written < frames) {
            auto count = decoder.DecodeNext(unit);
            if (!count) {
                return Result<void>::Failure(count.Error());
            }

            if (count.Value() == 0) {
                break;
            }

            for (uint32 frame = 0; frame < count.Value() && written < frames; ++frame, ++written) {
                for (uint32 channel = 0; channel < channels; ++channel) {
                    out[static_cast<usize>(channel) * frames + written] =
                        unit.channels[channel][frame];
                }
            }

            position += count.Value();
        }
    }

    return written == frames ? Result<void>::Success()
                             : Result<void>::Failure(ErrorCode::InvalidArgument);
}

Array<uint8> WriteOrDie(Allocator& allocator, const EncodedAudio& encoded,
                        const AudioPlatformDataOptions& options) noexcept {
    Array<uint8> bytes(allocator);
    if (!WriteAudioPlatformData(encoded, options, bytes)) {
        platform::Fatal("Audio codec test write failed");
    }

    return bytes;
}

ErrorCode ParseError(Span<const uint8> bytes) noexcept {
    return AudioPlatformDataView::Parse(bytes).Error();
}

} // namespace

TEST(Audio_Codec_Pcm16RoundTrip) {
    SystemAllocator allocator;
    constexpr uint32 Frames = 5000;
    const Signal signal(allocator, 2, Frames);
    Pcm16AudioEncoder encoder;
    EncodedAudio encoded(allocator);
    REQUIRE(encoder.Encode(signal.View(), Rate, DefaultAudioChannelMask(2), {}, encoded));
    CHECK(encoded.units.Size() == 5 && encoded.units[4].frames == Frames - 4 * Pcm16UnitFrames);
    AudioPlatformDataOptions options;
    options.loopStart = 100;
    options.loopEnd = 4000;
    const Array<uint8> bytes = WriteOrDie(allocator, encoded, options);
    auto view = AudioPlatformDataView::Parse(bytes.View());
    REQUIRE(view);
    const AudioPlatformDataHeader& header = view.Value().Header();
    CHECK(header.codec == AudioCodecId::Pcm16 && header.channels == 2 && header.sampleRate == Rate);
    CHECK(header.frames == Frames && header.loopStart == 100 && header.loopEnd == 4000 &&
          header.chunkCount == 1);
    CHECK(header.channelMask == (AudioChannelFrontLeft | AudioChannelFrontRight));
    CHECK(view.Value().HasAllPayload());

    const AudioDecoderTable table = MakeTable();
    DecodedAudioPcm decoded(allocator);
    REQUIRE(DecodeAudioPlatformData(view.Value(), table, allocator, decoded));
    REQUIRE(decoded.FrameCount() == Frames && decoded.ChannelCount() == 2);
    bool exact = true;
    for (uint32 channel = 0; channel < 2; ++channel) {
        for (uint32 frame = 0; frame < Frames; ++frame) {
            const float expected =
                static_cast<float>(QuantizePcm16(signal.View().channels[channel][frame])) *
                Pcm16SampleScale;
            exact = exact && decoded.Channel(channel)[frame] == expected;
        }
    }

    CHECK(exact);

    // 같은 입력이면 같은 byte다.
    EncodedAudio again(allocator);
    REQUIRE(encoder.Encode(signal.View(), Rate, DefaultAudioChannelMask(2), {}, again));
    const Array<uint8> bytesAgain = WriteOrDie(allocator, again, options);
    CHECK(bytesAgain.Size() == bytes.Size() &&
          Sha256::Hash(bytes.View()) == Sha256::Hash(bytesAgain.View()));
}

TEST(Audio_Codec_AdpcmRoundTripAndGolden) {
    SystemAllocator allocator;
    constexpr uint32 Frames = 3000;
    const Signal signal(allocator, 2, Frames);
    AdpcmAudioEncoder encoder;
    EncodedAudio encoded(allocator);
    REQUIRE(encoder.Encode(signal.View(), Rate, DefaultAudioChannelMask(2), {}, encoded));
    CHECK(encoded.units.Size() == 3 && encoded.units[2].frames == Frames - 2 * AdpcmFramesPerBlock);
    const Array<uint8> bytes = WriteOrDie(allocator, encoded, {});
    auto view = AudioPlatformDataView::Parse(bytes.View());
    REQUIRE(view);
    const AudioDecoderTable table = MakeTable();
    DecodedAudioPcm decoded(allocator);
    REQUIRE(DecodeAudioPlatformData(view.Value(), table, allocator, decoded));

    // DecodeAdpcmFrames로 같은 payload를 바로 디코드한 결과와 같다.
    Array<float> reference(allocator);
    REQUIRE(reference.TryResize(2 * Frames));
    PlanarView referenceView;
    referenceView.channelCount = 2;
    referenceView.frameCount = Frames;
    referenceView.channels[0] = reference.Data();
    referenceView.channels[1] = reference.Data() + Frames;
    REQUIRE(DecodeAdpcmFrames(encoded.payload.View(), 2, Frames, 0, referenceView));
    bool same = decoded.FrameCount() == Frames;
    for (uint32 channel = 0; channel < 2 && same; ++channel) {
        for (uint32 frame = 0; frame < Frames; ++frame) {
            same =
                same && decoded.Channel(channel)[frame] == referenceView.channels[channel][frame];
        }
    }

    CHECK(same);

    // 형식 고정: 고정 입력의 전체 byte SHA-256(형식·인코더가 우연히 바뀌면 실패한다).
    char hex[Sha256Digest::HexBytes + 1];
    Sha256::Hash(bytes.View()).ToHex(hex);
    CHECK(StringView(hex) ==
          StringView("658c1e545aa1fcf71659d350dbc38080aab315f53cde765145a12bb5e4c7c297"));
}

TEST(Audio_Codec_ChunkingAndMidChunkStart) {
    SystemAllocator allocator;
    const AudioDecoderTable table = MakeTable();
    constexpr uint32 Frames = 7000;
    const Signal signal(allocator, 2, Frames);
    // PCM16 단위 byte = 1024 × 2 × 2 = 4096. 첫 청크 목표 1 단위, 그 밖 3 단위 + 1 byte.
    Pcm16AudioEncoder pcmEncoder;
    AdpcmAudioEncoder adpcmEncoder;
    for (AudioEncoder* encoder :
         {static_cast<AudioEncoder*>(&pcmEncoder), static_cast<AudioEncoder*>(&adpcmEncoder)}) {
        EncodedAudio encoded(allocator);
        REQUIRE(encoder->Encode(signal.View(), Rate, DefaultAudioChannelMask(2), {}, encoded));
        const uint32 unitBytes = encoded.units[0].payloadBytes;
        AudioPlatformDataOptions options;
        options.firstChunkBytes = unitBytes;
        options.targetChunkBytes = 3 * unitBytes + 1;
        const Array<uint8> bytes = WriteOrDie(allocator, encoded, options);
        auto parsed = AudioPlatformDataView::Parse(bytes.View());
        REQUIRE(parsed);
        const AudioPlatformDataView& view = parsed.Value();
        REQUIRE(view.Header().chunkCount >= 3);
        CHECK(view.Chunk(0).payloadBytes == unitBytes);
        uint64 nextFrame = 0;
        for (uint32 index = 0; index < view.Header().chunkCount; ++index) {
            const AudioChunkEntry chunk = view.Chunk(index);
            CHECK(chunk.firstFrame == nextFrame && chunk.payloadOffset % AudioChunkAlignment == 0);
            CHECK(chunk.preRollFrames == 0);
            CHECK(index + 1 == view.Header().chunkCount || chunk.payloadBytes % unitBytes == 0);
            CHECK(chunk.payloadBytes <=
                  (index == 0 ? options.firstChunkBytes : options.targetChunkBytes));
            nextFrame += chunk.frameCount;
            // 앞 청크 끝과 이 청크 사이 빈틈은 0이다.
            if (index > 0) {
                const AudioChunkEntry previous = view.Chunk(index - 1);
                for (uint64 at = previous.payloadOffset + previous.payloadBytes;
                     at < chunk.payloadOffset; ++at) {
                    CHECK(bytes[view.MetaBytes() + static_cast<usize>(at)] == 0);
                }
            }
        }

        DecodedAudioPcm full(allocator);
        REQUIRE(DecodeAudioPlatformData(view, table, allocator, full));
        // 처음부터 끝까지 청크를 이어 읽은 결과 = 전체 디코드(delay 0이라 codec frame = 유효
        // frame).
        {
            Array<float> all(allocator);
            REQUIRE(ReadRange(view, table, allocator, 0, Frames, all));
            bool same = true;
            for (uint32 channel = 0; channel < 2; ++channel) {
                for (uint64 frame = 0; frame < Frames; ++frame) {
                    same = same && all[static_cast<usize>(channel * Frames + frame)] ==
                                       full.Channel(channel)[frame];
                }
            }

            CHECK(same);
        }

        // 청크마다 가운데에서 시작해 청크 끝까지 읽은 결과 = 전체 디코드의 같은 구간.
        for (uint32 index = 0; index < view.Header().chunkCount; ++index) {
            const AudioChunkEntry chunk = view.Chunk(index);
            const uint64 start = chunk.firstFrame + chunk.frameCount / 2;
            const uint64 frames = chunk.firstFrame + chunk.frameCount - start;
            Array<float> range(allocator);
            REQUIRE(ReadRange(view, table, allocator, start, frames, range));
            bool same = true;
            for (uint32 channel = 0; channel < 2; ++channel) {
                for (uint64 frame = 0; frame < frames; ++frame) {
                    same = same && range[static_cast<usize>(channel * frames + frame)] ==
                                       full.Channel(channel)[start + frame];
                }
            }

            CHECK(same);
        }
        // 여러 시작 위치(청크 머리·가운데·단위 경계·끝 바로 앞)에서 읽은 구간 = 전체 디코드의 같은
        // 구간.
        for (const uint64 start : {uint64{0}, uint64{1}, uint64{1023}, uint64{1024}, uint64{2500},
                                   uint64{4096}, uint64{Frames - 10}}) {
            const uint64 frames = Frames - start < 2000 ? Frames - start : 2000;
            Array<float> range(allocator);
            REQUIRE(ReadRange(view, table, allocator, start, frames, range));
            bool same = true;
            for (uint32 channel = 0; channel < 2; ++channel) {
                for (uint64 frame = 0; frame < frames; ++frame) {
                    same = same && range[static_cast<usize>(channel * frames + frame)] ==
                                       full.Channel(channel)[start + frame];
                }
            }

            CHECK(same);
        }
    }

    // 단위 하나가 목표보다 크면 그 청크만 목표를 넘는다.
    EncodedAudio encoded(allocator);
    REQUIRE(pcmEncoder.Encode(signal.View(), Rate, DefaultAudioChannelMask(2), {}, encoded));
    AudioPlatformDataOptions tiny;
    tiny.targetChunkBytes = 16;
    const Array<uint8> bytes = WriteOrDie(allocator, encoded, tiny);
    auto view = AudioPlatformDataView::Parse(bytes.View());
    REQUIRE(view);
    CHECK(view.Value().Header().chunkCount == encoded.units.Size());
}

TEST(Audio_Codec_PreRollAcrossChunks) {
    SystemAllocator allocator;
    const AudioDecoderTable table = MakeTable();
    constexpr uint32 Frames = 1000;
    constexpr uint32 Delay = 37;
    constexpr uint32 Padding = 11;
    const Signal signal(allocator, 2, Frames);
    EncodedAudio encoded(allocator);
    EncodeTestCodec(signal.View(), Delay, Padding, encoded);
    // 청크 = 단위 하나(100 frame). pre-roll 150은 앞 청크 둘에 걸친다. 청크 1은 앞이 100
    // frame뿐이다.
    AudioPlatformDataOptions options;
    options.targetChunkBytes = 1;
    const Array<uint8> bytes = WriteOrDie(allocator, encoded, options);
    auto parsed = AudioPlatformDataView::Parse(bytes.View());
    REQUIRE(parsed);
    const AudioPlatformDataView& view = parsed.Value();
    CHECK(view.Chunk(0).preRollFrames == 0 && view.Chunk(1).preRollFrames == 100 &&
          view.Chunk(2).preRollFrames == 200);

    // 전체 디코드: delay·padding을 자르고 기준(연속 디코드)과 같다.
    DecodedAudioPcm full(allocator);
    REQUIRE(DecodeAudioPlatformData(view, table, allocator, full));
    REQUIRE(full.FrameCount() == Frames);
    bool same = true;
    for (uint32 channel = 0; channel < 2; ++channel) {
        for (uint32 frame = 0; frame < Frames; ++frame) {
            same = same && full.Channel(channel)[frame] ==
                               TestReference(signal.View(), Delay, channel, frame + Delay);
        }
    }

    CHECK(same);

    // 청크 가운데에서 시작해도 기준과 같다.
    for (const uint64 start : {uint64{5}, uint64{150}, uint64{333}, uint64{899}}) {
        Array<float> range(allocator);
        constexpr uint64 Length = 120;
        REQUIRE(ReadRange(view, table, allocator, start, Length, range));
        bool rangeSame = true;
        for (uint32 channel = 0; channel < 2; ++channel) {
            for (uint64 frame = 0; frame < Length; ++frame) {
                rangeSame =
                    rangeSame && range[static_cast<usize>(channel * Length + frame)] ==
                                     TestReference(signal.View(), Delay, channel, start + frame);
            }
        }

        CHECK(rangeSame);
    }

    // 복제 pre-roll byte도 SHA에 들어간다: 청크 2의 첫 byte(복제분)를 바꾸면 확인이 실패한다.
    Array<uint8> corrupted(allocator);
    REQUIRE(corrupted.TryAppend(bytes.View()));
    auto corruptedView = AudioPlatformDataView::Parse(corrupted.View());
    REQUIRE(corruptedView);
    const AudioChunkEntry chunk2 = corruptedView.Value().Chunk(2);
    corrupted[view.MetaBytes() + static_cast<usize>(chunk2.payloadOffset)] ^= 0x01;
    CHECK(VerifyAudioChunk(chunk2, corruptedView.Value().ChunkPayload(2)).Error() ==
          ErrorCode::CorruptData);
    DecodedAudioPcm broken(allocator);
    CHECK(DecodeAudioPlatformData(corruptedView.Value(), table, allocator, broken).Error() ==
          ErrorCode::CorruptData);
    CHECK(broken.FrameCount() == 0);
}

TEST(Audio_Codec_MetaPayloadSplit) {
    SystemAllocator allocator;
    const AudioDecoderTable table = MakeTable();
    const Signal signal(allocator, 1, 6000);
    Pcm16AudioEncoder encoder;
    EncodedAudio encoded(allocator);
    REQUIRE(encoder.Encode(signal.View(), Rate, DefaultAudioChannelMask(1), {}, encoded));
    AudioPlatformDataOptions options;
    options.targetChunkBytes = 4096;
    const Array<uint8> bytes = WriteOrDie(allocator, encoded, options);
    auto full = AudioPlatformDataView::Parse(bytes.View());
    REQUIRE(full);
    const usize meta = full.Value().MetaBytes();
    CHECK(meta % AudioChunkAlignment == 0);
    const AudioChunkEntry chunk0 = full.Value().Chunk(0);

    // 메타만(Server): 통과, payload 없음, 전체 디코드 거부.
    auto metaOnly = AudioPlatformDataView::Parse(bytes.View().Left(meta));
    REQUIRE(metaOnly);
    CHECK(metaOnly.Value().ChunkPayload(0).Size() == 0 && !metaOnly.Value().HasAllPayload());
    DecodedAudioPcm decoded(allocator);
    CHECK(DecodeAudioPlatformData(metaOnly.Value(), table, allocator, decoded).Error() ==
          ErrorCode::InvalidArgument);

    // 메타 + 청크 0(패키지): 청크 0만 보인다.
    auto package = AudioPlatformDataView::Parse(
        bytes.View().Left(meta + static_cast<usize>(chunk0.payloadBytes)));
    REQUIRE(package);
    CHECK(package.Value().ChunkPayload(0).Size() == chunk0.payloadBytes &&
          package.Value().ChunkPayload(1).Size() == 0);
    CHECK(VerifyAudioChunk(chunk0, package.Value().ChunkPayload(0)));

    // payload 영역보다 긴 bytes는 거부한다.
    Array<uint8> longer(allocator);
    REQUIRE(longer.TryAppend(bytes.View()));
    REQUIRE(longer.TryPushBack(0));
    CHECK(ParseError(longer.View()) == ErrorCode::CorruptData);
}

TEST(Audio_Codec_ParseRejects) {
    SystemAllocator allocator;
    const Signal signal(allocator, 2, 3000);
    Pcm16AudioEncoder encoder;
    EncodedAudio encoded(allocator);
    REQUIRE(encoder.Encode(signal.View(), Rate, DefaultAudioChannelMask(2), {}, encoded));
    AudioPlatformDataOptions options;
    options.targetChunkBytes = 4096;
    const Array<uint8> valid = WriteOrDie(allocator, encoded, options);
    REQUIRE(AudioPlatformDataView::Parse(valid.View()));
    const usize table = AudioPlatformDataHeaderBytes;

    // 한 칸씩 바꾼 사본은 모두 거부된다.
    struct Change {
        usize offset;
        uint64 value;
        uint32 bytes;
        ErrorCode expected;
    };

    const Change changes[] = {
        {0, 'X', 1, ErrorCode::CorruptData},                      // magic
        {4, 2, 2, ErrorCode::Unsupported},                        // 형식 판(새 판 — 3.38절)
        {CodecOffset, MaxAudioCodecs, 1, ErrorCode::Unsupported}, // 모르는 codec
        {CodecOffset, 0, 1, ErrorCode::CorruptData},              // codec None
        {ChannelsOffset, 0, 1, ErrorCode::CorruptData},           // 채널 0
        {ChannelsOffset, 9, 1, ErrorCode::CorruptData},           // 채널 9
        {ChannelMaskOffset, AudioChannelFrontCenter, 4, ErrorCode::CorruptData}, // bit 수 불일치
        {ChannelMaskOffset, 0x203, 4, ErrorCode::CorruptData},                   // bit 9
        {RateOffset, 4000, 4, ErrorCode::CorruptData},                           // rate 범위 밖
        {FramesOffset, 0, 8, ErrorCode::CorruptData},                            // 유효 frame 0
        {FramesOffset, 2999, 8, ErrorCode::CorruptData},                         // frame 합 불일치
        {DelayOffset, 1, 4, ErrorCode::CorruptData},                             // delay 합 불일치
        {LoopStartOffset, 5, 8, ErrorCode::CorruptData},              // 끝 0인데 시작 ≠ 0
        {LoopEndOffset, 3001, 8, ErrorCode::CorruptData},             // 끝 > 유효
        {ChunkCountOffset, 0, 4, ErrorCode::CorruptData},             // 청크 0개
        {CodecHeaderBytesOffset, 4, 4, ErrorCode::CorruptData},       // 8 배수 아님
        {Reserved32Offset, 1, 4, ErrorCode::CorruptData},             // 0 칸
        {Reserved64Offset, 1, 8, ErrorCode::CorruptData},             // 0 칸
        {table + ChunkFlagsField, 0, 4, ErrorCode::CorruptData},      // flags
        {table + ChunkOffsetField, 16, 8, ErrorCode::CorruptData},    // 청크 0 위치 ≠ 0
        {table + ChunkPreRollField, 1, 4, ErrorCode::CorruptData},    // 청크 0 pre-roll
        {table + ChunkFrameCountField, 0, 4, ErrorCode::CorruptData}, // frame 0
        {table + AudioChunkEntryBytes + ChunkFirstFrameField, 5, 8,
         ErrorCode::CorruptData}, // frame 빈틈
        {table + AudioChunkEntryBytes + ChunkOffsetField, 4104, 8,
         ErrorCode::CorruptData}, // 정렬만 위반
        {table + AudioChunkEntryBytes + ChunkOffsetField, 4080, 8,
         ErrorCode::CorruptData},                      // 16 배수인데 앞 청크 끝보다 작음
        {PaddingOffset, 1, 4, ErrorCode::CorruptData}, // padding 합 불일치
        {table + AudioChunkEntryBytes + ChunkPreRollField, 5000, 4,
         ErrorCode::CorruptData}, // pre-roll > 첫 frame
        {table + AudioChunkEntryBytes + ChunkOffsetField, 1u << 30, 8,
         ErrorCode::CorruptData}, // 범위 밖
    };

    for (const Change& change : changes) {
        Array<uint8> copy(allocator);
        REQUIRE(copy.TryAppend(valid.View()));
        for (uint32 index = 0; index < change.bytes; ++index) {
            copy[change.offset + index] = static_cast<uint8>(change.value >> (index * 8));
        }

        CHECK(ParseError(copy.View()) == change.expected);
    }

    // loop 시작 ≥ 끝: 시작 = 끝 = 10.
    {
        Array<uint8> copy(allocator);
        REQUIRE(copy.TryAppend(valid.View()));
        StoreLittleEndian<uint64>(copy.Data() + LoopStartOffset, 10);
        StoreLittleEndian<uint64>(copy.Data() + LoopEndOffset, 10);
        CHECK(ParseError(copy.View()) == ErrorCode::CorruptData);
    }

    // 청크 수 최대값(메타 크기 넘침)과 머리보다 짧은 입력.
    {
        Array<uint8> copy(allocator);
        REQUIRE(copy.TryAppend(valid.View()));
        StoreLittleEndian<uint32>(copy.Data() + ChunkCountOffset, 0xFFFFFFFFu);
        CHECK(ParseError(copy.View()) == ErrorCode::CorruptData);
    }

    CHECK(ParseError(valid.View().Left(AudioPlatformDataHeaderBytes - 1)) ==
          ErrorCode::CorruptData);
    CHECK(ParseError(valid.View().Left(AudioPlatformDataHeaderBytes + 10)) ==
          ErrorCode::CorruptData);
}

TEST(Audio_Codec_DecoderContract) {
    SystemAllocator allocator;
    const AudioDecoderTable table = MakeTable();
    const Signal signal(allocator, 2, 3000);
    for (const AudioCodecId codec : {AudioCodecId::Pcm16, AudioCodecId::Adpcm}) {
        Pcm16AudioEncoder pcmEncoder;
        AdpcmAudioEncoder adpcmEncoder;
        AudioEncoder& encoder = codec == AudioCodecId::Pcm16
                                    ? static_cast<AudioEncoder&>(pcmEncoder)
                                    : static_cast<AudioEncoder&>(adpcmEncoder);
        EncodedAudio encoded(allocator);
        REQUIRE(encoder.Encode(signal.View(), Rate, DefaultAudioChannelMask(2), {}, encoded));
        const Array<uint8> bytes = WriteOrDie(allocator, encoded, {});
        auto view = AudioPlatformDataView::Parse(bytes.View());
        REQUIRE(view);
        auto created = table.Create(codec, allocator);
        REQUIRE(created);
        AudioDecoder& decoder = *created.Value();
        const uint32 unitFrames = decoder.MaxFramesPerCall();
        Array<float> scratch(allocator);
        REQUIRE(scratch.TryResize(static_cast<usize>(unitFrames) * 2));
        PlanarView out;
        out.channelCount = 2;
        out.frameCount = unitFrames;
        out.channels[0] = scratch.Data();
        out.channels[1] = scratch.Data() + unitFrames;

        // Open 전·BeginChunk 전 호출.
        CHECK(decoder.DecodeNext(out).Error() == ErrorCode::InvalidArgument);
        const AudioChunkEntry chunk = view.Value().Chunk(0);
        const Span<const uint8> payload = view.Value().ChunkPayload(0);
        CHECK(decoder.BeginChunk(chunk, payload, 0).Error() == ErrorCode::InvalidArgument);
        REQUIRE(decoder.Open(view.Value().Header(), view.Value().CodecHeader()));
        CHECK(decoder.DecodeNext(out).Error() == ErrorCode::InvalidArgument);

        // 시작 위치 범위 밖, payload 크기 불일치, pre-roll ≠ 0.
        CHECK(decoder.BeginChunk(chunk, payload, chunk.frameCount).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(decoder.BeginChunk(chunk, payload.Left(payload.Size() - 1), 0).Error() ==
              ErrorCode::InvalidArgument);
        AudioChunkEntry withPreRoll = chunk;
        withPreRoll.preRollFrames = 1;
        CHECK(decoder.BeginChunk(withPreRoll, payload, 0).Error() == ErrorCode::CorruptData);

        // out 채널 수 불일치·용량 부족, 청크 끝 뒤 0.
        REQUIRE(decoder.BeginChunk(chunk, payload, 0));
        PlanarView wrongChannels = out;
        wrongChannels.channelCount = 1;
        CHECK(decoder.DecodeNext(wrongChannels).Error() == ErrorCode::InvalidArgument);
        PlanarView small = out;
        small.frameCount = unitFrames - 1;
        CHECK(decoder.DecodeNext(small).Error() == ErrorCode::InvalidArgument);
        uint64 total = 0;
        while (true) {
            auto written = decoder.DecodeNext(out);
            REQUIRE(written);
            if (written.Value() == 0) {
                break;
            }

            total += written.Value();
        }

        CHECK(total == chunk.frameCount);
        CHECK(decoder.DecodeNext(out) && decoder.DecodeNext(out).Value() == 0);

        // ContinueChunk 순서 규칙은 상태 없는 codec도 같다(3.30.2절): 청크 없이·다 내지
        // 않고·이어지지 않으면 InvalidArgument이고 열린 청크를 닫는다. 다 낸 뒤 바로 다음 청크는
        // 연다.
        {
            AudioPlatformDataOptions chunked;
            chunked.targetChunkBytes = 1;
            const Array<uint8> chunkedBytes = WriteOrDie(allocator, encoded, chunked);
            auto chunkedView = AudioPlatformDataView::Parse(chunkedBytes.View());
            REQUIRE(chunkedView);
            const AudioPlatformDataView& many = chunkedView.Value();
            REQUIRE(many.Header().chunkCount >= 3);
            const auto drain = [&]() noexcept {
                while (true) {
                    auto count = decoder.DecodeNext(out);
                    if (!count || count.Value() == 0) {
                        return static_cast<bool>(count);
                    }
                }
            };

            REQUIRE(decoder.Open(many.Header(), many.CodecHeader()));
            CHECK(decoder.ContinueChunk(many.Chunk(1), many.ChunkPayload(1)).Error() ==
                  ErrorCode::InvalidArgument);
            REQUIRE(decoder.BeginChunk(many.Chunk(0), many.ChunkPayload(0), 0));
            REQUIRE(decoder.DecodeNext(out));
            CHECK(decoder.ContinueChunk(many.Chunk(1), many.ChunkPayload(1)).Error() ==
                  ErrorCode::InvalidArgument);
            CHECK(decoder.DecodeNext(out).Error() == ErrorCode::InvalidArgument);
            REQUIRE(decoder.BeginChunk(many.Chunk(0), many.ChunkPayload(0), 0));
            REQUIRE(drain());
            CHECK(decoder.ContinueChunk(many.Chunk(2), many.ChunkPayload(2)).Error() ==
                  ErrorCode::InvalidArgument);
            REQUIRE(decoder.BeginChunk(many.Chunk(0), many.ChunkPayload(0), 0));
            REQUIRE(drain());
            CHECK(decoder.ContinueChunk(many.Chunk(1), many.ChunkPayload(1)));
            CHECK(drain());
        }

        // codec 머리가 있으면 Open이 거부한다.
        const uint8 extra[8] = {};
        CHECK(decoder.Open(view.Value().Header(), Span<const uint8>(extra)).Error() ==
              ErrorCode::CorruptData);
    }
}

TEST(Audio_Codec_DecoderTable) {
    SystemAllocator allocator;
    AudioDecoderTable table;
    CHECK(table.Create(AudioCodecId::Pcm16, allocator).Error() == ErrorCode::NotFound);
    REQUIRE(RegisterBuiltInAudioDecoders(table));
    CHECK(table.Contains(AudioCodecId::Pcm16) && table.Contains(AudioCodecId::Adpcm) &&
          !table.Contains(AudioCodecId::Opus));
    CHECK(RegisterBuiltInAudioDecoders(table).Error() == ErrorCode::AlreadyExists);
    CHECK(table.Register(AudioCodecId::None, &CreateTestCodecDecoder).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(table.Register(static_cast<AudioCodecId>(MaxAudioCodecs), &CreateTestCodecDecoder)
              .Error() == ErrorCode::InvalidArgument);
    CHECK(table.Register(TestCodec, nullptr).Error() == ErrorCode::InvalidArgument);
    CHECK(table.Create(AudioCodecId::Opus, allocator).Error() == ErrorCode::NotFound);

    // factory 실패는 그대로 전달된다. 디코더가 없는 codec은 전체 디코드가 NotFound다.
    REQUIRE(table.Register(AudioCodecId::Opus, &FailingFactory));
    CHECK(table.Create(AudioCodecId::Opus, allocator).Error() == ErrorCode::OutOfMemory);
    const Signal signal(allocator, 1, 500);
    EncodedAudio encoded(allocator);
    EncodeTestCodec(signal.View(), 0, 0, encoded);
    const Array<uint8> bytes = WriteOrDie(allocator, encoded, {});
    auto view = AudioPlatformDataView::Parse(bytes.View());
    REQUIRE(view);
    DecodedAudioPcm decoded(allocator);
    CHECK(DecodeAudioPlatformData(view.Value(), table, allocator, decoded).Error() ==
          ErrorCode::NotFound);
}

// 내장 디코더의 오류 분류(3.38절): 호출자 오용(다른 codec 머리, 열기 전, 청크와 다른 payload 길이,
// 청크 밖 시작 frame)은 InvalidArgument, 읽은 데이터의 규칙 위반(채널·rate, codec 머리가 비지 않음,
// pre-roll, frame 수와 payload byte 수 불일치)은 CorruptData다.
TEST(Audio_Codec_BuiltInDecoderErrorClasses) {
    SystemAllocator allocator;
    const AudioDecoderTable table = MakeTable();
    const Signal signal(allocator, 2, 3000);
    Pcm16AudioEncoder pcm;
    AdpcmAudioEncoder adpcm;
    for (AudioEncoder* encoder :
         {static_cast<AudioEncoder*>(&pcm), static_cast<AudioEncoder*>(&adpcm)}) {
        EncodedAudio encoded(allocator);
        REQUIRE(encoder->Encode(signal.View(), Rate, DefaultAudioChannelMask(2), {}, encoded));
        const Array<uint8> bytes = WriteOrDie(allocator, encoded, {});
        auto view = AudioPlatformDataView::Parse(bytes.View());
        REQUIRE(view);
        const AudioPlatformDataHeader& valid = view.Value().Header();
        auto created = table.Create(valid.codec, allocator);
        REQUIRE(created);
        AudioDecoder& decoder = *created.Value();
        const AudioChunkEntry chunk = view.Value().Chunk(0);
        const Span<const uint8> payload = view.Value().ChunkPayload(0);

        // 열기 전 BeginChunk는 오용이다.
        CHECK(decoder.BeginChunk(chunk, payload, chunk.firstFrame).Error() ==
              ErrorCode::InvalidArgument);

        AudioPlatformDataHeader other = valid;
        other.codec =
            valid.codec == AudioCodecId::Pcm16 ? AudioCodecId::Adpcm : AudioCodecId::Pcm16;
        CHECK(decoder.Open(other, {}).Error() == ErrorCode::InvalidArgument);
        AudioPlatformDataHeader noChannels = valid;
        noChannels.channels = 0;
        CHECK(decoder.Open(noChannels, {}).Error() == ErrorCode::CorruptData);
        AudioPlatformDataHeader lowRate = valid;
        lowRate.sampleRate = MinPlatformSampleRate - 1;
        CHECK(decoder.Open(lowRate, {}).Error() == ErrorCode::CorruptData);
        const uint8 extra[8] = {};
        CHECK(decoder.Open(valid, Span<const uint8>(extra)).Error() == ErrorCode::CorruptData);

        REQUIRE(decoder.Open(valid, view.Value().CodecHeader()));
        CHECK(
            decoder.BeginChunk(chunk, payload.Left(payload.Size() - 1), chunk.firstFrame).Error() ==
            ErrorCode::InvalidArgument);
        CHECK(decoder.BeginChunk(chunk, payload, chunk.firstFrame + chunk.frameCount).Error() ==
              ErrorCode::InvalidArgument);
        AudioChunkEntry preRoll = chunk;
        preRoll.preRollFrames = 1;
        CHECK(decoder.BeginChunk(preRoll, payload, preRoll.firstFrame).Error() ==
              ErrorCode::CorruptData);
        // payload 길이는 표와 같지만 frame 수가 byte 수와 맞지 않는다(블록 둘만큼 더 많다).
        AudioChunkEntry longer = chunk;
        longer.frameCount = chunk.frameCount + AdpcmFramesPerBlock * 2;
        CHECK(decoder.BeginChunk(longer, payload, longer.firstFrame).Error() ==
              ErrorCode::CorruptData);
        CHECK(decoder.BeginChunk(chunk, payload, chunk.firstFrame));
    }
}

// 디코더 등록부(Docs/Design/AudioSystem 3.37.1절): 넣기·지우기·범위 밖 무시, Fill은 표에 없는
// codec만 넣는다(내장 디코더가 앞선다).
TEST(Audio_Codec_DecoderRegistry) {
    SystemAllocator allocator;
    AudioDecoderRegistry registry;
    for (uint32 index = 0; index < MaxAudioCodecs; ++index) {
        CHECK(registry.Factory(static_cast<AudioCodecId>(index)) == nullptr);
    }

    // None·범위 밖은 무시한다(조회도 nullptr).
    registry.SetFactory(AudioCodecId::None, &CreateTestCodecDecoder);
    registry.SetFactory(static_cast<AudioCodecId>(MaxAudioCodecs), &CreateTestCodecDecoder);
    CHECK(registry.Factory(AudioCodecId::None) == nullptr);
    CHECK(registry.Factory(static_cast<AudioCodecId>(MaxAudioCodecs)) == nullptr);

    registry.SetFactory(AudioCodecId::Opus, &FailingFactory);
    registry.SetFactory(AudioCodecId::Pcm16, &FailingFactory);
    registry.SetFactory(TestCodec, &CreateTestCodecDecoder);
    CHECK(registry.Factory(AudioCodecId::Opus) == &FailingFactory);

    // 내장 디코더가 먼저 있으면 Pcm16은 그대로이고, 표에 없던 Opus·시험 codec만 들어간다.
    AudioDecoderTable table;
    REQUIRE(RegisterBuiltInAudioDecoders(table));
    registry.Fill(table);
    CHECK(table.Contains(AudioCodecId::Opus) && table.Contains(TestCodec));
    auto pcm = table.Create(AudioCodecId::Pcm16, allocator);
    CHECK(pcm.IsOk());
    CHECK(table.Create(AudioCodecId::Opus, allocator).Error() == ErrorCode::OutOfMemory);
    // 다시 Fill해도 바뀌지 않는다(이미 있는 codec은 둔다).
    registry.Fill(table);
    CHECK(table.Create(AudioCodecId::Opus, allocator).Error() == ErrorCode::OutOfMemory);

    // 지우면 새 표에는 들어가지 않는다.
    registry.SetFactory(AudioCodecId::Opus, nullptr);
    CHECK(registry.Factory(AudioCodecId::Opus) == nullptr);
    AudioDecoderTable fresh;
    registry.Fill(fresh);
    CHECK(!fresh.Contains(AudioCodecId::Opus) && fresh.Contains(AudioCodecId::Pcm16) &&
          fresh.Contains(TestCodec));
}

TEST(Audio_Codec_EncoderRejects) {
    SystemAllocator allocator;
    Pcm16AudioEncoder pcm;
    AdpcmAudioEncoder adpcm;
    Signal signal(allocator, 2, 100);
    for (AudioEncoder* encoder :
         {static_cast<AudioEncoder*>(&pcm), static_cast<AudioEncoder*>(&adpcm)}) {
        EncodedAudio out(allocator);
        // 채널 마스크 bit 수 불일치, rate 범위 밖, 채널 0, frame 0.
        CHECK(encoder->Encode(signal.View(), Rate, AudioChannelFrontCenter, {}, out).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(encoder->Encode(signal.View(), 7999, DefaultAudioChannelMask(2), {}, out).Error() ==
              ErrorCode::InvalidArgument);
        PlanarConstView empty = signal.View();
        empty.channelCount = 0;
        CHECK(encoder->Encode(empty, Rate, 0, {}, out).Error() == ErrorCode::InvalidArgument);
        PlanarConstView noFrames = signal.View();
        noFrames.frameCount = 0;
        CHECK(encoder->Encode(noFrames, Rate, DefaultAudioChannelMask(2), {}, out).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(out.units.Size() == 0 && out.codec == AudioCodecId::None);
    }

    // 비유한 표본.
    Array<float> data(allocator);
    REQUIRE(data.TryResize(10));
    data[3] = platform::BitCast<float>(0x7FC00000u);
    PlanarConstView nan;
    nan.channelCount = 1;
    nan.frameCount = 10;
    nan.channels[0] = data.Data();
    EncodedAudio out(allocator);
    CHECK(pcm.Encode(nan, Rate, DefaultAudioChannelMask(1), {}, out).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(adpcm.Encode(nan, Rate, DefaultAudioChannelMask(1), {}, out).Error() ==
          ErrorCode::InvalidArgument);
}

TEST(Audio_Codec_CorruptedInputs) {
    // 올바른 blob의 byte를 무작위로 바꾸거나 자른다: 검증·디코드 어디서든 멈추거나 범위 밖을 읽지
    // 않는다.
    SystemAllocator allocator;
    const AudioDecoderTable table = MakeTable();
    const Signal signal(allocator, 2, 2500);
    AdpcmAudioEncoder encoder;
    EncodedAudio encoded(allocator);
    REQUIRE(encoder.Encode(signal.View(), Rate, DefaultAudioChannelMask(2), {}, encoded));
    AudioPlatformDataOptions options;
    options.targetChunkBytes = 1024;
    const Array<uint8> valid = WriteOrDie(allocator, encoded, options);
    RandomStream random(0x52415044);
    constexpr uint32 Rounds = 300;
    uint32 parsed = 0;
    for (uint32 round = 0; round < Rounds; ++round) {
        Array<uint8> copy(allocator);
        REQUIRE(copy.TryAppend(valid.View()));
        const usize at = random.NextUInt32Below(static_cast<uint32>(copy.Size()));
        copy[at] = static_cast<uint8>(random.NextUInt32());
        const usize size =
            round % 3 == 0 ? random.NextUInt32Below(static_cast<uint32>(copy.Size())) : copy.Size();
        auto view = AudioPlatformDataView::Parse(copy.View().Left(size));
        if (!view) {
            continue;
        }

        ++parsed;
        DecodedAudioPcm decoded(allocator);
        const Result<void> result =
            DecodeAudioPlatformData(view.Value(), table, allocator, decoded);
        CHECK(result || decoded.FrameCount() == 0);
    }

    CHECK(parsed > 0);
}

TEST(Audio_Codec_DecoderReopenAndBounds) {
    SystemAllocator allocator;
    const AudioDecoderTable table = MakeTable();
    const Signal signal(allocator, 2, 5000);
    Pcm16AudioEncoder encoder;
    EncodedAudio encoded(allocator);
    REQUIRE(encoder.Encode(signal.View(), Rate, DefaultAudioChannelMask(2), {}, encoded));
    AudioPlatformDataOptions options;
    options.targetChunkBytes = 4096;
    const Array<uint8> bytes = WriteOrDie(allocator, encoded, options);
    auto view = AudioPlatformDataView::Parse(bytes.View());
    REQUIRE(view);
    auto created = table.Create(AudioCodecId::Pcm16, allocator);
    REQUIRE(created);
    AudioDecoder& decoder = *created.Value();
    REQUIRE(decoder.Open(view.Value().Header(), view.Value().CodecHeader()));

    // 청크 1 이상에서 startCodecFrame < 첫 frame은 거부한다.
    const AudioChunkEntry chunk1 = view.Value().Chunk(1);
    CHECK(decoder.BeginChunk(chunk1, view.Value().ChunkPayload(1), chunk1.firstFrame - 1).Error() ==
          ErrorCode::InvalidArgument);

    // 다시 Open이 실패하면(bitstream 판 2 — Unsupported) 앞 청크 상태가 남지 않는다.
    REQUIRE(decoder.BeginChunk(chunk1, view.Value().ChunkPayload(1), chunk1.firstFrame));
    AudioPlatformDataHeader newer = view.Value().Header();
    newer.bitstreamVersion = Pcm16BitstreamVersion + 1;
    CHECK(decoder.Open(newer, Span<const uint8>()).Error() == ErrorCode::Unsupported);
    // 새 판이 codec 머리를 두더라도 손상(CorruptData)이 아니라 모르는 판이다.
    const uint8 newerCodecHeader[AudioCodecHeaderAlignment] = {};
    CHECK(decoder.Open(newer, Span<const uint8>(newerCodecHeader, AudioCodecHeaderAlignment))
              .Error() == ErrorCode::Unsupported);
    Array<float> scratch(allocator);
    REQUIRE(scratch.TryResize(2 * Pcm16UnitFrames));
    PlanarView out;
    out.channelCount = 2;
    out.frameCount = Pcm16UnitFrames;
    out.channels[0] = scratch.Data();
    out.channels[1] = scratch.Data() + Pcm16UnitFrames;
    CHECK(decoder.DecodeNext(out).Error() == ErrorCode::InvalidArgument);
    CHECK(decoder.BeginChunk(chunk1, view.Value().ChunkPayload(1), chunk1.firstFrame).Error() ==
          ErrorCode::InvalidArgument);

    // 검증하지 않은 머리(채널 9)로 Open하면 거부한다.
    AudioPlatformDataHeader nine = view.Value().Header();
    nine.channels = 9;
    CHECK(decoder.Open(nine, Span<const uint8>()).Error() == ErrorCode::CorruptData);

    // 다시 정상 Open하면 처음부터 쓸 수 있다.
    REQUIRE(decoder.Open(view.Value().Header(), view.Value().CodecHeader()));
    REQUIRE(decoder.BeginChunk(chunk1, view.Value().ChunkPayload(1), chunk1.firstFrame));
    CHECK(decoder.DecodeNext(out) && true);
}

TEST(Audio_Codec_MetaPaddingAndOverflow) {
    SystemAllocator allocator;
    const Signal signal(allocator, 1, 700);
    // codec 머리 8 byte면 메타 = 80 + 8 + 64 × 청크 수라 16 정렬 채움 8 byte가 생긴다.
    EncodedAudio encoded(allocator);
    EncodeTestCodec(signal.View(), 0, 0, encoded, true);
    const Array<uint8> bytes = WriteOrDie(allocator, encoded, {});
    auto view = AudioPlatformDataView::Parse(bytes.View());
    REQUIRE(view);
    const usize raw = AudioPlatformDataHeaderBytes + TestCodecHeaderBytes + AudioChunkEntryBytes;
    REQUIRE(view.Value().MetaBytes() > raw);
    CHECK(view.Value().CodecHeader().Size() == TestCodecHeaderBytes &&
          view.Value().CodecHeader()[0] == 1);
    Array<uint8> padded(allocator);
    REQUIRE(padded.TryAppend(bytes.View()));
    padded[raw] = 1;
    CHECK(ParseError(padded.View()) == ErrorCode::CorruptData);

    // delay + 유효 + padding 넘침.
    Array<uint8> overflow(allocator);
    REQUIRE(overflow.TryAppend(bytes.View()));
    StoreLittleEndian<uint64>(overflow.Data() + FramesOffset, NumericLimits<uint64>::Max() - 1);
    StoreLittleEndian<uint32>(overflow.Data() + DelayOffset, 5);
    CHECK(ParseError(overflow.View()) == ErrorCode::CorruptData);
}

TEST(Audio_Codec_HugeFrameCountRejectedBeforeAllocation) {
    // 청크 하나의 frame 수가 0xFFFFFFFF인데 payload는 16 byte인 blob(SHA는 다시 맞춤): 검증은
    // 통과하지만 전체 디코드는 할당 전에 BeginChunk 검사로 거부한다(채널 × 16 GiB를 잡지 않는다).
    SystemAllocator allocator;
    constexpr uint32 PayloadBytes = 16;
    const usize meta = AudioPlatformDataMetaBytes(0, 1);
    Array<uint8> bytes(allocator);
    REQUIRE(bytes.TryResize(meta + PayloadBytes));
    AudioPlatformDataHeader header;
    header.codec = AudioCodecId::Pcm16;
    header.channels = 2;
    header.bitstreamVersion = Pcm16BitstreamVersion;
    header.sampleRate = Rate;
    header.frames = 0xFFFFFFFFu;
    header.chunkCount = 1;
    header.channelMask = DefaultAudioChannelMask(2);
    header.payloadBytes = PayloadBytes;
    StoreAudioPlatformDataHeader(header, bytes.Data());
    AudioChunkEntry chunk;
    chunk.payloadBytes = PayloadBytes;
    chunk.flags = AudioChunkFlagIndependent;
    chunk.frameCount = 0xFFFFFFFFu;
    const Sha256Digest digest = Sha256::Hash(Span<const uint8>(bytes.Data() + meta, PayloadBytes));
    platform::MemoryCopy(chunk.sha256, digest.bytes, AudioChunkHashBytes);
    StoreAudioChunkEntry(chunk, bytes.Data() + AudioPlatformDataHeaderBytes);
    auto view = AudioPlatformDataView::Parse(bytes.View());
    REQUIRE(view);
    const AudioDecoderTable table = MakeTable();
    DecodedAudioPcm decoded(allocator);
    CHECK(DecodeAudioPlatformData(view.Value(), table, allocator, decoded).Error() ==
          ErrorCode::CorruptData);
}

TEST(Audio_Codec_CorruptedPayloadsReachDecoder) {
    // payload byte를 바꿔 디코더에 바로 준다(SHA를 거치지 않음 — 청크 캐시가 SHA를 맞춘 손상
    // payload를 넘긴 경우와 같다). PCM16·ADPCM·시험 codec 모두 멈추거나 범위 밖을 읽지 않고, 오류
    // 또는 청크 frame 수만큼 낸다.
    SystemAllocator allocator;
    const AudioDecoderTable table = MakeTable();
    const Signal signal(allocator, 2, 3000);
    Pcm16AudioEncoder pcm;
    AdpcmAudioEncoder adpcm;
    RandomStream random(0x434F5252);
    constexpr uint32 Rounds = 200;
    for (uint32 kind = 0; kind < 3; ++kind) {
        EncodedAudio encoded(allocator);
        if (kind == 0) {
            REQUIRE(pcm.Encode(signal.View(), Rate, DefaultAudioChannelMask(2), {}, encoded));
        } else if (kind == 1) {
            REQUIRE(adpcm.Encode(signal.View(), Rate, DefaultAudioChannelMask(2), {}, encoded));
        } else {
            EncodeTestCodec(signal.View(), 7, 3, encoded);
        }

        AudioPlatformDataOptions options;
        options.targetChunkBytes = 1500;
        const Array<uint8> bytes = WriteOrDie(allocator, encoded, options);
        auto view = AudioPlatformDataView::Parse(bytes.View());
        REQUIRE(view);
        auto created = table.Create(view.Value().Header().codec, allocator);
        REQUIRE(created);
        AudioDecoder& decoder = *created.Value();
        REQUIRE(decoder.Open(view.Value().Header(), view.Value().CodecHeader()));
        Array<float> scratch(allocator);
        const uint32 unitFrames = decoder.MaxFramesPerCall();
        REQUIRE(scratch.TryResize(static_cast<usize>(unitFrames) * 2));
        PlanarView out;
        out.channelCount = 2;
        out.frameCount = unitFrames;
        out.channels[0] = scratch.Data();
        out.channels[1] = scratch.Data() + unitFrames;
        for (uint32 round = 0; round < Rounds; ++round) {
            const uint32 index = random.NextUInt32Below(view.Value().Header().chunkCount);
            const AudioChunkEntry chunk = view.Value().Chunk(index);
            Array<uint8> payload(allocator);
            REQUIRE(payload.TryAppend(view.Value().ChunkPayload(index)));
            for (uint32 flip = 0; flip < 4; ++flip) {
                payload[random.NextUInt32Below(static_cast<uint32>(payload.Size()))] =
                    static_cast<uint8>(random.NextUInt32());
            }

            // 내장 디코더(PCM16·ADPCM)의 손상 실패는 CorruptData다(3.38절 — 시험 codec은 계약 밖).
            const bool builtIn = kind != 2;
            if (auto begun = decoder.BeginChunk(chunk, payload.View(), chunk.firstFrame); !begun) {
                CHECK(!builtIn || begun.Error() == ErrorCode::CorruptData);
                continue;
            }

            uint64 total = 0;
            bool failed = false;
            while (true) {
                auto written = decoder.DecodeNext(out);
                if (!written) {
                    CHECK(!builtIn || written.Error() == ErrorCode::CorruptData);
                    failed = true;
                    break;
                }

                if (written.Value() == 0) {
                    break;
                }

                total += written.Value();
            }

            CHECK(failed || total == chunk.frameCount);
        }
    }
}

TEST(Audio_Codec_Pcm16GoldenAndEncoderEdges) {
    SystemAllocator allocator;
    const Signal signal(allocator, 2, 2500);
    Pcm16AudioEncoder encoder;
    EncodedAudio encoded(allocator);
    REQUIRE(encoder.Encode(signal.View(), Rate, DefaultAudioChannelMask(2), {}, encoded));
    const Array<uint8> bytes = WriteOrDie(allocator, encoded, {});
    char hex[Sha256Digest::HexBytes + 1];
    Sha256::Hash(bytes.View()).ToHex(hex);
    CHECK(StringView(hex) ==
          StringView("4b46b0feec6cf31c03b70ffeac57f130dde8ce61aa9aef231ac7471b61dfae0e"));

    // 채널 9와 무한대 입력은 거부한다.
    PlanarConstView nine = signal.View();
    nine.channelCount = 9;
    EncodedAudio out(allocator);
    CHECK(encoder.Encode(nine, Rate, AudioChannelMaskAll, {}, out).Error() ==
          ErrorCode::InvalidArgument);
    Array<float> data(allocator);
    REQUIRE(data.TryResize(10));
    data[4] = platform::BitCast<float>(0x7F800000u);
    PlanarConstView infinite;
    infinite.channelCount = 1;
    infinite.frameCount = 10;
    infinite.channels[0] = data.Data();
    CHECK(encoder.Encode(infinite, Rate, DefaultAudioChannelMask(1), {}, out).Error() ==
          ErrorCode::InvalidArgument);

    // writer: PCM16에 pre-roll이나 codec 머리가 있으면 거부한다.
    EncodedAudio withPreRoll(allocator);
    REQUIRE(encoder.Encode(signal.View(), Rate, DefaultAudioChannelMask(2), {}, withPreRoll));
    withPreRoll.preRollFrames = 10;
    Array<uint8> rejected(allocator);
    CHECK(WriteAudioPlatformData(withPreRoll, {}, rejected).Error() == ErrorCode::InvalidArgument);
    CHECK(rejected.Size() == 0);
}

} // namespace audio_codec_tests_file
