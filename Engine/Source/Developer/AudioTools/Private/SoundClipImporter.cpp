#include <Audio/Decibels.h>
#include <AudioAssets/SoundClip.generated.h>
#include <AudioAssets/SoundClipImportOptions.generated.h>
#include <AudioTools/SoundClipImporter.h>
#include <AudioTools/SourceAudio.h>
#include <Core/Math/MathUtility.h>
#include <Object/ObjectRegistry.h>
#include <Platform/Memory.h>

namespace rocks {

namespace {

using audio::DecodedSourceAudio;
using audio::SourceCodec;
using audio::SourceSampleFormat;

constexpr Guid SoundClipImporterId{0x5d2a8c41e7b34f09ull, 0x9a61c3e05b7d2f14ull};

constexpr StringView SoundClipExtensions[] = {"wav", "aif", "aiff", "flac", "ogg", "opus", "mp3"};

// Ogg page 머리(27 byte) 뒤 segment 표 길이 칸과 식별 header다(RFC 3533, RFC 7845, Vorbis I 4.2).
constexpr usize OggPageHeaderBytes = 27;
constexpr usize OggSegmentCountOffset = 26;
constexpr uint8 OggCapturePattern[] = {'O', 'g', 'g', 'S'};
constexpr uint8 OpusHeadMagic[] = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd'};
constexpr uint8 VorbisIdentificationMagic[] = {0x01, 'v', 'o', 'r', 'b', 'i', 's'};

enum class SourceKind : uint8 {
    Unknown,
    // Ogg가 아니거나 첫 page가 잘렸다(손상 — InvalidSource).
    CorruptOgg,
    Wav,
    Aiff,
    Flac,
    Vorbis,
    Opus,
    Mp3
};

char LowerAscii(char value) noexcept {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
}

// path의 마지막 '.' 뒤 확장자가 extension(소문자)과 같은지다.
bool HasExtension(StringView path, StringView extension) noexcept {
    usize dot = path.ByteCount();
    for (usize index = path.ByteCount(); index > 0; --index) {
        const char at = path.Data()[index - 1];
        if (at == '.') {
            dot = index - 1;
            break;
        }

        if (at == '/' || at == '\\') {
            return false;
        }
    }

    if (dot == path.ByteCount() || path.ByteCount() - dot - 1 != extension.ByteCount()) {
        return false;
    }

    for (usize index = 0; index < extension.ByteCount(); ++index) {
        if (LowerAscii(path.Data()[dot + 1 + index]) != extension.Data()[index]) {
            return false;
        }
    }

    return true;
}

bool StartsWith(Span<const uint8> bytes, usize offset, Span<const uint8> magic) noexcept {
    return offset <= bytes.Size() && bytes.Size() - offset >= magic.Size() &&
           platform::MemoryCompare(bytes.Data() + offset, magic.Data(), magic.Size()) == 0;
}

// 첫 Ogg page 본문의 식별 header로 Opus·Vorbis를 가린다(Opus를 .ogg로 저장한 파일이 흔하다). Ogg가
// 아니거나 page가 잘렸으면 손상, page는 온전한데 다른 codec(Ogg FLAC 등)이면 미지원이다.
SourceKind SniffOgg(Span<const uint8> bytes) noexcept {
    if (!StartsWith(bytes, 0, Span<const uint8>(OggCapturePattern)) ||
        bytes.Size() <= OggSegmentCountOffset) {
        return SourceKind::CorruptOgg;
    }

    const usize body = OggPageHeaderBytes + bytes[OggSegmentCountOffset];
    if (body > bytes.Size()) {
        return SourceKind::CorruptOgg;
    }

    if (StartsWith(bytes, body, Span<const uint8>(OpusHeadMagic))) {
        return SourceKind::Opus;
    }

    if (StartsWith(bytes, body, Span<const uint8>(VorbisIdentificationMagic))) {
        return SourceKind::Vorbis;
    }

    return SourceKind::Unknown;
}

SourceKind ChooseKind(StringView path, Span<const uint8> bytes) noexcept {
    if (HasExtension(path, "wav")) {
        return SourceKind::Wav;
    }

    if (HasExtension(path, "aif") || HasExtension(path, "aiff")) {
        return SourceKind::Aiff;
    }

    if (HasExtension(path, "flac")) {
        return SourceKind::Flac;
    }

    if (HasExtension(path, "mp3")) {
        return SourceKind::Mp3;
    }

    if (HasExtension(path, "ogg") || HasExtension(path, "opus")) {
        return SniffOgg(bytes);
    }

    return SourceKind::Unknown;
}

Result<void> Decode(SourceKind kind, Span<const uint8> bytes, DecodedSourceAudio& out) noexcept {
    switch (kind) {
    case SourceKind::Wav:
        return audio::DecodeWav(bytes, out);
    case SourceKind::Aiff:
        return audio::DecodeAiff(bytes, out);
    case SourceKind::Flac:
        return audio::DecodeFlac(bytes, out);
    case SourceKind::Vorbis:
        return audio::DecodeVorbis(bytes, out);
    case SourceKind::Opus:
        return audio::DecodeOpus(bytes, out);
    case SourceKind::Mp3:
        return audio::DecodeMp3(bytes, out);
    case SourceKind::Unknown:
    case SourceKind::CorruptOgg:
        break;
    }

    return Result<void>::Failure(ErrorCode::Unsupported);
}

SoundSourceCodec CodecOf(SourceCodec codec) noexcept {
    switch (codec) {
    case SourceCodec::Pcm:
        return SoundSourceCodec::Pcm;
    case SourceCodec::Flac:
        return SoundSourceCodec::Flac;
    case SourceCodec::Vorbis:
        return SoundSourceCodec::Vorbis;
    case SourceCodec::Opus:
        return SoundSourceCodec::Opus;
    case SourceCodec::Mp3:
        return SoundSourceCodec::Mp3;
    }

    return SoundSourceCodec::Pcm;
}

// 원본 표본 형식의 저장 형식이다(3.10.1절).
SoundSourceStorage StorageOf(SourceSampleFormat format) noexcept {
    switch (format) {
    case SourceSampleFormat::UInt8:
    case SourceSampleFormat::Int16:
        return SoundSourceStorage::Int16;
    case SourceSampleFormat::Int24:
        return SoundSourceStorage::Int24;
    case SourceSampleFormat::Int32:
    case SourceSampleFormat::Float32:
    case SourceSampleFormat::Float64:
    case SourceSampleFormat::Lossy:
        return SoundSourceStorage::Float32;
    }

    return SoundSourceStorage::Float32;
}

int32 Quantize(float value, float scale, int32 low, int32 high) noexcept {
    const float scaled = math::Round(value * scale);
    if (scaled <= static_cast<float>(low)) {
        return low;
    }

    if (scaled >= static_cast<float>(high)) {
        return high;
    }

    return static_cast<int32>(scaled);
}

void WriteSample(uint8* out, float value, SoundSourceStorage storage) noexcept {
    switch (storage) {
    case SoundSourceStorage::Int16: {
        const uint32 raw = static_cast<uint32>(Quantize(value, SoundSourceInt16FullScale,
                                                        SoundSourceInt16Min, SoundSourceInt16Max)) &
                           SoundSourceInt16Mask;
        out[0] = static_cast<uint8>(raw);
        out[1] = static_cast<uint8>(raw >> 8);
        return;
    }
    case SoundSourceStorage::Int24: {
        const uint32 raw = static_cast<uint32>(Quantize(value, SoundSourceInt24FullScale,
                                                        SoundSourceInt24Min, SoundSourceInt24Max)) &
                           SoundSourceInt24Mask;
        out[0] = static_cast<uint8>(raw);
        out[1] = static_cast<uint8>(raw >> 8);
        out[2] = static_cast<uint8>(raw >> 16);
        return;
    }
    case SoundSourceStorage::Float32: {
        uint32 raw = 0;
        platform::MemoryCopy(&raw, &value, sizeof(raw));
        out[0] = static_cast<uint8>(raw);
        out[1] = static_cast<uint8>(raw >> 8);
        out[2] = static_cast<uint8>(raw >> 16);
        out[3] = static_cast<uint8>(raw >> 24);
        return;
    }
    case SoundSourceStorage::None:
        return;
    }
}

} // namespace

SoundClipImporter::SoundClipImporter() noexcept {
    info_.id = SoundClipImporterId;
    info_.name = SoundClipImporterName;
    info_.version = SoundClipImporterVersion;
    info_.outputType = TypeIdOf<SoundClip>();
    info_.optionsType = TypeIdOf<SoundClipImportOptions>();
    info_.extensions = Span<const StringView>(SoundClipExtensions);
}

Result<void> SoundClipImporter::Import(AssetImportContext& context) noexcept {
    Allocator& allocator = context.Registry().ObjectAllocator();
    const Span<const AssetImportSource> sources = context.Sources();
    if (sources.Size() != 1) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    SoundClipImportOptions options;
    if (const void* given = context.Options(); given != nullptr) {
        options = *static_cast<const SoundClipImportOptions*>(given);
    }

    if (!math::IsFinite(options.normalizePeakDb) || options.normalizePeakDb < MinNormalizePeakDb ||
        options.normalizePeakDb > MaxNormalizePeakDb) {
        return context.Fail(AssetDiagnosticCode::InvalidImportOption, ErrorCode::InvalidArgument,
                            0);
    }

    const AssetImportSource& source = sources[0];
    const SourceKind kind = ChooseKind(source.path, source.bytes);
    if (kind == SourceKind::Unknown) {
        return context.Fail(AssetDiagnosticCode::UnsupportedSourceFormat, ErrorCode::Unsupported,
                            0);
    }

    if (kind == SourceKind::CorruptOgg) {
        return context.Fail(AssetDiagnosticCode::InvalidSource, ErrorCode::CorruptData, 0);
    }

    // 디코더 오류 → 진단(3.36.3절): 형식 미지원은 UnsupportedSourceFormat, 할당 실패는 그대로,
    // 나머지 (손상·잘림·상한 초과)는 InvalidSource다.
    DecodedSourceAudio decoded(allocator);
    if (auto result = Decode(kind, source.bytes, decoded); !result) {
        if (result.Error() == ErrorCode::OutOfMemory) {
            return result;
        }

        if (result.Error() == ErrorCode::Unsupported) {
            return context.Fail(AssetDiagnosticCode::UnsupportedSourceFormat,
                                ErrorCode::Unsupported, 0);
        }

        return context.Fail(AssetDiagnosticCode::InvalidSource, result.Error(), 0);
    }

    const uint64 frames64 = decoded.FrameCount();
    if (frames64 == 0 || frames64 > NumericLimits<uint32>::Max()) {
        return context.Fail(AssetDiagnosticCode::InvalidSource, ErrorCode::CorruptData, 0);
    }

    const uint32 frames = static_cast<uint32>(frames64);
    const uint32 sourceChannels = decoded.ChannelCount();
    const bool downmix =
        options.channelHandling == SoundImportChannelHandling::DownmixToMono && sourceChannels > 1;
    const uint32 channels = downmix ? 1 : sourceChannels;

    // 바꾸는 처리가 있으면 작업 버퍼에 담는다(downmix → normalize 순서, 3.36.3절).
    Array<float> work(allocator);
    const bool modifies = downmix || options.normalize;
    if (modifies) {
        if (!work.TryResize(static_cast<usize>(channels) * frames)) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }

        for (uint32 frame = 0; frame < frames; ++frame) {
            if (downmix) {
                float sum = 0.0f;
                for (uint32 channel = 0; channel < sourceChannels; ++channel) {
                    sum += decoded.Channel(channel)[frame];
                }

                work[frame] = sum / static_cast<float>(sourceChannels);
            } else {
                for (uint32 channel = 0; channel < channels; ++channel) {
                    work[static_cast<usize>(channel) * frames + frame] =
                        decoded.Channel(channel)[frame];
                }
            }
        }

        if (options.normalize) {
            float peak = 0.0f;
            for (const float sample : work) {
                peak = math::Max(peak, math::Abs(sample));
            }

            // 비정규수 peak에서 float 나눗셈이 inf가 되지 않도록 double로 계산하고, 그래도
            // 비유한이면 (사실상 무음) 건너뛴다.
            const double gain =
                peak > 0.0f
                    ? static_cast<double>(audio::DecibelsToLinear(options.normalizePeakDb)) /
                          static_cast<double>(peak)
                    : 0.0;
            if (peak > 0.0f && math::IsFinite(gain)) {
                for (float& sample : work) {
                    sample = static_cast<float>(static_cast<double>(sample) * gain);
                }
            }
        }
    }

    const SoundSourceStorage storage =
        modifies ? SoundSourceStorage::Float32 : StorageOf(decoded.SampleFormat());
    const uint32 bytesPerSample = SoundSourceStorageBytes(storage);
    Array<uint8> raw(allocator);
    if (!raw.TryResize(static_cast<usize>(bytesPerSample) * channels * frames)) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    for (uint32 channel = 0; channel < channels; ++channel) {
        const float* samples = modifies ? work.Data() + static_cast<usize>(channel) * frames
                                        : decoded.Channel(channel);
        uint8* out = raw.Data() + static_cast<usize>(channel) * frames * bytesPerSample;
        for (uint32 frame = 0; frame < frames; ++frame) {
            WriteSample(out + static_cast<usize>(frame) * bytesPerSample, samples[frame], storage);
        }
    }

    auto made = context.Output({}, info_.outputType);
    if (!made) {
        return Result<void>::Failure(made.Error());
    }

    const ObjectHandle main = made.Value();
    auto bulk = context.AddBulk(main, Move(raw));
    if (!bulk) {
        return Result<void>::Failure(bulk.Error());
    }

    auto borrowed = context.Registry().ResolveForWrite<SoundClip>(context.Access(), main);
    if (!borrowed) {
        return Result<void>::Failure(borrowed.Error());
    }

    auto value = borrowed.Value().Get();
    if (!value) {
        return Result<void>::Failure(value.Error());
    }

    // 임포트 결과 필드는 매번 전부 쓴다(A2f 계약). 사용자 설정은 새 임포트·재임포트 모두 둔다.
    SoundClip& clip = *value.Value();
    clip.channelCount = channels;
    clip.sampleRate = decoded.SampleRate();
    clip.frameCount = frames;
    clip.sourceCodec = CodecOf(decoded.Codec());
    clip.hasFileLoop = false;
    clip.fileLoopBegin = 0;
    clip.fileLoopEnd = 0;
    clip.cues.Clear();
    if (options.loopFromFile) {
        const Span<const audio::SourceLoop> loops = decoded.Loops();
        if (!loops.IsEmpty() && loops[0].start < loops[0].end && loops[0].end <= frames) {
            clip.hasFileLoop = true;
            clip.fileLoopBegin = static_cast<uint32>(loops[0].start);
            clip.fileLoopEnd = static_cast<uint32>(loops[0].end);
        }

        for (const audio::SourceCue& cue : decoded.Cues()) {
            SoundCuePoint point(allocator);
            point.id = cue.id;
            point.frame = cue.frame < frames ? static_cast<uint32>(cue.frame) : frames;
            if (!point.name.TryAppend(cue.name.View()) || !clip.cues.TryPushBack(Move(point))) {
                return Result<void>::Failure(ErrorCode::OutOfMemory);
            }
        }
    }

    clip.source.format = storage;
    clip.source.bulk = bulk.Value();
    if (decoded.IgnoredChunks() != 0) {
        if (auto warned =
                context.Warn(AssetDiagnosticCode::IgnoredSourceChunks, decoded.IgnoredChunks());
            !warned) {
            return warned;
        }
    }

    return Result<void>::Success();
}

Result<void> RegisterSoundClipImporter(void* state, AssetImporterTable& table,
                                       Array<AssetImporterRegistration>& tokens) noexcept {
    if (state == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (auto reserved = tokens.TryReserve(tokens.Size() + 1); !reserved) {
        return reserved;
    }

    auto registered = table.Register(*static_cast<SoundClipImporter*>(state));
    if (!registered) {
        return Result<void>::Failure(registered.Error());
    }

    (void)tokens.TryPushBack(Move(registered.Value()));
    return Result<void>::Success();
}

} // namespace rocks
