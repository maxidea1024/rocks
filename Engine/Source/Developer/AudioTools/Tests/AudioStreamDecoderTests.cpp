#include <Audio/AudioChunkSource.h>
#include <Audio/AudioDecoder.h>
#include <Audio/AudioPlatformData.h>
#include <Audio/AudioStreamDecoder.h>
#include <Audio/AudioStreamRing.h>
#include <Audio/AudioStreamService.h>
#include <Audio/OpusCodecHeader.h>
#include <AudioOpus/OpusAudioDecoder.h>
#include <AudioTools/AdpcmEncoder.h>
#include <AudioTools/AudioEncoder.h>
#include <AudioTools/AudioPlatformDataWriter.h>
#include <AudioTools/OpusAudioEncoder.h>
#include <Base/InitializerList.h>
#include <Core/Containers/Array.h>
#include <Core/Math/MathUtility.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/Fatal.h>
#include <TestSupport/Test.h>

// AU6a2 스트림 생산자·디코드 서비스 시험이다(Docs/Design/AudioSystem 3.40.2·3.40.4절): 순차 출력이
// 전체 디코드와 bit까지 같음(PCM16·ADPCM·Opus, 청크 여러 개), 시작 frame(PCM16·ADPCM 정확, Opus
// 결정적), loop(이음매 앞은 전체 디코드, 이음매 뒤는 loop 시작에서 새로 연 출력, Opus 전체 loop는
// 정확·중간 loop 이음매는 seek SNR 하한), 손상 청크·Pending 청크, 서비스 슬롯(Inline·Thread — Flush
// gate, 반납 전 재사용 거부, 슬롯 부족).
using namespace rocks;
namespace audio_stream_decoder_tests_file {

using namespace rocks::audio;

namespace {

constexpr uint32 Rate = OpusPlatformSampleRate;
constexpr double TwoPi = 6.283185307179586;
constexpr double ToneAmplitude = 0.3;
// 청크를 여러 개로 나누는 목표 byte다.
constexpr uint32 SmallChunkBytes = 4096;
// seek 근사의 SNR 하한(에너지 비 80 dB — 3.32.4절 측정 약 91 dB)이다.
constexpr double MinimumSeekSignalToNoise = 1.0e8;
constexpr double PerfectSignalToNoise = 1.0e30;

struct Encoded {
    explicit Encoded(Allocator& allocator) noexcept : bytes(allocator) {
    }

    Array<uint8> bytes;
    AudioPlatformDataView view;
};

// 채널마다 다른 주파수의 순음을 codec으로 인코드해 플랫폼 데이터를 만든다.
void Encode(Allocator& allocator, AudioCodecId codec, uint32 channels, uint32 frames, Encoded& out,
            bool requireChunks = true) noexcept {
    Array<float> samples(allocator);
    REQUIRE(samples.TryResize(static_cast<usize>(channels) * frames));
    PlanarConstView view;
    view.channelCount = channels;
    view.frameCount = frames;
    for (uint32 channel = 0; channel < channels; ++channel) {
        float* data = samples.Data() + static_cast<usize>(channel) * frames;
        view.channels[channel] = data;
        for (uint32 frame = 0; frame < frames; ++frame) {
            data[frame] = static_cast<float>(
                ToneAmplitude * math::Sin(TwoPi * (330.0 + 110.0 * channel) * frame / Rate));
        }
    }

    Pcm16AudioEncoder pcm;
    AdpcmAudioEncoder adpcm;
    OpusAudioEncoder opus(allocator);
    AudioEncoder* encoder =
        codec == AudioCodecId::Pcm16
            ? static_cast<AudioEncoder*>(&pcm)
            : (codec == AudioCodecId::Adpcm ? static_cast<AudioEncoder*>(&adpcm)
                                            : static_cast<AudioEncoder*>(&opus));
    EncodedAudio encoded(allocator);
    REQUIRE(encoder->Encode(view, Rate, DefaultAudioChannelMask(channels), {}, encoded));
    AudioPlatformDataOptions options;
    options.targetChunkBytes = SmallChunkBytes;
    REQUIRE(WriteAudioPlatformData(encoded, options, out.bytes));
    auto parsed = AudioPlatformDataView::Parse(out.bytes.View());
    REQUIRE(parsed);
    out.view = parsed.Value();
    // 청크 경계(ContinueChunk) 경로를 지나게 청크가 여러 개여야 한다(짧은 clip은 예외).
    REQUIRE(!requireChunks || out.view.Header().chunkCount > 2);
}

AudioDecoderTable MakeTable() noexcept {
    AudioDecoderTable table;
    if (!RegisterBuiltInAudioDecoders(table) || !RegisterOpusAudioDecoder(table)) {
        platform::Fatal("Audio stream decoder test table failed");
    }

    return table;
}

UniquePtr<AudioStreamDecoder> MakeStream(Allocator& allocator, const AudioDecoderTable& table,
                                         const AudioPlatformDataView& view,
                                         AudioChunkSource& chunks, uint32 startFrame = 0,
                                         bool loop = false, uint32 loopBegin = 0,
                                         uint32 loopEnd = 0) noexcept {
    auto decoder = table.Create(view.Header().codec, allocator);
    if (!decoder) {
        platform::Fatal("Audio stream decoder test codec was not created");
    }

    AudioStreamDecoderDesc desc;
    desc.view = &view;
    desc.chunks = &chunks;
    desc.startFrame = startFrame;
    desc.loop = loop;
    desc.loopBegin = loopBegin;
    desc.loopEnd = loopEnd;
    auto stream = AudioStreamDecoder::TryCreate(allocator, Move(decoder.Value()), desc);
    if (!stream) {
        platform::Fatal("Audio stream decoder test stream was not created");
    }

    return Move(stream.Value());
}

// 링을 채우고 비우기를 반복해 frames frame(또는 끝 상태까지)을 planar로 모은다.
struct Collected {
    explicit Collected(Allocator& allocator) noexcept : samples(allocator) {
    }

    Array<float> samples;
    uint32 channels = 0;
    uint64 frames = 0;
    AudioStreamState state = AudioStreamState::Running;

    float At(uint32 channel, uint64 frame) const noexcept {
        return samples[static_cast<usize>(frame) * channels + channel];
    }
};

void Collect(Allocator& allocator, AudioStreamDecoder& stream, AudioStreamRing& ring, uint64 frames,
             Collected& out, AudioChunkSource* gate = nullptr) noexcept {
    (void)gate;
    out.channels = ring.ChannelCount();
    Array<float> scratch(allocator);
    REQUIRE(scratch.TryResize(static_cast<usize>(ring.CapacityFrames()) * out.channels));
    PlanarView view;
    view.channelCount = out.channels;
    view.frameCount = ring.CapacityFrames();
    for (uint32 channel = 0; channel < out.channels; ++channel) {
        view.channels[channel] = scratch.Data() + static_cast<usize>(channel) * view.frameCount;
    }

    uint32 idle = 0;
    while (out.frames < frames && idle < 4) {
        stream.Pump(ring);
        const uint32 readable = ring.ReadableFrames();
        const uint64 take64 = frames - out.frames < readable ? frames - out.frames : readable;
        const uint32 take = static_cast<uint32>(take64);
        if (take == 0) {
            ++idle;
            if (ring.State() != AudioStreamState::Running) {
                break;
            }

            continue;
        }

        idle = 0;
        ring.Peek(take, view);
        for (uint32 frame = 0; frame < take; ++frame) {
            for (uint32 channel = 0; channel < out.channels; ++channel) {
                CHECK(out.samples.TryPushBack(view.channels[channel][frame]));
            }
        }

        ring.Consume(take);
        out.frames += take;
    }

    out.state = ring.State();
}

UniquePtr<AudioStreamRing> MakeRing(Allocator& allocator, uint32 channels) noexcept {
    auto ring = AudioStreamRing::TryCreate(allocator, channels, StreamRingMinFrames);
    if (!ring) {
        platform::Fatal("Audio stream decoder test ring was not created");
    }

    return Move(ring.Value());
}

// 전체 디코드의 frame 하나다.
float Full(const DecodedAudioPcm& full, uint32 channel, uint64 frame) noexcept {
    return full.Channel(channel)[frame];
}

double SignalToNoise(const DecodedAudioPcm& reference, uint64 referenceStart,
                     const Collected& actual, uint64 actualStart, uint64 frames) noexcept {
    double signal = 0.0;
    double noise = 0.0;
    for (uint32 channel = 0; channel < actual.channels; ++channel) {
        for (uint64 frame = 0; frame < frames; ++frame) {
            const double expected = Full(reference, channel, referenceStart + frame);
            const double difference = actual.At(channel, actualStart + frame) - expected;
            signal += expected * expected;
            noise += difference * difference;
        }
    }

    return noise > 0.0 ? signal / noise : PerfectSignalToNoise;
}

// 청크 index부터 열어 둔 동안 Pending을 내는 공급자다(AU6b 경로 시험).
class GatedChunkSource final : public AudioChunkSource {
public:
    explicit GatedChunkSource(const AudioPlatformDataView& view) noexcept : resident_(view) {
    }

    AudioChunkAcquire TryAcquire(uint32 index) noexcept override {
        if (index >= gateFrom && closed) {
            ++pendingReplies;
            AudioChunkAcquire got;
            got.status = AudioChunkStatus::Pending;
            return got;
        }

        ++acquired;
        return resident_.TryAcquire(index);
    }

    void Release(uint32 index) noexcept override {
        ++released;
        resident_.Release(index);
    }

    void Prefetch(uint32 index) noexcept override {
        resident_.Prefetch(index);
    }

    ResidentAudioChunkSource resident_;
    uint32 gateFrom = 0;
    bool closed = false;
    uint32 pendingReplies = 0;
    uint32 acquired = 0;
    uint32 released = 0;
};

} // namespace

// 순차 출력은 전체 디코드와 bit까지 같다(청크 여러 개, ContinueChunk 경로).
TEST(AudioStream_SequentialMatchesFullDecode) {
    SystemAllocator allocator;
    const AudioDecoderTable table = MakeTable();
    for (const AudioCodecId codec :
         {AudioCodecId::Pcm16, AudioCodecId::Adpcm, AudioCodecId::Opus}) {
        Encoded encoded(allocator);
        Encode(allocator, codec, 2, Rate, encoded);
        DecodedAudioPcm full(allocator);
        REQUIRE(DecodeAudioPlatformData(encoded.view, table, allocator, full));
        ResidentAudioChunkSource chunks(encoded.view);
        UniquePtr<AudioStreamDecoder> stream = MakeStream(allocator, table, encoded.view, chunks);
        UniquePtr<AudioStreamRing> ring = MakeRing(allocator, 2);
        Collected out(allocator);
        Collect(allocator, *stream, *ring, full.FrameCount() + 1, out);
        CHECK(out.frames == full.FrameCount() && out.state == AudioStreamState::Ended);
        bool same = out.frames == full.FrameCount();
        for (uint64 frame = 0; same && frame < out.frames; ++frame) {
            for (uint32 channel = 0; channel < 2; ++channel) {
                same = same && out.At(channel, frame) == Full(full, channel, frame);
            }
        }

        CHECK(same);
        CHECK(stream->IsFinished());
    }
}

// 시작 frame s: PCM16·ADPCM은 전체 디코드의 [s, …)와 같고, Opus는 같은 s를 두 번 시작하면 같으며
// 전체 디코드와 seek SNR 하한 안이다.
TEST(AudioStream_StartFrame) {
    SystemAllocator allocator;
    const AudioDecoderTable table = MakeTable();
    constexpr uint32 Start = 17777;
    for (const AudioCodecId codec :
         {AudioCodecId::Pcm16, AudioCodecId::Adpcm, AudioCodecId::Opus}) {
        Encoded encoded(allocator);
        Encode(allocator, codec, 1, Rate, encoded);
        DecodedAudioPcm full(allocator);
        REQUIRE(DecodeAudioPlatformData(encoded.view, table, allocator, full));
        ResidentAudioChunkSource chunks(encoded.view);
        UniquePtr<AudioStreamDecoder> stream =
            MakeStream(allocator, table, encoded.view, chunks, Start);
        UniquePtr<AudioStreamRing> ring = MakeRing(allocator, 1);
        Collected out(allocator);
        Collect(allocator, *stream, *ring, full.FrameCount(), out);
        CHECK(out.frames == full.FrameCount() - Start && out.state == AudioStreamState::Ended);
        if (codec == AudioCodecId::Opus) {
            UniquePtr<AudioStreamDecoder> again =
                MakeStream(allocator, table, encoded.view, chunks, Start);
            UniquePtr<AudioStreamRing> ringAgain = MakeRing(allocator, 1);
            Collected repeat(allocator);
            Collect(allocator, *again, *ringAgain, full.FrameCount(), repeat);
            bool same = repeat.frames == out.frames;
            for (uint64 frame = 0; same && frame < out.frames; ++frame) {
                same = repeat.At(0, frame) == out.At(0, frame);
            }

            CHECK(same);
            CHECK(SignalToNoise(full, Start, out, 0, out.frames) >= MinimumSeekSignalToNoise);
        } else {
            bool same = true;
            for (uint64 frame = 0; same && frame < out.frames; ++frame) {
                same = out.At(0, frame) == Full(full, 0, Start + frame);
            }

            CHECK(same);
        }
    }
}

// loop: 이음매 앞은 전체 디코드, 이음매 뒤는 loop 시작에서 새로 연(non-loop) 출력과 같다(여러 바퀴,
// 링 경계와 겹치게 길게 모은다). PCM16·ADPCM은 전체 디코드의 loop 구간 반복과 정확히 같다. Opus
// 전체 loop(시작 0)는 바퀴마다 전체 디코드와 같고, 중간 loop 이음매는 seek SNR 하한 안이다.
TEST(AudioStream_LoopSeams) {
    SystemAllocator allocator;
    const AudioDecoderTable table = MakeTable();
    constexpr uint32 LoopBegin = 9000;
    constexpr uint32 LoopEnd = 30000;
    constexpr uint32 Laps = 3;
    for (const AudioCodecId codec :
         {AudioCodecId::Pcm16, AudioCodecId::Adpcm, AudioCodecId::Opus}) {
        Encoded encoded(allocator);
        Encode(allocator, codec, 1, Rate, encoded);
        DecodedAudioPcm full(allocator);
        REQUIRE(DecodeAudioPlatformData(encoded.view, table, allocator, full));
        ResidentAudioChunkSource chunks(encoded.view);
        UniquePtr<AudioStreamDecoder> stream =
            MakeStream(allocator, table, encoded.view, chunks, 0, true, LoopBegin, LoopEnd);
        UniquePtr<AudioStreamRing> ring = MakeRing(allocator, 1);
        const uint64 lap = LoopEnd - LoopBegin;
        const uint64 total = LoopEnd + lap * Laps;
        Collected out(allocator);
        Collect(allocator, *stream, *ring, total, out);
        REQUIRE(out.frames == total);
        // 이음매 앞(처음부터 순차)은 전체 디코드와 같다.
        bool head = true;
        for (uint64 frame = 0; head && frame < LoopEnd; ++frame) {
            head = out.At(0, frame) == Full(full, 0, frame);
        }

        CHECK(head);
        // 이음매 뒤 기준: loop 시작에서 새로 연 출력.
        UniquePtr<AudioStreamDecoder> seek =
            MakeStream(allocator, table, encoded.view, chunks, LoopBegin);
        UniquePtr<AudioStreamRing> seekRing = MakeRing(allocator, 1);
        Collected reference(allocator);
        Collect(allocator, *seek, *seekRing, lap, reference);
        REQUIRE(reference.frames == lap);
        bool laps = true;
        for (uint32 index = 0; laps && index < Laps; ++index) {
            const uint64 base = LoopEnd + lap * index;
            for (uint64 frame = 0; laps && frame < lap; ++frame) {
                laps = out.At(0, base + frame) == reference.At(0, frame);
            }
        }

        CHECK(laps);
        if (codec == AudioCodecId::Opus) {
            CHECK(SignalToNoise(full, LoopBegin, out, LoopEnd, lap) >= MinimumSeekSignalToNoise);
        } else {
            CHECK(SignalToNoise(full, LoopBegin, out, LoopEnd, lap) == PerfectSignalToNoise);
        }
    }

    // Opus 전체 loop(시작 0 — 시작 패킷이 스트림 머리라 정확)는 바퀴마다 전체 디코드와 같다.
    Encoded encoded(allocator);
    Encode(allocator, AudioCodecId::Opus, 2, Rate / 2, encoded);
    DecodedAudioPcm full(allocator);
    REQUIRE(DecodeAudioPlatformData(encoded.view, table, allocator, full));
    ResidentAudioChunkSource chunks(encoded.view);
    UniquePtr<AudioStreamDecoder> stream =
        MakeStream(allocator, table, encoded.view, chunks, 0, true);
    UniquePtr<AudioStreamRing> ring = MakeRing(allocator, 2);
    Collected out(allocator);
    Collect(allocator, *stream, *ring, full.FrameCount() * 3, out);
    REQUIRE(out.frames == full.FrameCount() * 3);
    bool exact = true;
    for (uint64 frame = 0; exact && frame < out.frames; ++frame) {
        for (uint32 channel = 0; channel < 2; ++channel) {
            exact =
                exact && out.At(channel, frame) == Full(full, channel, frame % full.FrameCount());
        }
    }

    CHECK(exact);
}

// 손상 청크(Opus TOC — 검증을 거치지 않은 공급)는 Failed이고 그 뒤로 쓰지 않는다. 잘못된 desc는
// TryCreate가 거부한다.
TEST(AudioStream_FailuresAndRejects) {
    SystemAllocator allocator;
    const AudioDecoderTable table = MakeTable();
    Encoded encoded(allocator);
    Encode(allocator, AudioCodecId::Opus, 1, Rate, encoded);
    // 둘째 청크 첫 패킷 TOC를 10 ms(config 30)로 바꾼다 — 패킷 frame 수가 960이 아니다.
    const AudioChunkEntry second = encoded.view.Chunk(1);
    const usize metaBytes = encoded.bytes.Size() - encoded.view.Header().payloadBytes;
    constexpr uint8 TocCeltFullband10Ms = 30;
    constexpr uint32 TocConfigShift = 3;
    const usize tocAt =
        metaBytes + static_cast<usize>(second.payloadOffset) + OpusLengthPrefixBytes;
    encoded.bytes[tocAt] =
        static_cast<uint8>((TocCeltFullband10Ms << TocConfigShift) | (encoded.bytes[tocAt] & 7u));
    auto parsed = AudioPlatformDataView::Parse(encoded.bytes.View());
    REQUIRE(parsed);
    ResidentAudioChunkSource chunks(parsed.Value());
    UniquePtr<AudioStreamDecoder> stream = MakeStream(allocator, table, parsed.Value(), chunks);
    UniquePtr<AudioStreamRing> ring = MakeRing(allocator, 1);
    Collected out(allocator);
    Collect(allocator, *stream, *ring, parsed.Value().Header().frames, out);
    CHECK(out.state == AudioStreamState::Failed && stream->IsFinished());
    CHECK(out.frames < parsed.Value().Header().frames);
    const uint32 writtenBefore = ring->ReadableFrames();
    stream->Pump(*ring);
    CHECK(ring->ReadableFrames() == writtenBefore);

    // desc 거부: 시작 frame 범위 밖, loop 구간, loop인데 시작 ≥ loop 끝, null.
    const auto reject = [&](AudioStreamDecoderDesc desc) noexcept {
        auto decoder = table.Create(AudioCodecId::Opus, allocator);
        if (!decoder) {
            platform::Fatal("Audio stream decoder test codec was not created");
        }

        return AudioStreamDecoder::TryCreate(allocator, Move(decoder.Value()), desc).Error();
    };

    AudioStreamDecoderDesc desc;
    desc.view = &encoded.view;
    desc.chunks = &chunks;
    desc.startFrame = static_cast<uint32>(encoded.view.Header().frames);
    CHECK(reject(desc) == ErrorCode::InvalidArgument);
    desc.startFrame = 0;
    desc.loopBegin = 10;
    desc.loopEnd = 10;
    CHECK(reject(desc) == ErrorCode::InvalidArgument);
    desc.loopEnd = 100;
    desc.loop = true;
    desc.startFrame = 100;
    CHECK(reject(desc) == ErrorCode::InvalidArgument);
    desc.view = nullptr;
    CHECK(reject(desc) == ErrorCode::InvalidArgument);
}

// Pending 청크: 생산자는 그 Pump에서 멈추고, 열리면 이어 쓴다 — 결과는 전체 디코드와 같다. 받은
// 청크는 모두 놓는다.
TEST(AudioStream_PendingChunkResumes) {
    SystemAllocator allocator;
    const AudioDecoderTable table = MakeTable();
    Encoded encoded(allocator);
    Encode(allocator, AudioCodecId::Adpcm, 1, Rate, encoded);
    DecodedAudioPcm full(allocator);
    REQUIRE(DecodeAudioPlatformData(encoded.view, table, allocator, full));
    GatedChunkSource chunks(encoded.view);
    chunks.gateFrom = 2;
    chunks.closed = true;
    UniquePtr<AudioStreamDecoder> stream = MakeStream(allocator, table, encoded.view, chunks);
    UniquePtr<AudioStreamRing> ring = MakeRing(allocator, 1);
    Collected out(allocator);
    Collect(allocator, *stream, *ring, full.FrameCount(), out);
    CHECK(out.state == AudioStreamState::Running && chunks.pendingReplies > 0);
    const uint64 stalled = out.frames;
    CHECK(stalled > 0 && stalled < full.FrameCount());
    chunks.closed = false;
    Collected rest(allocator);
    Collect(allocator, *stream, *ring, full.FrameCount() - stalled, rest);
    CHECK(rest.state == AudioStreamState::Ended && stalled + rest.frames == full.FrameCount());
    bool same = true;
    for (uint64 frame = 0; same && frame < stalled; ++frame) {
        same = out.At(0, frame) == Full(full, 0, frame);
    }

    for (uint64 frame = 0; same && frame < rest.frames; ++frame) {
        same = rest.At(0, frame) == Full(full, 0, stalled + frame);
    }

    CHECK(same);
    CHECK(chunks.acquired == chunks.released);
}

// 서비스(Inline): 슬롯 잡기 → 시작 → PumpAll로 링이 찬다, 취소 뒤 반납 전 ReleaseSlot은 Busy, 반납
// 뒤에는 회수되고 다시 잡은 슬롯의 링은 비어 있다. 슬롯이 다 차면 CapacityExceeded다.
TEST(AudioStream_ServiceInline) {
    SystemAllocator allocator;
    const AudioDecoderTable table = MakeTable();
    Encoded encoded(allocator);
    Encode(allocator, AudioCodecId::Opus, 2, Rate * 2, encoded);
    AudioStreamServiceConfig config;
    config.maxStreams = 2;
    config.mode = AudioStreamServiceMode::Inline;
    auto made = AudioStreamService::TryCreate(allocator, config);
    REQUIRE(made);
    AudioStreamService& service = *made.Value();
    ResidentAudioChunkSource chunks(encoded.view);
    auto slot = service.AcquireSlot(2);
    REQUIRE(slot);
    UniquePtr<AudioStreamDecoder> stream = MakeStream(allocator, table, encoded.view, chunks);
    REQUIRE(service.StartJob(slot.Value(), stream.Get()));
    CHECK(service.StartJob(slot.Value(), stream.Get()).Error() == ErrorCode::InvalidArgument);
    CHECK(!service.IsReturned(slot.Value()));
    service.PumpAll();
    AudioStreamRing& ring = service.Ring(slot.Value());
    CHECK(ring.ReadableFrames() == ring.CapacityFrames());
    service.CancelJob(slot.Value());
    CHECK(service.ReleaseSlot(slot.Value()).Error() == ErrorCode::Busy);
    service.PumpAll();
    CHECK(service.IsReturned(slot.Value()));
    REQUIRE(service.ReleaseSlot(slot.Value()));
    CHECK(service.AcquiredSlots() == 0);
    auto again = service.AcquireSlot(1);
    REQUIRE(again);
    CHECK(service.Ring(again.Value()).ReadableFrames() == 0 &&
          service.Ring(again.Value()).ChannelCount() == 1);
    REQUIRE(service.AcquireSlot(2));
    CHECK(service.AcquireSlot(2).Error() == ErrorCode::CapacityExceeded);
    CHECK(service.AcquireSlot(3).Error() == ErrorCode::InvalidArgument);
    // 작업 없이 잡은 슬롯은 바로 돌려줄 수 있다.
    CHECK(service.IsReturned(again.Value()));
    REQUIRE(service.ReleaseSlot(again.Value()));
}

// 서비스(Thread): Flush gate로만 순서를 만든다 — Flush 뒤 링이 차 있고, 소비 뒤 refill 신호와
// Flush로 다시 찬다, 취소·Flush 뒤 반납된다, 끝까지 쓴 작업은 스스로 반납된다. Shutdown은 join한다.
TEST(AudioStream_ServiceThread) {
    SystemAllocator allocator;
    const AudioDecoderTable table = MakeTable();
    Encoded encoded(allocator);
    Encode(allocator, AudioCodecId::Opus, 1, Rate * 2, encoded);
    AudioStreamServiceConfig config;
    config.maxStreams = 2;
    auto made = AudioStreamService::TryCreate(allocator, config);
    REQUIRE(made);
    AudioStreamService& service = *made.Value();
    CHECK(service.Flush().Error() == ErrorCode::NotReady);
    REQUIRE(service.Start());
    ResidentAudioChunkSource chunks(encoded.view);
    auto slot = service.AcquireSlot(1);
    REQUIRE(slot);
    UniquePtr<AudioStreamDecoder> stream = MakeStream(allocator, table, encoded.view, chunks);
    REQUIRE(service.StartJob(slot.Value(), stream.Get()));
    REQUIRE(service.Flush());
    AudioStreamRing& ring = service.Ring(slot.Value());
    CHECK(ring.ReadableFrames() == ring.CapacityFrames());
    ring.Consume(ring.CapacityFrames() / 2 + 1);
    service.WakeSignal().Signal();
    REQUIRE(service.Flush());
    CHECK(ring.ReadableFrames() == ring.CapacityFrames());
    service.CancelJob(slot.Value());
    REQUIRE(service.Flush());
    CHECK(service.IsReturned(slot.Value()));
    REQUIRE(service.ReleaseSlot(slot.Value()));

    // 짧은 작업은 끝까지 쓰고 스스로 반납된다(링이 한 번에 다 담는 길이).
    Encoded shortClip(allocator);
    Encode(allocator, AudioCodecId::Pcm16, 1, StreamRingMinFrames / 2, shortClip, false);
    ResidentAudioChunkSource shortChunks(shortClip.view);
    auto shortSlot = service.AcquireSlot(1);
    REQUIRE(shortSlot);
    UniquePtr<AudioStreamDecoder> shortStream =
        MakeStream(allocator, table, shortClip.view, shortChunks);
    REQUIRE(service.StartJob(shortSlot.Value(), shortStream.Get()));
    REQUIRE(service.Flush());
    CHECK(service.Ring(shortSlot.Value()).State() == AudioStreamState::Ended);
    CHECK(service.IsReturned(shortSlot.Value()));
    service.CancelJob(shortSlot.Value());
    REQUIRE(service.ReleaseSlot(shortSlot.Value()));
    service.Shutdown();
    CHECK(service.Flush().Error() == ErrorCode::NotReady);
}

} // namespace audio_stream_decoder_tests_file
