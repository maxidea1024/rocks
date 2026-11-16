#pragma once

#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/String.h>
#include <Object/ConstructionContext.h>
#include <Object/NativeClass.h>
#include <Object/ObjectRegistry.h>
#include <Platform/Fatal.h>

// ObjectRefMap(리플렉션 B9f4 — Docs/Design/ReflectionExtensions.ko.md 7.4절) 시험의 공용 fixture다:
// 대상 클래스(Item·OtherItem), String·uint32 키 ObjectRefMap을 담는 소유 클래스(Holder), 할당·해제
// 순간에 hook을 한 번 실행하고 지정한 번째 할당을 실패시키는 allocator(ScriptedAllocator), GC·생존
// 도우미. hook은 명시적으로 무장한 할당·해제 하나에서만 돌므로 시험 순서가 결정적이다.
namespace refmap_fixture {

using namespace rocks;

class Item : public Object {
    ROCKS_NATIVE_TYPE(Item, Object, 0x52464d4974656d31ull, 0x0b9f4a01c0de0001ull)

public:
    Item() noexcept = default;
};

class OtherItem final : public Object {
    ROCKS_NATIVE_TYPE(OtherItem, Object, 0x52464d4f74686572ull, 0x0b9f4a01c0de0002ull)
};

class Holder final : public Object {
    ROCKS_NATIVE_TYPE(Holder, Object, 0x52464d486f6c6472ull, 0x0b9f4a01c0de0003ull)

public:
    explicit Holder(Allocator& allocator) noexcept : named(allocator), numbered(allocator) {
    }

    void Trace(TraceVisitor& visitor) const noexcept {
        named.Trace(visitor);
        numbered.Trace(visitor);
    }

    ObjectRefMap<String, Item> named;
    ObjectRefMap<uint32, Item> numbered;
};

inline bool RegisterAll(TypeRegistry& types) noexcept {
    return RegisterNativeClass<Item>(types).IsOk() &&
           RegisterNativeClass<OtherItem>(types).IsOk() &&
           RegisterNativeClass<Holder>(types).IsOk();
}

// 할당을 세고, 무장한 뒤 n번째 할당을 실패시키고, 무장한 할당·해제 순간에 hook을 한 번 실행한다.
// 해제 hook은 지켜보는 주소(watched — nullptr이면 다음 해제 아무것이나)의 반환 직전에 돈다.
class ScriptedAllocator final : public Allocator {
public:
    using Hook = void (*)(void*) noexcept;

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag) noexcept override {
        ++allocations;
        ++sinceArm;
        if (failAt != 0 && sinceArm == failAt) {
            return Result<void*>::Failure(ErrorCode::OutOfMemory);
        }

        if (allocateHook != nullptr && sinceArm == allocateAt) {
            Hook run = allocateHook;
            allocateHook = nullptr;
            run(allocateContext);
        }

        return backing.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        ++deallocations;
        if (releaseHook != nullptr && (watched == nullptr || watched == address)) {
            Hook run = releaseHook;
            releaseHook = nullptr;
            run(releaseContext);
        }

        return backing.Deallocate(address);
    }

    // 지금부터 n번째(1부터) 할당을 실패시킨다(0이면 끈다).
    void FailAt(uint64 n) noexcept {
        sinceArm = 0;
        failAt = n;
    }

    // 지금부터 n번째(1부터) 할당 직전에 run을 한 번 실행한다.
    void OnAllocation(uint64 n, Hook run, void* context) noexcept {
        sinceArm = 0;
        allocateAt = n;
        allocateHook = run;
        allocateContext = context;
    }

    // address(nullptr이면 다음 아무 해제)의 반환 직전에 run을 한 번 실행한다.
    void OnRelease(const void* address, Hook run, void* context) noexcept {
        watched = address;
        releaseHook = run;
        releaseContext = context;
    }

    void Disarm() noexcept {
        failAt = 0;
        allocateHook = nullptr;
        releaseHook = nullptr;
    }

    SystemAllocator backing;
    uint64 allocations = 0;
    uint64 deallocations = 0;
    uint64 sinceArm = 0;
    uint64 failAt = 0;
    uint64 allocateAt = 0;
    Hook allocateHook = nullptr;
    void* allocateContext = nullptr;
    const void* watched = nullptr;
    Hook releaseHook = nullptr;
    void* releaseContext = nullptr;
};

template <class T>
T* Get(Borrow<T>& borrow) noexcept {
    auto pointer = borrow.Get();
    return pointer ? pointer.Value() : nullptr;
}

inline ErrorCode ErrorOf(const Result<void>& result) noexcept {
    return result ? ErrorCode::None : result.Error();
}

// 루트 없는 Item을 만든다(다음 GC에서 도달하지 않으면 회수된다).
inline ObjectHandle Unrooted(ObjectRegistry& objects) noexcept {
    auto created = objects.NewObject<Item>(objects.OwnerAccess(), NativeTypeIdOf<Item>());
    if (!created) {
        platform::Fatal("ObjectRefMap fixture item failed");
    }

    return created.Value().Handle();
}

inline bool StepOnce(ObjectRegistry& objects) noexcept {
    GcBudget budget;
    budget.maxObjects = 1;
    budget.maxNanoseconds = 0;
    return static_cast<bool>(objects.StepCollection(objects.OwnerAccess(), budget));
}

// GC를 끝까지 진행한다(slice마다 객체 하나).
inline bool FinishCollection(ObjectRegistry& objects) noexcept {
    for (uint32 guard = 0; guard < 4096 && objects.CollectionPhase() != GcPhase::Idle; ++guard) {
        if (!StepOnce(objects)) {
            return false;
        }
    }

    return objects.CollectionPhase() == GcPhase::Idle;
}

inline bool IsAlive(ObjectRegistry& objects, ObjectHandle handle) noexcept {
    auto lifecycle = objects.InspectLifecycle(objects.OwnerAccess(), handle);
    return lifecycle && lifecycle.Value() == ObjectLifecycle::Alive;
}

// 컨테이너 저장소의 시작 주소다(첫 산 항목의 키 — 항목 0이 살아 있으면 할당 시작).
template <class K>
const void* StorageOf(const ObjectRefMap<K, Item>& map) noexcept {
    for (auto entry : map.View()) {
        return &entry.key;
    }

    return nullptr;
}

// 삽입 순서의 키(String) 목록이 expected와 같은지다.
inline bool NamedOrderIs(const ObjectRefMap<String, Item>& map, const StringView* expected,
                         usize count) noexcept {
    if (map.Count() != count) {
        return false;
    }

    usize position = 0;
    for (auto entry : map.View()) {
        if (entry.key.View() != expected[position++]) {
            return false;
        }
    }

    return true;
}

} // namespace refmap_fixture
