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

} // namespace

Result<void> AppendCookedRoot(StringBuilderBase& out, const ProjectLayout& layout,
                              StringView profileId) noexcept {
    const StringView pieces[] = {layout.SavedDirectory(), "/", CookedDirectoryName, "/", profileId};
    return AppendAll(out, pieces);
}

CookPublisher::CookPublisher(Allocator& allocator, const CookPublishConfig& config) noexcept
    : allocator_(&allocator), config_(config), root_(allocator), entries_(allocator),
      directories_(allocator), diagnostics_(allocator) {
}

void CookPublisher::Warn(AssetDiagnosticCode code, AssetId asset, ErrorCode error) noexcept {
    // 경고 push의 할당 실패는 버린다(경고는 게시를 막지 않는다 — 결과가 바뀌지 않는다).
    (void)diagnostics_.TryPushBack(CookDiagnostic{code, true, asset, {}, 0, error});
}

bool CookPublisher::HasErrors() const noexcept {
    for (const CookDiagnostic& diagnostic : diagnostics_) {
        if (!diagnostic.warning) {
            return true;
        }
    }

    return false;
}

Result<void> CookPublisher::Begin(const AssetDatabaseLock& lock) noexcept {
    if (begun_ || config_.files == nullptr || config_.layout == nullptr ||
        !lock.IsHeldFor(*config_.layout)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    platform::FileSystem& files = *config_.files;
    PathBuilder root(*allocator_);
    if (auto built = AppendCookedRoot(root, *config_.layout, config_.profileId); !built) {
        return built;
    }

    if (auto assigned = root_.TryAssign(root.View()); !assigned) {
        return assigned;
    }

    // 쿠킹 루트까지의 디렉터리 중 없던 것은 그 부모를 Finish가 동기화한다(10절 게시 2 — 첫 쿠킹이면
    // 새 루트의 항목이 매니페스트보다 먼저 매체에 남아야 한다).
    PathBuilder cooked(*allocator_);
    const StringView cookedPieces[] = {config_.layout->SavedDirectory(), "/", CookedDirectoryName};
    if (auto built = AppendAll(cooked, cookedPieces); !built) {
        return built;
    }

    PathBuilder packagesPath(*allocator_);
    const StringView packagesPieces[] = {root_.View(), "/", CookPackagesDirectoryName};
    if (auto built = AppendAll(packagesPath, packagesPieces); !built) {
        return built;
    }

    const StringView chain[] = {config_.layout->SavedDirectory(), cooked.View(), root_.View(),
                                packagesPath.View()};
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

    const StringView children[] = {CookStagingDirectoryName, CookPackagesDirectoryName};
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
            Warn(AssetDiagnosticCode::None, {}, FileErrorCode(removed));
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

    platform::FileSystem& files = *config_.files;
    PathBuilder target(*allocator_);
    const StringView targetPieces[] = {root_.View(), "/"};
    Result<void> built = AppendAll(target, targetPieces);
    if (built) {
        built = AppendCookedPackagePath(target, package.asset, package.sha256);
    }

    if (!built) {
        return built;
    }

    // 대상은 `<루트>/Packages/<a>/<파일>`이라 '/'가 늘 있다.
    const StringView targetView = target.View();
    usize slash = targetView.ByteCount();
    while (targetView.Data()[slash - 1] != '/') {
        --slash;
    }

    // 임시 이름은 `.staging/<대상 파일 이름>.tmp`다(`.staging`은 잠금을 쥔 쿠커만 쓰고 Begin이
    // 비운다 — 한 쿠킹에 같은 패키지는 한 번만 넣는다).
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

        const platform::FileTransfer written =
            file.WriteAll(package.bytes.Data(), package.bytes.Size());
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

    const StringView directory(targetView.Data(), slash - 1);
    if (auto made = platform::MakeDirectoryTree(files, directory.Data(), directory.ByteCount());
        !made) {
        discard();
        return Result<void>::Failure(FileErrorCode(made));
    }

    const platform::FileStatus moved = files.Move(temp.View().Data(), temp.View().ByteCount(),
                                                  targetView.Data(), targetView.ByteCount(), false);
    bool wrote = static_cast<bool>(moved);
    if (!moved) {
        if (moved.code != platform::FileCode::AlreadyExists) {
            discard();
            return Result<void>::Failure(FileErrorCode(moved));
        }

        // 같은 이름(같은 내용 해시)이 있다 — 크기·SHA-256으로 확인한다.
        Sha256Digest digest;
        uint64 bytes = 0;
        auto hashed = HashPath(files, targetView, *allocator_, digest, bytes);
        if (!hashed && hashed.Error() == ErrorCode::OutOfMemory) {
            discard();
            return hashed;
        }

        if (hashed && bytes == package.bytes.Size() && digest == package.sha256) {
            discard();
        } else {
            // 손상된 게시 파일이다(시나리오 27) — 경고하고 새 byte로 교체한다.
            Warn(AssetDiagnosticCode::CorruptEntry, package.asset,
                 hashed ? ErrorCode::None : hashed.Error());
            const platform::FileStatus replaced =
                files.Replace(temp.View().Data(), temp.View().ByteCount(), targetView.Data(),
                              targetView.ByteCount());
            if (!replaced && replaced.code != platform::FileCode::NotDurable) {
                discard();
                return Result<void>::Failure(FileErrorCode(replaced));
            }

            wrote = true;
        }
    }

    if (wrote) {
        bytesWritten_ += package.bytes.Size();
    }

    if (auto remembered = RememberDirectory(directory); !remembered) {
        return remembered;
    }

    return AddEntry(package);
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

            Warn(AssetDiagnosticCode::NotDurable, {}, ErrorCode::None);
        }
    }

    return Result<void>::Success();
}

Result<void> CookPublisher::RemoveUnreferenced(const CookedManifest& manifest) noexcept {
    platform::FileSystem& files = *config_.files;
    // 새 매니페스트가 가리키는 파일 경로(쿠킹 루트 기준) 목록이다(정렬 — 이진 탐색).
    Array<String> referenced(*allocator_);
    if (auto reserved = referenced.TryReserve(manifest.entries.Size()); !reserved) {
        return reserved;
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

        (void)referenced.TryPushBack(Move(copy));
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
                Warn(AssetDiagnosticCode::None, {}, FileErrorCode(removed));
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
        Warn(AssetDiagnosticCode::NotDurable, {}, ErrorCode::None);
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
        if (hashed && bytes == entry->packageBytes && digest == entry->packageSha256) {
            out.reuse[at] = 1;
            out.entries[at] = entry;
            ++out.count;
        }
    }

    return Result<void>::Success();
}

} // namespace rocks
