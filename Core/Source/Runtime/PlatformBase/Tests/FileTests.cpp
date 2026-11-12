#include <Platform/Clock.h>
#include <Platform/File.h>
#include <Platform/Thread.h>
#include <TestSupport/Test.h>
#include <TestSupport/FileLink.h>
#include <stdio.h>
#include <string.h>

using namespace rocks;
using namespace rocks::platform;

namespace {

struct TemporaryFile {
    char path[160]{};
    usize length = 0;
    File file;
    bool owned = false;

    bool Create() noexcept {
        const int count = snprintf(path, sizeof(path), "rocks-file-한글-%llu-%llu.tmp",
                                   static_cast<unsigned long long>(MonotonicNanoseconds()),
                                   static_cast<unsigned long long>(CurrentThreadId()));
        if (count <= 0 || static_cast<usize>(count) >= sizeof(path)) {
            return false;
        }
        length = static_cast<usize>(count);
        auto result = file.Open(PhysicalFileSystem(), path, length, FileOpenMode::CreateNew);
        owned = static_cast<bool>(result);
        return owned;
    }

    ~TemporaryFile() noexcept {
        const auto closed = file.Close();
        CHECK(closed);
        if (owned) {
            const auto removed = PhysicalFileSystem().Delete(path, length);
            CHECK(removed);
        }
    }
};

class FakeHandle final : public FileHandle {
public:
    File* owner = nullptr;
    bool reenter = false;
    bool reenteredBusy = false;
    bool closed = false;
    bool destroyed = false;
    bool failClose = false;
    bool zeroWrite = false;
    uint32 successfulReadsBeforeFailure = 0;
    uint32 successfulWritesBeforeFailure = 0xffffffffU;
    uint8 writtenValues[16]{};
    uint32 writtenCount = 0;
    uint32 readCount = 0;
    uint32 readCalls = 0;
    uint32 writeCalls = 0;

    FileTransfer Read(void* destination, usize bytes) noexcept override {
        ++readCalls;
        if (reenter) {
            reenter = false;
            const auto nested = owner->Read(nullptr, 0);
            reenteredBusy = nested.status.code == FileCode::Busy;
        }
        if (bytes != 0) {
            static_cast<uint8*>(destination)[0] = static_cast<uint8>(0x41 + readCount++);
        }
        if (successfulReadsBeforeFailure != 0) {
            --successfulReadsBeforeFailure;
            return {{}, bytes ? 1u : 0u};
        }
        return {{FileCode::SystemError, 123}, bytes ? 1u : 0u};
    }
    FileTransfer Write(const void* source, usize bytes) noexcept override {
        ++writeCalls;
        if (bytes != 0 && !zeroWrite && writtenCount < sizeof(writtenValues)) {
            writtenValues[writtenCount++] = static_cast<const uint8*>(source)[0];
        }
        if (successfulWritesBeforeFailure == 0) {
            return {{FileCode::SystemError, 124}, bytes ? 1u : 0u};
        }
        if (successfulWritesBeforeFailure != 0xffffffffU) {
            --successfulWritesBeforeFailure;
        }
        return {{}, !zeroWrite && bytes ? 1u : 0u};
    }
    FileStatus Seek(int64, FileSeekOrigin, uint64& out) noexcept override {
        out = 7;
        return {FileCode::SystemError, 44};
    }
    FileStatus Size(uint64& out) noexcept override {
        out = 9;
        return {FileCode::SystemError, 45};
    }
    FileStatus Close() noexcept override {
        closed = true;
        return failClose ? FileStatus{FileCode::SystemError, 46} : FileStatus{};
    }
    void Destroy() noexcept override {
        destroyed = true;
    }
};

class FakeSystem final : public FileSystem {
public:
    FakeHandle handle;
    bool fail = false;
    FileStatus Open(const char*, usize, FileOpenMode, FileHandle*& out) noexcept override {
        if (fail) {
            return {FileCode::OutOfMemory, 0};
        }
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

struct ReadWorker {
    File* file;
    uint32 count = 0;
    uint32 sum = 0;
    bool passed = true;
};

void ReadAll(void* data) noexcept {
    auto& worker = *static_cast<ReadWorker*>(data);
    for (;;) {
        uint8 value = 0;
        const auto result = worker.file->Read(&value, 1);
        if (!result.status) {
            worker.passed = false;
            return;
        }
        if (result.bytes == 0) {
            return;
        }
        if (result.bytes != 1) {
            worker.passed = false;
            return;
        }
        worker.sum += value;
        ++worker.count;
    }
}

} // namespace

TEST(PlatformFilePhysicalUnicodeOwnershipAndPartialIo) {
    TemporaryFile temporary;
    REQUIRE(temporary.Create());
    bool exists = false;
    REQUIRE(PhysicalFileSystem().Exists(temporary.path, temporary.length, exists));
    CHECK(exists);
    const uint8 contents[] = {1, 2, 3, 4, 5};
    auto write = temporary.file.WriteAll(contents, sizeof(contents));
    REQUIRE(write.status);
    CHECK(write.bytes == sizeof(contents));
    uint64 size = 0;
    REQUIRE(temporary.file.Size(size));
    CHECK(size == sizeof(contents));
    uint64 position = 91;
    REQUIRE(temporary.file.Seek(0, FileSeekOrigin::Begin, position));
    CHECK(position == 0);
    uint8 data[16]{};
    auto read = temporary.file.ReadExact(data, sizeof(contents));
    REQUIRE(read.status);
    CHECK(read.bytes == sizeof(contents));
    CHECK(memcmp(data, contents, sizeof(contents)) == 0);
    REQUIRE(temporary.file.Seek(0, FileSeekOrigin::Begin, position));
    read = temporary.file.ReadExact(data, sizeof(data));
    CHECK(read.status.code == FileCode::EndOfFile && read.bytes == sizeof(contents));
    read = temporary.file.Read(data, sizeof(data));
    REQUIRE(read.status);
    CHECK(read.bytes == 0);
    CHECK(temporary.file.Read(nullptr, 1).status.code == FileCode::InvalidArgument);
    CHECK(temporary.file.Write(nullptr, 1).status.code == FileCode::InvalidArgument);
    CHECK(temporary.file.Read(nullptr, 0).status);
    CHECK(temporary.file.Write(nullptr, 0).status);
    position = 77;
    CHECK(!temporary.file.Seek(-1, FileSeekOrigin::Begin, position));
    CHECK(position == 77);
    REQUIRE(temporary.file.Close());
    CHECK(!temporary.file.IsOpen());
    CHECK(temporary.file.Read(data, 1).status.code == FileCode::NotOpen);
    REQUIRE(temporary.file.Open(PhysicalFileSystem(), temporary.path, temporary.length,
                                FileOpenMode::ReadExisting));
    CHECK(!temporary.file.Write(contents, 1).status);
    REQUIRE(temporary.file.Close());
    REQUIRE(PhysicalFileSystem().Delete(temporary.path, temporary.length));
    temporary.owned = false;
    REQUIRE(PhysicalFileSystem().Exists(temporary.path, temporary.length, exists));
    CHECK(!exists);
    CHECK(PhysicalFileSystem().Delete(temporary.path, temporary.length).code == FileCode::NotFound);
}

TEST(PlatformFileRejectsMalformedPathAndKeepsOutputs) {
    auto& system = PhysicalFileSystem();
    const char embedded[] = {'a', '\0', 'b'};
    const char bad[] = {static_cast<char>(0xed), static_cast<char>(0xa0), static_cast<char>(0x80)};
    bool exists = true;
    CHECK(system.Exists(embedded, sizeof(embedded), exists).code == FileCode::InvalidArgument);
    CHECK(exists);
    CHECK(system.Exists(bad, sizeof(bad), exists).code == FileCode::InvalidArgument);
    CHECK(exists);
    CHECK(system.Exists(nullptr, 0, exists).code == FileCode::InvalidArgument);
    File file;
    CHECK(file.Open(system, bad, sizeof(bad), FileOpenMode::ReadExisting).code ==
          FileCode::InvalidArgument);
    CHECK(!file.IsOpen());
    CHECK(file.Open(system, "x", 1, static_cast<FileOpenMode>(100)).code ==
          FileCode::InvalidArgument);
    uint64 output = 123;
    CHECK(file.Size(output).code == FileCode::NotOpen);
    CHECK(output == 123);
}

TEST(PlatformFileBackendFailureAndReentryContracts) {
    FakeSystem system;
    File file;
    CHECK(file.ReadExact(nullptr, 0).status.code == FileCode::NotOpen);
    CHECK(file.WriteAll(nullptr, 0).status.code == FileCode::NotOpen);
    system.fail = true;
    CHECK(file.Open(system, "x", 1, FileOpenMode::ReadExisting).code == FileCode::OutOfMemory);
    CHECK(!file.IsOpen());
    system.fail = false;
    REQUIRE(file.Open(system, "x", 1, FileOpenMode::ReadExisting));
    CHECK(file.ReadExact(nullptr, 0).status);
    CHECK(file.WriteAll(nullptr, 0).status);
    CHECK(system.handle.readCalls == 0 && system.handle.writeCalls == 0);
    CHECK(file.Open(system, "x", 1, FileOpenMode::ReadExisting).code == FileCode::AlreadyOpen);
    system.handle.owner = &file;
    system.handle.reenter = true;
    uint8 buffer[3]{};
    auto read = file.Read(buffer, sizeof(buffer));
    CHECK(read.status.code == FileCode::SystemError && read.status.nativeError == 123);
    CHECK(read.bytes == 1);
    CHECK(system.handle.reenteredBusy);
    auto write = file.Write(buffer, sizeof(buffer));
    CHECK(write.status && write.bytes == 1);
    const uint8 payload[] = {0x31, 0x72, 0xc4};
    system.handle.writtenCount = 0;
    write = file.WriteAll(payload, sizeof(payload));
    CHECK(write.status && write.bytes == sizeof(payload));
    CHECK(system.handle.writtenCount == sizeof(payload));
    CHECK(memcmp(system.handle.writtenValues, payload, sizeof(payload)) == 0);
    system.handle.zeroWrite = true;
    write = file.WriteAll(buffer, sizeof(buffer));
    CHECK(write.status.code == FileCode::NoProgress && write.bytes == 0);
    system.handle.successfulReadsBeforeFailure = 1;
    system.handle.readCount = 0;
    const auto exact = file.ReadExact(buffer, sizeof(buffer));
    CHECK(exact.status.code == FileCode::SystemError && exact.bytes == 2);
    CHECK(buffer[0] == 0x41 && buffer[1] == 0x42);
    system.handle.zeroWrite = false;
    system.handle.writtenCount = 0;
    system.handle.successfulWritesBeforeFailure = 1;
    write = file.WriteAll(payload, sizeof(payload));
    CHECK(write.status.code == FileCode::SystemError && write.status.nativeError == 124 &&
          write.bytes == 2);
    CHECK(system.handle.writtenCount == 2 && system.handle.writtenValues[0] == payload[0] &&
          system.handle.writtenValues[1] == payload[1]);
    uint64 value = 777;
    CHECK(!file.Seek(0, FileSeekOrigin::Begin, value));
    CHECK(value == 777);
    CHECK(!file.Size(value));
    CHECK(value == 777);
    system.handle.failClose = true;
    const auto closed = file.Close();
    CHECK(!closed && closed.nativeError == 46);
    CHECK(system.handle.closed && system.handle.destroyed && !file.IsOpen());
    CHECK(file.Close());
}

TEST(PlatformFileConcurrentReadsShareOnePosition) {
    TemporaryFile temporary;
    REQUIRE(temporary.Create());
    uint8 contents[512];
    for (usize i = 0; i < sizeof(contents); ++i) {
        contents[i] = 1;
    }
    const auto written = temporary.file.WriteAll(contents, sizeof(contents));
    REQUIRE(written.status && written.bytes == sizeof(contents));
    uint64 position = 0;
    REQUIRE(temporary.file.Seek(0, FileSeekOrigin::Begin, position));
    ReadWorker workers[] = {{&temporary.file}, {&temporary.file}};
    Thread threads[2];
    REQUIRE(threads[0].Start(ReadAll, &workers[0]) == ThreadStatus::Success);
    if (threads[1].Start(ReadAll, &workers[1]) != ThreadStatus::Success) {
        CHECK(threads[0].Join() == ThreadStatus::Success);
        CHECK(false);
        return;
    }
    CHECK(threads[0].Join() == ThreadStatus::Success);
    CHECK(threads[1].Join() == ThreadStatus::Success);
    CHECK(workers[0].passed && workers[1].passed);
    CHECK(workers[0].count + workers[1].count == sizeof(contents));
    CHECK(workers[0].sum + workers[1].sum == sizeof(contents));
}

TEST(PlatformFileExistsFollowsSymlinkTarget) {
    TemporaryFile temporary;
    REQUIRE(temporary.Create());
    REQUIRE(temporary.file.Close());
    char link[200]{};
    const int count = snprintf(link, sizeof(link), "%s.link", temporary.path);
    REQUIRE(count > 0 && static_cast<usize>(count) < sizeof(link));
    const auto outcome = test::CreateFileLink(link, temporary.path);
    if (outcome == test::FileLinkResult::Unavailable) {
        fprintf(stderr, "SKIP file symlink: host permissions or filesystem do not allow links\n");
        return;
    }
    REQUIRE(outcome == test::FileLinkResult::Created);
    bool exists = false;
    CHECK(PhysicalFileSystem().Exists(link, static_cast<usize>(count), exists));
    CHECK(exists);
    const auto removed = PhysicalFileSystem().Delete(temporary.path, temporary.length);
    CHECK(removed);
    if (removed) {
        temporary.owned = false;
        exists = true;
        CHECK(PhysicalFileSystem().Exists(link, static_cast<usize>(count), exists));
        CHECK(!exists);
    }
    CHECK(PhysicalFileSystem().Delete(link, static_cast<usize>(count)));
}
