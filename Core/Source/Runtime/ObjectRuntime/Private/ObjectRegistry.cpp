#include <Core/Algorithms.h>
#include <Core/Hash.h>
#include <Core/Text/NameComponent.h>
#include <Core/Text/StringBuilder.h>
#include <Object/ObjectRegistry.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>
#include <Task/WorkerScheduler.h>
#include "RegistryIdentity.h"
#include <new>

namespace rocks {

namespace {
constexpr uint32 SlotsPerChunk = 64;
enum class SlotState : uint8 {
    Free,
    Reserved,
    Alive,
    DestroyRequested,
    Retired,
    Reclaiming,
    Exhausted
};
} // namespace

// 슬롯과 객체 저장소를 이동하지 않는다. 슬롯 재사용 시 세대를 올리고 wrap 대신 영구 격리한다.
// TypeLease를 소멸 호출용 지역 변수로 옮긴 뒤에도 진단에는 stableType을 사용한다.
struct ObjectRegistry::Slot {
    uint32 generation = 1;
    uint32 index = 0;
    SlotState state = SlotState::Free;
    Object* object = nullptr;
    void* storage = nullptr;
    TypeLease type;
    TypeId stableType;
    uint32 roots = 0;
    uint32 borrows = 0;
    uint32 backendHolds = 0;
    // encode 고정 계수(PinForEncode). 0이 아니면 컨테이너·참조 변경이 Busy이고 회수하지 않는다.
    uint32 encodePins = 0;
    uint64 transaction = 0;
    uint64 mark = 0;
    // Outer·이름(생성 때 고정 — Docs/Design/ObjectFoundations.ko.md 1.4절). 고리는 슬롯 번호가
    // 아니라 Slot*다: chunk는 Registry 소멸 전까지 이동·반환되지 않으므로 포인터가 안정하고, 슬롯
    // 번호로 이으면 SlotAt의 chunk 선형 탐색 때문에 순회가 O(n·chunks)가 된다. Reclaiming·Free
    // 슬롯의 이 칸은 읽지 않는다. Retired 슬롯은 자기를 부모 고리에서 이미 뗐지만, Retired 부모의
    // firstChild는 그 자식들이 스스로 뗄 때까지 유효하다(그동안 회수하지 않는다 — IsReclaimable).
    ObjectHandle outer;
    Slot* outermost = nullptr;
    Slot* parent = nullptr;
    Slot* firstChild = nullptr;
    Slot* lastChild = nullptr;
    Slot* nextSibling = nullptr;
    Slot* prevSibling = nullptr;
    // 이름이 있거나 대표 자식이면 있다(Outer 없는 이름 없는 객체는 null). 회수 때 잠금 밖에서
    // 반환한다.
    NameRecord* name = nullptr;
    uint16 pathBytes = 0;
    bool indexed = false; // 이름 색인에 있다
    bool linked = false;  // 부모 고리에 있다
};

struct ObjectRegistry::Chunk {
    Chunk* next = nullptr;
    Slot slots[SlotsPerChunk];
};

struct ObjectRegistry::MarkState {
    ObjectRegistry* registry;
    Array<ObjectHandle>* work;
    uint64 transaction = 0;
    ErrorCode error = ErrorCode::None;
};

// 할당 하나에 NFC 이름과 그 뒤 비교 키를 담는다. keyHash는 (outer handle, 비교 키)의 색인 해시다.
struct ObjectRegistry::NameRecord {
    uint64 keyHash = 0;
    uint16 nameBytes = 0;
    uint16 keyBytes = 0;

    const char* Data() const noexcept {
        return reinterpret_cast<const char*>(this + 1);
    }

    StringView Name() const noexcept {
        return StringView(Data(), nameBytes);
    }

    StringView Key() const noexcept {
        return StringView(Data() + nameBytes, keyBytes);
    }
};

// 이름 색인 항목이다. slot이 null이면 빈 칸(keyHash 0) 또는 tombstone(keyHash 1)이다. 해시를 항목에
// 두어 rehash가 슬롯을 읽지 않는다.
struct ObjectRegistry::NameEntry {
    Slot* slot = nullptr;
    uint64 keyHash = 0;
};

// CollectInners 보조 배열 원소다. 수준 순서로 모은 뒤 같은 Outer 구간마다 정렬한다.
struct ObjectRegistry::InnerEntry {
    Slot* slot = nullptr;
    uint32 parent = 0;     // 부모 원소 위치(직계면 NoInnerParent)
    uint32 childStart = 0; // 자식 구간 시작(DepthFirst)
    uint32 childCount = 0;
};

namespace {

constexpr uint32 NameTableTag = 0x4e414d54u;  // 'NAMT'
constexpr uint32 NameRecordTag = 0x4e414d52u; // 'NAMR'
constexpr uint32 NoInnerParent = 0xffffffffu;
constexpr uint32 MinimumNameCapacity = 16;

void ReleaseRegistryMemory(Allocator& allocator, void* memory) noexcept {
    if (memory != nullptr && !allocator.Deallocate(memory)) {
        platform::Fatal("Registry allocation ownership mismatch");
    }
}

Result<void> OpenState(bool closed, bool processing) noexcept {
    if (closed || processing) {
        return Result<void>::Failure(closed ? ErrorCode::ShuttingDown : ErrorCode::Busy);
    }

    return Result<void>::Success();
}

// (outer handle 전체, 비교 키)의 색인 해시다. outer에 generation이 있으므로 회수된 Outer의 옛
// 항목이 새 객체와 섞이지 않는다.
uint64 NameKeyHash(ObjectHandle outer, StringView key) noexcept {
    const uint64 owner =
        MixHash(outer.registry ^ MixHash((uint64(outer.slot) << 32) | outer.generation));
    return MixHash(HashStringBytes(key) ^ owner);
}

// outer 아래 이름이 nameBytes byte인 새 객체의 객체 경로 길이다. 경로는 Outer 경로 + (둘 다 비어
// 있지 않으면 '.') + 이름이다(outermost·대표 자식은 빈 경로).
usize ChildPathBytes(const OuterSummary* outer, usize nameBytes) noexcept {
    if (outer == nullptr) {
        return 0;
    }

    return outer->pathBytes + (outer->pathBytes != 0 && nameBytes != 0 ? 1u : 0u) + nameBytes;
}

// NFC UTF-8 byte 순(빈 이름이 먼저)이다.
bool NameLess(StringView left, StringView right) noexcept {
    const usize common =
        left.ByteCount() < right.ByteCount() ? left.ByteCount() : right.ByteCount();
    const int order = platform::MemoryCompare(left.Data(), right.Data(), common);
    return order != 0 ? order < 0 : left.ByteCount() < right.ByteCount();
}

} // namespace

ObjectRegistry::ObjectRegistry(Allocator& allocator, TypeRegistry& types, ContextId context,
                               RegistryOptions options) noexcept
    : allocator_(allocator), types_(types), context_(context), options_(options),
      domain_(detail::NextRegistryDomain()), ownerThread_(platform::CurrentThreadId()),
      gcWork_(allocator) {
}

ObjectRegistry::~ObjectRegistry() {
    auto stopped = Shutdown(OwnerAccess());
    if (!stopped) {
        platform::Fatal(
            "ObjectRegistry destroyed before roots/readers/backend leases/encode pins drained");
    }

    // 종료 회수가 모든 NameRecord를 반환했다. 남은 것은 빈 색인 표다.
    ReleaseRegistryMemory(allocator_, names_);
    names_ = nullptr;
    while (chunks_) {
        Chunk* next = chunks_->next;
        chunks_->~Chunk();
        auto released = allocator_.Deallocate(chunks_);
        if (!released) {
            platform::Fatal("Registry chunk allocator mismatch");
        }

        chunks_ = next;
    }
}

Result<void> ObjectRegistry::CheckAccess(const ObjectAccessContext& access) const noexcept {
    if (platform::CurrentThreadId() != ownerThread_) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    if (!(access.context == context_)) {
        return Result<void>::Failure(ErrorCode::WrongContext);
    }

    return Result<void>::Success();
}

ObjectRegistry::Slot* ObjectRegistry::SlotAt(uint32 index) const noexcept {
    for (Chunk* chunk = chunks_; chunk; chunk = chunk->next) {
        const uint32 first = chunk->slots[0].index;
        if (index >= first && index - first < SlotsPerChunk) {
            return &chunk->slots[index - first];
        }
    }

    return nullptr;
}

ObjectRegistry::Slot* ObjectRegistry::FindSlot(ObjectHandle handle) const noexcept {
    if (handle.registry != domain_ || handle.generation == 0 || handle.slot >= options_.maxSlots) {
        return nullptr;
    }

    Slot* slot = SlotAt(handle.slot);
    return slot && slot->generation == handle.generation ? slot : nullptr;
}

Result<void> ObjectRegistry::CheckLive(Slot* slot, const void* nativeType) const noexcept {
    if (!slot || slot->state == SlotState::Free || slot->state == SlotState::Exhausted ||
        slot->state == SlotState::Reserved) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (slot->state != SlotState::Alive || IsCutoff(slot)) {
        return Result<void>::Failure(ErrorCode::Destroyed);
    }

    // 요청 타입이거나 그 파생이면 허용한다(단일 비가상 상속 사슬, TypeLease::IsNativeA).
    if (nativeType && !slot->type.IsNativeA(nativeType)) {
        return Result<void>::Failure(ErrorCode::TypeMismatch);
    }

    return Result<void>::Success();
}

ObjectHandle ObjectRegistry::HandleOf(const Slot& slot) const noexcept {
    return {domain_, slot.index, slot.generation};
}

Result<ObjectRegistry::Slot*>
ObjectRegistry::FindPublishedLocked(ObjectHandle handle) const noexcept {
    Slot* slot = FindSlot(handle);
    if (!slot || slot->state == SlotState::Free || slot->state == SlotState::Exhausted ||
        slot->state == SlotState::Reserved) {
        return Result<Slot*>::Failure(ErrorCode::StaleHandle);
    }

    if (IsCutoff(slot) ||
        !(slot->state == SlotState::Alive || slot->state == SlotState::DestroyRequested)) {
        return Result<Slot*>::Failure(ErrorCode::Destroyed);
    }

    return Result<Slot*>::Success(slot);
}

OuterSummary ObjectRegistry::DescribeSlotLocked(const Slot& slot,
                                                bool scanNamedInners) const noexcept {
    OuterSummary summary;
    const Slot& outermost = *slot.outermost;
    summary.outermost = HandleOf(outermost);
    summary.isOutermost = slot.outer.IsNull();
    summary.outermostIsPackage = options_.policy.packageNativeType != nullptr &&
                                 outermost.type.IsNativeA(options_.policy.packageNativeType);
    // Inner 유무는 이름 점유와 같은 기준이다(다른 트랜잭션의 Reserved는 세고 cutoff white는 세지
    // 않는다). 대표 자식은 색인으로 O(1)에 찾고, 이름 있는 Inner는 고리에서 처음 만나는 것까지
    // 본다.
    const ObjectHandle self = HandleOf(slot);
    summary.hasRepresentative = FindNamedLocked(self, NameKeyHash(self, {}), {}, false) != nullptr;
    for (const Slot* child = scanNamedInners ? slot.firstChild : nullptr; child;
         child = child->nextSibling) {
        if (!IsCutoff(child) && child->name != nullptr && child->name->nameBytes != 0) {
            summary.hasNamedInners = true;
            break;
        }
    }

    summary.pathBytes = slot.pathBytes;
    return summary;
}

ObjectRegistry::Slot* ObjectRegistry::FindNamedLocked(ObjectHandle outer, uint64 hash,
                                                      StringView key,
                                                      bool published) const noexcept {
    if (nameCapacity_ == 0) {
        return nullptr;
    }

    // 적재율이 1/2 이하라 빈 칸이 반드시 있어 탐사가 끝난다.
    const uint32 mask = nameCapacity_ - 1;
    for (uint32 index = static_cast<uint32>(hash) & mask;; index = (index + 1) & mask) {
        const NameEntry& entry = names_[index];
        if (entry.slot == nullptr) {
            if (entry.keyHash == 0) {
                return nullptr;
            }

            continue;
        }

        Slot* slot = entry.slot;
        if (entry.keyHash != hash || !(slot->outer == outer) || !(slot->name->Key() == key)) {
            continue;
        }

        // cutoff white 객체는 이름을 점유하지 않는다(같은 키의 새 항목이 뒤에 있을 수 있다).
        if (IsCutoff(slot) || (published && slot->state != SlotState::Alive &&
                               slot->state != SlotState::DestroyRequested)) {
            continue;
        }

        return slot;
    }
}

Result<uint32> ObjectRegistry::NameIndexGrowthLocked() const noexcept {
    const uint64 used = uint64(nameLive_) + nameTombstones_ + 1;
    if (nameCapacity_ != 0 && used * 2 <= nameCapacity_) {
        return Result<uint32>::Success(0);
    }

    if (nameCapacity_ == 0) {
        return Result<uint32>::Success(MinimumNameCapacity);
    }

    // live가 1/4 이하면 tombstone만 정리하는 같은 크기 rebuild, 넘으면 두 배다.
    if ((uint64(nameLive_) + 1) * 4 <= nameCapacity_) {
        return Result<uint32>::Success(nameCapacity_);
    }

    if (nameCapacity_ > 0x40000000u) {
        return Result<uint32>::Failure(ErrorCode::CapacityExceeded);
    }

    return Result<uint32>::Success(nameCapacity_ * 2);
}

ObjectRegistry::NameEntry* ObjectRegistry::RehashNamesLocked(NameEntry* table,
                                                             uint32 capacity) noexcept {
    const uint32 mask = capacity - 1;
    for (uint32 index = 0; index < nameCapacity_; ++index) {
        const NameEntry& entry = names_[index];
        if (entry.slot == nullptr) {
            continue;
        }

        uint32 position = static_cast<uint32>(entry.keyHash) & mask;
        while (table[position].slot != nullptr) {
            position = (position + 1) & mask;
        }

        table[position] = entry;
    }

    NameEntry* previous = names_;
    names_ = table;
    nameCapacity_ = capacity;
    nameTombstones_ = 0;
    return previous;
}

void ObjectRegistry::InsertNameLocked(Slot& slot, uint64 hash, StringView key) noexcept {
    const uint32 mask = nameCapacity_ - 1;
    uint32 target = NoInnerParent;
    for (uint32 index = static_cast<uint32>(hash) & mask;; index = (index + 1) & mask) {
        NameEntry& entry = names_[index];
        if (entry.slot == nullptr) {
            if (target == NoInnerParent) {
                target = index;
            }

            if (entry.keyHash == 0) {
                break;
            }

            continue;
        }

        // 같은 키의 cutoff white 항목은 이름을 점유하지 않으므로 이 자리에서 뺀다(그 객체의
        // Retire는 indexed가 꺼져 있어 색인을 다시 보지 않는다).
        if (entry.keyHash == hash && entry.slot->outer == slot.outer &&
            entry.slot->name->Key() == key && IsCutoff(entry.slot)) {
            entry.slot->indexed = false;
            entry.slot = nullptr;
            entry.keyHash = 1;
            --nameLive_;
            ++nameTombstones_;
            if (target == NoInnerParent) {
                target = index;
            }
        }
    }

    NameEntry& chosen = names_[target];
    if (chosen.keyHash == 1) {
        --nameTombstones_;
    }

    chosen.slot = &slot;
    chosen.keyHash = hash;
    ++nameLive_;
    slot.indexed = true;
}

void ObjectRegistry::RemoveNameLocked(Slot& slot) noexcept {
    const uint32 mask = nameCapacity_ - 1;
    for (uint32 index = static_cast<uint32>(slot.name->keyHash) & mask;;
         index = (index + 1) & mask) {
        NameEntry& entry = names_[index];
        if (entry.slot == &slot) {
            entry.slot = nullptr;
            entry.keyHash = 1;
            --nameLive_;
            ++nameTombstones_;
            slot.indexed = false;
            return;
        }

        if (entry.slot == nullptr && entry.keyHash == 0) {
            platform::Fatal("Object name index lost an entry");
        }
    }
}

void ObjectRegistry::LinkChildLocked(Slot& parent, Slot& child) noexcept {
    child.prevSibling = parent.lastChild;
    child.nextSibling = nullptr;
    if (parent.lastChild != nullptr) {
        parent.lastChild->nextSibling = &child;
    } else {
        parent.firstChild = &child;
    }

    parent.lastChild = &child;
    child.linked = true;
}

void ObjectRegistry::UnlinkChildLocked(Slot& child) noexcept {
    Slot& parent = *child.parent;
    if (child.prevSibling != nullptr) {
        child.prevSibling->nextSibling = child.nextSibling;
    } else {
        parent.firstChild = child.nextSibling;
    }

    if (child.nextSibling != nullptr) {
        child.nextSibling->prevSibling = child.prevSibling;
    } else {
        parent.lastChild = child.prevSibling;
    }

    child.prevSibling = nullptr;
    child.nextSibling = nullptr;
    child.linked = false;
}

void ObjectRegistry::DetachIdentityLocked(Slot& slot) noexcept {
    // 고리 해체는 하지 않는다: white 부모에 연결된 자식은 모두 white라 같은 sweep에서 스스로
    // 떨어지고, rollback은 생성 역순이라 자식이 먼저다(결정 15).
    if (slot.indexed) {
        RemoveNameLocked(slot);
    }

    if (slot.linked) {
        UnlinkChildLocked(slot);
    }
}

namespace detail {

Result<void> CheckNewObjectIdentity(const RegistryPolicy& policy, const TypeLease& type,
                                    const OuterSummary* outer, StringView name,
                                    Allocator& scratch) noexcept {
    // 6. 에셋 객체 Registry는 WorldOnly 타입을 만들지 않는다(상속 플래그라 자기 플래그만 본다).
    if (policy.rejectWorldOnly && HasFlag(type.Schema().flags, TypeFlags::WorldOnly)) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    // 7. 이름 형식(빈 이름은 이름 없음 또는 대표 자식이라 형식 검사 대상이 아니다).
    if (!name.IsEmpty()) {
        if (auto valid = ValidateNameComponent(name, scratch); !valid) {
            return valid;
        }
    }

    // 8. 패키지 정책·빈 이름·대표 자식.
    if (policy.packageNativeType != nullptr) {
        if (type.IsNativeA(policy.packageNativeType)) {
            if (outer != nullptr || !name.IsEmpty()) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }
        } else if (outer == nullptr || !outer->outermostIsPackage) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    if (outer != nullptr) {
        if (name.IsEmpty()) {
            if (!outer->isOutermost || outer->hasNamedInners) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }
        } else if (outer->hasRepresentative) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    // 9. 경로 길이.
    if (ChildPathBytes(outer, name.ByteCount()) > MaxObjectPathBytes) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    return Result<void>::Success();
}

} // namespace detail

Result<void> ObjectRegistry::AddRoot(const ObjectAccessContext& access, ObjectHandle handle,
                                     const void* nativeType) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return valid;
    }

    platform::LockGuard lock(gate_);
    if (closed_ || processing_) {
        return Result<void>::Failure(closed_ ? ErrorCode::ShuttingDown : ErrorCode::Busy);
    }

    Slot* slot = FindSlot(handle);
    auto live = CheckLive(slot, nativeType);
    if (!live) {
        return live;
    }

    if (slot->roots == 0xffffffffu) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    const ErrorCode shaded = ShadeLocked(handle);
    if (shaded != ErrorCode::None) {
        return Result<void>::Failure(shaded);
    }

    ++slot->roots;
    return Result<void>::Success();
}

Result<void> ObjectRegistry::AddBorrow(const ObjectAccessContext& access, ObjectHandle handle,
                                       const void* nativeType, bool rejectEncodePinned) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return valid;
    }

    platform::LockGuard lock(gate_);
    if (closed_ || processing_) {
        return Result<void>::Failure(closed_ ? ErrorCode::ShuttingDown : ErrorCode::Busy);
    }

    Slot* slot = FindSlot(handle);
    auto live = CheckLive(slot, nativeType);
    if (!live) {
        return live;
    }

    // 쓰기용 Borrow는 encode 중인 owner의 값을 바꾸지 못하게 한다(ResolveForWrite).
    if (rejectEncodePinned && slot->encodePins != 0) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    if (totalBorrows_ == 0xffffffffu || slot->borrows == 0xffffffffu) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    ++slot->borrows;
    ++totalBorrows_;
    return Result<void>::Success();
}

namespace detail {

Result<void> RetainRoot(ObjectRegistry* registry, ObjectHandle handle) noexcept {
    if (!registry) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    platform::LockGuard lock(registry->gate_);
    if (registry->processing_ || registry->closed_) {
        return Result<void>::Failure(registry->closed_ ? ErrorCode::ShuttingDown : ErrorCode::Busy);
    }

    auto* slot = registry->FindSlot(handle);
    if (!slot || slot->roots == 0 ||
        !(slot->state == SlotState::Alive || slot->state == SlotState::DestroyRequested)) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (slot->roots == 0xffffffffu) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    const ErrorCode shaded = registry->ShadeLocked(handle);
    if (shaded != ErrorCode::None) {
        return Result<void>::Failure(shaded);
    }

    ++slot->roots;
    return Result<void>::Success();
}

void ReleaseRoot(ObjectRegistry* registry, ObjectHandle handle) noexcept {
    platform::LockGuard lock(registry->gate_);
    auto* slot = registry->FindSlot(handle);
    if (!slot || slot->roots == 0) {
        platform::Fatal("RootRef release after lifetime closure");
    }

    --slot->roots;
}

Result<void*> AccessBorrow(ObjectRegistry* registry, ObjectHandle handle,
                           const void* nativeType) noexcept {
    if (!registry) {
        return Result<void*>::Failure(ErrorCode::InvalidArgument);
    }

    if (platform::CurrentThreadId() != registry->ownerThread_) {
        return Result<void*>::Failure(ErrorCode::WrongThread);
    }

    // 명시된 문맥이 있으면 다른 World의 Borrow에서 포인터를 얻을 수 없다.
    // 문맥이 없을 때의 일반 조회와 다른 스레드의 Borrow 반환은 그대로 허용한다.
    auto current = ObjectAccessScope::Current();
    if (current && !(current.Value().context == registry->context_)) {
        return Result<void*>::Failure(ErrorCode::WrongContext);
    }

    platform::LockGuard lock(registry->gate_);
    auto* slot = registry->FindSlot(handle);
    auto valid = registry->CheckLive(slot, nativeType);
    if (!valid) {
        return Result<void*>::Failure(valid.Error());
    }

    if (slot->borrows == 0) {
        return Result<void*>::Failure(ErrorCode::StaleHandle);
    }

    // 항상 Object 하위 객체 주소를 돌려준다. Borrow<T>::Get이 static_cast로 T*를 얻는다(파생 타입의
    // 기반 T 하위 객체 위치는 C++ 변환만 정확히 안다).
    return Result<void*>::Success(static_cast<void*>(slot->object));
}

Result<ObjectLifecycle> InspectBorrow(ObjectRegistry* registry, ObjectHandle handle) noexcept {
    if (!registry) {
        return Result<ObjectLifecycle>::Failure(ErrorCode::InvalidArgument);
    }

    auto access = registry->ScopedAccess();
    if (!access) {
        return Result<ObjectLifecycle>::Failure(access.Error());
    }

    return registry->InspectChecked(access.Value(), handle, nullptr, true);
}

void ReleaseBorrow(ObjectRegistry* registry, ObjectHandle handle) noexcept {
    platform::LockGuard lock(registry->gate_);
    auto* slot = registry->FindSlot(handle);
    if (!slot || slot->borrows == 0 || registry->totalBorrows_ == 0) {
        platform::Fatal("Borrow release after lifetime closure");
    }

    --slot->borrows;
    --registry->totalBorrows_;
}

void ReleaseBackend(ObjectRegistry* registry, ObjectHandle handle) noexcept {
    platform::LockGuard lock(registry->gate_);
    auto* slot = registry->FindSlot(handle);
    if (!slot || slot->backendHolds == 0) {
        platform::Fatal("Backend lease release after lifetime closure");
    }

    --slot->backendHolds;
}

void ReleaseEncodePin(ObjectRegistry* registry, ObjectHandle handle) noexcept {
    platform::LockGuard lock(registry->gate_);
    auto* slot = registry->FindSlot(handle);
    if (!slot || slot->encodePins == 0) {
        platform::Fatal("Encode pin release after lifetime closure");
    }

    --slot->encodePins;
}

} // namespace detail

namespace {

constexpr uint32 ObjectRefArrayTag = 0x4f524146u;   // 'ORAF'
constexpr uint32 StructRefElementTag = 0x53524145u; // 'SRAE'
constexpr uint32 StructRefIndexTag = 0x53524149u;   // 'SRAI'

// 컨테이너 버퍼의 다음 용량이다(4부터 두 배, uint32 상한에서 포화).
uint32 GrownCapacity(uint32 capacity) noexcept {
    return capacity < 4u ? 4u : capacity > 0x7fffffffu ? 0xffffffffu : capacity * 2u;
}

void ReleaseContainerMemory(Allocator& allocator, void* memory) noexcept {
    if (memory != nullptr && !allocator.Deallocate(memory)) {
        platform::Fatal("Registry container deallocation failed");
    }
}

// StructRefArray 원소 하나를 소멸하고 블록을 반환한다.
void DestroyStructElement(Allocator& allocator, const detail::StructElementOps& ops,
                          void* block) noexcept {
    ops.destroy(block);
    ReleaseContainerMemory(allocator, block);
}

} // namespace

Result<void> ObjectRegistry::CheckContainerOwnerLocked(ObjectHandle owner, const void* storage,
                                                       usize storageBytes, uint64 transaction,
                                                       bool allowDestroyRequested) noexcept {
    Slot* source = FindSlot(owner);
    if (!source || !source->object || IsCutoff(source)) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (transaction != 0) {
        // staged: 같은 생성 트랜잭션의 Reserved 객체다(Reserved는 Borrow를 얻을 수 없다).
        if (source->state != SlotState::Reserved || source->transaction != transaction) {
            return Result<void>::Failure(ErrorCode::StaleHandle);
        }
    } else {
        if (!(source->state == SlotState::Alive ||
              (allowDestroyRequested && source->state == SlotState::DestroyRequested))) {
            return Result<void>::Failure(ErrorCode::StaleHandle);
        }

        // 컨테이너는 owner 객체 안의 멤버이므로 호출자는 owner의 Borrow를 쥔 채 호출해야 한다.
        // Borrow가 있는 동안은 수집이 Busy이고 회수도 일어나지 않으므로, 잠금 밖 할당·원소 생성 중
        // allocator·생성자가 Registry에 재진입해도 owner 저장소가 사라지지 않는다.
        if (source->borrows == 0) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    // 컨테이너 객체는 owner 저장소 안에 직접 포함되어야 한다(SetEdge의 필드 위치 검사와 같다).
    const uptr start = reinterpret_cast<uptr>(source->storage);
    const uptr location = reinterpret_cast<uptr>(storage);
    const usize size = source->type.Ops().size;
    if (size < storageBytes || location < start || location - start > size - storageBytes) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // encode 중인 owner의 버퍼를 바꾸지 않는다(PinForEncode). 호출 오류(InvalidArgument)를 가리지
    // 않게 마지막에 본다. Reserved 객체는 고정할 수 없다.
    if (source->encodePins != 0) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    return Result<void>::Success();
}

Result<void> ObjectRegistry::CheckTargetLocked(ObjectHandle target, uint64 transaction,
                                               const void* nativeType) const noexcept {
    Slot* destination = FindSlot(target);
    if (!destination || IsCutoff(destination) ||
        !(destination->state == SlotState::Alive ||
          (transaction && destination->state == SlotState::Reserved &&
           destination->transaction == transaction))) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (nativeType && !destination->type.IsNativeA(nativeType)) {
        return Result<void>::Failure(ErrorCode::TypeMismatch);
    }

    return Result<void>::Success();
}

Result<void> ObjectRegistry::SetEdge(const ObjectAccessContext& access, ObjectHandle owner,
                                     ObjectHandle& field, ObjectHandle target, uint64 transaction,
                                     const void* nativeType,
                                     const detail::StructElementLocation* element) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return valid;
    }

    platform::LockGuard lock(gate_);
    if (processing_ || closed_) {
        return Result<void>::Failure(closed_ ? ErrorCode::ShuttingDown : ErrorCode::Busy);
    }

    const uptr location = reinterpret_cast<uptr>(&field);
    if (element) {
        // 원소 모드: 컨테이너가 owner 저장소 안, index < size, field가 그 원소 블록 안(O(1)).
        // 컨테이너 위치가 owner 저장소 안임을 확인하기 전에는 storage를 역참조하지 않는다.
        const detail::StructRefArrayStorage* storage = element->storage;
        auto checked =
            CheckContainerOwnerLocked(owner, storage, sizeof(*storage), transaction, false);
        if (!checked) {
            return checked;
        }

        if (element->index >= storage->size) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        const uptr block = reinterpret_cast<uptr>(storage->blocks[element->index]);
        const usize bytes = storage->element->size;
        if (bytes < sizeof(ObjectHandle) || location < block ||
            location - block > bytes - sizeof(ObjectHandle)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    } else {
        Slot* source = FindSlot(owner);
        if (!source || !source->object || IsCutoff(source) ||
            (transaction
                 ? source->state != SlotState::Reserved || source->transaction != transaction
                 : source->state != SlotState::Alive)) {
            return Result<void>::Failure(ErrorCode::StaleHandle);
        }

        const uptr start = reinterpret_cast<uptr>(source->storage);
        const usize size = source->type.Ops().size;
        if (location < start || location - start > size - sizeof(ObjectHandle)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        // 위치 오류를 가리지 않게 마지막에 본다(PinForEncode).
        if (source->encodePins != 0) {
            return Result<void>::Failure(ErrorCode::Busy);
        }
    }

    if (target.IsNull()) {
        // 비교·Required 검사·encode가 정규 null만 보게 한다.
        field = ObjectHandle{};
        return Result<void>::Success();
    }

    auto targetChecked = CheckTargetLocked(target, transaction, nativeType);
    if (!targetChecked) {
        return targetChecked;
    }

    // staged 내부 간선은 Commit에서 전체 게시·shade한다. 이미 게시된 대상은 즉시 보존한다.
    if (FindSlot(target)->state == SlotState::Alive) {
        const ErrorCode shaded = ShadeLocked(target);
        if (shaded != ErrorCode::None) {
            return Result<void>::Failure(shaded);
        }
    }

    field = target;
    return Result<void>::Success();
}

Result<void> ObjectRegistry::InsertArrayEdge(const ObjectAccessContext& access, ObjectHandle owner,
                                             detail::ObjectRefArrayStorage& storage, usize index,
                                             bool append, ObjectHandle target,
                                             const void* nativeType) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return valid;
    }

    if (target.IsNull()) {
        target = ObjectHandle{};
    }

    // owner 검증 뒤에만 부른다(배열이 owner 저장소 안임을 확인한 뒤 size를 읽는다).
    auto positionLocked = [&]() noexcept -> usize { return append ? storage.size : index; };
    Allocator& allocator = *storage.allocator;
    // owner·대상 검증(잠금 보유). 할당 전과 할당 뒤에 같은 검사를 하고, 그 사이 index를 본다.
    auto ownerLocked = [&]() noexcept -> Result<void> {
        if (processing_ || closed_) {
            return Result<void>::Failure(closed_ ? ErrorCode::ShuttingDown : ErrorCode::Busy);
        }

        return CheckContainerOwnerLocked(owner, &storage, sizeof(storage), 0, false);
    };
    auto targetLocked = [&]() noexcept -> Result<void> {
        return target.IsNull() ? Result<void>::Success() : CheckTargetLocked(target, 0, nativeType);
    };
    // position에 target을 넣어 게시한다(잠금 보유, 검증 뒤). buffer는 size + 1을 담는 버퍼(기존
    // 또는 새 것)이고, 새 버퍼로 바꾸면 잠금 밖에서 반환할 옛 버퍼를 돌려준다. shade 실패면
    // 불변이다.
    auto publishLocked = [&](usize position, ObjectHandle* buffer,
                             uint32 capacity) noexcept -> Result<ObjectHandle*> {
        if (!target.IsNull()) {
            const ErrorCode shaded = ShadeLocked(target);
            if (shaded != ErrorCode::None) {
                return Result<ObjectHandle*>::Failure(shaded);
            }
        }

        ObjectHandle* previous = nullptr;
        if (buffer != storage.data) {
            for (usize at = 0; at < position; ++at) {
                buffer[at] = storage.data[at];
            }

            for (usize at = storage.size; at > position; --at) {
                buffer[at] = storage.data[at - 1];
            }

            previous = storage.data;
            storage.data = buffer;
            storage.capacity = capacity;
        } else {
            for (usize at = storage.size; at > position; --at) {
                buffer[at] = buffer[at - 1];
            }
        }

        storage.data[position] = target;
        ++storage.size;
        return Result<ObjectHandle*>::Success(previous);
    };

    uint32 grown = 0;
    {
        platform::LockGuard lock(gate_);
        // 용량을 늘리기 전에 검증해 잘못된 호출이 할당을 일으키지 않게 한다.
        auto checked = ownerLocked();
        if (!checked) {
            return checked;
        }

        const usize position = positionLocked();
        if (position > storage.size) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        checked = targetLocked();
        if (!checked) {
            return checked;
        }

        if (storage.size == 0xffffffffu) {
            return Result<void>::Failure(ErrorCode::Overflow);
        }

        if (storage.size < storage.capacity) {
            auto published = publishLocked(position, storage.data, storage.capacity);
            return published ? Result<void>::Success() : Result<void>::Failure(published.Error());
        }

        grown = GrownCapacity(storage.capacity);
    }

    // allocator callback은 Registry 잠금 밖에서 실행하며, 이 구간에는 할당만 한다. 콜백이 같은
    // 배열을 바꿀 수 있으므로 복사·삽입·게시는 다시 잠근 뒤 그때의 size·index로 한다. 레지스트리
    // 전체 처리 표시(processing_)는 세우지 않는다(흔한 런타임 목록 변경이 다른 스레드를 막지 않게).
    // owner Borrow가 수집·회수를 막으므로 owner와 배열은 살아 있다.
    auto memory =
        allocator.Allocate(sizeof(ObjectHandle) * grown, alignof(ObjectHandle), ObjectRefArrayTag);
    if (!memory) {
        return Result<void>::Failure(memory.Error());
    }

    auto* fresh = static_cast<ObjectHandle*>(memory.Value());
    ObjectHandle* release = fresh;
    Result<void> result = Result<void>::Success();
    {
        platform::LockGuard lock(gate_);
        result = ownerLocked();
        const usize position = result ? positionLocked() : 0;
        if (result && position > storage.size) {
            // 콜백이 원소를 지워 index가 지금 size를 넘는다.
            result = Result<void>::Failure(ErrorCode::Busy);
        }

        if (result) {
            result = targetLocked();
        }

        if (result) {
            if (storage.size == 0xffffffffu) {
                result = Result<void>::Failure(ErrorCode::Overflow);
            } else if (storage.size < storage.capacity) {
                // 그 사이 용량이 충분해졌다. 기존 버퍼에 넣고 새 버퍼는 반환한다.
                auto published = publishLocked(position, storage.data, storage.capacity);
                if (!published) {
                    result = Result<void>::Failure(published.Error());
                }
            } else if (grown > storage.size) {
                auto published = publishLocked(position, fresh, grown);
                if (published) {
                    release = published.Value();
                } else {
                    result = Result<void>::Failure(published.Error());
                }
            } else {
                // 콜백이 원소를 늘려 새 버퍼가 size + 1을 담지 못한다.
                result = Result<void>::Failure(ErrorCode::Busy);
            }
        }
    }

    ReleaseContainerMemory(allocator, release);
    return result;
}

Result<void> ObjectRegistry::ReplaceArrayEdge(const ObjectAccessContext& access, ObjectHandle owner,
                                              detail::ObjectRefArrayStorage& storage, usize index,
                                              ObjectHandle target,
                                              const void* nativeType) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return valid;
    }

    platform::LockGuard lock(gate_);
    if (processing_ || closed_) {
        return Result<void>::Failure(closed_ ? ErrorCode::ShuttingDown : ErrorCode::Busy);
    }

    auto checked = CheckContainerOwnerLocked(owner, &storage, sizeof(storage), 0, false);
    if (!checked) {
        return checked;
    }

    if (index >= storage.size) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (target.IsNull()) {
        storage.data[index] = ObjectHandle{};
        return Result<void>::Success();
    }

    auto targetChecked = CheckTargetLocked(target, 0, nativeType);
    if (!targetChecked) {
        return targetChecked;
    }

    const ErrorCode shaded = ShadeLocked(target);
    if (shaded != ErrorCode::None) {
        return Result<void>::Failure(shaded);
    }

    storage.data[index] = target;
    return Result<void>::Success();
}

Result<void> ObjectRegistry::RemoveArrayEdge(const ObjectAccessContext& access, ObjectHandle owner,
                                             detail::ObjectRefArrayStorage& storage, usize index,
                                             ObjectHandle target, bool byHandle) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return valid;
    }

    platform::LockGuard lock(gate_);
    if (processing_ || closed_) {
        return Result<void>::Failure(closed_ ? ErrorCode::ShuttingDown : ErrorCode::Busy);
    }

    auto checked = CheckContainerOwnerLocked(owner, &storage, sizeof(storage), 0, true);
    if (!checked) {
        return checked;
    }

    if (byHandle) {
        // 원소 null은 정규 값이므로 비정규 null 인자도 첫 null 원소를 찾는다.
        if (target.IsNull()) {
            target = ObjectHandle{};
        }

        index = storage.size;
        for (uint32 position = 0; position < storage.size; ++position) {
            if (storage.data[position] == target) {
                index = position;
                break;
            }
        }

        if (index == storage.size) {
            return Result<void>::Failure(ErrorCode::NotFound);
        }
    } else if (index >= storage.size) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 삽입 장벽 GC에서 간선 삭제는 장벽이 필요 없다. 지운 대상은 다른 경로로 도달하지 않으면
    // 회수된다.
    for (usize position = index + 1; position < storage.size; ++position) {
        storage.data[position - 1] = storage.data[position];
    }

    --storage.size;
    return Result<void>::Success();
}

Result<void> ObjectRegistry::ClearArrayEdges(const ObjectAccessContext& access, ObjectHandle owner,
                                             detail::ObjectRefArrayStorage& storage) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return valid;
    }

    platform::LockGuard lock(gate_);
    if (processing_ || closed_) {
        return Result<void>::Failure(closed_ ? ErrorCode::ShuttingDown : ErrorCode::Busy);
    }

    auto checked = CheckContainerOwnerLocked(owner, &storage, sizeof(storage), 0, true);
    if (!checked) {
        return checked;
    }

    storage.size = 0;
    return Result<void>::Success();
}

Result<void> ObjectRegistry::AssignArrayEdges(const ObjectAccessContext& access, ObjectHandle owner,
                                              detail::ObjectRefArrayStorage& storage,
                                              Span<const ObjectHandle> targets, uint64 transaction,
                                              const void* nativeType) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return valid;
    }

    if (targets.Size() > 0xffffffffu) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    const auto count = static_cast<uint32>(targets.Size());
    Allocator& allocator = *storage.allocator;
    // 모든 원소를 검증한다(잠금 보유). 하나라도 틀리면 불변이다.
    auto validateLocked = [&]() noexcept -> Result<void> {
        auto checked =
            CheckContainerOwnerLocked(owner, &storage, sizeof(storage), transaction, false);
        if (!checked) {
            return checked;
        }

        for (uint32 index = 0; index < count; ++index) {
            if (!targets[index].IsNull()) {
                auto targetChecked = CheckTargetLocked(targets[index], transaction, nativeType);
                if (!targetChecked) {
                    return targetChecked;
                }
            }
        }

        return Result<void>::Success();
    };
    // 생존 대상만 shade한 뒤(ShadeLocked는 Reserved에 StaleHandle을 내므로 Reserved 대상은 Commit이
    // shade한다) buffer에 정규화해 복사하고 게시한다. 새 버퍼로 바꾸면 옛 버퍼를 돌려준다.
    auto publishLocked = [&](ObjectHandle* buffer,
                             uint32 capacity) noexcept -> Result<ObjectHandle*> {
        for (uint32 index = 0; index < count; ++index) {
            const ObjectHandle target = targets[index];
            if (!target.IsNull() && FindSlot(target)->state == SlotState::Alive) {
                const ErrorCode shaded = ShadeLocked(target);
                if (shaded != ErrorCode::None) {
                    return Result<ObjectHandle*>::Failure(shaded);
                }
            }
        }

        for (uint32 index = 0; index < count; ++index) {
            buffer[index] = targets[index].IsNull() ? ObjectHandle{} : targets[index];
        }

        ObjectHandle* previous = buffer != storage.data ? storage.data : nullptr;
        storage.data = buffer;
        storage.capacity = capacity;
        storage.size = count;
        return Result<ObjectHandle*>::Success(previous);
    };

    {
        platform::LockGuard lock(gate_);
        if (closed_ || processing_ || transaction == 0) {
            return Result<void>::Failure(closed_ ? ErrorCode::ShuttingDown : ErrorCode::Busy);
        }

        auto checked = validateLocked();
        if (!checked) {
            return checked;
        }

        if (count <= storage.capacity) {
            auto published = publishLocked(storage.data, storage.capacity);
            return published ? Result<void>::Success() : Result<void>::Failure(published.Error());
        }

        // 생성 트랜잭션 안의 잠금 밖 할당은 CreatePending과 같은 규칙이다: allocator 재진입은 Busy,
        // Rollback 재진입은 Fatal.
        processing_ = true;
    }

    auto finish = [this]() noexcept {
        platform::LockGuard lock(gate_);
        processing_ = false;
    };
    auto memory =
        allocator.Allocate(sizeof(ObjectHandle) * count, alignof(ObjectHandle), ObjectRefArrayTag);
    if (!memory) {
        finish();
        return Result<void>::Failure(memory.Error());
    }

    auto* fresh = static_cast<ObjectHandle*>(memory.Value());
    ObjectHandle* release = fresh;
    Result<void> result = Result<void>::Success();
    {
        platform::LockGuard lock(gate_);
        result = validateLocked();
        if (result) {
            auto published = publishLocked(fresh, count);
            if (published) {
                release = published.Value();
            } else {
                result = Result<void>::Failure(published.Error());
            }
        }
    }

    // 옛(또는 쓰이지 않은) 버퍼는 처리 표시 아래 잠금 밖에서 반환한다.
    ReleaseContainerMemory(allocator, release);
    finish();
    return result;
}

Result<usize> ObjectRegistry::InsertStructElement(const ObjectAccessContext& access,
                                                  ObjectHandle owner,
                                                  detail::StructRefArrayStorage& storage,
                                                  usize index, bool append) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return Result<usize>::Failure(valid.Error());
    }

    Allocator& allocator = *storage.allocator;
    const detail::StructElementOps& ops = *storage.element;
    auto validateLocked = [&]() noexcept -> Result<void> {
        if (processing_ || closed_) {
            return Result<void>::Failure(closed_ ? ErrorCode::ShuttingDown : ErrorCode::Busy);
        }

        return CheckContainerOwnerLocked(owner, &storage, sizeof(storage), 0, false);
    };

    uint32 grown = 0;
    {
        platform::LockGuard lock(gate_);
        auto checked = validateLocked();
        if (!checked) {
            return Result<usize>::Failure(checked.Error());
        }

        const usize position = append ? storage.size : index;
        if (position > storage.size) {
            return Result<usize>::Failure(ErrorCode::InvalidArgument);
        }

        if (storage.size == 0xffffffffu) {
            return Result<usize>::Failure(ErrorCode::Overflow);
        }

        if (storage.size == storage.capacity) {
            grown = GrownCapacity(storage.capacity);
        }
    }

    // 잠금 밖: 원소 블록 할당·기본 생성(참조는 null이라 장벽이 필요 없다)·필요하면 새 색인 할당.
    // 생성자·allocator 콜백이 같은 배열을 바꿀 수 있으므로 게시는 다시 잠근 뒤 그때 상태로 한다.
    auto memory = allocator.Allocate(ops.size, ops.alignment, StructRefElementTag);
    if (!memory) {
        return Result<usize>::Failure(memory.Error());
    }

    void* block = memory.Value();
    ops.construct(block, allocator);
    void** fresh = nullptr;
    if (grown != 0) {
        auto table = allocator.Allocate(sizeof(void*) * grown, alignof(void*), StructRefIndexTag);
        if (!table) {
            DestroyStructElement(allocator, ops, block);
            return Result<usize>::Failure(table.Error());
        }

        fresh = static_cast<void**>(table.Value());
    }

    void** release = fresh;
    bool published = false;
    usize position = 0;
    Result<void> result = Result<void>::Success();
    {
        platform::LockGuard lock(gate_);
        result = validateLocked();
        if (result) {
            position = append ? storage.size : index;
            void** target = nullptr;
            if (position > storage.size) {
                result = Result<void>::Failure(ErrorCode::Busy);
            } else if (storage.size == 0xffffffffu) {
                result = Result<void>::Failure(ErrorCode::Overflow);
            } else if (storage.size < storage.capacity) {
                target = storage.blocks;
            } else if (fresh != nullptr && grown > storage.size) {
                target = fresh;
            } else {
                // 새 색인이 없거나(그 사이 용량이 찼다) 모자라다.
                result = Result<void>::Failure(ErrorCode::Busy);
            }

            if (target != nullptr) {
                if (target != storage.blocks) {
                    for (usize at = 0; at < position; ++at) {
                        target[at] = storage.blocks[at];
                    }

                    for (usize at = storage.size; at > position; --at) {
                        target[at] = storage.blocks[at - 1];
                    }

                    release = storage.blocks;
                    storage.blocks = target;
                    storage.capacity = grown;
                } else {
                    for (usize at = storage.size; at > position; --at) {
                        target[at] = target[at - 1];
                    }
                }

                target[position] = block;
                ++storage.size;
                published = true;
            }
        }
    }

    ReleaseContainerMemory(allocator, release);
    if (!published) {
        DestroyStructElement(allocator, ops, block);
        return Result<usize>::Failure(result.Error());
    }

    return Result<usize>::Success(position);
}

Result<void> ObjectRegistry::RemoveStructElement(const ObjectAccessContext& access,
                                                 ObjectHandle owner,
                                                 detail::StructRefArrayStorage& storage,
                                                 usize index) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return valid;
    }

    Allocator& allocator = *storage.allocator;
    const detail::StructElementOps& ops = *storage.element;
    void* block = nullptr;
    {
        platform::LockGuard lock(gate_);
        if (processing_ || closed_) {
            return Result<void>::Failure(closed_ ? ErrorCode::ShuttingDown : ErrorCode::Busy);
        }

        auto checked = CheckContainerOwnerLocked(owner, &storage, sizeof(storage), 0, true);
        if (!checked) {
            return checked;
        }

        if (index >= storage.size) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        // 삭제 장벽은 필요 없다(삽입 장벽과 Sweeping cutoff가 지킨다). 색인에서 먼저 떼므로 소멸
        // 중 재진입이 이 원소를 다시 건드릴 수 없다.
        block = storage.blocks[index];
        for (usize position = index + 1; position < storage.size; ++position) {
            storage.blocks[position - 1] = storage.blocks[position];
        }

        --storage.size;
    }

    DestroyStructElement(allocator, ops, block);
    return Result<void>::Success();
}

Result<void> ObjectRegistry::ClearStructElements(const ObjectAccessContext& access,
                                                 ObjectHandle owner,
                                                 detail::StructRefArrayStorage& storage) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return valid;
    }

    Allocator& allocator = *storage.allocator;
    const detail::StructElementOps& ops = *storage.element;
    void** blocks = nullptr;
    uint32 size = 0;
    {
        platform::LockGuard lock(gate_);
        if (processing_ || closed_) {
            return Result<void>::Failure(closed_ ? ErrorCode::ShuttingDown : ErrorCode::Busy);
        }

        auto checked = CheckContainerOwnerLocked(owner, &storage, sizeof(storage), 0, true);
        if (!checked) {
            return checked;
        }

        // 색인째 떼어 낸다(잠금 밖에서 소멸할 원소를 담으려고 할당하지 않는다).
        blocks = storage.blocks;
        size = storage.size;
        storage.blocks = nullptr;
        storage.size = 0;
        storage.capacity = 0;
    }

    for (uint32 remaining = size; remaining != 0; --remaining) {
        DestroyStructElement(allocator, ops, blocks[remaining - 1]);
    }

    ReleaseContainerMemory(allocator, blocks);
    return Result<void>::Success();
}

Result<void> ObjectRegistry::AssignStructElements(const ObjectAccessContext& access,
                                                  ObjectHandle owner,
                                                  detail::StructRefArrayStorage& storage,
                                                  usize count, uint64 transaction) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return valid;
    }

    if (count > 0xffffffffu) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    Allocator& allocator = *storage.allocator;
    const detail::StructElementOps& ops = *storage.element;
    {
        platform::LockGuard lock(gate_);
        if (closed_ || processing_ || transaction == 0) {
            return Result<void>::Failure(closed_ ? ErrorCode::ShuttingDown : ErrorCode::Busy);
        }

        auto checked =
            CheckContainerOwnerLocked(owner, &storage, sizeof(storage), transaction, false);
        if (!checked) {
            return checked;
        }

        // 블록 할당·원소 생성·옛 원소 소멸은 처리 표시 아래 잠금 밖에서 한다(CreatePending과 같은
        // 규칙 — 그동안의 Registry 재진입은 Busy, Rollback 재진입은 Fatal).
        processing_ = true;
    }

    auto finish = [this]() noexcept {
        platform::LockGuard lock(gate_);
        processing_ = false;
    };
    void** fresh = nullptr;
    usize built = 0;
    auto discard = [&]() noexcept {
        for (usize remaining = built; remaining != 0; --remaining) {
            DestroyStructElement(allocator, ops, fresh[remaining - 1]);
        }

        ReleaseContainerMemory(allocator, fresh);
    };
    if (count != 0) {
        auto table = allocator.Allocate(sizeof(void*) * count, alignof(void*), StructRefIndexTag);
        if (!table) {
            finish();
            return Result<void>::Failure(table.Error());
        }

        fresh = static_cast<void**>(table.Value());
    }

    for (; built < count; ++built) {
        auto memory = allocator.Allocate(ops.size, ops.alignment, StructRefElementTag);
        if (!memory) {
            discard();
            finish();
            return Result<void>::Failure(memory.Error());
        }

        ops.construct(memory.Value(), allocator);
        fresh[built] = memory.Value();
    }

    void** previous = nullptr;
    uint32 previousSize = 0;
    Result<void> result = Result<void>::Success();
    {
        platform::LockGuard lock(gate_);
        result = CheckContainerOwnerLocked(owner, &storage, sizeof(storage), transaction, false);
        if (result) {
            previous = storage.blocks;
            previousSize = storage.size;
            storage.blocks = fresh;
            storage.size = static_cast<uint32>(count);
            storage.capacity = static_cast<uint32>(count);
        }
    }

    if (!result) {
        discard();
        finish();
        return result;
    }

    // 옛 원소는 게시 뒤 역순으로 소멸한다.
    for (uint32 remaining = previousSize; remaining != 0; --remaining) {
        DestroyStructElement(allocator, ops, previous[remaining - 1]);
    }

    ReleaseContainerMemory(allocator, previous);
    finish();
    return Result<void>::Success();
}

Result<EncodePin> ObjectRegistry::PinForEncode(const ObjectAccessContext& access,
                                               ObjectHandle owner) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return Result<EncodePin>::Failure(valid.Error());
    }

    platform::LockGuard lock(gate_);
    if (closed_ || processing_) {
        return Result<EncodePin>::Failure(closed_ ? ErrorCode::ShuttingDown : ErrorCode::Busy);
    }

    Slot* slot = FindSlot(owner);
    auto live = CheckLive(slot, nullptr);
    if (!live) {
        return Result<EncodePin>::Failure(live.Error());
    }

    if (slot->encodePins == 0xffffffffu) {
        return Result<EncodePin>::Failure(ErrorCode::Overflow);
    }

    ++slot->encodePins;
    return Result<EncodePin>::Success(EncodePin(this, owner));
}

Result<void> ObjectRegistry::RequestDestroy(const ObjectAccessContext& access,
                                            ObjectHandle handle) noexcept {
    return RequestDestroyChecked(access, handle, nullptr, false);
}

Result<ObjectAccessContext> ObjectRegistry::ScopedAccess() const noexcept {
    if (platform::CurrentThreadId() != ownerThread_) {
        return Result<ObjectAccessContext>::Failure(ErrorCode::WrongThread);
    }

    auto current = ObjectAccessScope::Current();
    if (current) {
        // 명시된 실행 문맥은 항상 우선한다. 다른 World라면 CheckAccess가 거부한다.
        return current;
    }

    // 실행 문맥이 없으면 객체가 속한 Registry를 사용한다. 이때 호출자의 World는 식별하지
    // 못한다. 등록된 Borrow·슬롯·스레드 검사는 이후에도 그대로 수행한다.
    if (current.Error() == ErrorCode::WrongContext && !options_.requireLifecycleScope) {
        return Result<ObjectAccessContext>::Success(OwnerAccess());
    }

    return current;
}

Result<void> ObjectRegistry::DestroySelf(ObjectHandle handle, const Object* object) noexcept {
    auto access = ScopedAccess();
    if (!access) {
        return Result<void>::Failure(access.Error());
    }

    return RequestDestroyChecked(access.Value(), handle, object, true);
}

Result<void> ObjectRegistry::RequestDestroyChecked(const ObjectAccessContext& access,
                                                   ObjectHandle handle, const Object* object,
                                                   bool requireBorrow) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return valid;
    }

    platform::LockGuard lock(gate_);
    if (closed_ || processing_) {
        return Result<void>::Failure(closed_ ? ErrorCode::ShuttingDown : ErrorCode::Busy);
    }

    Slot* slot = FindSlot(handle);
    if (requireBorrow) {
        if (!slot || slot->state == SlotState::Reserved || slot->state == SlotState::Free ||
            slot->state == SlotState::Exhausted || (object && slot->object != object)) {
            return Result<void>::Failure(ErrorCode::StaleHandle);
        }

        if (slot->borrows == 0) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    if (slot && (slot->state == SlotState::DestroyRequested || slot->state == SlotState::Retired)) {
        return Result<void>::Success();
    }

    auto live = CheckLive(slot, nullptr);
    if (!live) {
        return live;
    }

    slot->state = SlotState::DestroyRequested;
    return Result<void>::Success();
}

Result<ObjectLifecycle> ObjectRegistry::InspectLifecycle(const ObjectAccessContext& access,
                                                         ObjectHandle handle) const noexcept {
    return InspectChecked(access, handle, nullptr, false);
}

Result<ObjectLifecycle> ObjectRegistry::InspectSelf(ObjectHandle handle,
                                                    const Object* object) const noexcept {
    auto access = ScopedAccess();
    if (!access) {
        return Result<ObjectLifecycle>::Failure(access.Error());
    }

    return InspectChecked(access.Value(), handle, object, true);
}

Result<ObjectLifecycle> ObjectRegistry::InspectChecked(const ObjectAccessContext& access,
                                                       ObjectHandle handle, const Object* object,
                                                       bool requireBorrow) const noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return Result<ObjectLifecycle>::Failure(valid.Error());
    }

    platform::LockGuard lock(gate_);
    if (closed_ || processing_) {
        return Result<ObjectLifecycle>::Failure(closed_ ? ErrorCode::ShuttingDown
                                                        : ErrorCode::Busy);
    }

    Slot* slot = FindSlot(handle);
    if (!slot || slot->state == SlotState::Free || slot->state == SlotState::Reserved ||
        slot->state == SlotState::Exhausted) {
        return Result<ObjectLifecycle>::Failure(ErrorCode::StaleHandle);
    }

    if (object && slot->object != object) {
        return Result<ObjectLifecycle>::Failure(ErrorCode::StaleHandle);
    }

    if (requireBorrow && slot->borrows == 0) {
        return Result<ObjectLifecycle>::Failure(ErrorCode::InvalidArgument);
    }

    if (IsCutoff(slot)) {
        return Result<ObjectLifecycle>::Success(ObjectLifecycle::Unreachable);
    }

    switch (slot->state) {
    case SlotState::Alive:
        return Result<ObjectLifecycle>::Success(ObjectLifecycle::Alive);
    case SlotState::DestroyRequested:
        return Result<ObjectLifecycle>::Success(ObjectLifecycle::DestroyRequested);
    case SlotState::Retired:
        return Result<ObjectLifecycle>::Success(ObjectLifecycle::Retired);
    case SlotState::Reclaiming:
        return Result<ObjectLifecycle>::Failure(ErrorCode::Busy);
    default:
        return Result<ObjectLifecycle>::Failure(ErrorCode::StaleHandle);
    }
}

Result<BackendLease> ObjectRegistry::AcquireBackendLease(const ObjectAccessContext& access,
                                                         ObjectHandle handle) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return Result<BackendLease>::Failure(valid.Error());
    }

    platform::LockGuard lock(gate_);
    if (closed_ || processing_) {
        return Result<BackendLease>::Failure(closed_ ? ErrorCode::ShuttingDown : ErrorCode::Busy);
    }

    Slot* slot = FindSlot(handle);
    auto live = CheckLive(slot, nullptr);
    if (!live) {
        return Result<BackendLease>::Failure(live.Error());
    }

    if (slot->backendHolds == 0xffffffffu) {
        return Result<BackendLease>::Failure(ErrorCode::Overflow);
    }

    ++slot->backendHolds;
    return Result<BackendLease>::Success(BackendLease(this, handle));
}

Result<TypeLease> ObjectRegistry::AcquireTypeSelf(ObjectHandle handle,
                                                  const Object* object) noexcept {
    auto access = ScopedAccess();
    if (!access) {
        return Result<TypeLease>::Failure(access.Error());
    }

    auto valid = CheckAccess(access.Value());
    if (!valid) {
        return Result<TypeLease>::Failure(valid.Error());
    }

    platform::LockGuard lock(gate_);
    if (closed_ || processing_) {
        return Result<TypeLease>::Failure(closed_ ? ErrorCode::ShuttingDown : ErrorCode::Busy);
    }

    Slot* slot = FindSlot(handle);
    if (!slot || slot->state == SlotState::Free || slot->state == SlotState::Reserved ||
        slot->state == SlotState::Exhausted || slot->object != object) {
        return Result<TypeLease>::Failure(ErrorCode::StaleHandle);
    }

    if (slot->borrows == 0) {
        return Result<TypeLease>::Failure(ErrorCode::InvalidArgument);
    }

    auto live = CheckLive(slot, nullptr);
    if (!live) {
        return Result<TypeLease>::Failure(live.Error());
    }

    return slot->type.Clone();
}

Result<TypeLease> ObjectRegistry::AcquireType(const ObjectAccessContext& access,
                                              ObjectHandle handle) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return Result<TypeLease>::Failure(valid.Error());
    }

    platform::LockGuard lock(gate_);
    if (closed_ || processing_) {
        return Result<TypeLease>::Failure(closed_ ? ErrorCode::ShuttingDown : ErrorCode::Busy);
    }

    Slot* slot = FindSlot(handle);
    auto live = CheckLive(slot, nullptr);
    if (!live) {
        return Result<TypeLease>::Failure(live.Error());
    }

    return slot->type.Clone();
}

Result<TypeLease> ObjectRegistry::AcquireRegisteredType(const ObjectAccessContext& access,
                                                        TypeId type) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return Result<TypeLease>::Failure(valid.Error());
    }

    platform::LockGuard lock(gate_);
    if (closed_ || processing_) {
        return Result<TypeLease>::Failure(closed_ ? ErrorCode::ShuttingDown : ErrorCode::Busy);
    }

    return types_.Acquire(type);
}

Result<uint64> ObjectRegistry::BeginConstruction(const ObjectAccessContext& access) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return Result<uint64>::Failure(valid.Error());
    }

    platform::LockGuard lock(gate_);
    if (closed_ || processing_) {
        return Result<uint64>::Failure(closed_ ? ErrorCode::ShuttingDown : ErrorCode::Busy);
    }

    if (options_.maxSlots == 0 || options_.maxGeneration == 0) {
        return Result<uint64>::Failure(ErrorCode::InvalidArgument);
    }

    if (nextTransaction_ == 0 || constructions_ == 0xffffffffu) {
        return Result<uint64>::Failure(ErrorCode::Overflow);
    }

    ++constructions_;
    return Result<uint64>::Success(nextTransaction_++);
}

Result<ObjectHandle> ObjectRegistry::CreatePending(const ObjectAccessContext& access, TypeId type,
                                                   uint64 transaction, const void* nativeType,
                                                   ObjectHandle outer, StringView name) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return Result<ObjectHandle>::Failure(valid.Error());
    }

    {
        platform::LockGuard lock(gate_);
        if (closed_ || processing_ || transaction == 0) {
            return Result<ObjectHandle>::Failure(closed_ ? ErrorCode::ShuttingDown
                                                         : ErrorCode::Busy);
        }

        // 이후 잠금 밖 할당·사용자 콜백 동안 Registry 변경 재진입은 Busy다. 그래서 아래에서 확인한
        // Outer 상태·이름 색인은 게시할 때까지 바뀌지 않는다.
        processing_ = true;
    }

    // 실패하면 잠금 밖에서 반환할 할당이다. 게시에 성공하면 슬롯·색인이 소유한다.
    NameRecord* record = nullptr;
    NameEntry* grown = nullptr;
    void* storage = nullptr;
    auto fail = [&](ErrorCode error) noexcept -> Result<ObjectHandle> {
        ReleaseRegistryMemory(allocator_, storage);
        ReleaseRegistryMemory(allocator_, grown);
        ReleaseRegistryMemory(allocator_, record);
        platform::LockGuard lock(gate_);
        processing_ = false;
        return Result<ObjectHandle>::Failure(error);
    };
    // 3. 타입
    auto typeLease = types_.Acquire(type);
    if (!typeLease) {
        return fail(typeLease.Error());
    }

    if (nativeType && !typeLease.Value().IsNativeA(nativeType)) {
        return fail(ErrorCode::TypeMismatch);
    }

    const auto& ops = typeLease.Value().Ops();
    // 4. Abstract 선언은 C++가 생성 가능한 타입이어도 직접 생성할 수 없다.
    // 슬롯/메모리를 예약하기 전에 검사하여 실패가 관찰 가능한 부작용을 남기지 않는다.
    if (HasFlag(typeLease.Value().Schema().flags, TypeFlags::Abstract) ||
        ops.construct == nullptr) {
        return fail(ErrorCode::Unsupported);
    }

    // 5. Outer 상태: 생존(Alive, cutoff 아님) 또는 이 트랜잭션의 공개 전 객체다. 다른 Registry의
    // handle은 FindSlot이 domain 불일치로 찾지 않는다.
    const bool hasOuter = !outer.IsNull();
    const ObjectHandle outerKey = hasOuter ? outer : ObjectHandle{};
    Slot* parent = nullptr;
    OuterSummary summary;
    bool outerValid = true;
    if (hasOuter) {
        platform::LockGuard lock(gate_);
        parent = FindSlot(outer);
        outerValid = parent != nullptr && !IsCutoff(parent) &&
                     (parent->state == SlotState::Alive ||
                      (parent->state == SlotState::Reserved && parent->transaction == transaction &&
                       parent->object != nullptr));
        if (outerValid) {
            // hasNamedInners는 대표 자식(빈 이름) 생성에만 쓰인다. 이름 있는 생성은 고리를 훑지
            // 않는다 (Sweeping 중 cutoff white 형제가 많아도 잠금 시간이 늘지 않게).
            summary = DescribeSlotLocked(*parent, name.IsEmpty());
        }
    }

    if (!outerValid) {
        return fail(ErrorCode::StaleHandle);
    }

    // 6–9. WorldOnly 정책·이름 형식·패키지·대표 자식·경로 길이(NFC 판정 scratch는 잠금 밖).
    const OuterSummary* outerSummary = hasOuter ? &summary : nullptr;
    auto identity = detail::CheckNewObjectIdentity(options_.policy, typeLease.Value(), outerSummary,
                                                   name, allocator_);
    if (!identity) {
        return fail(identity.Error());
    }

    const usize pathBytes = ChildPathBytes(outerSummary, name.ByteCount());
    // 10·11. 이름 있는 객체와 대표 자식은 색인에 넣는다(Outer 없는 이름 없는 객체는 넣지 않는다).
    const bool named = hasOuter || !name.IsEmpty();
    uint64 hash = 0;
    uint32 grownCapacity = 0;
    if (named) {
        StringBuilder<128> key(allocator_);
        if (auto made = TryMakeNameComponentKey(name, key); !made) {
            return fail(made.Error());
        }

        hash = NameKeyHash(outerKey, key.View());
        const usize bytes = sizeof(NameRecord) + name.ByteCount() + key.ByteCount();
        auto allocated = allocator_.Allocate(bytes, alignof(NameRecord), NameRecordTag);
        if (!allocated) {
            return fail(allocated.Error());
        }

        record = ::new (allocated.Value()) NameRecord();
        record->keyHash = hash;
        record->nameBytes = static_cast<uint16>(name.ByteCount());
        record->keyBytes = static_cast<uint16>(key.ByteCount());
        char* data = reinterpret_cast<char*>(record + 1);
        platform::MemoryCopy(data, name.Data(), name.ByteCount());
        platform::MemoryCopy(data + name.ByteCount(), key.View().Data(), key.ByteCount());
        Result<uint32> growth = Result<uint32>::Success(0);
        {
            platform::LockGuard lock(gate_);
            growth = NameIndexGrowthLocked();
        }

        if (!growth) {
            return fail(growth.Error());
        }

        if (growth.Value() != 0) {
            // 새 표(같은 크기 rebuild 또는 두 배)를 잠금 밖에서 받고 게시할 때 잠금 안에서 옮긴다.
            const uint64 tableBytes = uint64(growth.Value()) * sizeof(NameEntry);
            if (tableBytes > static_cast<usize>(-1)) {
                return fail(ErrorCode::CapacityExceeded);
            }

            auto table = allocator_.Allocate(static_cast<usize>(tableBytes), alignof(NameEntry),
                                             NameTableTag);
            if (!table) {
                return fail(table.Error());
            }

            grown = static_cast<NameEntry*>(table.Value());
            platform::MemoryZero(grown, static_cast<usize>(tableBytes));
            grownCapacity = growth.Value();
        }

        bool taken = false;
        {
            platform::LockGuard lock(gate_);
            taken = FindNamedLocked(outerKey, hash, record->Key(), false) != nullptr;
        }

        if (taken) {
            return fail(ErrorCode::AlreadyExists);
        }
    }

    // 12. 객체 할당·슬롯.
    if (gcPhase_ == GcPhase::Marking) {
        // mutator가 새 객체를 만드는 지점에서만 작업 큐를 확장한다. gate/Trace 안의 barrier는
        // 할당하지 않는다. Marking 동안 공개된 객체는 회수하지 않아 슬롯 수가 큐 상한이 된다.
        uint64 capacity = (uint64(chunkCount_) + 1) * SlotsPerChunk;
        if (capacity > options_.maxSlots) {
            capacity = options_.maxSlots;
        }

        auto reserve = gcWork_.TryReserve(static_cast<usize>(capacity));
        if (!reserve) {
            return fail(reserve.Error());
        }
    }

    auto memory = allocator_.Allocate(ops.size, ops.alignment, 0x4f424a45u);
    if (!memory) {
        return fail(memory.Error());
    }

    storage = memory.Value();
    Slot* freeSlot = nullptr;
    {
        platform::LockGuard lock(gate_);
        for (Chunk* chunk = chunks_; chunk && !freeSlot; chunk = chunk->next) {
            for (auto& slot : chunk->slots) {
                if (slot.index < options_.maxSlots && slot.state == SlotState::Free) {
                    freeSlot = &slot;
                    break;
                }
            }
        }
    }

    if (!freeSlot) {
        if (uint64(chunkCount_) * SlotsPerChunk >= options_.maxSlots) {
            return fail(ErrorCode::CapacityExceeded);
        }

        auto block = allocator_.Allocate(sizeof(Chunk), alignof(Chunk), 0x43484e4bu);
        if (!block) {
            return fail(block.Error());
        }

        Chunk* chunk = ::new (block.Value()) Chunk();
        for (uint32 index = 0; index < SlotsPerChunk; ++index) {
            const uint64 absolute = uint64(chunkCount_) * SlotsPerChunk + index;
            chunk->slots[index].index = static_cast<uint32>(absolute);
            if (absolute >= options_.maxSlots) {
                chunk->slots[index].state = SlotState::Exhausted;
            }
        }

        {
            platform::LockGuard lock(gate_);
            chunk->next = chunks_;
            chunks_ = chunk;
            ++chunkCount_;
        }

        freeSlot = &chunk->slots[0];
    }

    ObjectHandle handle{domain_, freeSlot->index, freeSlot->generation};
    NameEntry* previousTable = nullptr;
    {
        // 상태·이름 색인·부모 고리를 한 잠금 안에서 게시한다(공개 전 객체도 이름을 점유한다).
        platform::LockGuard lock(gate_);
        freeSlot->state = SlotState::Reserved;
        freeSlot->storage = memory.Value();
        freeSlot->transaction = transaction;
        freeSlot->stableType = type;
        freeSlot->type = Move(typeLease.Value());
        freeSlot->outer = outerKey;
        freeSlot->parent = parent;
        freeSlot->outermost = parent != nullptr ? parent->outermost : freeSlot;
        freeSlot->pathBytes = static_cast<uint16>(pathBytes);
        freeSlot->name = record;
        if (named) {
            if (grown != nullptr) {
                previousTable = RehashNamesLocked(grown, grownCapacity);
            }

            InsertNameLocked(*freeSlot, hash, record->Key());
        }

        if (parent != nullptr) {
            LinkChildLocked(*parent, *freeSlot);
        }
    }

    record = nullptr;
    grown = nullptr;
    storage = nullptr;
    ReleaseRegistryMemory(allocator_, previousTable);
    Object* object = freeSlot->type.Ops().construct(memory.Value(), allocator_);
    if (!object) {
        platform::Fatal("A noexcept object constructor must return its object");
    }

    object->registry_ = this;
    object->handle_ = handle;
    {
        platform::LockGuard lock(gate_);
        freeSlot->object = object;
        processing_ = false;
    }

    return Result<ObjectHandle>::Success(handle);
}

Result<void*> ObjectRegistry::GetPending(const ObjectAccessContext& access, ObjectHandle handle,
                                         uint64 transaction, const void* nativeType) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return Result<void*>::Failure(valid.Error());
    }

    platform::LockGuard lock(gate_);
    Slot* slot = FindSlot(handle);
    if (processing_ || !slot || slot->state != SlotState::Reserved ||
        slot->transaction != transaction || !slot->object) {
        return Result<void*>::Failure(ErrorCode::StaleHandle);
    }

    if (nativeType && !slot->type.IsNativeA(nativeType)) {
        return Result<void*>::Failure(ErrorCode::TypeMismatch);
    }

    return Result<void*>::Success(static_cast<void*>(slot->object));
}

void ObjectRegistry::ValidateStagedEdge(void* pointer, ObjectHandle handle) noexcept {
    auto& state = *static_cast<MarkState*>(pointer);
    if (handle.IsNull() || state.error != ErrorCode::None) {
        return;
    }

    platform::LockGuard lock(state.registry->gate_);
    Slot* slot = state.registry->FindSlot(handle);
    if (!slot || state.registry->IsCutoff(slot) ||
        !(slot->state == SlotState::Alive ||
          (slot->state == SlotState::Reserved && slot->transaction == state.transaction))) {
        state.error = ErrorCode::StaleHandle;
    }
}

Result<void> ObjectRegistry::Commit(const ObjectAccessContext& access, uint64 transaction,
                                    Span<const ObjectHandle> staged, Span<const ObjectHandle> roots,
                                    Span<RootRef<Object>> out) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return valid;
    }

    {
        platform::LockGuard lock(gate_);
        if (auto open = OpenState(closed_, processing_); !open) {
            return open;
        }

        for (usize index = 0; index < staged.Size(); ++index) {
            Slot* slot = FindSlot(staged[index]);
            if (!slot || slot->state != SlotState::Reserved || slot->transaction != transaction ||
                !slot->object) {
                return Result<void>::Failure(ErrorCode::StaleHandle);
            }
        }

        // root는 이 트랜잭션의 객체여야 한다. 공개 전 객체의 root 계수는 0이므로(Reserved는 Pin할
        // 수 없다) 중복 root도 roots 크기가 uint32 안이면 넘치지 않는다.
        for (usize index = 0; index < roots.Size(); ++index) {
            Slot* slot = FindSlot(roots[index]);
            if (!slot || slot->state != SlotState::Reserved || slot->transaction != transaction ||
                !slot->object) {
                return Result<void>::Failure(ErrorCode::StaleHandle);
            }
        }

        if (roots.Size() > 0xffffffffu) {
            return Result<void>::Failure(ErrorCode::Overflow);
        }

        processing_ = true;
    }

    // staged 간선과 Outer를 잠금 밖에서 검증한다(처리 표시 아래라 간선·Outer가 바뀌지 않는다).
    // Outer는 대상처럼 생존(Alive) 또는 이 트랜잭션 객체이고 cutoff white가 아니어야 한다. A0-Ob는
    // 같은 trace 순서(staged 저널 순 × trace 순)로 관찰자 쌍을 집계한다(집계 맵 삽입은 gate 밖).
    MarkState state{this, nullptr, transaction};
    TraceVisitor visitor(&state, &ObjectRegistry::ValidateStagedEdge);
    for (usize index = 0; index < staged.Size() && state.error == ErrorCode::None; ++index) {
        Slot* slot = FindSlot(staged[index]);
        slot->type.Ops().trace(*slot->object, visitor);
        visitor.Visit(slot->outer);
    }

    // A0-Ob: 여기서 관찰자 공간을 예약한다(실패하면 아래 공개 없이 비공개 상태로 돌아간다).
    {
        platform::LockGuard lock(gate_);
        processing_ = false;
        if (state.error != ErrorCode::None) {
            return Result<void>::Failure(state.error);
        }

        // 이름 색인 항목은 Create 때 이미 들어 있으므로 상태만 바꾼다(조회가 Alive부터 보게 된다).
        for (usize index = 0; index < staged.Size(); ++index) {
            Slot* slot = FindSlot(staged[index]);
            slot->state = SlotState::Alive;
            slot->transaction = 0;
            if (gcPhase_ == GcPhase::Marking) {
                if (ShadeLocked(staged[index]) != ErrorCode::None) {
                    platform::Fatal("Preallocated newborn mark queue exhausted");
                }

                ++cycleGc_.examined;
            } else if (gcPhase_ == GcPhase::Sweeping) {
                // cutoff 이후 태어난 객체는 이전 cohort의 white 후보가 아니다. Commit 검증이
                // 기존 white 대상·Outer 연결을 거부하므로 새 객체를 이번 cycle에서 trace할 필요가
                // 없다.
                slot->mark = markEpoch_;
            }
        }

        // root 증가는 공개와 같은 잠금 안이다(A0-Ob 관찰자 기록도 여기다).
        for (usize index = 0; index < roots.Size(); ++index) {
            ++FindSlot(roots[index])->roots;
        }

        --constructions_;
    }

    for (usize index = 0; index < roots.Size(); ++index) {
        out[index] = RootRef<Object>(this, roots[index]);
    }

    return Result<void>::Success();
}

void ObjectRegistry::Rollback(uint64 transaction, Span<const ObjectHandle> staged) noexcept {
    if (platform::CurrentThreadId() != ownerThread_) {
        platform::Fatal("Construction rollback on another thread");
    }

    {
        platform::LockGuard lock(gate_);
        if (processing_ || constructions_ == 0) {
            platform::Fatal("Construction rollback while executing a registry callback");
        }

        processing_ = true;
    }

    // 생성 저널을 역순으로 처리해 나중에 만든 객체를 먼저 정리한다.
    // 소멸자는 Registry 잠금 밖에서 호출하되 processing_으로 변경 재진입을 막는다.
    for (usize remaining = staged.Size(); remaining != 0; --remaining) {
        {
            platform::LockGuard lock(gate_);
            Slot* slot = FindSlot(staged[remaining - 1]);
            if (!slot || slot->state != SlotState::Reserved || slot->transaction != transaction) {
                platform::Fatal("Corrupt construction journal");
            }

            // 역순이라 같은 트랜잭션 자식이 먼저 떨어지므로 부모는 자식이 없을 때 회수된다.
            slot->state = SlotState::Retired;
            DetachIdentityLocked(*slot);
        }

        ReclaimEligible();
    }

    {
        platform::LockGuard lock(gate_);
        --constructions_;
        processing_ = false;
    }
}

void ObjectRegistry::MarkEdge(void* pointer, ObjectHandle handle) noexcept {
    auto& state = *static_cast<MarkState*>(pointer);
    if (handle.IsNull()) {
        return;
    }

    platform::LockGuard lock(state.registry->gate_);
    if (state.error != ErrorCode::None) {
        return;
    }

    Slot* slot = state.registry->FindSlot(handle);
    if (!slot || !(slot->state == SlotState::Alive || slot->state == SlotState::DestroyRequested)) {
        state.error = ErrorCode::StaleHandle;
        return;
    }

    if (slot->mark == state.registry->markEpoch_) {
        return;
    }

    auto pushed = state.work->TryPushBack(handle);
    if (!pushed) {
        state.error = pushed.Error();
        return;
    }

    slot->mark = state.registry->markEpoch_;
}

bool ObjectRegistry::IsCutoff(const Slot* slot) const noexcept {
    return slot && gcPhase_ == GcPhase::Sweeping &&
           (slot->state == SlotState::Alive || slot->state == SlotState::DestroyRequested) &&
           slot->mark != markEpoch_;
}

ErrorCode ObjectRegistry::ShadeLocked(ObjectHandle handle) noexcept {
    if (gcPhase_ != GcPhase::Marking || handle.IsNull()) {
        return ErrorCode::None;
    }

    Slot* slot = FindSlot(handle);
    if (!slot || !(slot->state == SlotState::Alive || slot->state == SlotState::DestroyRequested)) {
        return ErrorCode::StaleHandle;
    }

    if (slot->mark == markEpoch_) {
        return ErrorCode::None;
    }

    // Reserve는 Begin/CreatePending에서 gate 밖에 수행한다. barrier가 allocator 콜백을 실행하지
    // 않도록 capacity를 먼저 검사하며 불변식 위반은 실패로 반환한다.
    if (gcWork_.Size() == gcWork_.Capacity()) {
        return ErrorCode::CapacityExceeded;
    }

    auto pushed = gcWork_.TryPushBack(handle);
    if (!pushed) {
        return pushed.Error();
    }

    slot->mark = markEpoch_;
    return ErrorCode::None;
}

GcPhase ObjectRegistry::CollectionPhase() const noexcept {
    platform::LockGuard lock(gate_);
    return gcPhase_;
}

Result<void> ObjectRegistry::BeginCollection(const ObjectAccessContext& access) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return valid;
    }

    const uint64 started = platform::MonotonicNanoseconds();
    usize capacity;
    {
        platform::LockGuard lock(gate_);
        if (processing_ || constructions_ || totalBorrows_ || gcPhase_ != GcPhase::Idle) {
            return Result<void>::Failure(ErrorCode::Busy);
        }

        processing_ = true;
        capacity = static_cast<usize>(uint64(chunkCount_) * SlotsPerChunk);
    }

    gcWork_.Clear();
    auto reserved = gcWork_.TryReserve(capacity);
    platform::LockGuard lock(gate_);
    processing_ = false;
    if (!reserved) {
        return reserved;
    }

    if (++markEpoch_ == 0) {
        markEpoch_ = 1;
        for (Chunk* chunk = chunks_; chunk; chunk = chunk->next) {
            for (auto& slot : chunk->slots) {
                slot.mark = 0;
            }
        }
    }

    gcPhase_ = GcPhase::Marking;
    gcCursor_ = 0;
    cycleGc_ = {};
    sweepChunk_ = nullptr;
    sweepOffset_ = 0;
    for (Chunk* chunk = chunks_; chunk; chunk = chunk->next) {
        for (auto& slot : chunk->slots) {
            if (slot.state == SlotState::Alive || slot.state == SlotState::DestroyRequested) {
                ++cycleGc_.examined;
                if (slot.roots != 0 &&
                    ShadeLocked({domain_, slot.index, slot.generation}) != ErrorCode::None) {
                    platform::Fatal("Preallocated root snapshot queue exhausted");
                }
            }
        }
    }

    cycleGc_.durationNanoseconds = platform::MonotonicNanoseconds() - started;
    return Result<void>::Success();
}

Result<void> ObjectRegistry::CancelCollection(const ObjectAccessContext& access) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return valid;
    }

    platform::LockGuard lock(gate_);
    if (processing_ || gcPhase_ == GcPhase::Sweeping || gcPhase_ == GcPhase::Reclaiming) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    gcPhase_ = GcPhase::Idle;
    gcWork_.Clear();
    gcCursor_ = 0;
    return Result<void>::Success();
}

bool ObjectRegistry::IsReclaimable(const Slot& slot) const noexcept {
    // 고리에 남은 자식이 있으면 부모 슬롯을 재사용하지 않는다. white 부모가 자식보다 먼저
    // Retire되면 자식들이 같은 sweep에서 스스로 떨어질 때까지 기다린다(그 사이
    // DrainRetired·rollback 회수 포함).
    return slot.state == SlotState::Retired && slot.roots == 0 && slot.borrows == 0 &&
           slot.backendHolds == 0 && slot.encodePins == 0 && slot.firstChild == nullptr;
}

bool ObjectRegistry::HasReclaimableLocked() const noexcept {
    for (Chunk* chunk = chunks_; chunk; chunk = chunk->next) {
        for (const auto& slot : chunk->slots) {
            if (IsReclaimable(slot)) {
                return true;
            }
        }
    }

    return false;
}

Result<GcProgress> ObjectRegistry::StepCollection(const ObjectAccessContext& access,
                                                  GcBudget budget,
                                                  WorkerScheduler* scheduler) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return Result<GcProgress>::Failure(valid.Error());
    }

    if (budget.maxObjects == 0) {
        return Result<GcProgress>::Failure(ErrorCode::InvalidArgument);
    }

    if (scheduler && scheduler->IsCurrentWorker()) {
        return Result<GcProgress>::Failure(ErrorCode::WrongThread);
    }

    const uint64 started = platform::MonotonicNanoseconds();
    uint64 deadline = 0;
    if (budget.maxNanoseconds != 0) {
        const uint64 maximum = static_cast<uint64>(-1);
        deadline =
            budget.maxNanoseconds > maximum - started ? maximum : started + budget.maxNanoseconds;
    }

    auto inBudget = [deadline]() noexcept {
        return deadline == 0 || platform::MonotonicNanoseconds() < deadline;
    };
    {
        platform::LockGuard lock(gate_);
        if (processing_ || constructions_ || totalBorrows_) {
            return Result<GcProgress>::Failure(ErrorCode::Busy);
        }

        if (gcPhase_ == GcPhase::Idle) {
            return Result<GcProgress>::Failure(ErrorCode::InvalidArgument);
        }

        processing_ = true;
    }

    uint32 processed = 0;
    MarkState state{this, &gcWork_};
    auto setError = [this, &state](ErrorCode error) noexcept {
        platform::LockGuard lock(gate_);
        if (state.error == ErrorCode::None) {
            state.error = error;
        }
    };
    auto hasError = [this, &state]() noexcept {
        platform::LockGuard lock(gate_);
        return state.error != ErrorCode::None;
    };
    while (gcPhase_ == GcPhase::Marking && processed < budget.maxObjects && inBudget() &&
           !hasError()) {
        ObjectHandle next;
        bool parallel = false;
        {
            platform::LockGuard lock(gate_);
            if (gcCursor_ == gcWork_.Size()) {
                // 모든 barrier가 같은 gate에서 게시되므로 빈 큐 판정과 cutoff는 하나의 선형화다.
                // 아직 slot을 Retired로 바꾸기 전에도 white 객체의 Resolve/Pin/연결을 차단한다.
                cycleGc_.marked = static_cast<uint32>(gcWork_.Size());
                gcPhase_ = GcPhase::Sweeping;
                sweepChunk_ = chunks_;
                sweepOffset_ = 0;
                break;
            }

            next = gcWork_[gcCursor_];
            parallel = scheduler && FindSlot(next)->type.Ops().parallelTraceSafe;
        }

        if (!parallel) {
            ++gcCursor_;
            TraceVisitor visitor(&state, &ObjectRegistry::MarkEdge);
            Slot* slot = FindSlot(next);
            slot->type.Ops().trace(*slot->object, visitor);
            // inner → outer는 Registry가 방문하는 강한 간선이다(gate_ 없이 — slice 동안 outer는
            // 불변이고 MarkEdge가 gate_를 잡는다).
            visitor.Visit(slot->outer);
            ++processed;
            continue;
        }

        // 한 batch는 최대 64개다. 각 task의 완료는 capture 정리 뒤 관측하고 모두 회수한 뒤에만
        // stack state/visitor를 없앤다. 미선언 Trace는 이 batch와 겹쳐 실행하지 않는다.
        WorkerTaskHandle tasks[64];
        uint32 submitted = 0;
        while (submitted < 64 && processed < budget.maxObjects && inBudget() && !hasError()) {
            ObjectHandle handle;
            {
                platform::LockGuard lock(gate_);
                if (gcCursor_ == gcWork_.Size()) {
                    break;
                }

                handle = gcWork_[gcCursor_];
                if (!FindSlot(handle)->type.Ops().parallelTraceSafe) {
                    break;
                }

                ++gcCursor_;
            }

            auto body = Function<void(WorkerTaskContext&)>::TryCreate(
                allocator_, [this, &state, handle](WorkerTaskContext&) noexcept {
                    TraceVisitor visitor(&state, &ObjectRegistry::MarkEdge);
                    Slot* slot = FindSlot(handle);
                    slot->type.Ops().trace(*slot->object, visitor);
                    // outer는 공개 전에 정해져 바뀌지 않으므로 worker가 읽어도 경합이 없다.
                    visitor.Visit(slot->outer);
                });
            if (!body) {
                setError(body.Error());
                break;
            }

            auto accepted = scheduler->Submit(Move(body.Value()));
            if (!accepted) {
                setError(accepted.Error());
                break;
            }

            tasks[submitted++] = Move(accepted.Value());
            ++processed;
        }

        for (uint32 index = 0; index < submitted; ++index) {
            auto waited = tasks[index].Wait();
            if (!waited || waited.Value() != platform::WaitResult::Signaled) {
                // stack에 남은 trace context를 사용하는 worker를 버리고 복귀할 수 없다.
                platform::Fatal("Cannot drain a GC marker task");
            }

            auto status = tasks[index].Status();
            if (!status) {
                setError(status.Error());
            } else if (status.Value().state != WorkerTaskState::Succeeded) {
                setError(status.Value().error == ErrorCode::None ? ErrorCode::DependencyFailed
                                                                 : status.Value().error);
            }
        }
    }

    if (hasError()) {
        platform::LockGuard lock(gate_);
        gcPhase_ = GcPhase::Idle;
        gcWork_.Clear();
        gcCursor_ = 0;
        processing_ = false;
        return Result<GcProgress>::Failure(state.error);
    }

    while (gcPhase_ == GcPhase::Sweeping && processed < budget.maxObjects && inBudget()) {
        platform::LockGuard lock(gate_);
        if (!sweepChunk_) {
            gcPhase_ = GcPhase::Reclaiming;
            break;
        }

        Slot& slot = sweepChunk_->slots[sweepOffset_++];
        if ((slot.state == SlotState::Alive || slot.state == SlotState::DestroyRequested) &&
            slot.mark != markEpoch_) {
            slot.state = SlotState::Retired;
            DetachIdentityLocked(slot);
            ++cycleGc_.retired;
        }

        if (sweepOffset_ == SlotsPerChunk) {
            sweepChunk_ = sweepChunk_->next;
            sweepOffset_ = 0;
        }

        ++processed;
    }

    if (gcPhase_ == GcPhase::Reclaiming && processed < budget.maxObjects && inBudget()) {
        const uint32 reclaimed = ReclaimEligible(budget.maxObjects - processed, deadline);
        cycleGc_.reclaimed += reclaimed;
        processed += reclaimed;
    }

    platform::LockGuard lock(gate_);
    cycleGc_.durationNanoseconds += platform::MonotonicNanoseconds() - started;
    if (gcPhase_ == GcPhase::Reclaiming && !HasReclaimableLocked()) {
        gcPhase_ = GcPhase::Idle;
        lastGc_ = cycleGc_;
        gcWork_.Clear();
        gcCursor_ = 0;
    }

    processing_ = false;
    return Result<GcProgress>::Success({gcPhase_, processed, cycleGc_});
}

Result<GcReport> ObjectRegistry::Collect(const ObjectAccessContext& access) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return Result<GcReport>::Failure(valid.Error());
    }

    const uint64 started = platform::MonotonicNanoseconds();
    GcReport report;
    {
        platform::LockGuard lock(gate_);
        if (processing_ || constructions_ || totalBorrows_ || gcPhase_ != GcPhase::Idle) {
            return Result<GcReport>::Failure(ErrorCode::Busy);
        }

        processing_ = true;
        for (Chunk* chunk = chunks_; chunk; chunk = chunk->next) {
            for (auto& slot : chunk->slots) {
                if (slot.state == SlotState::Alive || slot.state == SlotState::DestroyRequested) {
                    ++report.examined;
                }
            }
        }
    }

    // 모든 후보를 담을 용량을 먼저 확보한다. OOM이 발생해도 아직 어떤 객체도 retire하지 않았다.
    // 후보당 한 번만 큐에 넣으므로 이후 순회는 추가 할당 없이 깊은 그래프도 처리한다.
    Array<ObjectHandle> work(allocator_);
    auto reserved = work.TryReserve(report.examined);
    if (!reserved) {
        platform::LockGuard lock(gate_);
        processing_ = false;
        return Result<GcReport>::Failure(reserved.Error());
    }

    {
        platform::LockGuard lock(gate_);
        if (++markEpoch_ == 0) {
            markEpoch_ = 1;
            for (Chunk* chunk = chunks_; chunk; chunk = chunk->next) {
                for (auto& slot : chunk->slots) {
                    slot.mark = 0;
                }
            }
        }

        for (Chunk* chunk = chunks_; chunk; chunk = chunk->next) {
            for (auto& slot : chunk->slots) {
                if ((slot.state == SlotState::Alive || slot.state == SlotState::DestroyRequested) &&
                    slot.roots != 0) {
                    auto pushed = work.TryPushBack({domain_, slot.index, slot.generation});
                    if (!pushed) {
                        platform::Fatal("Preallocated GC work queue exhausted");
                    }

                    slot.mark = markEpoch_;
                }
            }
        }
    }

    MarkState state{this, &work};
    TraceVisitor visitor(&state, &ObjectRegistry::MarkEdge);
    for (usize cursor = 0; cursor < work.Size() && state.error == ErrorCode::None; ++cursor) {
        Slot* slot = FindSlot(work[cursor]);
        slot->type.Ops().trace(*slot->object, visitor);
        visitor.Visit(slot->outer);
    }

    if (state.error != ErrorCode::None) {
        platform::LockGuard lock(gate_);
        processing_ = false;
        return Result<GcReport>::Failure(state.error);
    }

    // 이 지점 이전에는 도달성 판단을 확정하지 않는다. Trace 오류 시 sweep 전체를 생략하고,
    // 다음 시도는 새 epoch를 사용해 부분 표식을 무시한다.
    report.marked = static_cast<uint32>(work.Size());
    {
        platform::LockGuard lock(gate_);
        for (Chunk* chunk = chunks_; chunk; chunk = chunk->next) {
            for (auto& slot : chunk->slots) {
                if ((slot.state == SlotState::Alive || slot.state == SlotState::DestroyRequested) &&
                    slot.mark != markEpoch_) {
                    slot.state = SlotState::Retired;
                    DetachIdentityLocked(slot);
                    ++report.retired;
                }
            }
        }
    }

    report.reclaimed = ReclaimEligible();
    report.durationNanoseconds = platform::MonotonicNanoseconds() - started;
    {
        platform::LockGuard lock(gate_);
        lastGc_ = report;
        processing_ = false;
    }

    return Result<GcReport>::Success(report);
}

uint32 ObjectRegistry::ReclaimEligible(uint32 maximum, uint64 deadline) noexcept {
    uint32 reclaimed = 0;
    while (reclaimed < maximum && (deadline == 0 || platform::MonotonicNanoseconds() < deadline)) {
        Slot* selected = nullptr;
        TypeLease type;
        Object* object = nullptr;
        void* storage = nullptr;
        NameRecord* record = nullptr;
        {
            platform::LockGuard lock(gate_);
            for (Chunk* chunk = chunks_; chunk && !selected; chunk = chunk->next) {
                for (auto& slot : chunk->slots) {
                    if (IsReclaimable(slot)) {
                        selected = &slot;
                        break;
                    }
                }
            }

            if (!selected) {
                break;
            }

            if (selected->indexed || selected->linked) {
                platform::Fatal("Reclaiming an object still in the name index or child ring");
            }

            // 저장소를 큐에서 분리한 뒤 잠금을 푼다. 콜백 중 통계 조회는 가능하지만 재회수는
            // 불가하다. 지역 TypeLease가 소멸자 코드까지 살아 있게 하므로 해제 콜백이 끝난 뒤
            // 반환한다. 이름 기록도 잠금 밖에서 반환한다(조회는 잠금 안에서 복사해 두었다).
            selected->state = SlotState::Reclaiming;
            type = Move(selected->type);
            object = selected->object;
            storage = selected->storage;
            record = selected->name;
            selected->name = nullptr;
        }

        // User destructors and allocator callbacks never execute under the registry gate.
        type.Ops().destroy(object);
        auto released = allocator_.Deallocate(storage);
        if (!released) {
            platform::Fatal("Object allocation ownership mismatch");
        }

        ReleaseRegistryMemory(allocator_, record);
        {
            platform::LockGuard lock(gate_);
            selected->object = nullptr;
            selected->storage = nullptr;
            selected->transaction = 0;
            selected->stableType = {};
            selected->mark = 0;
            selected->outer = {};
            selected->outermost = nullptr;
            selected->parent = nullptr;
            selected->lastChild = nullptr;
            selected->pathBytes = 0;
            // 세대를 0/과거 값으로 순환시키면 오래된 WeakRef가 새 객체를 가리킬 수 있다.
            // 한도에 도달한 슬롯은 재사용하지 않고 Exhausted로 남긴다.
            if (selected->generation >= options_.maxGeneration) {
                selected->state = SlotState::Exhausted;
            } else {
                ++selected->generation;
                selected->state = SlotState::Free;
            }
        }

        type.Reset();
        ++reclaimed;
    }

    return reclaimed;
}

Result<uint32> ObjectRegistry::DrainRetired(const ObjectAccessContext& access) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return Result<uint32>::Failure(valid.Error());
    }

    {
        platform::LockGuard lock(gate_);
        if (processing_) {
            return Result<uint32>::Failure(ErrorCode::Busy);
        }

        processing_ = true;
    }

    uint32 count = ReclaimEligible();
    {
        platform::LockGuard lock(gate_);
        processing_ = false;
    }

    return Result<uint32>::Success(count);
}

Result<void> ObjectRegistry::Shutdown(const ObjectAccessContext& access) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return valid;
    }

    {
        platform::LockGuard lock(gate_);
        if (processing_) {
            return Result<void>::Failure(ErrorCode::Busy);
        }

        closed_ = true;
    }

    // cutoff를 게시한 cycle은 끝까지 분류해야 한다. 중간 취소로 white 객체를 다시 노출하지 않는다.
    while (CollectionPhase() != GcPhase::Idle) {
        auto advanced = StepCollection(access, {0xffffffffu, 0});
        if (!advanced) {
            return Result<void>::Failure(advanced.Error());
        }
    }

    auto collected = Collect(access);
    if (!collected) {
        return Result<void>::Failure(collected.Error());
    }

    auto stats = Stats();
    if (stats.liveObjects || stats.reservedObjects || stats.destroyRequested ||
        stats.retiredObjects || stats.roots || stats.borrows || stats.backendHolds ||
        stats.encodePins) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    return Result<void>::Success();
}

Result<ObjectHandle> ObjectRegistry::GetOuter(const ObjectAccessContext& access,
                                              ObjectHandle handle) const noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return Result<ObjectHandle>::Failure(valid.Error());
    }

    platform::LockGuard lock(gate_);
    if (auto open = OpenState(closed_, processing_); !open) {
        return Result<ObjectHandle>::Failure(open.Error());
    }

    auto found = FindPublishedLocked(handle);
    if (!found) {
        return Result<ObjectHandle>::Failure(found.Error());
    }

    return Result<ObjectHandle>::Success(found.Value()->outer);
}

Result<ObjectHandle> ObjectRegistry::GetOutermost(const ObjectAccessContext& access,
                                                  ObjectHandle handle) const noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return Result<ObjectHandle>::Failure(valid.Error());
    }

    platform::LockGuard lock(gate_);
    if (auto open = OpenState(closed_, processing_); !open) {
        return Result<ObjectHandle>::Failure(open.Error());
    }

    auto found = FindPublishedLocked(handle);
    if (!found) {
        return Result<ObjectHandle>::Failure(found.Error());
    }

    // 공개된 객체의 Outer 사슬은 모두 회수 전이다(살아 있는 Inner가 inner → outer로 Outer를
    // 표식하고, cutoff white 객체는 위에서 거부했다).
    return Result<ObjectHandle>::Success(HandleOf(*found.Value()->outermost));
}

Result<void> ObjectRegistry::GetName(const ObjectAccessContext& access, ObjectHandle handle,
                                     String& out) const noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return valid;
    }

    // 잠금 안에서 스택에 복사하고 잠금 밖에서 붙인다. out의 할당 콜백이 그 객체를 회수해도 해제된
    // 이름을 읽지 않는다.
    char buffer[MaxNameComponentBytes];
    usize bytes = 0;
    {
        platform::LockGuard lock(gate_);
        if (auto open = OpenState(closed_, processing_); !open) {
            return open;
        }

        auto found = FindPublishedLocked(handle);
        if (!found) {
            return Result<void>::Failure(found.Error());
        }

        if (const NameRecord* record = found.Value()->name) {
            bytes = record->nameBytes;
            platform::MemoryCopy(buffer, record->Data(), bytes);
        }
    }

    return out.TryAppend(StringView(buffer, bytes));
}

Result<void> ObjectRegistry::GetObjectPath(const ObjectAccessContext& access, ObjectHandle handle,
                                           String& out) const noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return valid;
    }

    char buffer[MaxObjectPathBytes];
    usize bytes = 0;
    {
        platform::LockGuard lock(gate_);
        if (auto open = OpenState(closed_, processing_); !open) {
            return open;
        }

        auto found = FindPublishedLocked(handle);
        if (!found) {
            return Result<void>::Failure(found.Error());
        }

        // 경로 길이를 생성 때 계산해 두었으므로 끝에서부터 채운다. 대표 자식의 빈 이름은 건너뛰고,
        // 앞쪽 경로가 남아 있을 때만 '.'을 넣는다.
        const Slot* slot = found.Value();
        bytes = slot->pathBytes;
        usize position = bytes;
        for (const Slot* current = slot; !current->outer.IsNull(); current = current->parent) {
            const NameRecord& record = *current->name;
            if (record.nameBytes == 0) {
                continue;
            }

            if (record.nameBytes > position) {
                platform::Fatal("Corrupt object path length");
            }

            position -= record.nameBytes;
            platform::MemoryCopy(buffer + position, record.Data(), record.nameBytes);
            if (position != 0) {
                buffer[--position] = '.';
            }
        }

        if (position != 0) {
            platform::Fatal("Corrupt object path length");
        }
    }

    return out.TryAppend(StringView(buffer, bytes));
}

Result<ObjectHandle> ObjectRegistry::FindChild(const ObjectAccessContext& access,
                                               ObjectHandle outer, StringView name) const noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return Result<ObjectHandle>::Failure(valid.Error());
    }

    const bool hasOuter = !outer.IsNull();
    const ObjectHandle outerKey = hasOuter ? outer : ObjectHandle{};
    // 이름 형식 검사·키 생성(할당)을 잠금 밖에서 하므로 그 앞뒤로 같은 검사를 한다.
    auto checkLocked = [&]() noexcept -> Result<void> {
        if (auto open = OpenState(closed_, processing_); !open) {
            return open;
        }

        if (hasOuter) {
            auto found = FindPublishedLocked(outer);
            if (!found) {
                return Result<void>::Failure(found.Error());
            }
        }

        return Result<void>::Success();
    };
    {
        platform::LockGuard lock(gate_);
        if (auto checked = checkLocked(); !checked) {
            return Result<ObjectHandle>::Failure(checked.Error());
        }
    }

    if (name.IsEmpty()) {
        if (!hasOuter) {
            return Result<ObjectHandle>::Failure(ErrorCode::InvalidArgument);
        }
    } else if (auto format = ValidateNameComponent(name, allocator_); !format) {
        return Result<ObjectHandle>::Failure(format.Error());
    }

    StringBuilder<128> key(allocator_);
    if (auto made = TryMakeNameComponentKey(name, key); !made) {
        return Result<ObjectHandle>::Failure(made.Error());
    }

    const uint64 hash = NameKeyHash(outerKey, key.View());
    platform::LockGuard lock(gate_);
    if (auto checked = checkLocked(); !checked) {
        return Result<ObjectHandle>::Failure(checked.Error());
    }

    const Slot* found = FindNamedLocked(outerKey, hash, key.View(), true);
    if (found == nullptr) {
        return Result<ObjectHandle>::Failure(ErrorCode::NotFound);
    }

    return Result<ObjectHandle>::Success(HandleOf(*found));
}

Result<ObjectHandle> ObjectRegistry::FindObject(const ObjectAccessContext& access,
                                                ObjectHandle outermost,
                                                StringView objectPath) const noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return Result<ObjectHandle>::Failure(valid.Error());
    }

    ObjectHandle current{};
    {
        platform::LockGuard lock(gate_);
        if (auto open = OpenState(closed_, processing_); !open) {
            return Result<ObjectHandle>::Failure(open.Error());
        }

        auto found = FindPublishedLocked(outermost);
        if (!found) {
            return Result<ObjectHandle>::Failure(found.Error());
        }

        if (!found.Value()->outer.IsNull()) {
            return Result<ObjectHandle>::Failure(ErrorCode::InvalidArgument);
        }

        current = HandleOf(*found.Value());
    }

    if (objectPath.ByteCount() > MaxObjectPathBytes) {
        return Result<ObjectHandle>::Failure(ErrorCode::InvalidArgument);
    }

    // 구성 요소는 '.'로 나눈다('.'은 ASCII라 UTF-8 sequence 안에 나오지 않는다). 조회 전에 모든
    // 구성 요소의 형식을 검사해 결과가 앞 구성 요소의 존재 여부에 따라 달라지지 않게 한다.
    const char* data = objectPath.Data();
    const usize size = objectPath.ByteCount();
    auto forEachComponent = [&](auto&& visit) noexcept -> Result<void> {
        usize start = 0;
        for (usize position = 0; position <= size; ++position) {
            if (position == size || data[position] == '.') {
                if (auto visited = visit(StringView(data + start, position - start)); !visited) {
                    return visited;
                }

                start = position + 1;
            }
        }

        return Result<void>::Success();
    };
    if (size != 0) {
        auto formatted = forEachComponent([&](StringView component) noexcept -> Result<void> {
            return ValidateNameComponent(component, allocator_);
        });
        if (!formatted) {
            return Result<ObjectHandle>::Failure(formatted.Error());
        }
    }

    // 공개된 대표 자식이 있으면 경로는 그 아래에서 시작한다(빈 경로 = 대표 자식).
    {
        platform::LockGuard lock(gate_);
        if (auto open = OpenState(closed_, processing_); !open) {
            return Result<ObjectHandle>::Failure(open.Error());
        }

        auto found = FindPublishedLocked(current);
        if (!found) {
            return Result<ObjectHandle>::Failure(found.Error());
        }

        const Slot* representative = FindNamedLocked(current, NameKeyHash(current, {}), {}, true);
        if (representative != nullptr) {
            current = HandleOf(*representative);
        } else if (size == 0) {
            return Result<ObjectHandle>::Failure(ErrorCode::NotFound);
        }
    }

    if (size == 0) {
        return Result<ObjectHandle>::Success(current);
    }

    auto walked = forEachComponent([&](StringView component) noexcept -> Result<void> {
        StringBuilder<128> key(allocator_);
        if (auto made = TryMakeNameComponentKey(component, key); !made) {
            return made;
        }

        const uint64 hash = NameKeyHash(current, key.View());
        platform::LockGuard lock(gate_);
        if (auto open = OpenState(closed_, processing_); !open) {
            return open;
        }

        // 키 생성 할당 콜백이 그 사이 Registry를 바꿨을 수 있어 현재 객체를 다시 확인한다.
        auto found = FindPublishedLocked(current);
        if (!found) {
            return Result<void>::Failure(found.Error());
        }

        const Slot* child = FindNamedLocked(current, hash, key.View(), true);
        if (child == nullptr) {
            return Result<void>::Failure(ErrorCode::NotFound);
        }

        current = HandleOf(*child);
        return Result<void>::Success();
    });
    if (!walked) {
        return Result<ObjectHandle>::Failure(walked.Error());
    }

    return Result<ObjectHandle>::Success(current);
}

Result<bool> ObjectRegistry::IsNameTaken(const ObjectAccessContext& access, ObjectHandle outer,
                                         StringView name) const noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return Result<bool>::Failure(valid.Error());
    }

    const bool hasOuter = !outer.IsNull();
    const ObjectHandle outerKey = hasOuter ? outer : ObjectHandle{};
    // outer는 공개 전 객체여도 된다(공개 전 Inner도 이름을 점유하므로 같은 기준이다).
    auto checkLocked = [&]() noexcept -> Result<void> {
        if (auto open = OpenState(closed_, processing_); !open) {
            return open;
        }

        if (!hasOuter) {
            return Result<void>::Success();
        }

        const Slot* slot = FindSlot(outer);
        if (!slot || slot->state == SlotState::Free || slot->state == SlotState::Exhausted) {
            return Result<void>::Failure(ErrorCode::StaleHandle);
        }

        if (IsCutoff(slot) ||
            !(slot->state == SlotState::Reserved || slot->state == SlotState::Alive ||
              slot->state == SlotState::DestroyRequested)) {
            return Result<void>::Failure(ErrorCode::Destroyed);
        }

        return Result<void>::Success();
    };
    {
        platform::LockGuard lock(gate_);
        if (auto checked = checkLocked(); !checked) {
            return Result<bool>::Failure(checked.Error());
        }
    }

    if (name.IsEmpty()) {
        if (!hasOuter) {
            return Result<bool>::Failure(ErrorCode::InvalidArgument);
        }
    } else if (auto format = ValidateNameComponent(name, allocator_); !format) {
        return Result<bool>::Failure(format.Error());
    }

    StringBuilder<128> key(allocator_);
    if (auto made = TryMakeNameComponentKey(name, key); !made) {
        return Result<bool>::Failure(made.Error());
    }

    const uint64 hash = NameKeyHash(outerKey, key.View());
    platform::LockGuard lock(gate_);
    if (auto checked = checkLocked(); !checked) {
        return Result<bool>::Failure(checked.Error());
    }

    return Result<bool>::Success(FindNamedLocked(outerKey, hash, key.View(), false) != nullptr);
}

Result<OuterSummary> ObjectRegistry::DescribeOuter(const ObjectAccessContext& access,
                                                   ObjectHandle outer) const noexcept {
    return DescribeOuterChecked(access, outer, 0);
}

Result<OuterSummary> ObjectRegistry::DescribeOuterChecked(const ObjectAccessContext& access,
                                                          ObjectHandle outer,
                                                          uint64 transaction) const noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return Result<OuterSummary>::Failure(valid.Error());
    }

    platform::LockGuard lock(gate_);
    if (auto open = OpenState(closed_, processing_); !open) {
        return Result<OuterSummary>::Failure(open.Error());
    }

    if (outer.IsNull()) {
        return Result<OuterSummary>::Failure(ErrorCode::InvalidArgument);
    }

    // 생성 규칙 5와 같은 기준이다: 생존(Alive, cutoff 아님) 또는 이 트랜잭션의 공개 전 객체.
    const Slot* slot = FindSlot(outer);
    if (!slot || IsCutoff(slot) ||
        !(slot->state == SlotState::Alive ||
          (transaction != 0 && slot->state == SlotState::Reserved &&
           slot->transaction == transaction && slot->object != nullptr))) {
        return Result<OuterSummary>::Failure(ErrorCode::StaleHandle);
    }

    return Result<OuterSummary>::Success(DescribeSlotLocked(*slot));
}

Result<void> ObjectRegistry::CollectInners(const ObjectAccessContext& access, ObjectHandle outer,
                                           InnerOrder order, Array<ObjectHandle>& out) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return valid;
    }

    const bool depthFirst = order == InnerOrder::DepthFirst;
    // Alive 객체만 모으고, 모으지 않는 객체(공개 전·파괴 요청·cutoff white)의 부분 트리는 내려가지
    // 않는다. 그래서 DepthFirst에서도 Outer가 언제나 Inner보다 앞이다.
    auto collectable = [this](const Slot* slot) noexcept {
        return slot->state == SlotState::Alive && !IsCutoff(slot);
    };
    Slot* root = nullptr;
    usize count = 0;
    {
        platform::LockGuard lock(gate_);
        if (auto open = OpenState(closed_, processing_); !open) {
            return open;
        }

        auto found = FindPublishedLocked(outer);
        if (!found) {
            return Result<void>::Failure(found.Error());
        }

        root = found.Value();
        // (1) 개수를 센다. 스택 없이 부모 포인터로 되돌아 오르는 전위 순회다.
        const Slot* node = root->firstChild;
        while (node != nullptr) {
            const bool take = collectable(node);
            if (take) {
                ++count;
            }

            if (take && depthFirst && node->firstChild != nullptr) {
                node = node->firstChild;
                continue;
            }

            while (node != root && node->nextSibling == nullptr) {
                node = node->parent;
            }

            node = node == root ? nullptr : node->nextSibling;
        }

        // (2) 처리 표시 아래 잠금 밖에서 할당한다(그 사이 재진입 변경은 Busy라 트리가 바뀌지
        // 않는다).
        processing_ = true;
    }

    auto finish = [this](ErrorCode error) noexcept -> Result<void> {
        platform::LockGuard lock(gate_);
        processing_ = false;
        return error == ErrorCode::None ? Result<void>::Success() : Result<void>::Failure(error);
    };
    if (count == 0) {
        return finish(ErrorCode::None);
    }

    const usize before = out.Size();
    if (count > static_cast<usize>(-1) - before) {
        return finish(ErrorCode::Overflow);
    }

    if (auto reserved = out.TryReserve(before + count); !reserved) {
        return finish(reserved.Error());
    }

    Array<InnerEntry> entries(allocator_);
    if (auto reserved = entries.TryReserve(count); !reserved) {
        return finish(reserved.Error());
    }

    // (3) 수준 순서로 모은다: 직계, 이어서 각 원소의 자식(같은 Outer의 자식이 연속 구간이다).
    usize direct = 0;
    bool grew = false;
    {
        platform::LockGuard lock(gate_);
        auto appendChildren = [&](const Slot& parent, uint32 parentPosition) noexcept {
            for (Slot* child = parent.firstChild; child != nullptr; child = child->nextSibling) {
                if (!collectable(child)) {
                    continue;
                }

                if (entries.Size() == count) {
                    grew = true;
                    return;
                }

                InnerEntry entry;
                entry.slot = child;
                entry.parent = parentPosition;
                if (!entries.TryPushBack(entry)) {
                    platform::Fatal("Reserved inner collection unexpectedly failed");
                }
            }
        };
        appendChildren(*root, NoInnerParent);
        direct = entries.Size();
        if (depthFirst) {
            for (usize position = 0; position < entries.Size() && !grew; ++position) {
                const auto start = static_cast<uint32>(entries.Size());
                appendChildren(*entries[position].slot, static_cast<uint32>(position));
                entries[position].childStart = start;
                entries[position].childCount = static_cast<uint32>(entries.Size()) - start;
            }
        }
    }

    // 처리 표시 동안 트리가 바뀔 수 없으므로 개수가 다르면 계약 위반이다.
    if (grew || entries.Size() != count) {
        return finish(ErrorCode::Busy);
    }

    // (4) 잠금 밖에서 같은 Outer 구간마다 이름 순으로 제자리 정렬한다. 이 구간에는 할당·사용자
    // 콜백이 없고 회수는 소유 스레드의 명시 호출뿐이라 NameRecord가 사라지지 않는다. 같은 Outer 안
    // 이름은 서로 달라 순서가 결정적이다.
    auto byName = [](const InnerEntry& left, const InnerEntry& right) noexcept {
        return NameLess(left.slot->name->Name(), right.slot->name->Name());
    };
    algo::Sort(Span<InnerEntry>(entries.Data(), direct), byName);
    if (depthFirst) {
        // 원소는 자기 자식 구간(childStart, childCount)을 들고 움직이고, 자식 구간은 부모보다 뒤에
        // 있어 부모 구간을 정렬한 뒤에 방문된다.
        for (usize position = 0; position < entries.Size(); ++position) {
            const InnerEntry& entry = entries[position];
            if (entry.childCount > 1) {
                algo::Sort(Span<InnerEntry>(entries.Data() + entry.childStart, entry.childCount),
                           byName);
            }
        }

        for (usize position = 0; position < entries.Size(); ++position) {
            const InnerEntry& entry = entries[position];
            for (uint32 child = 0; child < entry.childCount; ++child) {
                entries[entry.childStart + child].parent = static_cast<uint32>(position);
            }
        }
    }

    // (5) Direct는 그대로, DepthFirst는 부모 위치·자식 구간으로 스택 없이 전위 순서를 만든다.
    auto push = [&](const InnerEntry& entry) noexcept {
        if (!out.TryPushBack(HandleOf(*entry.slot))) {
            platform::Fatal("Reserved inner output unexpectedly failed");
        }
    };
    if (!depthFirst) {
        for (usize position = 0; position < direct; ++position) {
            push(entries[position]);
        }
    } else {
        usize current = 0;
        bool done = false;
        while (!done) {
            push(entries[current]);
            if (entries[current].childCount != 0) {
                current = entries[current].childStart;
                continue;
            }

            for (;;) {
                const uint32 parent = entries[current].parent;
                const usize end = parent == NoInnerParent
                                      ? direct
                                      : entries[parent].childStart + entries[parent].childCount;
                if (current + 1 < end) {
                    ++current;
                    break;
                }

                if (parent == NoInnerParent) {
                    done = true;
                    break;
                }

                current = parent;
            }
        }
    }

    return finish(ErrorCode::None);
}

ObjectStats ObjectRegistry::Stats() const noexcept {
    platform::LockGuard lock(gate_);
    ObjectStats stats;
    stats.chunks = chunkCount_;
    for (Chunk* chunk = chunks_; chunk; chunk = chunk->next) {
        for (auto& slot : chunk->slots) {
            switch (slot.state) {
            case SlotState::Alive:
                ++stats.liveObjects;
                break;
            case SlotState::Reserved:
                ++stats.reservedObjects;
                break;
            case SlotState::DestroyRequested:
                ++stats.destroyRequested;
                break;
            case SlotState::Retired:
            case SlotState::Reclaiming:
                ++stats.retiredObjects;
                break;
            default:
                break;
            }

            stats.roots += slot.roots;
            stats.borrows += slot.borrows;
            stats.backendHolds += slot.backendHolds;
            stats.encodePins += slot.encodePins;
        }
    }

    stats.namedObjects = nameLive_;
    return stats;
}

Result<uint32>
ObjectRegistry::CaptureDiagnostics(Span<ObjectDiagnosticRecord> output) const noexcept {
    platform::LockGuard lock(gate_);
    uint32 required = 0;
    for (Chunk* chunk = chunks_; chunk; chunk = chunk->next) {
        for (auto& slot : chunk->slots) {
            if (slot.state != SlotState::Free && slot.state != SlotState::Exhausted) {
                ++required;
            }
        }
    }

    if (output.Size() < required) {
        return Result<uint32>::Failure(ErrorCode::CapacityExceeded);
    }

    uint32 written = 0;
    for (Chunk* chunk = chunks_; chunk; chunk = chunk->next) {
        for (auto& slot : chunk->slots) {
            ObjectPhase phase;
            switch (slot.state) {
            case SlotState::Reserved:
                phase = ObjectPhase::Reserved;
                break;
            case SlotState::Alive:
                phase = ObjectPhase::Alive;
                break;
            case SlotState::DestroyRequested:
                phase = ObjectPhase::DestroyRequested;
                break;
            case SlotState::Retired:
                phase = ObjectPhase::Retired;
                break;
            case SlotState::Reclaiming:
                phase = ObjectPhase::Reclaiming;
                break;
            default:
                continue;
            }

            output[written++] = {{domain_, slot.index, slot.generation},
                                 context_,
                                 slot.stableType,
                                 phase,
                                 slot.roots,
                                 slot.borrows,
                                 slot.backendHolds,
                                 slot.encodePins};
        }
    }

    return Result<uint32>::Success(written);
}

GcReport ObjectRegistry::LastGcReport() const noexcept {
    platform::LockGuard lock(gate_);
    return lastGc_;
}

} // namespace rocks
