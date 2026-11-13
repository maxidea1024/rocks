#include <Core/Memory/SystemAllocator.h>
#include <Object/ConstructionContext.h>
#include <Object/NativeClass.h>
#include <Object/NativeMemberTraits.h>
#include <Object/ObjectRegistry.h>
#include <TestSupport/Test.h>
#include <Platform/Fatal.h>
#include <Platform/Thread.h>

using namespace rocks;

// Registry 컨테이너(B5a) 시험이다(Docs/Design/ReflectionFoundations.ko.md 5.2·5.3·5.6절):
// ObjectRefArray의 null 원소·Insert·SetAt·성장 재진입 정정, staged AssignReferences,
// StructRefArray, encode 고정. 원소 struct의 GeneratedType 특수화는 생성 코드(B5b) 전이라 Trace만
// 손으로 썼다.
namespace container_fixture {

class Item : public Object {
    ROCKS_NATIVE_TYPE(Item, Object, 0x6c1f0e9a2b3d4c5eull, 0x8a7b6c5d4e3f2a1bull)

public:
    Item() noexcept = default;
};

class OtherItem final : public Object {
    ROCKS_NATIVE_TYPE(OtherItem, Object, 0x2e4d6c8b0a1f3e5dull, 0x9c8b7a6f5e4d3c2bull)
};

// 참조 struct 원소다. 생성자 hook으로 원소 생성 중 Registry 재진입을 시험한다(한 번 실행되고
// 지워진다).
struct MaterialSlot {
    explicit MaterialSlot(Allocator& allocator) noexcept : label(allocator) {
        ++Constructed;
        if (Hook != nullptr) {
            auto hook = Hook;
            Hook = nullptr;
            hook(HookContext);
        }
    }

    ~MaterialSlot() noexcept {
        ++Destroyed;
    }

    static inline uint32 Constructed = 0;
    static inline uint32 Destroyed = 0;
    static inline void (*Hook)(void*) noexcept = nullptr;
    static inline void* HookContext = nullptr;

    ObjectRef<Item> material;
    ObjectRef<Item> overlay;
    String label;
    uint32 index = 0;
};

class Holder final : public Object {
    ROCKS_NATIVE_TYPE(Holder, Object, 0x7d3c1b5a9e8f2d4cull, 0x1a2b3c4d5e6f7a8bull)

public:
    explicit Holder(Allocator& allocator) noexcept : items(allocator), slots(allocator) {
    }

    // GeneratedType<MaterialSlot> 특수화 뒤에 정의한다.
    void Trace(TraceVisitor& visitor) const noexcept;

    ObjectRef<Item> single;
    ObjectRefArray<Item> items;
    StructRefArray<MaterialSlot> slots;
};

} // namespace container_fixture

namespace rocks {

template <>
struct GeneratedType<container_fixture::MaterialSlot> {
    static constexpr bool ContainsStrongReference = true;

    static void Trace(const container_fixture::MaterialSlot& value,
                      TraceVisitor& visitor) noexcept {
        visitor.Visit(value.material);
        visitor.Visit(value.overlay);
    }
};

} // namespace rocks

void container_fixture::Holder::Trace(TraceVisitor& visitor) const noexcept {
    visitor.Visit(single);
    items.Trace(visitor);
    slots.Trace(visitor);
}

namespace {

using namespace container_fixture;

static_assert(IsNativeStrongReference<StructRefArray<MaterialSlot>>);
static_assert(detail::RegistryContainer<ObjectRefArray<Item>>::value);
static_assert(detail::RegistryContainer<StructRefArray<MaterialSlot>>::value);
static_assert(!detail::RegistryContainer<ObjectRef<Item>>::value);
static_assert(!detail::RegistryContainer<Array<uint32>>::value);

// Registry 컨테이너 할당 태그(ObjectRegistry.cpp): ObjectRefArray 버퍼, StructRefArray 원소
// 블록·색인.
constexpr uint32 RefArrayTag = 0x4f524146u;
constexpr uint32 ElementTag = 0x53524145u;
constexpr uint32 IndexTag = 0x53524149u;

// 컨테이너 태그 할당을 세고, 지정한 번째를 실패시키고, 지정한 태그의 할당 중에 hook을 한 번
// 실행한다(allocator 콜백의 Registry 재진입).
class HookAllocator final : public Allocator {
public:
    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag) noexcept override {
        const bool container = tag == RefArrayTag || tag == ElementTag || tag == IndexTag;
        if (container) {
            refArrays += tag == RefArrayTag;
            elements += tag == ElementTag;
            indexes += tag == IndexTag;
            ++containerAllocations;
            if (failAt != 0 && containerAllocations == failAt) {
                return Result<void*>::Failure(ErrorCode::OutOfMemory);
            }
        }

        if (hook != nullptr && tag == hookTag) {
            auto run = hook;
            hook = nullptr;
            run(hookContext);
        }

        return backing.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        return backing.Deallocate(address);
    }

    // 지금부터 n번째 컨테이너 할당을 실패시킨다(0이면 끈다).
    void FailContainerAllocation(uint64 n) noexcept {
        containerAllocations = 0;
        failAt = n;
    }

    void ResetCounts() noexcept {
        refArrays = 0;
        elements = 0;
        indexes = 0;
        containerAllocations = 0;
    }

    void Arm(uint32 tag, void (*run)(void*) noexcept, void* context) noexcept {
        hookTag = tag;
        hook = run;
        hookContext = context;
    }

    SystemAllocator backing;
    uint64 refArrays = 0;
    uint64 elements = 0;
    uint64 indexes = 0;
    uint64 containerAllocations = 0;
    uint64 failAt = 0;
    uint32 hookTag = 0;
    void (*hook)(void*) noexcept = nullptr;
    void* hookContext = nullptr;
};

bool RegisterAll(TypeRegistry& types) noexcept {
    return RegisterNativeClass<Item>(types).IsOk() &&
           RegisterNativeClass<OtherItem>(types).IsOk() &&
           RegisterNativeClass<Holder>(types).IsOk();
}

template <class T>
T* Get(Borrow<T>& borrow) noexcept {
    auto pointer = borrow.Get();
    return pointer ? pointer.Value() : nullptr;
}

ErrorCode ErrorOf(const Result<void>& result) noexcept {
    return result ? ErrorCode::None : result.Error();
}

// 루트 없는 Item을 만든다(다음 GC에서 도달하지 않으면 회수된다).
ObjectHandle Unrooted(ObjectRegistry& objects) noexcept {
    auto created = objects.NewObject<Item>(objects.OwnerAccess(), NativeTypeIdOf<Item>());
    return created ? created.Value().Handle() : ObjectHandle{};
}

// GC를 끝까지 진행한다(slice마다 객체 하나).
bool FinishCollection(ObjectRegistry& objects) noexcept {
    GcBudget budget;
    budget.maxObjects = 1;
    budget.maxNanoseconds = 0;
    for (uint32 guard = 0; guard < 4096 && objects.CollectionPhase() != GcPhase::Idle; ++guard) {
        if (!objects.StepCollection(objects.OwnerAccess(), budget)) {
            return false;
        }
    }

    return objects.CollectionPhase() == GcPhase::Idle;
}

bool StepOnce(ObjectRegistry& objects) noexcept {
    GcBudget budget;
    budget.maxObjects = 1;
    budget.maxNanoseconds = 0;
    return static_cast<bool>(objects.StepCollection(objects.OwnerAccess(), budget));
}

bool IsAlive(ObjectRegistry& objects, ObjectHandle handle) noexcept {
    auto lifecycle = objects.InspectLifecycle(objects.OwnerAccess(), handle);
    return lifecycle && lifecycle.Value() == ObjectLifecycle::Alive;
}

// allocator·생성자 콜백에서 같은 컨테이너를 바꾸는 재진입 탐침이다.
struct Reentry {
    ObjectRegistry* objects = nullptr;
    ObjectHandle owner;
    Holder* holder = nullptr;
    const ObjectHandle* targets = nullptr;
    uint32 adds = 0;
    uint32 removes = 0;
    ErrorCode last = ErrorCode::None;
};

void ReenterItems(void* raw) noexcept {
    auto& probe = *static_cast<Reentry*>(raw);
    const auto access = probe.objects->OwnerAccess();
    for (uint32 index = 0; index < probe.removes; ++index) {
        auto removed =
            probe.objects->RemoveReferenceAt(access, probe.owner, probe.holder->items, 0);
        probe.last = removed ? probe.last : removed.Error();
    }

    for (uint32 index = 0; index < probe.adds; ++index) {
        auto added = probe.objects->AddReference(access, probe.owner, probe.holder->items,
                                                 probe.targets[index]);
        probe.last = added ? probe.last : added.Error();
    }
}

void ReenterSlots(void* raw) noexcept {
    auto& probe = *static_cast<Reentry*>(raw);
    const auto access = probe.objects->OwnerAccess();
    for (uint32 index = 0; index < probe.removes; ++index) {
        auto removed = probe.objects->RemoveElementAt(access, probe.owner, probe.holder->slots, 0);
        probe.last = removed ? probe.last : removed.Error();
    }

    for (uint32 index = 0; index < probe.adds; ++index) {
        auto added = probe.objects->AddElement(access, probe.owner, probe.holder->slots);
        probe.last = added ? probe.last : added.Error();
    }
}

// 다른 스레드의 RootRef 복제와 소유 스레드의 Resolve가 콜백 중 Busy가 아닌지 본다.
struct ThreadProbe {
    ObjectRegistry* objects = nullptr;
    RootRef<Item>* root = nullptr;
    ObjectHandle other;
    ErrorCode cloned = ErrorCode::DependencyFailed;
    ErrorCode resolved = ErrorCode::DependencyFailed;
};

void RunClone(void* raw) noexcept {
    auto& probe = *static_cast<ThreadProbe*>(raw);
    auto cloned = probe.root->Clone();
    probe.cloned = cloned ? ErrorCode::None : cloned.Error();
}

void CloneFromOtherThread(void* raw) noexcept {
    auto& probe = *static_cast<ThreadProbe*>(raw);
    platform::Thread thread;
    if (thread.Start(RunClone, &probe) != platform::ThreadStatus::Success ||
        thread.WaitForCompletion(30000) != platform::WaitResult::Signaled ||
        thread.Join() != platform::ThreadStatus::Success) {
        platform::Fatal("Registry container clone thread did not finish");
    }

    auto borrowed = probe.objects->Resolve<Item>(probe.objects->OwnerAccess(), probe.other);
    probe.resolved = borrowed ? ErrorCode::None : borrowed.Error();
}

} // namespace

TEST(ObjectRefArray_NullElementsAreNormalized) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterAll(types));
        ObjectRegistry objects(allocator, types, {0x4352, 1});
        const auto access = objects.OwnerAccess();
        auto holder = objects.NewObject<Holder>(access, NativeTypeIdOf<Holder>());
        auto item = objects.NewObject<Item>(access, NativeTypeIdOf<Item>());
        REQUIRE(holder && item);
        const ObjectHandle owner = holder.Value().Handle();
        // 비정규 null: generation 0(다른 칸이 채워져 있어도) 또는 registry 0.
        const ObjectHandle generationZero{item.Value().Handle().registry, 3, 0};
        const ObjectHandle registryZero{0, 5, 7};
        {
            auto borrowed = objects.Resolve<Holder>(access, owner);
            REQUIRE(borrowed);
            Holder* pointer = Get(borrowed.Value());
            REQUIRE(pointer != nullptr);
            REQUIRE(objects.AddReference(access, owner, pointer->items, generationZero));
            REQUIRE(objects.AddReference(access, owner, pointer->items, item.Value().Handle()));
            REQUIRE(objects.InsertReferenceAt(access, owner, pointer->items, 0, registryZero));
            REQUIRE(objects.AddReference(access, owner, pointer->items, ObjectHandle{}));
            REQUIRE(pointer->items.Size() == 4);
            CHECK(pointer->items[0] == ObjectHandle{} && pointer->items[1] == ObjectHandle{});
            CHECK(pointer->items[2] == item.Value().Handle() &&
                  pointer->items[3] == ObjectHandle{});
            // 원소 교체도 null을 정규화한다.
            REQUIRE(objects.SetReferenceAt(access, owner, pointer->items, 2, generationZero));
            CHECK(pointer->items[2] == ObjectHandle{});
            REQUIRE(
                objects.SetReferenceAt(access, owner, pointer->items, 2, item.Value().Handle()));
            // 단일 참조 쓰기도 정규화한다.
            REQUIRE(objects.SetReference(access, owner, pointer->single, item.Value().Handle()));
            REQUIRE(objects.SetReference(access, owner, pointer->single, registryZero));
            CHECK(pointer->single.Handle() == ObjectHandle{});
            // 비정규 null로 찾고 지워도 첫 null 원소다.
            CHECK(pointer->items.IndexOf(registryZero) == 0 && pointer->items.Contains({}));
            REQUIRE(objects.RemoveReference(access, owner, pointer->items, generationZero));
            REQUIRE(objects.RemoveReference(access, owner, pointer->items, ObjectHandle{}));
            CHECK(pointer->items.Size() == 2 && pointer->items[0] == item.Value().Handle() &&
                  pointer->items[1] == ObjectHandle{});
        }

        // null 원소는 간선이 아니며 Trace가 방문해도 수집이 실패하지 않는다.
        const ObjectHandle kept = item.Value().Handle();
        item.Value().Reset();
        REQUIRE(objects.Collect(access));
        CHECK(objects.Stats().liveObjects == 2 && IsAlive(objects, kept));
        holder.Value().Reset();
        REQUIRE(objects.Collect(access));
        CHECK(objects.Stats().liveObjects == 0);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ObjectRefArray_InsertAndSetAtValidateLikeAdd) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterAll(types));
        ObjectRegistry objects(allocator, types, {0x4352, 2});
        const auto access = objects.OwnerAccess();
        auto holder = objects.NewObject<Holder>(access, NativeTypeIdOf<Holder>());
        auto other = objects.NewObject<OtherItem>(access, NativeTypeIdOf<OtherItem>());
        REQUIRE(holder && other);
        const ObjectHandle owner = holder.Value().Handle();
        RootRef<Item> items[4];
        for (RootRef<Item>& root : items) {
            auto created = objects.NewObject<Item>(access, NativeTypeIdOf<Item>());
            REQUIRE(created);
            root = Move(created.Value());
        }

        const ObjectHandle replaced = items[3].Handle();
        {
            auto borrowed = objects.Resolve<Holder>(access, owner);
            REQUIRE(borrowed);
            Holder* pointer = Get(borrowed.Value());
            REQUIRE(pointer != nullptr);
            // 끝(index == Size)·앞·가운데 삽입.
            REQUIRE(objects.InsertReferenceAt(access, owner, pointer->items, 0, items[1].Handle()));
            REQUIRE(objects.InsertReferenceAt(access, owner, pointer->items, 0, items[0].Handle()));
            REQUIRE(objects.InsertReferenceAt(access, owner, pointer->items, 2, items[3].Handle()));
            REQUIRE(objects.InsertReferenceAt(access, owner, pointer->items, 2, items[2].Handle()));
            REQUIRE(pointer->items.Size() == 4);
            for (usize index = 0; index < 4; ++index) {
                CHECK(pointer->items[index] == items[index].Handle());
            }

            // 범위 밖 index: Insert는 index > Size, SetAt은 index >= Size가 InvalidArgument다.
            CHECK(objects.InsertReferenceAt(access, owner, pointer->items, 5, items[0].Handle())
                      .Error() == ErrorCode::InvalidArgument);
            CHECK(objects.SetReferenceAt(access, owner, pointer->items, 4, items[0].Handle())
                      .Error() == ErrorCode::InvalidArgument);
            // 타입·생존 검증은 AddReference와 같다.
            CHECK(
                objects.InsertReferenceAt(access, owner, pointer->items, 0, other.Value().Handle())
                    .Error() == ErrorCode::TypeMismatch);
            CHECK(objects.SetReferenceAt(access, owner, pointer->items, 0, other.Value().Handle())
                      .Error() == ErrorCode::TypeMismatch);
            auto victim = objects.NewObject<Item>(access, NativeTypeIdOf<Item>());
            REQUIRE(victim);
            REQUIRE(objects.RequestDestroy(access, victim.Value().Handle()));
            CHECK(
                objects.InsertReferenceAt(access, owner, pointer->items, 0, victim.Value().Handle())
                    .Error() == ErrorCode::StaleHandle);
            CHECK(objects.SetReferenceAt(access, owner, pointer->items, 0, victim.Value().Handle())
                      .Error() == ErrorCode::StaleHandle);
            // 배열이 owner 안에 없으면 InvalidArgument다.
            ObjectRefArray<Item> local(allocator);
            CHECK(objects.InsertReferenceAt(access, owner, local, 0, items[0].Handle()).Error() ==
                  ErrorCode::InvalidArgument);
            for (usize index = 0; index < 4; ++index) {
                CHECK(pointer->items[index] == items[index].Handle());
            }

            // 교체: 마지막 원소를 첫 원소로 바꾼다.
            REQUIRE(objects.SetReferenceAt(access, owner, pointer->items, 3, items[0].Handle()));
            CHECK(pointer->items[3] == items[0].Handle());
        }

        // Borrow 없는 owner·파괴 요청된 owner.
        Holder* pointer = nullptr;
        {
            auto borrowed = objects.Resolve<Holder>(access, owner);
            REQUIRE(borrowed);
            pointer = Get(borrowed.Value());
        }

        CHECK(objects.InsertReferenceAt(access, owner, pointer->items, 0, items[0].Handle())
                  .Error() == ErrorCode::InvalidArgument);
        CHECK(objects.SetReferenceAt(access, owner, pointer->items, 0, items[0].Handle()).Error() ==
              ErrorCode::InvalidArgument);

        // 교체로 빠진 대상은 회수되고 넣은 대상은 남는다.
        for (RootRef<Item>& root : items) {
            root.Reset();
        }

        other.Value().Reset();
        REQUIRE(objects.Collect(access));
        CHECK(!IsAlive(objects, replaced));
        CHECK(objects.Stats().liveObjects == 4);
        {
            auto borrowed = objects.Resolve<Holder>(access, owner);
            REQUIRE(borrowed);
            REQUIRE(objects.RequestDestroy(access, owner));
            CHECK(objects.InsertReferenceAt(access, owner, pointer->items, 0, ObjectHandle{})
                      .Error() == ErrorCode::StaleHandle);
            CHECK(
                objects.SetReferenceAt(access, owner, pointer->items, 0, ObjectHandle{}).Error() ==
                ErrorCode::StaleHandle);
            CHECK(objects.ClearReferences(access, owner, pointer->items));
        }

        holder.Value().Reset();
        REQUIRE(objects.Collect(access));
        CHECK(objects.Stats().liveObjects == 0);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ObjectRefArray_InsertAndSetAtShadeDuringMarkingAndRejectCutoff) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterAll(types));
        ObjectRegistry objects(allocator, types, {0x4352, 3});
        const auto access = objects.OwnerAccess();
        // holder가 slot 0이라 root snapshot 큐의 맨 앞이다(첫 slice에서 검게 된다). 패딩 루트가
        // Marking을 이어 가게 한다.
        auto holder = objects.NewObject<Holder>(access, NativeTypeIdOf<Holder>());
        auto first = objects.NewObject<Item>(access, NativeTypeIdOf<Item>());
        auto second = objects.NewObject<Item>(access, NativeTypeIdOf<Item>());
        REQUIRE(holder && first && second);
        const ObjectHandle owner = holder.Value().Handle();
        const ObjectHandle inserted = Unrooted(objects);
        const ObjectHandle set = Unrooted(objects);
        const ObjectHandle appended = Unrooted(objects);
        const ObjectHandle garbage = Unrooted(objects);
        {
            // 용량 4를 null로 채운다(다음 삽입이 새 버퍼를 게시하는 경로를 탄다).
            auto borrowed = objects.Resolve<Holder>(access, owner);
            REQUIRE(borrowed);
            for (uint32 index = 0; index < 4; ++index) {
                REQUIRE(objects.AddReference(access, owner, Get(borrowed.Value())->items, {}));
            }
        }

        REQUIRE(objects.BeginCollection(access));
        REQUIRE(StepOnce(objects));
        REQUIRE(objects.CollectionPhase() == GcPhase::Marking);
        {
            // 이미 검게 스캔된 holder에 흰 대상을 넣는다: 삽입 장벽이 shade하지 않으면 회수된다.
            // 가득 찬 배열의 Insert·Add(새 버퍼 게시)와 SetAt(제자리)을 모두 거친다.
            auto borrowed = objects.Resolve<Holder>(access, owner);
            REQUIRE(borrowed);
            Holder* pointer = Get(borrowed.Value());
            REQUIRE(pointer->items.Size() == 4);
            REQUIRE(objects.InsertReferenceAt(access, owner, pointer->items, 0, inserted));
            REQUIRE(objects.SetReferenceAt(access, owner, pointer->items, 1, set));
            while (pointer->items.Size() < 8) {
                REQUIRE(objects.AddReference(access, owner, pointer->items, {}));
            }

            REQUIRE(objects.AddReference(access, owner, pointer->items, appended));
            CHECK(pointer->items.Size() == 9 && pointer->items[8] == appended);
        }

        REQUIRE(FinishCollection(objects));
        CHECK(IsAlive(objects, inserted) && IsAlive(objects, set) && IsAlive(objects, appended));
        CHECK(!IsAlive(objects, garbage));

        // cutoff 뒤의 흰 객체는 Insert·SetAt·staged AssignReferences로 되살릴 수 없다.
        const ObjectHandle white = Unrooted(objects);
        REQUIRE(objects.BeginCollection(access));
        for (uint32 guard = 0; guard < 64 && objects.CollectionPhase() == GcPhase::Marking;
             ++guard) {
            REQUIRE(StepOnce(objects));
        }

        REQUIRE(objects.CollectionPhase() == GcPhase::Sweeping);
        {
            auto borrowed = objects.Resolve<Holder>(access, owner);
            REQUIRE(borrowed);
            Holder* pointer = Get(borrowed.Value());
            CHECK(objects.InsertReferenceAt(access, owner, pointer->items, 0, white).Error() ==
                  ErrorCode::StaleHandle);
            CHECK(objects.SetReferenceAt(access, owner, pointer->items, 0, white).Error() ==
                  ErrorCode::StaleHandle);
            CHECK(pointer->items.Size() == 9 && pointer->items[0] == inserted);
        }

        {
            ConstructionContext transaction(objects, access);
            auto staged = transaction.Create<Holder>(NativeTypeIdOf<Holder>());
            REQUIRE(staged);
            auto stagedHolder = transaction.Get<Holder>(staged.Value());
            REQUIRE(stagedHolder);
            const ObjectHandle targets[] = {white};
            CHECK(transaction.AssignReferences(staged.Value(), stagedHolder.Value()->items, targets)
                      .Error() == ErrorCode::StaleHandle);
        }

        REQUIRE(FinishCollection(objects));
        holder.Value().Reset();
        first.Value().Reset();
        second.Value().Reset();
        REQUIRE(objects.Collect(access));
        CHECK(objects.Stats().liveObjects == 0);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ObjectRefArray_GrowthReentrancyRejudgesCurrentState) {
    // 잠금 밖 버퍼 할당 중 allocator 콜백이 같은 배열을 바꾸면 다시 잠근 뒤 그때의 size·index로
    // 다시 판단한다(현행 Append의 넘침 회귀 포함): 새 버퍼가 모자라면 Busy·내용 불변, 용량이
    // 충분해졌으면 기존 버퍼에 넣고, index가 size를 넘으면 Busy다.
    HookAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterAll(types));
        ObjectRegistry objects(allocator, types, {0x4352, 4});
        const auto access = objects.OwnerAccess();
        RootRef<Item> pool[17];
        ObjectHandle targets[17];
        for (usize index = 0; index < 17; ++index) {
            auto created = objects.NewObject<Item>(access, NativeTypeIdOf<Item>());
            REQUIRE(created);
            targets[index] = created.Value().Handle();
            pool[index] = Move(created.Value());
        }

        struct Case {
            usize initial;  // 시작 원소 수(용량 4가 꽉 찬다)
            usize insertAt; // ~0이면 AddReference
            uint32 adds;    // 콜백이 더하는 원소
            uint32 removes; // 콜백이 지우는 원소
            ErrorCode expected;
            usize finalSize;
        };

        constexpr usize Append = ~usize(0);
        const Case cases[] = {
            {4, Append, 12, 0, ErrorCode::Busy, 16}, // 콜백이 16칸을 채웠다: 새 버퍼(8) 부족
            {4, Append, 5, 0, ErrorCode::None, 10},  // 콜백 성장으로 용량 16: 기존 버퍼에 넣는다
            {4, Append, 0, 2, ErrorCode::None, 3},   // 콜백이 줄였다: 기존 버퍼에 넣는다
            {4, 4, 0, 2, ErrorCode::Busy, 2},        // index 4 > 현재 size 2
            {4, 1, 1, 0, ErrorCode::None, 6},        // 기존 버퍼(용량 8)의 index 1에 넣는다
            {4, Append, 4, 0, ErrorCode::Busy, 8},   // 콜백 뒤 size 8·용량 8: 새 버퍼 8도 부족
        };
        for (const Case& current : cases) {
            auto holder = objects.NewObject<Holder>(access, NativeTypeIdOf<Holder>());
            REQUIRE(holder);
            const ObjectHandle owner = holder.Value().Handle();
            auto borrowed = objects.Resolve<Holder>(access, owner);
            REQUIRE(borrowed);
            Holder* pointer = Get(borrowed.Value());
            for (usize element = 0; element < current.initial; ++element) {
                REQUIRE(objects.AddReference(access, owner, pointer->items, targets[element]));
            }

            Reentry probe;
            probe.objects = &objects;
            probe.owner = owner;
            probe.holder = pointer;
            probe.targets = targets + 4;
            probe.adds = current.adds;
            probe.removes = current.removes;
            allocator.Arm(RefArrayTag, ReenterItems, &probe);
            const ObjectHandle outer = targets[16];
            auto result = current.insertAt == Append
                              ? objects.AddReference(access, owner, pointer->items, outer)
                              : objects.InsertReferenceAt(access, owner, pointer->items,
                                                          current.insertAt, outer);
            CHECK(allocator.hook == nullptr); // 콜백이 실행됐다
            CHECK(probe.last == ErrorCode::None);
            CHECK(ErrorOf(result) == current.expected);
            CHECK(pointer->items.Size() == current.finalSize);
            CHECK(pointer->items.Contains(outer) == (current.expected == ErrorCode::None));
            if (current.expected == ErrorCode::None && current.insertAt != Append) {
                CHECK(pointer->items[current.insertAt] == outer);
            }

            // 콜백이 넣은 원소는 순서대로 남아 있다(넘쳐 쓰지 않았다).
            if (current.adds != 0) {
                const usize start = current.initial - current.removes;
                usize found = 0;
                for (usize at = 0; at < pointer->items.Size(); ++at) {
                    found += pointer->items[at] == targets[4 + found] && found < current.adds &&
                             at >= start;
                }

                CHECK(found == current.adds);
            }
        }

        for (RootRef<Item>& root : pool) {
            root.Reset();
        }
    }

    CHECK(allocator.backing.Stats().liveAllocations == 0);
}

TEST(ObjectRefArray_LiveGrowthDoesNotBlockOtherThreads) {
    // live 성장은 레지스트리 전체 처리 표시를 세우지 않는다: 잠금 밖 할당 콜백 중에도 다른
    // 스레드의 RootRef 복제와 소유 스레드의 Resolve가 Busy가 되지 않는다.
    HookAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterAll(types));
        ObjectRegistry objects(allocator, types, {0x4352, 5});
        const auto access = objects.OwnerAccess();
        auto holder = objects.NewObject<Holder>(access, NativeTypeIdOf<Holder>());
        auto target = objects.NewObject<Item>(access, NativeTypeIdOf<Item>());
        auto shared = objects.NewObject<Item>(access, NativeTypeIdOf<Item>());
        REQUIRE(holder && target && shared);
        {
            auto borrowed = objects.Resolve<Holder>(access, holder.Value().Handle());
            REQUIRE(borrowed);
            ThreadProbe probe;
            probe.objects = &objects;
            probe.root = &shared.Value();
            probe.other = target.Value().Handle();
            allocator.Arm(RefArrayTag, CloneFromOtherThread, &probe);
            REQUIRE(objects.AddReference(access, holder.Value().Handle(),
                                         Get(borrowed.Value())->items, target.Value().Handle()));
            CHECK(allocator.hook == nullptr);
            CHECK(probe.cloned == ErrorCode::None);
            CHECK(probe.resolved == ErrorCode::None);
        }

        holder.Value().Reset();
        target.Value().Reset();
        shared.Value().Reset();
        REQUIRE(objects.Collect(access));
        CHECK(objects.Stats().liveObjects == 0);
    }

    CHECK(allocator.backing.Stats().liveAllocations == 0);
}

TEST(ObjectRefArray_InsertGrowthFailureKeepsContents) {
    HookAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterAll(types));
        ObjectRegistry objects(allocator, types, {0x4352, 6});
        const auto access = objects.OwnerAccess();
        auto holder = objects.NewObject<Holder>(access, NativeTypeIdOf<Holder>());
        auto target = objects.NewObject<Item>(access, NativeTypeIdOf<Item>());
        REQUIRE(holder && target);
        const ObjectHandle owner = holder.Value().Handle();
        auto borrowed = objects.Resolve<Holder>(access, owner);
        REQUIRE(borrowed);
        Holder* pointer = Get(borrowed.Value());
        for (uint32 index = 0; index < 4; ++index) {
            REQUIRE(
                objects.AddReference(access, owner, pointer->items,
                                     index % 2 == 0 ? target.Value().Handle() : ObjectHandle{}));
        }

        allocator.FailContainerAllocation(1);
        CHECK(objects.InsertReferenceAt(access, owner, pointer->items, 0, target.Value().Handle())
                  .Error() == ErrorCode::OutOfMemory);
        CHECK(pointer->items.Size() == 4 && pointer->items[0] == target.Value().Handle() &&
              pointer->items[1] == ObjectHandle{});
        allocator.FailContainerAllocation(0);
        REQUIRE(
            objects.InsertReferenceAt(access, owner, pointer->items, 1, target.Value().Handle()));
        CHECK(pointer->items.Size() == 5 && pointer->items[1] == target.Value().Handle() &&
              pointer->items[2] == ObjectHandle{});
        borrowed.Value().Reset();
        holder.Value().Reset();
        target.Value().Reset();
        REQUIRE(objects.Collect(access));
    }

    CHECK(allocator.backing.Stats().liveAllocations == 0);
}

TEST(ConstructionContext_AssignReferencesStagedRules) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterAll(types));
        ObjectRegistry objects(allocator, types, {0x4352, 7});
        const auto access = objects.OwnerAccess();
        auto alive = objects.NewObject<Item>(access, NativeTypeIdOf<Item>());
        auto other = objects.NewObject<OtherItem>(access, NativeTypeIdOf<OtherItem>());
        auto committed = objects.NewObject<Holder>(access, NativeTypeIdOf<Holder>());
        auto victim = objects.NewObject<Item>(access, NativeTypeIdOf<Item>());
        REQUIRE(alive && other && committed && victim);
        REQUIRE(objects.RequestDestroy(access, victim.Value().Handle()));
        RootRef<Object> root;
        ObjectHandle stagedItem;
        {
            ConstructionContext transaction(objects, access);
            auto owner = transaction.Create<Holder>(NativeTypeIdOf<Holder>());
            auto item = transaction.Create<Item>(NativeTypeIdOf<Item>());
            REQUIRE(owner && item);
            stagedItem = item.Value();
            auto pointer = transaction.Get<Holder>(owner.Value());
            REQUIRE(pointer);
            ObjectRefArray<Item>& items = pointer.Value()->items;

            // 게시된(Alive) owner·다른 트랜잭션의 owner는 StaleHandle이다(Borrow 불요 — Reserved로
            // 판정한다).
            {
                auto borrowed = objects.Resolve<Holder>(access, committed.Value().Handle());
                REQUIRE(borrowed);
                const ObjectHandle one[] = {alive.Value().Handle()};
                CHECK(transaction
                          .AssignReferences(committed.Value().Handle(),
                                            Get(borrowed.Value())->items, one)
                          .Error() == ErrorCode::StaleHandle);
            }

            {
                ConstructionContext otherTransaction(objects, access);
                const ObjectHandle one[] = {alive.Value().Handle()};
                CHECK(otherTransaction.AssignReferences(owner.Value(), items, one).Error() ==
                      ErrorCode::StaleHandle);
            }

            // 원소 검증: 타입 불일치 TypeMismatch, 파괴 요청된 대상 StaleHandle, owner 밖 배열
            // InvalidArgument. 실패하면 불변이다.
            const ObjectHandle mismatched[] = {alive.Value().Handle(), other.Value().Handle()};
            CHECK(transaction.AssignReferences(owner.Value(), items, mismatched).Error() ==
                  ErrorCode::TypeMismatch);
            const ObjectHandle stale[] = {victim.Value().Handle()};
            CHECK(transaction.AssignReferences(owner.Value(), items, stale).Error() ==
                  ErrorCode::StaleHandle);
            ObjectRefArray<Item> local(allocator);
            CHECK(transaction.AssignReferences(owner.Value(), local, stale).Error() ==
                  ErrorCode::InvalidArgument);
            CHECK(items.IsEmpty());

            // null 원소(비정규 포함)·생존 대상·같은 트랜잭션의 Reserved 대상을 받는다.
            const ObjectHandle targets[] = {alive.Value().Handle(), ObjectHandle{0, 9, 9},
                                            stagedItem, ObjectHandle{}};
            REQUIRE(transaction.AssignReferences(owner.Value(), items, targets));
            REQUIRE(items.Size() == 4);
            CHECK(items[0] == alive.Value().Handle() && items[1] == ObjectHandle{} &&
                  items[2] == stagedItem && items[3] == ObjectHandle{});
            // 다시 부르면 통째로 바꾼다(짧게).
            const ObjectHandle shorter[] = {stagedItem};
            REQUIRE(transaction.AssignReferences(owner.Value(), items, shorter));
            CHECK(items.Size() == 1 && items[0] == stagedItem);
            REQUIRE(transaction.AssignReferences(owner.Value(), items,
                                                 Span<const ObjectHandle>(targets, 3)));
            auto committedRoot = transaction.Commit();
            REQUIRE(committedRoot);
            root = Move(committedRoot.Value());
            // 끝난 트랜잭션은 InvalidArgument다.
            CHECK(transaction.AssignReferences(owner.Value(), items, shorter).Error() ==
                  ErrorCode::InvalidArgument);
        }

        // 원소 간선이 대상을 보존한다(null은 무시).
        const ObjectHandle aliveHandle = alive.Value().Handle();
        alive.Value().Reset();
        other.Value().Reset();
        committed.Value().Reset();
        victim.Value().Reset();
        REQUIRE(objects.Collect(access));
        CHECK(IsAlive(objects, aliveHandle) && IsAlive(objects, stagedItem));
        CHECK(objects.Stats().liveObjects == 3);
        root.Reset();
        REQUIRE(objects.Collect(access));
        CHECK(objects.Stats().liveObjects == 0);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

namespace {

struct StagedReentry {
    ObjectRegistry* objects = nullptr;
    ObjectHandle liveOwner;
    Holder* liveHolder = nullptr;
    ObjectHandle target;
    RootRef<Item>* root = nullptr;
    ErrorCode resolved = ErrorCode::None;
    ErrorCode added = ErrorCode::None;
    ErrorCode cloned = ErrorCode::None;
};

void RunStagedClone(void* raw) noexcept {
    auto& probe = *static_cast<StagedReentry*>(raw);
    auto cloned = probe.root->Clone();
    probe.cloned = cloned ? ErrorCode::None : cloned.Error();
}

// staged 성장은 처리 표시 아래라 Registry 재진입(다른 스레드의 RootRef 복제 포함)이 Busy다.
void ReenterDuringStagedGrowth(void* raw) noexcept {
    auto& probe = *static_cast<StagedReentry*>(raw);
    const auto access = probe.objects->OwnerAccess();
    auto borrowed = probe.objects->Resolve<Item>(access, probe.target);
    probe.resolved = borrowed ? ErrorCode::None : borrowed.Error();
    probe.added = ErrorOf(probe.objects->AddReference(access, probe.liveOwner,
                                                      probe.liveHolder->items, probe.target));
    platform::Thread thread;
    if (thread.Start(RunStagedClone, &probe) != platform::ThreadStatus::Success ||
        thread.WaitForCompletion(30000) != platform::WaitResult::Signaled ||
        thread.Join() != platform::ThreadStatus::Success) {
        platform::Fatal("Registry container clone thread did not finish");
    }
}

} // namespace

TEST(ConstructionContext_AssignReferencesGrowthIsAtomicAndReentrancyIsBusy) {
    HookAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterAll(types));
        ObjectRegistry objects(allocator, types, {0x4352, 8});
        const auto access = objects.OwnerAccess();
        auto target = objects.NewObject<Item>(access, NativeTypeIdOf<Item>());
        auto live = objects.NewObject<Holder>(access, NativeTypeIdOf<Holder>());
        REQUIRE(target && live);
        const uint64 baseline = allocator.backing.Stats().liveAllocations;
        ObjectHandle targets[6];
        for (ObjectHandle& handle : targets) {
            handle = target.Value().Handle();
        }

        {
            ConstructionContext transaction(objects, access);
            auto owner = transaction.Create<Holder>(NativeTypeIdOf<Holder>());
            REQUIRE(owner);
            auto pointer = transaction.Get<Holder>(owner.Value());
            REQUIRE(pointer);
            ObjectRefArray<Item>& items = pointer.Value()->items;
            REQUIRE(transaction.AssignReferences(owner.Value(), items,
                                                 Span<const ObjectHandle>(targets, 2)));
            // 성장 할당 실패: OutOfMemory, 내용 불변.
            allocator.FailContainerAllocation(1);
            CHECK(transaction.AssignReferences(owner.Value(), items, targets).Error() ==
                  ErrorCode::OutOfMemory);
            allocator.FailContainerAllocation(0);
            CHECK(items.Size() == 2 && items[1] == target.Value().Handle());

            // 성장 중 재진입은 Busy다(처리 표시). 할당 뒤에는 정상으로 게시한다.
            auto borrowed = objects.Resolve<Holder>(access, live.Value().Handle());
            REQUIRE(borrowed);
            StagedReentry probe;
            probe.objects = &objects;
            probe.liveOwner = live.Value().Handle();
            probe.liveHolder = Get(borrowed.Value());
            probe.target = target.Value().Handle();
            probe.root = &target.Value();
            allocator.Arm(RefArrayTag, ReenterDuringStagedGrowth, &probe);
            REQUIRE(transaction.AssignReferences(owner.Value(), items, targets));
            CHECK(allocator.hook == nullptr);
            CHECK(probe.resolved == ErrorCode::Busy && probe.added == ErrorCode::Busy &&
                  probe.cloned == ErrorCode::Busy);
            CHECK(items.Size() == 6 && probe.liveHolder->items.IsEmpty());
            borrowed.Value().Reset();
            // Commit 없이 범위를 벗어나면 rollback이 객체와 원소 버퍼를 반환한다.
        }

        CHECK(objects.Stats().reservedObjects == 0);
        CHECK(allocator.backing.Stats().liveAllocations == baseline);
        target.Value().Reset();
        live.Value().Reset();
        REQUIRE(objects.Collect(access));
    }

    CHECK(allocator.backing.Stats().liveAllocations == 0);
}

TEST(ConstructionContext_AssignReferencesShadesAliveTargetsBetweenSlices) {
    // slice 사이 Marking에서 staged 배열에 넣은 생존 대상은 바로 shade된다: Commit 뒤 owner가
    // 스캔되기 전에 원소를 지워도 이번 cycle에서 회수되지 않는다(다음 수집에서 회수된다).
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterAll(types));
        ObjectRegistry objects(allocator, types, {0x4352, 9});
        const auto access = objects.OwnerAccess();
        RootRef<Item> padding[3];
        for (RootRef<Item>& root : padding) {
            auto created = objects.NewObject<Item>(access, NativeTypeIdOf<Item>());
            REQUIRE(created);
            root = Move(created.Value());
        }

        const ObjectHandle white = Unrooted(objects);
        REQUIRE(objects.BeginCollection(access));
        REQUIRE(StepOnce(objects));
        REQUIRE(objects.CollectionPhase() == GcPhase::Marking);
        RootRef<Object> root;
        ObjectHandle owner;
        {
            ConstructionContext transaction(objects, access);
            auto created = transaction.Create<Holder>(NativeTypeIdOf<Holder>());
            REQUIRE(created);
            owner = created.Value();
            auto pointer = transaction.Get<Holder>(owner);
            REQUIRE(pointer);
            const ObjectHandle targets[] = {white, ObjectHandle{}};
            REQUIRE(transaction.AssignReferences(owner, pointer.Value()->items, targets));
            auto committed = transaction.Commit();
            REQUIRE(committed);
            root = Move(committed.Value());
        }

        {
            auto borrowed = objects.Resolve<Holder>(access, owner);
            REQUIRE(borrowed);
            REQUIRE(objects.RemoveReference(access, owner, Get(borrowed.Value())->items, white));
        }

        REQUIRE(FinishCollection(objects));
        CHECK(IsAlive(objects, white)); // shade된 대상은 이번 cycle을 넘긴다
        REQUIRE(objects.Collect(access));
        CHECK(!IsAlive(objects, white));
        root.Reset();
        for (RootRef<Item>& padded : padding) {
            padded.Reset();
        }

        REQUIRE(objects.Collect(access));
        CHECK(objects.Stats().liveObjects == 0);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ConstructionContext_CommitVisitsAssignedElements) {
    // Commit의 staged 간선 검증은 owner Trace로 배열 원소도 방문한다: AssignReferences 뒤 대상이
    // 파괴 요청되면 Commit이 StaleHandle로 실패하고 아무것도 게시하지 않는다.
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterAll(types));
        ObjectRegistry objects(allocator, types, {0x4352, 10});
        const auto access = objects.OwnerAccess();
        auto target = objects.NewObject<Item>(access, NativeTypeIdOf<Item>());
        REQUIRE(target);
        {
            ConstructionContext transaction(objects, access);
            auto owner = transaction.Create<Holder>(NativeTypeIdOf<Holder>());
            REQUIRE(owner);
            auto pointer = transaction.Get<Holder>(owner.Value());
            REQUIRE(pointer);
            const ObjectHandle targets[] = {ObjectHandle{}, target.Value().Handle()};
            REQUIRE(transaction.AssignReferences(owner.Value(), pointer.Value()->items, targets));
            REQUIRE(objects.RequestDestroy(access, target.Value().Handle()));
            CHECK(transaction.Commit().Error() == ErrorCode::StaleHandle);
            CHECK(objects.Stats().reservedObjects == 1);
        }

        CHECK(objects.Stats().reservedObjects == 0 && objects.Stats().liveObjects == 0);
        target.Value().Reset();
        REQUIRE(objects.Collect(access));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(StructRefArray_AddInsertRemoveClearKeepElementAddresses) {
    SystemAllocator allocator;
    MaterialSlot::Constructed = 0;
    MaterialSlot::Destroyed = 0;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterAll(types));
        ObjectRegistry objects(allocator, types, {0x4352, 11});
        const auto access = objects.OwnerAccess();
        auto holder = objects.NewObject<Holder>(access, NativeTypeIdOf<Holder>());
        REQUIRE(holder);
        const ObjectHandle owner = holder.Value().Handle();
        auto borrowed = objects.Resolve<Holder>(access, owner);
        REQUIRE(borrowed);
        Holder* pointer = Get(borrowed.Value());
        StructRefArray<MaterialSlot>& slots = pointer->slots;
        CHECK(slots.IsEmpty());
        // 색인이 여러 번 자라도(4 → 8 → 16) 원소 주소는 그대로다.
        MaterialSlot* addresses[10] = {};
        for (uint32 index = 0; index < 10; ++index) {
            auto added = objects.AddElement(access, owner, slots);
            REQUIRE(added && added.Value() == index);
            slots[index].index = index;
            addresses[index] = &slots[index];
            if (index > 0) {
                CHECK(&slots[0] == addresses[0]);
            }
        }

        CHECK(slots.Size() == 10 && MaterialSlot::Constructed == 10);
        for (uint32 index = 0; index < 10; ++index) {
            CHECK(&slots[index] == addresses[index] && slots[index].index == index);
            CHECK(slots[index].label.IsEmpty());
        }

        // 삽입은 색인만 민다(앞·가운데·끝).
        REQUIRE(objects.InsertElementAt(access, owner, slots, 0));
        slots[0].index = 100;
        REQUIRE(objects.InsertElementAt(access, owner, slots, 5));
        slots[5].index = 105;
        REQUIRE(objects.InsertElementAt(access, owner, slots, slots.Size()));
        slots[slots.Size() - 1].index = 200;
        CHECK(slots.Size() == 13 && &slots[1] == addresses[0] && &slots[6] == addresses[4]);
        CHECK(objects.InsertElementAt(access, owner, slots, 14).Error() ==
              ErrorCode::InvalidArgument);

        // 제거는 그 원소만 소멸하고 나머지 주소는 그대로다.
        const uint32 destroyedBefore = MaterialSlot::Destroyed;
        REQUIRE(objects.RemoveElementAt(access, owner, slots, 5));
        CHECK(MaterialSlot::Destroyed == destroyedBefore + 1);
        CHECK(slots.Size() == 12 && &slots[5] == addresses[4] && slots[5].index == 4);
        CHECK(objects.RemoveElementAt(access, owner, slots, 12).Error() ==
              ErrorCode::InvalidArgument);

        // 비우면 모든 원소를 소멸한다.
        REQUIRE(objects.ClearElements(access, owner, slots));
        CHECK(slots.IsEmpty() && MaterialSlot::Destroyed == MaterialSlot::Constructed);
        REQUIRE(objects.AddElement(access, owner, slots));
        borrowed.Value().Reset();
        holder.Value().Reset();
        REQUIRE(objects.Collect(access));
        // owner 소멸이 남은 원소를 소멸·반환한다.
        CHECK(MaterialSlot::Destroyed == MaterialSlot::Constructed);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(StructRefArray_ElementReferencesAreValidatedAndKeepTargetsAlive) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterAll(types));
        ObjectRegistry objects(allocator, types, {0x4352, 12});
        const auto access = objects.OwnerAccess();
        auto holder = objects.NewObject<Holder>(access, NativeTypeIdOf<Holder>());
        auto first = objects.NewObject<Item>(access, NativeTypeIdOf<Item>());
        auto second = objects.NewObject<Item>(access, NativeTypeIdOf<Item>());
        auto other = objects.NewObject<OtherItem>(access, NativeTypeIdOf<OtherItem>());
        REQUIRE(holder && first && second && other);
        const ObjectHandle owner = holder.Value().Handle();
        const ObjectHandle firstHandle = first.Value().Handle();
        const ObjectHandle secondHandle = second.Value().Handle();
        Holder* pointer = nullptr;
        {
            auto borrowed = objects.Resolve<Holder>(access, owner);
            REQUIRE(borrowed);
            pointer = Get(borrowed.Value());
            StructRefArray<MaterialSlot>& slots = pointer->slots;
            REQUIRE(objects.AddElement(access, owner, slots) &&
                    objects.AddElement(access, owner, slots));
            REQUIRE(objects.SetElementReference(access, owner, slots, 0, slots[0].material,
                                                firstHandle));
            REQUIRE(objects.SetElementReference(access, owner, slots, 1, slots[1].overlay,
                                                secondHandle));
            // 범위 밖: 다른 원소의 field, index ≥ size, owner 직접 멤버(원소 블록 밖).
            CHECK(
                objects.SetElementReference(access, owner, slots, 0, slots[1].material, firstHandle)
                    .Error() == ErrorCode::InvalidArgument);
            CHECK(
                objects.SetElementReference(access, owner, slots, 2, slots[1].material, firstHandle)
                    .Error() == ErrorCode::InvalidArgument);
            CHECK(objects.SetElementReference(access, owner, slots, 0, pointer->single, firstHandle)
                      .Error() == ErrorCode::InvalidArgument);
            // owner 밖 컨테이너는 InvalidArgument다.
            StructRefArray<MaterialSlot> local(allocator);
            CHECK(objects.AddElement(access, owner, local).Error() == ErrorCode::InvalidArgument);
            // 대상 타입 불일치, null(비정규 포함)은 허용한다.
            CHECK(objects
                      .SetElementReference(access, owner, slots, 0, slots[0].overlay,
                                           other.Value().Handle())
                      .Error() == ErrorCode::TypeMismatch);
            REQUIRE(objects.SetElementReference(access, owner, slots, 0, slots[0].overlay,
                                                ObjectHandle{firstHandle.registry, 1, 0}));
            CHECK(slots[0].overlay.Handle() == ObjectHandle{});
            CHECK(slots[0].material.Handle() == firstHandle &&
                  slots[1].overlay.Handle() == secondHandle);
        }

        // live SetElementReference·AddElement는 owner Borrow가 필수다.
        CHECK(objects
                  .SetElementReference(access, owner, pointer->slots, 0, pointer->slots[0].material,
                                       secondHandle)
                  .Error() == ErrorCode::InvalidArgument);
        CHECK(objects.AddElement(access, owner, pointer->slots).Error() ==
              ErrorCode::InvalidArgument);

        // 원소 참조가 대상을 보존하고, 원소를 지우면 대상이 회수된다.
        first.Value().Reset();
        second.Value().Reset();
        other.Value().Reset();
        REQUIRE(objects.Collect(access));
        CHECK(IsAlive(objects, firstHandle) && IsAlive(objects, secondHandle));
        {
            auto borrowed = objects.Resolve<Holder>(access, owner);
            REQUIRE(borrowed);
            REQUIRE(objects.RemoveElementAt(access, owner, pointer->slots, 0));
            CHECK(pointer->slots[0].overlay.Handle() == secondHandle);
        }

        REQUIRE(objects.Collect(access));
        CHECK(!IsAlive(objects, firstHandle) && IsAlive(objects, secondHandle));

        // 파괴 요청된 owner: 추가·참조 쓰기는 StaleHandle, 정리용 제거·비우기는 된다.
        {
            auto borrowed = objects.Resolve<Holder>(access, owner);
            REQUIRE(borrowed);
            REQUIRE(objects.AddElement(access, owner, pointer->slots));
            REQUIRE(objects.RequestDestroy(access, owner));
            CHECK(objects.AddElement(access, owner, pointer->slots).Error() ==
                  ErrorCode::StaleHandle);
            CHECK(objects.InsertElementAt(access, owner, pointer->slots, 0).Error() ==
                  ErrorCode::StaleHandle);
            CHECK(objects
                      .SetElementReference(access, owner, pointer->slots, 0,
                                           pointer->slots[0].material, ObjectHandle{})
                      .Error() == ErrorCode::StaleHandle);
            REQUIRE(objects.RemoveElementAt(access, owner, pointer->slots, 0));
            REQUIRE(objects.ClearElements(access, owner, pointer->slots));
            CHECK(pointer->slots.IsEmpty());
        }

        holder.Value().Reset();
        REQUIRE(objects.Collect(access));
        CHECK(objects.Stats().liveObjects == 0);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(StructRefArray_ReentrancyRejudgesCurrentState) {
    // 원소 생성자·allocator 콜백이 같은 컨테이너를 바꾸면 다시 잠근 뒤 그때의 size·index로 다시
    // 판단한다: 새 색인이 없거나 모자라거나 index가 size를 넘으면 원소를 소멸·반환하고 Busy다.
    HookAllocator allocator;
    MaterialSlot::Constructed = 0;
    MaterialSlot::Destroyed = 0;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterAll(types));
        ObjectRegistry objects(allocator, types, {0x4352, 13});
        const auto access = objects.OwnerAccess();
        struct Case {
            bool allocatorHook; // 거짓이면 원소 생성자 hook
            usize initial;
            usize insertAt; // ~0이면 AddElement
            uint32 adds;
            uint32 removes;
            ErrorCode expected;
            usize finalSize;
        };

        constexpr usize Append = ~usize(0);
        const Case cases[] = {
            {false, 4, Append, 5, 0, ErrorCode::None, 10},  // 그 사이 용량 16: 기존 색인에 넣는다
            {false, 4, Append, 12, 0, ErrorCode::Busy, 16}, // 새 색인(8)이 17을 못 담는다
            {false, 2, Append, 2, 0, ErrorCode::Busy, 4},   // 성장 계획이 없었는데 용량이 찼다
            {false, 4, 3, 0, 2, ErrorCode::Busy, 2},        // index 3 > 현재 size 2
            {false, 4, 1, 0, 1, ErrorCode::None, 4},        // 줄어든 뒤 index 1에 넣는다
            {true, 4, Append, 5, 0, ErrorCode::None, 10},   // 색인 할당 콜백이 늘렸다
            {true, 4, Append, 12, 0, ErrorCode::Busy, 16},
        };
        for (const Case& current : cases) {
            auto holder = objects.NewObject<Holder>(access, NativeTypeIdOf<Holder>());
            REQUIRE(holder);
            const ObjectHandle owner = holder.Value().Handle();
            auto borrowed = objects.Resolve<Holder>(access, owner);
            REQUIRE(borrowed);
            Holder* pointer = Get(borrowed.Value());
            for (usize element = 0; element < current.initial; ++element) {
                REQUIRE(objects.AddElement(access, owner, pointer->slots));
            }

            MaterialSlot* first = &pointer->slots[0];
            Reentry probe;
            probe.objects = &objects;
            probe.owner = owner;
            probe.holder = pointer;
            probe.adds = current.adds;
            probe.removes = current.removes;
            if (current.allocatorHook) {
                allocator.Arm(IndexTag, ReenterSlots, &probe);
            } else {
                MaterialSlot::HookContext = &probe;
                MaterialSlot::Hook = ReenterSlots;
            }

            const uint32 liveBefore = MaterialSlot::Constructed - MaterialSlot::Destroyed;
            Result<void> result = Result<void>::Success();
            if (current.insertAt == Append) {
                auto added = objects.AddElement(access, owner, pointer->slots);
                if (added) {
                    CHECK(added.Value() == current.finalSize - 1);
                } else {
                    result = Result<void>::Failure(added.Error());
                }
            } else {
                result = objects.InsertElementAt(access, owner, pointer->slots, current.insertAt);
            }

            CHECK(MaterialSlot::Hook == nullptr && allocator.hook == nullptr);
            CHECK(probe.last == ErrorCode::None);
            CHECK(ErrorOf(result) == current.expected);
            CHECK(pointer->slots.Size() == current.finalSize);
            // 실패하면 바깥 원소는 소멸했고, 살아 있는 원소 수는 컨테이너 크기와 같다.
            CHECK(MaterialSlot::Constructed - MaterialSlot::Destroyed ==
                  liveBefore + current.finalSize - current.initial);
            if (current.removes == 0) {
                CHECK(&pointer->slots[0] == first);
            }
        }
    }

    CHECK(MaterialSlot::Constructed == MaterialSlot::Destroyed);
    CHECK(allocator.backing.Stats().liveAllocations == 0);
}

TEST(StructRefArray_EveryAllocationFailureLeavesContentsUnchanged) {
    HookAllocator allocator;
    MaterialSlot::Constructed = 0;
    MaterialSlot::Destroyed = 0;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterAll(types));
        ObjectRegistry objects(allocator, types, {0x4352, 14});
        const auto access = objects.OwnerAccess();
        auto holder = objects.NewObject<Holder>(access, NativeTypeIdOf<Holder>());
        REQUIRE(holder);
        const ObjectHandle owner = holder.Value().Handle();
        auto borrowed = objects.Resolve<Holder>(access, owner);
        REQUIRE(borrowed);
        StructRefArray<MaterialSlot>& slots = Get(borrowed.Value())->slots;
        for (uint32 index = 0; index < 4; ++index) {
            REQUIRE(objects.AddElement(access, owner, slots));
            slots[index].index = index;
        }

        MaterialSlot* addresses[4] = {&slots[0], &slots[1], &slots[2], &slots[3]};
        const uint64 live = allocator.backing.Stats().liveAllocations;
        // 꽉 찬 색인: 원소 블록(1번째)·새 색인(2번째) 할당이 실패해도 불변이다.
        for (uint64 attempt = 1; attempt <= 2; ++attempt) {
            for (int insert = 0; insert < 2; ++insert) {
                allocator.FailContainerAllocation(attempt);
                ErrorCode result = ErrorCode::None;
                if (insert == 0) {
                    result = ErrorOf(objects.InsertElementAt(access, owner, slots, 0));
                } else {
                    auto added = objects.AddElement(access, owner, slots);
                    result = added ? ErrorCode::None : added.Error();
                }

                allocator.FailContainerAllocation(0);
                CHECK(result == ErrorCode::OutOfMemory);
                CHECK(slots.Size() == 4 && allocator.backing.Stats().liveAllocations == live);
                for (uint32 index = 0; index < 4; ++index) {
                    CHECK(&slots[index] == addresses[index] && slots[index].index == index);
                }
            }
        }

        CHECK(MaterialSlot::Constructed - MaterialSlot::Destroyed == 4);
        // 실패 뒤에도 정상으로 이어진다.
        REQUIRE(objects.InsertElementAt(access, owner, slots, 2));
        CHECK(slots.Size() == 5 && &slots[3] == addresses[2]);
        borrowed.Value().Reset();
        holder.Value().Reset();
        REQUIRE(objects.Collect(access));
    }

    CHECK(MaterialSlot::Constructed == MaterialSlot::Destroyed);
    CHECK(allocator.backing.Stats().liveAllocations == 0);
}

namespace {

struct StagedSlotProbe {
    ObjectRegistry* objects = nullptr;
    ObjectHandle target;
    ErrorCode resolved = ErrorCode::None;
};

void ResolveDuringStagedConstruct(void* raw) noexcept {
    auto& probe = *static_cast<StagedSlotProbe*>(raw);
    auto borrowed = probe.objects->Resolve<Item>(probe.objects->OwnerAccess(), probe.target);
    probe.resolved = borrowed ? ErrorCode::None : borrowed.Error();
}

} // namespace

TEST(ConstructionContext_AssignElementsIsAllOrNothing) {
    HookAllocator allocator;
    MaterialSlot::Constructed = 0;
    MaterialSlot::Destroyed = 0;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterAll(types));
        ObjectRegistry objects(allocator, types, {0x4352, 15});
        const auto access = objects.OwnerAccess();
        auto target = objects.NewObject<Item>(access, NativeTypeIdOf<Item>());
        auto committed = objects.NewObject<Holder>(access, NativeTypeIdOf<Holder>());
        REQUIRE(target && committed);
        const ObjectHandle targetHandle = target.Value().Handle();
        RootRef<Object> root;
        ObjectHandle stagedItem;
        {
            ConstructionContext transaction(objects, access);
            auto owner = transaction.Create<Holder>(NativeTypeIdOf<Holder>());
            auto item = transaction.Create<Item>(NativeTypeIdOf<Item>());
            REQUIRE(owner && item);
            stagedItem = item.Value();
            auto pointer = transaction.Get<Holder>(owner.Value());
            REQUIRE(pointer);
            StructRefArray<MaterialSlot>& slots = pointer.Value()->slots;
            // 게시된 owner는 StaleHandle이다.
            {
                auto borrowed = objects.Resolve<Holder>(access, committed.Value().Handle());
                REQUIRE(borrowed);
                CHECK(
                    transaction
                        .AssignElements(committed.Value().Handle(), Get(borrowed.Value())->slots, 2)
                        .Error() == ErrorCode::StaleHandle);
            }

            REQUIRE(transaction.AssignElements(owner.Value(), slots, 3));
            REQUIRE(slots.Size() == 3);
            MaterialSlot* addresses[3] = {&slots[0], &slots[1], &slots[2]};
            for (uint32 index = 0; index < 3; ++index) {
                slots[index].index = index;
            }

            // staged 원소 참조: 위치 오버로드와 (배열, index) 오버로드, 생존 대상·같은 트랜잭션의
            // Reserved 대상, 범위 밖 field는 InvalidArgument.
            REQUIRE(transaction.SetElementReference(
                owner.Value(), detail::StructRefArrayAccess::Location(slots, 0), slots[0].material,
                targetHandle));
            REQUIRE(transaction.SetElementReference(owner.Value(), slots, 1, slots[1].overlay,
                                                    stagedItem));
            CHECK(transaction
                      .SetElementReference(owner.Value(), slots, 2, slots[1].material, targetHandle)
                      .Error() == ErrorCode::InvalidArgument);
            CHECK(transaction
                      .SetElementReference(owner.Value(), slots, 3, slots[2].material, targetHandle)
                      .Error() == ErrorCode::InvalidArgument);

            // 할당 실패 스윕(색인 1 + 원소 5): 어느 것이 실패해도 만든 원소를 되돌리고 불변이다.
            for (uint64 attempt = 1; attempt <= 6; ++attempt) {
                const uint64 live = allocator.backing.Stats().liveAllocations;
                allocator.FailContainerAllocation(attempt);
                CHECK(transaction.AssignElements(owner.Value(), slots, 5).Error() ==
                      ErrorCode::OutOfMemory);
                allocator.FailContainerAllocation(0);
                CHECK(allocator.backing.Stats().liveAllocations == live);
                CHECK(slots.Size() == 3 &&
                      MaterialSlot::Constructed - MaterialSlot::Destroyed == 3);
                for (uint32 index = 0; index < 3; ++index) {
                    CHECK(&slots[index] == addresses[index] && slots[index].index == index);
                }

                CHECK(slots[0].material.Handle() == targetHandle &&
                      slots[1].overlay.Handle() == stagedItem);
            }

            // 원소 생성 중 재진입은 Busy다(처리 표시). 성공하면 옛 원소를 게시 뒤 소멸한다.
            StagedSlotProbe probe;
            probe.objects = &objects;
            probe.target = targetHandle;
            MaterialSlot::HookContext = &probe;
            MaterialSlot::Hook = ResolveDuringStagedConstruct;
            REQUIRE(transaction.AssignElements(owner.Value(), slots, 2));
            CHECK(MaterialSlot::Hook == nullptr && probe.resolved == ErrorCode::Busy);
            CHECK(slots.Size() == 2 && MaterialSlot::Constructed - MaterialSlot::Destroyed == 2);
            CHECK(slots[0].material.Handle() == ObjectHandle{});
            REQUIRE(transaction.SetElementReference(owner.Value(), slots, 1, slots[1].material,
                                                    targetHandle));
            REQUIRE(transaction.SetElementReference(owner.Value(), slots, 0, slots[0].overlay,
                                                    stagedItem));
            auto published = transaction.Commit();
            REQUIRE(published);
            root = Move(published.Value());
        }

        // Commit 뒤 원소 참조가 대상을 보존한다.
        target.Value().Reset();
        committed.Value().Reset();
        REQUIRE(objects.Collect(access));
        CHECK(IsAlive(objects, targetHandle) && IsAlive(objects, stagedItem));

        // rollback은 원소를 소멸하고 블록·색인을 반환한다.
        const uint64 live = allocator.backing.Stats().liveAllocations;
        {
            ConstructionContext transaction(objects, access);
            auto owner = transaction.Create<Holder>(NativeTypeIdOf<Holder>());
            REQUIRE(owner);
            auto pointer = transaction.Get<Holder>(owner.Value());
            REQUIRE(pointer);
            REQUIRE(transaction.AssignElements(owner.Value(), pointer.Value()->slots, 4));
        }

        CHECK(allocator.backing.Stats().liveAllocations == live);
        root.Reset();
        REQUIRE(objects.Collect(access));
        CHECK(objects.Stats().liveObjects == 0);
    }

    CHECK(MaterialSlot::Constructed == MaterialSlot::Destroyed);
    CHECK(allocator.backing.Stats().liveAllocations == 0);
}

TEST(StructRefArray_TenThousandElementsAllocateOncePerElement) {
    // 원소 1만 개를 한 번에 채우는 staged 경로(그래프 로드)의 컨테이너 할당은 원소 블록 1만 개와
    // 색인 하나다(원소마다 색인을 다시 할당하지 않는다).
    HookAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterAll(types));
        ObjectRegistry objects(allocator, types, {0x4352, 16});
        const auto access = objects.OwnerAccess();
        ConstructionContext transaction(objects, access);
        auto owner = transaction.Create<Holder>(NativeTypeIdOf<Holder>());
        REQUIRE(owner);
        auto pointer = transaction.Get<Holder>(owner.Value());
        REQUIRE(pointer);
        allocator.ResetCounts();
        REQUIRE(transaction.AssignElements(owner.Value(), pointer.Value()->slots, 10000));
        CHECK(pointer.Value()->slots.Size() == 10000);
        CHECK(allocator.elements == 10000 && allocator.indexes == 1 && allocator.refArrays == 0);
        transaction.Rollback();
    }

    CHECK(allocator.backing.Stats().liveAllocations == 0);
}

TEST(EncodePin_BlocksContainerAndReferenceChanges) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterAll(types));
        ObjectRegistry objects(allocator, types, {0x4352, 17});
        const auto access = objects.OwnerAccess();
        auto holder = objects.NewObject<Holder>(access, NativeTypeIdOf<Holder>());
        auto other = objects.NewObject<Holder>(access, NativeTypeIdOf<Holder>());
        auto item = objects.NewObject<Item>(access, NativeTypeIdOf<Item>());
        REQUIRE(holder && other && item);
        const ObjectHandle owner = holder.Value().Handle();
        const ObjectHandle target = item.Value().Handle();
        auto borrowed = objects.Resolve<Holder>(access, owner);
        auto otherBorrowed = objects.Resolve<Holder>(access, other.Value().Handle());
        REQUIRE(borrowed && otherBorrowed);
        Holder* pointer = Get(borrowed.Value());
        REQUIRE(objects.AddReference(access, owner, pointer->items, target));
        REQUIRE(objects.AddElement(access, owner, pointer->slots));
        auto first = objects.PinForEncode(access, owner);
        auto second = objects.PinForEncode(access, owner);
        REQUIRE(first && second && first.Value().IsValid());
        // 고정 계수는 통계·진단에 보인다.
        CHECK(objects.Stats().encodePins == 2);
        ObjectDiagnosticRecord records[8];
        auto captured = objects.CaptureDiagnostics(records);
        REQUIRE(captured);
        uint32 pinsSeen = 0;
        for (uint32 index = 0; index < captured.Value(); ++index) {
            pinsSeen += records[index].handle == owner ? records[index].encodePins : 0;
        }

        CHECK(pinsSeen == 2);
        // 고정 Busy는 호출 오류를 가리지 않는다: owner 밖 컨테이너·field는 InvalidArgument다.
        ObjectRefArray<Item> outside(allocator);
        ObjectRef<Item> outsideField;
        CHECK(objects.AddReference(access, owner, outside, target).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(objects.SetReference(access, owner, outsideField, target).Error() ==
              ErrorCode::InvalidArgument);
        auto expectBusy = [&]() noexcept {
            CHECK(objects.AddReference(access, owner, pointer->items, target).Error() ==
                  ErrorCode::Busy);
            CHECK(objects.InsertReferenceAt(access, owner, pointer->items, 0, target).Error() ==
                  ErrorCode::Busy);
            CHECK(objects.SetReferenceAt(access, owner, pointer->items, 0, {}).Error() ==
                  ErrorCode::Busy);
            CHECK(objects.RemoveReference(access, owner, pointer->items, target).Error() ==
                  ErrorCode::Busy);
            CHECK(objects.RemoveReferenceAt(access, owner, pointer->items, 0).Error() ==
                  ErrorCode::Busy);
            CHECK(objects.ClearReferences(access, owner, pointer->items).Error() ==
                  ErrorCode::Busy);
            CHECK(objects.SetReference(access, owner, pointer->single, target).Error() ==
                  ErrorCode::Busy);
            CHECK(objects.AddElement(access, owner, pointer->slots).Error() == ErrorCode::Busy);
            CHECK(objects.InsertElementAt(access, owner, pointer->slots, 0).Error() ==
                  ErrorCode::Busy);
            CHECK(objects.RemoveElementAt(access, owner, pointer->slots, 0).Error() ==
                  ErrorCode::Busy);
            CHECK(objects.ClearElements(access, owner, pointer->slots).Error() == ErrorCode::Busy);
            CHECK(objects
                      .SetElementReference(access, owner, pointer->slots, 0,
                                           pointer->slots[0].material, target)
                      .Error() == ErrorCode::Busy);
            CHECK(pointer->items.Size() == 1 && pointer->slots.Size() == 1 &&
                  pointer->single.Handle() == ObjectHandle{});
        };
        expectBusy();
        // 고정은 owner별이다: 다른 객체는 바꿀 수 있다.
        REQUIRE(objects.AddReference(access, other.Value().Handle(),
                                     Get(otherBorrowed.Value())->items, target));
        // 계수: 하나를 풀어도 남은 고정이 막고, 모두 풀면 다시 된다. 이동해도 계수는 하나다.
        EncodePin moved = Move(first.Value());
        CHECK(!first.Value().IsValid() && moved.IsValid());
        moved.Reset();
        expectBusy();
        second.Value().Reset();
        REQUIRE(objects.AddReference(access, owner, pointer->items, target));
        REQUIRE(objects.SetElementReference(access, owner, pointer->slots, 0,
                                            pointer->slots[0].material, target));

        // 생존 객체만 고정한다.
        CHECK(objects.PinForEncode(access, ObjectHandle{}).Error() == ErrorCode::StaleHandle);
        auto doomed = objects.NewObject<Item>(access, NativeTypeIdOf<Item>());
        REQUIRE(doomed);
        REQUIRE(objects.RequestDestroy(access, doomed.Value().Handle()));
        CHECK(objects.PinForEncode(access, doomed.Value().Handle()).Error() ==
              ErrorCode::Destroyed);
        doomed.Value().Reset();

        // 고정 중에는 도달하지 않게 된 객체를 retire해도 회수하지 않는다.
        auto pinnedItem = objects.NewObject<Item>(access, NativeTypeIdOf<Item>());
        REQUIRE(pinnedItem);
        const ObjectHandle pinnedHandle = pinnedItem.Value().Handle();
        auto pin = objects.PinForEncode(access, pinnedHandle);
        REQUIRE(pin);
        pinnedItem.Value().Reset();
        borrowed.Value().Reset();
        otherBorrowed.Value().Reset();
        REQUIRE(objects.Collect(access));
        CHECK(objects.Stats().retiredObjects == 1);
        pin.Value().Reset();
        auto drained = objects.DrainRetired(access);
        CHECK(drained && drained.Value() == 1);
        CHECK(objects.Stats().retiredObjects == 0);
        holder.Value().Reset();
        other.Value().Reset();
        item.Value().Reset();
        REQUIRE(objects.Collect(access));
        CHECK(objects.Stats().liveObjects == 0);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(EncodePin_LeakedPinKeepsShutdownBusy) {
    // 쥔 고정은 다른 보유권처럼 Shutdown을 Busy로 남기고, 풀면 Shutdown이 끝난다.
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterAll(types));
        ObjectRegistry objects(allocator, types, {0x4352, 18});
        const auto access = objects.OwnerAccess();
        auto item = objects.NewObject<Item>(access, NativeTypeIdOf<Item>());
        REQUIRE(item);
        auto pin = objects.PinForEncode(access, item.Value().Handle());
        REQUIRE(pin);
        item.Value().Reset();
        CHECK(objects.Shutdown(access).Error() == ErrorCode::Busy);
        CHECK(objects.Stats().encodePins == 1 && objects.Stats().retiredObjects == 1);
        pin.Value().Reset();
        CHECK(objects.Shutdown(access));
        CHECK(objects.Stats().encodePins == 0 && objects.Stats().retiredObjects == 0);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

namespace {

enum class RelockAction : uint8 {
    DestroyTarget,
    DestroyOwner,
    PinOwner,
    Shutdown
};

// 잠금 밖 할당 중 Registry 상태를 바꾼다(재잠금 뒤 재검증 시험).
struct RelockProbe {
    ObjectRegistry* objects = nullptr;
    ObjectHandle owner;
    ObjectHandle target;
    RelockAction action = RelockAction::DestroyTarget;
    EncodePin pin;
    ErrorCode result = ErrorCode::DependencyFailed;
};

void ChangeStateDuringAllocation(void* raw) noexcept {
    auto& probe = *static_cast<RelockProbe*>(raw);
    const auto access = probe.objects->OwnerAccess();
    switch (probe.action) {
    case RelockAction::DestroyTarget:
        probe.result = ErrorOf(probe.objects->RequestDestroy(access, probe.target));
        break;
    case RelockAction::DestroyOwner:
        probe.result = ErrorOf(probe.objects->RequestDestroy(access, probe.owner));
        break;
    case RelockAction::PinOwner: {
        auto pin = probe.objects->PinForEncode(access, probe.owner);
        probe.result = pin ? ErrorCode::None : pin.Error();
        if (pin) {
            probe.pin = Move(pin.Value());
        }

        break;
    }
    case RelockAction::Shutdown:
        // 호출자의 Borrow 때문에 Busy로 끝나지만 종료는 시작된다(closed).
        probe.result = ErrorOf(probe.objects->Shutdown(access));
        break;
    }
}

} // namespace

TEST(RegistryContainer_GrowthRevalidatesOwnerAndTargetAfterRelock) {
    // 잠금 밖 할당 중 콜백이 대상·owner를 파괴 요청하거나 owner를 고정하거나 Registry를 닫으면,
    // 다시 잠근 뒤의 검증이 각각 StaleHandle·StaleHandle·Busy·ShuttingDown으로 거부하고 내용과
    // 할당은 그대로다(ObjectRefArray 새 버퍼, StructRefArray 원소·새 색인을 반환한다).
    struct Case {
        bool structElements;
        RelockAction action;
        ErrorCode expected;
        ErrorCode hookResult;
    };

    const Case cases[] = {
        {false, RelockAction::DestroyTarget, ErrorCode::StaleHandle, ErrorCode::None},
        {false, RelockAction::DestroyOwner, ErrorCode::StaleHandle, ErrorCode::None},
        {false, RelockAction::PinOwner, ErrorCode::Busy, ErrorCode::None},
        {false, RelockAction::Shutdown, ErrorCode::ShuttingDown, ErrorCode::Busy},
        {true, RelockAction::DestroyOwner, ErrorCode::StaleHandle, ErrorCode::None},
        {true, RelockAction::PinOwner, ErrorCode::Busy, ErrorCode::None},
        {true, RelockAction::Shutdown, ErrorCode::ShuttingDown, ErrorCode::Busy},
    };
    uint16 context = 30;
    MaterialSlot::Constructed = 0;
    MaterialSlot::Destroyed = 0;
    for (const Case& current : cases) {
        HookAllocator allocator;
        {
            TypeRegistry types(allocator);
            REQUIRE(RegisterAll(types));
            ObjectRegistry objects(allocator, types, {0x4352, context++});
            const auto access = objects.OwnerAccess();
            auto holder = objects.NewObject<Holder>(access, NativeTypeIdOf<Holder>());
            auto target = objects.NewObject<Item>(access, NativeTypeIdOf<Item>());
            REQUIRE(holder && target);
            const ObjectHandle owner = holder.Value().Handle();
            RelockProbe probe;
            {
                auto borrowed = objects.Resolve<Holder>(access, owner);
                REQUIRE(borrowed);
                Holder* pointer = Get(borrowed.Value());
                for (uint32 index = 0; index < 4; ++index) {
                    if (current.structElements) {
                        REQUIRE(objects.AddElement(access, owner, pointer->slots));
                    } else {
                        REQUIRE(objects.AddReference(access, owner, pointer->items,
                                                     target.Value().Handle()));
                    }
                }

                const uint64 live = allocator.backing.Stats().liveAllocations;
                const uint32 liveElements = MaterialSlot::Constructed - MaterialSlot::Destroyed;
                probe.objects = &objects;
                probe.owner = owner;
                probe.target = target.Value().Handle();
                probe.action = current.action;
                allocator.Arm(current.structElements ? IndexTag : RefArrayTag,
                              ChangeStateDuringAllocation, &probe);
                ErrorCode result = ErrorCode::None;
                if (current.structElements) {
                    auto added = objects.AddElement(access, owner, pointer->slots);
                    result = added ? ErrorCode::None : added.Error();
                } else {
                    result = ErrorOf(objects.AddReference(access, owner, pointer->items,
                                                          target.Value().Handle()));
                }

                CHECK(allocator.hook == nullptr);
                CHECK(probe.result == current.hookResult);
                CHECK(result == current.expected);
                CHECK(allocator.backing.Stats().liveAllocations == live);
                CHECK(MaterialSlot::Constructed - MaterialSlot::Destroyed == liveElements);
                CHECK((current.structElements ? pointer->slots.Size() : pointer->items.Size()) ==
                      4);
                probe.pin.Reset();
            }

            holder.Value().Reset();
            target.Value().Reset();
        }

        CHECK(allocator.backing.Stats().liveAllocations == 0);
    }

    CHECK(MaterialSlot::Constructed == MaterialSlot::Destroyed);
}

TEST(StructRefArray_ElementReferencesFollowIncrementalGcRules) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterAll(types));
        ObjectRegistry objects(allocator, types, {0x4352, 19});
        const auto access = objects.OwnerAccess();
        // holder가 slot 0이라 첫 slice에서 검게 된다. 패딩 루트가 Marking을 이어 가게 한다.
        auto holder = objects.NewObject<Holder>(access, NativeTypeIdOf<Holder>());
        auto first = objects.NewObject<Item>(access, NativeTypeIdOf<Item>());
        auto second = objects.NewObject<Item>(access, NativeTypeIdOf<Item>());
        REQUIRE(holder && first && second);
        const ObjectHandle owner = holder.Value().Handle();
        const ObjectHandle white = Unrooted(objects);
        const ObjectHandle garbage = Unrooted(objects);
        {
            auto borrowed = objects.Resolve<Holder>(access, owner);
            REQUIRE(borrowed);
            REQUIRE(objects.AddElement(access, owner, Get(borrowed.Value())->slots));
        }

        // (a) 증분 Marking 중 검은 owner의 원소에 흰 대상을 쓰면 shade되어 살아남는다.
        REQUIRE(objects.BeginCollection(access));
        REQUIRE(StepOnce(objects));
        REQUIRE(objects.CollectionPhase() == GcPhase::Marking);
        {
            auto borrowed = objects.Resolve<Holder>(access, owner);
            REQUIRE(borrowed);
            Holder* pointer = Get(borrowed.Value());
            REQUIRE(objects.SetElementReference(access, owner, pointer->slots, 0,
                                                pointer->slots[0].material, white));
        }

        REQUIRE(FinishCollection(objects));
        CHECK(IsAlive(objects, white) && !IsAlive(objects, garbage));

        // (b) Sweeping cutoff 뒤의 흰 객체는 원소 참조로 되살릴 수 없다.
        const ObjectHandle late = Unrooted(objects);
        REQUIRE(objects.BeginCollection(access));
        for (uint32 guard = 0; guard < 64 && objects.CollectionPhase() == GcPhase::Marking;
             ++guard) {
            REQUIRE(StepOnce(objects));
        }

        REQUIRE(objects.CollectionPhase() == GcPhase::Sweeping);
        {
            auto borrowed = objects.Resolve<Holder>(access, owner);
            REQUIRE(borrowed);
            Holder* pointer = Get(borrowed.Value());
            CHECK(objects
                      .SetElementReference(access, owner, pointer->slots, 0,
                                           pointer->slots[0].overlay, late)
                      .Error() == ErrorCode::StaleHandle);
            CHECK(pointer->slots[0].overlay.Handle() == ObjectHandle{});
        }

        REQUIRE(FinishCollection(objects));
        CHECK(!IsAlive(objects, late) && IsAlive(objects, white));

        // (c) staged 원소 참조의 대상이 Commit 전에 파괴 요청되면 Commit의 staged 간선 검증(owner
        // Trace가 원소를 방문)이 StaleHandle로 거부한다.
        {
            ConstructionContext transaction(objects, access);
            auto staged = transaction.Create<Holder>(NativeTypeIdOf<Holder>());
            REQUIRE(staged);
            auto pointer = transaction.Get<Holder>(staged.Value());
            REQUIRE(pointer);
            StructRefArray<MaterialSlot>& slots = pointer.Value()->slots;
            REQUIRE(transaction.AssignElements(staged.Value(), slots, 2));
            REQUIRE(transaction.SetElementReference(staged.Value(), slots, 1, slots[1].overlay,
                                                    first.Value().Handle()));
            REQUIRE(objects.RequestDestroy(access, first.Value().Handle()));
            CHECK(transaction.Commit().Error() == ErrorCode::StaleHandle);
            CHECK(objects.Stats().reservedObjects == 1);
        }

        CHECK(objects.Stats().reservedObjects == 0);
        holder.Value().Reset();
        first.Value().Reset();
        second.Value().Reset();
        REQUIRE(objects.Collect(access));
        CHECK(objects.Stats().liveObjects == 0);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}
