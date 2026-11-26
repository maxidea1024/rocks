#include "FlacTestFixtures.h"
#include "Mp3TestFixtures.h"
#include "OpusTestFixtures.h"
#include "SoundClipImportTestSupport.h"
#include "SourceAudioReference.h"
#include "VorbisTestFixtures.h"

#include <AssetRuntime/MemoryBulkSource.h>
#include <AudioTools/SoundClipImporter.h>
#include <AudioTools/SourceAudio.h>
#include <Platform/Memory.h>
#include <Base/Compiler.h>
#include <Platform/PlatformMath.h>

// SoundClip 필드·임포터·에디터 재생 PCM 시험이다(Docs/Design/AudioSystem 3.36.5절 —
// AU4b·AU4c·AU4d).
using namespace rocks;
namespace sound_clip_importer_tests_file {

namespace {

using namespace rocks::sound_import_test;
using namespace rocks::audio::flacfixtures;
using namespace rocks::audio::mp3fixtures;
using namespace rocks::audio::opusfixtures;
using namespace rocks::audio::vorbisfixtures;

struct ImportRun {
    ImportRun(SoundEnv& env, StringView path, Span<const uint8> bytes,
              const SoundClipImportOptions* options = nullptr, ObjectHandle existing = {}) noexcept
        : sources{{DefaultImportSourceRole, path, bytes}},
          context(env, Span<const AssetImportSource>(sources, 1), options, existing) {
        result = SoundClipImporter().Import(context);
    }

    AssetImportSource sources[1];
    FakeSoundImportContext context;
    Result<void> result = Result<void>::Success();
};

// 16비트 mono 표본 열의 원시 byte다.
void Pcm16(Bytes& out, Span<const int16> samples) noexcept {
    for (const int16 sample : samples) {
        out.Le16(static_cast<uint16>(sample));
    }
}

float StoredFloat(Span<const uint8> bulk, usize index) noexcept {
    const uint8* at = bulk.Data() + index * 4;
    const uint32 raw = static_cast<uint32>(at[0]) | static_cast<uint32>(at[1]) << 8 |
                       static_cast<uint32>(at[2]) << 16 | static_cast<uint32>(at[3]) << 24;
    float value = 0.0f;
    platform::MemoryCopy(&value, &raw, sizeof(value));
    return value;
}

// 디코더 결과의 planar float byte다(손실 원본 bulk가 이것과 같아야 한다 — 디코드 SHA 고정값은
// 디코더 시험이 이미 단언한다).
Sha256Digest DecodedFloatDigest(SoundEnv& env, StringView path, Span<const uint8> bytes) noexcept {
    audio::DecodedSourceAudio decoded(env.allocator);
    Result<void> result = Result<void>::Failure(ErrorCode::Unsupported);
    if (path.ByteCount() >= 4 && path.SliceBytes(path.ByteCount() - 4, 4) == StringView("flac")) {
        result = audio::DecodeFlac(bytes, decoded);
    } else if (path.ByteCount() >= 3 &&
               path.SliceBytes(path.ByteCount() - 3, 3) == StringView("mp3")) {
        result = audio::DecodeMp3(bytes, decoded);
    }

    CHECK(result);
    Sha256 hasher;
    for (uint32 channel = 0; channel < decoded.ChannelCount(); ++channel) {
        hasher.Update(Span<const uint8>(reinterpret_cast<const uint8*>(decoded.Channel(channel)),
                                        static_cast<usize>(decoded.FrameCount()) * sizeof(float)));
    }

    return hasher.Finish();
}

} // namespace

TEST(SoundClip_EffectiveLoopAndDimensionCheck) {
    SoundEnv env;
    REQUIRE(env.ready);
    auto clip = env.objects.NewObject<SoundClip>(env.objects.OwnerAccess(), TypeIdOf<SoundClip>());
    REQUIRE(clip);
    auto borrow = env.objects.Resolve<SoundClip>(env.objects.OwnerAccess(), clip.Value().Handle());
    REQUIRE(borrow);
    SoundClip& sound = *borrow.Value().Get().Value();
    CHECK(sound.channelCount == 0);
    CHECK(sound.source.format == SoundSourceStorage::None);
    uint32 begin = 9;
    uint32 end = 9;
    sound.EffectiveLoop(begin, end);
    CHECK(begin == 0);
    CHECK(end == 0);
    sound.hasFileLoop = true;
    sound.fileLoopBegin = 10;
    sound.fileLoopEnd = 20;
    sound.EffectiveLoop(begin, end);
    CHECK(begin == 10);
    CHECK(end == 20);
    sound.loopOverride.enabled = true;
    sound.loopOverride.begin = 2;
    sound.loopOverride.end = 5;
    sound.EffectiveLoop(begin, end);
    CHECK(begin == 2);
    CHECK(end == 5);

    auto pcm = SoundPcm::TryAllocate(env.allocator, 1, 40, 48000);
    REQUIRE(pcm);
    REQUIRE(sound.SetPcm(SharedPtr<const SoundPcm>(Move(pcm.Value()))));
    CHECK(sound.frameCount == 40);
    CHECK(sound.MakePlayable(env.allocator, env.objects, env.objects.OwnerAccess()));
    // 재임포트로 형식 필드만 바뀐 상태(재생 PCM은 옛 것)는 준비되지 않은 것이다.
    sound.frameCount = 80;
    CHECK(sound.MakePlayable(env.allocator, env.objects, env.objects.OwnerAccess()).Error() ==
          ErrorCode::NotReady);
}

TEST(SoundClipImport_RegistersWithOptionNameRules) {
    SoundEnv env;
    REQUIRE(env.ready);
    AssetImporterTable table(env.allocator, env.types);
    SoundClipImporter importer;
    Array<AssetImporterRegistration> tokens(env.allocator);
    REQUIRE(RegisterSoundClipImporter(&importer, table, tokens));
    CHECK(tokens.Size() == 1);
    CHECK(importer.Info().name == SoundClipImporterName);
    CHECK(RegisterSoundClipImporter(nullptr, table, tokens).Error() == ErrorCode::InvalidArgument);
}

TEST(SoundClipImport_WavStorageFormats) {
    SoundEnv env;
    REQUIRE(env.ready);
    {
        // 16비트 mono는 Int16 그대로(정확히 왕복).
        Bytes samples(env.allocator);
        const int16 values[] = {0, 16384, -16384, 32767, -32768};
        Pcm16(samples, Span<const int16>(values));
        Bytes file(env.allocator);
        BuildWav(file, WavSpec{}, samples.View());
        ImportRun run(env, "a.wav", file.View());
        REQUIRE(run.result);
        SoundClip* clip = run.context.Imported();
        REQUIRE(clip != nullptr);
        CHECK(clip->channelCount == 1);
        CHECK(clip->sampleRate == 48000);
        CHECK(clip->frameCount == 5);
        CHECK(clip->sourceCodec == SoundSourceCodec::Pcm);
        CHECK(clip->source.format == SoundSourceStorage::Int16);
        REQUIRE(run.context.bulk.Size() == samples.data.Size());
        CHECK(platform::MemoryCompare(run.context.bulk.Data(), samples.data.Data(),
                                      samples.data.Size()) == 0);
        CHECK(clip->source.bulk.rawBytes == samples.data.Size());
        CHECK(!clip->looping);
    }

    {
        // 8비트(부호 없는)는 Int16으로 넓힌다: 128 → 0, 255 → 127·256, 0 → −32768.
        Bytes samples(env.allocator);
        samples.U8(128);
        samples.U8(255);
        samples.U8(0);
        Bytes file(env.allocator);
        WavSpec spec;
        spec.bitsPerSample = 8;
        BuildWav(file, spec, samples.View());
        ImportRun run(env, "b.WAV", file.View());
        REQUIRE(run.result);
        CHECK(run.context.Imported()->source.format == SoundSourceStorage::Int16);
        Bytes expected(env.allocator);
        const int16 values[] = {0, 127 * 256, -32768};
        Pcm16(expected, Span<const int16>(values));
        REQUIRE(run.context.bulk.Size() == expected.data.Size());
        CHECK(platform::MemoryCompare(run.context.bulk.Data(), expected.data.Data(),
                                      expected.data.Size()) == 0);
    }

    {
        // 24비트 stereo는 Int24 packed planar.
        Bytes samples(env.allocator);
        samples.Le24(0x123456);
        samples.Le24(0xFEDCBA);
        samples.Le24(0x7FFFFF);
        samples.Le24(0x800000);
        Bytes file(env.allocator);
        WavSpec spec;
        spec.channels = 2;
        spec.bitsPerSample = 24;
        BuildWav(file, spec, samples.View());
        ImportRun run(env, "c.wav", file.View());
        REQUIRE(run.result);
        CHECK(run.context.Imported()->source.format == SoundSourceStorage::Int24);
        Bytes expected(env.allocator);
        expected.Le24(0x123456);
        expected.Le24(0x7FFFFF);
        expected.Le24(0xFEDCBA);
        expected.Le24(0x800000);
        REQUIRE(run.context.bulk.Size() == expected.data.Size());
        CHECK(platform::MemoryCompare(run.context.bulk.Data(), expected.data.Data(),
                                      expected.data.Size()) == 0);
    }

    {
        // 32비트 float는 Float32.
        Bytes samples(env.allocator);
        const float values[] = {0.25f, -0.5f};
        for (const float value : values) {
            uint32 raw = 0;
            platform::MemoryCopy(&raw, &value, sizeof(raw));
            samples.Le32(raw);
        }

        Bytes file(env.allocator);
        WavSpec spec;
        spec.formatTag = 3;
        spec.bitsPerSample = 32;
        BuildWav(file, spec, samples.View());
        ImportRun run(env, "d.wav", file.View());
        REQUIRE(run.result);
        CHECK(run.context.Imported()->source.format == SoundSourceStorage::Float32);
        CHECK(StoredFloat(run.context.bulk.View(), 0) == 0.25f);
        CHECK(StoredFloat(run.context.bulk.View(), 1) == -0.5f);
    }
}

TEST(SoundClipImport_LossyAndFlacSources) {
    SoundEnv env;
    REQUIRE(env.ready);
    {
        ImportRun run(env, "stereo.flac", Span<const uint8>(FlacStereo16));
        REQUIRE(run.result);
        SoundClip* clip = run.context.Imported();
        CHECK(clip->sourceCodec == SoundSourceCodec::Flac);
        CHECK(clip->channelCount == 2);
        CHECK(clip->source.format == SoundSourceStorage::Int16);
    }

    {
        // 손실 원본은 Float32이고 bulk는 디코더 결과와 같은 byte다.
        ImportRun run(env, "mono.mp3", Span<const uint8>(Mp3MonoLame));
        REQUIRE(run.result);
        SoundClip* clip = run.context.Imported();
        CHECK(clip->sourceCodec == SoundSourceCodec::Mp3);
        CHECK(clip->source.format == SoundSourceStorage::Float32);
        CHECK(clip->source.bulk.sha256 ==
              DecodedFloatDigest(env, "mono.mp3", Span<const uint8>(Mp3MonoLame)));
        CHECK(clip->source.bulk.rawBytes == static_cast<uint64>(clip->frameCount) * 4);
    }

    {
        ImportRun run(env, "music.ogg", Span<const uint8>(VorbisStereo));
        REQUIRE(run.result);
        CHECK(run.context.Imported()->sourceCodec == SoundSourceCodec::Vorbis);
        CHECK(run.context.Imported()->source.format == SoundSourceStorage::Float32);
    }

    {
        // .opus와, Opus를 .ogg로 저장한 파일을 모두 Opus로 읽는다(첫 page 식별 header).
        ImportRun opus(env, "voice.opus", Span<const uint8>(OpusStereo));
        ImportRun ogg(env, "voice.ogg", Span<const uint8>(OpusStereo));
        REQUIRE(opus.result);
        REQUIRE(ogg.result);
        CHECK(ogg.context.Imported()->sourceCodec == SoundSourceCodec::Opus);
        CHECK(ogg.context.Imported()->frameCount == opus.context.Imported()->frameCount);
        CHECK(ogg.context.Imported()->source.bulk.sha256 ==
              opus.context.Imported()->source.bulk.sha256);
    }
}

TEST(SoundClipImport_FileLoopsCuesAndWarnings) {
    SoundEnv env;
    REQUIRE(env.ready);
    Bytes samples(env.allocator);
    for (int16 value = 0; value < 64; ++value) {
        samples.Le16(static_cast<uint16>(value));
    }

    WavSpec spec;
    spec.loop = true;
    spec.loopStart = 8;
    spec.loopEndInclusive = 39;
    spec.cue = true;
    spec.cueId = 3;
    spec.cueFrame = 16;
    spec.unknownChunk = true;
    Bytes file(env.allocator);
    BuildWav(file, spec, samples.View());
    {
        ImportRun run(env, "loop.wav", file.View());
        REQUIRE(run.result);
        SoundClip* clip = run.context.Imported();
        CHECK(clip->hasFileLoop);
        CHECK(clip->fileLoopBegin == 8);
        // 파일 smpl 끝은 포함, 필드는 제외다.
        CHECK(clip->fileLoopEnd == 40);
        REQUIRE(clip->cues.Size() == 1);
        CHECK(clip->cues[0].id == 3);
        CHECK(clip->cues[0].frame == 16);
        REQUIRE(run.context.warnings.Size() == 1);
        CHECK(run.context.warnings[0].code == AssetDiagnosticCode::IgnoredSourceChunks);
        CHECK(run.context.warnings[0].detail >= 1);
        // 임포터는 looping을 켜지 않는다(UE bLooping과 같다).
        CHECK(!clip->looping);
    }

    {
        SoundClipImportOptions options;
        options.loopFromFile = false;
        ImportRun run(env, "loop.wav", file.View(), &options);
        REQUIRE(run.result);
        CHECK(!run.context.Imported()->hasFileLoop);
        CHECK(run.context.Imported()->cues.IsEmpty());
    }
}

TEST(SoundClipImport_DownmixAndNormalize) {
    SoundEnv env;
    REQUIRE(env.ready);
    Bytes stereo(env.allocator);
    // L = 0.5, R = −0.25가 두 frame.
    const int16 values[] = {16384, -8192, 16384, -8192};
    Pcm16(stereo, Span<const int16>(values));
    WavSpec spec;
    spec.channels = 2;
    Bytes file(env.allocator);
    BuildWav(file, spec, stereo.View());
    {
        SoundClipImportOptions options;
        options.channelHandling = SoundImportChannelHandling::DownmixToMono;
        ImportRun run(env, "s.wav", file.View(), &options);
        REQUIRE(run.result);
        SoundClip* clip = run.context.Imported();
        CHECK(clip->channelCount == 1);
        CHECK(clip->source.format == SoundSourceStorage::Float32);
        CHECK(StoredFloat(run.context.bulk.View(), 0) == 0.125f);
    }

    {
        SoundClipImportOptions options;
        options.normalize = true;
        options.normalizePeakDb = -6.0f;
        ImportRun run(env, "s.wav", file.View(), &options);
        REQUIRE(run.result);
        // 모든 채널 공통 peak(0.5)를 −6 dB(0.501187…)에 맞춘다 — 비율은 유지된다.
        const float left = StoredFloat(run.context.bulk.View(), 0);
        const float right = StoredFloat(run.context.bulk.View(), 2);
        CHECK(platform::PlatformMath::Abs(left - 0.5011872f) < 1.0e-5f);
        CHECK(platform::PlatformMath::Abs(right + 0.2505936f) < 1.0e-5f);
    }

    {
        // mono 원본에 downmix를 주면 아무것도 하지 않는다(정수 저장 유지).
        Bytes mono(env.allocator);
        Pcm16(mono, Span<const int16>(values, 2));
        Bytes monoFile(env.allocator);
        BuildWav(monoFile, WavSpec{}, mono.View());
        SoundClipImportOptions options;
        options.channelHandling = SoundImportChannelHandling::DownmixToMono;
        ImportRun run(env, "m.wav", monoFile.View(), &options);
        REQUIRE(run.result);
        CHECK(run.context.Imported()->source.format == SoundSourceStorage::Int16);
    }

    {
        SoundClipImportOptions options;
        options.normalizePeakDb = 1.0f;
        ImportRun run(env, "s.wav", file.View(), &options);
        CHECK(!run.result);
        CHECK(run.context.failCode == AssetDiagnosticCode::InvalidImportOption);
    }
}

TEST(SoundClipImport_RejectsInvalidSources) {
    SoundEnv env;
    REQUIRE(env.ready);
    const uint8 garbage[] = {'R', 'I', 'F', 'F', 1, 2, 3};
    {
        ImportRun run(env, "x.wav", Span<const uint8>(garbage));
        CHECK(!run.result);
        CHECK(run.context.failCode == AssetDiagnosticCode::InvalidSource);
    }

    {
        ImportRun run(env, "x.xyz", Span<const uint8>(garbage));
        CHECK(run.context.failCode == AssetDiagnosticCode::UnsupportedSourceFormat);
    }

    {
        // Ogg가 아니거나 잘린 page는 손상이다.
        ImportRun run(env, "x.ogg", Span<const uint8>(garbage));
        CHECK(run.context.failCode == AssetDiagnosticCode::InvalidSource);
        CHECK(run.context.failError == ErrorCode::CorruptData);
    }

    {
        // 온전한 첫 page인데 Opus·Vorbis가 아닌 codec(Ogg FLAC)은 미지원이다.
        Bytes page(env.allocator);
        page.Tag("OggS");
        for (uint32 index = 4; index < 26; ++index) {
            page.U8(0);
        }

        page.U8(1);
        page.U8(5);
        page.U8(0x7F);
        page.Tag("FLAC");
        ImportRun run(env, "flac.ogg", page.View());
        CHECK(run.context.failCode == AssetDiagnosticCode::UnsupportedSourceFormat);
    }

    {
        SoundClipImportOptions options;
        options.normalizePeakDb = -200.0f;
        ImportRun run(env, "x.wav", Span<const uint8>(garbage), &options);
        CHECK(run.context.failCode == AssetDiagnosticCode::InvalidImportOption);
    }

    Bytes one(env.allocator);
    const int16 sample[] = {1, 1, 1, 1, 1, 1, 1, 1, 1};
    Pcm16(one, Span<const int16>(sample));
    {
        WavSpec nine;
        nine.channels = 9;
        Bytes file(env.allocator);
        BuildWav(file, nine, one.View());
        ImportRun run(env, "nine.wav", file.View());
        CHECK(run.context.failCode == AssetDiagnosticCode::UnsupportedSourceFormat);
    }

    {
        WavSpec slow;
        slow.sampleRate = 4000;
        Bytes file(env.allocator);
        BuildWav(file, slow, one.View());
        ImportRun run(env, "slow.wav", file.View());
        CHECK(run.context.failCode == AssetDiagnosticCode::UnsupportedSourceFormat);
    }

    {
        // 빈 data chunk(frame 0)는 InvalidSource다.
        Bytes file(env.allocator);
        BuildWav(file, WavSpec{}, Span<const uint8>());
        ImportRun run(env, "empty.wav", file.View());
        CHECK(!run.result);
        CHECK(run.context.failCode == AssetDiagnosticCode::InvalidSource);
    }
}

// AIFF 하나다(big-endian, 컨테이너 bit 폭 = 표본 bit를 byte로 올림, 좌정렬). rate는 48000 고정이다.
void BuildAiff(Bytes& out, uint32 channels, uint32 frames, uint32 bits,
               Span<const uint8> samples) noexcept {
    // 48000 Hz의 80비트 확장 정밀도(지수 16383 + 15, 가수 0xBB80 << 48).
    constexpr uint8 Rate48000[10] = {0x40, 0x0E, 0xBB, 0x80, 0, 0, 0, 0, 0, 0};
    Bytes body(out.data.GetAllocator());
    body.Tag("AIFF");
    body.Tag("COMM");
    const auto be32 = [](Bytes& bytes, uint32 value) {
        bytes.U8(value >> 24);
        bytes.U8(value >> 16);
        bytes.U8(value >> 8);
        bytes.U8(value);
    };
    be32(body, 18);
    body.U8(channels >> 8);
    body.U8(channels);
    be32(body, frames);
    body.U8(bits >> 8);
    body.U8(bits);
    body.Append(Span<const uint8>(Rate48000));
    body.Tag("SSND");
    be32(body, 8 + static_cast<uint32>(samples.Size()));
    be32(body, 0);
    be32(body, 0);
    body.Append(samples);
    if ((samples.Size() & 1) != 0) {
        body.U8(0);
    }

    out.Tag("FORM");
    be32(out, static_cast<uint32>(body.data.Size()));
    out.Append(body.View());
}

TEST(SoundClipImport_IntegerSourcesRoundTripExactly) {
    SoundEnv env;
    REQUIRE(env.ready);
    MemoryBulkSource bulkSource(env.allocator);
    // 원본 표본을 디코더 float와 bit 단위로 같게 되돌리는지 본다.
    const auto checkEditorPcm = [&](ImportRun& run, Span<const uint8> file, bool aiff) {
        SoundClip* clip = run.context.Imported();
        auto ref = bulkSource.AddCopy(run.context.bulk.View());
        REQUIRE(ref);
        clip->source.bulk = ref.Value();
        REQUIRE(clip->LoadEditorPcm(env.allocator, bulkSource));
        audio::DecodedSourceAudio decoded(env.allocator);
        REQUIRE(aiff ? audio::DecodeAiff(file, decoded) : audio::DecodeWav(file, decoded));
        for (uint32 channel = 0; channel < clip->channelCount; ++channel) {
            CHECK(platform::MemoryCompare(clip->Pcm()->Channel(channel).Data(),
                                          decoded.Channel(channel),
                                          sizeof(float) * clip->frameCount) == 0);
        }
    };

    {
        // 32비트 정수 WAV는 Float32로 저장하고 디코더 float와 같다.
        Bytes samples(env.allocator);
        samples.Le32(0x40000000u);
        samples.Le32(0x80000000u);
        samples.Le32(0x00000001u);
        Bytes file(env.allocator);
        WavSpec spec;
        spec.bitsPerSample = 32;
        BuildWav(file, spec, samples.View());
        ImportRun run(env, "i32.wav", file.View());
        REQUIRE(run.result);
        CHECK(run.context.Imported()->source.format == SoundSourceStorage::Float32);
        CHECK(StoredFloat(run.context.bulk.View(), 0) == 0.5f);
        CHECK(StoredFloat(run.context.bulk.View(), 1) == -1.0f);
        checkEditorPcm(run, file.View(), false);
    }

    {
        // 12비트 AIFF(16비트 컨테이너, 좌정렬 v << 4)는 Int16에 컨테이너 값 그대로다.
        const int16 values[] = {100 << 4, -2048 * 16, 2047 << 4};
        Bytes samples(env.allocator);
        for (const int16 value : values) {
            const uint16 raw = static_cast<uint16>(value);
            samples.U8(raw >> 8);
            samples.U8(raw);
        }

        Bytes file(env.allocator);
        BuildAiff(file, 1, 3, 12, samples.View());
        ImportRun run(env, "a12.aiff", file.View());
        REQUIRE(run.result);
        CHECK(run.context.Imported()->source.format == SoundSourceStorage::Int16);
        Bytes expected(env.allocator);
        Pcm16(expected, Span<const int16>(values));
        REQUIRE(run.context.bulk.Size() == expected.data.Size());
        CHECK(platform::MemoryCompare(run.context.bulk.Data(), expected.data.Data(),
                                      expected.data.Size()) == 0);
        checkEditorPcm(run, file.View(), true);
    }

    {
        // 20비트 AIFF(24비트 컨테이너, 좌정렬)는 Int24에 컨테이너 값 그대로다.
        const uint32 values[] = {0x7FFFF0u, 0x800000u, 0x000010u};
        Bytes samples(env.allocator);
        for (const uint32 value : values) {
            samples.U8(value >> 16);
            samples.U8(value >> 8);
            samples.U8(value);
        }

        Bytes file(env.allocator);
        BuildAiff(file, 1, 3, 20, samples.View());
        ImportRun run(env, "a20.aif", file.View());
        REQUIRE(run.result);
        CHECK(run.context.Imported()->source.format == SoundSourceStorage::Int24);
        Bytes expected(env.allocator);
        for (const uint32 value : values) {
            expected.Le24(value);
        }

        REQUIRE(run.context.bulk.Size() == expected.data.Size());
        CHECK(platform::MemoryCompare(run.context.bulk.Data(), expected.data.Data(),
                                      expected.data.Size()) == 0);
        checkEditorPcm(run, file.View(), true);
    }
}

TEST(SoundClipImport_LossySourcesMatchDecoderReference) {
    SoundEnv env;
    REQUIRE(env.ready);
    namespace reference = audio::sourcereference;
    // frame 수는 delay·padding을 잘라 낸 원래 길이(디코더 시험 기준값)다.
    ImportRun flac(env, "s.flac", Span<const uint8>(FlacStereo16));
    ImportRun flac24(env, "m.flac", Span<const uint8>(FlacMono24));
    ImportRun vorbis(env, "v.ogg", Span<const uint8>(VorbisStereo));
    ImportRun opus(env, "o.opus", Span<const uint8>(OpusStereo));
    ImportRun mp3(env, "m.mp3", Span<const uint8>(Mp3MonoLame));
    REQUIRE(flac.result && flac24.result && vorbis.result && opus.result && mp3.result);
    CHECK(flac.context.Imported()->frameCount == reference::FlacStereo16Frames);
    CHECK(flac24.context.Imported()->frameCount == reference::FlacMono24Frames);
    CHECK(flac24.context.Imported()->source.format == SoundSourceStorage::Int24);
    CHECK(vorbis.context.Imported()->frameCount == reference::VorbisStereoFrames);
    CHECK(opus.context.Imported()->frameCount == reference::OpusStereoFrames);
    CHECK(mp3.context.Imported()->frameCount == reference::Mp3MonoLameFrames);
#if ROCKS_ARCH_X64
    // 손실 원본 bulk(Float32 planar LE)의 SHA가 디코더 고정 SHA와 같다(디코드 SHA 고정값 기준).
    const auto hex = [](ImportRun& run, char (&out)[Sha256Digest::HexBytes + 1]) {
        run.context.Imported()->source.bulk.sha256.ToHex(out);
    };
    char digest[Sha256Digest::HexBytes + 1];
    hex(vorbis, digest);
    CHECK(StringView(digest) == StringView(reference::VorbisStereoDigestX64));
    hex(opus, digest);
    CHECK(StringView(digest) == StringView(reference::OpusStereoDigestX64));
    hex(mp3, digest);
    CHECK(StringView(digest) == StringView(reference::Mp3MonoLameDigestX64));
#endif
}

TEST(SoundClipImport_ReimportClearsImportFields) {
    SoundEnv env;
    REQUIRE(env.ready);
    // 첫 원본: stereo 24비트, 파일 loop·cue 있음.
    Bytes first(env.allocator);
    for (uint32 frame = 0; frame < 32; ++frame) {
        first.Le24(frame * 1000);
        first.Le24(frame * 2000);
    }

    WavSpec firstSpec;
    firstSpec.channels = 2;
    firstSpec.bitsPerSample = 24;
    firstSpec.loop = true;
    firstSpec.loopStart = 4;
    firstSpec.loopEndInclusive = 20;
    firstSpec.cue = true;
    firstSpec.cueFrame = 10;
    Bytes firstFile(env.allocator);
    BuildWav(firstFile, firstSpec, first.View());
    ImportRun created(env, "r.wav", firstFile.View());
    REQUIRE(created.result);
    SoundClip* clip = created.context.Imported();
    REQUIRE(clip->hasFileLoop);
    REQUIRE(clip->cues.Size() == 1);
    clip->looping = true;
    clip->loopOverride.enabled = true;
    clip->loopOverride.begin = 1;
    clip->loopOverride.end = 3;
    clip->volume = 0.25f;
    clip->pitch = 1.5f;

    // 둘째 원본: mono 16비트, loop·cue 없음 — 임포트 필드는 모두 새 값(빈 값 포함)으로 바뀐다.
    Bytes second(env.allocator);
    const int16 values[] = {1, 2, 3, 4, 5};
    Pcm16(second, Span<const int16>(values));
    Bytes secondFile(env.allocator);
    BuildWav(secondFile, WavSpec{}, second.View());
    ImportRun again(env, "r.wav", secondFile.View(), nullptr, created.context.output);
    REQUIRE(again.result);
    CHECK(!clip->hasFileLoop);
    CHECK(clip->fileLoopBegin == 0);
    CHECK(clip->fileLoopEnd == 0);
    CHECK(clip->cues.IsEmpty());
    CHECK(clip->channelCount == 1);
    CHECK(clip->frameCount == 5);
    CHECK(clip->source.format == SoundSourceStorage::Int16);
    CHECK(clip->sourceCodec == SoundSourceCodec::Pcm);
    // 사용자 설정은 그대로다.
    CHECK(clip->looping);
    CHECK(clip->loopOverride.enabled);
    CHECK(clip->loopOverride.begin == 1);
    CHECK(clip->loopOverride.end == 3);
    CHECK(clip->volume == 0.25f);
    CHECK(clip->pitch == 1.5f);
}

TEST(SoundClipImport_ReimportKeepsUserSettings) {
    SoundEnv env;
    REQUIRE(env.ready);
    Bytes first(env.allocator);
    const int16 a[] = {1, 2, 3, 4};
    Pcm16(first, Span<const int16>(a));
    Bytes firstFile(env.allocator);
    BuildWav(firstFile, WavSpec{}, first.View());
    ImportRun created(env, "a.wav", firstFile.View());
    REQUIRE(created.result);
    SoundClip* clip = created.context.Imported();
    clip->looping = true;
    clip->loopOverride.enabled = true;
    clip->loopOverride.end = 2;
    clip->volume = 0.5f;

    Bytes second(env.allocator);
    const int16 b[] = {9, 8, 7, 6, 5, 4};
    Pcm16(second, Span<const int16>(b));
    WavSpec spec;
    spec.sampleRate = 44100;
    Bytes secondFile(env.allocator);
    BuildWav(secondFile, spec, second.View());
    ImportRun again(env, "a.wav", secondFile.View(), nullptr, created.context.output);
    REQUIRE(again.result);
    CHECK(clip->frameCount == 6);
    CHECK(clip->sampleRate == 44100);
    CHECK(clip->looping);
    CHECK(clip->loopOverride.enabled);
    CHECK(clip->loopOverride.end == 2);
    CHECK(clip->volume == 0.5f);
}

TEST(SoundClip_LoadEditorPcmFromSourceBulk) {
    SoundEnv env;
    REQUIRE(env.ready);
    MemoryBulkSource bulkSource(env.allocator);
    Bytes samples(env.allocator);
    // 24비트 mono 세 표본.
    samples.Le24(0x400000);
    samples.Le24(0xC00000);
    samples.Le24(0x000001);
    Bytes file(env.allocator);
    WavSpec spec;
    spec.bitsPerSample = 24;
    BuildWav(file, spec, samples.View());
    ImportRun run(env, "e.wav", file.View());
    REQUIRE(run.result);
    SoundClip* clip = run.context.Imported();
    // 저장된 원본 bulk를 메모리 원천에 넣고 ref를 그쪽 것으로 바꾼다.
    auto ref = bulkSource.AddCopy(run.context.bulk.View());
    REQUIRE(ref);
    clip->source.bulk = ref.Value();
    CHECK(clip->MakePlayable(env.allocator, env.objects, env.objects.OwnerAccess()).Error() ==
          ErrorCode::NotReady);
    REQUIRE(clip->LoadEditorPcm(env.allocator, bulkSource));
    const uint64 revision = clip->PcmRevision();
    REQUIRE(clip->Pcm());
    CHECK(clip->Pcm()->Channel(0)[0] == 0.5f);
    CHECK(clip->Pcm()->Channel(0)[1] == -0.5f);
    CHECK(clip->Pcm()->Channel(0)[2] == 1.0f / 8388608.0f);
    // 같은 키는 다시 올리지 않는다.
    REQUIRE(clip->LoadEditorPcm(env.allocator, bulkSource));
    CHECK(clip->PcmRevision() == revision);
    auto first = clip->MakePlayable(env.allocator, env.objects, env.objects.OwnerAccess());
    REQUIRE(first);
    // byte가 같아도 rate만 바뀐 재임포트는 다시 올린다.
    clip->sampleRate = 44100;
    REQUIRE(clip->LoadEditorPcm(env.allocator, bulkSource));
    CHECK(clip->PcmRevision() == revision + 1);
    CHECK(clip->Pcm()->SampleRate() == 44100);
    // 먼저 만든 스냅숏은 이전 PCM을 그대로 쥔다.
    CHECK(first.Value().Get()->pcm->SampleRate() == 48000);

    // 크기가 맞지 않는 bulk·비유한 float·3채널. 실패는 PCM·revision·키를 바꾸지 않는다.
    const SoundPcm* loaded = clip->Pcm().Get();
    const uint64 loadedRevision = clip->PcmRevision();
    clip->frameCount = 4;
    CHECK(clip->LoadEditorPcm(env.allocator, bulkSource).Error() == ErrorCode::CorruptData);
    CHECK(clip->Pcm().Get() == loaded);
    CHECK(clip->PcmRevision() == loadedRevision);
    clip->frameCount = 3;
    REQUIRE(clip->LoadEditorPcm(env.allocator, bulkSource));
    CHECK(clip->PcmRevision() == loadedRevision);
    clip->frameCount = 4;
    clip->frameCount = 1;
    clip->channelCount = 1;
    clip->source.format = SoundSourceStorage::Float32;
    const uint8 nan[] = {0x00, 0x00, 0xC0, 0x7F};
    auto nanRef = bulkSource.AddCopy(Span<const uint8>(nan));
    REQUIRE(nanRef);
    clip->source.bulk = nanRef.Value();
    CHECK(clip->LoadEditorPcm(env.allocator, bulkSource).Error() == ErrorCode::CorruptData);
    clip->channelCount = 3;
    CHECK(clip->LoadEditorPcm(env.allocator, bulkSource).Error() == ErrorCode::Unsupported);
    clip->source.format = SoundSourceStorage::None;
    CHECK(clip->LoadEditorPcm(env.allocator, bulkSource).Error() == ErrorCode::NotReady);
}

} // namespace sound_clip_importer_tests_file
