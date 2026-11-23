#include <AssetRuntime/PackageRangeReader.h>
#include <Core/FileError.h>
#include <string.h>

// 패키지 범위 읽기 구현이다(Docs/Design/AssetSystem 7.1절 13 — A2b).
namespace rocks {

namespace {

bool InRange(uint64 offset, usize bytes, uint64 fileBytes) noexcept {
    return offset <= fileBytes && bytes <= fileBytes - offset;
}

} // namespace

Result<void> MemoryPackageRangeReader::ReadAt(uint64 offset, Span<uint8> out) noexcept {
    if (!InRange(offset, out.Size(), bytes_.Size())) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (!out.IsEmpty()) {
        memcpy(out.Data(), bytes_.Data() + offset, out.Size());
    }

    return Result<void>::Success();
}

Result<void> FilePackageRangeReader::Open(platform::FileSystem& files, StringView path) noexcept {
    (void)file_.Close();
    fileBytes_ = 0;
    if (auto opened =
            file_.Open(files, path.Data(), path.ByteCount(), platform::FileOpenMode::ReadExisting);
        !opened) {
        return Result<void>::Failure(FileErrorCode(opened));
    }

    uint64 size = 0;
    if (auto sized = file_.Size(size); !sized) {
        (void)file_.Close();
        return Result<void>::Failure(FileErrorCode(sized));
    }

    fileBytes_ = size;
    return Result<void>::Success();
}

Result<void> FilePackageRangeReader::ReadAt(uint64 offset, Span<uint8> out) noexcept {
    if (!InRange(offset, out.Size(), fileBytes_)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    uint64 position = 0;
    if (auto sought =
            file_.Seek(static_cast<int64>(offset), platform::FileSeekOrigin::Begin, position);
        !sought) {
        return Result<void>::Failure(FileErrorCode(sought));
    }

    if (out.IsEmpty()) {
        return Result<void>::Success();
    }

    if (auto read = file_.ReadExact(out.Data(), out.Size()); !read.status) {
        return Result<void>::Failure(FileErrorCode(read.status));
    }

    return Result<void>::Success();
}

} // namespace rocks
