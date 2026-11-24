#include <Base/PlacementNew.h>
#include <Platform/ApkFile.h>
#include <Platform/Atomics.h>
#include <Platform/Memory.h>

#include <android/asset_manager.h>
#include <stdio.h>

namespace rocks::platform {

namespace {

// AAssetManager 경로는 NUL로 끝나는 C 문자열이어야 한다. 그보다 긴 경로는 InvalidArgument다.
constexpr usize MaxApkPathBytes = 1024;

AAssetManager* registeredManager = nullptr;

AAssetManager* Manager() noexcept {
    return AtomicLoad(&registeredManager);
}

// 경로를 NUL 종료 버퍼로 옮긴다. 빈 경로·NUL 포함·너무 긴 경로는 false다.
bool CopyPath(const char* path, usize bytes, char (&buffer)[MaxApkPathBytes]) noexcept {
    if (path == nullptr || bytes == 0 || bytes >= MaxApkPathBytes) {
        return false;
    }

    for (usize index = 0; index < bytes; ++index) {
        if (path[index] == '\0') {
            return false;
        }

        buffer[index] = path[index];
    }

    buffer[bytes] = '\0';
    return true;
}

FileStatus Status(FileCode code) noexcept {
    FileStatus status;
    status.code = code;
    return status;
}

class ApkFileHandle final : public FileHandle {
public:
    explicit ApkFileHandle(AAsset* asset) noexcept : asset_(asset) {
    }

    FileTransfer Read(void* destination, usize bytes) noexcept override {
        FileTransfer transfer;
        if (asset_ == nullptr) {
            transfer.status = Status(FileCode::NotOpen);
            return transfer;
        }

        const int read = AAsset_read(asset_, destination, bytes);
        if (read < 0) {
            transfer.status = Status(FileCode::SystemError);
            return transfer;
        }

        transfer.bytes = static_cast<usize>(read);
        return transfer;
    }

    FileTransfer Write(const void*, usize) noexcept override {
        FileTransfer transfer;
        transfer.status = Status(FileCode::Unsupported);
        return transfer;
    }

    FileStatus Seek(int64 offset, FileSeekOrigin origin, uint64& position) noexcept override {
        if (asset_ == nullptr) {
            return Status(FileCode::NotOpen);
        }

        const int whence = origin == FileSeekOrigin::Begin     ? SEEK_SET
                           : origin == FileSeekOrigin::Current ? SEEK_CUR
                                                               : SEEK_END;
        const off64_t moved = AAsset_seek64(asset_, static_cast<off64_t>(offset), whence);
        if (moved < 0) {
            return Status(FileCode::InvalidArgument);
        }

        position = static_cast<uint64>(moved);
        return {};
    }

    FileStatus Size(uint64& bytes) noexcept override {
        if (asset_ == nullptr) {
            return Status(FileCode::NotOpen);
        }

        bytes = static_cast<uint64>(AAsset_getLength64(asset_));
        return {};
    }

    // 읽기 전용이라 내보낼 것이 없다.
    FileStatus Flush() noexcept override {
        return {};
    }

    FileStatus Close() noexcept override {
        if (asset_ == nullptr) {
            return Status(FileCode::NotOpen);
        }

        AAsset_close(asset_);
        asset_ = nullptr;
        return {};
    }

    void Destroy() noexcept override {
        if (asset_ != nullptr) {
            AAsset_close(asset_);
            asset_ = nullptr;
        }

        this->~ApkFileHandle();
        AlignedFree(this);
    }

private:
    AAsset* asset_;
};

class ApkFileSystemImpl final : public FileSystem {
public:
    FileStatus Open(const char* path, usize pathBytes, FileOpenMode mode,
                    FileHandle*& out) noexcept override {
        if (mode != FileOpenMode::ReadExisting) {
            return Status(FileCode::Unsupported);
        }

        char native[MaxApkPathBytes];
        if (!CopyPath(path, pathBytes, native)) {
            return Status(FileCode::InvalidArgument);
        }

        AAssetManager* manager = Manager();
        AAsset* asset =
            manager != nullptr ? AAssetManager_open(manager, native, AASSET_MODE_RANDOM) : nullptr;
        if (asset == nullptr) {
            return Status(FileCode::NotFound);
        }

        void* storage = AlignedAllocate(sizeof(ApkFileHandle), alignof(ApkFileHandle));
        if (storage == nullptr) {
            AAsset_close(asset);
            return Status(FileCode::OutOfMemory);
        }

        out = ::new (storage) ApkFileHandle(asset);
        return {};
    }

    FileStatus FileExists(const char* path, usize pathBytes, bool& exists) noexcept override {
        char native[MaxApkPathBytes];
        if (!CopyPath(path, pathBytes, native)) {
            return Status(FileCode::InvalidArgument);
        }

        exists = false;
        if (AAssetManager* manager = Manager(); manager != nullptr) {
            if (AAsset* asset = AAssetManager_open(manager, native, AASSET_MODE_UNKNOWN);
                asset != nullptr) {
                AAsset_close(asset);
                exists = true;
            }
        }

        return {};
    }

    FileStatus Delete(const char*, usize) noexcept override {
        return Status(FileCode::Unsupported);
    }

    FileStatus Stat(const char* path, usize pathBytes, FileInfo& info) noexcept override {
        char native[MaxApkPathBytes];
        if (!CopyPath(path, pathBytes, native)) {
            return Status(FileCode::InvalidArgument);
        }

        AAssetManager* manager = Manager();
        if (manager == nullptr) {
            return Status(FileCode::NotFound);
        }

        if (AAsset* asset = AAssetManager_open(manager, native, AASSET_MODE_UNKNOWN);
            asset != nullptr) {
            info = FileInfo{};
            info.type = FileType::File;
            info.readOnly = true;
            info.size = static_cast<uint64>(AAsset_getLength64(asset));
            AAsset_close(asset);
            return {};
        }

        if (HasEntries(manager, native)) {
            info = FileInfo{};
            info.type = FileType::Directory;
            info.readOnly = true;
            return {};
        }

        return Status(FileCode::NotFound);
    }

    FileStatus EnumerateDirectory(const char* path, usize pathBytes, DirectoryVisitor visitor,
                                  void* context) noexcept override {
        char native[MaxApkPathBytes];
        // 루트("")도 받는다(assets/ 바로 아래).
        if (pathBytes != 0 && !CopyPath(path, pathBytes, native)) {
            return Status(FileCode::InvalidArgument);
        }

        if (pathBytes == 0) {
            native[0] = '\0';
        }

        AAssetManager* manager = Manager();
        AAssetDir* directory =
            manager != nullptr ? AAssetManager_openDir(manager, native) : nullptr;
        if (directory == nullptr) {
            return Status(FileCode::NotFound);
        }

        while (const char* name = AAssetDir_getNextFileName(directory)) {
            DirectoryEntry entry;
            entry.name = name;
            usize bytes = 0;
            while (name[bytes] != '\0') {
                ++bytes;
            }

            entry.nameBytes = bytes;
            entry.info.type = FileType::File;
            entry.info.readOnly = true;
            if (!visitor(entry, context)) {
                break;
            }
        }

        AAssetDir_close(directory);
        return {};
    }

private:
    static bool HasEntries(AAssetManager* manager, const char* path) noexcept {
        AAssetDir* directory = AAssetManager_openDir(manager, path);
        if (directory == nullptr) {
            return false;
        }

        const bool any = AAssetDir_getNextFileName(directory) != nullptr;
        AAssetDir_close(directory);
        return any;
    }
};

alignas(ApkFileSystemImpl) unsigned char apkFileSystemStorage[sizeof(ApkFileSystemImpl)];

} // namespace

FileSystem& ApkFileSystem() noexcept {
    static ApkFileSystemImpl* system = ::new (apkFileSystemStorage) ApkFileSystemImpl();
    return *system;
}

FileStatus RegisterApkAssetManager(void* assetManager) noexcept {
    if (assetManager == nullptr) {
        return Status(FileCode::InvalidArgument);
    }

    AtomicStore(&registeredManager, static_cast<AAssetManager*>(assetManager));
    return {};
}

} // namespace rocks::platform
