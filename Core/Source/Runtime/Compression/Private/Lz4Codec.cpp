#include "CompressionInternal.h"

// LZ4 1.11.1 adapter. 정의(LZ4_HC_STATIC_LINKING_ONLY: lz4hc.h가 LZ4_STATIC_LINKING_ONLY도
// 정의한다, LZ4_STATIC_LINKING_ONLY_DISABLE_MEMORY_ALLOCATION)는 모듈 규칙이 ThirdParty.json에서
// 받는다. 할당 API는 컴파일 단계에서 제거되어 있으므로 상태는 항상 Allocator로 직접 만든다.
#include <lz4.h>
#include <lz4hc.h>

namespace rocks::detail::lz4 {
namespace {

constexpr int32 FastLevel = 1;
constexpr usize IntMaximum = 0x7FFFFFFF;
constexpr usize AppendInitialBytes = usize{64} << 10;

// LZ4 sequence는 match 길이를 255 단위 byte로 늘리므로 압축 byte당 원본은 255byte를 넘지 못한다.
// 이를 넘는 출력 용량에서도 "출력 부족(-2)"이면 손상된 입력이다. 넉넉하게 256배 + 상수로
// 잡는다(과대 추정은 안전하다).
usize MaximumDecompressedBytes(usize sourceBytes) noexcept {
    if (sourceBytes > (IntMaximum - 64) / 256) {
        return IntMaximum;
    }
    return sourceBytes * 256 + 64;
}

int ClampCapacity(usize bytes) noexcept {
    return static_cast<int>(bytes > IntMaximum ? IntMaximum : bytes);
}

bool IsHighCompression(int32 level) noexcept {
    return level > FastLevel;
}

usize StateBytes(int32 level) noexcept {
    // HC 확장 상태는 수준 10–12의 optimal parser 작업 버퍼(~64KiB)를 상태 뒤에 포함해 압축 중
    // 할당이 없다.
    return IsHighCompression(level) ? static_cast<usize>(LZ4_STREAMHC_EXTSIZE)
                                    : static_cast<usize>(LZ4_sizeofState());
}

} // namespace

Result<usize> Bound(usize sourceBytes) noexcept {
    if (sourceBytes > Lz4MaxInputBytes) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<usize>::Success(
        static_cast<usize>(LZ4_compressBound(static_cast<int>(sourceBytes))));
}

Result<void*> CreateCompressor(int32 level, Allocator& allocator) noexcept {
    const usize bytes = StateBytes(level);
    void* state = CodecAllocate(allocator, bytes);
    if (state == nullptr) {
        return Result<void*>::Failure(ErrorCode::OutOfMemory);
    }

    // 상태는 한 번 초기화하고 이후에는 fastReset 압축으로 재사용한다.
    const bool initialized = IsHighCompression(level) ? LZ4_initStreamHC(state, bytes) != nullptr
                                                      : LZ4_initStream(state, bytes) != nullptr;
    if (!initialized) {
        CodecFree(allocator, state);
        return Result<void*>::Failure(ErrorCode::DependencyFailed);
    }

    return Result<void*>::Success(state);
}

void DestroyCompressor(void* codec, int32 level, Allocator& allocator) noexcept {
    (void)level;
    CodecFree(allocator, codec);
}

Result<usize> Compress(void* codec, int32 level, Span<const uint8> source,
                       Span<uint8> destination) noexcept {
    const char* input = reinterpret_cast<const char*>(NonNullSource(source));
    char* output = reinterpret_cast<char*>(NonNullDestination(destination));
    const int inputBytes = static_cast<int>(source.Size());
    const int capacity = ClampCapacity(destination.Size());

    int written = 0;
    if (IsHighCompression(level)) {
        written =
            LZ4_compress_HC_extStateHC_fastReset(codec, input, output, inputBytes, capacity, level);
    } else {
        written =
            LZ4_compress_fast_extState_fastReset(codec, input, output, inputBytes, capacity, 1);
    }
    if (written > 0) {
        return Result<usize>::Success(static_cast<usize>(written));
    }

    // 0은 출력 부족이다. 충분한 출력(bound 이상)에서도 0이면 계약 밖의 실패다.
    const usize bound = static_cast<usize>(LZ4_compressBound(inputBytes));
    return Result<usize>::Failure(static_cast<usize>(capacity) >= bound
                                      ? ErrorCode::DependencyFailed
                                      : ErrorCode::CapacityExceeded);
}

Result<usize> Decompress(Span<const uint8> source, Span<uint8> destination) noexcept {
    if (source.Size() > IntMaximum) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    const int capacity = ClampCapacity(destination.Size());
    const int result = LZ4_decompress_safe(reinterpret_cast<const char*>(NonNullSource(source)),
                                           reinterpret_cast<char*>(NonNullDestination(destination)),
                                           static_cast<int>(source.Size()), capacity);
    if (result >= 0) {
        return Result<usize>::Success(static_cast<usize>(result));
    }

    // -2는 "출력 부족 또는 손상"이다. 가능한 최대 출력 이상의 용량에서도 -2면 손상이다. 모르는
    // 음수는 손상이다.
    if (result == -2 && static_cast<usize>(capacity) < MaximumDecompressedBytes(source.Size()) &&
        destination.Size() <= IntMaximum) {
        return Result<usize>::Failure(ErrorCode::CapacityExceeded);
    }
    return Result<usize>::Failure(ErrorCode::InvalidArgument);
}

Result<usize> DecompressAppend(Span<const uint8> source, usize maxOutputBytes,
                               Array<uint8>& output) noexcept {
    if (source.Size() > IntMaximum) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    // raw block에는 원본 크기가 없다. 출력이 부족하면 두 배로 늘려 처음부터 다시 해제한다.
    const usize theoretical = MaximumDecompressedBytes(source.Size());
    const usize limit = maxOutputBytes < theoretical ? maxOutputBytes : theoretical;
    const usize base = output.Size();
    usize capacity = AppendInitialBytes;
    if (source.Size() <= limit / 4 && source.Size() * 4 > capacity) {
        capacity = source.Size() * 4;
    }
    if (capacity > limit) {
        capacity = limit;
    }

    while (true) {
        // 재시도 전 길이를 되돌린다(축소는 저장소 유지). 0으로 채우지 않는다.
        RestoreLength(output, base);
        auto grown = output.TryAppendUninitialized(capacity);
        if (!grown) {
            RestoreLength(output, base);
            return Result<usize>::Failure(grown.Error());
        }

        uint8* destination = NonNullDestination(Span<uint8>(output.Data() + base, capacity));
        const int result =
            LZ4_decompress_safe(reinterpret_cast<const char*>(NonNullSource(source)),
                                reinterpret_cast<char*>(destination),
                                static_cast<int>(source.Size()), static_cast<int>(capacity));
        if (result >= 0) {
            RestoreLength(output, base + static_cast<usize>(result));
            return Result<usize>::Success(static_cast<usize>(result));
        }
        if (result != -2 || capacity >= limit) {
            RestoreLength(output, base);
            // 상한(maxOutputBytes)이 이론 최대보다 작아 멈췄다면 상한 초과로 본다.
            const bool limitedByCaller =
                result == -2 && limit == maxOutputBytes && maxOutputBytes < theoretical;
            return Result<usize>::Failure(limitedByCaller ? ErrorCode::CapacityExceeded
                                                          : ErrorCode::InvalidArgument);
        }

        capacity = capacity > limit / 2 ? limit : capacity * 2;
    }
}

} // namespace rocks::detail::lz4
