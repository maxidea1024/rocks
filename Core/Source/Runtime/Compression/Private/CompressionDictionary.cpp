#include "CompressionInternal.h"

#include <Core/Assert.h>
#include <Core/Move.h>

// 압축 사전 handle과 사전을 쓰는 공개 함수다(Docs/Design/Compression.ko.md 11절).
namespace rocks {
namespace detail {

DictionaryState::~DictionaryState() noexcept {
    if (codec == nullptr) {
        return;
    }

    // 코덱은 해제할 때도 memory(할당기)를 쓰므로 이 객체보다 먼저 정리된다.
    if (compressing) {
        zstd::DestroyCompressDictionary(codec);
    } else {
        zstd::DestroyDecompressDictionary(codec);
    }
}

} // namespace detail

namespace {

using DictionaryHandle = SharedPtr<detail::DictionaryState>;

bool InDictionaryRange(Span<const uint8> bytes) noexcept {
    return bytes.Size() >= MinDictionaryBytes && bytes.Size() <= MaxDictionaryBytes;
}

// 상태 블록을 만들고 내용 식별값을 채운다. 코덱은 호출자가 만든다(memory가 상태 안에 있어야 한다).
Result<DictionaryHandle> TryMakeState(CompressionFormat format, bool compressing, int32 level,
                                      Span<const uint8> bytes, Allocator& allocator) noexcept {
    auto made = DictionaryHandle::TryMake(allocator);
    if (!made) {
        return Result<DictionaryHandle>::Failure(made.Error());
    }

    detail::DictionaryState& state = *made.Value().Get();
    state.format = format;
    state.compressing = compressing;
    state.level = level;
    state.bytes = bytes.Size();
    state.digest = Sha256::Hash(bytes);
    state.memory.allocator = &allocator;
    return made;
}

CompressionSettings SettingsOf(const detail::DictionaryState& state) noexcept {
    auto settings = CompressionSettings::TryMake(state.format, state.level);
    ROCKS_ASSERT(settings.IsOk());
    return settings.Value();
}

} // namespace

Result<CompressDictionary> CompressDictionary::TryCreate(const CompressionSettings& settings,
                                                         Span<const uint8> bytes,
                                                         Allocator& allocator) noexcept {
    if (settings.Format() != CompressionFormat::Zstd) {
        return Result<CompressDictionary>::Failure(ErrorCode::Unsupported);
    }
    if (!InDictionaryRange(bytes)) {
        return Result<CompressDictionary>::Failure(ErrorCode::InvalidArgument);
    }

    auto made = TryMakeState(settings.Format(), true, settings.Level(), bytes, allocator);
    if (!made) {
        return Result<CompressDictionary>::Failure(made.Error());
    }

    detail::DictionaryState& state = *made.Value().Get();
    auto codec = detail::zstd::CreateCompressDictionary(bytes, settings.Level(), state.memory);
    if (!codec) {
        return Result<CompressDictionary>::Failure(codec.Error());
    }
    state.codec = codec.Value();
    state.id = detail::zstd::CompressDictionaryId(state.codec);

    CompressDictionary dictionary;
    dictionary.state_ = Move(made.Value());
    return Result<CompressDictionary>::Success(Move(dictionary));
}

CompressionSettings CompressDictionary::Settings() const noexcept {
    return SettingsOf(*state_);
}

uint32 CompressDictionary::Id() const noexcept {
    return state_->id;
}

const Sha256Digest& CompressDictionary::Digest() const noexcept {
    return state_->digest;
}

usize CompressDictionary::Size() const noexcept {
    return state_->bytes;
}

usize CompressDictionary::MemoryBytes() const noexcept {
    return sizeof(detail::DictionaryState) + detail::zstd::CompressDictionaryMemory(state_->codec);
}

Result<DecompressDictionary> DecompressDictionary::TryCreate(CompressionFormat format,
                                                             Span<const uint8> bytes,
                                                             Allocator& allocator) noexcept {
    if (format != CompressionFormat::Zstd) {
        // 아는 형식이면 사전을 아직 지원하지 않는 것이고, 모르는 값이면 인자 오류다.
        return Result<DecompressDictionary>::Failure(TryGetCompressionLevelRange(format).IsOk()
                                                         ? ErrorCode::Unsupported
                                                         : ErrorCode::InvalidArgument);
    }
    if (!InDictionaryRange(bytes)) {
        return Result<DecompressDictionary>::Failure(ErrorCode::InvalidArgument);
    }

    auto made = TryMakeState(format, false, 0, bytes, allocator);
    if (!made) {
        return Result<DecompressDictionary>::Failure(made.Error());
    }

    detail::DictionaryState& state = *made.Value().Get();
    auto codec = detail::zstd::CreateDecompressDictionary(bytes, state.memory);
    if (!codec) {
        return Result<DecompressDictionary>::Failure(codec.Error());
    }
    state.codec = codec.Value();
    state.id = detail::zstd::DecompressDictionaryId(state.codec);

    DecompressDictionary dictionary;
    dictionary.state_ = Move(made.Value());
    return Result<DecompressDictionary>::Success(Move(dictionary));
}

CompressionFormat DecompressDictionary::Format() const noexcept {
    return state_->format;
}

uint32 DecompressDictionary::Id() const noexcept {
    return state_->id;
}

const Sha256Digest& DecompressDictionary::Digest() const noexcept {
    return state_->digest;
}

usize DecompressDictionary::Size() const noexcept {
    return state_->bytes;
}

usize DecompressDictionary::MemoryBytes() const noexcept {
    return sizeof(detail::DictionaryState) +
           detail::zstd::DecompressDictionaryMemory(state_->codec);
}

Result<uint32> TryGetZstdFrameDictionaryId(Span<const uint8> frame) noexcept {
    return detail::zstd::FrameDictionaryId(frame);
}

// --- 사전을 쓰는 일회성 함수·문맥 ---

Result<usize> TryCompress(const CompressDictionary& dictionary, Span<const uint8> source,
                          Span<uint8> destination, Allocator& scratch) noexcept {
    const DictionaryHandle& state = detail::DictionaryAccess::State(dictionary);
    if (!state) {
        return Result<usize>::Failure(ErrorCode::StaleHandle);
    }
    if (detail::Overlaps(source.Data(), source.Size(), destination.Data(), destination.Size())) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    auto compressor =
        detail::CreateCompressorState(state->format, state->level, scratch, true, state);
    if (!compressor) {
        return Result<usize>::Failure(compressor.Error());
    }

    auto written =
        detail::CompressInto(*compressor.Value(), source, destination, detail::CompressionProbe{});
    detail::DestroyCompressorState(compressor.Value());
    return written;
}

Result<usize> TryCompressAppend(const CompressDictionary& dictionary, Span<const uint8> source,
                                Array<uint8>& output) noexcept {
    return TryCompressAppend(dictionary, source, output, output.GetAllocator());
}

Result<usize> TryCompressAppend(const CompressDictionary& dictionary, Span<const uint8> source,
                                Array<uint8>& output, Allocator& scratch) noexcept {
    const DictionaryHandle& state = detail::DictionaryAccess::State(dictionary);
    if (!state) {
        return Result<usize>::Failure(ErrorCode::StaleHandle);
    }

    return detail::CompressAppendOnce(SettingsOf(*state), source, output, scratch,
                                      detail::CompressionProbe{}, state);
}

Result<usize> TryDecompress(const DecompressDictionary& dictionary, Span<const uint8> source,
                            Span<uint8> destination, Allocator& scratch) noexcept {
    const DictionaryHandle& state = detail::DictionaryAccess::State(dictionary);
    if (!state) {
        return Result<usize>::Failure(ErrorCode::StaleHandle);
    }

    return detail::DecompressIntoOnce(state->format, source, destination, scratch,
                                      detail::CompressionProbe{}, state);
}

Result<usize> TryDecompressAppend(const DecompressDictionary& dictionary, Span<const uint8> source,
                                  usize maxOutputBytes, Array<uint8>& output) noexcept {
    return TryDecompressAppend(dictionary, source, maxOutputBytes, output, output.GetAllocator());
}

Result<usize> TryDecompressAppend(const DecompressDictionary& dictionary, Span<const uint8> source,
                                  usize maxOutputBytes, Array<uint8>& output,
                                  Allocator& scratch) noexcept {
    const DictionaryHandle& state = detail::DictionaryAccess::State(dictionary);
    if (!state) {
        return Result<usize>::Failure(ErrorCode::StaleHandle);
    }

    return detail::DecompressAppendOnce(state->format, source, maxOutputBytes, output, scratch,
                                        detail::CompressionProbe{}, state);
}

Result<Compressor> Compressor::TryCreate(const CompressDictionary& dictionary,
                                         Allocator& allocator) noexcept {
    const DictionaryHandle& state = detail::DictionaryAccess::State(dictionary);
    if (!state) {
        return Result<Compressor>::Failure(ErrorCode::StaleHandle);
    }

    auto created =
        detail::CreateCompressorState(state->format, state->level, allocator, true, state);
    if (!created) {
        return Result<Compressor>::Failure(created.Error());
    }

    return Result<Compressor>::Success(Compressor(created.Value()));
}

Result<Decompressor> Decompressor::TryCreate(const DecompressDictionary& dictionary,
                                             Allocator& allocator) noexcept {
    const DictionaryHandle& state = detail::DictionaryAccess::State(dictionary);
    if (!state) {
        return Result<Decompressor>::Failure(ErrorCode::StaleHandle);
    }

    auto created = detail::CreateDecompressorState(state->format, allocator, state);
    if (!created) {
        return Result<Decompressor>::Failure(created.Error());
    }

    return Result<Decompressor>::Success(Decompressor(created.Value()));
}

} // namespace rocks
