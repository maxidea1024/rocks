#include <AssetRuntime/CookedPackageIo.h>

#include <Core/FileError.h>
#include <Core/Move.h>

namespace rocks {

namespace {

// 요청 하나의 IO 상태다. 조각 읽기는 요청 객체 둘을 번갈아 쓴다(완료 callback 안에서 끝난 요청을
// 다시 쓰지 않는다 — 다음 조각은 다른 요청 객체로 제출한다).
struct AsyncPackageReadState {
    CookedPackageRead* read = nullptr;
    uint64 chunkBytes = 0;
    uint64 offset = 0;
    uint32 nextRequest = 0;
    platform::AsyncReadFile file;
    platform::AsyncIoRequest requests[2];
};

void FinishAsyncPackageRead(AsyncPackageReadState& state, ErrorCode error,
                            uint64 fileBytes) noexcept {
    CookedPackageRead& read = *state.read;
    CookedPackageReadResult result;
    result.error = error;
    result.fileBytes = fileBytes;
    // complete가 마지막 접근이다(그 뒤 로더가 Release로 이 상태를 지울 수 있다).
    read.complete(read, result);
}

void SubmitAsyncPackageChunk(AsyncPackageReadState& state) noexcept;

void OnAsyncPackageChunk(const platform::AsyncIoResult& result, void* context) noexcept {
    auto& state = *static_cast<AsyncPackageReadState*>(context);
    const uint64 remaining = state.read->expectedBytes - state.offset;
    const uint64 expected = remaining < state.chunkBytes ? remaining : state.chunkBytes;
    if (!result.status) {
        FinishAsyncPackageRead(state, FileErrorCode(result.status), state.read->expectedBytes);
        return;
    }

    if (result.bytes != expected) {
        // 크기를 본 뒤 파일이 줄었다.
        FinishAsyncPackageRead(state, ErrorCode::InvalidArgument, state.read->expectedBytes);
        return;
    }

    state.offset += expected;
    if (state.offset == state.read->expectedBytes) {
        FinishAsyncPackageRead(state, ErrorCode::None, state.read->expectedBytes);
        return;
    }

    SubmitAsyncPackageChunk(state);
}

void SubmitAsyncPackageChunk(AsyncPackageReadState& state) noexcept {
    const uint64 remaining = state.read->expectedBytes - state.offset;
    const uint64 bytes = remaining < state.chunkBytes ? remaining : state.chunkBytes;
    platform::AsyncIoRequest& request = state.requests[state.nextRequest];
    state.nextRequest ^= 1u;
    const platform::FileStatus submitted =
        state.file.SubmitRead(request, state.offset, state.read->destination + state.offset,
                              static_cast<usize>(bytes), OnAsyncPackageChunk, &state);
    if (!submitted) {
        FinishAsyncPackageRead(state, FileErrorCode(submitted), state.read->expectedBytes);
    }
}

} // namespace

AsyncCookedPackageIo::AsyncCookedPackageIo(Allocator& allocator, platform::AsyncIoContext& io,
                                           uint64 chunkBytes) noexcept
    : allocator_(allocator), io_(io), chunkBytes_(chunkBytes == 0 ? 1 : chunkBytes) {
}

Result<void> AsyncCookedPackageIo::Begin(CookedPackageRead& read) noexcept {
    auto memory =
        allocator_.Allocate(sizeof(AsyncPackageReadState), alignof(AsyncPackageReadState));
    if (!memory) {
        return Result<void>::Failure(memory.Error());
    }

    auto* state = InitializeAt<AsyncPackageReadState>(memory.Value());
    state->read = &read;
    state->chunkBytes = chunkBytes_;
    read.backendState = state;
    const platform::FileStatus opened = state->file.Open(io_, read.path, read.pathBytes);
    if (!opened) {
        FinishAsyncPackageRead(*state, FileErrorCode(opened), 0);
        return Result<void>::Success();
    }

    uint64 size = 0;
    const platform::FileStatus sized = state->file.Size(size);
    if (!sized) {
        FinishAsyncPackageRead(*state, FileErrorCode(sized), 0);
        return Result<void>::Success();
    }

    if (size != read.expectedBytes) {
        FinishAsyncPackageRead(*state, ErrorCode::None, size);
        return Result<void>::Success();
    }

    SubmitAsyncPackageChunk(*state);
    return Result<void>::Success();
}

void AsyncCookedPackageIo::Release(CookedPackageRead& read) noexcept {
    // Begin이 성공한 요청에만 불리므로 상태가 있다.
    auto* state = static_cast<AsyncPackageReadState*>(read.backendState);
    // 완료가 게시된 요청뿐이다(소멸자는 진행 중 요청이 없으므로 기다리지 않는다).
    if (state->file.IsOpen()) {
        (void)state->file.Close();
    }

    state->~AsyncPackageReadState();
    (void)allocator_.Deallocate(state);
    read.backendState = nullptr;
}

} // namespace rocks
