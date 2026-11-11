#include <Platform/AsyncFile.h>
#include <Platform/Clock.h>
#include <Platform/File.h>
#include <Tests/Support/Test.h>
#include <stdio.h>

using namespace rocks;
using namespace rocks::platform;

// 실제 OS 파일로 비동기 읽기 계약을 검사한다. 지원하는 모든 backend(Windows: Native와 WorkerPool,
// macOS: WorkerPool)에 같은 테스트를 적용한다.
namespace {

constexpr usize PatternBytes = 1 << 20; // 1 MiB

uint8 PatternByte(uint64 offset) noexcept {
    return static_cast<uint8>((offset * 2654435761ull) >> 13);
}

// 테스트용 패턴 파일이다. 소멸 시 삭제한다.
struct PatternFile {
    char path[160]{};
    usize length = 0;
    bool created = false;

    bool Create(usize bytes) noexcept {
        const int count = snprintf(path, sizeof(path), "rocks-async-%llu-%llu.tmp",
                                   static_cast<unsigned long long>(MonotonicNanoseconds()),
                                   static_cast<unsigned long long>(CurrentThreadId()));
        if (count <= 0 || static_cast<usize>(count) >= sizeof(path)) {
            return false;
        }
        length = static_cast<usize>(count);
        File file;
        if (!file.Open(PhysicalFileSystem(), path, length, FileOpenMode::CreateNew)) {
            return false;
        }
        created = true;
        uint8 chunk[4096];
        for (usize written = 0; written < bytes;) {
            const usize size = bytes - written < sizeof(chunk) ? bytes - written : sizeof(chunk);
            for (usize index = 0; index < size; ++index) {
                chunk[index] = PatternByte(written + index);
            }
            auto result = file.WriteAll(chunk, size);
            if (!result.status || result.bytes != size) {
                return false;
            }
            written += size;
        }
        return static_cast<bool>(file.Close());
    }

    ~PatternFile() noexcept {
        if (created) {
            CHECK(PhysicalFileSystem().Delete(path, length));
        }
    }
};

bool MatchesPattern(const uint8* data, uint64 offset, usize bytes) noexcept {
    for (usize index = 0; index < bytes; ++index) {
        if (data[index] != PatternByte(offset + index)) {
            return false;
        }
    }
    return true;
}

// 사용 가능한 backend 목록이다. Native를 지원하지 않는 플랫폼은 Start가 Unsupported다.
template <typename Body>
void ForEachBackend(Body&& body) {
    const AsyncIoBackend backends[] = {AsyncIoBackend::Native, AsyncIoBackend::WorkerPool};
    for (AsyncIoBackend backend : backends) {
        AsyncIoContext context;
        auto started = context.Start(backend, 3);
        if (backend == AsyncIoBackend::Native && started.code == FileCode::Unsupported) {
            continue;
        }
        REQUIRE(started);
        body(context);
        CHECK(context.Shutdown());
    }
}

struct CallbackLog {
    int calls = 0; // 원자적으로 갱신
    usize lastBytes = 0;
};

// callback은 Wait가 반환된 뒤에 끝날 수 있다(계약). 고정 횟수 yield 대신 시간 제한으로 기다린다.
bool WaitForCallbacks(const CallbackLog& log, int expected) noexcept {
    const uint64 deadline = MonotonicNanoseconds() + 10000000000ull;
    while (__atomic_load_n(&log.calls, __ATOMIC_ACQUIRE) < expected) {
        if (MonotonicNanoseconds() > deadline) {
            return false;
        }
        YieldThread();
    }
    return __atomic_load_n(&log.calls, __ATOMIC_ACQUIRE) == expected;
}

void CountingCallback(const AsyncReadResult& result, void* raw) noexcept {
    auto* log = static_cast<CallbackLog*>(raw);
    __atomic_store_n(&log->lastBytes, result.bytes, __ATOMIC_RELAXED);
    __atomic_fetch_add(&log->calls, 1, __ATOMIC_ACQ_REL);
}

} // namespace

TEST(AsyncFile_DefaultBackendIsDeclared) {
    const AsyncIoBackend backend = DefaultAsyncIoBackend();
    CHECK(backend == AsyncIoBackend::Native || backend == AsyncIoBackend::WorkerPool);
    AsyncIoContext context;
    REQUIRE(context.Start(backend));
    CHECK(context.IsRunning() && context.Backend() == backend);
    CHECK(context.Start(backend).code == FileCode::AlreadyOpen);
    CHECK(context.Shutdown());
    CHECK(!context.IsRunning());
    CHECK(context.Shutdown().code == FileCode::NotOpen);
    // 재시작 가능
    REQUIRE(context.Start(AsyncIoBackend::WorkerPool, 1));
    CHECK(context.Shutdown());
    CHECK(context.Start(AsyncIoBackend::WorkerPool, 0).code == FileCode::InvalidArgument);
}

TEST(AsyncFile_ManyConcurrentReadsMatchFileContents) {
    PatternFile pattern;
    REQUIRE(pattern.Create(PatternBytes));
    ForEachBackend([&](AsyncIoContext& context) {
        AsyncReadFile file;
        REQUIRE(file.Open(context, pattern.path, pattern.length));
        uint64 size = 0;
        REQUIRE(file.Size(size));
        CHECK(size == PatternBytes);
        constexpr int Count = 32;
        static uint8 buffers[Count][8192];
        AsyncReadRequest requests[Count];
        CallbackLog log;
        for (int index = 0; index < Count; ++index) {
            const uint64 offset = static_cast<uint64>(index) * 30011u;
            REQUIRE(file.SubmitRead(requests[index], offset, buffers[index], sizeof(buffers[index]),
                                    CountingCallback, &log));
        }
        bool allMatch = true;
        for (int index = 0; index < Count; ++index) {
            REQUIRE(requests[index].Wait(30000) == WaitResult::Signaled);
            const AsyncReadResult result = requests[index].Result();
            allMatch = allMatch && result.status && result.bytes == sizeof(buffers[index]) &&
                       MatchesPattern(buffers[index], static_cast<uint64>(index) * 30011u,
                                      sizeof(buffers[index]));
        }
        CHECK(allMatch);
        // 모든 callback은 정확히 한 번씩 실행된다(Wait 반환 뒤에 끝날 수 있으므로 기다린다).
        CHECK(WaitForCallbacks(log, Count));
        CHECK(file.Close());
    });
}

TEST(AsyncFile_EndOfFileAndShortReads) {
    PatternFile pattern;
    REQUIRE(pattern.Create(10000));
    ForEachBackend([&](AsyncIoContext& context) {
        AsyncReadFile file;
        REQUIRE(file.Open(context, pattern.path, pattern.length));
        uint8 buffer[4096];
        AsyncReadRequest request;
        // 끝에 걸친 읽기는 짧은 성공이다.
        REQUIRE(file.SubmitRead(request, 9000, buffer, sizeof(buffer)));
        REQUIRE(request.Wait(30000) == WaitResult::Signaled);
        CHECK(request.Result().status && request.Result().bytes == 1000);
        CHECK(MatchesPattern(buffer, 9000, 1000));
        // 같은 요청 객체를 완료 후 다시 쓸 수 있다. 파일 끝 이상은 0 byte 성공이다.
        REQUIRE(file.SubmitRead(request, 10000, buffer, sizeof(buffer)));
        REQUIRE(request.Wait(30000) == WaitResult::Signaled);
        CHECK(request.Result().status && request.Result().bytes == 0);
        REQUIRE(file.SubmitRead(request, 1ull << 40, buffer, sizeof(buffer)));
        REQUIRE(request.Wait(30000) == WaitResult::Signaled);
        CHECK(request.Result().status && request.Result().bytes == 0);
        // 끝 위치가 부호 있는 64-bit offset 최대값까지는 받는다(파일 끝 너머이므로 0 byte).
        REQUIRE(file.SubmitRead(request, 0x7fffffffffffffffull - 8, buffer, 8));
        REQUIRE(request.Wait(30000) == WaitResult::Signaled);
        CHECK(request.Result().status && request.Result().bytes == 0);
        // 0 byte 요청도 같은 완료 경로로 통지된다.
        REQUIRE(file.SubmitRead(request, 0, nullptr, 0));
        REQUIRE(request.Wait(30000) == WaitResult::Signaled);
        CHECK(request.Result().status && request.Result().bytes == 0);
        CHECK(file.Close());
    });
}

TEST(AsyncFile_InvalidSubmissionsLeaveRequestUntouched) {
    PatternFile pattern;
    REQUIRE(pattern.Create(4096));
    ForEachBackend([&](AsyncIoContext& context) {
        AsyncReadFile file;
        AsyncReadRequest request;
        uint8 buffer[16];
        CHECK(file.SubmitRead(request, 0, buffer, sizeof(buffer)).code == FileCode::NotOpen);
        CHECK(!request.IsComplete() && request.Wait(0) == WaitResult::Failed);
        REQUIRE(file.Open(context, pattern.path, pattern.length));
        CHECK(file.Open(context, pattern.path, pattern.length).code == FileCode::AlreadyOpen);
        CHECK(file.SubmitRead(request, 0, nullptr, 8).code == FileCode::InvalidArgument);
        CHECK(file.SubmitRead(request, ~0ull - 3, buffer, 8).code == FileCode::InvalidArgument);
        // 2^63 이상은 OS가 특수 값(현재 위치·파일 끝)으로 해석할 수 있으므로 거부한다.
        CHECK(file.SubmitRead(request, 1ull << 63, buffer, 8).code == FileCode::InvalidArgument);
        CHECK(file.SubmitRead(request, 0x7fffffffffffffffull - 7, buffer, 8).code ==
              FileCode::InvalidArgument);
        CHECK(!request.IsComplete());
        CHECK(file.Close());
        CHECK(file.Close().code == FileCode::NotOpen);
        // 경로 오류
        AsyncReadFile missing;
        CHECK(missing.Open(context, "rocks-async-missing-file.tmp").code == FileCode::NotFound);
        const char embedded[] = {'a', '\0', 'b'};
        CHECK(missing.Open(context, embedded, 3).code == FileCode::InvalidArgument);
    });
    AsyncIoContext stopped;
    AsyncReadFile file;
    CHECK(file.Open(stopped, pattern.path, pattern.length).code == FileCode::NotOpen);
}

TEST(AsyncFile_CloseIsBusyWhileRequestsAreInFlight) {
    PatternFile pattern;
    REQUIRE(pattern.Create(PatternBytes));
    ForEachBackend([&](AsyncIoContext& context) {
        AsyncReadFile file;
        REQUIRE(file.Open(context, pattern.path, pattern.length));
        static uint8 large[PatternBytes];
        AsyncReadRequest request;
        REQUIRE(file.SubmitRead(request, 0, large, sizeof(large)));
        // 진행 중인 요청은 다시 제출할 수 없고, 파일 닫기·context 종료는 Busy다.
        if (!request.IsComplete()) {
            const auto again = file.SubmitRead(request, 0, large, 16);
            CHECK(again.code == FileCode::Busy || request.IsComplete());
            const auto closed = file.Close();
            CHECK(closed.code == FileCode::Busy || request.IsComplete());
            if (closed) {
                REQUIRE(file.Open(context, pattern.path, pattern.length));
            }
        }
        CHECK(context.Shutdown().code == FileCode::Busy);
        REQUIRE(request.Wait(30000) == WaitResult::Signaled);
        CHECK(request.Result().status && request.Result().bytes == sizeof(large));
        CHECK(MatchesPattern(large, 0, sizeof(large)));
        CHECK(file.Close());
    });
}

TEST(AsyncFile_CancellationReportsExactlyOneOutcome) {
    PatternFile pattern;
    REQUIRE(pattern.Create(PatternBytes));
    ForEachBackend([&](AsyncIoContext& context) {
        AsyncReadFile file;
        REQUIRE(file.Open(context, pattern.path, pattern.length));
        constexpr int Count = 64;
        static uint8 buffers[Count][16384];
        AsyncReadRequest requests[Count];
        CallbackLog log;
        for (int index = 0; index < Count; ++index) {
            REQUIRE(file.SubmitRead(requests[index], static_cast<uint64>(index) * 16384u,
                                    buffers[index], sizeof(buffers[index]), CountingCallback,
                                    &log));
            requests[index].Cancel();
            requests[index].Cancel(); // 반복 취소는 무해하다.
        }
        int cancelled = 0;
        int completed = 0;
        bool consistent = true;
        for (int index = 0; index < Count; ++index) {
            REQUIRE(requests[index].Wait(30000) == WaitResult::Signaled);
            const AsyncReadResult result = requests[index].Result();
            if (result.status.code == FileCode::Cancelled) {
                ++cancelled;
            } else {
                // 취소가 완료와 경합해 정상 완료되면 데이터는 유효해야 한다.
                ++completed;
                consistent =
                    consistent && result.status && result.bytes == 16384 &&
                    MatchesPattern(buffers[index], static_cast<uint64>(index) * 16384u, 16384);
            }
        }
        CHECK(consistent && cancelled + completed == Count);
        CHECK(WaitForCallbacks(log, Count));
        // 완료된 요청의 취소는 아무것도 바꾸지 않는다.
        const AsyncReadResult before = requests[0].Result();
        requests[0].Cancel();
        CHECK(requests[0].Result().status.code == before.status.code);
        CHECK(file.Close());
    });
}

TEST(AsyncFile_DestructorsWaitForInFlightRequests) {
    PatternFile pattern;
    REQUIRE(pattern.Create(PatternBytes));
    ForEachBackend([&](AsyncIoContext& context) {
        static uint8 buffer[PatternBytes];
        {
            AsyncReadFile file;
            REQUIRE(file.Open(context, pattern.path, pattern.length));
            AsyncReadRequest request;
            REQUIRE(file.SubmitRead(request, 0, buffer, sizeof(buffer)));
            // 요청 소멸자가 취소 후 완료를 기다린 뒤 파일 소멸자가 닫는다.
        }
        {
            AsyncReadRequest request;
            AsyncReadFile file;
            REQUIRE(file.Open(context, pattern.path, pattern.length));
            REQUIRE(file.SubmitRead(request, 0, buffer, sizeof(buffer)));
            // 파일이 먼저 소멸한다(선언 역순). 파일 소멸자가 진행 중 요청을 취소하고 기다린다.
        }
        // 모든 요청과 파일이 정리되었으므로 종료할 수 있다. 공용 루프의 Shutdown을 위해 다시
        // 시작한다.
        CHECK(context.Shutdown());
        REQUIRE(context.Start(context.Backend(), 2));
    });
}

// ---- 비동기 쓰기 ----
namespace {

// 쓰기 테스트용 경로다. 소멸 시 남아 있으면 삭제한다.
struct TempPath {
    char path[160]{};
    usize length = 0;

    bool Make(const char* tag) noexcept {
        static int counter = 0;
        const int count = snprintf(path, sizeof(path), "rocks-async-%s-%llu-%d.tmp", tag,
                                   static_cast<unsigned long long>(MonotonicNanoseconds()),
                                   __atomic_add_fetch(&counter, 1, __ATOMIC_RELAXED));
        if (count <= 0 || static_cast<usize>(count) >= sizeof(path)) {
            return false;
        }
        length = static_cast<usize>(count);
        return true;
    }

    ~TempPath() noexcept {
        if (length != 0) {
            (void)PhysicalFileSystem().Delete(path, length);
        }
    }
};

// 같은 context에서 파일 전체를 읽어 확인한다.
bool ReadBack(AsyncIoContext& context, const TempPath& target, uint8* buffer, usize capacity,
              usize& bytes) noexcept {
    AsyncReadFile reader;
    if (!reader.Open(context, target.path, target.length)) {
        return false;
    }
    AsyncIoRequest request;
    if (!reader.SubmitRead(request, 0, buffer, capacity) ||
        request.Wait(30000) != WaitResult::Signaled || !request.Result().status) {
        return false;
    }
    bytes = request.Result().bytes;
    return static_cast<bool>(reader.Close());
}

} // namespace

TEST(AsyncFile_ConcurrentWritesReadBackExactly) {
    ForEachBackend([&](AsyncIoContext& context) {
        TempPath target;
        REQUIRE(target.Make("write"));
        constexpr int Count = 8;
        constexpr usize Chunk = 64 * 1024;
        static uint8 source[Count * Chunk];
        for (usize index = 0; index < sizeof(source); ++index) {
            source[index] = PatternByte(index);
        }
        AsyncWriteFile file;
        REQUIRE(file.Open(context, target.path, target.length, FileOpenMode::CreateNew));
        AsyncIoRequest requests[Count];
        CallbackLog log;
        // 역순으로 제출해도 각 요청은 자기 offset에 쓴다.
        for (int index = Count - 1; index >= 0; --index) {
            const usize offset = static_cast<usize>(index) * Chunk;
            REQUIRE(file.SubmitWrite(requests[index], offset, source + offset, Chunk,
                                     CountingCallback, &log));
        }
        bool allWritten = true;
        for (AsyncIoRequest& request : requests) {
            REQUIRE(request.Wait(30000) == WaitResult::Signaled);
            const AsyncIoResult result = request.Result();
            allWritten = allWritten && result.status && result.bytes == Chunk;
        }
        CHECK(allWritten);
        CHECK(WaitForCallbacks(log, Count));
        CHECK(file.Flush());
        uint64 size = 0;
        REQUIRE(file.Size(size));
        CHECK(size == sizeof(source));
        CHECK(file.Close());
        static uint8 readBack[Count * Chunk + 16];
        usize bytes = 0;
        REQUIRE(ReadBack(context, target, readBack, sizeof(readBack), bytes));
        CHECK(bytes == sizeof(source) && MatchesPattern(readBack, 0, bytes));
    });
}

TEST(AsyncFile_WriteOpenModesAndExtension) {
    ForEachBackend([&](AsyncIoContext& context) {
        TempPath target;
        REQUIRE(target.Make("modes"));
        AsyncWriteFile file;
        // 읽기 전용 모드는 쓰기 파일에 쓸 수 없다. 없는 파일을 기존 모드로 열면 NotFound다.
        CHECK(file.Open(context, target.path, target.length, FileOpenMode::ReadExisting).code ==
              FileCode::InvalidArgument);
        CHECK(
            file.Open(context, target.path, target.length, FileOpenMode::ReadWriteExisting).code ==
            FileCode::NotFound);
        REQUIRE(file.Open(context, target.path, target.length, FileOpenMode::CreateNew));
        // 파일 끝 너머에 쓰면 늘어나고 사이는 0이다.
        const uint8 tail[4] = {1, 2, 3, 4};
        AsyncIoRequest request;
        REQUIRE(file.SubmitWrite(request, 100, tail, sizeof(tail)));
        REQUIRE(request.Wait(30000) == WaitResult::Signaled);
        CHECK(request.Result().status && request.Result().bytes == 4);
        // 0 byte 쓰기는 성공이며 크기를 바꾸지 않는다.
        REQUIRE(file.SubmitWrite(request, 500, nullptr, 0));
        REQUIRE(request.Wait(30000) == WaitResult::Signaled);
        CHECK(request.Result().status && request.Result().bytes == 0);
        uint64 size = 0;
        REQUIRE(file.Size(size));
        CHECK(size == 104);
        // 잘못된 제출은 요청을 바꾸지 않는다.
        CHECK(file.SubmitWrite(request, 0, nullptr, 1).code == FileCode::InvalidArgument);
        CHECK(file.SubmitWrite(request, ~0ull, tail, 4).code == FileCode::InvalidArgument);
        CHECK(file.SubmitWrite(request, ~0ull - 1, tail, 1).code == FileCode::InvalidArgument);
        CHECK(file.SubmitWrite(request, 1ull << 63, tail, 4).code == FileCode::InvalidArgument);
        CHECK(file.Close());
        CHECK(file.Flush().code == FileCode::NotOpen);
        uint8 buffer[128]{};
        usize bytes = 0;
        REQUIRE(ReadBack(context, target, buffer, sizeof(buffer), bytes));
        bool zeros = true;
        for (usize index = 0; index < 100; ++index) {
            zeros = zeros && buffer[index] == 0;
        }
        CHECK(bytes == 104 && zeros && buffer[100] == 1 && buffer[103] == 4);

        // 이미 있으면 CreateNew는 AlreadyExists, ReadWriteExisting은 내용을 유지하며 덮어쓴다.
        CHECK(file.Open(context, target.path, target.length, FileOpenMode::CreateNew).code ==
              FileCode::AlreadyExists);
        REQUIRE(file.Open(context, target.path, target.length, FileOpenMode::ReadWriteExisting));
        const uint8 patch[2] = {9, 9};
        REQUIRE(file.SubmitWrite(request, 101, patch, sizeof(patch)));
        REQUIRE(request.Wait(30000) == WaitResult::Signaled);
        CHECK(request.Result().status);
        CHECK(file.Close());
        REQUIRE(ReadBack(context, target, buffer, sizeof(buffer), bytes));
        CHECK(bytes == 104 && buffer[100] == 1 && buffer[101] == 9 && buffer[102] == 9 &&
              buffer[103] == 4);

        // CreateAlways는 기존 내용을 비운다.
        REQUIRE(file.Open(context, target.path, target.length, FileOpenMode::CreateAlways));
        REQUIRE(file.Size(size));
        CHECK(size == 0);
        CHECK(file.Close());
    });
}

TEST(AsyncFile_WriteDestructorsWaitForInFlightWrites) {
    ForEachBackend([&](AsyncIoContext& context) {
        TempPath target;
        REQUIRE(target.Make("dtor"));
        constexpr usize Bytes = 4 * 1024 * 1024;
        static uint8 source[Bytes];
        for (usize index = 0; index < Bytes; index += 4096) {
            source[index] = static_cast<uint8>(index >> 12);
        }
        CallbackLog log;
        {
            AsyncWriteFile file;
            REQUIRE(file.Open(context, target.path, target.length, FileOpenMode::CreateNew));
            AsyncIoRequest first;
            AsyncIoRequest second;
            REQUIRE(file.SubmitWrite(first, 0, source, Bytes, CountingCallback, &log));
            REQUIRE(file.SubmitWrite(second, Bytes, source, Bytes, CountingCallback, &log));
            // 기다리지 않고 블록을 벗어난다. 요청 소멸자(second, first 순)가 취소 후 완료를
            // 기다리고 그 뒤 파일 소멸자가 닫는다. 각 요청은 완료 또는 취소로 정확히 한 번
            // 통지된다.
        }
        CHECK(WaitForCallbacks(log, 2));
    });
}
