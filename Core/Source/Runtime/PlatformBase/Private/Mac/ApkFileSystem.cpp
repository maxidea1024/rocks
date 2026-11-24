#include <Base/PlacementNew.h>
#include <Platform/ApkFile.h>

namespace rocks::platform {

namespace {

FileStatus Unsupported() noexcept {
    FileStatus status;
    status.code = FileCode::Unsupported;
    return status;
}

// APK가 없는 플랫폼이다. 모든 연산이 Unsupported다(Platform/ApkFile.h).
class NoApkFileSystem final : public FileSystem {
public:
    FileStatus Open(const char*, usize, FileOpenMode, FileHandle*&) noexcept override {
        return Unsupported();
    }

    FileStatus FileExists(const char*, usize, bool& exists) noexcept override {
        exists = false;
        return Unsupported();
    }

    FileStatus Delete(const char*, usize) noexcept override {
        return Unsupported();
    }
};

alignas(NoApkFileSystem) unsigned char noApkFileSystemStorage[sizeof(NoApkFileSystem)];

} // namespace

FileSystem& ApkFileSystem() noexcept {
    static NoApkFileSystem* system = ::new (noApkFileSystemStorage) NoApkFileSystem();
    return *system;
}

FileStatus RegisterApkAssetManager(void*) noexcept {
    return Unsupported();
}

} // namespace rocks::platform
