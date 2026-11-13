#include "CompressionInternal.h"

#include <Core/Move.h>

// Brotli 1.1.1 해제 adapter다(RFC 7932, 해제 전용). 해제기 상태는
// BrotliDecoderCreateInstance(alloc_func, free_func, opaque)에 Allocator 경유 두 함수를 모두 넘겨
// 만들므로 상태·ring buffer·Huffman 표의 할당이 모두 Rocks Allocator를 거친다(할당 함수가 없을 때만
// 쓰는 CRT 경로는 타지 않는다). 설계와 근거는 Docs/Design/Compression.ko.md 13절이다.
#include <brotli/decode.h>

namespace rocks::detail::brotli {
namespace {

usize Minimum(usize first, usize second) noexcept {
    return first < second ? first : second;
}

void* Allocate(void* opaque, size_t bytes) {
    return CodecAllocate(*static_cast<Allocator*>(opaque), bytes);
}

// upstream 계약대로 nullptr이면 아무것도 하지 않는다(CodecFree가 처리한다).
void Free(void* opaque, void* address) {
    CodecFree(*static_cast<Allocator*>(opaque), address);
}

// stream 첫 byte에 기록된 window 크기다(RFC 7932 9.1 WBITS, 하위 1–7bit). window는 2^bits −
// 16byte다.
// - large: large window 확장(비표준, 첫 byte가 정확히 0x11: 표지 0010001 다음 bit 0). 확장 stream은
//   WBITS 10–30을 따로 기록하며 지원하지 않는다(Unsupported).
// - bits 0: 표준에서 예약된 값(처음 7bit 0010001 다음 bit 1, 첫 byte 0x91)이다. 어느 모드에서도
//   형식 오류이므로 해제기가 InvalidArgument로 보고하게 둔다.
struct WindowBits {
    uint32 bits = 0;
    bool large = false;
};

WindowBits ReadWindowBits(uint8 first) noexcept {
    WindowBits window;
    if ((first & 0x01) == 0) {
        window.bits = 16;
        return window;
    }

    const uint32 extended = (first >> 1) & 0x07;
    if (extended != 0) {
        window.bits = 17 + extended;
        return window;
    }

    const uint32 lower = (first >> 4) & 0x07;
    if (lower == 1) {
        window.large = first == 0x11;
    } else {
        window.bits = lower != 0 ? 8 + lower : 17;
    }

    return window;
}

// 해제 오류의 분류다. 형식 오류는 손상(InvalidArgument), 할당 실패는 OutOfMemory다. API 계약 오류·
// 도달할 수 없는 상태는 DependencyFailed다. 사전 오류 두 개는 이 빌드에서 나오지 않는다.
// - COMPOUND_DICTIONARY: 붙인 사전 범위를 벗어난 참조(손상)다. 사전을 붙이지 않는다.
// - DICTIONARY_NOT_SET: 내장 정적 사전 데이터가 없는 빌드(BROTLI_EXTERNAL_DICTIONARY_DATA)의
//   의존성 문제다. 그 정의를 쓰지 않는다.
ErrorCode DecodeError(BrotliDecoderErrorCode code) noexcept {
    switch (code) {
    case BROTLI_DECODER_ERROR_ALLOC_CONTEXT_MODES:
    case BROTLI_DECODER_ERROR_ALLOC_TREE_GROUPS:
    case BROTLI_DECODER_ERROR_ALLOC_CONTEXT_MAP:
    case BROTLI_DECODER_ERROR_ALLOC_RING_BUFFER_1:
    case BROTLI_DECODER_ERROR_ALLOC_RING_BUFFER_2:
    case BROTLI_DECODER_ERROR_ALLOC_BLOCK_TYPE_TREES:
        return ErrorCode::OutOfMemory;
    case BROTLI_DECODER_NO_ERROR:
    case BROTLI_DECODER_SUCCESS:
    case BROTLI_DECODER_NEEDS_MORE_INPUT:
    case BROTLI_DECODER_NEEDS_MORE_OUTPUT:
    case BROTLI_DECODER_ERROR_INVALID_ARGUMENTS:
    case BROTLI_DECODER_ERROR_DICTIONARY_NOT_SET:
    case BROTLI_DECODER_ERROR_UNREACHABLE:
        return ErrorCode::DependencyFailed;
    default:
        return ErrorCode::InvalidArgument;
    }
}

// 해제기 상태를 만든다. 실패는 상태(또는 그 안의 shared dictionary 상태) 할당 실패뿐이다. large
// window는 켜지 않는다. ring buffer는 기본("canny") 방식이라 출력에 맞춰 2배씩 늘며 window를 넘지
// 않는다. 늘리는 순간 이전·새 ring이 함께 있어 최고 사용량은 약 1.5 × window + Huffman 표다(작은
// 출력에 window 전체를 잡지 않는 대가, Docs/Design/Compression.ko.md 13.3절).
Result<BrotliDecoderState*> CreateDecoder(Allocator& allocator) noexcept {
    BrotliDecoderState* state = BrotliDecoderCreateInstance(&Allocate, &Free, &allocator);
    if (state == nullptr) {
        return Result<BrotliDecoderState*>::Failure(ErrorCode::OutOfMemory);
    }

    return Result<BrotliDecoderState*>::Success(state);
}

// 일회성 해제 루프다.
// - 입력·출력을 분할 단위로 넣어 취소를 확인한다.
// - 쓸 공간이 없으면 1byte 예비 창으로 계속 진행한다. 1byte라도 나오면 출력을 늘리거나(Append) 상한
//   초과다. stream 끝만 남았으면 출력 없이 끝나므로 정확히 상한 크기인 결과는 성공한다.
// - Brotli stream은 이어 붙일 수 없다. 끝난 뒤 남은 byte는 손상이다(해제기는 입력을 넘겨 읽지
//   않는다).
// - 입력을 모두 넣었는데 더 필요하면 절단이다.
Result<void> Run(BrotliDecoderState* state, Span<const uint8> source, DecodeOutput& output,
                 const CompressionProbe& probe) noexcept {
    const uint8* input = NonNullSource(source);
    usize consumed = 0;
    uint8 spare = 0;
    while (true) {
        if (probe.IsCancelled()) {
            return Result<void>::Failure(ErrorCode::Cancelled);
        }

        const bool probing = output.writable == 0;
        const usize inputChunk = Minimum(source.Size() - consumed, CompressionChunkBytes);
        const usize outputChunk = probing ? 1 : Minimum(output.writable, CompressionChunkBytes);
        size_t availableIn = inputChunk;
        const uint8_t* nextIn = input + consumed;
        size_t availableOut = outputChunk;
        uint8_t* nextOut = probing ? &spare : output.Cursor();
        const BrotliDecoderResult result = BrotliDecoderDecompressStream(
            state, &availableIn, &nextIn, &availableOut, &nextOut, nullptr);
        const usize read = inputChunk - availableIn;
        const usize written = outputChunk - availableOut;
        consumed += read;
        if (result == BROTLI_DECODER_RESULT_ERROR) {
            return Result<void>::Failure(DecodeError(BrotliDecoderGetErrorCode(state)));
        }

        if (!probing) {
            output.produced += written;
            output.writable -= written;
        } else if (written != 0) {
            auto accepted = output.AcceptSpare(spare);
            if (!accepted) {
                return accepted;
            }
        }

        if (result == BROTLI_DECODER_RESULT_SUCCESS) {
            return consumed == source.Size() ? Result<void>::Success()
                                             : Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        if (result == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT && consumed == source.Size()) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        if (read == 0 && written == 0) {
            // 입력·출력 공간이 있는데 진행하지 않았다: 계약 밖의 상태다(무한 반복 방지).
            return Result<void>::Failure(ErrorCode::DependencyFailed);
        }
    }
}

// 일회성 해제다. large window stream은 해제기를 만들기 전에 Unsupported로 거부한다.
Result<void> Decode(Allocator& allocator, Span<const uint8> source, DecodeOutput& output,
                    const CompressionProbe& probe) noexcept {
    if (ReadWindowBits(source[0]).large) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    auto created = CreateDecoder(allocator);
    if (!created) {
        return Result<void>::Failure(created.Error());
    }

    auto decoded = Run(created.Value(), source, output, probe);
    BrotliDecoderDestroyInstance(created.Value());
    return decoded;
}

// 스트림 해제 문맥이다. 해제기 상태는 한 stream만 풀 수 있어 Reset이 새로 만든다.
struct StreamDecoder {
    Allocator* allocator = nullptr;
    BrotliDecoderState* state = nullptr;
};

} // namespace

Result<usize> Decompress(Allocator& allocator, Span<const uint8> source, Span<uint8> destination,
                         const CompressionProbe& probe) noexcept {
    DecodeOutput output;
    output.fixed = NonNullDestination(destination);
    output.limit = destination.Size();
    output.writable = destination.Size();

    auto decoded = Decode(allocator, source, output, probe);
    if (!decoded) {
        return Result<usize>::Failure(decoded.Error());
    }

    return Result<usize>::Success(output.produced);
}

Result<usize> DecompressAppend(Allocator& allocator, Span<const uint8> source, usize maxOutputBytes,
                               Array<uint8>& output, const CompressionProbe& probe) noexcept {
    // large window stream은 출력을 확보하기 전에 거부한다(Decode도 같은 검사를 한다).
    if (ReadWindowBits(source[0]).large) {
        return Result<usize>::Failure(ErrorCode::Unsupported);
    }

    DecodeOutput target;
    target.array = &output;
    target.base = output.Size();
    target.limit = maxOutputBytes;

    // Brotli stream에는 전체 원본 크기가 없다. 64KiB(상한 이하)를 먼저 잡고 예비 창에 출력이 더
    // 나올 때만 두 배씩 늘린다.
    const usize initial = Minimum(DecodeOutputInitialBytes, maxOutputBytes);
    if (initial != 0) {
        auto grown = target.Grow(initial);
        if (!grown) {
            RestoreLength(output, target.base);
            return Result<usize>::Failure(grown.Error());
        }
    }

    auto decoded = Decode(allocator, source, target, probe);
    if (!decoded) {
        RestoreLength(output, target.base);
        return Result<usize>::Failure(decoded.Error());
    }

    RestoreLength(output, target.base + target.produced);
    return Result<usize>::Success(target.produced);
}

Result<void*> CreateStreamDecompressor(Allocator& allocator) noexcept {
    void* memory = CodecAllocate(allocator, sizeof(StreamDecoder));
    if (memory == nullptr) {
        return Result<void*>::Failure(ErrorCode::OutOfMemory);
    }

    StreamDecoder* decoder = ConstructAt<StreamDecoder>(memory);
    decoder->allocator = &allocator;
    auto created = CreateDecoder(allocator);
    if (!created) {
        DestroyAt(decoder);
        CodecFree(allocator, decoder);
        return Result<void*>::Failure(created.Error());
    }

    decoder->state = created.Value();
    return Result<void*>::Success(decoder);
}

void DestroyStreamDecompressor(void* codec) noexcept {
    if (codec == nullptr) {
        return;
    }

    auto* decoder = static_cast<StreamDecoder*>(codec);
    Allocator& allocator = *decoder->allocator;
    BrotliDecoderDestroyInstance(decoder->state);
    DestroyAt(decoder);
    CodecFree(allocator, decoder);
}

Result<void> ResetStreamDecompressor(void* codec) noexcept {
    // 이전 상태를 먼저 해제해 최고 메모리를 낮춘다. 새로 만들지 못하면 상태가 없는 채로
    // OutOfMemory가 남고(스트림 실패), 다음 Reset이 다시 만든다.
    auto& decoder = *static_cast<StreamDecoder*>(codec);
    BrotliDecoderDestroyInstance(decoder.state);
    decoder.state = nullptr;
    auto created = CreateDecoder(*decoder.allocator);
    if (!created) {
        return Result<void>::Failure(created.Error());
    }

    decoder.state = created.Value();
    return Result<void>::Success();
}

Result<StreamProgress> DecompressStep(void* codec, Span<const uint8> input, Span<uint8> output,
                                      uint32 maxWindowLog, StreamDecodeState& state) noexcept {
    auto& decoder = *static_cast<StreamDecoder*>(codec);
    if (decoder.state == nullptr) {
        return Result<StreamProgress>::Failure(ErrorCode::DependencyFailed);
    }

    if (state.ended && input.Size() != 0) {
        // Brotli stream은 이어 붙일 수 없다. 끝난 뒤의 byte는 손상이다.
        return Result<StreamProgress>::Failure(ErrorCode::InvalidArgument);
    }

    // 첫 byte의 window를 해제기에 넣기 전에 검사한다. large window는 지원하지 않고(Unsupported),
    // 호출자 상한(2^maxWindowLog)보다 크면 CapacityExceeded다. ring buffer는 window를 넘지 않으므로
    // 작업 메모리는 window로 묶인다(최고 약 1.5 × window + Huffman 표). 예약 값(bits 0)은 해제기가
    // 형식 오류로 보고한다.
    if (state.atFrameStart && input.Size() != 0) {
        const WindowBits window = ReadWindowBits(input[0]);
        if (window.large) {
            return Result<StreamProgress>::Failure(ErrorCode::Unsupported);
        }

        if (window.bits > maxWindowLog) {
            return Result<StreamProgress>::Failure(ErrorCode::CapacityExceeded);
        }

        state.atFrameStart = false;
    }

    size_t availableIn = input.Size();
    const uint8_t* nextIn = NonNullSource(input);
    size_t availableOut = output.Size();
    uint8_t* nextOut = NonNullDestination(output);
    const BrotliDecoderResult result = BrotliDecoderDecompressStream(
        decoder.state, &availableIn, &nextIn, &availableOut, &nextOut, nullptr);
    if (result == BROTLI_DECODER_RESULT_ERROR) {
        return Result<StreamProgress>::Failure(
            DecodeError(BrotliDecoderGetErrorCode(decoder.state)));
    }

    StreamProgress progress;
    progress.consumed = input.Size() - availableIn;
    progress.produced = output.Size() - availableOut;
    // 해제기는 한 호출에서 입력이 바닥나거나, 출력이 차거나, stream이 끝날 때까지 진행한다. 입력이
    // 바닥났을 때도 출력 창이 차서 ring buffer에 내보낼 출력이 남을 수 있으므로(NEEDS_MORE_INPUT과
    // 함께) 결과 코드가 아니라 남은 출력으로 판정한다.
    const bool pending = result == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT ||
                         BrotliDecoderHasMoreOutput(decoder.state) == BROTLI_TRUE;
    if (result == BROTLI_DECODER_RESULT_SUCCESS) {
        state.ended = true;
        state.sawStream = true;
        state.atBoundary = true;
        if (progress.consumed != input.Size()) {
            return Result<StreamProgress>::Failure(ErrorCode::InvalidArgument);
        }
    }

    state.outputPending = pending;
    progress.complete = progress.consumed == input.Size() && !pending;
    return Result<StreamProgress>::Success(progress);
}

} // namespace rocks::detail::brotli
