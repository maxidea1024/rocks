#include <Platform/File.h>
#include <TestSupport/HostFailure.h>

namespace rocks::test {
namespace {

class BrokenHandle final : public platform::FileHandle {
public:
    platform::File* owner = nullptr;
    uint32 operation = 0;
    platform::FileTransfer Read(void*, usize bytes) noexcept override {
        if (operation == 4) {
            (void)owner->IsOpen();
        }
        return {{}, bytes + 1};
    }
    platform::FileTransfer Write(const void*, usize bytes) noexcept override {
        return {{}, operation == 6 || operation == 7 ? bytes + 1 : 0};
    }
    platform::FileStatus Seek(int64, platform::FileSeekOrigin, uint64&) noexcept override {
        return {};
    }
    platform::FileStatus Size(uint64&) noexcept override {
        return {};
    }
    platform::FileStatus Close() noexcept override {
        return operation == 3 ? platform::FileStatus{platform::FileCode::SystemError, 1}
                              : platform::FileStatus{};
    }
    void Destroy() noexcept override {
    }
};

class BrokenSystem final : public platform::FileSystem {
public:
    BrokenHandle handle;
    uint32 operation = 0;
    platform::FileStatus Open(const char*, usize, platform::FileOpenMode,
                              platform::FileHandle*& out) noexcept override {
        if (operation == 0) {
            return {};
        }
        out = &handle;
        if (operation == 1) {
            return {platform::FileCode::SystemError, 1};
        }
        return {};
    }
    platform::FileStatus Exists(const char*, usize, bool&) noexcept override {
        return {platform::FileCode::Unsupported, 0};
    }
    platform::FileStatus Delete(const char*, usize) noexcept override {
        return {platform::FileCode::Unsupported, 0};
    }
};
} // namespace

// 자식 프로세스에서만 사용한다. 진단 종류는 호출 측에서 별도로 확인한다.
int RunFileNegativeControl(uint32 operation) noexcept {
    PrepareFatalTestProcess();
    BrokenSystem system;
    system.operation = operation;
    system.handle.operation = operation;
    {
        platform::File file;
        system.handle.owner = &file;
        const auto opened = file.Open(system, "x", 1, platform::FileOpenMode::ReadExisting);
        if (!opened) {
            return 2;
        }
        uint8 value = 0;
        if (operation == 2 || operation == 4) {
            (void)file.Read(&value, 1);
        } else if (operation == 5) {
            (void)file.ReadExact(&value, 1);
        } else if (operation == 6) {
            (void)file.Write(&value, 1);
        } else if (operation == 7) {
            (void)file.WriteAll(&value, 1);
        }
    }
    return 0;
}

} // namespace rocks::test
