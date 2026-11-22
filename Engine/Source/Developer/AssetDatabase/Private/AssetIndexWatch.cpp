#include "AssetDatabaseInternal.h"

#include <AssetDatabase/AssetIndexWatch.h>
#include <Core/Algorithms.h>
#include <Core/FileError.h>

// 색인 감시 갱신이다(Docs/Design/AssetSystem.ko.md 7.1절 16 — A2d3).
namespace rocks {

namespace {

using asset_database_detail::IsContentRelativeName;
using asset_database_detail::NameSet;

// since부터 now까지 지난 tick이다. 시계가 뒤로 갔으면 since를 now로 당기고 0이다(기다림이 길어질
// 뿐 앞당기지 않는다).
int64 Elapsed(int64 now, int64& since) noexcept {
    if (now < since) {
        since = now;
        return 0;
    }

    return now - since;
}

// path(유효한 UTF-8이 아님)의 첫 잘못된 구성 요소 앞 디렉터리다(첫 구성 요소가 잘못되었으면 빈
// 문자열). '/'는 다중 byte 문자 안에 나오지 않으므로 구성 요소마다 검사할 수 있다.
StringView ValidPrefix(StringView path) noexcept {
    usize start = 0;
    for (usize at = 0; at < path.ByteCount(); ++at) {
        if (path.Data()[at] != '/') {
            continue;
        }

        if (!path.SliceBytes(start, at - start).IsValidUtf8()) {
            break;
        }

        start = at + 1;
    }

    return StringView(path.Data(), start == 0 ? 0 : start - 1);
}

} // namespace

Result<void> PlatformAssetChangeSource::Open(StringView root) noexcept {
    const platform::FileStatus opened = watcher_.Open(root.Data(), root.ByteCount());
    return opened ? Result<void>::Success() : Result<void>::Failure(FileErrorCode(opened));
}

Result<void> PlatformAssetChangeSource::Poll(platform::DirectoryChangeVisitor visitor,
                                             void* context) noexcept {
    const platform::FileStatus polled = watcher_.Poll(visitor, context);
    return polled ? Result<void>::Success() : Result<void>::Failure(FileErrorCode(polled));
}

void PlatformAssetChangeSource::Close() noexcept {
    watcher_.Close();
}

bool PlatformAssetChangeSource::Wait(uint32 timeoutMilliseconds) noexcept {
    return watcher_.Wait(timeoutMilliseconds);
}

AssetIndexWatch::AssetIndexWatch(Allocator& allocator, AssetIndex& index,
                                 platform::FileSystem& files, const PackageReadOptions& options,
                                 AssetChangeSource& source, AssetClock& clock) noexcept
    : allocator_(&allocator), index_(&index), fileSystem_(&files), options_(options),
      source_(&source), clock_(&clock), files_(allocator), subtrees_(allocator),
      waiting_(allocator) {
}

AssetIndexWatch::~AssetIndexWatch() noexcept {
    Stop();
}

Result<void> AssetIndexWatch::Start(const AssetIndexCache* cache) noexcept {
    if (started_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (auto opened = source_->Open(index_->Layout().ContentDirectory()); !opened) {
        return opened;
    }

    if (auto built = index_->Rebuild(*fileSystem_, options_, cache); !built) {
        source_->Close();
        return built;
    }

    ClearWork();
    waiting_.Clear();
    started_ = true;
    sourceOpen_ = true;
    rootLost_ = false;
    return Result<void>::Success();
}

void AssetIndexWatch::Stop() noexcept {
    if (sourceOpen_) {
        source_->Close();
        sourceOpen_ = false;
    }

    started_ = false;
    rootLost_ = false;
    ClearWork();
    waiting_.Clear();
}

void AssetIndexWatch::ClearWork() noexcept {
    files_.Clear();
    subtrees_.Clear();
    fullRescan_ = false;
    rootRemoved_ = false;
}

bool AssetIndexWatch::Visit(const platform::DirectoryChange& change, void* context) noexcept {
    auto& watch = *static_cast<AssetIndexWatch*>(context);
    const StringView path(change.path, change.pathBytes);
    switch (change.kind) {
    case platform::DirectoryChangeKind::RootRemoved:
        watch.rootRemoved_ = true;
        break;
    case platform::DirectoryChangeKind::Overflow:
        if (path.IsEmpty()) {
            watch.fullRescan_ = true;
        } else {
            watch.AddPath(path, platform::DirectoryEntryKind::Directory);
        }

        break;
    case platform::DirectoryChangeKind::Renamed:
        watch.AddPath(StringView(change.oldPath, change.oldPathBytes), change.entry);
        watch.AddPath(path, change.entry);
        break;
    default:
        watch.AddPath(path, change.entry);
        break;
    }

    // 기록하지 못한 것(할당 실패)은 전체 다시 검색으로 접었으므로 이벤트를 늘 소비한다.
    return true;
}

void AssetIndexWatch::AddPath(StringView path, platform::DirectoryEntryKind entry) noexcept {
    if (fullRescan_) {
        return;
    }

    // 유효한 UTF-8이 아닌 이름(POSIX)은 그 앞의 유효한 디렉터리를 다시 검색한다(검색이 그 이름을
    // InvalidPath로 진단한다).
    if (!path.IsValidUtf8()) {
        const StringView prefix = ValidPrefix(path);
        if (prefix.IsEmpty()) {
            fullRescan_ = true;
        } else {
            AddName(subtrees_, prefix);
        }

        return;
    }

    // 규칙에 맞지 않는 경로(원천 계약 위반)는 전체 다시 검색이다.
    if (!IsContentRelativeName(path)) {
        fullRescan_ = true;
        return;
    }

    const bool package = path.ByteCount() > EditorPackageExtension.ByteCount() &&
                         path.EndsWith(EditorPackageExtension);
    if (package && entry != platform::DirectoryEntryKind::Directory) {
        AddName(files_, path);
    }

    if (entry != platform::DirectoryEntryKind::File) {
        AddName(subtrees_, path);
    }
}

void AssetIndexWatch::AddName(Array<String>& to, StringView name) noexcept {
    String copy(*allocator_);
    if (!copy.TryAssign(name) || !to.TryPushBack(Move(copy))) {
        fullRescan_ = true;
    }
}

Result<void> AssetIndexWatch::Pump() noexcept {
    if (!started_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const int64 now = clock_->NowTicks();
    if (rootLost_) {
        return Reopen(now);
    }

    // 루트 소실을 적용하지 못했으면(할당 실패) 원천은 이미 닫혔다 — 다시 적용만 한다.
    if (!rootRemoved_) {
        if (auto polled = source_->Poll(&Visit, this); !polled) {
            // 원천 오류: 잃은 이벤트는 전체 다시 검색으로 덮는다(감시는 열린 채다).
            fullRescan_ = true;
        }
    }

    if (rootRemoved_) {
        return LoseRoot(now);
    }

    return Apply(now);
}

Result<void> AssetIndexWatch::LoseRoot(int64 now) noexcept {
    if (sourceOpen_) {
        source_->Close();
        sourceOpen_ = false;
    }

    if (auto marked = index_->MarkRootLost(); !marked) {
        return marked;
    }

    ClearWork();
    waiting_.Clear();
    rootLost_ = true;
    lastReopen_ = now;
    return Result<void>::Success();
}

Result<void> AssetIndexWatch::Reopen(int64 now) noexcept {
    if (Elapsed(now, lastReopen_) < AssetWatchRootRetryTicks) {
        return Result<void>::Success();
    }

    lastReopen_ = now;
    if (!sourceOpen_) {
        if (auto opened = source_->Open(index_->Layout().ContentDirectory()); !opened) {
            return opened.Error() == ErrorCode::NotFound ? Result<void>::Success()
                                                         : Result<void>::Failure(opened.Error());
        }

        sourceOpen_ = true;
    }

    // 연 뒤 전체를 다시 검색한다(실패하면 색인은 RootLost 그대로이고 다음 간격에 검색만 다시
    // 한다 — 연 뒤의 이벤트는 원천에 쌓여 있다).
    const StringView all[] = {StringView()};
    AssetIndexRefreshPaths paths;
    paths.subtrees = all;
    if (auto refreshed = index_->RefreshPaths(*fileSystem_, paths, options_); !refreshed) {
        return refreshed;
    }

    rootLost_ = false;
    ClearWork();
    return Result<void>::Success();
}

Result<bool> AssetIndexWatch::Observe(Waiting& waiting, int64 now) noexcept {
    if (Elapsed(now, waiting.firstSeen) >= AssetWatchWriteTimeoutTicks) {
        return Result<bool>::Success(true);
    }

    String full(*allocator_);
    const StringView pieces[] = {index_->Layout().ContentDirectory(), "/", waiting.name.View()};
    for (StringView piece : pieces) {
        if (auto appended = full.TryAppend(piece); !appended) {
            return Result<bool>::Failure(appended.Error());
        }
    }

    // Stat 실패(없어짐 등)·일반 파일이 아님은 지금 다시 읽는다(다시 읽기가 결과를 정한다).
    platform::FileInfo info;
    const platform::FileStatus stat =
        fileSystem_->Stat(full.View().Data(), full.View().ByteCount(), info);
    if (!stat || info.isLink || info.type != platform::FileType::File) {
        return Result<bool>::Success(true);
    }

    if (!waiting.observed || info.size != waiting.observedBytes ||
        info.modifiedTicks != waiting.observedTicks) {
        waiting.observed = true;
        waiting.observedBytes = info.size;
        waiting.observedTicks = info.modifiedTicks;
        waiting.observedAt = now;
        return Result<bool>::Success(false);
    }

    return Result<bool>::Success(Elapsed(now, waiting.observedAt) >= AssetWatchStableTicks);
}

Result<void> AssetIndexWatch::Apply(int64 now) noexcept {
    // 대기는 복사본에서 관찰한다(실패하면 대기 그대로 — 성공할 때만 바꾼다). 자리는 새로 미룰 수
    // 있는 이름(이벤트 파일 수)까지 미리 잡는다(뒤의 넣기는 실패하지 않는다).
    Array<Waiting> next(*allocator_);
    if (auto reserved = next.TryReserve(waiting_.Size() + files_.Size()); !reserved) {
        return reserved;
    }

    for (const Waiting& waiting : waiting_) {
        Waiting copy(*allocator_);
        if (auto assigned = copy.name.TryAssign(waiting.name.View()); !assigned) {
            return assigned;
        }

        copy.firstSeen = waiting.firstSeen;
        copy.observedAt = waiting.observedAt;
        copy.observedBytes = waiting.observedBytes;
        copy.observedTicks = waiting.observedTicks;
        copy.observed = waiting.observed;
        (void)next.TryPushBack(Move(copy));
    }

    // 다시 읽을 파일: 이벤트 파일(쓰는 중이면 미룰 수 있다)과 기다림이 끝난 대기 파일(미루지
    // 않는다).
    Array<StringView> names(*allocator_);
    Array<bool> mayDefer(*allocator_);
    const usize capacity = files_.Size() + next.Size();
    if (auto reserved = names.TryReserve(capacity); !reserved) {
        return reserved;
    }

    if (auto reserved = mayDefer.TryReserve(capacity); !reserved) {
        return reserved;
    }

    for (const String& file : files_) {
        (void)names.TryPushBack(file.View());
        (void)mayDefer.TryPushBack(true);
    }

    for (Waiting& waiting : next) {
        auto due = Observe(waiting, now);
        if (!due) {
            return Result<void>::Failure(due.Error());
        }

        if (due.Value()) {
            (void)names.TryPushBack(waiting.name.View());
            (void)mayDefer.TryPushBack(false);
        }
    }

    // 다시 읽을 것이 없으면 관찰만 남긴다(버리면 바뀐 관찰을 다음 Pump가 다시 "바뀜"으로 보아
    // 안정이 끝나지 않는다).
    if (names.IsEmpty() && subtrees_.IsEmpty() && !fullRescan_) {
        waiting_ = Move(next);
        return Result<void>::Success();
    }

    Array<StringView> trees(*allocator_);
    if (fullRescan_) {
        if (auto pushed = trees.TryPushBack(StringView()); !pushed) {
            return pushed;
        }
    } else {
        if (auto reserved = trees.TryReserve(subtrees_.Size()); !reserved) {
            return reserved;
        }

        for (const String& subtree : subtrees_) {
            (void)trees.TryPushBack(subtree.View());
        }
    }

    Array<usize> deferred(*allocator_);
    AssetIndexRefreshPaths paths;
    paths.files = names.View();
    paths.subtrees = trees.View();
    paths.mayDefer = mayDefer.View();
    paths.deferred = &deferred;
    AssetIndexRefresh plan(*allocator_);
    if (auto prepared = index_->PrepareRefreshPaths(*fileSystem_, paths, options_, plan);
        !prepared) {
        return prepared;
    }

    // 새 대기 목록을 확정 전에 모두 만든다(확정 뒤에는 실패가 없게): 다시 읽은 이름과 다시 검색한
    // 하위 트리가 덮는 이름의 대기는 빼고, 미룬 이름은 남기거나(처음 본 시각 유지) 새로
    // 넣는다(지금 관찰). 대조는 모두 정렬 표의 이진 검색이다(이벤트·대기가 수만 개여도 쌍마다
    // 대조하지 않는다).
    Array<bool> isDeferred(*allocator_);
    Array<usize> nameOrder(*allocator_);
    Array<usize> waitingOrder(*allocator_);
    Array<bool> keep(*allocator_);
    if (auto sized = isDeferred.TryResize(names.Size()); !sized) {
        return sized;
    }

    if (auto sized = nameOrder.TryResize(names.Size()); !sized) {
        return sized;
    }

    if (auto sized = waitingOrder.TryResize(next.Size()); !sized) {
        return sized;
    }

    if (auto sized = keep.TryResize(next.Size()); !sized) {
        return sized;
    }

    for (usize at = 0; at < names.Size(); ++at) {
        isDeferred[at] = false;
        nameOrder[at] = at;
    }

    for (usize at : deferred) {
        isDeferred[at] = true;
    }

    for (usize at = 0; at < next.Size(); ++at) {
        waitingOrder[at] = at;
    }

    algo::Sort(nameOrder.View(), [&](usize left, usize right) noexcept {
        const int compared = StringView::CompareBytes(names[left], names[right]);
        return compared != 0 ? compared < 0 : left < right;
    });
    algo::Sort(waitingOrder.View(), [&](usize left, usize right) noexcept {
        return StringView::CompareBytes(next[left].name.View(), next[right].name.View()) < 0;
    });
    NameSet treeSet(*allocator_);
    for (StringView tree : trees) {
        if (auto added = treeSet.Add(tree); !added) {
            return added;
        }
    }

    treeSet.Finish();
    // name의 묶음 첫 위치(nameOrder 안)다.
    auto firstOf = [&](StringView name) noexcept {
        return algo::LowerBound(nameOrder.View(), name, [&](usize index, StringView key) noexcept {
            return StringView::CompareBytes(names[index], key) < 0;
        });
    };
    for (usize at = 0; at < next.Size(); ++at) {
        const StringView name = next[at].name.View();
        bool listed = false;
        bool kept = false;
        for (usize position = firstOf(name);
             position < nameOrder.Size() && names[nameOrder[position]] == name; ++position) {
            listed = true;
            if (isDeferred[nameOrder[position]]) {
                kept = true;
            }
        }

        keep[at] = listed ? kept : !treeSet.Covers(name);
    }

    // 미룬 이름 중 대기에 없는 것(묶음마다 한 번)을 새로 넣는다.
    Array<Waiting> fresh(*allocator_);
    for (usize position = 0; position < nameOrder.Size();) {
        const StringView name = names[nameOrder[position]];
        bool kept = false;
        for (; position < nameOrder.Size() && names[nameOrder[position]] == name; ++position) {
            if (isDeferred[nameOrder[position]]) {
                kept = true;
            }
        }

        const usize known =
            algo::LowerBound(waitingOrder.View(), name, [&](usize index, StringView key) noexcept {
                return StringView::CompareBytes(next[index].name.View(), key) < 0;
            });
        if (!kept ||
            (known < waitingOrder.Size() && next[waitingOrder[known]].name.View() == name)) {
            continue;
        }

        Waiting created(*allocator_);
        if (auto assigned = created.name.TryAssign(name); !assigned) {
            return assigned;
        }

        created.firstSeen = now;
        if (auto observed = Observe(created, now); !observed) {
            return Result<void>::Failure(observed.Error());
        }

        if (auto pushed = fresh.TryPushBack(Move(created)); !pushed) {
            return pushed;
        }
    }

    // 여기부터 실패가 없다: 남길 대기를 앞으로 모으고 새 대기를 붙인다(자리는 위에서 잡았다).
    usize kept = 0;
    for (usize at = 0; at < next.Size(); ++at) {
        if (!keep[at]) {
            continue;
        }

        if (kept != at) {
            next[kept] = Move(next[at]);
        }

        ++kept;
    }

    while (next.Size() > kept) {
        (void)next.TryPopBack();
    }

    for (Waiting& created : fresh) {
        (void)next.TryPushBack(Move(created));
    }

    index_->CommitRefresh(plan);
    waiting_ = Move(next);
    ClearWork();
    return Result<void>::Success();
}

} // namespace rocks
