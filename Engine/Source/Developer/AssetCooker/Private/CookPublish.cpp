#include <AssetCooker/CookPublish.h>
#include <AssetDatabase/AtomicFileWrite.h>
#include <Core/Algorithms.h>
#include <Core/Encoding/Sha256File.h>
#include <Core/FileError.h>

namespace rocks {

namespace {

// 경로 조립 버퍼의 인라인 크기다(짧은 경로만 — 쿠킹 경로는 대개 allocator로 자란다. 경로 조립은
// 파일 IO에 비해 싸다).
constexpr usize CookPathInlineBytes = 64;
using PathBuilder = StringBuilder<CookPathInlineBytes>;

// 정렬된 경로 목록(String)과 경로(StringView)의 양방향 순서다(BinarySearch).
struct PathOrder {
    bool operator()(const String& left, StringView right) const noexcept {
        return StringView::CompareBytes(left.View(), right) < 0;
    }

    bool operator()(StringView left, const String& right) const noexcept {
        return StringView::CompareBytes(left, right.View()) < 0;
    }
};

bool EntryLess(const CookedManifestEntry& left, const CookedManifestEntry& right) noexcept {
    return GuidLess(left.asset.value, right.asset.value);
}

Result<void> AppendAll(StringBuilderBase& out, Span<const StringView> pieces) noexcept {
    for (StringView piece : pieces) {
        if (auto appended = out.TryAppend(piece); !appended) {
            return appended;
        }
    }

    return Result<void>::Success();
}

// 매니페스트 항목의 bundle을 target에서 만든다.
Result<void> CopyBundles(const CookTarget& target, CookedManifestEntry& entry,
                         Allocator& allocator) noexcept {
    if (auto reserved = entry.bundles.TryReserve(target.bundles.Size()); !reserved) {
        return reserved;
    }

    for (const CookTargetBundle& bundle : target.bundles) {
        CookedManifestBundle copy(allocator);
        if (auto assigned = copy.name.TryAssign(bundle.name.View()); !assigned) {
            return assigned;
        }

        if (auto appended = copy.members.TryAppend(bundle.members.View()); !appended) {
            return appended;
        }

        (void)entry.bundles.TryPushBack(Move(copy));
    }

    return Result<void>::Success();
}

// 항목을 깊이 복사한다(부분 쿠킹 병합).
Result<void> CopyEntry(const CookedManifestEntry& source, CookedManifestEntry& out,
                       Allocator& allocator) noexcept {
    out.asset = source.asset;
    out.type = source.type;
    out.packageSha256 = source.packageSha256;
    out.packageBytes = source.packageBytes;
    out.bulkRawBytes = source.bulkRawBytes;
    out.sidecarBytes = source.sidecarBytes;
    out.sidecarSha256 = source.sidecarSha256;
    out.cookKey = source.cookKey;
    out.flags = source.flags;
    if (auto assigned = out.pathHint.TryAssign(source.pathHint.View()); !assigned) {
        return assigned;
    }

    if (auto appended = out.dependencies.TryAppend(source.dependencies.View()); !appended) {
        return appended;
    }

    if (auto reserved = out.bundles.TryReserve(source.bundles.Size()); !reserved) {
        return reserved;
    }

    for (const CookedManifestBundle& bundle : source.bundles) {
        CookedManifestBundle copy(allocator);
        if (auto assigned = copy.name.TryAssign(bundle.name.View()); !assigned) {
            return assigned;
        }

        if (auto appended = copy.members.TryAppend(bundle.members.View()); !appended) {
            return appended;
        }

        (void)out.bundles.TryPushBack(Move(copy));
    }

    return Result<void>::Success();
}

// path 파일의 크기·SHA-256이다. 오류: 열기·읽기 IO 오류(FileErrorCode), 할당 실패.
Result<void> HashPath(platform::FileSystem& files, StringView path, Allocator& allocator,
                      Sha256Digest& digest, uint64& bytes) noexcept {
    Array<uint8> buffer(allocator);
    if (auto sized = buffer.TryResize(CookHashChunkBytes); !sized) {
        return sized;
    }

    platform::File file;
    if (auto opened =
            file.Open(files, path.Data(), path.ByteCount(), platform::FileOpenMode::ReadExisting);
        !opened) {
        return Result<void>::Failure(FileErrorCode(opened));
    }

    auto hashed = HashFileSha256(file, buffer.View(), digest, bytes);
    (void)file.Close();
    return hashed;
}

// 디렉터리 열거 문맥이다(파일 이름·디렉터리 이름을 모은다 — 링크는 따라가지 않는다).
struct Listing {
    explicit Listing(Allocator& allocator) noexcept : files(allocator), directories(allocator) {
    }

    Array<String> files;
    Array<String> directories;
    ErrorCode error = ErrorCode::None;
};

bool CollectEntry(const platform::DirectoryEntry& entry, void* context) noexcept {
    auto* listing = static_cast<Listing*>(context);
    if (entry.info.isLink) {
        return true;
    }

    Array<String>& target =
        entry.info.type == platform::FileType::Directory ? listing->directories : listing->files;
    String name(target.GetAllocator());
    if (!name.TryAssign(StringView(entry.name, entry.nameBytes)) ||
        !target.TryPushBack(Move(name))) {
        listing->error = ErrorCode::OutOfMemory;
        return false;
    }

    return true;
}

Result<void> List(platform::FileSystem& files, StringView path, Listing& out) noexcept {
    const platform::FileStatus listed =
        files.EnumerateDirectory(path.Data(), path.ByteCount(), CollectEntry, &out);
    if (out.error != ErrorCode::None) {
        return Result<void>::Failure(out.error);
    }

    if (!listed) {
        return Result<void>::Failure(FileErrorCode(listed));
    }

    return Result<void>::Success();
}

// artifact 항목 하나를 복사한다(부분 쿠킹 병합 — 10.3.1절 A4f2 8번).
Result<void> CopyArtifact(const CookedManifestArtifact& source, Array<CookedManifestArtifact>& out,
                          Allocator& allocator) noexcept {
    CookedManifestArtifact copy(allocator);
    copy.kind = source.kind;
    copy.bytes = source.bytes;
    copy.sha256 = source.sha256;
    copy.cookKey = source.cookKey;
    if (auto assigned = copy.name.TryAssign(source.name.View()); !assigned) {
        return assigned;
    }

    if (auto assigned = copy.formatId.TryAssign(source.formatId.View()); !assigned) {
        return assigned;
    }

    return out.TryPushBack(Move(copy));
}

// backend 표를 복사한다.
Result<void> CopyBackends(Span<const CookedManifestBackend> source,
                          Array<CookedManifestBackend>& out, Allocator& allocator) noexcept {
    if (auto reserved = out.TryReserve(source.Size()); !reserved) {
        return reserved;
    }

    for (const CookedManifestBackend& backend : source) {
        CookedManifestBackend copy(allocator);
        if (auto assigned = copy.name.TryAssign(backend.name.View()); !assigned) {
            return assigned;
        }

        if (auto reserved = copy.formats.TryReserve(backend.formats.Size()); !reserved) {
            return reserved;
        }

        for (const String& format : backend.formats) {
            // 형식 수만큼 예약했다(빈 String 추가는 할당하지 않는다).
            String& slot = *copy.formats.TryEmplaceBack(allocator).Value();
            if (auto assigned = slot.TryAssign(format.View()); !assigned) {
                return assigned;
            }
        }

        // backend 수만큼 예약했다.
        (void)out.TryPushBack(Move(copy));
    }

    return Result<void>::Success();
}

} // namespace

Result<void> AppendCookedRoot(StringBuilderBase& out, StringView savedDirectory,
                              StringView profileId) noexcept {
    const StringView pieces[] = {savedDirectory, "/", CookedDirectoryName, "/", profileId};
    return AppendAll(out, pieces);
}

CookPublisher::CookPublisher(Allocator& allocator, const CookPublishConfig& config) noexcept
    : allocator_(&allocator), config_(config), root_(allocator), entries_(allocator),
      artifacts_(allocator), directories_(allocator), diagnostics_(allocator) {
}

// 경고를 남긴다(push의 할당 실패는 그 오류 — 경고를 조용히 잃지 않는다).
Result<void> CookPublisher::Warn(AssetDiagnosticCode code, AssetId asset,
                                 ErrorCode error) noexcept {
    return diagnostics_.TryPushBack(CookDiagnostic{code, true, asset, {}, 0, error});
}

bool CookPublisher::HasErrors() const noexcept {
    for (const CookDiagnostic& diagnostic : diagnostics_) {
        if (!diagnostic.warning) {
            return true;
        }
    }

    return false;
}

Result<void> CookPublisher::Begin(const CookRootLock& lock) noexcept {
    if (begun_ || config_.files == nullptr || config_.savedDirectory.IsEmpty() ||
        config_.profileId.IsEmpty() || !lock.IsHeldFor(config_.savedDirectory, config_.profileId)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    platform::FileSystem& files = *config_.files;
    PathBuilder root(*allocator_);
    if (auto built = AppendCookedRoot(root, config_.savedDirectory, config_.profileId); !built) {
        return built;
    }

    if (auto assigned = root_.TryAssign(root.View()); !assigned) {
        return assigned;
    }

    // 쿠킹 루트까지의 디렉터리 중 없던 것은 그 부모를 Finish가 동기화한다(10절 게시 2 — 첫 쿠킹이면
    // 새 루트의 항목이 매니페스트보다 먼저 매체에 남아야 한다).
    PathBuilder cooked(*allocator_);
    const StringView cookedPieces[] = {config_.savedDirectory, "/", CookedDirectoryName};
    if (auto built = AppendAll(cooked, cookedPieces); !built) {
        return built;
    }

    PathBuilder packagesPath(*allocator_);
    const StringView packagesPieces[] = {root_.View(), "/", CookPackagesDirectoryName};
    if (auto built = AppendAll(packagesPath, packagesPieces); !built) {
        return built;
    }

    PathBuilder shadersPath(*allocator_);
    const StringView shadersPieces[] = {root_.View(), "/", CookShadersDirectoryName};
    if (auto built = AppendAll(shadersPath, shadersPieces); !built) {
        return built;
    }

    const StringView chain[] = {config_.savedDirectory, cooked.View(), root_.View(),
                                packagesPath.View(), shadersPath.View()};
    for (StringView directory : chain) {
        bool exists = false;
        if (auto checked = files.DirectoryExists(directory.Data(), directory.ByteCount(), exists);
            !checked) {
            return Result<void>::Failure(FileErrorCode(checked));
        }

        if (!exists) {
            // 절대 경로라 '/'가 늘 있다(부모는 프로젝트 루트 이하).
            usize slash = directory.ByteCount();
            while (directory.Data()[slash - 1] != '/') {
                --slash;
            }

            if (auto remembered = RememberDirectory(StringView(directory.Data(), slash - 1));
                !remembered) {
                return remembered;
            }
        }
    }

    const StringView children[] = {CookStagingDirectoryName, CookPackagesDirectoryName,
                                   CookShadersDirectoryName};
    for (StringView child : children) {
        PathBuilder path(*allocator_);
        const StringView pieces[] = {root_.View(), "/", child};
        if (auto built = AppendAll(path, pieces); !built) {
            return built;
        }

        if (auto made =
                platform::MakeDirectoryTree(files, path.View().Data(), path.View().ByteCount());
            !made) {
            return Result<void>::Failure(FileErrorCode(made));
        }
    }

    // `.staging`은 잠금을 쥔 쿠커만 쓴다 — 남은 파일은 모두 끝나지 못한 쿠킹의 고아다.
    PathBuilder staging(*allocator_);
    const StringView stagingPieces[] = {root_.View(), "/", CookStagingDirectoryName};
    if (auto built = AppendAll(staging, stagingPieces); !built) {
        return built;
    }

    Listing listing(*allocator_);
    if (auto listed = List(files, staging.View(), listing); !listed) {
        return listed;
    }

    for (const String& name : listing.files) {
        PathBuilder path(*allocator_);
        const StringView pieces[] = {staging.View(), "/", name.View()};
        if (auto built = AppendAll(path, pieces); !built) {
            return built;
        }

        if (auto removed = files.Delete(path.View().Data(), path.View().ByteCount()); !removed) {
            if (auto warned = Warn(AssetDiagnosticCode::None, {}, FileErrorCode(removed));
                !warned) {
                return warned;
            }
        }
    }

    begun_ = true;
    return Result<void>::Success();
}

Result<void> CookPublisher::RememberDirectory(StringView directory) noexcept {
    for (const String& known : directories_) {
        if (known.View() == directory) {
            return Result<void>::Success();
        }
    }

    String copy(*allocator_);
    if (auto assigned = copy.TryAssign(directory); !assigned) {
        return assigned;
    }

    return directories_.TryPushBack(Move(copy));
}

Result<void> CookPublisher::AddEntry(const CookedPackage& package) noexcept {
    CookedManifestEntry entry(*allocator_);
    entry.asset = package.asset;
    entry.type = package.type;
    entry.packageSha256 = package.sha256;
    entry.packageBytes = package.bytes.Size();
    entry.bulkRawBytes = package.bulkRawBytes;
    entry.sidecarBytes = package.sidecar.Size();
    entry.sidecarSha256 = package.sidecarSha256;
    entry.cookKey = package.cookKey;
    entry.flags = package.flags;
    if (auto assigned = entry.pathHint.TryAssign(package.pathHint); !assigned) {
        return assigned;
    }

    if (auto appended = entry.dependencies.TryAppend(package.dependencies); !appended) {
        return appended;
    }

    if (auto reserved = entry.bundles.TryReserve(package.bundles.Size()); !reserved) {
        return reserved;
    }

    for (const CookTargetBundle& bundle : package.bundles) {
        CookedManifestBundle copy(*allocator_);
        if (auto assigned = copy.name.TryAssign(bundle.name.View()); !assigned) {
            return assigned;
        }

        if (auto appended = copy.members.TryAppend(bundle.members.View()); !appended) {
            return appended;
        }

        (void)entry.bundles.TryPushBack(Move(copy));
    }

    return entries_.TryPushBack(Move(entry));
}

Result<void> CookPublisher::Put(const CookedPackage& package) noexcept {
    if (!begun_ || finished_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // sidecar를 먼저 둔다(AS 20.5 4 — 패키지가 가리키기 전에 있어야 한다, 매니페스트는 Finish에서
    // 마지막에 바뀐다).
    if (!package.sidecar.IsEmpty()) {
        PathBuilder sidecar(*allocator_);
        if (auto built = AppendCookedSidecarPath(sidecar, package.asset, package.sidecarSha256);
            !built) {
            return built;
        }

        if (auto put =
                PutFile(sidecar.View(), package.sidecar, package.sidecarSha256, package.asset);
            !put) {
            return put;
        }
    }

    PathBuilder relative(*allocator_);
    if (auto built = AppendCookedPackagePath(relative, package.asset, package.sha256); !built) {
        return built;
    }

    if (auto put = PutFile(relative.View(), package.bytes, package.sha256, package.asset); !put) {
        return put;
    }

    return AddEntry(package);
}

Result<void> CookPublisher::PutFile(StringView relative, Span<const uint8> bytes,
                                    const Sha256Digest& digest, AssetId asset) noexcept {
    platform::FileSystem& files = *config_.files;
    PathBuilder target(*allocator_);
    const StringView targetPieces[] = {root_.View(), "/", relative};
    if (auto built = AppendAll(target, targetPieces); !built) {
        return built;
    }

    // 대상은 `<루트>/Packages/<a>/<파일>`이라 '/'가 늘 있다.
    const StringView targetView = target.View();
    usize slash = targetView.ByteCount();
    while (targetView.Data()[slash - 1] != '/') {
        --slash;
    }

    // 임시 이름은 `.staging/<대상 파일 이름>.tmp`다(`.staging`은 잠금을 쥔 쿠커만 쓰고 Begin이
    // 비운다 — 한 쿠킹에 같은 파일은 한 번만 넣는다).
    const StringView fileName(targetView.Data() + slash, targetView.ByteCount() - slash);
    PathBuilder temp(*allocator_);
    const StringView tempPieces[] = {root_.View(), "/",      CookStagingDirectoryName,
                                     "/",          fileName, ".tmp"};
    if (auto made = AppendAll(temp, tempPieces); !made) {
        return made;
    }

    // 임시 파일에 다 쓰고 Flush·Close한다.
    auto discard = [&]() noexcept {
        (void)files.Delete(temp.View().Data(), temp.View().ByteCount());
    };
    {
        platform::File file;
        if (auto opened = file.Open(files, temp.View().Data(), temp.View().ByteCount(),
                                    platform::FileOpenMode::CreateAlways);
            !opened) {
            return Result<void>::Failure(FileErrorCode(opened));
        }

        const platform::FileTransfer written = file.WriteAll(bytes.Data(), bytes.Size());
        platform::FileStatus finished = written.status;
        if (finished) {
            finished = file.Flush();
        }

        const platform::FileStatus closed = file.Close();
        if (finished && !closed) {
            finished = closed;
        }

        if (!finished) {
            discard();
            return Result<void>::Failure(FileErrorCode(finished));
        }
    }

    bool wrote = false;
    if (auto placed =
            PlaceStaged(temp.View(), targetView, slash - 1, bytes.Size(), digest, asset, wrote);
        !placed) {
        return placed;
    }

    if (wrote) {
        bytesWritten_ += bytes.Size();
    }

    return RememberDirectory(StringView(targetView.Data(), slash - 1));
}

Result<void> CookPublisher::PlaceStaged(StringView temp, StringView target, usize directoryBytes,
                                        uint64 bytes, const Sha256Digest& digest, AssetId asset,
                                        bool& wrote) noexcept {
    platform::FileSystem& files = *config_.files;
    wrote = false;
    auto discard = [&]() noexcept { (void)files.Delete(temp.Data(), temp.ByteCount()); };
    const StringView directory(target.Data(), directoryBytes);
    if (auto made = platform::MakeDirectoryTree(files, directory.Data(), directory.ByteCount());
        !made) {
        discard();
        return Result<void>::Failure(FileErrorCode(made));
    }

    const platform::FileStatus moved =
        files.Move(temp.Data(), temp.ByteCount(), target.Data(), target.ByteCount(), false);
    if (moved) {
        wrote = true;
        return Result<void>::Success();
    }

    if (moved.code != platform::FileCode::AlreadyExists) {
        discard();
        return Result<void>::Failure(FileErrorCode(moved));
    }

    // 같은 이름(같은 내용 해시)이 있다 — 크기·SHA-256으로 확인한다.
    Sha256Digest existing;
    uint64 existingBytes = 0;
    auto hashed = HashPath(files, target, *allocator_, existing, existingBytes);
    if (!hashed && hashed.Error() == ErrorCode::OutOfMemory) {
        discard();
        return hashed;
    }

    if (hashed && existingBytes == bytes && existing == digest) {
        discard();
        return Result<void>::Success();
    }

    // 손상된 게시 파일이다(시나리오 27) — 경고하고 새 byte로 교체한다.
    if (auto warned = Warn(AssetDiagnosticCode::CorruptEntry, asset,
                           hashed ? ErrorCode::None : hashed.Error());
        !warned) {
        discard();
        return warned;
    }

    const platform::FileStatus replaced =
        files.Replace(temp.Data(), temp.ByteCount(), target.Data(), target.ByteCount());
    if (!replaced && replaced.code != platform::FileCode::NotDurable) {
        discard();
        return Result<void>::Failure(FileErrorCode(replaced));
    }

    wrote = true;
    return Result<void>::Success();
}

Result<void> CookPublisher::PutArtifact(CookArtifactKind kind, StringView name, StringView formatId,
                                        const Sha256Digest& cookKey, StringView stagingFile,
                                        const CookedManifestArtifact** placed) noexcept {
    if (!begun_ || finished_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    platform::FileSystem& files = *config_.files;
    auto discard = [&]() noexcept {
        (void)files.Delete(stagingFile.Data(), stagingFile.ByteCount());
    };
    // 크기·SHA-256은 단계가 말한 값이 아니라 파일에서 직접 계산한다(10.3절 3항).
    CookedManifestArtifact artifact(*allocator_);
    artifact.kind = kind;
    artifact.cookKey = cookKey;
    if (auto hashed = HashPath(files, stagingFile, *allocator_, artifact.sha256, artifact.bytes);
        !hashed) {
        discard();
        return hashed;
    }

    if (artifact.bytes == 0 || artifact.bytes > CookedArtifactMaxBytes) {
        discard();
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (auto assigned = artifact.name.TryAssign(name); !assigned) {
        discard();
        return assigned;
    }

    if (auto assigned = artifact.formatId.TryAssign(formatId); !assigned) {
        discard();
        return assigned;
    }

    PathBuilder target(*allocator_);
    const StringView targetPieces[] = {root_.View(), "/"};
    Result<void> built = AppendAll(target, targetPieces);
    if (built) {
        // 칸 문법 위반은 InvalidArgument다(경로 조각 — 쿠킹 루트 밖을 가리키지 못한다).
        built = AppendCookedArtifactPath(target, artifact);
    }

    if (!built) {
        discard();
        return built;
    }

    if (auto reserved = artifacts_.TryReserve(artifacts_.Size() + 1); !reserved) {
        discard();
        return reserved;
    }

    // 대상은 `<루트>/Shaders/<파일>`이다.
    const usize directoryBytes =
        root_.View().ByteCount() + 1 + CookShadersDirectoryName.ByteCount();
    bool wrote = false;
    if (auto moved = PlaceStaged(stagingFile, target.View(), directoryBytes, artifact.bytes,
                                 artifact.sha256, {}, wrote);
        !moved) {
        return moved;
    }

    if (wrote) {
        bytesWritten_ += artifact.bytes;
    }

    if (auto remembered = RememberDirectory(StringView(target.View().Data(), directoryBytes));
        !remembered) {
        return remembered;
    }

    // 예약했다.
    (void)artifacts_.TryPushBack(Move(artifact));
    if (placed != nullptr) {
        *placed = &artifacts_[artifacts_.Size() - 1];
    }

    return Result<void>::Success();
}

Result<void> CookPublisher::ReuseArtifact(const CookedManifestArtifact& artifact) noexcept {
    if (!begun_ || finished_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return CopyArtifact(artifact, artifacts_, *allocator_);
}

Result<bool> CookPublisher::ArtifactFileMatches(const CookedManifestArtifact& artifact) noexcept {
    if (!begun_) {
        return Result<bool>::Failure(ErrorCode::InvalidArgument);
    }

    PathBuilder path(*allocator_);
    const StringView pieces[] = {root_.View(), "/"};
    Result<void> built = AppendAll(path, pieces);
    if (built) {
        built = AppendCookedArtifactPath(path, artifact);
    }

    if (!built) {
        // 칸 위반 항목은 게시 파일이 아니다(후보가 아니다).
        return built.Error() == ErrorCode::InvalidArgument ? Result<bool>::Success(false)
                                                           : Result<bool>::Failure(built.Error());
    }

    Sha256Digest digest;
    uint64 bytes = 0;
    auto hashed = HashPath(*config_.files, path.View(), *allocator_, digest, bytes);
    if (!hashed) {
        return hashed.Error() == ErrorCode::OutOfMemory ? Result<bool>::Failure(hashed.Error())
                                                        : Result<bool>::Success(false);
    }

    return Result<bool>::Success(bytes == artifact.bytes && digest == artifact.sha256);
}

Result<void> CookPublisher::Reuse(const CookedManifestEntry& existing, const CookTarget& target,
                                  StringView pathHint) noexcept {
    if (!begun_ || finished_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    CookedManifestEntry entry(*allocator_);
    entry.asset = existing.asset;
    entry.type = existing.type;
    entry.packageSha256 = existing.packageSha256;
    entry.packageBytes = existing.packageBytes;
    entry.bulkRawBytes = existing.bulkRawBytes;
    entry.sidecarBytes = existing.sidecarBytes;
    entry.sidecarSha256 = existing.sidecarSha256;
    entry.cookKey = existing.cookKey;
    entry.flags = target.primary ? static_cast<uint32>(CookedManifestFlags::PrimaryAsset) : 0u;
    if (auto assigned = entry.pathHint.TryAssign(pathHint); !assigned) {
        return assigned;
    }

    if (auto appended = entry.dependencies.TryAppend(existing.dependencies.View()); !appended) {
        return appended;
    }

    if (auto bundles = CopyBundles(target, entry, *allocator_); !bundles) {
        return bundles;
    }

    return entries_.TryPushBack(Move(entry));
}

Result<void> CookPublisher::SyncDirectories() noexcept {
    // 안쪽부터(경로가 긴 것부터) 동기화한다 — 패키지 디렉터리 뒤에 Packages.
    PathBuilder packages(*allocator_);
    const StringView pieces[] = {root_.View(), "/", CookPackagesDirectoryName};
    if (auto built = AppendAll(packages, pieces); !built) {
        return built;
    }

    if (auto remembered = RememberDirectory(packages.View()); !remembered) {
        return remembered;
    }

    algo::Sort(directories_.View(), [](const String& left, const String& right) noexcept {
        return left.View().ByteCount() > right.View().ByteCount();
    });
    for (const String& directory : directories_) {
        const platform::FileStatus synced =
            config_.files->SyncDirectory(directory.View().Data(), directory.View().ByteCount());
        if (!synced) {
            if (synced.code != platform::FileCode::NotDurable) {
                return Result<void>::Failure(FileErrorCode(synced));
            }

            if (auto warned = Warn(AssetDiagnosticCode::NotDurable, {}, ErrorCode::None); !warned) {
                return warned;
            }
        }
    }

    return Result<void>::Success();
}

Result<void> CookPublisher::RemoveUnreferenced(const CookedManifest& manifest) noexcept {
    platform::FileSystem& files = *config_.files;
    // 새 매니페스트가 가리키는 파일 경로(쿠킹 루트 기준) 목록이다(정렬 — 이진 탐색).
    Array<String> referenced(*allocator_);
    if (auto reserved = referenced.TryReserve(manifest.entries.Size() + manifest.artifacts.Size());
        !reserved) {
        return reserved;
    }

    for (const CookedManifestArtifact& artifact : manifest.artifacts) {
        PathBuilder path(*allocator_);
        if (auto built = AppendCookedArtifactPath(path, artifact); !built) {
            return built;
        }

        String copy(*allocator_);
        if (auto assigned = copy.TryAssign(path.View()); !assigned) {
            return assigned;
        }

        (void)referenced.TryPushBack(Move(copy));
    }

    for (const CookedManifestEntry& entry : manifest.entries) {
        PathBuilder path(*allocator_);
        if (auto built = AppendCookedPackagePath(path, entry.asset, entry.packageSha256); !built) {
            return built;
        }

        String copy(*allocator_);
        if (auto assigned = copy.TryAssign(path.View()); !assigned) {
            return assigned;
        }

        if (auto pushed = referenced.TryPushBack(Move(copy)); !pushed) {
            return pushed;
        }

        // sidecar도 남긴다(AS 20.5 6 — 낡은 `.rbsc`만 지운다).
        if (entry.sidecarBytes == 0) {
            continue;
        }

        PathBuilder sidecar(*allocator_);
        if (auto built = AppendCookedSidecarPath(sidecar, entry.asset, entry.sidecarSha256);
            !built) {
            return built;
        }

        String sidecarCopy(*allocator_);
        if (auto assigned = sidecarCopy.TryAssign(sidecar.View()); !assigned) {
            return assigned;
        }

        if (auto pushed = referenced.TryPushBack(Move(sidecarCopy)); !pushed) {
            return pushed;
        }
    }

    algo::Sort(referenced.View(), [](const String& left, const String& right) noexcept {
        return StringView::CompareBytes(left.View(), right.View()) < 0;
    });

    // Packages/<a>/<파일> 두 단계를 훑는다(그 밖의 모양은 쿠커가 만든 것이 아니라 건드리지 않는다).
    PathBuilder packages(*allocator_);
    const StringView packagePieces[] = {root_.View(), "/", CookPackagesDirectoryName};
    if (auto built = AppendAll(packages, packagePieces); !built) {
        return built;
    }

    Listing top(*allocator_);
    if (auto listed = List(files, packages.View(), top); !listed) {
        return listed;
    }

    for (const String& shard : top.directories) {
        PathBuilder shardPath(*allocator_);
        const StringView shardPieces[] = {packages.View(), "/", shard.View()};
        if (auto built = AppendAll(shardPath, shardPieces); !built) {
            return built;
        }

        Listing inner(*allocator_);
        if (auto listed = List(files, shardPath.View(), inner); !listed) {
            return listed;
        }

        for (const String& name : inner.files) {
            PathBuilder relative(*allocator_);
            const StringView relativePieces[] = {CookPackagesDirectoryName, "/", shard.View(), "/",
                                                 name.View()};
            if (auto built = AppendAll(relative, relativePieces); !built) {
                return built;
            }

            const usize found = algo::BinarySearch(referenced.View(), relative.View(), PathOrder{});
            if (found != algo::NotFound) {
                continue;
            }

            PathBuilder full(*allocator_);
            const StringView fullPieces[] = {shardPath.View(), "/", name.View()};
            if (auto built = AppendAll(full, fullPieces); !built) {
                return built;
            }

            if (auto removed = files.Delete(full.View().Data(), full.View().ByteCount());
                !removed) {
                if (auto warned = Warn(AssetDiagnosticCode::None, {}, FileErrorCode(removed));
                    !warned) {
                    return warned;
                }
            }
        }
    }

    // Shaders/<파일> 한 단계를 훑는다(10.3.1절 A4f3 4번 — 디렉터리는 건드리지 않는다).
    PathBuilder shaders(*allocator_);
    const StringView shaderPieces[] = {root_.View(), "/", CookShadersDirectoryName};
    if (auto built = AppendAll(shaders, shaderPieces); !built) {
        return built;
    }

    Listing libraries(*allocator_);
    if (auto listed = List(files, shaders.View(), libraries); !listed) {
        return listed;
    }

    for (const String& name : libraries.files) {
        PathBuilder relative(*allocator_);
        const StringView relativePieces[] = {CookShadersDirectoryName, "/", name.View()};
        if (auto built = AppendAll(relative, relativePieces); !built) {
            return built;
        }

        if (algo::BinarySearch(referenced.View(), relative.View(), PathOrder{}) != algo::NotFound) {
            continue;
        }

        PathBuilder full(*allocator_);
        const StringView fullPieces[] = {shaders.View(), "/", name.View()};
        if (auto built = AppendAll(full, fullPieces); !built) {
            return built;
        }

        if (auto removed = files.Delete(full.View().Data(), full.View().ByteCount()); !removed) {
            if (auto warned = Warn(AssetDiagnosticCode::None, {}, FileErrorCode(removed));
                !warned) {
                return warned;
            }
        }
    }

    return Result<void>::Success();
}

// 게시하지 않는 오류 진단을 남기고 끝낸다(진단 push의 할당 실패는 그 오류다).
Result<void> CookPublisher::Refuse(AssetDiagnosticCode code, AssetId asset, AssetId related,
                                   ErrorCode error) noexcept {
    if (auto pushed =
            diagnostics_.TryPushBack(CookDiagnostic{code, false, asset, related, 0, error});
        !pushed) {
        return pushed;
    }

    algo::Sort(diagnostics_.View(), CookDiagnosticBefore);
    return Result<void>::Success();
}

Result<void> CookPublisher::Finish(const CookedManifest* merge,
                                   Span<const AssetId> removed) noexcept {
    if (!begun_ || finished_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    finished_ = true;
    if (auto synced = SyncDirectories(); !synced) {
        return synced;
    }

    CookedManifest manifest(*allocator_);
    if (auto assigned = manifest.profileId.TryAssign(config_.profileId); !assigned) {
        return assigned;
    }

    manifest.profileSettings = config_.profileSettings;
    manifest.cookSettings = config_.cookSettings;
    manifest.cookerVersion = config_.cookerVersion;
    manifest.cookedBy = config_.cookedBy;
    if (auto copied = CopyBackends(config_.backends, manifest.backends, *allocator_); !copied) {
        return copied;
    }

    manifest.artifacts = Move(artifacts_);
    artifacts_ = Array<CookedManifestArtifact>(*allocator_);
    // 정렬은 늘 한다(같은 키 두 번은 writer 검증 ArtifactOrder가 거부한다 — sink가 먼저 막는다).
    algo::Sort(manifest.artifacts.View(), CookedArtifactLess);

    if (auto reserved = manifest.entries.TryReserve(entries_.Size() +
                                                    (merge != nullptr ? merge->entries.Size() : 0));
        !reserved) {
        return reserved;
    }

    for (CookedManifestEntry& entry : entries_) {
        (void)manifest.entries.TryPushBack(Move(entry));
    }

    entries_.Clear();
    algo::Sort(manifest.entries.View(), EntryLess);
    if (merge != nullptr) {
        AssetDiagnostic failure;
        if (auto compatible = CheckCookedManifestMerge(*merge, manifest, &failure); !compatible) {
            return Refuse(AssetDiagnosticCode::IncompatibleCook, {}, {}, compatible.Error());
        }

        const usize fresh = manifest.entries.Size();
        for (const CookedManifestEntry& old : merge->entries) {
            const bool replaced =
                algo::BinarySearch(Span<const CookedManifestEntry>(manifest.entries.Data(), fresh),
                                   old, EntryLess) != algo::NotFound;
            const bool gone =
                algo::BinarySearch(removed, old.asset, [](AssetId left, AssetId right) noexcept {
                    return GuidLess(left.value, right.value);
                }) != algo::NotFound;
            if (replaced || gone) {
                continue;
            }

            CookedManifestEntry copy(*allocator_);
            if (auto copied = CopyEntry(old, copy, *allocator_); !copied) {
                return copied;
            }

            (void)manifest.entries.TryPushBack(Move(copy));
        }

        algo::Sort(manifest.entries.View(), EntryLess);
        // artifact 표: 이번 쿠킹이 만든 항목이 이기고, 같은 (kind, 이름, formatId)가 없는 이전
        // 항목은 남는다(위에서 이번 구간을 정렬했다 — 이진 탐색).
        const usize freshArtifacts = manifest.artifacts.Size();
        for (const CookedManifestArtifact& old : merge->artifacts) {
            const bool replaced = algo::BinarySearch(Span<const CookedManifestArtifact>(
                                                         manifest.artifacts.Data(), freshArtifacts),
                                                     old, CookedArtifactLess) != algo::NotFound;
            if (replaced) {
                continue;
            }

            if (auto copied = CopyArtifact(old, manifest.artifacts, *allocator_); !copied) {
                return copied;
            }
        }

        algo::Sort(manifest.artifacts.View(), CookedArtifactLess);
    }

    Array<uint8> bytes(*allocator_);
    AssetDiagnostic failure;
    if (auto written = WriteCookedManifest(*allocator_, manifest, bytes, &failure); !written) {
        if (written.Error() == ErrorCode::OutOfMemory) {
            return written;
        }

        // 매니페스트 규칙 위반(대상 계산과 어긋난 입력) — 게시하지 않는다.
        return Refuse(failure.code, failure.asset, failure.related, written.Error());
    }

    auto replaced = WriteFileAtomically(*config_.files, *allocator_, root_.View(),
                                        CookedManifestFileName, bytes.View());
    if (!replaced) {
        return Result<void>::Failure(replaced.Error());
    }

    if (replaced.Value().notDurable) {
        if (auto warned = Warn(AssetDiagnosticCode::NotDurable, {}, ErrorCode::None); !warned) {
            return warned;
        }
    }

    published_ = true;
    if (merge == nullptr) {
        if (auto cleaned = RemoveUnreferenced(manifest); !cleaned) {
            return cleaned;
        }
    }

    algo::Sort(diagnostics_.View(), CookDiagnosticBefore);
    return Result<void>::Success();
}

Result<void> PlanCookReuse(Allocator& allocator, const CookReuseRequest& request,
                           CookReusePlan& out) noexcept {
    out.reuse.Clear();
    out.entries.Clear();
    out.count = 0;
    if (request.files == nullptr || request.targets == nullptr || request.keys == nullptr ||
        request.keys->keys.Size() != request.targets->targets.Size()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const usize count = request.targets->targets.Size();
    if (auto sized = out.reuse.TryResize(count); !sized) {
        return sized;
    }

    if (auto sized = out.entries.TryResize(count); !sized) {
        return sized;
    }

    for (usize at = 0; at < count; ++at) {
        out.reuse[at] = 0;
        out.entries[at] = nullptr;
    }

    const CookedManifest* existing = request.existing;
    if (existing == nullptr || existing->profileId.View() != request.profileId ||
        !(existing->profileSettings == request.profileSettings) ||
        existing->cookerVersion != request.cookerVersion) {
        return Result<void>::Success();
    }

    for (usize at = 0; at < count; ++at) {
        const CookKey& key = request.keys->keys[at];
        const CookedManifestEntry* entry = existing->Find(key.asset);
        if (!key.hasKey || entry == nullptr || !(entry->cookKey == key.cookKey)) {
            continue;
        }

        PathBuilder path(allocator);
        const StringView pieces[] = {request.cookedRoot, "/"};
        Result<void> built = AppendAll(path, pieces);
        if (built) {
            built = AppendCookedPackagePath(path, entry->asset, entry->packageSha256);
        }

        if (!built) {
            return built;
        }

        Sha256Digest digest;
        uint64 bytes = 0;
        auto hashed = HashPath(*request.files, path.View(), allocator, digest, bytes);
        if (!hashed && hashed.Error() == ErrorCode::OutOfMemory) {
            return hashed;
        }

        // 없거나 읽을 수 없거나 다르면 다시 쿠킹한다(시나리오 27).
        bool same = hashed && bytes == entry->packageBytes && digest == entry->packageSha256;
        // sidecar도 같아야 한다(AS 20.5 5 — sidecar만 지워지거나 바뀌어도 다시 쿠킹).
        if (same && entry->sidecarBytes != 0) {
            PathBuilder sidecar(allocator);
            Result<void> sidecarBuilt = AppendAll(sidecar, pieces);
            if (sidecarBuilt) {
                sidecarBuilt = AppendCookedSidecarPath(sidecar, entry->asset, entry->sidecarSha256);
            }

            if (!sidecarBuilt) {
                return sidecarBuilt;
            }

            Sha256Digest sidecarDigest;
            uint64 sidecarBytes = 0;
            auto sidecarHashed =
                HashPath(*request.files, sidecar.View(), allocator, sidecarDigest, sidecarBytes);
            if (!sidecarHashed && sidecarHashed.Error() == ErrorCode::OutOfMemory) {
                return sidecarHashed;
            }

            same = sidecarHashed && sidecarBytes == entry->sidecarBytes &&
                   sidecarDigest == entry->sidecarSha256;
        }

        if (same) {
            out.reuse[at] = 1;
            out.entries[at] = entry;
            ++out.count;
        }
    }

    return Result<void>::Success();
}

} // namespace rocks
