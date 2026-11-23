#include <Core/FileError.h>
#include <Core/FileRead.h>

namespace rocks {

Result<void> ReadWholeFile(platform::FileSystem& files, StringView path, uint64 maxBytes,
                           Array<uint8>& out, bool& existed) noexcept {
    existed = false;
    out.Clear();
    platform::File file;
    if (auto opened =
            file.Open(files, path.Data(), path.ByteCount(), platform::FileOpenMode::ReadExisting);
        !opened) {
        if (opened.code == platform::FileCode::NotFound) {
            return Result<void>::Success();
        }

        return Result<void>::Failure(FileErrorCode(opened));
    }

    existed = true;
    uint64 size = 0;
    if (auto sized = file.Size(size); !sized) {
        return Result<void>::Failure(FileErrorCode(sized));
    }

    if (size > maxBytes) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    if (auto resized = out.TryResize(static_cast<usize>(size)); !resized) {
        return resized;
    }

    const platform::FileTransfer read = file.ReadExact(out.Data(), out.Size());
    return read.status ? Result<void>::Success()
                       : Result<void>::Failure(FileErrorCode(read.status));
}

} // namespace rocks
