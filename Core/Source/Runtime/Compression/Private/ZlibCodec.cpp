#include "CompressionInternal.h"

#include <Core/Move.h>

// zlib 1.4 adapter. Z_SOLO·Z_PREFIX·Z_U4·Z_U8은 모듈 규칙이 ThirdParty.json의 consumerDefines에서
// 정의한다 (라이브러리 빌드와 같아야 한다). Z_SOLO라 기본 할당기가 없으므로 zalloc·zfree를 반드시
// 넘긴다.
#include <zlib.h>

namespace rocks::detail::zlib {
namespace {

constexpr int WindowBits = 15;
constexpr int MemoryLevel = 8;
constexpr usize AppendInitialBytes = usize{64} << 10;

// z_stream과 gzip 헤더는 zlib이 주소를 보관하므로(state->strm 역참조, gzhead 포인터) heap에
// 고정한다.
struct ZlibState {
    z_stream stream;
    gz_header header;
    Allocator* allocator;
    CompressionFormat format;
    bool deflating;
};

voidpf Allocate(voidpf opaque, uInt items, uInt size) {
    // uInt 곱은 64bit usize에서 넘치지 않는다.
    return CodecAllocate(*static_cast<Allocator*>(opaque), static_cast<usize>(items) * size);
}

void Free(voidpf opaque, voidpf address) {
    CodecFree(*static_cast<Allocator*>(opaque), address);
}

int WindowBitsFor(CompressionFormat format) noexcept {
    switch (format) {
    case CompressionFormat::Deflate:
        return -WindowBits;
    case CompressionFormat::Gzip:
        return WindowBits + 16;
    default:
        return WindowBits;
    }
}

usize Minimum(usize first, usize second) noexcept {
    return first < second ? first : second;
}

ErrorCode StatusError(int status) noexcept {
    switch (status) {
    case Z_MEM_ERROR:
        return ErrorCode::OutOfMemory;
    case Z_DATA_ERROR:
        return ErrorCode::InvalidArgument;
    case Z_NEED_DICT:
        return ErrorCode::Unsupported;
    default:
        return ErrorCode::DependencyFailed;
    }
}

Result<ZlibState*> CreateState(CompressionFormat format, Allocator& allocator,
                               bool deflating) noexcept {
    void* memory = CodecAllocate(allocator, sizeof(ZlibState));
    if (memory == nullptr) {
        return Result<ZlibState*>::Failure(ErrorCode::OutOfMemory);
    }

    ZlibState* state = ConstructAt<ZlibState>(memory);
    *state = ZlibState{};
    state->allocator = &allocator;
    state->format = format;
    state->deflating = deflating;
    state->stream.zalloc = &Allocate;
    state->stream.zfree = &Free;
    state->stream.opaque = &allocator;
    return Result<ZlibState*>::Success(state);
}

// 결정적 gzip 헤더: mtime 0, OS 255(unknown), 이름·주석·extra·FHCRC 없음. zlib 기본 OS 코드는
// 플랫폼마다 다르고 1.4의 deflateReset은 헤더 설정을 지우므로 매 압축 전에 다시 적용한다.
bool ApplyGzipHeader(ZlibState& state) noexcept {
    if (state.format != CompressionFormat::Gzip) {
        return true;
    }

    state.header = gz_header{};
    state.header.os = 255;
    return deflateSetHeader(&state.stream, &state.header) == Z_OK;
}

// 해제 공통 루프의 출력 대상이다. fixed면 Span, 아니면 Array 뒤를 상한까지 점진적으로 늘린다.
struct InflateOutput {
    uint8* fixed = nullptr;
    Array<uint8>* array = nullptr;
    usize base = 0;
    usize limit = 0;
    usize produced = 0;
    usize writable = 0;

    uint8* Cursor() noexcept {
        if (array != nullptr) {
            return array->Data() + base + produced;
        }
        return fixed + produced;
    }

    // 쓸 공간이 없으면 늘린다. 상한에 도달했으면 아무것도 하지 않는다.
    Result<void> Reserve() noexcept {
        if (array == nullptr || writable != 0 || produced == limit) {
            return Result<void>::Success();
        }

        usize growth = produced > AppendInitialBytes ? produced : AppendInitialBytes;
        growth = Minimum(growth, limit - produced);
        auto grown = array->TryResize(base + produced + growth);
        if (!grown) {
            return grown;
        }
        writable = growth;
        return Result<void>::Success();
    }
};

// Deflate·Zlib·Gzip 해제 공통 루프다.
// - 입력·출력을 분할 단위로 넣어 uInt 한계와 취소 확인을 처리한다.
// - 출력이 상한에 이르면 1byte 예비 버퍼로 계속 진행한다. 1byte라도 나오면 상한 초과, 블록
// 끝·체크섬만 남았으면
//   성공이다(이 상태들은 출력 공간 없이 진행된다). 그래서 정확히 상한 크기인 결과는 성공한다.
// - Gzip은 연속 member를 모두 해제한다. member 뒤의 쓰레기·0 padding과 다른 형식의 남는 byte는
// 손상이다.
Result<void> Inflate(ZlibState& state, Span<const uint8> source, InflateOutput& output,
                     const CompressionProbe& probe) noexcept {
    z_stream& stream = state.stream;
    if (inflateReset(&stream) != Z_OK) {
        return Result<void>::Failure(ErrorCode::DependencyFailed);
    }

    const uint8* input = NonNullSource(source);
    usize consumed = 0;
    uint8 spare = 0;
    while (true) {
        if (probe.IsCancelled()) {
            return Result<void>::Failure(ErrorCode::Cancelled);
        }

        auto reserved = output.Reserve();
        if (!reserved) {
            return reserved;
        }

        const bool probing = output.writable == 0;
        const usize inputChunk = Minimum(source.Size() - consumed, CompressionChunkBytes);
        const usize outputChunk = probing ? 1 : Minimum(output.writable, CompressionChunkBytes);
        stream.next_in = const_cast<Bytef*>(input + consumed);
        stream.avail_in = static_cast<uInt>(inputChunk);
        stream.next_out = probing ? &spare : output.Cursor();
        stream.avail_out = static_cast<uInt>(outputChunk);

        const int status = inflate(&stream, Z_NO_FLUSH);
        const usize read = inputChunk - stream.avail_in;
        const usize written = outputChunk - stream.avail_out;
        consumed += read;
        if (probing && written != 0) {
            return Result<void>::Failure(ErrorCode::CapacityExceeded);
        }
        output.produced += probing ? 0 : written;
        output.writable -= probing ? 0 : written;

        if (status == Z_STREAM_END) {
            const usize remaining = source.Size() - consumed;
            if (remaining == 0) {
                return Result<void>::Success();
            }
            const bool nextMember = state.format == CompressionFormat::Gzip && remaining >= 2 &&
                                    input[consumed] == 0x1f && input[consumed + 1] == 0x8b;
            if (!nextMember) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }
            if (inflateReset(&stream) != Z_OK) {
                return Result<void>::Failure(ErrorCode::DependencyFailed);
            }
            continue;
        }
        if (status == Z_OK) {
            continue;
        }
        if (status == Z_BUF_ERROR) {
            // 진행이 불가능하다. 입력을 모두 넣었으면 절단, 아니면 계약 밖의 상태다(무한 반복
            // 방지).
            return Result<void>::Failure(consumed == source.Size() && read == 0
                                             ? ErrorCode::InvalidArgument
                                             : ErrorCode::DependencyFailed);
        }
        return Result<void>::Failure(StatusError(status));
    }
}

} // namespace

Result<usize> Bound(usize sourceBytes) noexcept {
    // 스트림 없이 구한 보수적 상한이다: raw·zlib·gzip(확장 필드 없는 헤더) 래퍼를 모두 덮는다.
    const z_size_t bound = deflateBound_z(Z_NULL, static_cast<z_size_t>(sourceBytes));
    if (bound == static_cast<z_size_t>(-1) || bound < sourceBytes) {
        return Result<usize>::Failure(ErrorCode::Overflow);
    }

    return Result<usize>::Success(static_cast<usize>(bound));
}

Result<void*> CreateCompressor(CompressionFormat format, int32 level,
                               Allocator& allocator) noexcept {
    auto created = CreateState(format, allocator, true);
    if (!created) {
        return Result<void*>::Failure(created.Error());
    }

    ZlibState* state = created.Value();
    const int status = deflateInit2(&state->stream, level, Z_DEFLATED, WindowBitsFor(format),
                                    MemoryLevel, Z_DEFAULT_STRATEGY);
    if (status != Z_OK) {
        CodecFree(allocator, state);
        return Result<void*>::Failure(status == Z_MEM_ERROR ? ErrorCode::OutOfMemory
                                                            : ErrorCode::DependencyFailed);
    }

    return Result<void*>::Success(state);
}

Result<void*> CreateDecompressor(CompressionFormat format, Allocator& allocator) noexcept {
    auto created = CreateState(format, allocator, false);
    if (!created) {
        return Result<void*>::Failure(created.Error());
    }

    ZlibState* state = created.Value();
    const int status = inflateInit2(&state->stream, WindowBitsFor(format));
    if (status != Z_OK) {
        CodecFree(allocator, state);
        return Result<void*>::Failure(status == Z_MEM_ERROR ? ErrorCode::OutOfMemory
                                                            : ErrorCode::DependencyFailed);
    }

    return Result<void*>::Success(state);
}

void Destroy(void* codec) noexcept {
    if (codec == nullptr) {
        return;
    }

    auto* state = static_cast<ZlibState*>(codec);
    Allocator& allocator = *state->allocator;
    if (state->deflating) {
        deflateEnd(&state->stream);
    } else {
        inflateEnd(&state->stream);
    }
    CodecFree(allocator, state);
}

Result<usize> Compress(void* codec, Span<const uint8> source, Span<uint8> destination,
                       const CompressionProbe& probe) noexcept {
    ZlibState& state = *static_cast<ZlibState*>(codec);
    z_stream& stream = state.stream;
    if (deflateReset(&stream) != Z_OK || !ApplyGzipHeader(state)) {
        return Result<usize>::Failure(ErrorCode::DependencyFailed);
    }

    // 입력·출력을 분할 단위로 넣는다. 남은 입력을 모두 넣는 호출부터 Z_FINISH이며 끝까지 유지된다.
    // 분할 단위는 동기·비동기가 같아 결과 byte가 같다(수준 0의 stored block 경계가 호출 형태를
    // 따른다).
    const uint8* input = NonNullSource(source);
    uint8* output = NonNullDestination(destination);
    usize consumed = 0;
    usize produced = 0;
    while (true) {
        if (probe.IsCancelled()) {
            return Result<usize>::Failure(ErrorCode::Cancelled);
        }

        const usize inputLeft = source.Size() - consumed;
        const usize inputChunk = Minimum(inputLeft, CompressionChunkBytes);
        const usize outputChunk = Minimum(destination.Size() - produced, CompressionChunkBytes);
        stream.next_in = const_cast<Bytef*>(input + consumed);
        stream.avail_in = static_cast<uInt>(inputChunk);
        stream.next_out = output + produced;
        stream.avail_out = static_cast<uInt>(outputChunk);

        const int status = deflate(&stream, inputChunk == inputLeft ? Z_FINISH : Z_NO_FLUSH);
        consumed += inputChunk - stream.avail_in;
        produced += outputChunk - stream.avail_out;
        if (status == Z_STREAM_END) {
            return Result<usize>::Success(produced);
        }
        if (status == Z_OK) {
            continue;
        }
        if (status == Z_BUF_ERROR) {
            // 진행 불가: 출력 공간이 없으면 용량 부족이다.
            return Result<usize>::Failure(produced == destination.Size()
                                              ? ErrorCode::CapacityExceeded
                                              : ErrorCode::DependencyFailed);
        }
        return Result<usize>::Failure(status == Z_MEM_ERROR ? ErrorCode::OutOfMemory
                                                            : ErrorCode::DependencyFailed);
    }
}

Result<usize> Decompress(void* codec, Span<const uint8> source, Span<uint8> destination,
                         const CompressionProbe& probe) noexcept {
    InflateOutput output;
    output.fixed = NonNullDestination(destination);
    output.limit = destination.Size();
    output.writable = destination.Size();

    auto inflated = Inflate(*static_cast<ZlibState*>(codec), source, output, probe);
    if (!inflated) {
        return Result<usize>::Failure(inflated.Error());
    }
    return Result<usize>::Success(output.produced);
}

Result<usize> DecompressAppend(void* codec, Span<const uint8> source, usize maxOutputBytes,
                               Array<uint8>& output, const CompressionProbe& probe) noexcept {
    InflateOutput target;
    target.array = &output;
    target.base = output.Size();
    target.limit = maxOutputBytes;

    auto inflated = Inflate(*static_cast<ZlibState*>(codec), source, target, probe);
    if (!inflated) {
        RestoreLength(output, target.base);
        return Result<usize>::Failure(inflated.Error());
    }

    RestoreLength(output, target.base + target.produced);
    return Result<usize>::Success(target.produced);
}

} // namespace rocks::detail::zlib
