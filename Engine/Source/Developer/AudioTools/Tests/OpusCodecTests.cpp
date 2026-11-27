#include <Audio/AudioDecoder.h>
#include <Audio/AudioPlatformData.h>
#include <Audio/OpusCodecHeader.h>
#include <Audio/VorbisChannelOrder.h>
#include <AudioOpus/OpusAudioDecoder.h>
#include <AudioTools/AudioPlatformDataWriter.h>
#include <AudioTools/OpusAudioEncoder.h>
#include <Base/Compiler.h>
#include <Base/Endian.h>
#include <Base/InitializerList.h>
#include <Core/Containers/Array.h>
#include <Core/Encoding/Sha256.h>
#include <Core/Math/MathUtility.h>
#include <Core/Math/RandomStream.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>
#include <TestSupport/Test.h>

using namespace rocks;
namespace opus_codec_tests_file {

using namespace rocks::audio;

// 코덱 계층 C3 시험이다(Docs/Design/AudioSystem 3.32.5절): Opus 인코더(AudioTools)와 런타임
// 디코더(AudioOpus)의 왕복, 머리, 청크 가운데 시작, 거부.

namespace {

constexpr double TwoPi = 6.283185307179586;
constexpr uint32 Rate = OpusPlatformSampleRate;

// 채널(내부 순서 자리)마다 다른 주파수다. 자리 3은 6채널 이상에서 LFE라 낮은 주파수를 둔다.
constexpr double ChannelFrequencies[MaxOutputChannels] = {300.0,  500.0,  700.0,  70.0,
                                                          1100.0, 1300.0, 1500.0, 1700.0};
constexpr double ToneAmplitude = 0.3;

// 왕복 SNR 하한(에너지 비, 품질 80 순음). 가장자리는 시작 과도 응답을 뺀다.
constexpr double MinimumRoundTripSignalToNoise = 100.0;

// 오차가 0일 때 SNR 계산이 돌려주는 값이다.
constexpr double PerfectSignalToNoise = 1.0e30;
constexpr uint32 RoundTripEdgeFrames = 4800;

// 시험 도우미가 실패를 알리는 표지다(검사 대상 오류 코드와 겹치지 않는 값).
constexpr ErrorCode TestSetupFailed = ErrorCode::DependencyFailed;
constexpr ErrorCode OutputNotCleared = ErrorCode::WrongContext;

// 48 kHz AUDIO 인코더의 lookahead(2.5 ms + 4 ms 지연 보정)다.
constexpr uint32 ExpectedLookahead = 312;

// 거부 시험에 쓰는 유효 품질과 Opus가 받지 않는 rate다.
constexpr uint32 MidQuality = 50;
constexpr uint32 NonOpusRate = 44100;

// 품질 비교 시험의 낮은·높은 품질이다.
constexpr uint32 LowQuality = 10;
constexpr uint32 HighQuality = 90;

// 청크 시험: 첫 청크·나머지 청크 목표 byte(작게 해 청크 여러 개), 다음 청크로 넘겨 읽는 frame,
// 청크 가운데 시작의 어긋남(패킷 경계를 피함).
constexpr uint32 SmallFirstChunkBytes = 4096;
constexpr uint32 SmallChunkBytes = 16384;
constexpr uint64 CrossChunkFrames = 100;
// seek 비교 창의 최소 길이다(100 ms — 300 Hz 30주기). 청크 끝 직전 시작도 다음 청크로 이어 읽어 이
// 길이를 채운다(스트림 끝에서는 거기까지). 시작 직후 몇 frame만 보면 신호 에너지가 작아 SNR이
// 측정(3.32.4절)보다 흔들린다.
constexpr uint64 MinimumSeekCompareFrames = 4800;
constexpr uint64 MidChunkOffset = 7;

// 청크 0의 payload 앞 이 패킷 수 안에서 시작하면 디코더가 스트림 머리부터 디코드한다(pre-roll 패킷
// + 시작 패킷).
constexpr uint64 ExactStartPackets = OpusPreRollFrames / OpusPacketFrames + 1;

// seek 시작(320 ms pre-roll로 수렴한 근사)과 연속 디코드 사이 SNR 하한이다(에너지 비, 80 dB. 측정
// 값은 약 91 dB, 3.32.4절).
constexpr double MinimumSeekSignalToNoise = 1.0e8;

// 무작위 손상 시험의 seed와 반복 수다.
constexpr uint32 CorruptionSeed = 0x4F505553;
constexpr uint32 CorruptionRounds = 200;

// Opus TOC byte(RFC 6716 3.1절): config(상위 5 bit)·stereo(1 bit)·frame 수 code(하위 2 bit). CELT
// fullband config 30은 10 ms, code 1은 같은 길이 frame 2개다.
constexpr uint8 TocConfigShift = 3;
constexpr uint8 TocCodeMask = 0x03;
constexpr uint8 TocCeltFullband10Ms = 30;
constexpr uint8 TocCodeTwoFrames = 1;

// libopus family 1 배치(opus_multistream_encoder.c vorbis_mappings, RFC 7845 5.1.1.2)다.
struct VorbisMapping {
    uint8 streams;
    uint8 coupledStreams;
    uint8 mapping[MaxOutputChannels];
};

constexpr VorbisMapping ExpectedVorbisMappings[MaxOutputChannels] = {
    {1, 0, {0}},
    {1, 1, {0, 1}},
    {2, 1, {0, 2, 1}},
    {2, 2, {0, 1, 2, 3}},
    {3, 2, {0, 4, 1, 2, 3}},
    {4, 2, {0, 4, 1, 2, 3, 5}},
    {4, 3, {0, 4, 1, 2, 3, 5, 6}},
    {5, 3, {0, 6, 1, 2, 3, 4, 5, 7}}};

// 내부 순서 채널 c(값 c)를 Vorbis 순서로 교차했을 때 슬롯별 값이다(RFC 7845 5.1.1.2 배치와 내부
// 순서 FL FR FC LFE SL SR BL BR BC, 3.3절). 예: 6채널 Vorbis = FL FC FR SL SR LFE.
constexpr uint8 ExpectedVorbisSlots[MaxOutputChannels][MaxOutputChannels] = {
    {0},
    {0, 1},
    {0, 2, 1},
    {0, 1, 2, 3},
    {0, 2, 1, 3, 4},
    {0, 2, 1, 4, 5, 3},
    {0, 2, 1, 4, 5, 6, 3},
    {0, 2, 1, 4, 5, 6, 7, 3}};

// 채널마다 ChannelFrequencies의 sine을 담는 planar 배열이다.
class MultiTone {
public:
    MultiTone(Allocator& allocator, uint32 channels, uint32 frames) noexcept : samples_(allocator) {
        if (!samples_.TryResize(static_cast<usize>(channels) * frames)) {
            platform::Fatal("Opus codec test signal allocation failed");
        }

        view_.channelCount = channels;
        view_.frameCount = frames;
        for (uint32 channel = 0; channel < channels; ++channel) {
            float* data = samples_.Data() + static_cast<usize>(channel) * frames;
            view_.channels[channel] = data;
            for (uint32 frame = 0; frame < frames; ++frame) {
                data[frame] = static_cast<float>(
                    ToneAmplitude * math::Sin(TwoPi * ChannelFrequencies[channel] * frame / Rate));
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

AudioDecoderTable MakeTable() noexcept {
    AudioDecoderTable table;
    if (!RegisterBuiltInAudioDecoders(table) || !RegisterOpusAudioDecoder(table)) {
        platform::Fatal("Opus codec test table setup failed");
    }

    return table;
}

AudioEncodeSettings Quality(uint32 quality) noexcept {
    AudioEncodeSettings settings;
    settings.quality = quality;
    return settings;
}

void EncodeOrDie(Allocator& allocator, const PlanarConstView& pcm, uint32 quality,
                 EncodedAudio& out) noexcept {
    OpusAudioEncoder encoder(allocator);
    if (!encoder.Encode(pcm, Rate, DefaultAudioChannelMask(pcm.channelCount), Quality(quality),
                        out)) {
        platform::Fatal("Opus codec test encode failed");
    }
}

Array<uint8> WriteOrDie(Allocator& allocator, const EncodedAudio& encoded,
                        const AudioPlatformDataOptions& options) noexcept {
    Array<uint8> bytes(allocator);
    if (!WriteAudioPlatformData(encoded, options, bytes)) {
        platform::Fatal("Opus codec test write failed");
    }

    return bytes;
}

// codec 시간축 [startCodecFrame, + frames)를 decoder로 청크를 이어 디코드해 out(planar, 채널마다
// frames)에 쓴다. decoder는 먼저 view 머리로 연다.
Result<void> ReadCodecRangeWith(AudioDecoder& decoder, const AudioPlatformDataView& view,
                                Allocator& allocator, uint64 startCodecFrame, uint64 frames,
                                Array<float>& out) noexcept {
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

    // 재생과 같이 첫 청크는 BeginChunk(seek)로, 그 뒤 청크는 ContinueChunk로 상태를 이어 연다.
    uint64 position = startCodecFrame;
    uint64 written = 0;
    bool started = false;
    for (uint32 index = 0; index < view.Header().chunkCount && written < frames; ++index) {
        const AudioChunkEntry chunk = view.Chunk(index);
        if (position >= chunk.firstFrame + chunk.frameCount) {
            continue;
        }

        const Span<const uint8> payload = view.ChunkPayload(index);
        if (auto begun = started ? decoder.ContinueChunk(chunk, payload)
                                 : decoder.BeginChunk(chunk, payload, position);
            !begun) {
            return begun;
        }

        started = true;

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

// 새 디코더로 ReadCodecRangeWith를 부른다.
Result<void> ReadCodecRange(const AudioPlatformDataView& view, const AudioDecoderTable& table,
                            Allocator& allocator, uint64 startCodecFrame, uint64 frames,
                            Array<float>& out) noexcept {
    auto created = table.Create(view.Header().codec, allocator);
    if (!created) {
        return Result<void>::Failure(created.Error());
    }

    return ReadCodecRangeWith(*created.Value(), view, allocator, startCodecFrame, frames, out);
}

// 원본과 디코드 결과의 SNR 에너지 비다(가장자리 제외).
double RoundTripSignalToNoise(const PlanarConstView& source, const DecodedAudioPcm& decoded,
                              uint32 channel) noexcept {
    double signal = 0.0;
    double noise = 0.0;
    for (uint64 frame = RoundTripEdgeFrames; frame + RoundTripEdgeFrames < decoded.FrameCount();
         ++frame) {
        const double reference = source.channels[channel][frame];
        const double difference = decoded.Channel(channel)[frame] - reference;
        signal += reference * reference;
        noise += difference * difference;
    }

    return noise > 0.0 ? signal / noise : PerfectSignalToNoise;
}

// channel이 ChannelFrequencies[channel]을 다른 채널 주파수보다 강하게 가지면 true다.
bool ChannelHasOwnFrequency(const DecodedAudioPcm& decoded, uint32 channel) noexcept {
    const auto power = [&](double frequency) noexcept {
        const double coefficient = 2.0 * math::Cos(TwoPi * frequency / Rate);
        double previous = 0.0;
        double before = 0.0;
        for (uint64 index = 0; index < decoded.FrameCount(); ++index) {
            const double current =
                decoded.Channel(channel)[index] + coefficient * previous - before;
            before = previous;
            previous = current;
        }

        return previous * previous + before * before - coefficient * previous * before;
    };

    const double own = power(ChannelFrequencies[channel]);
    for (uint32 other = 0; other < decoded.ChannelCount(); ++other) {
        if (other != channel && power(ChannelFrequencies[other]) >= own) {
            return false;
        }
    }

    return true;
}

// 한 디코더로 Open → BeginChunk 결과를 본다.
ErrorCode BeginError(const AudioPlatformDataView& view, const AudioChunkEntry& chunk,
                     Span<const uint8> payload, uint64 start) noexcept {
    SystemAllocator allocator;
    const AudioDecoderTable table = MakeTable();
    auto created = table.Create(AudioCodecId::Opus, allocator);
    if (!created || !created.Value()->Open(view.Header(), view.CodecHeader())) {
        return TestSetupFailed;
    }

    return created.Value()->BeginChunk(chunk, payload, start).Error();
}

} // namespace

TEST(Audio_OpusCodec_QualityBitrate) {
    CHECK(OpusBitrateForQuality(1, 1) == 16000);
    CHECK(OpusBitrateForQuality(100, 1) == 96000);
    CHECK(OpusBitrateForQuality(40, 1) == 47515);
    CHECK(OpusBitrateForQuality(DefaultAudioCompressionQuality, 2) == 2 * 79838);
    CHECK(OpusBitrateForQuality(0, 1) == 0 && OpusBitrateForQuality(101, 1) == 0 &&
          OpusBitrateForQuality(50, 0) == 0 && OpusBitrateForQuality(50, 9) == 0);

    // UE GetBitRateFromQuality(GetMappedRangeValueClamped를 float로 계산해 int로 자름)와 같다.
    bool same = true;
    for (uint32 quality = MinAudioCompressionQuality; quality <= MaxAudioCompressionQuality;
         ++quality) {
        const float percent = (static_cast<float>(quality) - 1.0f) / (100.0f - 1.0f);
        const int32 ue = static_cast<int32>(16000.0f + percent * (96000.0f - 16000.0f));
        same = same && OpusBitrateForQuality(quality, 1) == static_cast<uint32>(ue);
    }

    CHECK(same);

    // 품질이 높으면 byte가 많다.
    SystemAllocator allocator;
    const MultiTone tone(allocator, 2, Rate);
    EncodedAudio low(allocator);
    EncodedAudio high(allocator);
    EncodeOrDie(allocator, tone.View(), LowQuality, low);
    EncodeOrDie(allocator, tone.View(), HighQuality, high);
    CHECK(low.payload.Size() < high.payload.Size());
}

TEST(Audio_OpusCodec_RoundTripChannels) {
    SystemAllocator allocator;
    const AudioDecoderTable table = MakeTable();
    constexpr uint32 Frames = Rate;
    for (uint32 channels = 1; channels <= MaxOutputChannels; ++channels) {
        const MultiTone tone(allocator, channels, Frames);
        EncodedAudio encoded(allocator);
        EncodeOrDie(allocator, tone.View(), DefaultAudioCompressionQuality, encoded);
        CHECK(encoded.codec == AudioCodecId::Opus && encoded.encoderDelay == ExpectedLookahead);
        CHECK((encoded.encoderDelay + Frames + encoded.endPadding) % OpusPacketFrames == 0 &&
              encoded.endPadding < OpusPacketFrames);
        CHECK(encoded.preRollFrames == OpusPreRollFrames &&
              encoded.codecHeader.Size() == OpusCodecHeaderBytes);
        const Array<uint8> bytes = WriteOrDie(allocator, encoded, {});
        auto view = AudioPlatformDataView::Parse(bytes.View());
        REQUIRE(view);
        const AudioPlatformDataHeader& header = view.Value().Header();
        CHECK(header.codec == AudioCodecId::Opus && header.sampleRate == Rate &&
              header.channels == channels && header.frames == Frames &&
              header.bitstreamVersion == OpusBitstreamVersion);
        auto codecHeader = ParseOpusCodecHeader(view.Value().CodecHeader(), header);
        REQUIRE(codecHeader);
        // family byte와 stream·coupled·mapping이 libopus Vorbis mapping(RFC 7845 5.1.1.2)과 같다.
        const OpusCodecHeader& opus = codecHeader.Value();
        const VorbisMapping& expected = ExpectedVorbisMappings[channels - 1];
        bool mappingSame =
            opus.streams == expected.streams && opus.coupledStreams == expected.coupledStreams;
        for (uint32 channel = 0; channel < channels; ++channel) {
            mappingSame = mappingSame && opus.mapping[channel] == expected.mapping[channel];
        }

        CHECK(view.Value().CodecHeader()[0] == OpusMappingFamilyVorbis && mappingSame &&
              opus.preSkip == ExpectedLookahead);

        DecodedAudioPcm decoded(allocator);
        REQUIRE(DecodeAudioPlatformData(view.Value(), table, allocator, decoded));
        REQUIRE(decoded.FrameCount() == Frames && decoded.ChannelCount() == channels &&
                decoded.SampleRate() == Rate);
        for (uint32 channel = 0; channel < channels; ++channel) {
            CHECK(ChannelHasOwnFrequency(decoded, channel));
            CHECK(RoundTripSignalToNoise(tone.View(), decoded, channel) >=
                  MinimumRoundTripSignalToNoise);
        }
    }
}

TEST(Audio_OpusCodec_ChunksAndMidChunkStart) {
    SystemAllocator allocator;
    const AudioDecoderTable table = MakeTable();
    constexpr uint32 Frames = 4 * Rate;
    const MultiTone tone(allocator, 2, Frames);
    EncodedAudio encoded(allocator);
    EncodeOrDie(allocator, tone.View(), DefaultAudioCompressionQuality, encoded);
    AudioPlatformDataOptions options;
    options.firstChunkBytes = SmallFirstChunkBytes;
    options.targetChunkBytes = SmallChunkBytes;
    const Array<uint8> bytes = WriteOrDie(allocator, encoded, options);
    auto parsed = AudioPlatformDataView::Parse(bytes.View());
    REQUIRE(parsed);
    const AudioPlatformDataView& view = parsed.Value();
    const uint32 delay = view.Header().encoderDelay;
    REQUIRE(view.Header().chunkCount >= 4);
    for (uint32 index = 0; index < view.Header().chunkCount; ++index) {
        const AudioChunkEntry chunk = view.Chunk(index);
        // pre-roll은 OpusPreRollFrames(앞쪽 청크는 codec frame 0까지만)이다.
        const uint64 expectedPreRoll =
            chunk.firstFrame < OpusPreRollFrames ? chunk.firstFrame : OpusPreRollFrames;
        CHECK(chunk.preRollFrames == expectedPreRoll && chunk.frameCount % OpusPacketFrames == 0);
    }

    // 전체 디코드(청크 0 BeginChunk, 그 뒤 ContinueChunk) = 청크 하나로 쓴 같은 인코드의
    // 디코드(연속 디코드). 청크로 나눠도 순차 재생 결과는 bit까지 같다.
    DecodedAudioPcm full(allocator);
    REQUIRE(DecodeAudioPlatformData(view, table, allocator, full));
    REQUIRE(full.FrameCount() == Frames);
    {
        const Array<uint8> singleBytes = WriteOrDie(allocator, encoded, {});
        auto single = AudioPlatformDataView::Parse(singleBytes.View());
        REQUIRE(single && single.Value().Header().chunkCount == 1);
        DecodedAudioPcm continuous(allocator);
        REQUIRE(DecodeAudioPlatformData(single.Value(), table, allocator, continuous));
        bool same = continuous.FrameCount() == Frames;
        for (uint32 channel = 0; channel < 2 && same; ++channel) {
            for (uint64 frame = 0; frame < Frames; ++frame) {
                same = same && full.Channel(channel)[frame] == continuous.Channel(channel)[frame];
            }
        }

        CHECK(same);
    }

    // 청크마다 여러 시작 위치(머리·패킷 경계 앞뒤·가운데·끝 직전)에서 seek(BeginChunk)해 다음 청크
    // 조금까지 읽는다(다음 청크는 ContinueChunk). 청크 0의 payload 앞 ExactStartPackets 패킷 안
    // 시작은 스트림 머리부터 디코드하므로 연속 디코드와 bit가 같다. 그 밖의 시작은 pre-roll로
    // 수렴한 근사라 SNR 하한으로 본다. 같은 시작 두 번은 언제나 bit가 같다. codec frame c는 유효
    // frame c − delay다.
    bool exact = true;
    bool close = true;
    bool repeatable = true;
    uint32 approximated = 0;
    const AudioChunkEntry last = view.Chunk(view.Header().chunkCount - 1);
    const uint64 streamEnd = last.firstFrame + last.frameCount;
    for (uint32 index = 0; index < view.Header().chunkCount; ++index) {
        const AudioChunkEntry chunk = view.Chunk(index);
        const uint64 end = chunk.firstFrame + chunk.frameCount;
        for (const uint64 offset :
             {uint64{0}, uint64{1}, uint64{OpusPacketFrames - 1}, uint64{OpusPacketFrames},
              uint64{chunk.frameCount / 2 + MidChunkOffset}, uint64{chunk.frameCount - 1}}) {
            const uint64 start = chunk.firstFrame + offset;
            const uint64 wanted =
                end - start + (index + 1 < view.Header().chunkCount ? CrossChunkFrames : 0);
            const uint64 longEnough =
                wanted < MinimumSeekCompareFrames ? MinimumSeekCompareFrames : wanted;
            const uint64 frames = longEnough < streamEnd - start ? longEnough : streamEnd - start;
            Array<float> range(allocator);
            Array<float> again(allocator);
            REQUIRE(ReadCodecRange(view, table, allocator, start, frames, range));
            REQUIRE(ReadCodecRange(view, table, allocator, start, frames, again));
            const bool exactStart = index == 0 && offset / OpusPacketFrames < ExactStartPackets;
            approximated += exactStart ? 0 : 1;
            double signal = 0.0;
            double noise = 0.0;
            for (uint32 channel = 0; channel < 2; ++channel) {
                for (uint64 frame = 0; frame < frames; ++frame) {
                    const usize at = static_cast<usize>(channel * frames + frame);
                    repeatable = repeatable && range[at] == again[at];
                    const uint64 codecFrame = start + frame;
                    if (codecFrame < delay || codecFrame - delay >= Frames) {
                        continue;
                    }

                    const double reference = full.Channel(channel)[codecFrame - delay];
                    const double difference = range[at] - reference;
                    signal += reference * reference;
                    noise += difference * difference;
                    exact = exact && (!exactStart || difference == 0.0);
                }
            }

            close = close && (noise == 0.0 || signal / noise >= MinimumSeekSignalToNoise);
        }
    }

    CHECK(exact);
    CHECK(close);
    CHECK(repeatable);
    CHECK(approximated > 0);
}

TEST(Audio_OpusCodec_ContinueChunkRules) {
    SystemAllocator allocator;
    const AudioDecoderTable table = MakeTable();
    const MultiTone tone(allocator, 2, 2 * Rate);
    EncodedAudio encoded(allocator);
    EncodeOrDie(allocator, tone.View(), DefaultAudioCompressionQuality, encoded);
    AudioPlatformDataOptions options;
    options.firstChunkBytes = SmallFirstChunkBytes;
    options.targetChunkBytes = SmallChunkBytes;
    const Array<uint8> bytes = WriteOrDie(allocator, encoded, options);
    auto parsed = AudioPlatformDataView::Parse(bytes.View());
    REQUIRE(parsed);
    const AudioPlatformDataView& view = parsed.Value();
    REQUIRE(view.Header().chunkCount >= 3);
    auto created = table.Create(AudioCodecId::Opus, allocator);
    REQUIRE(created);
    AudioDecoder& decoder = *created.Value();
    Array<float> scratch(allocator);
    REQUIRE(scratch.TryResize(static_cast<usize>(OpusPacketFrames) * 2));
    PlanarView out;
    out.channelCount = 2;
    out.frameCount = OpusPacketFrames;
    out.channels[0] = scratch.Data();
    out.channels[1] = scratch.Data() + OpusPacketFrames;
    const auto drain = [&]() noexcept {
        while (true) {
            auto count = decoder.DecodeNext(out);
            if (!count || count.Value() == 0) {
                return static_cast<bool>(count);
            }
        }
    };

    // Open 전·청크 없이 ContinueChunk는 InvalidArgument다.
    CHECK(decoder.ContinueChunk(view.Chunk(1), view.ChunkPayload(1)).Error() ==
          ErrorCode::InvalidArgument);
    REQUIRE(decoder.Open(view.Header(), view.CodecHeader()));
    CHECK(decoder.ContinueChunk(view.Chunk(1), view.ChunkPayload(1)).Error() ==
          ErrorCode::InvalidArgument);
    // 앞 청크를 다 내지 않았으면 InvalidArgument다(열린 청크도 닫힌다).
    REQUIRE(decoder.BeginChunk(view.Chunk(0), view.ChunkPayload(0), 0));
    REQUIRE(decoder.DecodeNext(out));
    CHECK(decoder.ContinueChunk(view.Chunk(1), view.ChunkPayload(1)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(decoder.DecodeNext(out).Error() == ErrorCode::InvalidArgument);
    // 이어지지 않는 청크(0 다음 2)는 InvalidArgument다.
    REQUIRE(decoder.BeginChunk(view.Chunk(0), view.ChunkPayload(0), 0));
    REQUIRE(drain());
    CHECK(decoder.ContinueChunk(view.Chunk(2), view.ChunkPayload(2)).Error() ==
          ErrorCode::InvalidArgument);
    // 청크 가운데에서 seek해 끝까지 낸 뒤에도 다음 청크로 이을 수 있다. payload 검사는 BeginChunk와
    // 같다.
    const AudioChunkEntry first = view.Chunk(0);
    REQUIRE(
        decoder.BeginChunk(first, view.ChunkPayload(0), first.firstFrame + first.frameCount / 2));
    REQUIRE(drain());
    AudioChunkEntry broken = view.Chunk(1);
    broken.payloadBytes -= 1;
    CHECK(decoder.ContinueChunk(broken, view.ChunkPayload(1)).Error() ==
          ErrorCode::InvalidArgument);
    REQUIRE(
        decoder.BeginChunk(first, view.ChunkPayload(0), first.firstFrame + first.frameCount / 2));
    REQUIRE(drain());
    CHECK(decoder.ContinueChunk(view.Chunk(1), view.ChunkPayload(1)));
    CHECK(drain());
}

TEST(Audio_OpusCodec_CodecHeaderRejects) {
    SystemAllocator allocator;
    const AudioDecoderTable table = MakeTable();
    const MultiTone tone(allocator, 6, OpusPacketFrames * 4);
    EncodedAudio encoded(allocator);
    EncodeOrDie(allocator, tone.View(), DefaultAudioCompressionQuality, encoded);
    const Array<uint8> bytes = WriteOrDie(allocator, encoded, {});
    auto parsed = AudioPlatformDataView::Parse(bytes.View());
    REQUIRE(parsed);
    const AudioPlatformDataView& view = parsed.Value();
    auto created = table.Create(AudioCodecId::Opus, allocator);
    REQUIRE(created);
    AudioDecoder& decoder = *created.Value();
    REQUIRE(decoder.Open(view.Header(), view.CodecHeader()));

    uint8 valid[OpusCodecHeaderBytes];
    for (usize index = 0; index < OpusCodecHeaderBytes; ++index) {
        valid[index] = view.CodecHeader()[index];
    }

    const auto openWith = [&](usize at, uint8 value) noexcept {
        uint8 changed[OpusCodecHeaderBytes];
        for (usize index = 0; index < OpusCodecHeaderBytes; ++index) {
            changed[index] = valid[index];
        }

        changed[at] = value;
        return decoder.Open(view.Header(), Span<const uint8>(changed, OpusCodecHeaderBytes))
            .Error();
    };

    // 6채널: stream 4, coupled 2(coded 6). 칸 위치는 3.32.2절.
    CHECK(openWith(0, 2) == ErrorCode::CorruptData); // family
    CHECK(openWith(1, 0) == ErrorCode::CorruptData); // stream 0
    CHECK(openWith(2, 5) == ErrorCode::CorruptData); // coupled > stream
    CHECK(openWith(3, 1) == ErrorCode::CorruptData); // 예약
    CHECK(openWith(4, static_cast<uint8>(valid[4] + 1)) == ErrorCode::CorruptData); // pre-skip
    CHECK(openWith(6, 1) == ErrorCode::CorruptData);                                // gain
    CHECK(openWith(1, 5) == ErrorCode::CorruptData);  // stream + coupled = 7 ≠ 채널 수
    CHECK(openWith(8, 6) == ErrorCode::CorruptData);  // mapping 범위 밖
    CHECK(openWith(14, 1) == ErrorCode::CorruptData); // 채널 수 뒤 mapping 칸
    CHECK(decoder.Open(view.Header(), Span<const uint8>(valid, 8)).Error() ==
          ErrorCode::CorruptData);

    const auto openHeader = [&](auto change) noexcept {
        AudioPlatformDataHeader header = view.Header();
        change(header);
        return decoder.Open(header, view.CodecHeader()).Error();
    };

    CHECK(openHeader([](AudioPlatformDataHeader& h) noexcept { h.sampleRate = NonOpusRate; }) ==
          ErrorCode::CorruptData);
    // 6채널이지만 기본 배치(SL·SR)가 아니라 BL·BR이다.
    CHECK(openHeader([](AudioPlatformDataHeader& h) noexcept {
              h.channelMask = AudioChannelFrontLeft | AudioChannelFrontRight |
                              AudioChannelFrontCenter | AudioChannelLowFrequency |
                              AudioChannelBackLeft | AudioChannelBackRight;
          }) == ErrorCode::CorruptData);
    CHECK(openHeader([](AudioPlatformDataHeader& h) noexcept { h.channels = 0; }) ==
          ErrorCode::CorruptData);
    CHECK(openHeader([](AudioPlatformDataHeader& h) noexcept { h.channels = 9; }) ==
          ErrorCode::CorruptData);
    CHECK(openHeader([](AudioPlatformDataHeader& h) noexcept { h.codec = AudioCodecId::Adpcm; }) ==
          ErrorCode::InvalidArgument);
    CHECK(openHeader([](AudioPlatformDataHeader& h) noexcept { h.bitstreamVersion = 2; }) ==
          ErrorCode::Unsupported);
    // 판은 codec 머리 규칙보다 먼저 본다: 새 판의 codec 머리 크기가 달라도 Unsupported다.
    AudioPlatformDataHeader newer = view.Header();
    newer.bitstreamVersion = OpusBitstreamVersion + 1;
    CHECK(decoder.Open(newer, Span<const uint8>(valid, AudioCodecHeaderAlignment)).Error() ==
          ErrorCode::Unsupported);

    // 열린 청크가 있는 디코더의 Open이 실패하면 청크가 남지 않는다(DecodeNext·BeginChunk 거부).
    Array<float> scratch(allocator);
    REQUIRE(scratch.TryResize(static_cast<usize>(OpusPacketFrames) * MaxOutputChannels));
    PlanarView out;
    out.channelCount = view.Header().channels;
    out.frameCount = OpusPacketFrames;
    for (uint32 channel = 0; channel < out.channelCount; ++channel) {
        out.channels[channel] = scratch.Data() + static_cast<usize>(channel) * OpusPacketFrames;
    }

    REQUIRE(decoder.Open(view.Header(), view.CodecHeader()));
    REQUIRE(decoder.BeginChunk(view.Chunk(0), view.ChunkPayload(0), 0));
    REQUIRE(decoder.DecodeNext(out));
    CHECK(openWith(0, 2) == ErrorCode::CorruptData);
    CHECK(decoder.DecodeNext(out).Error() == ErrorCode::InvalidArgument);
    CHECK(decoder.BeginChunk(view.Chunk(0), view.ChunkPayload(0), 0).Error() ==
          ErrorCode::InvalidArgument);
    // 다시 열면 된다.
    REQUIRE(decoder.Open(view.Header(), view.CodecHeader()));
    CHECK(decoder.BeginChunk(view.Chunk(0), view.ChunkPayload(0), 0));
}

TEST(Audio_OpusCodec_ReopenChannelCounts) {
    // 같은 디코더를 2 → 8(상태 다시 할당) → 1(잡은 상태 다시 씀)채널로 다시 열어도 새 디코더와 같은
    // 결과다.
    SystemAllocator allocator;
    const AudioDecoderTable table = MakeTable();
    auto created = table.Create(AudioCodecId::Opus, allocator);
    REQUIRE(created);
    AudioDecoder& shared = *created.Value();
    for (const uint32 channels : {2u, 8u, 1u}) {
        const MultiTone tone(allocator, channels, OpusPacketFrames * 8);
        EncodedAudio encoded(allocator);
        EncodeOrDie(allocator, tone.View(), DefaultAudioCompressionQuality, encoded);
        const Array<uint8> bytes = WriteOrDie(allocator, encoded, {});
        auto parsed = AudioPlatformDataView::Parse(bytes.View());
        REQUIRE(parsed);
        const AudioPlatformDataView& view = parsed.Value();
        const uint64 frames = view.Header().CodecFrames();
        Array<float> reused(allocator);
        Array<float> fresh(allocator);
        REQUIRE(ReadCodecRangeWith(shared, view, allocator, 0, frames, reused));
        REQUIRE(ReadCodecRange(view, table, allocator, 0, frames, fresh));
        bool same = reused.Size() == fresh.Size();
        for (usize index = 0; same && index < reused.Size(); ++index) {
            same = reused[index] == fresh[index];
        }

        CHECK(same);
    }
}

TEST(Audio_OpusCodec_VorbisChannelOrderTable) {
    // 인코더·디코더가 같은 표를 쓰므로 왕복으로는 표 오류가 드러나지 않는다. 고정 기대값과 직접
    // 비교한다.
    for (uint32 channels = 1; channels <= MaxOutputChannels; ++channels) {
        float values[MaxOutputChannels] = {};
        const float* channelData[MaxOutputChannels] = {};
        for (uint32 channel = 0; channel < channels; ++channel) {
            values[channel] = static_cast<float>(channel);
            channelData[channel] = &values[channel];
        }

        float interleaved[MaxOutputChannels] = {};
        InterleaveVorbisOrder(channelData, channels, 0, 1, interleaved);
        bool same = true;
        for (uint32 slot = 0; slot < channels; ++slot) {
            same = same &&
                   interleaved[slot] == static_cast<float>(ExpectedVorbisSlots[channels - 1][slot]);
        }

        // 되돌리면 내부 순서다.
        float planar[MaxOutputChannels] = {};
        DeinterleaveVorbisOrder(interleaved, channels, 1, planar);
        for (uint32 channel = 0; channel < channels; ++channel) {
            same = same && planar[channel] == static_cast<float>(channel);
        }

        CHECK(same);
    }
}

TEST(Audio_OpusCodec_ChunkCorruption) {
    SystemAllocator allocator;
    const MultiTone tone(allocator, 2, OpusPacketFrames * 6);
    EncodedAudio encoded(allocator);
    EncodeOrDie(allocator, tone.View(), DefaultAudioCompressionQuality, encoded);
    const Array<uint8> bytes = WriteOrDie(allocator, encoded, {});
    auto parsed = AudioPlatformDataView::Parse(bytes.View());
    REQUIRE(parsed);
    const AudioPlatformDataView& view = parsed.Value();
    const AudioChunkEntry chunk = view.Chunk(0);
    const Span<const uint8> payload = view.ChunkPayload(0);
    REQUIRE(BeginError(view, chunk, payload, 0) == ErrorCode::None);

    const auto withPayload = [&](auto change, uint32 extraBytes) noexcept {
        Array<uint8> copy(allocator);
        if (!copy.TryAppend(payload) || !copy.TryResize(copy.Size() + extraBytes)) {
            platform::Fatal("Opus codec test copy failed");
        }

        change(copy);
        AudioChunkEntry changed = chunk;
        changed.payloadBytes = static_cast<uint32>(copy.Size());
        return BeginError(view, changed, copy.View(), 0);
    };

    // 첫 길이 0, 길이가 payload 밖, 남는 byte 하나.
    CHECK(withPayload([](Array<uint8>& p) noexcept { StoreLittleEndian<uint16>(p.Data(), 0); },
                      0) == ErrorCode::CorruptData);
    CHECK(withPayload([](Array<uint8>& p) noexcept { StoreLittleEndian<uint16>(p.Data(), 0xFFFF); },
                      0) == ErrorCode::CorruptData);
    CHECK(withPayload([](Array<uint8>&) noexcept {}, 1) == ErrorCode::CorruptData);
    // 끝에 붙은 단위의 길이가 남은 byte(0)보다 크다.
    CHECK(
        withPayload(
            [](Array<uint8>& p) noexcept { StoreLittleEndian<uint16>(p.Data() + p.Size() - 2, 5); },
            2) == ErrorCode::CorruptData);

    // 청크 표 값: payload byte 수 불일치, frame 수가 960 배수 아님, 패킷 수 불일치.
    AudioChunkEntry changed = chunk;
    changed.payloadBytes = chunk.payloadBytes - 1;
    CHECK(BeginError(view, changed, payload, 0) == ErrorCode::InvalidArgument);
    changed = chunk;
    changed.frameCount = chunk.frameCount - 1;
    CHECK(BeginError(view, changed, payload, 0) == ErrorCode::CorruptData);
    changed = chunk;
    changed.frameCount = chunk.frameCount - OpusPacketFrames;
    CHECK(BeginError(view, changed, payload, 0) == ErrorCode::CorruptData);
    changed = chunk;
    changed.preRollFrames = 1;
    CHECK(BeginError(view, changed, payload, 0) == ErrorCode::CorruptData);
    // startCodecFrame 범위 밖.
    CHECK(BeginError(view, chunk, payload, chunk.frameCount) == ErrorCode::InvalidArgument);

    // 첫 패킷 TOC를 10 ms(config 30)나 frame 2개(code 1, 40 ms)로 바꾸면 패킷 길이가 960이 아니다.
    // DecodeNext가 이미 PCM을 낸 뒤 실패하지 않게 BeginChunk가 거른다.
    constexpr usize TocAt = OpusLengthPrefixBytes;
    CHECK(withPayload(
              [](Array<uint8>& p) noexcept {
                  const uint8 lowBits = static_cast<uint8>(p[TocAt] & ((1u << TocConfigShift) - 1));
                  p[TocAt] = static_cast<uint8>((TocCeltFullband10Ms << TocConfigShift) | lowBits);
              },
              0) == ErrorCode::CorruptData);
    CHECK(withPayload(
              [](Array<uint8>& p) noexcept {
                  p[TocAt] = static_cast<uint8>((p[TocAt] & ~TocCodeMask) | TocCodeTwoFrames);
              },
              0) == ErrorCode::CorruptData);

    // 패킷 byte(길이 접두 밖) 무작위 교체: 오류 또는 성공, 멈추거나 범위 밖을 읽지 않는다.
    const AudioDecoderTable table = MakeTable();
    RandomStream random(CorruptionSeed);
    for (uint32 round = 0; round < CorruptionRounds; ++round) {
        Array<uint8> copy(allocator);
        REQUIRE(copy.TryAppend(payload));
        // 첫 패킷 본문 안 byte를 바꾼다(길이 접두는 그대로).
        const usize firstLength = LoadLittleEndian<uint16>(copy.Data());
        const usize at =
            OpusLengthPrefixBytes + random.NextUInt32Below(static_cast<uint32>(firstLength));
        copy[at] = static_cast<uint8>(random.NextUInt32());
        auto created = table.Create(AudioCodecId::Opus, allocator);
        REQUIRE(created);
        AudioDecoder& decoder = *created.Value();
        REQUIRE(decoder.Open(view.Header(), view.CodecHeader()));
        Array<float> scratch(allocator);
        REQUIRE(scratch.TryResize(static_cast<usize>(OpusPacketFrames) * 2));
        PlanarView out;
        out.channelCount = 2;
        out.frameCount = OpusPacketFrames;
        out.channels[0] = scratch.Data();
        out.channels[1] = scratch.Data() + OpusPacketFrames;
        // 손상 실패는 CorruptData다(3.38절 — 호출 순서·인자는 맞다).
        if (auto begun = decoder.BeginChunk(chunk, copy.View(), 0); !begun) {
            CHECK(begun.Error() == ErrorCode::CorruptData);
            continue;
        }

        uint32 calls = 0;
        while (calls < CorruptionRounds) {
            auto count = decoder.DecodeNext(out);
            ++calls;
            if (!count) {
                CHECK(count.Error() == ErrorCode::CorruptData);
                break;
            }

            if (count.Value() == 0) {
                break;
            }
        }

        CHECK(calls < CorruptionRounds);
    }
}

TEST(Audio_OpusCodec_EncoderRejects) {
    SystemAllocator allocator;
    const MultiTone tone(allocator, 2, OpusPacketFrames);
    OpusAudioEncoder encoder(allocator);
    CHECK(encoder.Info().codec == AudioCodecId::Opus && encoder.Info().minSampleRate == Rate &&
          encoder.Info().maxSampleRate == Rate);
    const uint32 stereo = DefaultAudioChannelMask(2);
    EncodedAudio out(allocator);
    const auto error = [&](const PlanarConstView& pcm, uint32 rate, uint32 mask,
                           uint32 quality) noexcept {
        // 먼저 성공 인코드로 out을 채워 둔다.
        if (!encoder.Encode(tone.View(), Rate, DefaultAudioChannelMask(2), Quality(MidQuality),
                            out) ||
            out.payload.Size() == 0) {
            return TestSetupFailed;
        }

        const ErrorCode code = encoder.Encode(pcm, rate, mask, Quality(quality), out).Error();
        // 실패하면 out을 비운다.
        return out.codec == AudioCodecId::None && out.payload.Size() == 0 ? code : OutputNotCleared;
    };

    CHECK(error(tone.View(), NonOpusRate, stereo, MidQuality) == ErrorCode::InvalidArgument);
    CHECK(error(tone.View(), Rate, stereo, 0) == ErrorCode::InvalidArgument);
    CHECK(error(tone.View(), Rate, stereo, 101) == ErrorCode::InvalidArgument);
    CHECK(error(tone.View(), Rate, AudioChannelFrontLeft, MidQuality) ==
          ErrorCode::InvalidArgument);
    CHECK(error(tone.View(), Rate, AudioChannelFrontLeft | AudioChannelFrontCenter, MidQuality) ==
          ErrorCode::Unsupported);
    PlanarConstView noChannels = tone.View();
    noChannels.channelCount = 0;
    CHECK(error(noChannels, Rate, 0, MidQuality) == ErrorCode::InvalidArgument);
    PlanarConstView nine = tone.View();
    nine.channelCount = MaxOutputChannels + 1;
    CHECK(error(nine, Rate, AudioChannelMaskAll, MidQuality) == ErrorCode::InvalidArgument);
    PlanarConstView noFrames = tone.View();
    noFrames.frameCount = 0;
    CHECK(error(noFrames, Rate, stereo, MidQuality) == ErrorCode::InvalidArgument);
    Array<float> data(allocator);
    REQUIRE(data.TryResize(OpusPacketFrames));
    data[10] = platform::BitCast<float>(0x7F800000u);
    PlanarConstView infinite;
    infinite.channelCount = 1;
    infinite.frameCount = OpusPacketFrames;
    infinite.channels[0] = data.Data();
    CHECK(error(infinite, Rate, DefaultAudioChannelMask(1), MidQuality) ==
          ErrorCode::InvalidArgument);
}

TEST(Audio_OpusCodec_DecoderContract) {
    SystemAllocator allocator;
    AudioDecoderTable table = MakeTable();
    CHECK(RegisterOpusAudioDecoder(table).Error() == ErrorCode::AlreadyExists);
    const MultiTone tone(allocator, 2, OpusPacketFrames * 2);
    EncodedAudio encoded(allocator);
    EncodeOrDie(allocator, tone.View(), DefaultAudioCompressionQuality, encoded);
    const Array<uint8> bytes = WriteOrDie(allocator, encoded, {});
    auto parsed = AudioPlatformDataView::Parse(bytes.View());
    REQUIRE(parsed);
    const AudioPlatformDataView& view = parsed.Value();
    auto created = table.Create(AudioCodecId::Opus, allocator);
    REQUIRE(created);
    AudioDecoder& decoder = *created.Value();
    CHECK(decoder.MaxFramesPerCall() == OpusPacketFrames);

    Array<float> scratch(allocator);
    REQUIRE(scratch.TryResize(static_cast<usize>(OpusPacketFrames) * 3));
    PlanarView out;
    out.channelCount = 2;
    out.frameCount = OpusPacketFrames;
    out.channels[0] = scratch.Data();
    out.channels[1] = scratch.Data() + OpusPacketFrames;
    // Open·BeginChunk 전.
    CHECK(decoder.DecodeNext(out).Error() == ErrorCode::InvalidArgument);
    CHECK(decoder.BeginChunk(view.Chunk(0), view.ChunkPayload(0), 0).Error() ==
          ErrorCode::InvalidArgument);
    REQUIRE(decoder.Open(view.Header(), view.CodecHeader()));
    CHECK(decoder.DecodeNext(out).Error() == ErrorCode::InvalidArgument);
    REQUIRE(decoder.BeginChunk(view.Chunk(0), view.ChunkPayload(0), 0));

    // 채널 수 불일치·용량 부족.
    PlanarView wrongChannels = out;
    wrongChannels.channelCount = 3;
    wrongChannels.channels[2] = scratch.Data() + 2 * OpusPacketFrames;
    CHECK(decoder.DecodeNext(wrongChannels).Error() == ErrorCode::InvalidArgument);
    PlanarView small = out;
    small.frameCount = OpusPacketFrames - 1;
    CHECK(decoder.DecodeNext(small).Error() == ErrorCode::InvalidArgument);

    // 청크 끝까지 960씩 내고 끝이면 0이다.
    uint64 total = 0;
    while (true) {
        auto count = decoder.DecodeNext(out);
        REQUIRE(count);
        if (count.Value() == 0) {
            break;
        }

        CHECK(count.Value() == OpusPacketFrames);
        total += count.Value();
    }

    CHECK(total == view.Chunk(0).frameCount);
    CHECK(decoder.DecodeNext(out) && decoder.DecodeNext(out).Value() == 0);
}

TEST(Audio_OpusCodec_DeterministicGolden) {
    // 정수로 만든 톱니파(입력에 libm 없음)를 인코드한다. libopus 인코더·디코더는 libm을 부르므로
    // 고정값은 x86-64(Windows UCRT)만 단언하고, 그 밖은 두 번 같은지만 본다(3.32.5절).
    SystemAllocator allocator;
    constexpr uint32 Frames = 9600;
    constexpr uint32 SawPeriod = 400;
    constexpr uint32 SawStep = 7;
    Array<float> data(allocator);
    REQUIRE(data.TryResize(2 * Frames));
    for (uint32 frame = 0; frame < Frames; ++frame) {
        const int32 phase =
            static_cast<int32>(frame * SawStep % SawPeriod) - static_cast<int32>(SawPeriod / 2);
        data[frame] = static_cast<float>(phase) / static_cast<float>(2 * SawPeriod);
        data[Frames + frame] = -data[frame];
    }

    PlanarConstView in;
    in.channelCount = 2;
    in.frameCount = Frames;
    in.channels[0] = data.Data();
    in.channels[1] = data.Data() + Frames;
    EncodedAudio first(allocator);
    EncodedAudio second(allocator);
    EncodeOrDie(allocator, in, DefaultAudioCompressionQuality, first);
    EncodeOrDie(allocator, in, DefaultAudioCompressionQuality, second);
    const Array<uint8> firstBytes = WriteOrDie(allocator, first, {});
    const Array<uint8> secondBytes = WriteOrDie(allocator, second, {});
    CHECK(Sha256::Hash(firstBytes.View()) == Sha256::Hash(secondBytes.View()));

    auto view = AudioPlatformDataView::Parse(firstBytes.View());
    REQUIRE(view);
    const AudioDecoderTable table = MakeTable();
    DecodedAudioPcm decoded(allocator);
    REQUIRE(DecodeAudioPlatformData(view.Value(), table, allocator, decoded));
    const Span<const uint8> pcmBytes(reinterpret_cast<const uint8*>(decoded.Channel(0)),
                                     static_cast<usize>(decoded.FrameCount()) * 2 * sizeof(float));
#if ROCKS_ARCH_X64
    char hex[Sha256Digest::HexBytes + 1];
    Sha256::Hash(firstBytes.View()).ToHex(hex);
    CHECK(StringView(hex) ==
          StringView("2931817da339a38230fdeae25999e3b65daffd3f159aa4d209936ec55c07fba5"));
    Sha256::Hash(pcmBytes).ToHex(hex);
    CHECK(StringView(hex) ==
          StringView("bb3fd574075772d698ec312d5d781de8cc372486b97f074be6e9afc87ba9d234"));
#else
    DecodedAudioPcm again(allocator);
    REQUIRE(DecodeAudioPlatformData(view.Value(), table, allocator, again));
    const Span<const uint8> againBytes(reinterpret_cast<const uint8*>(again.Channel(0)),
                                       static_cast<usize>(again.FrameCount()) * 2 * sizeof(float));
    CHECK(Sha256::Hash(pcmBytes) == Sha256::Hash(againBytes));
#endif
}

} // namespace opus_codec_tests_file
