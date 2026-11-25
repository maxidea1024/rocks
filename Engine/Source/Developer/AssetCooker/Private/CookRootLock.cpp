#include <AssetCooker/CookRootLock.h>

#include <AssetCooker/CookPublish.h>
#include <Core/FileError.h>
#include <Core/Text/StringBuilder.h>

// 쿠킹 루트 쓰기 잠금이다(Docs/Design/AssetSystem 10.3.1절 A4f4 1번).
namespace rocks {

namespace {

// 잠금 파일 경로의 인라인 byte 수다(넘으면 allocator로 자란다).
constexpr usize CookLockPathBytes = 256;

// 쿠킹 루트(`<saved>/Cooked/<profile>`)의 SHA-256이다(조각을 차례로 넣는다 — 할당 없이).
Sha256Digest CookRootDigest(StringView savedDirectory, StringView profileId) noexcept {
    Sha256 hash;
    const StringView pieces[] = {savedDirectory, "/", CookedDirectoryName, "/", profileId};
    for (StringView piece : pieces) {
        hash.Update(
            Span<const uint8>(reinterpret_cast<const uint8*>(piece.Data()), piece.ByteCount()));
    }

    return hash.Finish();
}

} // namespace

Result<void> CookRootLock::Acquire(Allocator& allocator, StringView savedDirectory,
                                   StringView profileId, uint32 waitMilliseconds) noexcept {
    if (lock_.IsHeld()) {
        return Result<void>::Failure(ErrorCode::AlreadyExists);
    }

    if (savedDirectory.IsEmpty() || profileId.IsEmpty()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    StringBuilder<CookLockPathBytes> directory(allocator);
    StringBuilder<CookLockPathBytes> path(allocator);
    const StringView directoryPieces[] = {savedDirectory, "/", CookLockDirectoryName};
    const StringView pathPieces[] = {savedDirectory,     "/",       CookLockDirectoryName, "/",
                                     CookLockFilePrefix, profileId, CookLockFileSuffix};
    if (auto built = directory.TryAppendJoin(directoryPieces, ""); !built) {
        return built;
    }

    if (auto built = path.TryAppendJoin(pathPieces, ""); !built) {
        return built;
    }

    if (path.View().ByteCount() > MaxNativePathBytes()) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    platform::FileSystem& files = platform::PhysicalFileSystem();
    if (auto made = platform::MakeDirectoryTree(files, directory.View().Data(),
                                                directory.View().ByteCount());
        !made) {
        return Result<void>::Failure(FileErrorCode(made));
    }

    if (auto acquired =
            lock_.Acquire(path.View().Data(), path.View().ByteCount(), waitMilliseconds);
        !acquired) {
        return Result<void>::Failure(FileErrorCode(acquired));
    }

    root_ = CookRootDigest(savedDirectory, profileId);
    return Result<void>::Success();
}

bool CookRootLock::IsHeldFor(StringView savedDirectory, StringView profileId) const noexcept {
    return lock_.IsHeld() && !savedDirectory.IsEmpty() && !profileId.IsEmpty() &&
           root_ == CookRootDigest(savedDirectory, profileId);
}

} // namespace rocks
