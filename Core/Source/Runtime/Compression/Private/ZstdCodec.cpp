#include "CompressionInternal.h"

// Zstandard 1.5.7 adapter. ZSTD_STATIC_LINKING_ONLY(사용자 할당기·stable buffer·decompressBound)는
// 모듈 규칙이 ThirdParty.json의 consumerDefines에서 정의한다. 정적 링크·고정 버전이라 실험 API를 쓸
// 수 있다.
#include <zstd.h>
#include <zstd_errors.h>

namespace rocks::detail::zstd {
namespace {

void* Allocate(void* opaque, size_t bytes) {
    return CodecAllocate(*static_cast<Allocator*>(opaque), bytes);
}

void Free(void* opaque, void* address) {
    CodecFree(*static_cast<Allocator*>(opaque), address);
}

ZSTD_customMem Memory(Allocator& allocator) noexcept {
    return ZSTD_customMem{&Allocate, &Free, &allocator};
}

bool SetCompressionParameter(ZSTD_CCtx* context, ZSTD_cParameter parameter, int value) noexcept {
    return !ZSTD_isError(ZSTD_CCtx_setParameter(context, parameter, value));
}

// 압축 경로의 오류다. 입력 데이터가 원인일 수 없으므로 출력 부족·OOM 외에는 계약 밖의 실패다.
ErrorCode CompressionError(size_t code) noexcept {
    switch (ZSTD_getErrorCode(code)) {
    case ZSTD_error_dstSize_tooSmall:
        return ErrorCode::CapacityExceeded;
    case ZSTD_error_memory_allocation:
        return ErrorCode::OutOfMemory;
    default:
        return ErrorCode::DependencyFailed;
    }
}

// 해제 경로의 오류다. 출력 부족은 호출자가 크기 정보로 다시 분류한다. 그 밖의 대부분은 입력
// 손상이다.
ErrorCode DecompressionError(size_t code) noexcept {
    switch (ZSTD_getErrorCode(code)) {
    case ZSTD_error_dstSize_tooSmall:
        return ErrorCode::CapacityExceeded;
    case ZSTD_error_memory_allocation:
        return ErrorCode::OutOfMemory;
    case ZSTD_error_dictionary_wrong:
    case ZSTD_error_dictionary_corrupted:
        return ErrorCode::Unsupported;
    case ZSTD_error_stage_wrong:
    case ZSTD_error_init_missing:
    case ZSTD_error_parameter_unsupported:
    case ZSTD_error_parameter_outOfBound:
        return ErrorCode::DependencyFailed;
    default:
        return ErrorCode::InvalidArgument;
    }
}

usize Minimum(usize first, usize second) noexcept {
    return first < second ? first : second;
}

// 크기를 아는 해제의 공통 루프다. stable output이므로 window 메모리를 할당하지 않는다.
// authoritativeCapacity: 출력 용량이 원본 크기 이상임이 frame 정보로 보장되면 true이며, 이때 출력
// 부족은 거짓 크기 헤더(손상)다.
Result<usize> DecodeInto(ZSTD_DCtx* context, Span<const uint8> source, Span<uint8> destination,
                         bool authoritativeCapacity, const CompressionProbe& probe) noexcept {
    ZSTD_DCtx_reset(context, ZSTD_reset_session_only);
    ZSTD_inBuffer input{NonNullSource(source), 0, 0};
    ZSTD_outBuffer output{NonNullDestination(destination), destination.Size(), 0};

    while (true) {
        if (probe.IsCancelled()) {
            return Result<usize>::Failure(ErrorCode::Cancelled);
        }

        input.size = Minimum(source.Size(), input.pos + CompressionChunkBytes);
        const usize inputBefore = input.pos;
        const usize outputBefore = output.pos;
        const size_t hint = ZSTD_decompressStream(context, &output, &input);
        if (ZSTD_isError(hint)) {
            ErrorCode error = DecompressionError(hint);
            if (error == ErrorCode::CapacityExceeded && authoritativeCapacity) {
                error = ErrorCode::InvalidArgument;
            }
            return Result<usize>::Failure(error);
        }

        const bool consumedAll = input.pos == source.Size();
        if (consumedAll && hint == 0) {
            return Result<usize>::Success(output.pos);
        }
        if (input.pos == inputBefore && output.pos == outputBefore) {
            // 진행이 없다. 출력이 가득 찼으면 용량 부족(크기 정보가 보장한 용량이면 손상), 입력을
            // 모두 넣었으면 절단이다.
            const bool full = output.pos == output.size;
            if (full && !authoritativeCapacity && !consumedAll) {
                return Result<usize>::Failure(ErrorCode::CapacityExceeded);
            }
            return Result<usize>::Failure(ErrorCode::InvalidArgument);
        }
    }
}

} // namespace

Result<usize> Bound(usize sourceBytes) noexcept {
    const size_t bound = ZSTD_compressBound(sourceBytes);
    if (ZSTD_isError(bound) || bound < sourceBytes) {
        return Result<usize>::Failure(ErrorCode::Overflow);
    }

    return Result<usize>::Success(bound);
}

Result<void*> CreateCompressor(int32 level, Allocator& allocator) noexcept {
    ZSTD_CCtx* context = ZSTD_createCCtx_advanced(Memory(allocator));
    if (context == nullptr) {
        return Result<void*>::Failure(ErrorCode::OutOfMemory);
    }

    // 결정성: 수준·checksum·원본 크기를 고정하고 사전 ID는 쓰지 않는다. stable buffer는 입력·출력
    // 복사본을 만들지 않는다(입력은 분할 단위로 늘려 주며 출력 부족은 dstSize_tooSmall로 보고된다).
    const bool configured = SetCompressionParameter(context, ZSTD_c_compressionLevel, level) &&
                            SetCompressionParameter(context, ZSTD_c_checksumFlag, 1) &&
                            SetCompressionParameter(context, ZSTD_c_contentSizeFlag, 1) &&
                            SetCompressionParameter(context, ZSTD_c_dictIDFlag, 0) &&
                            SetCompressionParameter(context, ZSTD_c_stableInBuffer, 1) &&
                            SetCompressionParameter(context, ZSTD_c_stableOutBuffer, 1);
    if (!configured) {
        ZSTD_freeCCtx(context);
        return Result<void*>::Failure(ErrorCode::DependencyFailed);
    }

    return Result<void*>::Success(context);
}

void DestroyCompressor(void* codec) noexcept {
    ZSTD_freeCCtx(static_cast<ZSTD_CCtx*>(codec));
}

Result<void*> CreateDecompressor(Allocator& allocator) noexcept {
    ZSTD_DCtx* context = ZSTD_createDCtx_advanced(Memory(allocator));
    if (context == nullptr) {
        return Result<void*>::Failure(ErrorCode::OutOfMemory);
    }

    // stable output은 출력 버퍼를 window로 쓰므로 window 메모리를 할당하지 않는다. 그래서 유효한 큰
    // window frame도 받을 수 있게 window 상한을 최대로 둔다(메모리 상한은 출력 용량이 정한다).
    const bool configured =
        !ZSTD_isError(ZSTD_DCtx_setParameter(context, ZSTD_d_windowLogMax, ZSTD_WINDOWLOG_MAX)) &&
        !ZSTD_isError(ZSTD_DCtx_setParameter(context, ZSTD_d_stableOutBuffer, 1));
    if (!configured) {
        ZSTD_freeDCtx(context);
        return Result<void*>::Failure(ErrorCode::DependencyFailed);
    }

    return Result<void*>::Success(context);
}

void DestroyDecompressor(void* codec) noexcept {
    ZSTD_freeDCtx(static_cast<ZSTD_DCtx*>(codec));
}

Result<usize> Compress(void* codec, Span<const uint8> source, Span<uint8> destination,
                       const CompressionProbe& probe) noexcept {
    auto* context = static_cast<ZSTD_CCtx*>(codec);

    // 이전 호출의 실패·취소 상태를 지우고 원본 크기를 고정한다(frame header에 기록된다).
    ZSTD_CCtx_reset(context, ZSTD_reset_session_only);
    if (ZSTD_isError(ZSTD_CCtx_setPledgedSrcSize(context, source.Size()))) {
        return Result<usize>::Failure(ErrorCode::DependencyFailed);
    }

    ZSTD_inBuffer input{NonNullSource(source), 0, 0};
    ZSTD_outBuffer output{NonNullDestination(destination), destination.Size(), 0};
    while (true) {
        if (probe.IsCancelled()) {
            ZSTD_CCtx_reset(context, ZSTD_reset_session_only);
            return Result<usize>::Failure(ErrorCode::Cancelled);
        }

        // stable input은 같은 주소에서 size만 늘려 줄 수 있다. 분할 단위는 동기·비동기가 같다.
        input.size = Minimum(source.Size(), input.pos + CompressionChunkBytes);
        const ZSTD_EndDirective mode = input.size == source.Size() ? ZSTD_e_end : ZSTD_e_continue;
        const usize inputBefore = input.pos;
        const usize outputBefore = output.pos;
        const size_t remaining = ZSTD_compressStream2(context, &output, &input, mode);
        if (ZSTD_isError(remaining)) {
            ZSTD_CCtx_reset(context, ZSTD_reset_session_only);
            return Result<usize>::Failure(CompressionError(remaining));
        }
        if (mode == ZSTD_e_end && remaining == 0) {
            return Result<usize>::Success(output.pos);
        }
        if (input.pos == inputBefore && output.pos == outputBefore) {
            // 진행이 없다. 출력이 가득 찼으면 용량 부족, 아니면 계약 밖의 상태다(무한 반복 방지).
            ZSTD_CCtx_reset(context, ZSTD_reset_session_only);
            return Result<usize>::Failure(output.pos == output.size ? ErrorCode::CapacityExceeded
                                                                    : ErrorCode::DependencyFailed);
        }
    }
}

Result<usize> Decompress(void* codec, Span<const uint8> source, Span<uint8> destination,
                         const CompressionProbe& probe) noexcept {
    // 모든 frame의 원본 크기가 기록되어 있으면 해제 전에 용량을 확인한다(skippable frame은 0).
    const unsigned long long total = ZSTD_findDecompressedSize(source.Data(), source.Size());
    if (total == ZSTD_CONTENTSIZE_ERROR) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    const bool known = total != ZSTD_CONTENTSIZE_UNKNOWN;
    if (known && total > destination.Size()) {
        return Result<usize>::Failure(ErrorCode::CapacityExceeded);
    }

    return DecodeInto(static_cast<ZSTD_DCtx*>(codec), source, destination, known, probe);
}

Result<usize> DecompressAppend(void* codec, Span<const uint8> source, usize maxOutputBytes,
                               Array<uint8>& output, const CompressionProbe& probe) noexcept {
    const unsigned long long total = ZSTD_findDecompressedSize(source.Data(), source.Size());
    if (total == ZSTD_CONTENTSIZE_ERROR) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    // 원본 크기가 모두 기록되어 있으면 정확히 그만큼, 아니면 블록 수로 구한 상한(블록당 최대
    // 128KiB)과 호출자 상한 중 작은 값을 한 번에 확보한다. 재할당·재시작이 없다.
    usize capacity = 0;
    bool authoritative = false;
    if (total != ZSTD_CONTENTSIZE_UNKNOWN) {
        if (total > maxOutputBytes) {
            return Result<usize>::Failure(ErrorCode::CapacityExceeded);
        }
        capacity = static_cast<usize>(total);
        authoritative = true;
    } else {
        const unsigned long long bound = ZSTD_decompressBound(source.Data(), source.Size());
        if (bound == ZSTD_CONTENTSIZE_ERROR) {
            return Result<usize>::Failure(ErrorCode::InvalidArgument);
        }
        capacity = bound < maxOutputBytes ? static_cast<usize>(bound) : maxOutputBytes;
        authoritative = bound <= maxOutputBytes;
    }

    const usize base = output.Size();
    auto grown = output.TryResize(base + capacity);
    if (!grown) {
        return Result<usize>::Failure(grown.Error());
    }

    auto written = DecodeInto(static_cast<ZSTD_DCtx*>(codec), source,
                              Span<uint8>(output.Data() + base, capacity), authoritative, probe);
    if (!written) {
        RestoreLength(output, base);
        return written;
    }

    RestoreLength(output, base + written.Value());
    return written;
}

} // namespace rocks::detail::zstd
