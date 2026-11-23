#include "AssetDatabaseInternal.h"

#include <AssetDatabase/AssetDatabaseLock.h>
#include <Core/FileError.h>
#include <Core/Text/StringBuilder.h>

// 프로젝트 쓰기 잠금이다(Docs/Design/AssetSystem 8절, 7.1절 4 — A2a).
namespace rocks {

namespace {

// 잠금 파일 경로의 인라인 byte 수다(넘으면 allocator로 자란다).
constexpr usize LockPathBytes = 512;

Sha256Digest RootDigest(StringView root) noexcept {
    return Sha256::Hash(
        Span<const uint8>(reinterpret_cast<const uint8*>(root.Data()), root.ByteCount()));
}

} // namespace

Result<void> AssetDatabaseLock::Acquire(const ProjectLayout& layout,
                                        uint32 waitMilliseconds) noexcept {
    if (lock_.IsHeld()) {
        return Result<void>::Failure(ErrorCode::AlreadyExists);
    }

    // Initialize 전 배치는 파일 시스템 루트 아래 경로를 만들 수 있으므로 거부한다.
    if (layout.Root().IsEmpty()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const StringView directory = layout.IntermediateDirectory();
    if (directory.ByteCount() + 1 + AssetDatabaseLockFileName.ByteCount() > MaxNativePathBytes()) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    platform::FileSystem& files = platform::PhysicalFileSystem();
    if (auto made = platform::MakeDirectoryTree(files, directory.Data(), directory.ByteCount());
        !made) {
        return Result<void>::Failure(FileErrorCode(made));
    }

    StringBuilder<LockPathBytes> path(layout.GetAllocator());
    const StringView pieces[] = {directory, "/", AssetDatabaseLockFileName};
    if (auto appended = asset_database_detail::AppendAll(path, pieces); !appended) {
        return appended;
    }

    if (auto acquired =
            lock_.Acquire(path.View().Data(), path.View().ByteCount(), waitMilliseconds);
        !acquired) {
        return Result<void>::Failure(FileErrorCode(acquired));
    }

    root_ = RootDigest(layout.Root());
    return Result<void>::Success();
}

bool AssetDatabaseLock::IsHeldFor(const ProjectLayout& layout) const noexcept {
    return lock_.IsHeld() && !layout.Root().IsEmpty() && root_ == RootDigest(layout.Root());
}

} // namespace rocks
