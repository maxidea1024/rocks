#include <AssetDatabase/AssetIndex.h>
#include <Core/FileError.h>

namespace rocks {

namespace {

// 콘텐츠 해시를 흘려 읽는 조각 크기다.
constexpr usize ContentHashChunkBytes = 64 * 1024;

} // namespace

Result<Sha256Digest> AssetIndex::ContentHash(platform::FileSystem& files, AssetId id) noexcept {
    using Out = Sha256Digest;
    // 없음 NotFound·같은 id 둘 이상 AlreadyExists는 FindById와 같다.
    auto found = FindById(id);
    if (!found) {
        return Result<Out>::Failure(found.Error());
    }

    AssetIndexEntry& entry = entries_[static_cast<usize>(found.Value() - entries_.Data())];
    String path(*allocator_);
    Result<void> step = path.TryAppend(layout_->ContentDirectory());
    if (step) {
        step = path.TryAppend("/");
    }

    if (step) {
        step = path.TryAppend(entry.fileName.View());
    }

    if (!step) {
        return Result<Out>::Failure(step.Error());
    }

    platform::FileInfo before;
    if (auto stat = files.Stat(path.View().Data(), path.View().ByteCount(), before); !stat) {
        return Result<Out>::Failure(FileErrorCode(stat));
    }

    if (entry.hasContentHash && entry.contentHashBytes == before.size &&
        entry.contentHashTicks == before.modifiedTicks) {
        return Result<Out>::Success(entry.contentHash);
    }

    Array<uint8> buffer(*allocator_);
    if (auto sized = buffer.TryResize(ContentHashChunkBytes); !sized) {
        return Result<Out>::Failure(sized.Error());
    }

    platform::File file;
    if (auto opened = file.Open(files, path.View().Data(), path.View().ByteCount(),
                                platform::FileOpenMode::ReadExisting);
        !opened) {
        return Result<Out>::Failure(FileErrorCode(opened));
    }

    Sha256 hasher;
    for (;;) {
        const platform::FileTransfer read = file.Read(buffer.Data(), buffer.Size());
        if (!read.status) {
            return Result<Out>::Failure(FileErrorCode(read.status));
        }

        if (read.bytes == 0) {
            break;
        }

        hasher.Update(Span<const uint8>(buffer.Data(), read.bytes));
    }

    (void)file.Close();
    platform::FileInfo after;
    if (auto stat = files.Stat(path.View().Data(), path.View().ByteCount(), after); !stat) {
        return Result<Out>::Failure(FileErrorCode(stat));
    }

    if (after.size != before.size || after.modifiedTicks != before.modifiedTicks) {
        // 해시하는 동안 바뀌었다 — 이 해시는 어느 내용의 것인지 알 수 없다.
        return Result<Out>::Failure(ErrorCode::Busy);
    }

    entry.contentHash = hasher.Finish();
    entry.contentHashBytes = before.size;
    entry.contentHashTicks = before.modifiedTicks;
    entry.hasContentHash = true;
    return Result<Out>::Success(entry.contentHash);
}

} // namespace rocks
