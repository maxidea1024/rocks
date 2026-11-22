#include "AssetDatabaseInternal.h"

#include <Core/FileError.h>
#include <Platform/File.h>

namespace rocks::asset_database_detail {

Result<void> ReadWhole(platform::FileSystem& files, StringView path, uint64 limit,
                       Array<uint8>& bytes, bool& existed, bool& tooLarge) noexcept {
    existed = false;
    tooLarge = false;
    platform::File file;
    if (auto opened =
            file.Open(files, path.Data(), path.ByteCount(), platform::FileOpenMode::ReadExisting);
        !opened) {
        return opened.code == platform::FileCode::NotFound
                   ? Result<void>::Success()
                   : Result<void>::Failure(FileErrorCode(opened));
    }

    existed = true;
    uint64 size = 0;
    if (auto sized = file.Size(size); !sized) {
        return Result<void>::Failure(FileErrorCode(sized));
    }

    if (size > limit) {
        tooLarge = true;
        return Result<void>::Success();
    }

    if (auto resized = bytes.TryResize(static_cast<usize>(size)); !resized) {
        return resized;
    }

    // 크기를 잰 뒤 줄었으면 EndOfFile — IO 오류로 보고 다시 시도하게 한다(쓰는 중인 파일).
    const platform::FileTransfer read = file.ReadExact(bytes.Data(), bytes.Size());
    if (!read.status) {
        return Result<void>::Failure(FileErrorCode(read.status));
    }

    return Result<void>::Success();
}

} // namespace rocks::asset_database_detail
