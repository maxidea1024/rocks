#include "FlacTestFixtures.h"
#include "Mp3TestFixtures.h"
#include "OpusTestFixtures.h"
#include "SoundClipImportTestSupport.h"
#include "VorbisTestFixtures.h"

#include <AssetRuntime/MemoryBulkSource.h>
#include <AudioTools/SoundClipImporter.h>
#include <AudioTools/SourceAudio.h>
#include <Platform/Memory.h>
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
        ImportRun run(env, "x.ogg", Span<const uint8>(garbage));
        CHECK(run.context.failCode == AssetDiagnosticCode::UnsupportedSourceFormat);
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

    // 크기가 맞지 않는 bulk·비유한 float·3채널.
    clip->frameCount = 4;
    CHECK(clip->LoadEditorPcm(env.allocator, bulkSource).Error() == ErrorCode::CorruptData);
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
