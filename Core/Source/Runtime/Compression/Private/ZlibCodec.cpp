#include "CompressionInternal.h"

#include <Core/Move.h>
#include <Platform/Memory.h>

// zlib 1.4 adapter. Z_SOLO·Z_PREFIX·Z_U4·Z_U8은 모듈 규칙이 ThirdParty.json의 consumerDefines에서
// 정의한다 (라이브러리 빌드와 같아야 한다). Z_SOLO라 기본 할당기가 없으므로 zalloc·zfree를 반드시
// 넘긴다.
#include <zlib.h>

namespace rocks::detail::zlib {
namespace {

constexpr int WindowBits = 15;
constexpr int MemoryLevel = 8;
constexpr usize AppendInitialBytes = usize{64} << 10;

// 스트림 압축에서 출력 창이 이보다 작으면 deflate를 carry 버퍼에 대고 부른 뒤 나눠 넘긴다.
// deflate는 flush 중 avail_out이 0이 되면 다음 호출에서 flush marker(빈 stored block)를 다시
// 만들므로, 창이 marker보다 작으면 끝나지 않는다(zlib.h: flush marker가 시작될 때 avail_out은 6보다
// 커야 한다).
constexpr usize CarryBytes = 64;

// z_stream과 gzip 헤더는 zlib이 주소를 보관하므로(state->strm 역참조, gzhead 포인터) heap에
// 고정한다.
struct ZlibState {
    z_stream stream;
    gz_header header;
    Allocator* allocator;
    CompressionFormat format;
    bool deflating;
    // 스트림 압축의 carry 버퍼다. [carryBegin, carryEnd)가 아직 넘기지 않은 출력이다.
    uint8 carry[CarryBytes];
    uint32 carryBegin;
    uint32 carryEnd;
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
        // 늘린 구간은 inflate가 채우므로 0으로 채우지 않는다.
        auto grown = array->TryAppendUninitialized(base + produced + growth - array->Size());
        if (!grown) {
            return Result<void>::Failure(grown.Error());
        }
        writable = growth;
        return Result<void>::Success();
    }
};

// Deflate·Zlib·Gzip 해제 공통 루프다.
// - 입력·출력을 분할 단위로 넣어 uInt 한계와 취소 확인을 처리한다.
// - 출력이 상한에 이르면 1byte 예비 버퍼로 계속 진행한다. 1byte라도 나오면 상한 초과,
//   블록 끝·체크섬만 남았으면 성공이다(이 상태들은 출력 공간 없이 진행된다). 그래서
//   정확히 상한 크기인 결과는 성공한다.
// - Gzip은 연속 member를 모두 해제한다. member 뒤의 쓰레기·0 padding과 다른 형식의
//   남는 byte는 손상이다.
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

Result<usize> StreamBound(void* codec, usize sourceBytes) noexcept {
    // 문맥의 수준·window·memLevel·래퍼(gzip은 확장 필드 없는 헤더)로 구한 상한이다.
    // Z_NO_FLUSH·Z_FINISH만 쓰므로(zlib.h deflateBound 계약) 분할 호출에도 성립한다. 수준 0은
    // zlib이 stored 기준 상한을 쓴다.
    auto* state = static_cast<ZlibState*>(codec);
    const z_size_t bound = deflateBound_z(&state->stream, static_cast<z_size_t>(sourceBytes));
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

// --- 스트리밍(10절) ---

Result<void> ResetStreamCompressor(void* codec) noexcept {
    ZlibState& state = *static_cast<ZlibState*>(codec);
    state.carryBegin = 0;
    state.carryEnd = 0;
    if (deflateReset(&state.stream) != Z_OK || !ApplyGzipHeader(state)) {
        return Result<void>::Failure(ErrorCode::DependencyFailed);
    }
    return Result<void>::Success();
}

Result<void> ResetStreamDecompressor(void* codec) noexcept {
    ZlibState& state = *static_cast<ZlibState*>(codec);
    return inflateReset(&state.stream) == Z_OK ? Result<void>::Success()
                                               : Result<void>::Failure(ErrorCode::DependencyFailed);
}

Result<StreamProgress> CompressStep(void* codec, Span<const uint8> input, Span<uint8> output,
                                    StreamFlush flush) noexcept {
    ZlibState& state = *static_cast<ZlibState*>(codec);
    z_stream& stream = state.stream;
    const uint8* source = NonNullSource(input);
    usize consumed = 0;
    usize produced = 0;
    StreamProgress progress;
    // 입력·출력은 uInt 한계 안의 분할 단위로 넣는다. flush 모드는 남은 입력의 마지막 조각에만 쓴다.
    while (produced < output.Size()) {
        // 지난 호출의 carry를 먼저 넘긴다(출력 순서 유지).
        if (state.carryBegin != state.carryEnd) {
            const usize copy = Minimum(state.carryEnd - state.carryBegin, output.Size() - produced);
            platform::MemoryCopy(output.Data() + produced, state.carry + state.carryBegin, copy);
            state.carryBegin += static_cast<uint32>(copy);
            produced += copy;
            continue;
        }

        const usize room = output.Size() - produced;
        const bool viaCarry = room < CarryBytes;
        const usize inputChunk = Minimum(input.Size() - consumed, CompressionChunkBytes);
        const usize outputChunk = viaCarry ? CarryBytes : Minimum(room, CompressionChunkBytes);
        const bool lastInput = inputChunk == input.Size() - consumed;
        stream.next_in = const_cast<Bytef*>(source + consumed);
        stream.avail_in = static_cast<uInt>(inputChunk);
        stream.next_out = viaCarry ? state.carry : output.Data() + produced;
        stream.avail_out = static_cast<uInt>(outputChunk);
        const int mode = !lastInput || flush == StreamFlush::None ? Z_NO_FLUSH
                         : flush == StreamFlush::Sync             ? Z_SYNC_FLUSH
                                                                  : Z_FINISH;
        const int status = deflate(&stream, mode);
        const usize read = inputChunk - stream.avail_in;
        const usize written = outputChunk - stream.avail_out;
        const bool roomLeft = stream.avail_out != 0;
        consumed += read;
        if (viaCarry) {
            const usize copy = Minimum(written, room);
            platform::MemoryCopy(output.Data() + produced, state.carry, copy);
            produced += copy;
            state.carryBegin = static_cast<uint32>(copy);
            state.carryEnd = static_cast<uint32>(written);
        } else {
            produced += written;
        }

        // 요청 완료: End는 Z_STREAM_END, Sync는 입력을 다 넣고 출력 공간이 남은 채 돌아온 때(zlib
        // 계약), None은 입력을 다 넣은 때다. Z_BUF_ERROR는 진행할 것이 없다는 뜻이다(완료된 flush의
        // 반복 등).
        bool done = false;
        if (status == Z_STREAM_END) {
            done = true;
        } else if (status == Z_BUF_ERROR) {
            done = flush != StreamFlush::End && consumed == input.Size();
            if (!done) {
                break;
            }
        } else if (status != Z_OK) {
            return Result<StreamProgress>::Failure(
                status == Z_MEM_ERROR ? ErrorCode::OutOfMemory : ErrorCode::DependencyFailed);
        } else if (consumed == input.Size()) {
            done = flush == StreamFlush::None || (flush == StreamFlush::Sync && roomLeft);
        }

        if (done) {
            // flush·finish는 carry까지 다 넘겨야 끝이다. 남았으면 다음 호출이 넘긴 뒤 다시 확인한다
            // (완료된 flush·finish를 반복해 불러도 deflate는 byte를 더 만들지 않는다).
            progress.complete = flush == StreamFlush::None || state.carryBegin == state.carryEnd;
            break;
        }
        if (read == 0 && written == 0) {
            break;
        }
    }

    progress.consumed = consumed;
    progress.produced = produced;
    return Result<StreamProgress>::Success(progress);
}

Result<StreamProgress> DecompressStep(void* codec, Span<const uint8> input, Span<uint8> output,
                                      StreamDecodeState& decode) noexcept {
    ZlibState& state = *static_cast<ZlibState*>(codec);
    z_stream& stream = state.stream;
    const uint8* source = NonNullSource(input);
    usize consumed = 0;
    usize produced = 0;
    bool filled = false;
    while (produced < output.Size()) {
        const usize remaining = input.Size() - consumed;
        if (decode.ended) {
            // Deflate·Zlib 스트림 뒤의 byte는 손상이다.
            if (remaining != 0) {
                return Result<StreamProgress>::Failure(ErrorCode::InvalidArgument);
            }
            break;
        }
        if (remaining == 0 && !decode.outputPending) {
            break;
        }

        const usize inputChunk = Minimum(remaining, CompressionChunkBytes);
        const usize outputChunk = Minimum(output.Size() - produced, CompressionChunkBytes);
        stream.next_in = const_cast<Bytef*>(source + consumed);
        stream.avail_in = static_cast<uInt>(inputChunk);
        stream.next_out = output.Data() + produced;
        stream.avail_out = static_cast<uInt>(outputChunk);
        const int status = inflate(&stream, Z_NO_FLUSH);
        const usize read = inputChunk - stream.avail_in;
        const usize written = outputChunk - stream.avail_out;
        consumed += read;
        produced += written;
        filled = stream.avail_out == 0;
        decode.outputPending = filled;
        if (read != 0) {
            decode.atBoundary = false;
        }

        if (status == Z_STREAM_END) {
            decode.sawStream = true;
            decode.atBoundary = true;
            decode.outputPending = false;
            if (state.format == CompressionFormat::Gzip) {
                // 다음 member: inflate가 magic을 직접 검사한다(gzip이 아니면 Z_DATA_ERROR).
                if (inflateReset(&stream) != Z_OK) {
                    return Result<StreamProgress>::Failure(ErrorCode::DependencyFailed);
                }
            } else {
                decode.ended = true;
            }
            continue;
        }
        if (status == Z_BUF_ERROR) {
            // 진행 불가: 입력이 더 필요하다(스트림에서는 오류가 아니다). 입력과 출력 공간이
            // 있는데도 진행하지 못했다면 zlib 계약 밖이다.
            if (inputChunk != 0 && read == 0 && written == 0) {
                return Result<StreamProgress>::Failure(ErrorCode::DependencyFailed);
            }
            decode.outputPending = false;
            break;
        }
        if (status != Z_OK) {
            return Result<StreamProgress>::Failure(StatusError(status));
        }
        if (read == 0 && written == 0) {
            break;
        }
    }

    // Deflate·Zlib 스트림 뒤의 byte는 출력 창 크기와 무관하게 이 호출에서 거부한다(inflate는 끝난
    // 스트림 뒤를 소비하지 않으므로 adapter가 판정한다). 거부에는 출력 공간이 필요 없다.
    if (decode.ended && consumed != input.Size()) {
        return Result<StreamProgress>::Failure(ErrorCode::InvalidArgument);
    }

    StreamProgress progress;
    progress.consumed = consumed;
    progress.produced = produced;
    progress.complete = consumed == input.Size() && !decode.outputPending;
    return Result<StreamProgress>::Success(progress);
}

uint32 Crc32(Span<const uint8> data, uint32 previous) noexcept {
    return static_cast<uint32>(
        crc32_z(previous, NonNullSource(data), static_cast<z_size_t>(data.Size())));
}

} // namespace rocks::detail::zlib
