#include <AssetNugget/NuggetCache.h>
#include <Core/Algorithms.h>
#include <Platform/Atomics.h>
#include <Platform/Fatal.h>

namespace rocks {

namespace {

// 값 행과 ValueId의 순서 비교다(행은 ValueId 오름차순).
struct CacheRowLess {
    bool operator()(const NuggetValueRow& row, uint32 id) const noexcept {
        return row.id < id;
    }

    bool operator()(uint32 id, const NuggetValueRow& row) const noexcept {
        return id < row.id;
    }
};

bool HasRow(const NuggetRecord& record, uint32 id) noexcept {
    return algo::BinarySearch(record.values.View(), id, CacheRowLess{}) != algo::NotFound;
}

Result<void> CopyRecord(const NuggetRecord& source, NuggetRecord& out) noexcept {
    Result<void> step = out.nameSpace.TryAssign(source.nameSpace.View());
    if (step) {
        step = out.bucket.TryAssign(source.bucket.View());
    }

    out.key = source.key;
    out.values.Clear();
    out.inputs.Clear();
    out.meta.Clear();
    if (step) {
        step = out.values.TryAppend(source.values.View());
    }

    if (step) {
        step = out.meta.TryAppend(source.meta.View());
    }

    if (step) {
        step = out.inputs.TryReserve(source.inputs.Size());
    }

    for (usize index = 0; step && index < source.inputs.Size(); ++index) {
        NuggetInput input(out.inputs.GetAllocator());
        step = input.bucket.TryAssign(source.inputs[index].bucket.View());
        input.key = source.inputs[index].key;
        input.digest = source.inputs[index].digest;
        if (step) {
            (void)out.inputs.TryPushBack(Move(input));
        }
    }

    return step;
}

bool IsHitStatus(NuggetStatus status) noexcept {
    return status == NuggetStatus::Hit || status == NuggetStatus::Partial;
}

// 결과를 데이터 없는 Miss로 만든다.
void MakeMiss(NuggetCacheGetResult& out, ErrorCode error, bool failed) noexcept {
    out.status = NuggetStatus::Miss;
    out.error = error;
    out.failed = failed;
    out.values.Clear();
    out.record.values.Clear();
    out.record.inputs.Clear();
    out.record.meta.Clear();
    out.digest = Sha256Digest{};
}

} // namespace

NuggetCache::NuggetCache(Allocator& allocator, platform::FileSystem& files, NuggetClock& clock,
                         const NuggetCacheLimits& limits) noexcept
    : allocator_(&allocator), files_(&files), clock_(&clock), limits_(limits), pins_(allocator),
      index_(allocator), entries_(allocator) {
}

NuggetCache::~NuggetCache() noexcept = default;

Result<void> NuggetCache::Open(const NuggetCacheSettings& settings,
                               StringView projectRoot) noexcept {
    if (opened_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    switch (settings.backend) {
    case NuggetBackendKind::Off:
        break;
    case NuggetBackendKind::Rockpile:
        // 계약이다(Rockpile 서버·클라이언트는 별도 작업 — 9.1절 머리말).
        return Result<void>::Failure(ErrorCode::Unsupported);
    case NuggetBackendKind::FileSystem:
    case NuggetBackendKind::Auto: {
        String root(*allocator_);
        if (auto resolved = ResolveNuggetLocalPath(settings, projectRoot, root); !resolved) {
            return resolved;
        }

        auto made = UniquePtr<NuggetFileSystemBackend>::TryMake(*allocator_, *allocator_, *files_,
                                                                root.View(), *clock_);
        if (!made) {
            return Result<void>::Failure(made.Error());
        }

        if (auto backendOpened = made.Value()->Open(); !backendOpened) {
            if (settings.required || backendOpened.Error() == ErrorCode::OutOfMemory) {
                return backendOpened;
            }

            // required가 아니면 off로 내리고 원인을 남긴다(진단).
            openError_ = backendOpened.Error();
            break;
        }

        backend_ = Move(made.Value());
        break;
    }
    }

    required_ = settings.required;
    generation_ = settings.generation;
    opened_ = true;
    return Result<void>::Success();
}

bool NuggetCache::IsFailure(ErrorCode error) noexcept {
    switch (error) {
    case ErrorCode::None:
    case ErrorCode::Unsupported:
    case ErrorCode::CapacityExceeded:
    case ErrorCode::Busy:
        return false;
    default:
        return true;
    }
}

void NuggetCache::Count(uint64& counter) noexcept {
    (void)platform::AtomicFetchAdd<platform::MemoryOrder::Relaxed>(&counter, 1);
}

usize NuggetCache::EntryBytes(const NuggetRecord& record,
                              Span<const HeldValue> values) const noexcept {
    usize bytes = NuggetMemoryEntryBytes + record.meta.Size() +
                  record.values.Size() * NuggetValueRowBytes +
                  record.inputs.Size() * NuggetMemoryInputBytes;
    for (const HeldValue& value : values) {
        bytes += value.bytes->Size();
    }

    return bytes;
}

void NuggetCache::Link(SlotHandle handle) noexcept {
    MemoryEntry& entry = entries_[handle];
    entry.newer = SlotHandle{};
    entry.older = newest_;
    if (!newest_.IsNull()) {
        entries_[newest_].newer = handle;
    }

    newest_ = handle;
    if (oldest_.IsNull()) {
        oldest_ = handle;
    }
}

void NuggetCache::Unlink(SlotHandle handle) noexcept {
    MemoryEntry& entry = entries_[handle];
    if (entry.newer.IsNull()) {
        newest_ = entry.older;
    } else {
        entries_[entry.newer].older = entry.older;
    }

    if (entry.older.IsNull()) {
        oldest_ = entry.newer;
    } else {
        entries_[entry.older].newer = entry.newer;
    }

    entry.newer = SlotHandle{};
    entry.older = SlotHandle{};
}

void NuggetCache::Evict(SlotHandle handle) noexcept {
    MemoryEntry& entry = entries_[handle];
    // 고정 보관 항목은 세션 동안 지우지 않는다(결정 11 — EvictKey가 건너뛴다).
    ROCKS_ASSERT(!entry.pinned);
    Unlink(handle);
    memoryBytes_ -= entry.bytes;
    (void)index_.Erase(entry.key);
    (void)entries_.TryRemove(handle);
}

void NuggetCache::EvictKey(const Sha256Digest& key) noexcept {
    if (const SlotHandle* found = index_.Find(key); found != nullptr && !entries_[*found].pinned) {
        Evict(*found);
    }
}

void NuggetCache::Trim() noexcept {
    // memoryBytes_는 LRU 목록(고정 보관 아닌 항목)의 합이므로 넘으면 목록이 비어 있지 않다.
    while (memoryBytes_ > limits_.memoryBytes) {
        Evict(oldest_);
    }
}

bool NuggetCache::LookupMemory(const NuggetCacheGet& request, MemoryView& view) noexcept {
    const SlotHandle* found = index_.Find(request.key);
    if (found == nullptr) {
        return false;
    }

    const SlotHandle handle = *found;
    MemoryEntry& entry = entries_[handle];
    if (entry.bucket.View() != request.bucket) {
        // backend가 판정한다(bucket 불일치 Miss InvalidArgument — 지우지 않는다).
        return false;
    }

    // 쥔 값(ValueId 오름차순)에서 id의 위치다(없으면 NotFound).
    auto held = [&](uint32 id) noexcept {
        const usize at = algo::LowerBound(
            entry.values.View(), id,
            [](const HeldValue& value, uint32 probe) noexcept { return value.id < probe; });
        return at < entry.values.Size() && entry.values[at].id == id ? at : algo::NotFound;
    };
    const NuggetRecord& record = *entry.record;
    const bool skipData = HasAnyNuggetPolicy(request.policy, NuggetPolicy::SkipData);
    bool missing = false;
    if (!skipData) {
        if (request.values.IsEmpty()) {
            for (const NuggetValueRow& row : record.values) {
                if (held(row.id) == algo::NotFound) {
                    return false;
                }
            }
        } else {
            for (usize at = 0; at < request.values.Size(); ++at) {
                const uint32 id = request.values[at];
                for (usize earlier = 0; earlier < at; ++earlier) {
                    if (request.values[earlier] == id) {
                        return false;
                    }
                }

                if (!HasRow(record, id)) {
                    missing = true;
                } else if (held(id) == algo::NotFound) {
                    return false;
                }
            }
        }
    }

    view.found = true;
    if (missing && !HasAnyNuggetPolicy(request.policy, NuggetPolicy::PartialRecord)) {
        // Record에 없는 값이다 — 확정 미적중(backend의 같은 Record도 없다).
        view.status = NuggetStatus::Miss;
        return true;
    }

    // 참조만 얻는다(복사는 잠금 밖에서 — 값 하나가 64 MiB일 수 있다).
    if (!skipData) {
        const usize count = request.values.IsEmpty() ? record.values.Size() : request.values.Size();
        if (auto reserved = view.values.TryReserve(count); !reserved) {
            view.error = reserved.Error();
            return true;
        }

        for (usize at = 0; at < count; ++at) {
            const uint32 id = request.values.IsEmpty() ? record.values[at].id : request.values[at];
            const usize index = held(id);
            if (index != algo::NotFound) {
                (void)view.values.TryPushBack(entry.values[index]);
            }
        }
    }

    view.record = entry.record;
    view.status = missing ? NuggetStatus::Partial : NuggetStatus::Hit;
    view.digest = entry.digest;
    if (!entry.pinned) {
        Unlink(handle);
        Link(handle);
    }

    return true;
}

NuggetCache::Verdict NuggetCache::Judge(const Sha256Digest& key, const Sha256Digest& digest,
                                        Span<const NuggetInput> inputs) noexcept {
    Pin* pin = pins_.Find(key);
    if (pin != nullptr && pin->state == PinState::Uncached) {
        return Verdict::Uncached;
    }

    if (pin != nullptr && !(pin->digest == digest)) {
        // 세션 고정과 다르다 — 이 Record의 값을 버리고 다시 얻거나 다시 만든다(결정 11).
        if (pin->state == PinState::Normal) {
            pin->state = PinState::Changed;
        }

        EvictKey(key);
        Count(stats_.digestChanged);
        return Verdict::DigestChanged;
    }

    for (const NuggetInput& input : inputs) {
        const Pin* consumed = pins_.Find(input.key);
        if (consumed != nullptr && !(consumed->digest == input.digest)) {
            // 이 세션이 쓴 의존 결과와 다른 것을 소비해 만든 Record다.
            Count(stats_.inputMismatches);
            return Verdict::InputMismatch;
        }
    }

    // 받아들인다: 고정 없는 입력과 이 키를 고정한다.
    for (const NuggetInput& input : inputs) {
        if (!pins_.FindOrAdd(input.key, Pin{input.digest, PinState::Normal})) {
            return Verdict::OutOfMemory;
        }
    }

    if (pin == nullptr && !pins_.FindOrAdd(key, Pin{digest, PinState::Normal})) {
        return Verdict::OutOfMemory;
    }

    return Verdict::Accept;
}

void NuggetCache::Insert(MemoryEntry&& candidate) noexcept {
    auto byId = [](const HeldValue& value, uint32 id) noexcept { return value.id < id; };
    if (const SlotHandle* found = index_.Find(candidate.key); found != nullptr) {
        const SlotHandle handle = *found;
        MemoryEntry& entry = entries_[handle];
        // Memory 항목은 늘 고정 digest이고 넣는 쪽도 고정과 같을 때만 넣는다(같은 Record). 고정
        // 보관 항목은 값을 다 쥐어 Memory가 늘 답하고 Put은 따로 걸러 여기에 오지 않는다.
        ROCKS_ASSERT(entry.digest == candidate.digest && !entry.pinned);
        // 같은 Record다 — 없는 값을 더한다(공유 버퍼라 복사하지 않는다).
        for (HeldValue& value : candidate.values) {
            const usize at = algo::LowerBound(entry.values.View(), value.id, byId);
            if (at < entry.values.Size() && entry.values[at].id == value.id) {
                continue;
            }

            const usize bytes = value.bytes->Size();
            if (!entry.values.TryInsert(Move(value), at)) {
                break;
            }

            entry.bytes += bytes;
            memoryBytes_ += bytes;
        }

        Unlink(handle);
        Link(handle);
        Trim();
        return;
    }

    candidate.bytes = EntryBytes(*candidate.record, candidate.values.View());
    if (candidate.bytes > limits_.memoryBytes) {
        return;
    }

    const Sha256Digest key = candidate.key;
    auto added = entries_.TryAdd(Move(candidate));
    if (!added) {
        return;
    }

    if (!index_.InsertOrAssign(key, added.Value())) {
        (void)entries_.TryRemove(added.Value());
        return;
    }

    memoryBytes_ += entries_[added.Value()].bytes;
    Link(added.Value());
    Trim();
}

NuggetCacheGetResult NuggetCache::Get(const NuggetCacheGet& request) noexcept {
    NuggetCacheGetResult result(*allocator_);
    if (!opened_) {
        result.error = ErrorCode::InvalidArgument;
        return result;
    }

    if (!IsEnabled() || !HasAnyNuggetPolicy(request.policy, NuggetPolicy::QueryLocal)) {
        Count(stats_.misses);
        return result;
    }

    const bool skipData = HasAnyNuggetPolicy(request.policy, NuggetPolicy::SkipData);
    // 판정을 결과에 적용한다.
    auto apply = [&](Verdict verdict) noexcept {
        switch (verdict) {
        case Verdict::Accept:
            break;
        case Verdict::DigestChanged:
            result.status = NuggetStatus::DigestChanged;
            result.values.Clear();
            break;
        case Verdict::InputMismatch:
        case Verdict::Uncached:
            MakeMiss(result, ErrorCode::None, false);
            break;
        case Verdict::OutOfMemory:
            MakeMiss(result, ErrorCode::OutOfMemory, true);
            break;
        }
    };
    MemoryView view(*allocator_);
    Verdict verdict = Verdict::Accept;
    {
        platform::LockGuard lock(mutex_);
        const Pin* pin = pins_.Find(request.key);
        if (pin != nullptr && pin->state == PinState::Uncached) {
            Count(stats_.misses);
            return result;
        }

        if (LookupMemory(request, view) && view.error == ErrorCode::None &&
            IsHitStatus(view.status)) {
            verdict = Judge(request.key, view.digest, view.record->inputs.View());
        }
    }

    if (view.found) {
        result.source = NuggetSource::Memory;
        if (view.error != ErrorCode::None) {
            MakeMiss(result, view.error, true);
        } else if (IsHitStatus(view.status)) {
            // 잠금 밖에서 복사한다.
            result.status = view.status;
            result.digest = view.digest;
            Result<void> copied = CopyRecord(*view.record, result.record);
            if (copied) {
                copied = result.values.TryReserve(view.values.Size());
            }

            for (usize index = 0; copied && index < view.values.Size(); ++index) {
                NuggetValue value(*allocator_);
                value.id = view.values[index].id;
                copied = value.bytes.TryAppend(view.values[index].bytes->View());
                if (copied) {
                    (void)result.values.TryPushBack(Move(value));
                }
            }

            if (!copied) {
                MakeMiss(result, copied.Error(), true);
            } else {
                apply(verdict);
            }

            if (IsHitStatus(result.status) &&
                HasAnyNuggetPolicy(request.policy, NuggetPolicy::KeepAlive)) {
                (void)backend_->Touch(request.key);
            }
        }
    } else {
        NuggetBackendGet get;
        get.bucket = request.bucket;
        get.key = request.key;
        get.values = request.values;
        get.partial = HasAnyNuggetPolicy(request.policy, NuggetPolicy::PartialRecord);
        get.skipData = skipData;
        // Memory 항목은 meta까지 쥐므로 backend에는 늘 meta를 청한다(SkipMeta는 아래에서 비운다).
        get.skipMeta = false;
        NuggetGetResult got = backend_->Get(get);
        result.status = got.status;
        result.error = got.error;
        result.digest = got.digest;
        result.record = Move(got.record);
        result.values = Move(got.values);
        result.source = IsHitStatus(got.status) ? NuggetSource::Local : NuggetSource::None;
        result.failed = got.status == NuggetStatus::Miss && IsFailure(got.error);
        if (IsHitStatus(result.status)) {
            // Memory 후보를 잠금 밖에서 만든다(복사). 실패하면 넣지 않는다(최선 노력).
            MemoryEntry candidate(*allocator_);
            bool prepared = false;
            if (!skipData) {
                candidate.key = request.key;
                candidate.digest = result.digest;
                auto record = SharedPtr<NuggetRecord>::TryMake(*allocator_, *allocator_);
                prepared = record && candidate.bucket.TryAssign(request.bucket) &&
                           CopyRecord(result.record, *record.Value()) &&
                           candidate.values.TryReserve(result.values.Size());
                if (prepared) {
                    candidate.record = Move(record.Value());
                }

                for (usize index = 0; prepared && index < result.values.Size(); ++index) {
                    const NuggetValue& value = result.values[index];
                    if (value.bytes.Size() > limits_.memoryValueBytes) {
                        continue;
                    }

                    auto bytes = SharedPtr<Array<uint8>>::TryMake(*allocator_, *allocator_);
                    prepared = bytes && bytes.Value()->TryAppend(value.bytes.View());
                    if (prepared) {
                        (void)candidate.values.TryPushBack(
                            HeldValue{value.id, Move(bytes.Value())});
                    }
                }

                algo::Sort(candidate.values.View(),
                           [](const HeldValue& left, const HeldValue& right) noexcept {
                               return left.id < right.id;
                           });
            }

            platform::LockGuard lock(mutex_);
            verdict = Judge(request.key, result.digest, result.record.inputs.View());
            if (verdict == Verdict::Accept && prepared) {
                Insert(Move(candidate));
            }

            apply(verdict);
        }
    }

    if (HasAnyNuggetPolicy(request.policy, NuggetPolicy::SkipMeta)) {
        result.record.meta.Clear();
    }

    switch (result.status) {
    case NuggetStatus::Hit:
        Count(view.found ? stats_.memoryHits : stats_.localHits);
        break;
    case NuggetStatus::Partial:
        Count(stats_.partials);
        break;
    case NuggetStatus::Corrupt:
        Count(stats_.corrupts);
        break;
    case NuggetStatus::DigestChanged:
        break;
    default:
        Count(stats_.misses);
        break;
    }

    return result;
}

void NuggetCache::Settle(const NuggetCachePut& request, const Sha256Digest& digest, bool planned,
                         bool regenerated, MemoryEntry&& prepared, bool preparedAll) noexcept {
    // 계획한 Record가 없으면(정규 형식 위반 등) Memory·고정에 남길 것이 없다.
    if (!planned) {
        return;
    }

    Pin* pin = pins_.Find(request.key);
    // 고정은 세션 동안 지우지 않는다 — regenerated(Put 앞에서 Changed를 봄)면 고정이 있다.
    ROCKS_ASSERT(!regenerated || pin != nullptr);
    if (regenerated && pin->state == PinState::Changed) {
        // DigestChanged 뒤 다시 만든 Record다 — 값 전부를 고정 보관한다(결정 11).
        auto uncache = [&]() noexcept {
            pin->state = PinState::Uncached;
            Count(stats_.uncachedKeys);
        };
        if (!preparedAll) {
            uncache();
            return;
        }

        EvictKey(request.key);
        const usize bytes = EntryBytes(*prepared.record, prepared.values.View());
        if (bytes > limits_.pinnedBytes || pinnedBytes_ > limits_.pinnedBytes - bytes) {
            uncache();
            return;
        }

        prepared.pinned = true;
        prepared.bytes = bytes;
        auto added = entries_.TryAdd(Move(prepared));
        if (!added) {
            uncache();
            return;
        }

        if (!index_.InsertOrAssign(request.key, added.Value())) {
            (void)entries_.TryRemove(added.Value());
            uncache();
            return;
        }

        pinnedBytes_ += bytes;
        pin->digest = digest;
        pin->state = PinState::Regenerated;
        return;
    }

    if (pin != nullptr && (!(pin->digest == digest) || pin->state == PinState::Changed)) {
        // 세션 고정과 다른 Record다(비결정적 처리기 — 또는 그 사이 DigestChanged) — 남기지 않는다.
        return;
    }

    if (pin == nullptr && !pins_.FindOrAdd(request.key, Pin{digest, PinState::Normal})) {
        return;
    }

    // Memory(축출 가능)에 넣는다 — 이 세션이 쓴 값이다. 고정 보관 항목이 있으면(같은 다시 만든
    // Record의 Put) 그대로 둔다.
    if (const SlotHandle* found = index_.Find(request.key);
        !preparedAll || (found != nullptr && entries_[*found].pinned)) {
        return;
    }

    // 64 MiB를 넘는 값은 쥐지 않는다.
    usize kept = 0;
    for (usize index = 0; index < prepared.values.Size(); ++index) {
        if (prepared.values[index].bytes->Size() <= limits_.memoryValueBytes) {
            if (kept != index) {
                prepared.values[kept] = Move(prepared.values[index]);
            }

            ++kept;
        }
    }

    (void)prepared.values.TryResize(kept);

    Insert(Move(prepared));
}

NuggetCachePutResult NuggetCache::Put(const NuggetCachePut& request) noexcept {
    NuggetCachePutResult result;
    auto drop = [&](ErrorCode error, bool failed) noexcept {
        result.status = NuggetStatus::Dropped;
        result.error = error;
        result.failed = failed;
        Count(stats_.dropped);
        return result;
    };
    if (!opened_) {
        // 열기 전 호출은 계약 위반이다(통계에 세지 않는다 — Get과 같다).
        result.error = ErrorCode::InvalidArgument;
        return result;
    }

    if (!IsEnabled() || !HasAnyNuggetPolicy(request.policy, NuggetPolicy::Store)) {
        return drop(ErrorCode::None, false);
    }

    bool regenerated = false;
    {
        platform::LockGuard lock(mutex_);
        const Pin* pin = pins_.Find(request.key);
        if (pin != nullptr && pin->state == PinState::Uncached) {
            return drop(ErrorCode::None, false);
        }

        regenerated = pin != nullptr && pin->state == PinState::Changed;
    }

    Array<NuggetPutValue> values(*allocator_);
    if (auto reserved = values.TryReserve(request.values.Size()); !reserved) {
        return drop(reserved.Error(), true);
    }

    for (const NuggetValue& value : request.values) {
        (void)values.TryPushBack(NuggetPutValue{value.id, value.bytes.View()});
    }

    NuggetBackendPut put;
    put.bucket = request.bucket;
    put.key = request.key;
    put.values = values.View();
    put.inputs = request.inputs;
    put.meta = request.meta;
    NuggetPutResult stored = backend_->Put(put);
    result.status = stored.status;
    result.error = stored.error;
    result.digest = stored.digest;
    result.existing = stored.existing;
    result.failed = stored.status == NuggetStatus::Dropped && IsFailure(stored.error);

    // Memory 후보를 잠금 밖에서 만든다(값 버퍼는 옮긴다 — 소유권 이전, Record도 옮긴다).
    const bool planned = !stored.record.bucket.IsEmpty();
    MemoryEntry prepared(*allocator_);
    bool preparedAll = false;
    if (planned) {
        prepared.key = request.key;
        prepared.digest = stored.digest;
        auto record = SharedPtr<NuggetRecord>::TryMake(*allocator_, Move(stored.record));
        preparedAll = record && prepared.bucket.TryAssign(request.bucket) &&
                      prepared.values.TryReserve(request.values.Size());
        if (preparedAll) {
            prepared.record = Move(record.Value());
        }

        for (usize index = 0; preparedAll && index < request.values.Size(); ++index) {
            NuggetValue& value = request.values[index];
            auto bytes = SharedPtr<Array<uint8>>::TryMake(*allocator_, Move(value.bytes));
            preparedAll = static_cast<bool>(bytes);
            if (preparedAll) {
                (void)prepared.values.TryPushBack(HeldValue{value.id, Move(bytes.Value())});
            }
        }
    }

    {
        platform::LockGuard lock(mutex_);
        Settle(request, stored.digest, planned, regenerated, Move(prepared), preparedAll);
    }

    switch (result.status) {
    case NuggetStatus::Stored:
        Count(stats_.stored);
        break;
    case NuggetStatus::Exists:
        Count(stats_.exists);
        break;
    case NuggetStatus::Conflict:
        Count(stats_.conflicts);
        break;
    default:
        Count(stats_.dropped);
        break;
    }

    return result;
}

NuggetCachePutResult NuggetCache::PutScc(Span<const AssetId> members,
                                         const NuggetCachePut& request) noexcept {
    NuggetCachePutResult result;
    auto drop = [&](ErrorCode error, bool failed) noexcept {
        result.status = NuggetStatus::Dropped;
        result.error = error;
        result.failed = failed;
        Count(stats_.dropped);
        return result;
    };
    Array<uint8> meta(*allocator_);
    if (auto written = WriteNuggetSccMeta(members, meta); !written) {
        return drop(written.Error(), written.Error() == ErrorCode::OutOfMemory);
    }

    for (const NuggetValue& value : request.values) {
        if (NuggetSccMemberOf(value.id) >= members.Size()) {
            return drop(ErrorCode::InvalidArgument, false);
        }
    }

    NuggetCachePut put = request;
    put.meta = meta.View();
    return Put(put);
}

NuggetCacheGetResult NuggetCache::GetSccMember(StringView bucket, const Sha256Digest& sccKey,
                                               AssetId asset, NuggetPolicy policy) noexcept {
    // meta로 순번을 찾는다(값 없이).
    const NuggetPolicy first = (policy & ~(NuggetPolicy::SkipData | NuggetPolicy::SkipMeta |
                                           NuggetPolicy::PartialRecord)) |
                               NuggetPolicy::SkipData;
    NuggetCacheGetResult record = Get(NuggetCacheGet{bucket, sccKey, {}, first});
    if (record.status != NuggetStatus::Hit) {
        return record;
    }

    auto member = FindNuggetSccMember(record.record.meta.View(), asset);
    if (!member) {
        NuggetCacheGetResult missed(*allocator_);
        missed.error = member.Error();
        return missed;
    }

    Array<uint32> ids(*allocator_);
    for (const NuggetValueRow& row : record.record.values) {
        if (NuggetSccMemberOf(row.id) == member.Value()) {
            if (auto pushed = ids.TryPushBack(row.id); !pushed) {
                NuggetCacheGetResult failed(*allocator_);
                failed.error = pushed.Error();
                failed.failed = true;
                return failed;
            }
        }
    }

    if (ids.IsEmpty()) {
        // 그 구성원의 값이 없다 — 빈 적중(빈 목록 Get은 "전체"이므로 다시 묻지 않는다).
        if (HasAnyNuggetPolicy(policy, NuggetPolicy::SkipMeta)) {
            record.record.meta.Clear();
        }

        return record;
    }

    return Get(NuggetCacheGet{bucket, sccKey, ids.View(), policy});
}

NuggetCacheStats NuggetCache::Stats() const noexcept {
    NuggetCacheStats out;
    auto load = [](const uint64& counter) noexcept {
        return platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&counter);
    };
    out.memoryHits = load(stats_.memoryHits);
    out.localHits = load(stats_.localHits);
    out.partials = load(stats_.partials);
    out.misses = load(stats_.misses);
    out.digestChanged = load(stats_.digestChanged);
    out.inputMismatches = load(stats_.inputMismatches);
    out.corrupts = load(stats_.corrupts);
    out.stored = load(stats_.stored);
    out.exists = load(stats_.exists);
    out.conflicts = load(stats_.conflicts);
    out.dropped = load(stats_.dropped);
    out.uncachedKeys = load(stats_.uncachedKeys);
    platform::LockGuard lock(mutex_);
    out.memoryBytes = memoryBytes_;
    out.pinnedBytes = pinnedBytes_;
    out.pinnedKeys = pins_.Count();
    return out;
}

} // namespace rocks
