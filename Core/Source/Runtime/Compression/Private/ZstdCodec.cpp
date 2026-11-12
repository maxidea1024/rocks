#include "CompressionInternal.h"

// Zstandard 1.5.7 adapter. ZSTD_STATIC_LINKING_ONLY(사용자 할당기·stable buffer·decompressBound)는
// 모듈 규칙이 ThirdParty.json의 consumerDefines에서 정의한다. 정적 링크·고정 버전이라 실험 API를 쓸
// 수 있다.
#include <zstd.h>
#include <zstd_errors.h>

#include <Platform/Memory.h>

namespace rocks::detail::zstd {
namespace {

// stable input 압축은 ZSTD_e_continue 호출마다 입력을 분할 단위만큼 늘려 준다. 분할 단위가 최대
// 블록 크기(128KiB)의 배수여야 호출이 끝날 때 미소비 입력이 남지 않는다. 남으면
// ZSTD_CCtx_reset(session_only)이 그 값을 지우지 않아, 취소·실패 뒤 문맥을 재사용할 때 다음 세션의
// 입력 위치가 어긋난다.
static_assert(CompressionChunkBytes % ZSTD_BLOCKSIZE_MAX == 0,
              "compression chunk must be a multiple of the zstd maximum block size");

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
    case ZSTD_error_frameParameter_windowTooLarge:
        // RFC 8878은 2^31보다 큰 window도 허용하지만 이 구현의 상한(ZSTD_WINDOWLOG_MAX)을 넘는다.
        return ErrorCode::Unsupported;
    case ZSTD_error_stage_wrong:
    case ZSTD_error_init_missing:
    case ZSTD_error_parameter_unsupported:
    case ZSTD_error_parameter_outOfBound:
    case ZSTD_error_stabilityCondition_notRespected:
    case ZSTD_error_dstBuffer_null:
    case ZSTD_error_dstBuffer_wrong:
    case ZSTD_error_srcBuffer_wrong:
    case ZSTD_error_workSpace_tooSmall:
    case ZSTD_error_noForwardProgress_destFull:
    case ZSTD_error_noForwardProgress_inputEmpty:
        // adapter가 API 계약을 어긴 경우다(입력 데이터 탓이 아니다).
        return ErrorCode::DependencyFailed;
    default:
        // GENERIC을 포함한 나머지는 입력 손상이다(예: FSE 정규화 표가 틀리면 GENERIC).
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

Result<void*> CreateCompressor(int32 level, Allocator& allocator, bool checksum) noexcept {
    ZSTD_CCtx* context = ZSTD_createCCtx_advanced(Memory(allocator));
    if (context == nullptr) {
        return Result<void*>::Failure(ErrorCode::OutOfMemory);
    }

    // 결정성: 수준·checksum·원본 크기를 고정하고 사전 ID는 쓰지 않는다. stable buffer는 입력·출력
    // 복사본을 만들지 않는다(입력은 분할 단위로 늘려 주며 출력 부족은 dstSize_tooSmall로 보고된다).
    const bool configured =
        SetCompressionParameter(context, ZSTD_c_compressionLevel, level) &&
        SetCompressionParameter(context, ZSTD_c_checksumFlag, checksum ? 1 : 0) &&
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
    auto grown = output.TryAppendUninitialized(capacity);
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

// --- 스트리밍(10절) ---

namespace {

// 스트림 해제의 오류 분류다. 스트림 해제기는 내부 버퍼에 먼저 풀어 호출자 출력으로 나눠 내보내므로,
// dstSize_tooSmall은 출력 부족이 아니라 블록이 frame 정보(원본 크기·블록 최대 크기)를 어긴
// 손상이다.
ErrorCode StreamDecodeError(size_t code) noexcept {
    switch (ZSTD_getErrorCode(code)) {
    case ZSTD_error_dstSize_tooSmall:
        return ErrorCode::InvalidArgument;
    case ZSTD_error_frameParameter_windowTooLarge:
        // 헤더를 미리 검사하므로 오지 않는다. 오면 호출자 상한 초과다.
        return ErrorCode::CapacityExceeded;
    default:
        return DecompressionError(code);
    }
}

} // namespace

Result<void*> CreateStreamCompressor(int32 level, uint64 sizeHint, Allocator& allocator) noexcept {
    ZSTD_CCtx* context = ZSTD_createCCtx_advanced(Memory(allocator));
    if (context == nullptr) {
        return Result<void*>::Failure(ErrorCode::OutOfMemory);
    }

    // 스트림은 입력·출력 버퍼가 호출마다 바뀌므로 stable buffer를 쓰지 않는다. 크기 힌트는 매개변수
    // 선택에만 쓰고 frame에 기록하지 않는다(원본 크기는 모름).
    const int hint = sizeHint > 0x7fffffffull ? 0x7fffffff : static_cast<int>(sizeHint);
    const bool configured =
        SetCompressionParameter(context, ZSTD_c_compressionLevel, level) &&
        SetCompressionParameter(context, ZSTD_c_checksumFlag, 1) &&
        SetCompressionParameter(context, ZSTD_c_dictIDFlag, 0) &&
        (hint == 0 || SetCompressionParameter(context, ZSTD_c_srcSizeHint, hint));
    if (!configured) {
        ZSTD_freeCCtx(context);
        return Result<void*>::Failure(ErrorCode::DependencyFailed);
    }

    return Result<void*>::Success(context);
}

Result<void*> CreateStreamDecompressor(uint32 maxWindowLog, Allocator& allocator) noexcept {
    ZSTD_DCtx* context = ZSTD_createDCtx_advanced(Memory(allocator));
    if (context == nullptr) {
        return Result<void*>::Failure(ErrorCode::OutOfMemory);
    }

    if (ZSTD_isError(
            ZSTD_DCtx_setParameter(context, ZSTD_d_windowLogMax, static_cast<int>(maxWindowLog)))) {
        ZSTD_freeDCtx(context);
        return Result<void*>::Failure(ErrorCode::DependencyFailed);
    }

    return Result<void*>::Success(context);
}

void ResetStreamCompressor(void* codec) noexcept {
    ZSTD_CCtx_reset(static_cast<ZSTD_CCtx*>(codec), ZSTD_reset_session_only);
}

void ResetStreamDecompressor(void* codec) noexcept {
    ZSTD_DCtx_reset(static_cast<ZSTD_DCtx*>(codec), ZSTD_reset_session_only);
}

Result<StreamProgress> CompressStep(void* codec, Span<const uint8> input, Span<uint8> output,
                                    StreamFlush flush) noexcept {
    auto* context = static_cast<ZSTD_CCtx*>(codec);
    const ZSTD_EndDirective mode = flush == StreamFlush::End    ? ZSTD_e_end
                                   : flush == StreamFlush::Sync ? ZSTD_e_flush
                                                                : ZSTD_e_continue;
    ZSTD_inBuffer in{NonNullSource(input), input.Size(), 0};
    ZSTD_outBuffer out{NonNullDestination(output), output.Size(), 0};
    StreamProgress progress;
    while (out.pos < out.size) {
        const usize inBefore = in.pos;
        const usize outBefore = out.pos;
        const size_t remaining = ZSTD_compressStream2(context, &out, &in, mode);
        if (ZSTD_isError(remaining)) {
            return Result<StreamProgress>::Failure(CompressionError(remaining));
        }
        // continue는 입력을 모두 소비하면, flush·end는 내보낼 것이 없으면 끝이다.
        if (flush == StreamFlush::None ? in.pos == in.size : remaining == 0) {
            progress.complete = true;
            break;
        }
        if (in.pos == inBefore && out.pos == outBefore) {
            break;
        }
    }

    progress.consumed = in.pos;
    progress.produced = out.pos;
    return Result<StreamProgress>::Success(progress);
}

Result<StreamProgress> DecompressStep(void* codec, Span<const uint8> input, Span<uint8> output,
                                      uint32 maxWindowLog, StreamDecodeState& state) noexcept {
    auto* context = static_cast<ZSTD_DCtx*>(codec);
    const uint8* source = NonNullSource(input);
    usize consumed = 0;
    usize produced = 0;
    // zstd가 내보내지 못한 출력을 갖고 있을 수 있는지다. 출력을 가득 채우고 frame이 끝나지 않았을
    // 때만 참이다(frame 끝의 반환값 0은 모두 내보냈다는 뜻이다).
    bool pending = false;
    while (produced < output.Size()) {
        if (state.atFrameStart) {
            if (consumed == input.Size()) {
                break;
            }
            // frame 헤더를 먼저 모아 window를 검사한다. 헤더 byte도 새 frame의 입력이므로 경계를
            // 지운다.
            const usize need = sizeof(state.header) - state.headerBytes;
            const usize take = Minimum(need, input.Size() - consumed);
            ZSTD_FrameHeader header{};
            platform::MemoryCopy(state.header + state.headerBytes, source + consumed, take);
            const size_t parsed =
                ZSTD_getFrameHeader(&header, state.header, state.headerBytes + take);
            if (ZSTD_isError(parsed)) {
                return Result<StreamProgress>::Failure(
                    ZSTD_getErrorCode(parsed) == ZSTD_error_frameParameter_windowTooLarge
                        ? ErrorCode::Unsupported
                        : ErrorCode::InvalidArgument);
            }
            state.atBoundary = false;
            if (parsed != 0) {
                // 헤더가 아직 덜 왔다. 받은 만큼 보관하고 소비한다.
                state.headerBytes += static_cast<uint32>(take);
                consumed += take;
                continue;
            }
            // window(single segment frame은 원본 크기)가 구현 상한 2^31을 넘으면 어떤 설정으로도 풀
            // 수 없어 Unsupported, 호출자 상한만 넘으면 CapacityExceeded다.
            if (header.frameType == ZSTD_frame) {
                if (header.windowSize > (1ull << MaxZstdWindowLog)) {
                    return Result<StreamProgress>::Failure(ErrorCode::Unsupported);
                }
                if (header.windowSize > (1ull << maxWindowLog)) {
                    return Result<StreamProgress>::Failure(ErrorCode::CapacityExceeded);
                }
            }

            // 검사한 헤더 byte(보관분 + 이번 입력에서 헤더까지)를 zstd에 먼저 넣는다. 헤더를 따로
            // 넣으므로 frame 전체가 한 입력에 들어 있지 않아 zstd의 단일 패스 경로(상한 검사
            // 생략)도 타지 않는다.
            const usize headerFromInput = header.headerSize - state.headerBytes;
            ZSTD_inBuffer in{state.header, header.headerSize, 0};
            ZSTD_outBuffer out{output.Data() + produced, output.Size() - produced, 0};
            const size_t hint = ZSTD_decompressStream(context, &out, &in);
            if (ZSTD_isError(hint)) {
                return Result<StreamProgress>::Failure(StreamDecodeError(hint));
            }
            produced += out.pos;
            consumed += headerFromInput;
            state.headerBytes = 0;
            state.atFrameStart = false;
            pending = false;
            if (hint == 0) {
                // 헤더만으로 끝나는 frame(빈 skippable 등).
                state.atFrameStart = true;
                state.atBoundary = true;
                state.sawStream = true;
            }
            continue;
        }

        ZSTD_inBuffer in{source + consumed, input.Size() - consumed, 0};
        ZSTD_outBuffer out{output.Data() + produced, output.Size() - produced, 0};
        const size_t hint = ZSTD_decompressStream(context, &out, &in);
        if (ZSTD_isError(hint)) {
            return Result<StreamProgress>::Failure(StreamDecodeError(hint));
        }
        consumed += in.pos;
        produced += out.pos;
        if (hint == 0) {
            pending = false;
            state.atFrameStart = true;
            state.atBoundary = true;
            state.sawStream = true;
            continue;
        }
        pending = out.pos == out.size;
        // 입력을 다 넣었고 출력 공간이 남았다면 zstd가 가진 출력도 다 나왔다. 빈 입력으로 다시
        // 부르지 않는다(무진행 호출 계수를 늘리지 않는다).
        if (consumed == input.Size() && !pending) {
            break;
        }
        if (in.pos == 0 && out.pos == 0) {
            break;
        }
    }

    state.outputPending = pending;
    StreamProgress progress;
    progress.consumed = consumed;
    progress.produced = produced;
    progress.complete = consumed == input.Size() && !pending;
    return Result<StreamProgress>::Success(progress);
}

} // namespace rocks::detail::zstd
