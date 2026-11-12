#include "CompressionInternal.h"

#include <Compression/CompressionStream.h>
#include <Core/Move.h>

namespace rocks {
namespace detail {

// 압축 스트림의 단계다. 코덱을 부른 flush·finish가 끝나기 전에는 같은 요청만 받는다. zlib은 미완료
// flush를 같은 모드로 다시 부르기를 요구하고 zstd는 미완료 e_end 뒤에 e_end·e_flush만 받는다. 두
// 형식을 같은 규칙으로 맞춘다.
enum class StreamPhase : uint8 {
    Open,
    Flushing,
    Finishing,
    Finished
};

// CompressStream·DecompressStream이 함께 쓰는 heap 상태다. 이동해도 주소가 바뀌지 않는다.
struct StreamState {
    Allocator* allocator = nullptr;
    void* codec = nullptr;
    CompressionFormat format = CompressionFormat::Zstd;
    bool compressing = false;
    // Reset 전까지 유지하는 실패다(ErrorCode::None이면 정상).
    ErrorCode failure = ErrorCode::None;

    // 압축.
    StreamPhase phase = StreamPhase::Open;

    // 해제.
    usize maxOutputBytes = 0;
    uint32 maxWindowLog = 0;
    usize totalOutput = 0;
    StreamDecodeState decode;

    // 사전 스트림이 가진 사전 handle이다(코덱이 CDict·DDict를 참조한다).
    SharedPtr<DictionaryState> dictionary;
};

namespace {

// Append 편의 함수가 한 번에 늘리는 출력 크기다. 고정이라 같은 입력 조각이면 같은 압축 byte가
// 나온다.
constexpr usize StreamGrowBytes = usize{1} << 20;

bool IsStreamFormat(CompressionFormat format) noexcept {
    switch (format) {
    case CompressionFormat::Zstd:
    case CompressionFormat::Deflate:
    case CompressionFormat::Zlib:
    case CompressionFormat::Gzip:
        return true;
    case CompressionFormat::Lz4:
        return false;
    }
    return false;
}

bool IsKnownFormat(CompressionFormat format) noexcept {
    return IsStreamFormat(format) || format == CompressionFormat::Lz4;
}

void DestroyCodec(StreamState& state) noexcept {
    if (state.codec == nullptr) {
        return;
    }

    if (state.format == CompressionFormat::Zstd) {
        if (state.compressing) {
            zstd::DestroyCompressor(state.codec);
        } else {
            zstd::DestroyDecompressor(state.codec);
        }
    } else {
        zlib::Destroy(state.codec);
    }
    state.codec = nullptr;
}

void DestroyState(StreamState* state) noexcept {
    if (state == nullptr) {
        return;
    }

    // 코덱이 사전을 참조하므로 코덱을 먼저 정리한 뒤 사전 handle을 놓는다.
    Allocator& allocator = *state->allocator;
    DestroyCodec(*state);
    DestroyAt(state);
    CodecFree(allocator, state);
}

// 코덱을 받아 상태 블록을 만든다. 실패하면 codec도 정리한다.
Result<StreamState*> AdoptCodec(StreamState initial) noexcept {
    void* memory = CodecAllocate(*initial.allocator, sizeof(StreamState));
    if (memory == nullptr) {
        DestroyCodec(initial);
        return Result<StreamState*>::Failure(ErrorCode::OutOfMemory);
    }

    return Result<StreamState*>::Success(ConstructAt<StreamState>(memory, initial));
}

template <typename T>
Result<T> Fail(StreamState& state, ErrorCode error) noexcept {
    state.failure = error;
    return Result<T>::Failure(error);
}

// --- 압축 ---

Result<void> ResetCompressorCodec(StreamState& state) noexcept {
    if (state.format == CompressionFormat::Zstd) {
        zstd::ResetStreamCompressor(state.codec);
        return Result<void>::Success();
    }
    return zlib::ResetStreamCompressor(state.codec);
}

// 단계 검사 뒤 코덱 한 단계를 부른다. 출력 공간이 0이면 코덱을 부르지 않는다.
Result<StreamProgress> CompressStep(StreamState& state, Span<const uint8> input, Span<uint8> output,
                                    StreamFlush flush) noexcept {
    if (flush == StreamFlush::None && input.Size() == 0) {
        StreamProgress progress;
        progress.complete = true;
        return Result<StreamProgress>::Success(progress);
    }
    if (output.Size() == 0) {
        return Result<StreamProgress>::Success(StreamProgress{});
    }

    auto step = state.format == CompressionFormat::Zstd
                    ? zstd::CompressStep(state.codec, input, output, flush)
                    : zlib::CompressStep(state.codec, input, output, flush);
    if (!step) {
        return Fail<StreamProgress>(state, step.Error());
    }

    const StreamProgress progress = step.Value();
    if (flush == StreamFlush::Sync) {
        state.phase = progress.complete ? StreamPhase::Open : StreamPhase::Flushing;
    } else if (flush == StreamFlush::End) {
        state.phase = progress.complete ? StreamPhase::Finished : StreamPhase::Finishing;
    }
    return step;
}

// 요청이 지금 단계에서 허용되는지다. 거부는 상태를 바꾸지 않는다.
bool AcceptsCompress(const StreamState& state, StreamFlush flush) noexcept {
    switch (state.phase) {
    case StreamPhase::Open:
        return true;
    case StreamPhase::Flushing:
        return flush == StreamFlush::Sync;
    case StreamPhase::Finishing:
    case StreamPhase::Finished:
        return flush == StreamFlush::End;
    }
    return false;
}

Result<StreamProgress> CompressRequest(StreamState* state, Span<const uint8> input,
                                       Span<uint8> output, StreamFlush flush) noexcept {
    if (state == nullptr) {
        return Result<StreamProgress>::Failure(ErrorCode::StaleHandle);
    }
    if (state->failure != ErrorCode::None) {
        return Result<StreamProgress>::Failure(state->failure);
    }
    if (!AcceptsCompress(*state, flush) ||
        Overlaps(input.Data(), input.Size(), output.Data(), output.Size())) {
        return Result<StreamProgress>::Failure(ErrorCode::InvalidArgument);
    }
    if (state->phase == StreamPhase::Finished) {
        StreamProgress progress;
        progress.complete = true;
        return Result<StreamProgress>::Success(progress);
    }

    return CompressStep(*state, input, output, flush);
}

// Append 편의 함수 공통이다. complete가 될 때까지 출력을 StreamGrowBytes씩 늘리며 부른다.
Result<usize> CompressAppendRequest(StreamState* state, Span<const uint8> input,
                                    Array<uint8>& output, StreamFlush flush) noexcept {
    if (state == nullptr) {
        return Result<usize>::Failure(ErrorCode::StaleHandle);
    }
    if (state->failure != ErrorCode::None) {
        return Result<usize>::Failure(state->failure);
    }
    if (!AcceptsCompress(*state, flush) || OverlapsArrayStorage(input, output)) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }
    if (state->phase == StreamPhase::Finished ||
        (flush == StreamFlush::None && input.Size() == 0)) {
        return Result<usize>::Success(0);
    }

    const usize base = output.Size();
    usize consumed = 0;
    while (true) {
        const usize chunkBase = output.Size();
        auto grown = output.TryAppendUninitialized(StreamGrowBytes);
        if (!grown) {
            RestoreLength(output, base);
            return Fail<usize>(*state, grown.Error());
        }

        const Span<const uint8> rest(input.Data() + consumed, input.Size() - consumed);
        auto step = CompressStep(*state, rest, grown.Value(), flush);
        if (!step) {
            RestoreLength(output, base);
            return Result<usize>::Failure(step.Error());
        }

        const StreamProgress progress = step.Value();
        RestoreLength(output, chunkBase + progress.produced);
        consumed += progress.consumed;
        if (progress.complete) {
            return Result<usize>::Success(output.Size() - base);
        }
        if (progress.consumed == 0 && progress.produced == 0) {
            // 입력·출력 공간이 있는데 진행하지 않았다: 코덱 계약 위반이다.
            RestoreLength(output, base);
            return Fail<usize>(*state, ErrorCode::DependencyFailed);
        }
    }
}

// --- 해제 ---

Result<void> ResetDecompressorCodec(StreamState& state) noexcept {
    if (state.format == CompressionFormat::Zstd) {
        zstd::ResetStreamDecompressor(state.codec);
        return Result<void>::Success();
    }
    return zlib::ResetStreamDecompressor(state.codec);
}

// 누계 상한까지 남은 출력이다.
usize Allowance(const StreamState& state) noexcept {
    return state.maxOutputBytes - state.totalOutput;
}

// 코덱 한 단계다. 출력 창을 남은 허용량 + 1로 줄여 상한 초과를 1byte로 감지한다. 정확히 상한에서
// 끝나는 스트림은 1byte 창으로 끝(블록 끝·체크섬)까지 진행해 성공한다.
Result<StreamProgress> DecompressStep(StreamState& state, Span<const uint8> input,
                                      Span<uint8> output) noexcept {
    if (input.Size() == 0 && !state.decode.outputPending) {
        StreamProgress progress;
        progress.complete = true;
        return Result<StreamProgress>::Success(progress);
    }
    if (output.Size() == 0) {
        return Result<StreamProgress>::Success(StreamProgress{});
    }

    const usize allowance = Allowance(state);
    const usize window = output.Size() > allowance ? allowance + 1 : output.Size();
    const Span<uint8> target(output.Data(), window);
    auto step =
        state.format == CompressionFormat::Zstd
            ? zstd::DecompressStep(state.codec, input, target, state.maxWindowLog,
                                   state.dictionary ? state.dictionary->id : 0, state.decode)
            : zlib::DecompressStep(state.codec, input, target, state.decode);
    if (!step) {
        return Fail<StreamProgress>(state, step.Error());
    }
    if (step.Value().produced > allowance) {
        return Fail<StreamProgress>(state, ErrorCode::CapacityExceeded);
    }

    state.totalOutput += step.Value().produced;
    return step;
}

} // namespace
} // namespace detail

// --- CompressStream ---

CompressStream::~CompressStream() noexcept {
    detail::DestroyState(state_);
}

CompressStream::CompressStream(CompressStream&& other) noexcept : state_(other.state_) {
    other.state_ = nullptr;
}

CompressStream& CompressStream::operator=(CompressStream&& other) noexcept {
    if (this != &other) {
        detail::DestroyState(state_);
        state_ = other.state_;
        other.state_ = nullptr;
    }
    return *this;
}

Result<CompressStream> CompressStream::TryCreate(const CompressionSettings& settings,
                                                 Allocator& allocator, uint64 sizeHint) noexcept {
    return TryCreateWith(settings.Format(), settings.Level(), allocator, sizeHint,
                         SharedPtr<detail::DictionaryState>());
}

Result<CompressStream> CompressStream::TryCreate(const CompressDictionary& dictionary,
                                                 Allocator& allocator, uint64 sizeHint) noexcept {
    const auto& state = detail::DictionaryAccess::State(dictionary);
    if (!state) {
        return Result<CompressStream>::Failure(ErrorCode::StaleHandle);
    }

    return TryCreateWith(state->format, state->level, allocator, sizeHint, state);
}

Result<CompressStream>
CompressStream::TryCreateWith(CompressionFormat format, int32 level, Allocator& allocator,
                              uint64 sizeHint,
                              const SharedPtr<detail::DictionaryState>& dictionary) noexcept {
    if (!detail::IsStreamFormat(format)) {
        return Result<CompressStream>::Failure(
            detail::IsKnownFormat(format) ? ErrorCode::Unsupported : ErrorCode::InvalidArgument);
    }

    auto codec =
        format == CompressionFormat::Zstd
            ? detail::zstd::CreateStreamCompressor(level, sizeHint, allocator, dictionary.Get())
            : detail::zlib::CreateCompressor(format, level, allocator);
    if (!codec) {
        return Result<CompressStream>::Failure(codec.Error());
    }

    detail::StreamState initial;
    initial.allocator = &allocator;
    initial.codec = codec.Value();
    initial.format = format;
    initial.compressing = true;
    initial.dictionary = dictionary;
    auto state = detail::AdoptCodec(initial);
    if (!state) {
        return Result<CompressStream>::Failure(state.Error());
    }

    // zlib은 gzip 헤더(결정적 mtime·OS)를 스트림 시작 전에 적용한다.
    CompressStream stream(state.Value());
    auto reset = detail::ResetCompressorCodec(*state.Value());
    if (!reset) {
        return Result<CompressStream>::Failure(reset.Error());
    }

    return Result<CompressStream>::Success(Move(stream));
}

Result<StreamProgress> CompressStream::TryProcess(Span<const uint8> input,
                                                  Span<uint8> output) noexcept {
    return detail::CompressRequest(state_, input, output, detail::StreamFlush::None);
}

Result<StreamProgress> CompressStream::TryFlush(Span<uint8> output) noexcept {
    return detail::CompressRequest(state_, Span<const uint8>(), output, detail::StreamFlush::Sync);
}

Result<StreamProgress> CompressStream::TryFinish(Span<uint8> output) noexcept {
    return detail::CompressRequest(state_, Span<const uint8>(), output, detail::StreamFlush::End);
}

Result<usize> CompressStream::TryWriteAppend(Span<const uint8> input,
                                             Array<uint8>& output) noexcept {
    return detail::CompressAppendRequest(state_, input, output, detail::StreamFlush::None);
}

Result<usize> CompressStream::TryFlushAppend(Array<uint8>& output) noexcept {
    return detail::CompressAppendRequest(state_, Span<const uint8>(), output,
                                         detail::StreamFlush::Sync);
}

Result<usize> CompressStream::TryFinishAppend(Array<uint8>& output) noexcept {
    return detail::CompressAppendRequest(state_, Span<const uint8>(), output,
                                         detail::StreamFlush::End);
}

void CompressStream::Reset() noexcept {
    if (state_ == nullptr) {
        return;
    }

    state_->phase = detail::StreamPhase::Open;
    auto reset = detail::ResetCompressorCodec(*state_);
    state_->failure = reset ? ErrorCode::None : reset.Error();
}

// --- DecompressStream ---

DecompressStream::~DecompressStream() noexcept {
    detail::DestroyState(state_);
}

DecompressStream::DecompressStream(DecompressStream&& other) noexcept : state_(other.state_) {
    other.state_ = nullptr;
}

DecompressStream& DecompressStream::operator=(DecompressStream&& other) noexcept {
    if (this != &other) {
        detail::DestroyState(state_);
        state_ = other.state_;
        other.state_ = nullptr;
    }
    return *this;
}

Result<DecompressStream> DecompressStream::TryCreate(CompressionFormat format,
                                                     const DecompressStreamLimits& limits,
                                                     Allocator& allocator) noexcept {
    return TryCreateWith(format, limits, allocator, SharedPtr<detail::DictionaryState>());
}

Result<DecompressStream> DecompressStream::TryCreate(const DecompressDictionary& dictionary,
                                                     const DecompressStreamLimits& limits,
                                                     Allocator& allocator) noexcept {
    const auto& state = detail::DictionaryAccess::State(dictionary);
    if (!state) {
        return Result<DecompressStream>::Failure(ErrorCode::StaleHandle);
    }

    return TryCreateWith(state->format, limits, allocator, state);
}

Result<DecompressStream>
DecompressStream::TryCreateWith(CompressionFormat format, const DecompressStreamLimits& limits,
                                Allocator& allocator,
                                const SharedPtr<detail::DictionaryState>& dictionary) noexcept {
    if (!detail::IsStreamFormat(format)) {
        return Result<DecompressStream>::Failure(
            detail::IsKnownFormat(format) ? ErrorCode::Unsupported : ErrorCode::InvalidArgument);
    }
    if (limits.maxOutputBytes == 0 || limits.maxWindowLog < MinZstdWindowLog ||
        limits.maxWindowLog > MaxZstdWindowLog) {
        return Result<DecompressStream>::Failure(ErrorCode::InvalidArgument);
    }

    auto codec = format == CompressionFormat::Zstd
                     ? detail::zstd::CreateStreamDecompressor(limits.maxWindowLog, allocator,
                                                              dictionary.Get())
                     : detail::zlib::CreateDecompressor(format, allocator);
    if (!codec) {
        return Result<DecompressStream>::Failure(codec.Error());
    }

    detail::StreamState initial;
    initial.allocator = &allocator;
    initial.codec = codec.Value();
    initial.format = format;
    initial.compressing = false;
    initial.maxOutputBytes = limits.maxOutputBytes;
    initial.maxWindowLog = limits.maxWindowLog;
    initial.dictionary = dictionary;
    auto state = detail::AdoptCodec(initial);
    if (!state) {
        return Result<DecompressStream>::Failure(state.Error());
    }

    return Result<DecompressStream>::Success(DecompressStream(state.Value()));
}

Result<StreamProgress> DecompressStream::TryProcess(Span<const uint8> input,
                                                    Span<uint8> output) noexcept {
    if (state_ == nullptr) {
        return Result<StreamProgress>::Failure(ErrorCode::StaleHandle);
    }
    if (state_->failure != ErrorCode::None) {
        return Result<StreamProgress>::Failure(state_->failure);
    }
    if (detail::Overlaps(input.Data(), input.Size(), output.Data(), output.Size())) {
        return Result<StreamProgress>::Failure(ErrorCode::InvalidArgument);
    }

    return detail::DecompressStep(*state_, input, output);
}

Result<usize> DecompressStream::TryWriteAppend(Span<const uint8> input,
                                               Array<uint8>& output) noexcept {
    if (state_ == nullptr) {
        return Result<usize>::Failure(ErrorCode::StaleHandle);
    }
    if (state_->failure != ErrorCode::None) {
        return Result<usize>::Failure(state_->failure);
    }
    if (detail::OverlapsArrayStorage(input, output)) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    // output은 base + 남은 허용량보다 길게 늘리지 않는다. 허용량을 다 쓰면 1byte 임시
    // 창으로 끝(블록 끝·체크섬)까지만 진행한다. 거기에 1byte라도 나오면 상한 초과다.
    const usize base = output.Size();
    usize consumed = 0;
    while (consumed < input.Size() || state_->decode.outputPending) {
        const usize allowance = detail::Allowance(*state_);
        const usize chunkBase = output.Size();
        uint8 probe = 0;
        Span<uint8> target(&probe, 1);
        if (allowance != 0) {
            auto grown = output.TryAppendUninitialized(
                allowance < detail::StreamGrowBytes ? allowance : detail::StreamGrowBytes);
            if (!grown) {
                detail::RestoreLength(output, base);
                return detail::Fail<usize>(*state_, grown.Error());
            }
            target = grown.Value();
        }

        const Span<const uint8> rest(input.Data() + consumed, input.Size() - consumed);
        auto step = detail::DecompressStep(*state_, rest, target);
        if (!step) {
            detail::RestoreLength(output, base);
            return Result<usize>::Failure(step.Error());
        }

        // 임시 창에 쓴 byte가 있으면 위에서 상한 초과로 실패했다.
        const StreamProgress progress = step.Value();
        if (allowance != 0) {
            detail::RestoreLength(output, chunkBase + progress.produced);
        }
        consumed += progress.consumed;
        if (progress.complete) {
            break;
        }
        if (progress.consumed == 0 && progress.produced == 0) {
            detail::RestoreLength(output, base);
            return detail::Fail<usize>(*state_, ErrorCode::DependencyFailed);
        }
    }

    return Result<usize>::Success(output.Size() - base);
}

bool DecompressStream::IsAtStreamBoundary() const noexcept {
    return state_ != nullptr && state_->failure == ErrorCode::None && state_->decode.atBoundary;
}

Result<void> DecompressStream::TryFinish() const noexcept {
    if (state_ == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }
    if (state_->failure != ErrorCode::None) {
        return Result<void>::Failure(state_->failure);
    }
    if (!state_->decode.atBoundary || !state_->decode.sawStream) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<void>::Success();
}

void DecompressStream::Reset() noexcept {
    if (state_ == nullptr) {
        return;
    }

    state_->totalOutput = 0;
    state_->decode = detail::StreamDecodeState{};
    auto reset = detail::ResetDecompressorCodec(*state_);
    state_->failure = reset ? ErrorCode::None : reset.Error();
}

} // namespace rocks
