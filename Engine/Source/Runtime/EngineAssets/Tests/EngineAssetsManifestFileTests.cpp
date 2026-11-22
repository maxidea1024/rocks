#include "../Private/EngineAssetsInternal.h"

#include <Core/FileError.h>
#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>
#include <string.h>

// EngineAssets 매니페스트 파일 읽기 시험이다(Docs/Design/AssetSystem.ko.md 11.8절 4): 고장 난
// FileSystem을 끼워 존재 확인·열기·크기·읽기·닫기 실패, 크기 상한, 빈 파일, 할당 실패와 정상 읽기를
// 결정적으로 본다(OS 상태에 기대지 않는다).
using namespace rocks;
namespace engine_assets_manifest_file_tests_file {

namespace {

enum class Fault : uint8 {
    None,
    Exists,
    Missing,
    Open,
    Size,
    Read,
    Close
};

constexpr uint8 FileBytes[] = {1, 2, 3, 4, 5, 6, 7};
constexpr platform::FileStatus SystemError{platform::FileCode::SystemError, 5};

class FakeHandle final : public platform::FileHandle {
public:
    platform::FileTransfer Read(void* destination, usize bytes) noexcept override {
        if (fault == Fault::Read) {
            return {SystemError, 0};
        }

        const usize left = sizeof(FileBytes) - position;
        const usize count = bytes < left ? bytes : left;
        memcpy(destination, FileBytes + position, count);
        position += count;
        return {{}, count};
    }

    platform::FileTransfer Write(const void*, usize) noexcept override {
        return {SystemError, 0};
    }

    platform::FileStatus Seek(int64, platform::FileSeekOrigin, uint64&) noexcept override {
        return SystemError;
    }

    platform::FileStatus Size(uint64& bytes) noexcept override {
        if (fault == Fault::Size) {
            return SystemError;
        }

        bytes = reportedSize;
        return {};
    }

    platform::FileStatus Close() noexcept override {
        return fault == Fault::Close ? SystemError : platform::FileStatus{};
    }

    void Destroy() noexcept override {
    }

    Fault fault = Fault::None;
    uint64 reportedSize = sizeof(FileBytes);
    usize position = 0;
};

class FakeSystem final : public platform::FileSystem {
public:
    platform::FileStatus Open(const char*, usize, platform::FileOpenMode,
                              platform::FileHandle*& out) noexcept override {
        if (handle.fault == Fault::Open) {
            return SystemError;
        }

        handle.position = 0;
        out = &handle;
        return {};
    }

    platform::FileStatus FileExists(const char*, usize, bool& exists) noexcept override {
        if (handle.fault == Fault::Exists) {
            return SystemError;
        }

        exists = handle.fault != Fault::Missing;
        return {};
    }

    platform::FileStatus Delete(const char*, usize) noexcept override {
        return SystemError;
    }

    FakeHandle handle;
};

} // namespace

// 파일 오류(11.8절 4): 존재 확인·열기·크기·읽기·닫기 실패는 그 파일 오류, 없음은 NotFound, 크기가
// 상한을 넘으면 읽지 않고 CapacityExceeded, 빈 파일은 빈 결과, 정상은 내용 그대로다. 할당 실패는 그
// 오류다.
TEST(EngineAssets_ManifestFileReadFailures) {
    SystemAllocator allocator;
    const ErrorCode systemError = FileErrorCode(SystemError);
    struct Case {
        Fault fault;
        ErrorCode expected;
    };
    const Case cases[] = {
        {Fault::Exists, systemError}, {Fault::Missing, ErrorCode::NotFound},
        {Fault::Open, systemError},   {Fault::Size, systemError},
        {Fault::Read, systemError},   {Fault::Close, systemError},
    };
    for (const Case& one : cases) {
        FakeSystem files;
        files.handle.fault = one.fault;
        Array<uint8> out(allocator);
        const auto read =
            engine_assets_detail::ReadManifestFile(files, "Cooked/CookedManifest.rman", 1024, out);
        CHECK(read.Error() == one.expected);
    }

    {
        FakeSystem files;
        files.handle.reportedSize = 1025;
        Array<uint8> out(allocator);
        CHECK(engine_assets_detail::ReadManifestFile(files, "M", 1024, out).Error() ==
              ErrorCode::CapacityExceeded);
    }

    {
        FakeSystem files;
        files.handle.reportedSize = 0;
        Array<uint8> out(allocator);
        REQUIRE(engine_assets_detail::ReadManifestFile(files, "M", 1024, out));
        CHECK(out.IsEmpty());
    }

    {
        FakeSystem files;
        Array<uint8> out(allocator);
        REQUIRE(engine_assets_detail::ReadManifestFile(files, "M", 1024, out));
        CHECK(out.Size() == sizeof(FileBytes) && memcmp(out.Data(), FileBytes, out.Size()) == 0);
    }

    {
        FakeSystem files;
        test::NFailAllocator failing(allocator);
        Array<uint8> out(failing);
        failing.FailOnAttempt(1);
        CHECK(engine_assets_detail::ReadManifestFile(files, "M", 1024, out).Error() ==
              ErrorCode::OutOfMemory);
        failing.DisableFailure();
    }
}

} // namespace engine_assets_manifest_file_tests_file
