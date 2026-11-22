#include <AudioTools/SourceAudio.h>
#include <Base/InitializerList.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>
#include <TestSupport/Test.h>

using namespace rocks;
namespace source_audio_tests_file {

using namespace rocks::audio;

// WAV·AIFF 원본 디코드 시험이다(Docs/Design/AudioSystem.ko.md 3.25.4절). 파일 byte를 시험 안에서
// 조립하고 표본 값이 정확히 일치하는지 본다.

namespace {

constexpr uint32 Rate = 48000;

// 끝 방향을 고를 수 있는 byte 조립기다.
class Bytes {
public:
    explicit Bytes(Allocator& allocator, bool bigEndian = false) noexcept
        : data_(allocator), bigEndian_(bigEndian) {
    }

    void Tag(const char* tag) noexcept {
        for (uint32 index = 0; index < 4; ++index) {
            Push(static_cast<uint8>(tag[index]));
        }
    }

    void U8(uint32 value) noexcept {
        Push(static_cast<uint8>(value));
    }

    void Unsigned(uint64 value, uint32 count) noexcept {
        for (uint32 index = 0; index < count; ++index) {
            const uint32 shift = bigEndian_ ? (count - 1 - index) * 8 : index * 8;
            Push(static_cast<uint8>((value >> shift) & 0xFF));
        }
    }

    void U16(uint32 value) noexcept {
        Unsigned(value, 2);
    }

    void U32(uint64 value) noexcept {
        Unsigned(value, 4);
    }

    void Raw(Span<const uint8> values) noexcept {
        for (const uint8 value : values) {
            Push(value);
        }
    }

    // 4 byte 크기 자리를 만들고 위치를 돌려준다. Close로 채운다.
    usize OpenSize() noexcept {
        const usize at = data_.Size();
        U32(0);
        return at;
    }

    void CloseSize(usize at, bool pad = true) noexcept {
        const usize size = data_.Size() - at - 4;
        for (uint32 index = 0; index < 4; ++index) {
            const uint32 shift = bigEndian_ ? (3 - index) * 8 : index * 8;
            data_[at + index] = static_cast<uint8>((size >> shift) & 0xFF);
        }

        if (pad && (size & 1u) != 0) {
            Push(0);
        }
    }

    Span<const uint8> View() const noexcept {
        return Span<const uint8>(data_.Data(), data_.Size());
    }

    usize Size() const noexcept {
        return data_.Size();
    }

    uint8& operator[](usize index) noexcept {
        return data_[index];
    }

private:
    void Push(uint8 value) noexcept {
        if (!data_.TryPushBack(value)) {
            platform::Fatal("Source audio test buffer allocation failed");
        }
    }

    Array<uint8> data_;
    bool bigEndian_;
};

struct WavSpec {
    uint16 tag = 1;
    uint32 channels = 1;
    uint32 rate = Rate;
    uint32 bits = 16;
    bool extensible = false;
    uint16 subFormatTag = 1;
    bool bigEndian = false;
    int32 blockAlignOverride = -1;
    // fmt 18 byte(cbSize = 0만 붙음)로 쓴다.
    bool cbSizeOnly = false;
    // EXTENSIBLE인데 fmt를 40 byte보다 짧게(GUID 없이) 쓴다.
    bool shortExtensible = false;
    // GUID Data4 마지막 byte를 바꾼다.
    bool unknownGuid = false;
    // 0이면 bits와 같다.
    uint32 validBits = 0;
};

// fmt·data만 있는 WAV를 만든다. sampleBytes는 교차 표본 byte다.
void BuildWav(Bytes& out, const WavSpec& spec, Span<const uint8> sampleBytes) noexcept {
    out.Tag(spec.bigEndian ? "RIFX" : "RIFF");
    const usize riff = out.OpenSize();
    out.Tag("WAVE");
    out.Tag("fmt ");
    const usize fmt = out.OpenSize();
    out.U16(spec.extensible ? 0xFFFE : spec.tag);
    out.U16(spec.channels);
    out.U32(spec.rate);
    const uint32 blockAlign = spec.blockAlignOverride >= 0
                                  ? static_cast<uint32>(spec.blockAlignOverride)
                                  : spec.channels * spec.bits / 8;
    out.U32(spec.rate * blockAlign);
    out.U16(blockAlign);
    out.U16(spec.bits);
    if (spec.extensible) {
        out.U16(22);
        out.U16(spec.validBits != 0 ? spec.validBits : spec.bits);
        if (!spec.shortExtensible) {
            out.U32(0);
            // GUID {0000tttt-0000-0010-8000-00AA00389B71}: Data1·2·3은 파일 끝 방향, Data4는 byte
            // 그대로.
            out.U32(spec.subFormatTag);
            out.U16(0);
            out.U16(0x0010);
            const uint8 data4[8] = {
                0x80, 0x00, 0x00, 0xAA,
                0x00, 0x38, 0x9B, static_cast<uint8>(spec.unknownGuid ? 0x72 : 0x71)};
            out.Raw(Span<const uint8>(data4));
        }
    } else if (spec.cbSizeOnly) {
        out.U16(0);
    }

    out.CloseSize(fmt);
    out.Tag("data");
    const usize data = out.OpenSize();
    out.Raw(sampleBytes);
    out.CloseSize(data);
    out.CloseSize(riff, false);
}

// 큰 끝 방향 byte로 값을 쓴다(시험 표본 조립용).
template <usize Count>
void StoreBigEndian(uint8 (&out)[Count], usize at, uint64 value, uint32 count) noexcept {
    for (uint32 index = 0; index < count; ++index) {
        out[at + index] = static_cast<uint8>(value >> ((count - 1 - index) * 8));
    }
}

DecodedSourceAudio DecodeOrDie(Span<const uint8> file, Allocator& allocator,
                               bool aiff = false) noexcept {
    DecodedSourceAudio audio(allocator);
    const Result<void> decoded = aiff ? DecodeAiff(file, audio) : DecodeWav(file, audio);
    if (!decoded) {
        platform::Fatal("Source audio test file was rejected");
    }

    return audio;
}

ErrorCode WavError(Span<const uint8> file, Allocator& allocator) noexcept {
    DecodedSourceAudio audio(allocator);
    return DecodeWav(file, audio).Error();
}

// 메타데이터 chunk가 있는 WAV를 만든다(끝 방향을 고른다). frame 100개, smpl loop 셋(개수 필드는 5 —
// 본문보다 큼), cue 셋(개수 필드는 4), adtl labl 이름 셋(NUL 끝·NUL 없음·잘못된 UTF-8).
void BuildChunkedWav(Bytes& file, bool bigEndian) noexcept {
    file.Tag(bigEndian ? "RIFX" : "RIFF");
    const usize riff = file.OpenSize();
    file.Tag("WAVE");
    // data가 fmt보다 먼저 와도 되고, 홀수 크기 chunk는 pad 1 byte가 붙는다.
    file.Tag("junk");
    const usize junk = file.OpenSize();
    file.U8(7);
    file.CloseSize(junk);
    file.Tag("data");
    const usize data = file.OpenSize();
    for (uint32 frame = 0; frame < 100; ++frame) {
        file.U16(frame * 100);
    }

    file.U8(0xAB); // blockAlign의 배수가 아닌 남는 byte는 버린다.
    file.CloseSize(data);
    file.Tag("fmt ");
    const usize fmt = file.OpenSize();
    file.U16(1);
    file.U16(1);
    file.U32(Rate);
    file.U32(Rate * 2);
    file.U16(2);
    file.U16(16);
    file.CloseSize(fmt);
    // 두 번째 fmt·data는 무시하고 센다.
    file.Tag("fmt ");
    const usize duplicate = file.OpenSize();
    file.U32(0);
    file.CloseSize(duplicate);
    file.Tag("data");
    const usize duplicateData = file.OpenSize();
    file.U32(0x7FFF7FFF);
    file.CloseSize(duplicateData);
    // smpl: end(포함) 49 → 50, 99 → 100(= frame 수, 받음), 100 → 101(범위 밖).
    file.Tag("smpl");
    const usize smpl = file.OpenSize();
    for (uint32 index = 0; index < 7; ++index) {
        file.U32(0);
    }

    file.U32(5);
    file.U32(0);
    for (const uint32 end : {49u, 99u, 100u}) {
        file.U32(1);
        file.U32(0);
        file.U32(10);
        file.U32(end);
        file.U32(0);
        file.U32(0);
    }

    file.CloseSize(smpl);
    // cue: dwPosition = id × 10(100은 frame 수와 같아 받는다). sampleOffset은 다른 값이라 어느 쪽을
    // 읽는지 가린다.
    file.Tag("cue ");
    const usize cue = file.OpenSize();
    file.U32(4);
    for (const uint32 id : {5u, 9u, 10u}) {
        file.U32(id);
        file.U32(id * 10);
        file.Tag("data");
        file.U32(0);
        file.U32(0);
        file.U32(1);
    }

    file.CloseSize(cue);
    file.Tag("LIST");
    const usize list = file.OpenSize();
    file.Tag("adtl");
    const auto label = [&](uint32 id, Span<const uint8> text) {
        file.Tag("labl");
        const usize at = file.OpenSize();
        file.U32(id);
        file.Raw(text);
        file.CloseSize(at);
    };

    const uint8 hit[] = {0x48, 0x69, 0x74, 0x00, 0x78};
    const uint8 go[] = {0x47, 0x6F};
    const uint8 invalid[] = {0xFF, 0xFE};
    label(9, Span<const uint8>(hit));
    label(10, Span<const uint8>(go));
    label(5, Span<const uint8>(invalid));
    file.CloseSize(list);
    file.CloseSize(riff, false);
}

ErrorCode AiffError(Span<const uint8> file, Allocator& allocator) noexcept {
    DecodedSourceAudio audio(allocator);
    return DecodeAiff(file, audio).Error();
}

// 80비트 extended rate(10 byte)다.
constexpr usize ExtendedRateBytes = 10;

struct AiffSpec {
    const char* form = "AIFF";
    const char* compression = nullptr;
    uint32 bits = 16;
    uint32 frames = 1;
    uint32 channels = 1;
    // 기본은 44100(0x400E AC44 0000 0000 0000).
    uint8 rate[ExtendedRateBytes] = {0x40, 0x0E, 0xAC, 0x44, 0, 0, 0, 0, 0, 0};
    uint32 ssndOffset = 0;
    bool ssnd = true;
};

void SetRate(AiffSpec& spec, const uint8 (&rate)[ExtendedRateBytes]) noexcept {
    for (usize index = 0; index < ExtendedRateBytes; ++index) {
        spec.rate[index] = rate[index];
    }
}

void BuildAiff(Bytes& file, const AiffSpec& spec, Span<const uint8> samples) noexcept {
    file.Tag("FORM");
    const usize formSize = file.OpenSize();
    file.Tag(spec.form);
    file.Tag("COMM");
    const usize comm = file.OpenSize();
    file.U16(spec.channels);
    file.U32(spec.frames);
    file.U16(spec.bits);
    file.Raw(Span<const uint8>(spec.rate));
    if (spec.compression != nullptr) {
        file.Tag(spec.compression);
        file.U8(0);
        file.U8(0);
    }

    file.CloseSize(comm);
    if (spec.ssnd) {
        file.Tag("SSND");
        const usize ssnd = file.OpenSize();
        file.U32(spec.ssndOffset);
        file.U32(0);
        for (uint32 index = 0; index < spec.ssndOffset; ++index) {
            file.U8(0xEE);
        }

        file.Raw(samples);
        file.CloseSize(ssnd);
    }

    file.CloseSize(formSize, false);
}

} // namespace

TEST(Audio_Source_WavIntegerEncodings) {
    SystemAllocator allocator;
    // 8비트(부호 없음): 0 → −1, 128 → 0, 255 → 127/128.
    {
        const uint8 samples[] = {0, 128, 255};
        Bytes file(allocator);
        WavSpec spec;
        spec.bits = 8;
        BuildWav(file, spec, Span<const uint8>(samples));
        const DecodedSourceAudio audio = DecodeOrDie(file.View(), allocator);
        CHECK(audio.FrameCount() == 3 && audio.SampleFormat() == SourceSampleFormat::UInt8);
        CHECK(audio.Channel(0)[0] == -1.0f && audio.Channel(0)[1] == 0.0f &&
              audio.Channel(0)[2] == 127.0f / 128.0f);
    }

    // 16비트 스테레오 LE: 교차 → planar. fmt 18 byte(cbSize = 0)도 받는다.
    {
        const uint8 samples[] = {0x00, 0x80, 0xFF, 0x7F, 0x01, 0x00, 0xFF, 0xFF};
        Bytes file(allocator);
        WavSpec spec;
        spec.channels = 2;
        spec.cbSizeOnly = true;
        BuildWav(file, spec, Span<const uint8>(samples));
        const DecodedSourceAudio audio = DecodeOrDie(file.View(), allocator);
        CHECK(audio.ChannelCount() == 2 && audio.FrameCount() == 2 && audio.SampleRate() == Rate);
        CHECK(audio.Channel(0)[0] == -1.0f && audio.Channel(1)[0] == 32767.0f / 32768.0f);
        CHECK(audio.Channel(0)[1] == 1.0f / 32768.0f && audio.Channel(1)[1] == -1.0f / 32768.0f);
    }

    // 24비트와 32비트.
    {
        const uint8 samples[] = {0x00, 0x00, 0x80, 0x01, 0x00, 0x00};
        Bytes file(allocator);
        WavSpec spec;
        spec.bits = 24;
        BuildWav(file, spec, Span<const uint8>(samples));
        const DecodedSourceAudio audio = DecodeOrDie(file.View(), allocator);
        CHECK(audio.SampleFormat() == SourceSampleFormat::Int24);
        CHECK(audio.Channel(0)[0] == -1.0f && audio.Channel(0)[1] == 1.0f / 8388608.0f);
    }

    {
        const uint8 samples[] = {0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x40};
        Bytes file(allocator);
        WavSpec spec;
        spec.bits = 32;
        BuildWav(file, spec, Span<const uint8>(samples));
        const DecodedSourceAudio audio = DecodeOrDie(file.View(), allocator);
        CHECK(audio.SampleFormat() == SourceSampleFormat::Int32);
        CHECK(audio.Channel(0)[0] == -1.0f && audio.Channel(0)[1] == 0.5f);
    }
}

TEST(Audio_Source_WavFloatExtensibleAndRifx) {
    SystemAllocator allocator;
    // float32·float64.
    {
        const float values[] = {0.25f, -0.75f};
        Bytes file(allocator);
        WavSpec spec;
        spec.tag = 3;
        spec.bits = 32;
        BuildWav(file, spec,
                 Span<const uint8>(reinterpret_cast<const uint8*>(values), sizeof(values)));
        const DecodedSourceAudio audio = DecodeOrDie(file.View(), allocator);
        CHECK(audio.SampleFormat() == SourceSampleFormat::Float32);
        CHECK(audio.Channel(0)[0] == 0.25f && audio.Channel(0)[1] == -0.75f);
    }

    {
        const double values[] = {0.125, -0.5};
        Bytes file(allocator);
        WavSpec spec;
        spec.tag = 3;
        spec.bits = 64;
        BuildWav(file, spec,
                 Span<const uint8>(reinterpret_cast<const uint8*>(values), sizeof(values)));
        const DecodedSourceAudio audio = DecodeOrDie(file.View(), allocator);
        CHECK(audio.SampleFormat() == SourceSampleFormat::Float64);
        CHECK(audio.Channel(0)[0] == 0.125f && audio.Channel(0)[1] == -0.5f);
    }

    // EXTENSIBLE: PCM GUID와 float GUID, 8채널, LE(RIFF)와 BE(RIFX).
    for (const bool bigEndian : {false, true}) {
        for (const uint16 subFormat : {uint16{1}, uint16{3}}) {
            Array<uint8> samples(allocator);
            REQUIRE(samples.TryResize(8 * 4));
            for (uint32 channel = 0; channel < 8; ++channel) {
                const uint32 bits =
                    subFormat == 1 ? static_cast<uint32>((channel + 1) * 1000)
                                   : platform::BitCast<uint32>(static_cast<float>(channel) * 0.1f);
                for (uint32 byte = 0; byte < 4; ++byte) {
                    const uint32 shift = bigEndian ? (3 - byte) * 8 : byte * 8;
                    samples[channel * 4 + byte] = static_cast<uint8>(bits >> shift);
                }
            }

            Bytes file(allocator, bigEndian);
            WavSpec spec;
            spec.bigEndian = bigEndian;
            spec.extensible = true;
            spec.subFormatTag = subFormat;
            spec.channels = 8;
            spec.bits = 32;
            BuildWav(file, spec, Span<const uint8>(samples.Data(), samples.Size()));
            const DecodedSourceAudio audio = DecodeOrDie(file.View(), allocator);
            REQUIRE(audio.ChannelCount() == 8 && audio.FrameCount() == 1);
            for (uint32 channel = 0; channel < 8; ++channel) {
                const float expected =
                    subFormat == 1 ? static_cast<float>((channel + 1) * 1000) / 2147483648.0f
                                   : static_cast<float>(channel) * 0.1f;
                CHECK(audio.Channel(channel)[0] == expected);
            }
        }
    }

    // RIFX(BE): 16비트 0x4000 = 0.5, 24비트, float32.
    {
        const uint8 samples[] = {0x40, 0x00, 0xC0, 0x00};
        Bytes file(allocator, true);
        WavSpec spec;
        spec.bigEndian = true;
        BuildWav(file, spec, Span<const uint8>(samples));
        const DecodedSourceAudio audio = DecodeOrDie(file.View(), allocator);
        CHECK(audio.Channel(0)[0] == 0.5f && audio.Channel(0)[1] == -0.5f);
    }

    {
        const uint8 samples[] = {0x80, 0x00, 0x00, 0x00, 0x00, 0x01};
        Bytes file(allocator, true);
        WavSpec spec;
        spec.bigEndian = true;
        spec.bits = 24;
        BuildWav(file, spec, Span<const uint8>(samples));
        const DecodedSourceAudio audio = DecodeOrDie(file.View(), allocator);
        CHECK(audio.Channel(0)[0] == -1.0f && audio.Channel(0)[1] == 1.0f / 8388608.0f);
    }

    {
        uint8 samples[4] = {};
        StoreBigEndian(samples, 0, platform::BitCast<uint32>(0.25f), 4);
        Bytes file(allocator, true);
        WavSpec spec;
        spec.bigEndian = true;
        spec.tag = 3;
        spec.bits = 32;
        BuildWav(file, spec, Span<const uint8>(samples));
        const DecodedSourceAudio audio = DecodeOrDie(file.View(), allocator);
        CHECK(audio.Channel(0)[0] == 0.25f && audio.SampleFormat() == SourceSampleFormat::Float32);
    }
}

TEST(Audio_Source_WavChunksLoopsAndCues) {
    SystemAllocator allocator;
    for (const bool bigEndian : {false, true}) {
        Bytes file(allocator, bigEndian);
        BuildChunkedWav(file, bigEndian);
        const DecodedSourceAudio audio = DecodeOrDie(file.View(), allocator);
        CHECK(audio.FrameCount() == 100 && audio.Channel(0)[3] == 300.0f / 32768.0f);
        // 개수 필드(5)가 본문(3)보다 커도 본문에 있는 것만 읽고, 범위 밖 하나는 버린다.
        REQUIRE(audio.Loops().Size() == 2);
        CHECK(audio.Loops()[0].start == 10 && audio.Loops()[0].end == 50);
        CHECK(audio.Loops()[1].start == 10 && audio.Loops()[1].end == 100);
        REQUIRE(audio.Cues().Size() == 3);
        // 위치는 dwPosition이다. 잘못된 UTF-8 이름은 빈 이름, 첫 NUL 뒤는 뗀다, NUL이 없어도
        // 받는다.
        CHECK(audio.Cues()[0].id == 5 && audio.Cues()[0].frame == 50 &&
              audio.Cues()[0].name.IsEmpty());
        CHECK(audio.Cues()[1].frame == 90 && audio.Cues()[1].name.View() == StringView("Hit"));
        CHECK(audio.Cues()[2].frame == 100 && audio.Cues()[2].name.View() == StringView("Go"));
        // junk, 중복 fmt, 중복 data.
        CHECK(audio.IgnoredChunks() == 3);
    }
}

TEST(Audio_Source_WavToleratesTruncation) {
    SystemAllocator allocator;
    const uint8 samples[] = {0x00, 0x40, 0x00, 0xC0};
    // RIFF 크기가 파일보다 커도(스트리밍 기록기의 0xFFFFFFFF) 파일 끝까지 읽는다.
    {
        Bytes file(allocator);
        BuildWav(file, WavSpec{}, Span<const uint8>(samples));
        for (uint32 index = 4; index < 8; ++index) {
            file[index] = 0xFF;
        }

        const DecodedSourceAudio audio = DecodeOrDie(file.View(), allocator);
        CHECK(audio.FrameCount() == 2 && audio.Channel(0)[1] == -0.5f);
    }

    // data가 파일 끝을 넘으면 끝까지로 자르고, 남는 byte는 버린다.
    {
        Bytes file(allocator);
        BuildWav(file, WavSpec{}, Span<const uint8>(samples));
        const DecodedSourceAudio audio = DecodeOrDie(file.View().Left(file.Size() - 1), allocator);
        CHECK(audio.FrameCount() == 1 && audio.Channel(0)[0] == 0.5f);
    }

    // data 뒤 메타데이터 chunk가 끝을 넘으면 거기서 멈추고 센다. PCM은 그대로다.
    {
        Bytes file(allocator);
        BuildWav(file, WavSpec{}, Span<const uint8>(samples));
        file.Tag("smpl");
        file.U32(1000);
        file.U32(0);
        const usize size = file.Size() - 8;
        for (uint32 index = 0; index < 4; ++index) {
            file[4 + index] = static_cast<uint8>(size >> (index * 8));
        }

        const DecodedSourceAudio audio = DecodeOrDie(file.View(), allocator);
        CHECK(audio.FrameCount() == 2 && audio.Loops().Size() == 0 && audio.IgnoredChunks() == 1);
    }

    // LIST 안 labl이 LIST 끝을 넘으면 이름 없이 멈추고 센다.
    {
        Bytes file(allocator);
        file.Tag("RIFF");
        const usize riff = file.OpenSize();
        file.Tag("WAVE");
        file.Tag("fmt ");
        const usize fmt = file.OpenSize();
        file.U16(1);
        file.U16(1);
        file.U32(Rate);
        file.U32(Rate * 2);
        file.U16(2);
        file.U16(16);
        file.CloseSize(fmt);
        file.Tag("data");
        const usize data = file.OpenSize();
        file.Raw(Span<const uint8>(samples));
        file.CloseSize(data);
        file.Tag("cue ");
        const usize cue = file.OpenSize();
        file.U32(1);
        file.U32(3);
        file.U32(1);
        file.Tag("data");
        file.U32(0);
        file.U32(0);
        file.U32(0);
        file.CloseSize(cue);
        file.Tag("LIST");
        const usize list = file.OpenSize();
        file.Tag("adtl");
        file.Tag("labl");
        file.U32(64);
        file.U32(3);
        file.Tag("Name");
        file.CloseSize(list);
        file.CloseSize(riff, false);
        const DecodedSourceAudio audio = DecodeOrDie(file.View(), allocator);
        REQUIRE(audio.Cues().Size() == 1);
        CHECK(audio.Cues()[0].frame == 1 && audio.Cues()[0].name.IsEmpty());
        CHECK(audio.IgnoredChunks() == 1);
    }
}

TEST(Audio_Source_WavRejectsInvalid) {
    SystemAllocator allocator;
    const uint8 samples[] = {0, 0, 0, 0};
    // 압축 부호화(ADPCM tag 2)·µ-law는 Unsupported.
    for (const uint16 tag : {uint16{2}, uint16{7}}) {
        Bytes file(allocator);
        WavSpec spec;
        spec.tag = tag;
        BuildWav(file, spec, Span<const uint8>(samples));
        CHECK(WavError(file.View(), allocator) == ErrorCode::Unsupported);
    }

    WavSpec nine;
    nine.channels = 9;
    nine.bits = 8;
    Bytes nineFile(allocator);
    const uint8 nineSamples[9] = {};
    BuildWav(nineFile, nine, Span<const uint8>(nineSamples));
    CHECK(WavError(nineFile.View(), allocator) == ErrorCode::Unsupported);
    for (const uint32 rate : {MinSourceSampleRate - 1, MaxSourceSampleRate + 1}) {
        WavSpec spec;
        spec.rate = rate;
        Bytes file(allocator);
        BuildWav(file, spec, Span<const uint8>(samples));
        CHECK(WavError(file.View(), allocator) == ErrorCode::Unsupported);
    }

    // 지원하지 않는 bit 수(12비트 PCM, 16비트 float).
    for (const uint16 tag : {uint16{1}, uint16{3}}) {
        WavSpec spec;
        spec.tag = tag;
        spec.bits = tag == 1 ? 12 : 16;
        spec.blockAlignOverride = 2;
        Bytes file(allocator);
        BuildWav(file, spec, Span<const uint8>(samples));
        CHECK(WavError(file.View(), allocator) == ErrorCode::Unsupported);
    }

    WavSpec misaligned;
    misaligned.blockAlignOverride = 3;
    Bytes misalignedFile(allocator);
    BuildWav(misalignedFile, misaligned, Span<const uint8>(samples));
    CHECK(WavError(misalignedFile.View(), allocator) == ErrorCode::InvalidArgument);

    // EXTENSIBLE: fmt가 40 byte보다 짧음, 모르는 GUID, validBits > bits.
    {
        WavSpec spec;
        spec.extensible = true;
        spec.shortExtensible = true;
        Bytes file(allocator);
        BuildWav(file, spec, Span<const uint8>(samples));
        CHECK(WavError(file.View(), allocator) == ErrorCode::InvalidArgument);
    }

    {
        WavSpec spec;
        spec.extensible = true;
        spec.unknownGuid = true;
        Bytes file(allocator);
        BuildWav(file, spec, Span<const uint8>(samples));
        CHECK(WavError(file.View(), allocator) == ErrorCode::Unsupported);
    }

    {
        WavSpec spec;
        spec.extensible = true;
        spec.validBits = 20;
        Bytes file(allocator);
        BuildWav(file, spec, Span<const uint8>(samples));
        CHECK(WavError(file.View(), allocator) == ErrorCode::InvalidArgument);
    }

    // float 표본의 NaN·무한대와 float32로 바꾸면 넘치는 float64는 InvalidArgument.
    for (const uint32 bits : {0x7FC00000u, 0x7F800000u, 0xFF800000u}) {
        const uint32 values[] = {0, bits};
        WavSpec spec;
        spec.tag = 3;
        spec.bits = 32;
        Bytes file(allocator);
        BuildWav(file, spec,
                 Span<const uint8>(reinterpret_cast<const uint8*>(values), sizeof(values)));
        CHECK(WavError(file.View(), allocator) == ErrorCode::InvalidArgument);
    }

    {
        const double values[] = {1.0e300};
        WavSpec spec;
        spec.tag = 3;
        spec.bits = 64;
        Bytes file(allocator);
        BuildWav(file, spec,
                 Span<const uint8>(reinterpret_cast<const uint8*>(values), sizeof(values)));
        CHECK(WavError(file.View(), allocator) == ErrorCode::InvalidArgument);
    }

    // 빈 data, fmt 없음(fmt chunk 중간에서 파일이 끝남), 머리 없음.
    Bytes empty(allocator);
    BuildWav(empty, WavSpec{}, Span<const uint8>());
    CHECK(WavError(empty.View(), allocator) == ErrorCode::InvalidArgument);
    Bytes good(allocator);
    BuildWav(good, WavSpec{}, Span<const uint8>(samples));
    CHECK(WavError(good.View().Left(30), allocator) == ErrorCode::InvalidArgument);
    CHECK(WavError(Span<const uint8>(samples), allocator) == ErrorCode::InvalidArgument);

    // PCM 상한: float32 기준 MaxSoundClipPcmBytes를 넘는 크기는 할당 전에 CapacityExceeded다. WAV는
    // data 크기로 frame 수를 얻으므로 파일로 이 경로를 타려면 512 MiB 넘는 입력이 든다 — 파일
    // 경로는 AIFF가 COMM으로 시험한다(Audio_Source_AiffRejectsInvalid).
    DecodedSourceAudio audio(allocator);
    const uint64 overLimit = MaxSoundClipPcmBytes / (8 * sizeof(float)) + 1;
    CHECK(audio.TryAllocatePcm(8, Rate, overLimit, SourceSampleFormat::UInt8, SourceCodec::Pcm)
              .Error() == ErrorCode::CapacityExceeded);
    CHECK(audio.FrameCount() == 0);
}

TEST(Audio_Source_FailureResetsOutput) {
    SystemAllocator allocator;
    DecodedSourceAudio audio(allocator);
    Bytes chunked(allocator);
    BuildChunkedWav(chunked, false);
    REQUIRE(DecodeWav(chunked.View(), audio));
    REQUIRE(audio.Loops().Size() == 2 && audio.Cues().Size() == 3);

    // PCM 할당 뒤에 실패해도(NaN 표본) 부분 결과를 남기지 않는다.
    const uint32 values[] = {0, 0x7FC00000u};
    WavSpec spec;
    spec.tag = 3;
    spec.bits = 32;
    Bytes nan(allocator);
    BuildWav(nan, spec, Span<const uint8>(reinterpret_cast<const uint8*>(values), sizeof(values)));
    CHECK(DecodeWav(nan.View(), audio).Error() == ErrorCode::InvalidArgument);
    CHECK(audio.ChannelCount() == 0 && audio.FrameCount() == 0 && audio.SampleRate() == 0);
    CHECK(audio.Loops().Size() == 0 && audio.Cues().Size() == 0 && audio.IgnoredChunks() == 0);

    REQUIRE(DecodeWav(chunked.View(), audio));
    const uint8 samples[] = {0, 0};
    AiffSpec aiff;
    aiff.frames = 4;
    Bytes shortData(allocator, true);
    BuildAiff(shortData, aiff, Span<const uint8>(samples));
    CHECK(DecodeAiff(shortData.View(), audio).Error() == ErrorCode::InvalidArgument);
    CHECK(audio.FrameCount() == 0 && audio.Cues().Size() == 0 && audio.IgnoredChunks() == 0);
}

TEST(Audio_Source_Aiff) {
    SystemAllocator allocator;
    // AIFF 16비트 BE, rate 44100.
    {
        const uint8 samples[] = {0x40, 0x00, 0x80, 0x00};
        AiffSpec spec;
        spec.frames = 2;
        Bytes file(allocator, true);
        BuildAiff(file, spec, Span<const uint8>(samples));
        const DecodedSourceAudio audio = DecodeOrDie(file.View(), allocator, true);
        CHECK(audio.SampleRate() == 44100 && audio.FrameCount() == 2);
        CHECK(audio.Channel(0)[0] == 0.5f && audio.Channel(0)[1] == -1.0f);
        CHECK(audio.SampleFormat() == SourceSampleFormat::Int16);
    }

    // AIFC sowt(LE), rate 48000.
    {
        const uint8 samples[] = {0x00, 0x40, 0x00, 0x80};
        AiffSpec spec;
        spec.form = "AIFC";
        spec.compression = "sowt";
        spec.frames = 2;
        SetRate(spec, {0x40, 0x0E, 0xBB, 0x80, 0, 0, 0, 0, 0, 0});
        Bytes file(allocator, true);
        BuildAiff(file, spec, Span<const uint8>(samples));
        const DecodedSourceAudio audio = DecodeOrDie(file.View(), allocator, true);
        CHECK(audio.SampleRate() == 48000 && audio.Channel(0)[0] == 0.5f &&
              audio.Channel(0)[1] == -1.0f);
    }

    // AIFC fl32·FL32(BE float), rate 22050.
    for (const char* type : {"fl32", "FL32"}) {
        uint8 samples[4] = {};
        StoreBigEndian(samples, 0, platform::BitCast<uint32>(-0.25f), 4);
        AiffSpec spec;
        spec.form = "AIFC";
        spec.compression = type;
        spec.bits = 32;
        SetRate(spec, {0x40, 0x0D, 0xAC, 0x44, 0, 0, 0, 0, 0, 0});
        Bytes file(allocator, true);
        BuildAiff(file, spec, Span<const uint8>(samples));
        const DecodedSourceAudio audio = DecodeOrDie(file.View(), allocator, true);
        CHECK(audio.SampleRate() == 22050 && audio.Channel(0)[0] == -0.25f);
        CHECK(audio.SampleFormat() == SourceSampleFormat::Float32);
    }

    // 8비트는 부호 있음: 0x80 = −1, 0x40 = 0.5. 저장 형식은 Int16으로 넓혀 보고한다.
    {
        const uint8 samples[] = {0x80, 0x40};
        AiffSpec spec;
        spec.bits = 8;
        spec.frames = 2;
        Bytes file(allocator, true);
        BuildAiff(file, spec, Span<const uint8>(samples));
        const DecodedSourceAudio audio = DecodeOrDie(file.View(), allocator, true);
        CHECK(audio.Channel(0)[0] == -1.0f && audio.Channel(0)[1] == 0.5f);
        CHECK(audio.SampleFormat() == SourceSampleFormat::Int16);
    }

    // 24비트 스테레오 교차, SSND offset ≠ 0.
    {
        const uint8 samples[] = {0x80, 0x00, 0x00, 0x00, 0x00, 0x01};
        AiffSpec spec;
        spec.bits = 24;
        spec.channels = 2;
        spec.ssndOffset = 3;
        Bytes file(allocator, true);
        BuildAiff(file, spec, Span<const uint8>(samples));
        const DecodedSourceAudio audio = DecodeOrDie(file.View(), allocator, true);
        CHECK(audio.ChannelCount() == 2 && audio.FrameCount() == 1);
        CHECK(audio.Channel(0)[0] == -1.0f && audio.Channel(1)[0] == 1.0f / 8388608.0f);
        CHECK(audio.SampleFormat() == SourceSampleFormat::Int24);
    }

    {
        uint8 samples[4] = {};
        StoreBigEndian(samples, 0, 0x40000000u, 4);
        AiffSpec spec;
        spec.bits = 32;
        Bytes file(allocator, true);
        BuildAiff(file, spec, Span<const uint8>(samples));
        const DecodedSourceAudio audio = DecodeOrDie(file.View(), allocator, true);
        CHECK(audio.Channel(0)[0] == 0.5f && audio.SampleFormat() == SourceSampleFormat::Int32);
    }

    // 12비트·20비트는 왼쪽 맞춤 2·3 byte다.
    {
        const uint8 samples[] = {0x40, 0x00, 0x80, 0x00};
        AiffSpec spec;
        spec.bits = 12;
        spec.frames = 2;
        Bytes file(allocator, true);
        BuildAiff(file, spec, Span<const uint8>(samples));
        const DecodedSourceAudio audio = DecodeOrDie(file.View(), allocator, true);
        CHECK(audio.Channel(0)[0] == 0.5f && audio.Channel(0)[1] == -1.0f);
        CHECK(audio.SampleFormat() == SourceSampleFormat::Int16);
    }

    {
        const uint8 samples[] = {0xC0, 0x00, 0x00};
        AiffSpec spec;
        spec.bits = 20;
        Bytes file(allocator, true);
        BuildAiff(file, spec, Span<const uint8>(samples));
        const DecodedSourceAudio audio = DecodeOrDie(file.View(), allocator, true);
        CHECK(audio.Channel(0)[0] == -0.5f && audio.SampleFormat() == SourceSampleFormat::Int24);
    }

    // FORM 크기가 파일보다 크고 SSND가 끝을 넘어도 frame 수만큼 있으면 받는다.
    {
        const uint8 samples[] = {0x40, 0x00, 0x20, 0x00, 0x10, 0x00};
        AiffSpec spec;
        spec.frames = 2;
        Bytes file(allocator, true);
        BuildAiff(file, spec, Span<const uint8>(samples));
        const DecodedSourceAudio audio =
            DecodeOrDie(file.View().Left(file.Size() - 2), allocator, true);
        CHECK(audio.FrameCount() == 2 && audio.Channel(0)[1] == 0.25f);
    }
}

TEST(Audio_Source_AiffRejectsInvalid) {
    SystemAllocator allocator;
    const uint8 samples[] = {0, 0};
    // 모르는 압축, 소수 rate, rate 범위 밖(4000), 9채널, 33비트.
    {
        AiffSpec spec;
        spec.form = "AIFC";
        spec.compression = "ima4";
        Bytes file(allocator, true);
        BuildAiff(file, spec, Span<const uint8>(samples));
        CHECK(AiffError(file.View(), allocator) == ErrorCode::Unsupported);
    }

    {
        AiffSpec spec;
        spec.rate[4] = 0x80;
        Bytes file(allocator, true);
        BuildAiff(file, spec, Span<const uint8>(samples));
        CHECK(AiffError(file.View(), allocator) == ErrorCode::Unsupported);
    }

    {
        AiffSpec spec;
        SetRate(spec, {0x40, 0x0A, 0xFA, 0x00, 0, 0, 0, 0, 0, 0});
        Bytes file(allocator, true);
        BuildAiff(file, spec, Span<const uint8>(samples));
        CHECK(AiffError(file.View(), allocator) == ErrorCode::Unsupported);
    }

    {
        const uint8 nineSamples[9] = {};
        AiffSpec spec;
        spec.bits = 8;
        spec.channels = 9;
        Bytes file(allocator, true);
        BuildAiff(file, spec, Span<const uint8>(nineSamples));
        CHECK(AiffError(file.View(), allocator) == ErrorCode::Unsupported);
    }

    {
        AiffSpec spec;
        spec.bits = 33;
        Bytes file(allocator, true);
        BuildAiff(file, spec, Span<const uint8>(samples));
        CHECK(AiffError(file.View(), allocator) == ErrorCode::Unsupported);
    }

    // 짧은 SSND, SSND offset이 본문보다 큼, SSND 없음, frame 0.
    {
        AiffSpec spec;
        spec.frames = 4;
        Bytes file(allocator, true);
        BuildAiff(file, spec, Span<const uint8>(samples));
        CHECK(AiffError(file.View(), allocator) == ErrorCode::InvalidArgument);
    }

    {
        AiffSpec spec;
        Bytes file(allocator, true);
        BuildAiff(file, spec, Span<const uint8>(samples));
        // SSND 본문은 offset(4)·blockSize(4)·표본(2)이다. offset 필드 마지막 byte를 키운다.
        file[file.Size() - 10 + 3] = 0x10;
        CHECK(AiffError(file.View(), allocator) == ErrorCode::InvalidArgument);
    }

    {
        AiffSpec spec;
        spec.ssnd = false;
        Bytes file(allocator, true);
        BuildAiff(file, spec, Span<const uint8>());
        CHECK(AiffError(file.View(), allocator) == ErrorCode::InvalidArgument);
    }

    {
        AiffSpec spec;
        spec.frames = 0;
        Bytes file(allocator, true);
        BuildAiff(file, spec, Span<const uint8>(samples));
        CHECK(AiffError(file.View(), allocator) == ErrorCode::InvalidArgument);
    }

    // PCM 상한은 COMM frame 수로 SSND를 보기 전에 판정한다(SSND가 없어도 CapacityExceeded).
    {
        AiffSpec spec;
        spec.channels = 8;
        spec.frames = static_cast<uint32>(MaxSoundClipPcmBytes / (8 * sizeof(float)) + 1);
        spec.ssnd = false;
        Bytes file(allocator, true);
        BuildAiff(file, spec, Span<const uint8>());
        CHECK(AiffError(file.View(), allocator) == ErrorCode::CapacityExceeded);
    }

    // FORM 형식 id가 AIFF·AIFC가 아니면 InvalidArgument.
    {
        AiffSpec spec;
        spec.form = "AIFX";
        Bytes file(allocator, true);
        BuildAiff(file, spec, Span<const uint8>(samples));
        CHECK(AiffError(file.View(), allocator) == ErrorCode::InvalidArgument);
    }
}

} // namespace source_audio_tests_file
