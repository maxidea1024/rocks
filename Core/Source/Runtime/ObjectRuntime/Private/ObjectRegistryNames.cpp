#include "ObjectRegistryInternal.h"

#include <Core/Algorithms.h>
#include <Core/Text/NameComponent.h>
#include <Core/Text/StringBuilder.h>
#include <Object/ObjectRegistry.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>

// ObjectRegistry의 Outer·이름이다. 이름 색인(개방 주소 표)과 자식 고리 유지, 공개 객체 조회, Outer
// 요약(DescribeSlotLocked·DescribeOuter), 새 객체 신원 규칙(detail::CheckNewObjectIdentity), 조회
// API(GetOuter·GetOutermost·GetName·GetObjectPath·FindChild·FindObject·IsNameTaken·
// CollectInners)를 둔다.
namespace rocks {

using namespace registry_detail;

// CollectInners 보조 배열 원소다. 수준 순서로 모은 뒤 같은 Outer 구간마다 정렬한다.
struct ObjectRegistry::InnerEntry {
    Slot* slot = nullptr;
    uint32 parent = 0;     // 부모 원소 위치(직계면 NoInnerParent)
    uint32 childStart = 0; // 자식 구간 시작(DepthFirst)
    uint32 childCount = 0;
};

namespace {

constexpr uint32 NoInnerParent = NumericLimits<uint32>::Max();
constexpr uint32 MinimumNameCapacity = 16;

// NFC UTF-8 byte 순(빈 이름이 먼저)이다.
bool NameLess(StringView left, StringView right) noexcept {
    const usize common =
        left.ByteCount() < right.ByteCount() ? left.ByteCount() : right.ByteCount();
    const int order = platform::MemoryCompare(left.Data(), right.Data(), common);
    return order != 0 ? order < 0 : left.ByteCount() < right.ByteCount();
}

} // namespace

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

Result<ObjectRegistry::Slot*> ObjectRegistry::FindVisibleLocked(ObjectHandle handle,
                                                                uint64 transaction) const noexcept {
    // 생성 규칙 5·DescribeOuterChecked와 같은 기준의 공개 전 객체다(생성자 실행이 끝난 것).
    if (transaction != 0) {
        Slot* slot = FindSlot(handle);
        if (slot != nullptr && slot->state == SlotState::Reserved &&
            ColdOf(*slot).transaction == transaction && slot->object != nullptr &&
            !IsCutoff(slot)) {
            return Result<Slot*>::Success(slot);
        }
    }

    return FindPublishedLocked(handle);
}

OuterSummary ObjectRegistry::DescribeSlotLocked(const Slot& slot,
                                                bool scanNamedInners) const noexcept {
    OuterSummary summary;
    const Slot& outermost = *ColdOf(slot).outermost;
    summary.outermost = HandleOf(outermost);
    summary.isOutermost = ColdOf(slot).outer.IsNull();
    summary.outermostIsPackage = options_.policy.packageNativeType != nullptr &&
                                 outermost.type.IsNativeA(options_.policy.packageNativeType);
    // Inner 유무는 이름 점유와 같은 기준이다(다른 트랜잭션의 Reserved는 세고 cutoff white는 세지
    // 않는다). 대표 자식은 색인으로 O(1)에 찾고, 이름 있는 Inner는 고리에서 처음 만나는 것까지
    // 본다.
    const ObjectHandle self = HandleOf(slot);
    summary.hasRepresentative = FindNamedLocked(self, NameKeyHash(self, {}), {}, false) != nullptr;
    for (const Slot* child = scanNamedInners ? ColdOf(slot).firstChild : nullptr; child;
         child = ColdOf(*child).nextSibling) {
        if (!IsCutoff(child) && ColdOf(*child).name != nullptr &&
            ColdOf(*child).name->nameBytes != 0) {
            summary.hasNamedInners = true;
            break;
        }
    }

    summary.pathBytes = ColdOf(slot).pathBytes;
    // Within 판정의 Outer 타입이다(slot이 사는 동안 유효 — 리플렉션 B9h2).
    summary.type = &slot.type;
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
        if (entry.keyHash != hash || !(ColdOf(*slot).outer == outer) ||
            !(ColdOf(*slot).name->Key() == key)) {
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

ObjectRegistry::Slot* ObjectRegistry::FindVisibleNamedLocked(ObjectHandle outer, uint64 hash,
                                                             StringView key,
                                                             uint64 transaction) const noexcept {
    // (outer, 키)는 점유 기준으로 유일하므로 점유 항목 하나를 찾아 보이는지만 가린다.
    Slot* slot = FindNamedLocked(outer, hash, key, transaction == 0);
    if (slot != nullptr && slot->state == SlotState::Reserved &&
        (ColdOf(*slot).transaction != transaction || slot->object == nullptr)) {
        return nullptr;
    }

    return slot;
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
        if (entry.keyHash == hash && ColdOf(*entry.slot).outer == ColdOf(slot).outer &&
            ColdOf(*entry.slot).name->Key() == key && IsCutoff(entry.slot)) {
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
    for (uint32 index = static_cast<uint32>(ColdOf(slot).name->keyHash) & mask;;
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
    ColdOf(child).prevSibling = ColdOf(parent).lastChild;
    ColdOf(child).nextSibling = nullptr;
    if (ColdOf(parent).lastChild != nullptr) {
        ColdOf(*ColdOf(parent).lastChild).nextSibling = &child;
    } else {
        ColdOf(parent).firstChild = &child;
    }

    ColdOf(parent).lastChild = &child;
    child.linked = true;
}

void ObjectRegistry::UnlinkChildLocked(Slot& child) noexcept {
    Slot& parent = *ColdOf(child).parent;
    if (ColdOf(child).prevSibling != nullptr) {
        ColdOf(*ColdOf(child).prevSibling).nextSibling = ColdOf(child).nextSibling;
    } else {
        ColdOf(parent).firstChild = ColdOf(child).nextSibling;
    }

    if (ColdOf(child).nextSibling != nullptr) {
        ColdOf(*ColdOf(child).nextSibling).prevSibling = ColdOf(child).prevSibling;
    } else {
        ColdOf(parent).lastChild = ColdOf(child).prevSibling;
    }

    ColdOf(child).prevSibling = nullptr;
    ColdOf(child).nextSibling = nullptr;
    child.linked = false;
    // 마지막 자식이 떨어진 Retired 부모는 회수 가능해졌을 수 있다(지연 목록이면 풀린 목록으로).
    if (ColdOf(parent).firstChild == nullptr) {
        WakeDeferredLocked(parent);
    }
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

    // 8a. Within(리플렉션 B9h2): Outer가 Within 클래스 또는 파생이어야 하고 Outer 없는 객체는 될 수
    // 없다. Outer 타입을 모르는 요약(그래프 1단계의 기준 Outer)은 판정을 미룬다.
    const TypeId within = type.Schema().withinType;
    if (within != TypeId{} &&
        (outer == nullptr || (outer->type != nullptr && !outer->type->IsA(within)))) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 9. 경로 길이.
    if (ChildPathBytes(outer, name.ByteCount()) > MaxObjectPathBytes) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    return Result<void>::Success();
}

} // namespace detail

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

    return Result<ObjectHandle>::Success(ColdOf(*found.Value()).outer);
}

Result<ObjectOrigin> ObjectRegistry::GetOrigin(const ObjectAccessContext& access,
                                               ObjectHandle handle) const noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return Result<ObjectOrigin>::Failure(valid.Error());
    }

    platform::LockGuard lock(gate_);
    if (auto open = OpenState(closed_, processing_); !open) {
        return Result<ObjectOrigin>::Failure(open.Error());
    }

    auto found = FindPublishedLocked(handle);
    if (!found) {
        return Result<ObjectOrigin>::Failure(found.Error());
    }

    return Result<ObjectOrigin>::Success(ColdOf(*found.Value()).origin);
}

Result<void> ObjectRegistry::SetPendingOrigin(ObjectHandle handle, uint64 transaction,
                                              ObjectOrigin origin) noexcept {
    platform::LockGuard lock(gate_);
    auto found = FindVisibleLocked(handle, transaction);
    if (!found || ColdOf(*found.Value()).transaction != transaction || transaction == 0) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    ColdOf(*found.Value()).origin = origin;
    return Result<void>::Success();
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
    return Result<ObjectHandle>::Success(HandleOf(*ColdOf(*found.Value()).outermost));
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

        if (const NameRecord* record = ColdOf(*found.Value()).name) {
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
        bytes = ColdOf(*slot).pathBytes;
        usize position = bytes;
        for (const Slot* current = slot; !ColdOf(*current).outer.IsNull();
             current = ColdOf(*current).parent) {
            const NameRecord& record = *ColdOf(*current).name;
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
    return FindObjectChecked(access, outermost, objectPath, 0);
}

Result<ObjectHandle> ObjectRegistry::FindObjectChecked(const ObjectAccessContext& access,
                                                       ObjectHandle outermost,
                                                       StringView objectPath,
                                                       uint64 transaction) const noexcept {
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

        auto found = FindVisibleLocked(outermost, transaction);
        if (!found) {
            return Result<ObjectHandle>::Failure(found.Error());
        }

        if (!ColdOf(*found.Value()).outer.IsNull()) {
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

    // 보이는 대표 자식이 있으면 경로는 그 아래에서 시작한다(빈 경로 = 대표 자식).
    {
        platform::LockGuard lock(gate_);
        if (auto open = OpenState(closed_, processing_); !open) {
            return Result<ObjectHandle>::Failure(open.Error());
        }

        auto found = FindVisibleLocked(current, transaction);
        if (!found) {
            return Result<ObjectHandle>::Failure(found.Error());
        }

        const Slot* representative =
            FindVisibleNamedLocked(current, NameKeyHash(current, {}), {}, transaction);
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
        auto found = FindVisibleLocked(current, transaction);
        if (!found) {
            return Result<void>::Failure(found.Error());
        }

        const Slot* child = FindVisibleNamedLocked(current, hash, key.View(), transaction);
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

Result<ObjectHandle> ObjectRegistry::FindChildChecked(const ObjectAccessContext& access,
                                                      ObjectHandle outer, StringView name,
                                                      uint64 transaction) const noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return Result<ObjectHandle>::Failure(valid.Error());
    }

    // 이름 형식 검사·키 생성(할당)을 잠금 밖에서 하므로 그 앞뒤로 outer를 확인한다.
    auto checkLocked = [&]() noexcept -> Result<void> {
        if (auto open = OpenState(closed_, processing_); !open) {
            return open;
        }

        auto found = FindVisibleLocked(outer, transaction);
        return found ? Result<void>::Success() : Result<void>::Failure(found.Error());
    };
    {
        platform::LockGuard lock(gate_);
        if (auto checked = checkLocked(); !checked) {
            return Result<ObjectHandle>::Failure(checked.Error());
        }
    }

    if (!name.IsEmpty()) {
        if (auto format = ValidateNameComponent(name, allocator_); !format) {
            return Result<ObjectHandle>::Failure(format.Error());
        }
    }

    StringBuilder<128> key(allocator_);
    if (auto made = TryMakeNameComponentKey(name, key); !made) {
        return Result<ObjectHandle>::Failure(made.Error());
    }

    const uint64 hash = NameKeyHash(outer, key.View());
    platform::LockGuard lock(gate_);
    // 키 생성 할당 콜백이 그 사이 Registry를 바꿨을 수 있어 outer를 다시 확인한다.
    if (auto checked = checkLocked(); !checked) {
        return Result<ObjectHandle>::Failure(checked.Error());
    }

    const Slot* child = FindVisibleNamedLocked(outer, hash, key.View(), transaction);
    if (child == nullptr) {
        return Result<ObjectHandle>::Failure(ErrorCode::NotFound);
    }

    return Result<ObjectHandle>::Success(HandleOf(*child));
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
           ColdOf(*slot).transaction == transaction && slot->object != nullptr))) {
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
        const Slot* node = ColdOf(*root).firstChild;
        while (node != nullptr) {
            const bool take = collectable(node);
            if (take) {
                ++count;
            }

            if (take && depthFirst && ColdOf(*node).firstChild != nullptr) {
                node = ColdOf(*node).firstChild;
                continue;
            }

            while (node != root && ColdOf(*node).nextSibling == nullptr) {
                node = ColdOf(*node).parent;
            }

            node = node == root ? nullptr : ColdOf(*node).nextSibling;
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
    if (count > MaxSize - before) {
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
            for (Slot* child = ColdOf(parent).firstChild; child != nullptr;
                 child = ColdOf(*child).nextSibling) {
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
    auto byName = [this](const InnerEntry& left, const InnerEntry& right) noexcept {
        return NameLess(ColdOf(*left.slot).name->Name(), ColdOf(*right.slot).name->Name());
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

} // namespace rocks
