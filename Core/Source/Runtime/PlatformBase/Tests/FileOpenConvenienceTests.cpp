#include <Platform/Clock.h>
#include <Platform/File.h>
#include <Platform/Thread.h>
#include <TestSupport/Test.h>
#include <stdio.h>

using namespace rocks;
using namespace rocks::platform;

namespace {

class CaptureHandle final : public FileHandle {
public:
    FileTransfer Read(void*, usize) noexcept override {
        return {{}, 0};
    }
    FileTransfer Write(const void*, usize) noexcept override {
        return {{}, 0};
    }
    FileStatus Seek(int64, FileSeekOrigin, uint64&) noexcept override {
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

class CaptureSystem final : public FileSystem {
public:
    CaptureHandle handle;
    File* owner = nullptr;
    bool reenter = false;
    FileCode nested = FileCode::Success;
    usize receivedBytes = 0;
    uint32 calls = 0;
    FileStatus Open(const char*, usize bytes, FileOpenMode, FileHandle*& out) noexcept override {
        ++calls;
        if (reenter) {
            reenter = false;
            nested = owner->Open(*this, nullptr, FileOpenMode::ReadExisting).code;
        }
        receivedBytes = bytes;
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

} // namespace

TEST(PlatformFileConvenienceSelectsPhysicalBackend) {
    char path[160]{};
    const int length = snprintf(path, sizeof(path), "rocks-open-한글-%llu-%llu.tmp",
                                static_cast<unsigned long long>(MonotonicNanoseconds()),
                                static_cast<unsigned long long>(CurrentThreadId()));
    REQUIRE(length > 0 && static_cast<usize>(length) < sizeof(path));
    File file;
    auto created = file.Open(path, FileOpenMode::CreateNew);
    REQUIRE(created);
    CHECK(file.Close());
    const auto removed = PhysicalFileSystem().Delete(path, static_cast<usize>(length));
    CHECK(removed);
    CHECK(file.Open(nullptr, FileOpenMode::ReadExisting).code == FileCode::InvalidArgument);
    CHECK(file.Open("", FileOpenMode::ReadExisting).code == FileCode::InvalidArgument);
}

TEST(PlatformFileConvenienceUsesSuppliedSystemAndFirstNul) {
    CaptureSystem system;
    File file;
    const char path[] = {'a', '\0', 'b', '\0'};
    auto opened = file.Open(system, path, FileOpenMode::ReadExisting);
    REQUIRE(opened);
    CHECK(system.calls == 1 && system.receivedBytes == 1);
    CHECK(file.Close());
    auto invalid = file.Open(system, path, 3, FileOpenMode::ReadExisting);
    CHECK(invalid.code == FileCode::InvalidArgument);
    CHECK(system.calls == 1 && !file.IsOpen());
    CHECK(file.Open(system, nullptr, FileOpenMode::ReadExisting).code == FileCode::InvalidArgument);
}

TEST(PlatformFileConvenienceNullReentryReportsBusy) {
    CaptureSystem system;
    File file;
    system.owner = &file;
    system.reenter = true;
    REQUIRE(file.Open(system, "valid", FileOpenMode::ReadExisting));
    CHECK(system.calls == 1);
    CHECK(system.nested == FileCode::Busy);
    CHECK(file.Close());
    CHECK(file.Open(system, nullptr, FileOpenMode::ReadExisting).code == FileCode::InvalidArgument);
}
