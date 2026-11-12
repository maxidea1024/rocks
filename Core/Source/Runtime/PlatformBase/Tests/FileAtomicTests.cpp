#include <Platform/File.h>
#include <Platform/Thread.h>
#include <TestSupport/Test.h>

using namespace rocks;
using namespace rocks::platform;

namespace {

class BlockingHandle final : public FileHandle {
public:
    ManualResetEvent firstRead;
    ManualResetEvent resume;
    // File의 직렬화에 결함이 생겨도 테스트 fixture 자체는 data race를 만들지 않는다.
    Mutex state;
    uint32 reads = 0;
    uint32 writes = 0;
    uint8 written[2]{};
    bool writeMode = false;
    bool seekTooEarly = false;

    FileTransfer Read(void* destination, usize bytes) noexcept override {
        uint32 call = 0;
        {
            LockGuard lock(state);
            call = ++reads;
        }
        if (call == 1) {
            firstRead.Signal();
            if (resume.Wait(5000) != WaitResult::Signaled) {
                return {{FileCode::SystemError, 0}, 0};
            }
        }
        if (bytes == 0) {
            return {{}, 0};
        }
        static_cast<uint8*>(destination)[0] = static_cast<uint8>(call);
        return {{}, 1};
    }
    FileTransfer Write(const void* source, usize bytes) noexcept override {
        if (bytes == 0) {
            return {{}, 0};
        }
        uint32 call = 0;
        {
            LockGuard lock(state);
            if (writes < 2) {
                written[writes] = static_cast<const uint8*>(source)[0];
            }
            call = ++writes;
        }
        if (call == 1) {
            firstRead.Signal();
            if (resume.Wait(5000) != WaitResult::Signaled) {
                return {{FileCode::SystemError, 0}, 0};
            }
        }
        return {{}, 1};
    }
    FileStatus Seek(int64, FileSeekOrigin, uint64& position) noexcept override {
        LockGuard lock(state);
        seekTooEarly = writeMode ? writes != 2 : reads != 2;
        position = 0;
        return {};
    }
    FileStatus Size(uint64&) noexcept override {
        return {};
    }
    FileStatus Close() noexcept override {
        return {};
    }
    void Destroy() noexcept override {
    }
};

class BlockingSystem final : public FileSystem {
public:
    BlockingHandle handle;
    FileStatus Open(const char*, usize, FileOpenMode, FileHandle*& out) noexcept override {
        out = &handle;
        return {};
    }
    FileStatus Exists(const char*, usize, bool&) noexcept override {
        return {FileCode::Unsupported, 0};
    }
    FileStatus Delete(const char*, usize) noexcept override {
        return {FileCode::Unsupported, 0};
    }
};

struct Worker {
    explicit Worker(File* value) noexcept : file(value), entered(false) {
    }

    File* file;
    ManualResetEvent entered;
    bool passed = false;
};

void ExactReader(void* data) noexcept {
    auto& worker = *static_cast<Worker*>(data);
    uint8 buffer[2]{};
    const auto result = worker.file->ReadExact(buffer, 2);
    worker.passed = result.status && result.bytes == 2 && buffer[0] == 1 && buffer[1] == 2;
}

void ExactWriter(void* data) noexcept {
    auto& worker = *static_cast<Worker*>(data);
    const uint8 buffer[] = {0x31, 0x72};
    const auto result = worker.file->WriteAll(buffer, 2);
    worker.passed = result.status && result.bytes == 2;
}

void ConcurrentSeek(void* data) noexcept {
    auto& worker = *static_cast<Worker*>(data);
    worker.entered.Signal();
    uint64 position = 9;
    worker.passed = static_cast<bool>(worker.file->Seek(0, FileSeekOrigin::Begin, position));
}

struct DirectWorker {
    BlockingHandle* handle;
    bool write;
    bool passed = false;
};

void DirectTransfer(void* data) noexcept {
    auto& worker = *static_cast<DirectWorker*>(data);
    uint8 value = 42;
    const auto result =
        worker.write ? worker.handle->Write(&value, 1) : worker.handle->Read(&value, 1);
    worker.passed = result.status && result.bytes == 1;
}
} // namespace

TEST(PlatformFileExactReadSerializesWholeOperation) {
    BlockingSystem system;
    File file;
    REQUIRE(file.Open(system, "x", 1, FileOpenMode::ReadExisting));
    Worker reader{&file};
    Worker seeker{&file};
    Thread readThread;
    Thread seekThread;
    REQUIRE(readThread.Start(ExactReader, &reader) == ThreadStatus::Success);
    if (system.handle.firstRead.Wait(3000) != WaitResult::Signaled) {
        system.handle.resume.Signal();
        CHECK(readThread.Join() == ThreadStatus::Success);
        CHECK(false);
        return;
    }
    if (seekThread.Start(ConcurrentSeek, &seeker) != ThreadStatus::Success) {
        system.handle.resume.Signal();
        CHECK(readThread.Join() == ThreadStatus::Success);
        CHECK(false);
        return;
    }
    CHECK(seeker.entered.Wait(3000) == WaitResult::Signaled);
    // 준비 신호 뒤에도 OS 스케줄링은 보장되지 않아 이 시간 검사는 완전한 증명이 아니다.
    CHECK(seekThread.WaitForCompletion(50) == WaitResult::TimedOut);
    system.handle.resume.Signal();
    CHECK(readThread.Join() == ThreadStatus::Success);
    CHECK(seekThread.Join() == ThreadStatus::Success);
    CHECK(reader.passed && seeker.passed);
    CHECK(!system.handle.seekTooEarly);
    CHECK(file.Close());
}

TEST(PlatformFileWriteAllSerializesWholeOperation) {
    BlockingSystem system;
    system.handle.writeMode = true;
    File file;
    REQUIRE(file.Open(system, "x", 1, FileOpenMode::ReadExisting));
    Worker writer{&file};
    Worker seeker{&file};
    Thread writeThread;
    Thread seekThread;
    REQUIRE(writeThread.Start(ExactWriter, &writer) == ThreadStatus::Success);
    if (system.handle.firstRead.Wait(3000) != WaitResult::Signaled) {
        system.handle.resume.Signal();
        CHECK(writeThread.Join() == ThreadStatus::Success);
        CHECK(false);
        return;
    }
    if (seekThread.Start(ConcurrentSeek, &seeker) != ThreadStatus::Success) {
        system.handle.resume.Signal();
        CHECK(writeThread.Join() == ThreadStatus::Success);
        CHECK(false);
        return;
    }
    CHECK(seeker.entered.Wait(3000) == WaitResult::Signaled);
    // 타 스레드의 실행 시점은 보장되지 않는다. 아래 순서 검사는 보조 증거다.
    CHECK(seekThread.WaitForCompletion(50) == WaitResult::TimedOut);
    system.handle.resume.Signal();
    CHECK(writeThread.Join() == ThreadStatus::Success);
    CHECK(seekThread.Join() == ThreadStatus::Success);
    CHECK(writer.passed && seeker.passed);
    CHECK(system.handle.written[0] == 0x31 && system.handle.written[1] == 0x72);
    CHECK(!system.handle.seekTooEarly);
    CHECK(file.Close());
}

// 잘못된 직렬화의 음성 대조다. 첫 backend 전송 중 Seek가 실제로 실행되면
// fixture가 앞선 호출을 검출하는지 확인한다. 직접 호출은 File 계약을 테스트하지 않는다.
TEST(PlatformFileAtomicFixtureDetectsEarlyBackendAccess) {
    const bool modes[] = {false, true};
    for (bool writing : modes) {
        BlockingHandle handle;
        handle.writeMode = writing;
        DirectWorker worker{&handle, writing};
        Thread thread;
        REQUIRE(thread.Start(DirectTransfer, &worker) == ThreadStatus::Success);
        if (handle.firstRead.Wait(3000) != WaitResult::Signaled) {
            handle.resume.Signal();
            CHECK(thread.Join() == ThreadStatus::Success);
            CHECK(false);
            return;
        }
        uint64 position = 9;
        CHECK(handle.Seek(0, FileSeekOrigin::Begin, position));
        CHECK(handle.seekTooEarly);
        handle.resume.Signal();
        CHECK(thread.Join() == ThreadStatus::Success);
        CHECK(worker.passed);
    }
}
