#include "CompressionInternal.h"

#include <Core/Assert.h>
#include <Core/Move.h>

namespace rocks {
namespace detail {
namespace {

// 빈 Span의 대체 주소다. 코덱은 이 주소로 0 byte만 읽고 쓴다.
uint8 EmptyStorage = 0;

bool IsKnownFormat(CompressionFormat format) noexcept {
    switch (format) {
    case CompressionFormat::Lz4:
    case CompressionFormat::Zstd:
    case CompressionFormat::Deflate:
    case CompressionFormat::Zlib:
    case CompressionFormat::Gzip:
        return true;
    }
    return false;
}

bool IsZlibFamily(CompressionFormat format) noexcept {
    return format == CompressionFormat::Deflate || format == CompressionFormat::Zlib ||
           format == CompressionFormat::Gzip;
}

void DestroyCompressorCodec(CompressionFormat format, int32 level, void* codec,
                            Allocator& allocator) noexcept {
    if (format == CompressionFormat::Lz4) {
        lz4::DestroyCompressor(codec, level, allocator);
    } else if (format == CompressionFormat::Zstd) {
        zstd::DestroyCompressor(codec);
    } else if (IsZlibFamily(format)) {
        zlib::Destroy(codec);
    }
}

} // namespace

void* CodecAllocate(Allocator& allocator, usize bytes) noexcept {
    auto allocation = allocator.Allocate(bytes == 0 ? 1 : bytes, CodecAlignment);
    return allocation ? allocation.Value() : nullptr;
}

void CodecFree(Allocator& allocator, void* address) noexcept {
    if (address == nullptr) {
        return;
    }

    // 코덱이 받은 주소만 돌려주므로 해제 실패는 ledger 손상이나 다른 할당기 혼용이다.
    auto released = allocator.Deallocate(address);
    ROCKS_ASSERT(released.IsOk());
}

const uint8* NonNullSource(Span<const uint8> source) noexcept {
    return source.Data() != nullptr ? source.Data() : &EmptyStorage;
}

uint8* NonNullDestination(Span<uint8> destination) noexcept {
    return destination.Data() != nullptr ? destination.Data() : &EmptyStorage;
}

bool Overlaps(const void* first, usize firstSize, const void* second, usize secondSize) noexcept {
    if (firstSize == 0 || secondSize == 0) {
        return false;
    }

    const uptr firstBegin = reinterpret_cast<uptr>(first);
    const uptr secondBegin = reinterpret_cast<uptr>(second);
    return firstBegin < secondBegin + secondSize && secondBegin < firstBegin + firstSize;
}

bool OverlapsArrayStorage(Span<const uint8> source, const Array<uint8>& output) noexcept {
    return Overlaps(source.Data(), source.Size(), output.Data(), output.Capacity());
}

void RestoreLength(Array<uint8>& output, usize base) noexcept {
    // 축소는 저장소를 유지하며 실패하지 않는다(Array::TryResize 계약).
    auto restored = output.TryResize(base);
    ROCKS_ASSERT(restored.IsOk());
}

Result<CompressorState*> CreateCompressorState(CompressionFormat format, int32 level,
                                               Allocator& allocator) noexcept {
    Result<void*> codec = Result<void*>::Failure(ErrorCode::InvalidArgument);
    switch (format) {
    case CompressionFormat::Lz4:
        codec = lz4::CreateCompressor(level, allocator);
        break;
    case CompressionFormat::Zstd:
        codec = zstd::CreateCompressor(level, allocator);
        break;
    case CompressionFormat::Deflate:
    case CompressionFormat::Zlib:
    case CompressionFormat::Gzip:
        codec = zlib::CreateCompressor(format, level, allocator);
        break;
    }
    if (!codec) {
        return Result<CompressorState*>::Failure(codec.Error());
    }

    void* memory = CodecAllocate(allocator, sizeof(CompressorState));
    if (memory == nullptr) {
        // 상태 블록을 만들지 못했으므로 이미 만든 codec만 정리한다.
        DestroyCompressorCodec(format, level, codec.Value(), allocator);
        return Result<CompressorState*>::Failure(ErrorCode::OutOfMemory);
    }

    return Result<CompressorState*>::Success(ConstructAt<CompressorState>(
        memory, CompressorState{format, level, &allocator, codec.Value()}));
}

void DestroyCompressorState(CompressorState* state) noexcept {
    if (state == nullptr) {
        return;
    }

    Allocator& allocator = *state->allocator;
    DestroyCompressorCodec(state->format, state->level, state->codec, allocator);
    CodecFree(allocator, state);
}

Result<DecompressorState*> CreateDecompressorState(CompressionFormat format,
                                                   Allocator& allocator) noexcept {
    if (!IsKnownFormat(format)) {
        return Result<DecompressorState*>::Failure(ErrorCode::InvalidArgument);
    }

    void* codec = nullptr;
    if (format == CompressionFormat::Zstd) {
        auto created = zstd::CreateDecompressor(allocator);
        if (!created) {
            return Result<DecompressorState*>::Failure(created.Error());
        }
        codec = created.Value();
    } else if (IsZlibFamily(format)) {
        auto created = zlib::CreateDecompressor(format, allocator);
        if (!created) {
            return Result<DecompressorState*>::Failure(created.Error());
        }
        codec = created.Value();
    }

    void* memory = CodecAllocate(allocator, sizeof(DecompressorState));
    if (memory == nullptr) {
        if (format == CompressionFormat::Zstd) {
            zstd::DestroyDecompressor(codec);
        } else if (IsZlibFamily(format)) {
            zlib::Destroy(codec);
        }
        return Result<DecompressorState*>::Failure(ErrorCode::OutOfMemory);
    }

    return Result<DecompressorState*>::Success(
        ConstructAt<DecompressorState>(memory, DecompressorState{format, &allocator, codec}));
}

void DestroyDecompressorState(DecompressorState* state) noexcept {
    if (state == nullptr) {
        return;
    }

    Allocator& allocator = *state->allocator;
    if (state->format == CompressionFormat::Zstd) {
        zstd::DestroyDecompressor(state->codec);
    } else if (IsZlibFamily(state->format)) {
        zlib::Destroy(state->codec);
    }
    CodecFree(allocator, state);
}

Result<usize> CompressInto(CompressorState& state, Span<const uint8> source,
                           Span<uint8> destination, const CompressionProbe& probe) noexcept {
    if (Overlaps(source.Data(), source.Size(), destination.Data(), destination.Size())) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    switch (state.format) {
    case CompressionFormat::Lz4:
        if (source.Size() > Lz4MaxInputBytes) {
            return Result<usize>::Failure(ErrorCode::InvalidArgument);
        }
        if (probe.IsCancelled()) {
            return Result<usize>::Failure(ErrorCode::Cancelled);
        }
        return lz4::Compress(state.codec, state.level, source, destination);
    case CompressionFormat::Zstd:
        return zstd::Compress(state.codec, source, destination, probe);
    case CompressionFormat::Deflate:
    case CompressionFormat::Zlib:
    case CompressionFormat::Gzip:
        return zlib::Compress(state.codec, source, destination, probe);
    }
    return Result<usize>::Failure(ErrorCode::InvalidArgument);
}

Result<usize> CompressAppend(CompressorState& state, Span<const uint8> source, Array<uint8>& output,
                             const CompressionProbe& probe) noexcept {
    // 성장 재할당 전에 겹침을 확인한다. 이후 source는 output 저장소와 무관하다.
    if (OverlapsArrayStorage(source, output)) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    // zlib 계열은 문맥의 실제 설정(수준·window·memLevel)으로 구한 상한을 쓴다. 문맥 없는 공개
    // bound보다 훨씬 작아(약 13% → 0.03% 여유) 0으로 채우는 확보량이 줄어든다.
    auto bound = IsZlibFamily(state.format) ? zlib::StreamBound(state.codec, source.Size())
                                            : TryGetCompressBound(state.format, source.Size());
    if (!bound) {
        return Result<usize>::Failure(bound.Error());
    }

    const usize base = output.Size();
    if (bound.Value() > static_cast<usize>(-1) - base) {
        return Result<usize>::Failure(ErrorCode::Overflow);
    }

    auto grown = output.TryResize(base + bound.Value());
    if (!grown) {
        return Result<usize>::Failure(grown.Error());
    }

    auto written =
        CompressInto(state, source, Span<uint8>(output.Data() + base, bound.Value()), probe);
    if (!written) {
        RestoreLength(output, base);
        return written;
    }

    RestoreLength(output, base + written.Value());
    return written;
}

Result<usize> DecompressInto(DecompressorState& state, Span<const uint8> source,
                             Span<uint8> destination, const CompressionProbe& probe) noexcept {
    if (source.Size() == 0 ||
        Overlaps(source.Data(), source.Size(), destination.Data(), destination.Size())) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    switch (state.format) {
    case CompressionFormat::Lz4:
        if (probe.IsCancelled()) {
            return Result<usize>::Failure(ErrorCode::Cancelled);
        }
        return lz4::Decompress(source, destination);
    case CompressionFormat::Zstd:
        return zstd::Decompress(state.codec, source, destination, probe);
    case CompressionFormat::Deflate:
    case CompressionFormat::Zlib:
    case CompressionFormat::Gzip:
        return zlib::Decompress(state.codec, source, destination, probe);
    }
    return Result<usize>::Failure(ErrorCode::InvalidArgument);
}

Result<usize> DecompressAppend(DecompressorState& state, Span<const uint8> source,
                               usize maxOutputBytes, Array<uint8>& output,
                               const CompressionProbe& probe) noexcept {
    if (source.Size() == 0 || OverlapsArrayStorage(source, output)) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }
    if (maxOutputBytes > static_cast<usize>(-1) - output.Size()) {
        return Result<usize>::Failure(ErrorCode::Overflow);
    }

    switch (state.format) {
    case CompressionFormat::Lz4:
        if (probe.IsCancelled()) {
            return Result<usize>::Failure(ErrorCode::Cancelled);
        }
        return lz4::DecompressAppend(source, maxOutputBytes, output);
    case CompressionFormat::Zstd:
        return zstd::DecompressAppend(state.codec, source, maxOutputBytes, output, probe);
    case CompressionFormat::Deflate:
    case CompressionFormat::Zlib:
    case CompressionFormat::Gzip:
        return zlib::DecompressAppend(state.codec, source, maxOutputBytes, output, probe);
    }
    return Result<usize>::Failure(ErrorCode::InvalidArgument);
}

Result<usize> CompressAppendOnce(const CompressionSettings& settings, Span<const uint8> source,
                                 Array<uint8>& output, Allocator& scratch,
                                 const CompressionProbe& probe) noexcept {
    if (OverlapsArrayStorage(source, output)) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    auto state = CreateCompressorState(settings.Format(), settings.Level(), scratch);
    if (!state) {
        return Result<usize>::Failure(state.Error());
    }

    auto written = CompressAppend(*state.Value(), source, output, probe);
    DestroyCompressorState(state.Value());
    return written;
}

Result<usize> DecompressIntoOnce(CompressionFormat format, Span<const uint8> source,
                                 Span<uint8> destination, Allocator& scratch,
                                 const CompressionProbe& probe) noexcept {
    if (source.Size() == 0 ||
        Overlaps(source.Data(), source.Size(), destination.Data(), destination.Size())) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    // Lz4 해제는 코덱 상태가 없으므로 할당하지 않는다(가장 빠른 경로에 OOM 실패를 만들지 않는다).
    if (format == CompressionFormat::Lz4) {
        DecompressorState stateless{format, &scratch, nullptr};
        return DecompressInto(stateless, source, destination, probe);
    }

    auto state = CreateDecompressorState(format, scratch);
    if (!state) {
        return Result<usize>::Failure(state.Error());
    }

    auto written = DecompressInto(*state.Value(), source, destination, probe);
    DestroyDecompressorState(state.Value());
    return written;
}

Result<usize> DecompressAppendOnce(CompressionFormat format, Span<const uint8> source,
                                   usize maxOutputBytes, Array<uint8>& output, Allocator& scratch,
                                   const CompressionProbe& probe) noexcept {
    if (source.Size() == 0 || OverlapsArrayStorage(source, output)) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    if (format == CompressionFormat::Lz4) {
        DecompressorState stateless{format, &scratch, nullptr};
        return DecompressAppend(stateless, source, maxOutputBytes, output, probe);
    }

    auto state = CreateDecompressorState(format, scratch);
    if (!state) {
        return Result<usize>::Failure(state.Error());
    }

    auto written = DecompressAppend(*state.Value(), source, maxOutputBytes, output, probe);
    DestroyDecompressorState(state.Value());
    return written;
}

} // namespace detail

Result<CompressionLevelRange> TryGetCompressionLevelRange(CompressionFormat format) noexcept {
    switch (format) {
    case CompressionFormat::Lz4:
        return Result<CompressionLevelRange>::Success(CompressionLevelRange{1, 12, 1});
    case CompressionFormat::Zstd:
        return Result<CompressionLevelRange>::Success(CompressionLevelRange{1, 19, 3});
    case CompressionFormat::Deflate:
    case CompressionFormat::Zlib:
    case CompressionFormat::Gzip:
        return Result<CompressionLevelRange>::Success(CompressionLevelRange{0, 9, 6});
    }
    return Result<CompressionLevelRange>::Failure(ErrorCode::InvalidArgument);
}

Result<CompressionSettings> CompressionSettings::TryMake(CompressionFormat format,
                                                         int32 level) noexcept {
    auto range = TryGetCompressionLevelRange(format);
    if (!range) {
        return Result<CompressionSettings>::Failure(range.Error());
    }
    if (level < range.Value().minimum || level > range.Value().maximum) {
        return Result<CompressionSettings>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<CompressionSettings>::Success(CompressionSettings(format, level));
}

Result<CompressionSettings> CompressionSettings::TryDefault(CompressionFormat format) noexcept {
    auto range = TryGetCompressionLevelRange(format);
    if (!range) {
        return Result<CompressionSettings>::Failure(range.Error());
    }

    return Result<CompressionSettings>::Success(
        CompressionSettings(format, range.Value().defaultLevel));
}

Result<usize> TryGetCompressBound(CompressionFormat format, usize sourceBytes) noexcept {
    switch (format) {
    case CompressionFormat::Lz4:
        return detail::lz4::Bound(sourceBytes);
    case CompressionFormat::Zstd:
        return detail::zstd::Bound(sourceBytes);
    case CompressionFormat::Deflate:
    case CompressionFormat::Zlib:
    case CompressionFormat::Gzip:
        return detail::zlib::Bound(sourceBytes);
    }
    return Result<usize>::Failure(ErrorCode::InvalidArgument);
}

Result<usize> TryCompress(const CompressionSettings& settings, Span<const uint8> source,
                          Span<uint8> destination, Allocator& scratch) noexcept {
    if (detail::Overlaps(source.Data(), source.Size(), destination.Data(), destination.Size())) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    auto state = detail::CreateCompressorState(settings.Format(), settings.Level(), scratch);
    if (!state) {
        return Result<usize>::Failure(state.Error());
    }

    auto written =
        detail::CompressInto(*state.Value(), source, destination, detail::CompressionProbe{});
    detail::DestroyCompressorState(state.Value());
    return written;
}

Result<usize> TryCompressAppend(const CompressionSettings& settings, Span<const uint8> source,
                                Array<uint8>& output) noexcept {
    return detail::CompressAppendOnce(settings, source, output, output.GetAllocator(),
                                      detail::CompressionProbe{});
}

Result<usize> TryCompressAppend(const CompressionSettings& settings, Span<const uint8> source,
                                Array<uint8>& output, Allocator& scratch) noexcept {
    return detail::CompressAppendOnce(settings, source, output, scratch,
                                      detail::CompressionProbe{});
}

Result<usize> TryDecompress(CompressionFormat format, Span<const uint8> source,
                            Span<uint8> destination, Allocator& scratch) noexcept {
    return detail::DecompressIntoOnce(format, source, destination, scratch,
                                      detail::CompressionProbe{});
}

Result<usize> TryDecompressAppend(CompressionFormat format, Span<const uint8> source,
                                  usize maxOutputBytes, Array<uint8>& output) noexcept {
    return detail::DecompressAppendOnce(format, source, maxOutputBytes, output,
                                        output.GetAllocator(), detail::CompressionProbe{});
}

Result<usize> TryDecompressAppend(CompressionFormat format, Span<const uint8> source,
                                  usize maxOutputBytes, Array<uint8>& output,
                                  Allocator& scratch) noexcept {
    return detail::DecompressAppendOnce(format, source, maxOutputBytes, output, scratch,
                                        detail::CompressionProbe{});
}

Compressor::~Compressor() noexcept {
    Release();
}

Compressor::Compressor(Compressor&& other) noexcept : state_(other.state_) {
    other.state_ = nullptr;
}

Compressor& Compressor::operator=(Compressor&& other) noexcept {
    if (this != &other) {
        Release();
        state_ = other.state_;
        other.state_ = nullptr;
    }
    return *this;
}

void Compressor::Release() noexcept {
    detail::DestroyCompressorState(state_);
    state_ = nullptr;
}

Result<Compressor> Compressor::TryCreate(const CompressionSettings& settings,
                                         Allocator& allocator) noexcept {
    auto state = detail::CreateCompressorState(settings.Format(), settings.Level(), allocator);
    if (!state) {
        return Result<Compressor>::Failure(state.Error());
    }

    return Result<Compressor>::Success(Compressor(state.Value()));
}

Result<usize> Compressor::TryCompress(Span<const uint8> source, Span<uint8> destination) noexcept {
    if (state_ == nullptr) {
        return Result<usize>::Failure(ErrorCode::StaleHandle);
    }

    return detail::CompressInto(*state_, source, destination, detail::CompressionProbe{});
}

Result<usize> Compressor::TryCompressAppend(Span<const uint8> source,
                                            Array<uint8>& output) noexcept {
    if (state_ == nullptr) {
        return Result<usize>::Failure(ErrorCode::StaleHandle);
    }

    return detail::CompressAppend(*state_, source, output, detail::CompressionProbe{});
}

Decompressor::~Decompressor() noexcept {
    Release();
}

Decompressor::Decompressor(Decompressor&& other) noexcept : state_(other.state_) {
    other.state_ = nullptr;
}

Decompressor& Decompressor::operator=(Decompressor&& other) noexcept {
    if (this != &other) {
        Release();
        state_ = other.state_;
        other.state_ = nullptr;
    }
    return *this;
}

void Decompressor::Release() noexcept {
    detail::DestroyDecompressorState(state_);
    state_ = nullptr;
}

Result<Decompressor> Decompressor::TryCreate(CompressionFormat format,
                                             Allocator& allocator) noexcept {
    auto state = detail::CreateDecompressorState(format, allocator);
    if (!state) {
        return Result<Decompressor>::Failure(state.Error());
    }

    return Result<Decompressor>::Success(Decompressor(state.Value()));
}

Result<usize> Decompressor::TryDecompress(Span<const uint8> source,
                                          Span<uint8> destination) noexcept {
    if (state_ == nullptr) {
        return Result<usize>::Failure(ErrorCode::StaleHandle);
    }

    return detail::DecompressInto(*state_, source, destination, detail::CompressionProbe{});
}

Result<usize> Decompressor::TryDecompressAppend(Span<const uint8> source, usize maxOutputBytes,
                                                Array<uint8>& output) noexcept {
    if (state_ == nullptr) {
        return Result<usize>::Failure(ErrorCode::StaleHandle);
    }

    return detail::DecompressAppend(*state_, source, maxOutputBytes, output,
                                    detail::CompressionProbe{});
}

} // namespace rocks
