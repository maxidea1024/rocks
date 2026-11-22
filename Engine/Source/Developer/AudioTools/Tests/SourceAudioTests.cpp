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
        out.U16(spec.bits);
        out.U32(0);
        // GUID 메모리 배치: tag LE 2 byte + 00 00 00 00 10 00 80 00 00 AA 00 38 9B 71.
        out.U8(spec.subFormatTag & 0xFF);
        out.U8(spec.subFormatTag >> 8);
        const uint8 tail[14] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80,
                                0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
        out.Raw(Span<const uint8>(tail));
    }

    out.CloseSize(fmt);
    out.Tag("data");
    const usize data = out.OpenSize();
    out.Raw(sampleBytes);
    out.CloseSize(data);
    out.CloseSize(riff, false);
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

    // 16비트 스테레오 LE: 교차 → planar.
    {
        const uint8 samples[] = {0x00, 0x80, 0xFF, 0x7F, 0x01, 0x00, 0xFF, 0xFF};
        Bytes file(allocator);
        WavSpec spec;
        spec.channels = 2;
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

    // EXTENSIBLE: PCM GUID와 float GUID, 8채널.
    for (const uint16 subFormat : {uint16{1}, uint16{3}}) {
        Array<uint8> samples(allocator);
        REQUIRE(samples.TryResize(8 * 4));
        for (uint32 channel = 0; channel < 8; ++channel) {
            const uint32 bits = subFormat == 1
                                    ? static_cast<uint32>((channel + 1) * 1000)
                                    : platform::BitCast<uint32>(static_cast<float>(channel) * 0.1f);
            for (uint32 byte = 0; byte < 4; ++byte) {
                samples[channel * 4 + byte] = static_cast<uint8>(bits >> (byte * 8));
            }
        }

        Bytes file(allocator);
        WavSpec spec;
        spec.extensible = true;
        spec.subFormatTag = subFormat;
        spec.channels = 8;
        spec.bits = 32;
        BuildWav(file, spec, Span<const uint8>(samples.Data(), samples.Size()));
        const DecodedSourceAudio audio = DecodeOrDie(file.View(), allocator);
        REQUIRE(audio.ChannelCount() == 8 && audio.FrameCount() == 1);
        for (uint32 channel = 0; channel < 8; ++channel) {
            const float expected = subFormat == 1
                                       ? static_cast<float>((channel + 1) * 1000) / 2147483648.0f
                                       : static_cast<float>(channel) * 0.1f;
            CHECK(audio.Channel(channel)[0] == expected);
        }
    }

    // RIFX(BE): 16비트 0x4000 = 0.5.
    {
        const uint8 samples[] = {0x40, 0x00, 0xC0, 0x00};
        Bytes file(allocator, true);
        WavSpec spec;
        spec.bigEndian = true;
        BuildWav(file, spec, Span<const uint8>(samples));
        const DecodedSourceAudio audio = DecodeOrDie(file.View(), allocator);
        CHECK(audio.Channel(0)[0] == 0.5f && audio.Channel(0)[1] == -0.5f);
    }
}

TEST(Audio_Source_WavChunksLoopsAndCues) {
    SystemAllocator allocator;
    Bytes file(allocator);
    file.Tag("RIFF");
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
    // 두 번째 fmt는 무시하고 센다.
    file.Tag("fmt ");
    const usize duplicate = file.OpenSize();
    file.U32(0);
    file.CloseSize(duplicate);
    // smpl: loop 두 개(하나는 범위 밖).
    file.Tag("smpl");
    const usize smpl = file.OpenSize();
    for (uint32 index = 0; index < 7; ++index) {
        file.U32(0);
    }

    file.U32(2);
    file.U32(0);
    for (const uint32 end : {49u, 500u}) {
        file.U32(1);
        file.U32(0);
        file.U32(10);
        file.U32(end);
        file.U32(0);
        file.U32(0);
    }

    file.CloseSize(smpl);
    // cue 두 개와 adtl labl 이름 하나.
    file.Tag("cue ");
    const usize cue = file.OpenSize();
    file.U32(2);
    for (const uint32 id : {5u, 9u}) {
        file.U32(id);
        file.U32(0);
        file.Tag("data");
        file.U32(0);
        file.U32(0);
        file.U32(id * 10);
    }

    file.CloseSize(cue);
    file.Tag("LIST");
    const usize list = file.OpenSize();
    file.Tag("adtl");
    file.Tag("labl");
    const usize label = file.OpenSize();
    file.U32(9);
    const char name[] = "Hit";
    for (const char character : name) {
        file.U8(static_cast<uint8>(character));
    }

    file.CloseSize(label);
    file.CloseSize(list);
    file.CloseSize(riff, false);

    const DecodedSourceAudio audio = DecodeOrDie(file.View(), allocator);
    CHECK(audio.FrameCount() == 100 && audio.Channel(0)[3] == 300.0f / 32768.0f);
    REQUIRE(audio.Loops().Size() == 1);
    CHECK(audio.Loops()[0].start == 10 && audio.Loops()[0].end == 50);
    REQUIRE(audio.Cues().Size() == 2);
    CHECK(audio.Cues()[0].id == 5 && audio.Cues()[0].frame == 50 && audio.Cues()[0].name.IsEmpty());
    CHECK(audio.Cues()[1].frame == 90 && audio.Cues()[1].name.View() == StringView("Hit"));
    // junk와 중복 fmt.
    CHECK(audio.IgnoredChunks() == 2);
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

    WavSpec misaligned;
    misaligned.blockAlignOverride = 3;
    Bytes misalignedFile(allocator);
    BuildWav(misalignedFile, misaligned, Span<const uint8>(samples));
    CHECK(WavError(misalignedFile.View(), allocator) == ErrorCode::InvalidArgument);

    // 빈 data, 잘린 chunk, RIFF 크기가 파일보다 큼, 머리 없음.
    Bytes empty(allocator);
    BuildWav(empty, WavSpec{}, Span<const uint8>());
    CHECK(WavError(empty.View(), allocator) == ErrorCode::InvalidArgument);
    Bytes good(allocator);
    BuildWav(good, WavSpec{}, Span<const uint8>(samples));
    CHECK(WavError(good.View().Left(good.Size() - 1), allocator) == ErrorCode::InvalidArgument);
    good[4] = 0xFF;
    CHECK(WavError(good.View(), allocator) == ErrorCode::InvalidArgument);
    CHECK(WavError(Span<const uint8>(samples), allocator) == ErrorCode::InvalidArgument);

    // PCM 상한: float32 기준 MaxSoundClipPcmBytes를 넘는 크기는 할당 전에
    // CapacityExceeded다(디코더가 머리에서 얻은 frame 수로 이 경로를 탄다).
    DecodedSourceAudio audio(allocator);
    const uint64 overLimit = MaxSoundClipPcmBytes / (8 * sizeof(float)) + 1;
    CHECK(audio.TryAllocatePcm(8, Rate, overLimit, SourceSampleFormat::UInt8, SourceCodec::Pcm)
              .Error() == ErrorCode::CapacityExceeded);
    CHECK(audio.FrameCount() == 0);
}

TEST(Audio_Source_Aiff) {
    SystemAllocator allocator;
    // AIFF 16비트 BE, rate 44100(80비트 extended: 0x400E AC44 0000 0000 0000).
    const auto build = [&](Bytes& file, const char* form, const char* compression, uint32 bits,
                           Span<const uint8> rate, Span<const uint8> samples, uint32 frames,
                           uint32 channels) {
        file.Tag("FORM");
        const usize formSize = file.OpenSize();
        file.Tag(form);
        file.Tag("COMM");
        const usize comm = file.OpenSize();
        file.U16(channels);
        file.U32(frames);
        file.U16(bits);
        file.Raw(rate);
        if (compression != nullptr) {
            file.Tag(compression);
            file.U8(0);
            file.U8(0);
        }

        file.CloseSize(comm);
        file.Tag("SSND");
        const usize ssnd = file.OpenSize();
        file.U32(0);
        file.U32(0);
        file.Raw(samples);
        file.CloseSize(ssnd);
        file.CloseSize(formSize, false);
    };

    const uint8 rate44100[10] = {0x40, 0x0E, 0xAC, 0x44, 0, 0, 0, 0, 0, 0};
    const uint8 rate48000[10] = {0x40, 0x0E, 0xBB, 0x80, 0, 0, 0, 0, 0, 0};
    const uint8 rate22050[10] = {0x40, 0x0D, 0xAC, 0x44, 0, 0, 0, 0, 0, 0};
    {
        const uint8 samples[] = {0x40, 0x00, 0x80, 0x00};
        Bytes file(allocator, true);
        build(file, "AIFF", nullptr, 16, Span<const uint8>(rate44100), Span<const uint8>(samples),
              2, 1);
        const DecodedSourceAudio audio = DecodeOrDie(file.View(), allocator, true);
        CHECK(audio.SampleRate() == 44100 && audio.FrameCount() == 2);
        CHECK(audio.Channel(0)[0] == 0.5f && audio.Channel(0)[1] == -1.0f);
    }

    // AIFC sowt(LE)와 fl32(BE float), rate 48000·22050.
    {
        const uint8 samples[] = {0x00, 0x40, 0x00, 0x80};
        Bytes file(allocator, true);
        build(file, "AIFC", "sowt", 16, Span<const uint8>(rate48000), Span<const uint8>(samples), 2,
              1);
        const DecodedSourceAudio audio = DecodeOrDie(file.View(), allocator, true);
        CHECK(audio.SampleRate() == 48000 && audio.Channel(0)[0] == 0.5f &&
              audio.Channel(0)[1] == -1.0f);
    }

    {
        const uint32 bits = platform::BitCast<uint32>(-0.25f);
        const uint8 samples[] = {static_cast<uint8>(bits >> 24), static_cast<uint8>(bits >> 16),
                                 static_cast<uint8>(bits >> 8), static_cast<uint8>(bits)};
        Bytes file(allocator, true);
        build(file, "AIFC", "fl32", 32, Span<const uint8>(rate22050), Span<const uint8>(samples), 1,
              1);
        const DecodedSourceAudio audio = DecodeOrDie(file.View(), allocator, true);
        CHECK(audio.SampleRate() == 22050 && audio.Channel(0)[0] == -0.25f);
        CHECK(audio.SampleFormat() == SourceSampleFormat::Float32);
    }

    // 8비트는 부호 있음: 0x80 = −1, 0x40 = 0.5.
    {
        const uint8 samples[] = {0x80, 0x40};
        Bytes file(allocator, true);
        build(file, "AIFF", nullptr, 8, Span<const uint8>(rate44100), Span<const uint8>(samples), 2,
              1);
        const DecodedSourceAudio audio = DecodeOrDie(file.View(), allocator, true);
        CHECK(audio.Channel(0)[0] == -1.0f && audio.Channel(0)[1] == 0.5f);
    }

    // 음성: 모르는 압축, 소수 rate, 짧은 SSND.
    {
        const uint8 samples[] = {0, 0};
        Bytes file(allocator, true);
        build(file, "AIFC", "ima4", 16, Span<const uint8>(rate44100), Span<const uint8>(samples), 1,
              1);
        DecodedSourceAudio audio(allocator);
        CHECK(DecodeAiff(file.View(), audio).Error() == ErrorCode::Unsupported);
        const uint8 fractional[10] = {0x40, 0x0E, 0xAC, 0x44, 0x80, 0, 0, 0, 0, 0};
        Bytes odd(allocator, true);
        build(odd, "AIFF", nullptr, 16, Span<const uint8>(fractional), Span<const uint8>(samples),
              1, 1);
        CHECK(DecodeAiff(odd.View(), audio).Error() == ErrorCode::Unsupported);
        Bytes shortData(allocator, true);
        build(shortData, "AIFF", nullptr, 16, Span<const uint8>(rate44100),
              Span<const uint8>(samples), 4, 1);
        CHECK(DecodeAiff(shortData.View(), audio).Error() == ErrorCode::InvalidArgument);
    }
}

} // namespace source_audio_tests_file
