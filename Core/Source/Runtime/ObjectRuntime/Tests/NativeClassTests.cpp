#include <Core/Memory/SystemAllocator.h>
#include <Object/NativeClass.h>
#include <Object/ObjectRegistry.h>
#include <TestSupport/Test.h>

using namespace rocks;

namespace {

uint32 nativeDestructions = 0;

class NativeBase : public Object {
    ROCKS_NATIVE_CLASS(NativeBase, Object, 0x57ca819b95494dd8ull, 0x8218eb5a2683d903ull)

public:
    NativeBase() noexcept = default;

    virtual ~NativeBase() noexcept {
        ++nativeDestructions;
    }

    virtual int32 Value() const noexcept {
        return 1;
    }

    int32 baseField = 7;
};

class NativeDerived final : public NativeBase {
    ROCKS_NATIVE_CLASS(NativeDerived, NativeBase, 0xb73983e396224842ull, 0x9e8910effde52fc8ull)

public:
    int32 Value() const noexcept override {
        return 2;
    }

    double extra = 3.0;
};

class NativeOther final : public Object {
    ROCKS_NATIVE_CLASS(NativeOther, Object, 0x6e48877d1bbe4adaull, 0x8ea5b47fb588fc8bull)
};

class NativeHolder final : public Object {
    ROCKS_NATIVE_CLASS(NativeHolder, Object, 0xb91bc41cc98a4792ull, 0xa53ae998473352d9ull)

public:
    explicit NativeHolder(Allocator& allocator) noexcept : items(allocator) {
    }

    void Trace(TraceVisitor& visitor) const noexcept {
        visitor.Visit(single);
        items.Trace(visitor);
    }

    ObjectRef<NativeBase> single;
    ObjectRefArray<NativeBase> items;
};

// 기반보다 작은 타입에 기반 키를 수동으로 붙인 잘못된 함수표다(등록에서 거부되어야 한다).
struct SmallFake final : Object {};

constexpr TypeId SmallFakeId{0xca0ad244c63b49f6ull, 0xb80e6e111e21e3abull};

bool RegisterAll(TypeRegistry& types) noexcept {
    return RegisterNativeClass<NativeBase>(types).IsOk() &&
           RegisterNativeClass<NativeDerived>(types).IsOk() &&
           RegisterNativeClass<NativeOther>(types).IsOk() &&
           RegisterNativeClass<NativeHolder>(types).IsOk();
}

template <class T>
T* Get(Borrow<T>& borrow) noexcept {
    auto pointer = borrow.Get();
    return pointer ? pointer.Value() : nullptr;
}

} // namespace

TEST(NativeClass_RegistrationRequiresBaseFirstAndRejectsDuplicates) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    CHECK(RegisterNativeClass<NativeDerived>(types).Error() == ErrorCode::NotFound);
    REQUIRE(RegisterNativeClass<NativeBase>(types));
    REQUIRE(RegisterNativeClass<NativeDerived>(types));
    CHECK(RegisterNativeClass<NativeBase>(types).Error() == ErrorCode::AlreadyExists);
    auto lease = types.Acquire(NativeTypeIdOf<NativeDerived>());
    REQUIRE(lease);
    CHECK(lease.Value().IsA(NativeTypeIdOf<NativeBase>()));
    CHECK(lease.Value().IsNativeA(NativeTypeKey<NativeBase>()));
    CHECK(lease.Value().IsNativeA(nullptr));
    CHECK(!lease.Value().IsNativeA(NativeTypeKey<NativeOther>()));
    CHECK(lease.Value().Name() == StringView("NativeDerived"));
}

TEST(NativeClass_ManualBaseKeyOnSmallerTypeIsRejected) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterAll(types));
    TypeSchema schema;
    schema.id = SmallFakeId;
    schema.name = Span<const char>("SmallFake", 9);
    schema.baseType = NativeTypeIdOf<NativeDerived>();
    RuntimeTypeOps ops = MakeRuntimeTypeOps<SmallFake>();
    ops.nativeBaseType = NativeTypeKey<NativeDerived>();
    CHECK(types.Register(schema, Move(ops)).Error() == ErrorCode::TypeMismatch);
}

TEST(NativeClass_ResolveAcceptsBaseAndRejectsUnrelatedOrNarrower) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterAll(types));
        ObjectRegistry objects(allocator, types, {0x4e43, 1});
        const auto access = objects.OwnerAccess();
        auto derived = objects.NewObject<NativeBase>(access, NativeTypeIdOf<NativeDerived>());
        auto base = objects.NewObject<NativeBase>(access, NativeTypeIdOf<NativeBase>());
        REQUIRE(derived && base);
        CHECK(objects.NewObject<NativeDerived>(access, NativeTypeIdOf<NativeBase>()).Error() ==
              ErrorCode::TypeMismatch);
        CHECK(objects.NewObject<NativeOther>(access, NativeTypeIdOf<NativeDerived>()).Error() ==
              ErrorCode::TypeMismatch);
        {
            auto asBase = objects.Resolve<NativeBase>(access, derived.Value().Handle());
            REQUIRE(asBase);
            NativeBase* pointer = Get(asBase.Value());
            REQUIRE(pointer != nullptr);
            CHECK(pointer->Value() == 2 && pointer->baseField == 7);
            // 기반 객체를 파생 타입으로, 파생 객체를 관계없는 타입으로 Resolve하면 거부한다.
            CHECK(objects.Resolve<NativeDerived>(access, base.Value().Handle()).Error() ==
                  ErrorCode::TypeMismatch);
            CHECK(objects.Resolve<NativeOther>(access, derived.Value().Handle()).Error() ==
                  ErrorCode::TypeMismatch);
        }
        auto pinned = objects.TryPin<NativeBase>(access, derived.Value().Handle());
        CHECK(pinned.IsOk());
        CHECK(objects.TryPin<NativeDerived>(access, base.Value().Handle()).Error() ==
              ErrorCode::TypeMismatch);
        pinned.Value().Reset();
        derived.Value().Reset();
        base.Value().Reset();
        REQUIRE(objects.Collect(access));
        CHECK(objects.Stats().liveObjects == 0);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(NativeClass_SetReferenceAcceptsDerivedTarget) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterAll(types));
        ObjectRegistry objects(allocator, types, {0x4e43, 2});
        const auto access = objects.OwnerAccess();
        auto holder = objects.NewObject<NativeHolder>(access, NativeTypeIdOf<NativeHolder>());
        auto derived = objects.NewObject<NativeDerived>(access, NativeTypeIdOf<NativeDerived>());
        auto other = objects.NewObject<NativeOther>(access, NativeTypeIdOf<NativeOther>());
        REQUIRE(holder && derived && other);
        {
            auto borrowed = objects.Resolve<NativeHolder>(access, holder.Value().Handle());
            REQUIRE(borrowed);
            NativeHolder* pointer = Get(borrowed.Value());
            REQUIRE(pointer != nullptr);
            REQUIRE(objects.SetReference(access, holder.Value().Handle(), pointer->single,
                                         derived.Value().Handle()));
            CHECK(objects
                      .SetReference(access, holder.Value().Handle(), pointer->single,
                                    other.Value().Handle())
                      .Error() == ErrorCode::TypeMismatch);
            CHECK(pointer->single.Handle() == derived.Value().Handle());
        }
        // 강한 간선만 남은 파생 객체는 수집 후에도 살아 있다.
        derived.Value().Reset();
        other.Value().Reset();
        REQUIRE(objects.Collect(access));
        CHECK(objects.Stats().liveObjects == 2);
        holder.Value().Reset();
        REQUIRE(objects.Collect(access));
        CHECK(objects.Stats().liveObjects == 0);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ObjectRefArray_AddRemoveGrowAndKeepTargetsAlive) {
    SystemAllocator allocator;
    nativeDestructions = 0;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterAll(types));
        ObjectRegistry objects(allocator, types, {0x4e43, 3});
        const auto access = objects.OwnerAccess();
        auto holder = objects.NewObject<NativeHolder>(access, NativeTypeIdOf<NativeHolder>());
        REQUIRE(holder);
        const ObjectHandle owner = holder.Value().Handle();
        ObjectHandle items[10];
        {
            auto borrowed = objects.Resolve<NativeHolder>(access, owner);
            REQUIRE(borrowed);
            NativeHolder* pointer = Get(borrowed.Value());
            REQUIRE(pointer != nullptr);
            // 초기 용량(4)을 넘겨 버퍼 확장과 기존 원소 보존을 확인한다.
            for (ObjectHandle& item : items) {
                auto created =
                    objects.NewObject<NativeBase>(access, NativeTypeIdOf<NativeDerived>());
                REQUIRE(created);
                item = created.Value().Handle();
                REQUIRE(objects.AddReference(access, owner, pointer->items, item));
            }
            CHECK(pointer->items.Size() == 10);
            CHECK(pointer->items[0] == items[0] && pointer->items[9] == items[9]);
            CHECK(pointer->items.Contains(items[5]) && pointer->items.IndexOf(items[5]) == 5);
            // 같은 대상을 다시 담을 수 있다.
            REQUIRE(objects.AddReference(access, owner, pointer->items, items[0]));
            CHECK(pointer->items.Size() == 11);
        }
        // 루트를 모두 놓아도 배열 간선이 대상을 보존한다.
        REQUIRE(objects.Collect(access));
        CHECK(objects.Stats().liveObjects == 11);
        {
            auto borrowed = objects.Resolve<NativeHolder>(access, owner);
            REQUIRE(borrowed);
            NativeHolder* pointer = Get(borrowed.Value());
            REQUIRE(pointer != nullptr);
            REQUIRE(objects.RemoveReference(access, owner, pointer->items, items[3]));
            CHECK(pointer->items.Size() == 10 && pointer->items[3] == items[4]);
            CHECK(objects.RemoveReference(access, owner, pointer->items, items[3]).Error() ==
                  ErrorCode::NotFound);
            REQUIRE(objects.RemoveReferenceAt(access, owner, pointer->items, 0));
            CHECK(pointer->items[0] == items[1]);
            CHECK(objects.RemoveReferenceAt(access, owner, pointer->items, 99).Error() ==
                  ErrorCode::InvalidArgument);
        }
        // items[3]은 회수되고, items[0]은 끝에 다시 담은 원소가 남아 있어 보존된다.
        REQUIRE(objects.Collect(access));
        CHECK(objects.Stats().liveObjects == 10);
        CHECK(nativeDestructions == 1);
        CHECK(objects.Resolve<NativeBase>(access, items[3]).Error() == ErrorCode::StaleHandle);
        CHECK(objects.Resolve<NativeBase>(access, items[0]).IsOk());
        {
            auto borrowed = objects.Resolve<NativeHolder>(access, owner);
            REQUIRE(borrowed);
            NativeHolder* pointer = Get(borrowed.Value());
            REQUIRE(pointer != nullptr);
            REQUIRE(objects.ClearReferences(access, owner, pointer->items));
            CHECK(pointer->items.IsEmpty());
        }
        REQUIRE(objects.Collect(access));
        CHECK(objects.Stats().liveObjects == 1);
        holder.Value().Reset();
        REQUIRE(objects.Collect(access));
        CHECK(objects.Stats().liveObjects == 0);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ObjectRefArray_RejectsInvalidTargetsAndForeignStorage) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterAll(types));
        ObjectRegistry objects(allocator, types, {0x4e43, 4});
        ObjectRegistry foreign(allocator, types, {0x4e43, 5});
        const auto access = objects.OwnerAccess();
        auto holder = objects.NewObject<NativeHolder>(access, NativeTypeIdOf<NativeHolder>());
        auto other = objects.NewObject<NativeOther>(access, NativeTypeIdOf<NativeOther>());
        auto outsider =
            foreign.NewObject<NativeBase>(foreign.OwnerAccess(), NativeTypeIdOf<NativeBase>());
        auto victim = objects.NewObject<NativeBase>(access, NativeTypeIdOf<NativeBase>());
        REQUIRE(holder && other && outsider && victim);
        const ObjectHandle owner = holder.Value().Handle();
        {
            auto borrowed = objects.Resolve<NativeHolder>(access, owner);
            REQUIRE(borrowed);
            NativeHolder* pointer = Get(borrowed.Value());
            REQUIRE(pointer != nullptr);
            CHECK(objects.AddReference(access, owner, pointer->items, ObjectHandle{}).Error() ==
                  ErrorCode::InvalidArgument);
            CHECK(objects.AddReference(access, owner, pointer->items, other.Value().Handle())
                      .Error() == ErrorCode::TypeMismatch);
            CHECK(objects.AddReference(access, owner, pointer->items, outsider.Value().Handle())
                      .Error() == ErrorCode::StaleHandle);
            REQUIRE(objects.RequestDestroy(access, victim.Value().Handle()));
            CHECK(objects.AddReference(access, owner, pointer->items, victim.Value().Handle())
                      .Error() == ErrorCode::StaleHandle);
            // 소유 객체 밖(스택)의 배열은 거부한다.
            ObjectRefArray<NativeBase> local(allocator);
            auto valid = objects.NewObject<NativeBase>(access, NativeTypeIdOf<NativeBase>());
            REQUIRE(valid);
            CHECK(objects.AddReference(access, owner, local, valid.Value().Handle()).Error() ==
                  ErrorCode::InvalidArgument);
            CHECK(pointer->items.IsEmpty() && local.IsEmpty());
            // 배열이 그 객체 안에 없거나 Borrow 없는 소유자는 InvalidArgument, 빈 handle은
            // StaleHandle이다.
            CHECK(objects
                      .AddReference(access, other.Value().Handle(), pointer->items,
                                    valid.Value().Handle())
                      .Error() == ErrorCode::InvalidArgument);
            CHECK(
                objects.AddReference(access, ObjectHandle{}, pointer->items, valid.Value().Handle())
                    .Error() == ErrorCode::StaleHandle);
        }
        // 파괴 요청된 소유자에서는 추가할 수 없지만 정리용 제거는 허용한다.
        auto item = objects.NewObject<NativeBase>(access, NativeTypeIdOf<NativeBase>());
        REQUIRE(item);
        {
            auto borrowed = objects.Resolve<NativeHolder>(access, owner);
            REQUIRE(borrowed);
            NativeHolder* pointer = Get(borrowed.Value());
            REQUIRE(pointer != nullptr);
            REQUIRE(objects.AddReference(access, owner, pointer->items, item.Value().Handle()));
            REQUIRE(objects.RequestDestroy(access, owner));
            CHECK(objects.AddReference(access, owner, pointer->items, item.Value().Handle())
                      .Error() == ErrorCode::StaleHandle);
            CHECK(objects.ClearReferences(access, owner, pointer->items).IsOk());
        }
        holder.Value().Reset();
        other.Value().Reset();
        outsider.Value().Reset();
        victim.Value().Reset();
        item.Value().Reset();
        REQUIRE(objects.Collect(access));
        REQUIRE(foreign.Collect(foreign.OwnerAccess()));
        CHECK(objects.Stats().liveObjects == 0 && foreign.Stats().liveObjects == 0);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ObjectRefArray_InsertionBarrierPreservesTargetsMovedDuringIncrementalMarking) {
    // 두 holder 사이에서 대상을 옮기는 사이 Marking slice가 진행되어도 도달 가능한 대상은 회수되지
    // 않아야 한다. 어느 holder가 먼저 스캔되든 삽입 장벽이 옮겨 간 대상을 shade한다.
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterAll(types));
        ObjectRegistry objects(allocator, types, {0x4e43, 6});
        const auto access = objects.OwnerAccess();
        auto first = objects.NewObject<NativeHolder>(access, NativeTypeIdOf<NativeHolder>());
        auto second = objects.NewObject<NativeHolder>(access, NativeTypeIdOf<NativeHolder>());
        REQUIRE(first && second);
        ObjectHandle inFirst[8];
        ObjectHandle inSecond[8];
        auto fill = [&](ObjectHandle owner, ObjectHandle(&targets)[8]) noexcept {
            auto borrowed = objects.Resolve<NativeHolder>(access, owner);
            NativeHolder* pointer = borrowed ? Get(borrowed.Value()) : nullptr;
            if (pointer == nullptr) {
                return false;
            }
            for (ObjectHandle& target : targets) {
                auto created = objects.NewObject<NativeBase>(access, NativeTypeIdOf<NativeBase>());
                if (!created || !objects.AddReference(access, owner, pointer->items,
                                                      created.Value().Handle())) {
                    return false;
                }
                target = created.Value().Handle();
            }
            return true;
        };
        REQUIRE(fill(first.Value().Handle(), inFirst));
        REQUIRE(fill(second.Value().Handle(), inSecond));
        REQUIRE(objects.BeginCollection(access));
        GcBudget budget;
        budget.maxObjects = 1;
        budget.maxNanoseconds = 0;
        auto stepped = objects.StepCollection(access, budget);
        REQUIRE(stepped);
        REQUIRE(objects.CollectionPhase() == GcPhase::Marking);
        auto move = [&](ObjectHandle from, ObjectHandle to, ObjectHandle target) noexcept {
            auto source = objects.Resolve<NativeHolder>(access, from);
            auto destination = objects.Resolve<NativeHolder>(access, to);
            NativeHolder* sourcePointer = source ? Get(source.Value()) : nullptr;
            NativeHolder* destinationPointer = destination ? Get(destination.Value()) : nullptr;
            return sourcePointer != nullptr && destinationPointer != nullptr &&
                   objects.AddReference(access, to, destinationPointer->items, target).IsOk() &&
                   objects.RemoveReference(access, from, sourcePointer->items, target).IsOk();
        };
        for (uint32 index = 0; index < 8; ++index) {
            REQUIRE(move(second.Value().Handle(), first.Value().Handle(), inSecond[index]));
            REQUIRE(move(first.Value().Handle(), second.Value().Handle(), inFirst[index]));
        }
        for (uint32 guard = 0; guard < 1000 && objects.CollectionPhase() != GcPhase::Idle;
             ++guard) {
            REQUIRE(objects.StepCollection(access, budget));
        }
        CHECK(objects.CollectionPhase() == GcPhase::Idle);
        CHECK(objects.Stats().liveObjects == 18);
        for (uint32 index = 0; index < 8; ++index) {
            CHECK(objects.Resolve<NativeBase>(access, inFirst[index]).IsOk());
            CHECK(objects.Resolve<NativeBase>(access, inSecond[index]).IsOk());
        }
        first.Value().Reset();
        second.Value().Reset();
        REQUIRE(objects.Collect(access));
        CHECK(objects.Stats().liveObjects == 0);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

namespace {

// ObjectRefArray 버퍼 태그(0x4f524146) 할당을 정해진 횟수 뒤부터 실패시키는 allocator다.
class ArrayFailingAllocator final : public Allocator {
public:
    explicit ArrayFailingAllocator(uint32 allowedArrayAllocations) noexcept
        : allowed_(allowedArrayAllocations) {
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag) noexcept override {
        if (tag == 0x4f524146u) {
            if (allowed_ == 0) {
                return Result<void*>::Failure(ErrorCode::OutOfMemory);
            }
            --allowed_;
        }
        return backing_.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        return backing_.Deallocate(address);
    }

    SystemAllocator backing_;

private:
    uint32 allowed_;
};

ObjectRegistry* reentrantRegistry = nullptr;
ObjectHandle reentrantTarget;
ErrorCode reentrantResult = ErrorCode::None;

// Trace 안에서 배열 변경을 시도하는 잘못된 타입이다(Registry가 Busy로 거부해야 한다).
class ReentrantHolder final : public Object {
    ROCKS_NATIVE_CLASS(ReentrantHolder, Object, 0x5c1d2e3f4a5b6c7dull, 0x8e9fa0b1c2d3e4f5ull)

public:
    explicit ReentrantHolder(Allocator& allocator) noexcept : items(allocator) {
    }

    void Trace(TraceVisitor& visitor) const noexcept {
        items.Trace(visitor);
        if (reentrantRegistry != nullptr) {
            auto& mutableItems = const_cast<ObjectRefArray<NativeBase>&>(items);
            reentrantResult = reentrantRegistry
                                  ->AddReference(reentrantRegistry->OwnerAccess(), Handle(),
                                                 mutableItems, reentrantTarget)
                                  .Error();
        }
    }

    ObjectRefArray<NativeBase> items;
};

} // namespace

TEST(ObjectRefArray_RequiresOwnerBorrow) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterAll(types));
        ObjectRegistry objects(allocator, types, {0x4e43, 7});
        const auto access = objects.OwnerAccess();
        auto holder = objects.NewObject<NativeHolder>(access, NativeTypeIdOf<NativeHolder>());
        auto item = objects.NewObject<NativeBase>(access, NativeTypeIdOf<NativeBase>());
        REQUIRE(holder && item);
        NativeHolder* pointer = nullptr;
        {
            auto borrowed = objects.Resolve<NativeHolder>(access, holder.Value().Handle());
            REQUIRE(borrowed);
            pointer = Get(borrowed.Value());
            REQUIRE(pointer != nullptr);
        }
        // Borrow를 놓은 뒤의 변경은 계약 위반이다(루트가 저장소를 살려 두므로 포인터 자체는 유효).
        CHECK(objects
                  .AddReference(access, holder.Value().Handle(), pointer->items,
                                item.Value().Handle())
                  .Error() == ErrorCode::InvalidArgument);
        CHECK(objects.ClearReferences(access, holder.Value().Handle(), pointer->items).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(pointer->items.IsEmpty());
        holder.Value().Reset();
        item.Value().Reset();
        REQUIRE(objects.Collect(access));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ObjectRefArray_RejectsCutoffTargetDuringSweeping) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterAll(types));
        ObjectRegistry objects(allocator, types, {0x4e43, 8});
        const auto access = objects.OwnerAccess();
        auto holder = objects.NewObject<NativeHolder>(access, NativeTypeIdOf<NativeHolder>());
        REQUIRE(holder);
        ObjectHandle white;
        ObjectHandle other;
        {
            auto first = objects.NewObject<NativeBase>(access, NativeTypeIdOf<NativeBase>());
            auto second = objects.NewObject<NativeBase>(access, NativeTypeIdOf<NativeBase>());
            REQUIRE(first && second);
            white = first.Value().Handle();
            other = second.Value().Handle();
        }
        REQUIRE(objects.BeginCollection(access));
        GcBudget budget;
        budget.maxObjects = 1;
        budget.maxNanoseconds = 0;
        for (uint32 guard = 0; guard < 16 && objects.CollectionPhase() == GcPhase::Marking;
             ++guard) {
            REQUIRE(objects.StepCollection(access, budget));
        }
        REQUIRE(objects.CollectionPhase() == GcPhase::Sweeping);
        {
            // cutoff 이후 흰 객체는 간선으로 되살릴 수 없다.
            auto borrowed = objects.Resolve<NativeHolder>(access, holder.Value().Handle());
            REQUIRE(borrowed);
            NativeHolder* pointer = Get(borrowed.Value());
            REQUIRE(pointer != nullptr);
            CHECK(objects.AddReference(access, holder.Value().Handle(), pointer->items, white)
                      .Error() == ErrorCode::StaleHandle);
            CHECK(pointer->items.IsEmpty());
            CHECK(!objects.Resolve<NativeBase>(access, other).IsOk());
        }
        // maxObjects=1이면 sweep이 슬롯마다 한 단위를 쓰므로 chunk(64슬롯)보다 넉넉한 상한을 둔다.
        for (uint32 guard = 0; guard < 1000 && objects.CollectionPhase() != GcPhase::Idle;
             ++guard) {
            REQUIRE(objects.StepCollection(access, budget));
        }
        CHECK(objects.CollectionPhase() == GcPhase::Idle);
        CHECK(objects.Stats().liveObjects == 1);
        holder.Value().Reset();
        REQUIRE(objects.Collect(access));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ObjectRefArray_MutationFromTraceIsBusy) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterAll(types));
        REQUIRE(RegisterNativeClass<ReentrantHolder>(types));
        ObjectRegistry objects(allocator, types, {0x4e43, 9});
        const auto access = objects.OwnerAccess();
        auto holder = objects.NewObject<ReentrantHolder>(access, NativeTypeIdOf<ReentrantHolder>());
        auto item = objects.NewObject<NativeBase>(access, NativeTypeIdOf<NativeBase>());
        REQUIRE(holder && item);
        reentrantRegistry = &objects;
        reentrantTarget = item.Value().Handle();
        reentrantResult = ErrorCode::None;
        REQUIRE(objects.Collect(access));
        reentrantRegistry = nullptr;
        CHECK(reentrantResult == ErrorCode::Busy);
        holder.Value().Reset();
        item.Value().Reset();
        REQUIRE(objects.Collect(access));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ObjectRefArray_GrowthFailureKeepsExistingElements) {
    ArrayFailingAllocator allocator(1);
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterAll(types));
        ObjectRegistry objects(allocator, types, {0x4e43, 10});
        const auto access = objects.OwnerAccess();
        auto holder = objects.NewObject<NativeHolder>(access, NativeTypeIdOf<NativeHolder>());
        REQUIRE(holder);
        RootRef<NativeBase> roots[5];
        for (RootRef<NativeBase>& root : roots) {
            auto created = objects.NewObject<NativeBase>(access, NativeTypeIdOf<NativeBase>());
            REQUIRE(created);
            root = Move(created.Value());
        }
        {
            auto borrowed = objects.Resolve<NativeHolder>(access, holder.Value().Handle());
            REQUIRE(borrowed);
            NativeHolder* pointer = Get(borrowed.Value());
            REQUIRE(pointer != nullptr);
            // 첫 버퍼(용량 4)는 허용, 두 번째 확장은 실패한다.
            for (uint32 index = 0; index < 4; ++index) {
                REQUIRE(objects.AddReference(access, holder.Value().Handle(), pointer->items,
                                             roots[index].Handle()));
            }
            CHECK(objects
                      .AddReference(access, holder.Value().Handle(), pointer->items,
                                    roots[4].Handle())
                      .Error() == ErrorCode::OutOfMemory);
            CHECK(pointer->items.Size() == 4);
            CHECK(pointer->items[0] == roots[0].Handle() && pointer->items[3] == roots[3].Handle());
        }
        for (RootRef<NativeBase>& root : roots) {
            root.Reset();
        }
        holder.Value().Reset();
        REQUIRE(objects.Collect(access));
        CHECK(objects.Stats().liveObjects == 0);
    }
    CHECK(allocator.backing_.Stats().liveAllocations == 0);
}

namespace {

class ChainBase : public Object {
    ROCKS_NATIVE_CLASS(ChainBase, Object, 0xc331405def4c4b33ull, 0xa44f05b9d37df2d8ull)

public:
    explicit ChainBase(Allocator& allocator) noexcept : items(allocator) {
    }

    void Trace(TraceVisitor& visitor) const noexcept {
        items.Trace(visitor);
    }

    ObjectRefArray<NativeBase> items;
};

// 자기 필드만 방문하고 ChainBase::Trace를 부르지 않는다. 등록이 사슬을 따라 기반 Trace를 부른다.
class ChainDerived final : public ChainBase {
    ROCKS_NATIVE_CLASS(ChainDerived, ChainBase, 0x9a3e2cd17e7347e7ull, 0x9af28d76f74e6cecull)

public:
    using ChainBase::ChainBase;

    void Trace(TraceVisitor& visitor) const noexcept {
        visitor.Visit(extra);
    }

    ObjectRef<NativeBase> extra;
};

} // namespace

TEST(NativeClass_TraceChainVisitsBaseEdgesWithoutSuperCall) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterAll(types));
        REQUIRE(RegisterNativeClass<ChainBase>(types));
        REQUIRE(RegisterNativeClass<ChainDerived>(types));
        ObjectRegistry objects(allocator, types, {0x4e43, 11});
        const auto access = objects.OwnerAccess();
        auto holder = objects.NewObject<ChainBase>(access, NativeTypeIdOf<ChainDerived>());
        auto inBase = objects.NewObject<NativeBase>(access, NativeTypeIdOf<NativeBase>());
        auto inDerived = objects.NewObject<NativeBase>(access, NativeTypeIdOf<NativeBase>());
        REQUIRE(holder && inBase && inDerived);
        {
            auto borrowed = objects.Resolve<ChainDerived>(access, holder.Value().Handle());
            REQUIRE(borrowed);
            ChainDerived* pointer = Get(borrowed.Value());
            REQUIRE(pointer != nullptr);
            REQUIRE(objects.AddReference(access, holder.Value().Handle(), pointer->items,
                                         inBase.Value().Handle()));
            REQUIRE(objects.SetReference(access, holder.Value().Handle(), pointer->extra,
                                         inDerived.Value().Handle()));
        }
        inBase.Value().Reset();
        inDerived.Value().Reset();
        REQUIRE(objects.Collect(access));
        // 기반 필드(items)와 파생 필드(extra)의 대상이 모두 살아 있다.
        CHECK(objects.Stats().liveObjects == 3);
        holder.Value().Reset();
        REQUIRE(objects.Collect(access));
        CHECK(objects.Stats().liveObjects == 0);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}
