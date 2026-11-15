#include <Core/Text/NamePool.h>
#include <Base/Checked.h>
#include <Platform/Memory.h>

namespace rocks {
namespace {

struct DomainSource {
    platform::Mutex gate;
    uint64 next = 1;
};

Result<uint64> TakeDomain() noexcept {
    // 첫 사용 시 구성하여 다른 translation unit의 전역 초기화 순서에 의존하지 않는다.
    // 발급량은 process 수명 동안만 유효하다. 마지막 값 뒤의 0은 영구 소진 sentinel이다.
    static DomainSource source;
    platform::LockGuard lock(source.gate);
    if (source.next == 0) {
        return Result<uint64>::Failure(ErrorCode::Overflow);
    }

    return Result<uint64>::Success(source.next++);
}

uint8 FoldAscii(uint8 value) noexcept {
    return value >= 'A' && value <= 'Z' ? static_cast<uint8>(value + ('a' - 'A')) : value;
}

struct ParsedName {
    usize baseBytes;
    // 번호가 없으면 0이고, 번호가 있으면 외부 번호 + 1이다.
    uint32 number;
};

ParsedName ParseName(StringView source) noexcept {
    const usize size = source.ByteCount();
    usize firstDigit = size;
    while (firstDigit != 0) {
        const char value = source.Data()[firstDigit - 1];
        if (value < '0' || value > '9') {
            break;
        }
        --firstDigit;
    }

    const usize digits = size - firstDigit;
    if (digits == 0 || digits > 10 || firstDigit == 0 || source.Data()[firstDigit - 1] != '_' ||
        (digits > 1 && source.Data()[firstDigit] == '0')) {
        return {size, 0};
    }

    uint64 value = 0;
    for (usize index = firstDigit; index < size; ++index) {
        value = value * 10 + static_cast<uint64>(source.Data()[index] - '0');
    }
    // UE5.8은 MAX_int32 미만의 외부 번호만 접미사로 해석한다(NameTypes.h ParseNumberFromName).
    if (value >= static_cast<uint64>(NumericLimits<int32>::Max())) {
        return {size, 0};
    }
    return {firstDigit - 1, static_cast<uint32>(value + 1)};
}

bool IsNoneBase(StringView source, usize baseBytes) noexcept {
    return baseBytes == 4 && FoldAscii(static_cast<uint8>(source.Data()[0])) == 'n' &&
           FoldAscii(static_cast<uint8>(source.Data()[1])) == 'o' &&
           FoldAscii(static_cast<uint8>(source.Data()[2])) == 'n' &&
           FoldAscii(static_cast<uint8>(source.Data()[3])) == 'e';
}

uint64 HashBytes(StringView value, usize baseBytes) noexcept {
    uint64 hash = 14695981039346656037ull;
    for (usize index = 0; index < baseBytes; ++index) {
        hash ^= FoldAscii(static_cast<uint8>(value.Data()[index]));
        hash *= 1099511628211ull;
    }

    return hash;
}

// NameKey 영속 알고리즘 v1이다. pool 내부 hash(HashBytes)와 분리해 두며 값을 바꾸지 않는다.
uint64 NameKeyHashV1(StringView value, usize baseBytes) noexcept {
    uint64 hash = 14695981039346656037ull;
    for (usize index = 0; index < baseBytes; ++index) {
        hash ^= FoldAscii(static_cast<uint8>(value.Data()[index]));
        hash *= 1099511628211ull;
    }
    hash = (hash ^ (hash >> 30)) * 0xbf58476d1ce4e5b9ull;
    hash = (hash ^ (hash >> 27)) * 0x94d049bb133111ebull;
    hash ^= hash >> 31;
    return hash == 0 ? 1 : hash;
}

NameKey KeyFor(StringView source) noexcept {
    const ParsedName parsed = ParseName(source);
    if (parsed.baseBytes == 0) {
        return {};
    }
    if (IsNoneBase(source, parsed.baseBytes)) {
        return NameKey{0, parsed.number};
    }
    return NameKey{NameKeyHashV1(source, parsed.baseBytes), parsed.number};
}

bool EqualBase(StringView left, StringView right, usize baseBytes) noexcept {
    for (usize index = 0; index < baseBytes; ++index) {
        if (FoldAscii(static_cast<uint8>(left.Data()[index])) !=
            FoldAscii(static_cast<uint8>(right.Data()[index]))) {
            return false;
        }
    }
    return true;
}

} // namespace

struct NamePool::Entry {
    Entry* next = nullptr;
    uint64 hash = 0;
    uint64 id = 0;
    uint64 comparisonId = 0;
    uint32 number = 0;
    usize baseBytes = 0;
    usize bytes = 0;

    char* Data() noexcept {
        return reinterpret_cast<char*>(this + 1);
    }

    StringView View() noexcept {
        return StringView(Data(), bytes);
    }
};

NameLease::NameLease(NamePool& pool, Name name, StringView view) noexcept
    : pool_(&pool), name_(name), view_(view) {
}

NameLease::~NameLease() noexcept {
    Reset();
}

NameLease::NameLease(NameLease&& other) noexcept
    : pool_(other.pool_), name_(other.name_), view_(other.view_) {
    other.pool_ = nullptr;
    other.name_ = {};
    other.view_ = {};
}

NameLease& NameLease::operator=(NameLease&& other) noexcept {
    if (this != &other) {
        Reset();
        pool_ = other.pool_;
        name_ = other.name_;
        view_ = other.view_;
        other.pool_ = nullptr;
        other.name_ = {};
        other.view_ = {};
    }

    return *this;
}

NameLease::operator bool() const noexcept {
    return pool_ != nullptr;
}

Name NameLease::Identifier() const noexcept {
    return name_;
}

StringView NameLease::View() const noexcept {
    return view_;
}

StringView NameLease::BaseView() const noexcept {
    if (!name_.HasNumber()) {
        return view_;
    }

    // Acquire가 표시 entry와 번호를 함께 검증한다. lease 수명은 원문 저장소를 보호한다.
    return StringView(view_.Data(), ParseName(view_).baseBytes);
}

NameKey NameLease::Key() const noexcept {
    return pool_ == nullptr ? NameKey{} : KeyFor(view_);
}

Result<NameLease> NameLease::TryClone() const noexcept {
    if (pool_ == nullptr) {
        return Result<NameLease>::Failure(ErrorCode::StaleHandle);
    }

    return pool_->Acquire(name_);
}

void NameLease::Reset() noexcept {
    NamePool* pool = pool_;
    pool_ = nullptr;
    name_ = {};
    view_ = {};
    if (pool != nullptr) {
        pool->ReleaseLease();
    }
}

NamePool::NamePool(Allocator& allocator) noexcept : allocator_(&allocator) {
}

NamePool::~NamePool() noexcept {
    if (!Shutdown()) {
        platform::Fatal("NamePool destroyed with active operations or leases");
    }
}

Result<void> NamePool::Initialize(NamePoolConfig config) noexcept {
    {
        platform::LockGuard lock(gate_);
        if (state_ == State::Running) {
            return Result<void>::Failure(ErrorCode::AlreadyExists);
        }

        if (state_ != State::Stopped) {
            return Result<void>::Failure(ErrorCode::Busy);
        }

        state_ = State::Initializing;
        ++stats_.activeOperations;
    }

    // 전역 domain gate와 pool gate를 동시에 보유하지 않는다.
    auto domain = TakeDomain();
    platform::LockGuard lock(gate_);
    --stats_.activeOperations;
    if (state_ != State::Initializing) {
        // domain 발급 중 Shutdown이 admission을 닫았다면 다시 열지 않는다.
        return Result<void>::Failure(ErrorCode::ShuttingDown);
    }

    if (!domain) {
        state_ = State::Stopped;
        return Result<void>::Failure(domain.Error());
    }

    config_ = config;
    stats_ = {};
    stats_.domain = domain.Value();
    stats_.accepting = true;
    nextSerial_ = 1;
    state_ = State::Running;
    return Result<void>::Success();
}

Result<Name> NamePool::TryIntern(StringView source) noexcept {
    constexpr uint64 maximum = NumericLimits<uint64>::Max();
    constexpr uint64 maximumSerial = maximum >> BucketBits;
    {
        platform::LockGuard lock(gate_);
        if (state_ != State::Running) {
            return Result<Name>::Failure(ErrorCode::ShuttingDown);
        }

        if (stats_.activeOperations == maximum) {
            return Result<Name>::Failure(ErrorCode::Overflow);
        }

        ++stats_.activeOperations;
    }

    usize allocationBytes = 0;
    if (!TryAdd(sizeof(Entry), source.ByteCount(), allocationBytes) ||
        !TryAdd(allocationBytes, 1, allocationBytes)) {
        FinishOperation(0);
        return Result<Name>::Failure(ErrorCode::Overflow);
    }

    auto valid = source.ValidateUtf8();
    if (!valid) {
        FinishOperation(0);
        return Result<Name>::Failure(valid.Error());
    }

    const ParsedName parsed = ParseName(source);
    const bool noneBase = IsNoneBase(source, parsed.baseBytes);
    const bool canonicalNone = source.IsEmpty() || (parsed.number != 0 && parsed.baseBytes == 0) ||
                               (source == StringView("None"));
    const uint64 hash = HashBytes(source, parsed.baseBytes) & config_.hashMask;
    {
        platform::LockGuard lock(gate_);
        if (state_ != State::Running) {
            --stats_.activeOperations;
            return Result<Name>::Failure(ErrorCode::ShuttingDown);
        }

        // 기본 None은 별도 entry가 필요 없고 닫힌 pool에서 새 이름을 발급하지 않는다.
        if (canonicalNone) {
            --stats_.activeOperations;
            return Result<Name>::Success(Name{});
        }

        if (Entry* found = FindBytes(source, hash)) {
            --stats_.activeOperations;
            return Result<Name>::Success(TokenFor(*found));
        }

        if (nextSerial_ > maximumSerial) {
            --stats_.activeOperations;
            return Result<Name>::Failure(ErrorCode::Overflow);
        }

        const uint64 remainingEntries = config_.maxEntries - stats_.entryCount;
        const usize remainingBytes = config_.maxStorageBytes - stats_.storageBytes;
        if (stats_.pendingEntries >= remainingEntries ||
            allocationBytes > remainingBytes - stats_.pendingStorageBytes) {
            --stats_.activeOperations;
            return Result<Name>::Failure(ErrorCode::CapacityExceeded);
        }

        ++stats_.pendingEntries;
        stats_.pendingStorageBytes += allocationBytes;
    }

    auto allocation = allocator_->Allocate(allocationBytes, alignof(Entry));
    if (!allocation) {
        FinishOperation(allocationBytes);
        return Result<Name>::Failure(allocation.Error());
    }

    if (allocation.Value() == nullptr) {
        platform::Fatal("Allocator returned null for a nonzero name allocation");
    }

    Entry* candidate = ConstructAt<Entry>(allocation.Value());
    candidate->hash = hash;
    candidate->number = parsed.number;
    candidate->baseBytes = parsed.baseBytes;
    candidate->bytes = source.ByteCount();
    if (!source.IsEmpty()) {
        platform::MemoryCopy(candidate->Data(), source.Data(), source.ByteCount());
    }

    candidate->Data()[candidate->bytes] = '\0';
    Name result{};
    ErrorCode error = ErrorCode::None;
    {
        platform::LockGuard lock(gate_);
        if (state_ != State::Running) {
            error = ErrorCode::ShuttingDown;
        } else if (Entry* found = FindBytes(source, hash)) {
            result = TokenFor(*found);
        } else if (nextSerial_ > maximumSerial) {
            error = ErrorCode::Overflow;
        } else {
            const usize bucket = static_cast<usize>(hash & (BucketCount - 1));
            candidate->id = (nextSerial_++ << BucketBits) | bucket;
            Entry* comparison = FindComparison(source, parsed.baseBytes, hash);
            candidate->comparisonId =
                noneBase ? 0 : (comparison != nullptr ? comparison->comparisonId : candidate->id);
            candidate->next = buckets_[bucket];
            buckets_[bucket] = candidate;
            ++stats_.entryCount;
            stats_.storageBytes += allocationBytes;
            --stats_.pendingEntries;
            stats_.pendingStorageBytes -= allocationBytes;
            --stats_.activeOperations;
            return Result<Name>::Success(TokenFor(*candidate));
        }
    }

    // 중복 패자/종료 취소 후보를 gate 밖에서 해제한다. allocator callback이 진행되는 동안도
    // operation과 예산 예약을 유지하여 Shutdown 성공이 미완료 cleanup을 앞지르지 못하게 한다.
    FreeEntry(candidate);
    FinishOperation(allocationBytes);
    if (error != ErrorCode::None) {
        return Result<Name>::Failure(error);
    }

    return Result<Name>::Success(result);
}

Result<Name> NamePool::TryFind(StringView source) const noexcept {
    // 검사·분해·hash는 pool 상태를 읽지 않으므로 gate 밖에서 한다. 오류 순서는 TryIntern과 같게
    // 실행 상태를 먼저 본다.
    const auto valid = source.ValidateUtf8();
    const ParsedName parsed = ParseName(source);
    const bool noneBase = IsNoneBase(source, parsed.baseBytes);
    const uint64 rawHash = HashBytes(source, parsed.baseBytes);
    platform::LockGuard lock(gate_);
    if (state_ != State::Running) {
        return Result<Name>::Failure(ErrorCode::ShuttingDown);
    }

    if (!valid) {
        return Result<Name>::Failure(valid.Error());
    }

    // 빈 base(빈 문자열과 "_7")는 기본 None이다. 정확히 "None"인 원문은 TryIntern이 entry를 만들지
    // 않으므로 아래 None 변형 분기에서 기본 None이 된다.
    if (parsed.baseBytes == 0) {
        return Result<Name>::Success(Name{});
    }

    bool exact = false;
    Entry* display =
        FindDisplay(source, parsed.baseBytes, parsed.number, rawHash & config_.hashMask, exact);
    // 번호 없는 None의 첫 표시는 정적 "None"이다(기본 None은 entry 없이 언제나 있다).
    if (noneBase && parsed.number == 0 && !exact) {
        return Result<Name>::Success(Name{});
    }

    if (display != nullptr) {
        return Result<Name>::Success(TokenFor(*display));
    }

    return Result<Name>::Failure(ErrorCode::NotFound);
}

// gate를 쥔 상태에서 부른다(domain을 읽는다).
Name NamePool::TokenFor(const Entry& entry) const noexcept {
    return Name{stats_.domain, entry.comparisonId, entry.id, entry.number,
                entry.comparisonId == 0 ? NameKind::NoneDisplay : NameKind::Normal};
}

NamePool::Entry* NamePool::FindBytes(StringView source, uint64 hash) const noexcept {
    const usize bucket = static_cast<usize>(hash & (BucketCount - 1));
    for (Entry* entry = buckets_[bucket]; entry != nullptr; entry = entry->next) {
        if (entry->hash == hash && entry->View() == source) {
            return entry;
        }
    }

    return nullptr;
}

NamePool::Entry* NamePool::FindComparison(StringView source, usize baseBytes,
                                          uint64 hash) const noexcept {
    const usize bucket = static_cast<usize>(hash & (BucketCount - 1));
    for (Entry* entry = buckets_[bucket]; entry != nullptr; entry = entry->next) {
        if (entry->hash == hash && entry->baseBytes == baseBytes &&
            EqualBase(entry->View(), source, baseBytes)) {
            return entry;
        }
    }
    return nullptr;
}

// TryFind의 bucket 한 번 순회다. byte가 같은 표시 entry가 있으면 그것을 돌려주고 exact를 true로
// 둔다. 없으면 비교 identity(base)와 번호가 같은 표시 가운데 가장 먼저 게시된 entry다(없으면
// null). byte가 같은 entry는 분해 결과도 같으므로 후보 조건 안에 있다. 같은 base는 같은 bucket에
// 있고, 한 bucket 안의 id는 게시 순번 순서이므로 가장 작은 id가 첫 표시다(chain은 최근 게시가
// 앞이다).
NamePool::Entry* NamePool::FindDisplay(StringView source, usize baseBytes, uint32 number,
                                       uint64 hash, bool& exact) const noexcept {
    exact = false;
    const usize bucket = static_cast<usize>(hash & (BucketCount - 1));
    Entry* first = nullptr;
    for (Entry* entry = buckets_[bucket]; entry != nullptr; entry = entry->next) {
        if (entry->hash != hash || entry->number != number || entry->baseBytes != baseBytes ||
            !EqualBase(entry->View(), source, baseBytes)) {
            continue;
        }

        if (entry->View() == source) {
            exact = true;
            return entry;
        }

        if (first == nullptr || entry->id < first->id) {
            first = entry;
        }
    }

    return first;
}

Result<NameLease> NamePool::Acquire(Name name) noexcept {
    platform::LockGuard lock(gate_);
    if (state_ != State::Running) {
        return Result<NameLease>::Failure(ErrorCode::ShuttingDown);
    }

    if (!name.IsValid()) {
        return Result<NameLease>::Failure(ErrorCode::StaleHandle);
    }
    // 기본 None은 pool 저장소를 사용하지 않지만 lease 수명·shutdown 규약은 동일하다.
    if (name.kind == NameKind::None) {
        if (stats_.activeLeases == NumericLimits<uint64>::Max()) {
            return Result<NameLease>::Failure(ErrorCode::Overflow);
        }
        ++stats_.activeLeases;
        return Result<NameLease>::Success(NameLease(*this, name, StringView("None")));
    }
    if (name.domain != stats_.domain) {
        return Result<NameLease>::Failure(ErrorCode::StaleHandle);
    }

    const uint64 displayId = name.displayId;
    const usize bucket = static_cast<usize>(displayId & (BucketCount - 1));
    for (Entry* entry = buckets_[bucket]; entry != nullptr; entry = entry->next) {
        if (entry->id != displayId || entry->comparisonId != name.id ||
            entry->number != name.number) {
            continue;
        }

        if (stats_.activeLeases == NumericLimits<uint64>::Max()) {
            return Result<NameLease>::Failure(ErrorCode::Overflow);
        }

        ++stats_.activeLeases;
        return Result<NameLease>::Success(NameLease(*this, name, entry->View()));
    }

    return Result<NameLease>::Failure(ErrorCode::StaleHandle);
}

void NamePool::ReleaseLease() noexcept {
    platform::LockGuard lock(gate_);
    if (stats_.activeLeases == 0) {
        platform::Fatal("NameLease count underflow");
    }

    --stats_.activeLeases;
}

void NamePool::FinishOperation(usize reservedBytes) noexcept {
    platform::LockGuard lock(gate_);
    if (stats_.activeOperations == 0 ||
        (reservedBytes != 0 &&
         (stats_.pendingEntries == 0 || stats_.pendingStorageBytes < reservedBytes))) {
        platform::Fatal("NamePool operation reservation underflow");
    }

    if (reservedBytes != 0) {
        --stats_.pendingEntries;
        stats_.pendingStorageBytes -= reservedBytes;
    }

    --stats_.activeOperations;
}

void NamePool::FreeEntry(Entry* entry) noexcept {
    DestroyAt(entry);
    if (!allocator_->Deallocate(entry)) {
        platform::Fatal("Name entry storage was not owned by its allocator");
    }
}

Result<void> NamePool::Shutdown() noexcept {
    Entry* detached[BucketCount]{};
    {
        platform::LockGuard lock(gate_);
        if (state_ == State::Stopped) {
            return Result<void>::Success();
        }

        if (state_ == State::Reclaiming) {
            return Result<void>::Failure(ErrorCode::Busy);
        }

        state_ = State::Closing;
        stats_.accepting = false;
        if (stats_.activeOperations != 0 || stats_.activeLeases != 0) {
            return Result<void>::Failure(ErrorCode::Busy);
        }

        for (usize index = 0; index < BucketCount; ++index) {
            detached[index] = buckets_[index];
            buckets_[index] = nullptr;
        }

        state_ = State::Reclaiming;
        stats_.reclaiming = true;
    }

    for (Entry* chain : detached) {
        while (chain != nullptr) {
            Entry* next = chain->next;
            FreeEntry(chain);
            chain = next;
        }
    }

    platform::LockGuard lock(gate_);
    stats_.entryCount = 0;
    stats_.storageBytes = 0;
    stats_.reclaiming = false;
    state_ = State::Stopped;
    return Result<void>::Success();
}

NamePoolStats NamePool::Stats() const noexcept {
    platform::LockGuard lock(gate_);
    return stats_;
}

Result<NameKey> TryMakeNameKey(StringView text) noexcept {
    auto valid = text.ValidateUtf8();
    if (!valid) {
        return Result<NameKey>::Failure(valid.Error());
    }
    return Result<NameKey>::Success(KeyFor(text));
}

NameTextParts SplitNameText(StringView text) noexcept {
    const ParsedName parsed = ParseName(text);
    return NameTextParts{StringView(text.Data(), parsed.baseBytes), parsed.number};
}

bool NameTextEquals(StringView left, StringView right) noexcept {
    const ParsedName leftParsed = ParseName(left);
    const ParsedName rightParsed = ParseName(right);
    // 빈 base는 번호와 무관하게 기본 None이고, base "None"(ASCII 대소문자 무시)은 번호로만 구별한다
    // (TryIntern의 None 판정·KeyFor와 같다).
    const bool leftNone = leftParsed.baseBytes == 0 || IsNoneBase(left, leftParsed.baseBytes);
    const bool rightNone = rightParsed.baseBytes == 0 || IsNoneBase(right, rightParsed.baseBytes);
    if (leftNone || rightNone) {
        const uint32 leftNumber = leftParsed.baseBytes == 0 ? 0 : leftParsed.number;
        const uint32 rightNumber = rightParsed.baseBytes == 0 ? 0 : rightParsed.number;
        return leftNone && rightNone && leftNumber == rightNumber;
    }

    return leftParsed.number == rightParsed.number &&
           leftParsed.baseBytes == rightParsed.baseBytes &&
           EqualBase(left, right, leftParsed.baseBytes);
}

} // namespace rocks
