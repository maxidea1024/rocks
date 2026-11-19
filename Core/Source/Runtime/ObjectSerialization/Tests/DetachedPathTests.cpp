#include <ReflectionTestFixtures/PathFixture.generated.h>
#include <ReflectionTestFixtures/ReflectionFixture.generated.h>
#include <ReflectionTestFixtures/ReflectionTestFixtures.module.generated.h>
#include <ReflectionTestFixtures/StructFixture.generated.h>
#include <Core/Memory/SystemAllocator.h>
#include <Object/ConstructionContext.h>
#include <Object/DetachedPath.h>
#include <Object/ObjectBatchPublish.h>
#include <TestSupport/Test.h>

// 분리된 값에서 시작하는 경로와 Detach·Attach 시험이다(리플렉션 B9i3c0 —
// Docs/Design/ReflectionExtensions.ko.md 10.2절 "구현(B9i3c0)에서 정한 점"). MetaCompiler 생성 경로
// thunk(StructFixture·PathFixture·ReflectionFixture)로: struct 값·상자 위의 읽기·쓰기, Detach →
// 편집 → Attach 왕복(묶음 게시 구간 안, 무할당), Detach 할당 실패 때 원본 불변, staged 객체의 중첩
// 경로와 staged Attach, 최상위 단일 값(String·Soft·정수) 필드의 경로 thunk, 재진입(Detach 복사 중
// 할당 콜백의 쓰기는 Busy, Attach 뒤 옛 값 해제 콜백은 게시된 새 값을 본다)을 본다.
using namespace rocks;
using reflection_fixture::AssetFields;
using reflection_fixture::PathOwner;
using reflection_fixture::StructHolder;
using reflection_fixture::StructLeaf;
using reflection_fixture::StructOwner;

namespace {

ErrorCode ErrorOf(const Result<void>& result) noexcept {
    return result ? ErrorCode::None : result.Error();
}

// 할당·반환을 세고 실패 주입과 한 번짜리 콜백(할당·반환)을 거는 allocator다.
class HookAllocator final : public Allocator {
public:
    using Hook = void (*)(void* context) noexcept;

    explicit HookAllocator(Allocator& inner) noexcept : inner_(inner) {
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        ++allocations;
        if (failAt != 0 && allocations == failAt) {
            return Result<void*>::Failure(ErrorCode::OutOfMemory);
        }

        if (Hook hook = onAllocate) {
            onAllocate = nullptr;
            hook(context);
        }

        return inner_.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        ++frees;
        if (Hook hook = onFree) {
            onFree = nullptr;
            hook(context);
        }

        return inner_.Deallocate(address);
    }

    uint64 allocations = 0;
    uint64 frees = 0;
    uint64 failAt = 0;
    Hook onAllocate = nullptr;
    Hook onFree = nullptr;
    void* context = nullptr;

private:
    Allocator& inner_;
};

FieldValue StringValue(Allocator& allocator, const char* text) noexcept {
    FieldValue value(allocator);
    value.kind = ValueKind::Utf8String;
    CHECK(value.string.TryAssign(StringView(text)));
    return value;
}

FieldValue Unsigned(Allocator& allocator, ValueKind kind, uint64 number) noexcept {
    FieldValue value(allocator);
    value.kind = kind;
    value.unsignedInteger = number;
    return value;
}

// 클래스 lease로 해석한 경로다(skip개 앞 단계를 떼면 상자·struct 값에서 시작하는 경로가 된다).
class ParsedPath {
public:
    ParsedPath(const TypeLease& type, const char* text) noexcept {
        REQUIRE(ParsePropertyPath(type, StringView(text), buffer_));
    }

    PropertyPath View(usize skip = 0) const noexcept {
        const PropertyPath path = buffer_.View();
        return PropertyPath{path.steps.Mid(skip), path.keys};
    }

private:
    PropertyPathBuffer buffer_;
};

StringView ReadText(ObjectRegistry& objects, const PathTarget& target, PropertyPath path,
                    FieldValue& out) noexcept {
    CHECK(PathRead(objects, objects.OwnerAccess(), target, path, out));
    return out.string.View();
}

// 생성 fixture 모듈을 등록한 타입 Registry다(소멸 때 해제·Shutdown).
struct Modules {
    Modules() noexcept : types(system), registration(types, system) {
        REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(registration));
    }

    ~Modules() {
        REQUIRE(registration.Unregister());
        CHECK(types.Shutdown());
    }

    SystemAllocator system;
    TypeRegistry types;
    ModuleRegistration registration;
};

// 객체 Registry 하나다(소멸 때 Shutdown — root·lease·Borrow는 그 전에 놓는다).
struct World {
    World(Modules& modules, Allocator& objectAllocator) noexcept
        : types(modules.types), objects(objectAllocator, modules.types, {0x4450, 1}) {
    }

    ~World() {
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    TypeRegistry& types;
    ObjectRegistry objects;
};

} // namespace

// struct 값과 상자에서 시작하는 경로의 읽기·쓰기다: struct 값의 첫 단계는 그 struct의 멤버이고,
// 상자의 첫 단계는 상자에 담은 필드 자신이다(중첩 경로에서 분리하면 그 단계부터). 상자를 고쳐도
// 원본은 그대로이고, 강한 참조 쓰기·복사할 수 없는 값의 분리·찬 상자로의 분리·다른 첫 필드는
// 거부한다.
TEST(DetachedPath_ReadWriteOnStructValueAndBox) {
    SystemAllocator allocator;
    Modules modules;
    {
        World world(modules, allocator);
        ObjectRegistry& objects = world.objects;
        const auto access = objects.OwnerAccess();
        auto lease = world.types.Acquire(TypeIdOf<StructOwner>());
        auto owner = objects.NewObject<StructOwner>(access, TypeIdOf<StructOwner>());
        REQUIRE(lease && owner);
        const ObjectHandle handle = owner.Value().Handle();
        const ParsedPath extraLabel(lease.Value(), "extra.label");
        const ParsedPath extra(lease.Value(), "extra");
        const ParsedPath leafLabel(lease.Value(), "holder.middle.leaf.label");
        const ParsedPath middle(lease.Value(), "holder.middle");
        const ParsedPath target(lease.Value(), "holder.target");
        FieldValue original = StringValue(allocator, "원본");
        REQUIRE(PathWrite(objects, access, handle, extraLabel.View(), original));
        REQUIRE(PathWrite(objects, access, handle, leafLabel.View(), original));

        // struct 값(호출자의 StructLeaf) — 경로는 extra 단계를 뗀 멤버부터다.
        const usize extraIndex = lease.Value().FieldIndex(extra.View().steps[0].fieldId);
        StructLeaf leaf(allocator);
        const PathTarget leafValue = PathTarget::Value(
            lease.Value(), lease.Value().FieldStruct(extraIndex), &leaf, allocator);
        FieldValue written = StringValue(allocator, "값");
        REQUIRE(PathWrite(objects, access, leafValue, extraLabel.View(1), written));
        CHECK(leaf.label.View() == StringView("값"));
        FieldValue read(allocator);
        CHECK(ReadText(objects, leafValue, extraLabel.View(1), read) == StringView("값"));

        // 강한 참조를 담은 struct 값의 참조 쓰기는 Unsupported(NoPathAccess)다.
        const usize holderIndex = lease.Value().FieldIndex(target.View().steps[0].fieldId);
        StructHolder holder(allocator);
        const PathTarget holderValue = PathTarget::Value(
            lease.Value(), lease.Value().FieldStruct(holderIndex), &holder, allocator);
        FieldValue null(allocator);
        null.kind = ValueKind::ObjectReference;
        PropertyPathFailure failure;
        CHECK(ErrorOf(PathWrite(objects, access, holderValue, target.View(1), null, nullptr,
                                &failure)) == ErrorCode::Unsupported);
        CHECK(failure.reason == PropertyPathError::NoPathAccess);

        // 상자: 최상위 struct 필드와 중첩 struct 멤버.
        DetachedValue box(objects.ObjectAllocator());
        REQUIRE(PathDetach(objects, access, handle, extra.View(), box));
        CHECK(!box.IsEmpty() && box.FieldId() == extra.View().steps[0].fieldId);
        CHECK(ReadText(objects, PathTarget::Value(box), extraLabel.View(), read) ==
              StringView("원본"));
        FieldValue edited = StringValue(allocator, "상자");
        REQUIRE(PathWrite(objects, access, PathTarget::Value(box), extraLabel.View(), edited));
        CHECK(ReadText(objects, PathTarget::Value(box), extraLabel.View(), read) ==
              StringView("상자"));
        CHECK(ReadText(objects, handle, extraLabel.View(), read) == StringView("원본"));
        DetachedValue nested(objects.ObjectAllocator());
        REQUIRE(PathDetach(objects, access, handle, middle.View(), nested));
        CHECK(ReadText(objects, PathTarget::Value(nested), leafLabel.View(1), read) ==
              StringView("원본"));

        // 상자의 첫 단계가 상자 필드가 아니면 NotFound(UnknownField), 찬 상자로 분리하면
        // InvalidArgument, 강한 참조를 담은 값은 Unsupported(NotCopyable)다.
        CHECK(ErrorOf(PathRead(objects, access, PathTarget::Value(nested), leafLabel.View(), read,
                               &failure)) == ErrorCode::NotFound);
        CHECK(failure.reason == PropertyPathError::UnknownField);
        CHECK(ErrorOf(PathDetach(objects, access, handle, extra.View(), box)) ==
              ErrorCode::InvalidArgument);
        DetachedValue refused(objects.ObjectAllocator());
        const ParsedPath holderPath(lease.Value(), "holder");
        CHECK(ErrorOf(PathDetach(objects, access, handle, holderPath.View(), refused, &failure)) ==
              ErrorCode::Unsupported);
        CHECK(failure.reason == PropertyPathError::NotCopyable && refused.IsEmpty());
        lease.Value().Reset();
    }

    CHECK(true);
}

// Detach → 상자 편집 → Attach 왕복이다: 게시된 객체는 변경 표지를 읽은 뒤 준비하고(Detach·
// PathPrepareAttach는 표지를 올리지 않는다) 묶음 게시 구간 안에서 할당·반환 없이 맞바꾸며, End가
// 표지를 올린다. 옛 값은 상자가 받는다. 최상위 단일 값(Soft·정수) 필드도 경로 thunk로 같은 길을
// 간다. ReadOnly 필드와 다른 필드의 상자는 Attach하지 못한다.
TEST(DetachedPath_DetachEditAttachRoundTrip) {
    SystemAllocator system;
    Modules modules;
    HookAllocator counting(system);
    {
        World world(modules, counting);
        ObjectRegistry& objects = world.objects;
        const auto access = objects.OwnerAccess();
        auto lease = world.types.Acquire(TypeIdOf<StructOwner>());
        auto owner = objects.NewObject<StructOwner>(access, TypeIdOf<StructOwner>());
        REQUIRE(lease && owner);
        const ObjectHandle handle = owner.Value().Handle();
        const ParsedPath extra(lease.Value(), "extra");
        const ParsedPath extraLabel(lease.Value(), "extra.label");
        const ParsedPath extraCount(lease.Value(), "extra.count");
        const ParsedPath plain(lease.Value(), "plain");
        FieldValue original = StringValue(system, "원본");
        REQUIRE(PathWrite(objects, access, handle, extraLabel.View(), original));
        FieldValue seven = Unsigned(system, ValueKind::UInt32, 7);
        REQUIRE(PathWrite(objects, access, handle, plain.View(), seven));

        auto stamp = objects.MutationStamp(access, handle);
        REQUIRE(stamp);
        DetachedValue box(objects.ObjectAllocator());
        DetachedValue plainBox(objects.ObjectAllocator());
        REQUIRE(PathDetach(objects, access, handle, extra.View(), box));
        REQUIRE(PathDetach(objects, access, handle, plain.View(), plainBox));
        FieldValue edited = StringValue(system, "편집한 긴 이름 — 버퍼가 바뀐다");
        REQUIRE(PathWrite(objects, access, PathTarget::Value(box), extraLabel.View(), edited));
        FieldValue nine = Unsigned(system, ValueKind::UInt32, 9);
        REQUIRE(PathWrite(objects, access, PathTarget::Value(box), extraCount.View(), nine));
        FieldValue eleven = Unsigned(system, ValueKind::UInt32, 11);
        REQUIRE(PathWrite(objects, access, PathTarget::Value(plainBox), plain.View(), eleven));
        PreparedAttach prepared;
        PreparedAttach preparedPlain;
        REQUIRE(PathPrepareAttach(objects, access, handle, extra.View(), box, prepared));
        REQUIRE(PathPrepareAttach(objects, access, handle, plain.View(), plainBox, preparedPlain));
        CHECK(prepared.IsReady() && preparedPlain.IsReady());
        CHECK(objects.MutationStamp(access, handle).Value() == stamp.Value());

        {
            auto borrowed = objects.Resolve<StructOwner>(access, handle);
            REQUIRE(borrowed);
            auto batch = objects.BeginBatchPublish(access, handle, stamp.Value(), {}, {});
            REQUIRE(batch);
            // 구간 안의 다른 변경은 Busy다.
            CHECK(ErrorOf(PathWrite(objects, access, handle, extraLabel.View(), original)) ==
                  ErrorCode::Busy);
            const uint64 allocations = counting.allocations;
            const uint64 frees = counting.frees;
            prepared.Publish(batch.Value());
            preparedPlain.Publish(batch.Value());
            CHECK(counting.allocations == allocations && counting.frees == frees);
            CHECK(!prepared.IsReady());
            batch.Value().End();
            StructOwner* object = borrowed.Value().Get().Value();
            CHECK(object->extra.label.View() == edited.string.View());
            CHECK(object->extra.count == 9 && object->plain == 11);
        }

        CHECK(objects.MutationStamp(access, handle).Value() > stamp.Value());
        FieldValue read(system);
        CHECK(ReadText(objects, PathTarget::Value(box), extraLabel.View(), read) ==
              StringView("원본"));
        REQUIRE(PathRead(objects, access, PathTarget::Value(plainBox), plain.View(), read));
        CHECK(read.unsignedInteger == 7);
        box.Reset();
        plainBox.Reset();

        // 최상위 Soft 필드(SoftObjectRef<>)도 경로 thunk로 분리·게시한다.
        auto assetLease = world.types.Acquire(TypeIdOf<AssetFields>());
        auto asset = objects.NewObject<AssetFields>(access, TypeIdOf<AssetFields>());
        REQUIRE(assetLease && asset);
        const ParsedPath anyAsset(assetLease.Value(), "anyAsset");
        FieldValue soft(system);
        soft.kind = ValueKind::SoftObjectReference;
        soft.guid = Guid{0xA55E7, 1};
        REQUIRE(soft.string.TryAssign(StringView("/Game/Rock.Rock")));
        REQUIRE(PathWrite(objects, access, asset.Value().Handle(), anyAsset.View(), soft));
        DetachedValue softBox(objects.ObjectAllocator());
        REQUIRE(PathDetach(objects, access, asset.Value().Handle(), anyAsset.View(), softBox));
        FieldValue moved(system);
        moved.kind = ValueKind::SoftObjectReference;
        moved.guid = Guid{0xA55E7, 2};
        REQUIRE(moved.string.TryAssign(StringView("/Game/Moved.Moved")));
        REQUIRE(PathWrite(objects, access, PathTarget::Value(softBox), anyAsset.View(), moved));
        auto assetStamp = objects.MutationStamp(access, asset.Value().Handle());
        PreparedAttach preparedSoft;
        REQUIRE(PathPrepareAttach(objects, access, asset.Value().Handle(), anyAsset.View(), softBox,
                                  preparedSoft));
        {
            auto borrowed = objects.Resolve<AssetFields>(access, asset.Value().Handle());
            REQUIRE(borrowed);
            auto batch = objects.BeginBatchPublish(access, asset.Value().Handle(),
                                                   assetStamp.Value(), {}, {});
            REQUIRE(batch);
            const uint64 allocations = counting.allocations;
            preparedSoft.Publish(batch.Value());
            CHECK(counting.allocations == allocations);
        }

        REQUIRE(PathRead(objects, access, asset.Value().Handle(), anyAsset.View(), read));
        CHECK(read.guid == moved.guid && read.string.View() == StringView("/Game/Moved.Moved"));

        // ReadOnly 필드는 Attach하지 못하고, 다른 필드의 상자는 TypeMismatch다.
        auto pathLease = world.types.Acquire(TypeIdOf<PathOwner>());
        auto pathOwner = objects.NewObject<PathOwner>(access, TypeIdOf<PathOwner>());
        REQUIRE(pathLease && pathOwner);
        const ParsedPath frozen(pathLease.Value(), "frozen");
        const ParsedPath main(pathLease.Value(), "Main");
        DetachedValue frozenBox(objects.ObjectAllocator());
        REQUIRE(PathDetach(objects, access, pathOwner.Value().Handle(), frozen.View(), frozenBox));
        PreparedAttach refused;
        PropertyPathFailure failure;
        CHECK(ErrorOf(PathPrepareAttach(objects, access, pathOwner.Value().Handle(), frozen.View(),
                                        frozenBox, refused, &failure)) == ErrorCode::Unsupported);
        CHECK(failure.reason == PropertyPathError::ReadOnly && !refused.IsReady());
        CHECK(ErrorOf(PathPrepareAttach(objects, access, pathOwner.Value().Handle(), main.View(),
                                        frozenBox, refused, &failure)) == ErrorCode::TypeMismatch);
        CHECK(failure.reason == PropertyPathError::StaticTypeMismatch);
        lease.Value().Reset();
        assetLease.Value().Reset();
        pathLease.Value().Reset();
    }

    CHECK(counting.allocations >= counting.frees);
}

// Detach의 할당 실패(상자 저장소·깊은 복사의 String)는 원본과 상자를 그대로 두고 만든 것을 모두
// 반환한다.
TEST(DetachedPath_DetachFailureLeavesSourceUnchanged) {
    SystemAllocator system;
    Modules modules;
    {
        World world(modules, system);
        ObjectRegistry& objects = world.objects;
        const auto access = objects.OwnerAccess();
        auto lease = world.types.Acquire(TypeIdOf<StructOwner>());
        auto owner = objects.NewObject<StructOwner>(access, TypeIdOf<StructOwner>());
        REQUIRE(lease && owner);
        const ObjectHandle handle = owner.Value().Handle();
        const ParsedPath extra(lease.Value(), "extra");
        const ParsedPath extraLabel(lease.Value(), "extra.label");
        FieldValue original = StringValue(system, "실패해도 그대로인 원본");
        REQUIRE(PathWrite(objects, access, handle, extraLabel.View(), original));
        for (uint64 attempt = 1; attempt <= 2; ++attempt) {
            HookAllocator failing(system);
            failing.failAt = attempt;
            {
                DetachedValue box(failing);
                CHECK(ErrorOf(PathDetach(objects, access, handle, extra.View(), box)) ==
                      ErrorCode::OutOfMemory);
                CHECK(box.IsEmpty());
            }

            CHECK(failing.allocations - 1 == failing.frees);
            FieldValue read(system);
            CHECK(ReadText(objects, handle, extraLabel.View(), read) == original.string.View());
        }

        lease.Value().Reset();
    }

    CHECK(true);
}

// staged 객체의 중첩 경로다: 트랜잭션의 공개 전 객체에 중첩 멤버·최상위 단일 값(경로 thunk)·참조
// 멤버(staged 간선)를 경로로 쓰고 읽으며, Detach → 편집 → PreparedAttach::Publish()로 게시한 뒤
// Commit한 객체에서 그 값을 본다. 다른 Registry의 트랜잭션은 WrongContext다.
TEST(DetachedPath_StagedNestedPaths) {
    SystemAllocator system;
    Modules modules;
    {
        World world(modules, system);
        ObjectRegistry& objects = world.objects;
        const auto access = objects.OwnerAccess();
        auto lease = world.types.Acquire(TypeIdOf<StructOwner>());
        REQUIRE(lease);
        const ParsedPath leafLabel(lease.Value(), "holder.middle.leaf.label");
        const ParsedPath plain(lease.Value(), "plain");
        const ParsedPath target(lease.Value(), "holder.target");
        const ParsedPath extra(lease.Value(), "extra");
        const ParsedPath extraCount(lease.Value(), "extra.count");
        ObjectHandle created{};
        RootRef<Object> root;
        {
            ConstructionContext transaction(objects, access);
            auto made = transaction.Create<StructOwner>(TypeIdOf<StructOwner>());
            REQUIRE(made);
            created = made.Value();
            const PathTarget staged = PathTarget::Staged(transaction, created);
            FieldValue label = StringValue(system, "staged 잎");
            REQUIRE(PathWrite(objects, access, staged, leafLabel.View(), label));
            FieldValue five = Unsigned(system, ValueKind::UInt32, 5);
            REQUIRE(PathWrite(objects, access, staged, plain.View(), five));
            FieldValue self(system);
            self.kind = ValueKind::ObjectReference;
            self.reference = created;
            REQUIRE(PathWrite(objects, access, staged, target.View(), self));
            FieldValue read(system);
            CHECK(ReadText(objects, staged, leafLabel.View(), read) == StringView("staged 잎"));
            REQUIRE(PathRead(objects, access, staged, plain.View(), read));
            CHECK(read.unsignedInteger == 5);

            // 게시된 객체 경로는 staged 객체를 받지 않는다.
            CHECK(ErrorOf(PathRead(objects, access, created, plain.View(), read)) !=
                  ErrorCode::None);

            DetachedValue box(objects.ObjectAllocator());
            REQUIRE(PathDetach(objects, access, staged, extra.View(), box));
            FieldValue twelve = Unsigned(system, ValueKind::UInt32, 12);
            REQUIRE(PathWrite(objects, access, PathTarget::Value(box), extraCount.View(), twelve));
            PreparedAttach prepared;
            REQUIRE(PathPrepareAttach(objects, access, staged, extra.View(), box, prepared));
            REQUIRE(prepared.Publish());
            CHECK(ErrorOf(prepared.Publish()) == ErrorCode::InvalidArgument);

            // 다른 Registry의 트랜잭션이다.
            ObjectRegistry other(system, world.types, {0x4451, 1});
            ConstructionContext foreign(other, other.OwnerAccess());
            CHECK(ErrorOf(PathRead(objects, access, PathTarget::Staged(foreign, created),
                                   plain.View(), read)) == ErrorCode::WrongContext);
            foreign.Rollback();
            REQUIRE(other.Shutdown(other.OwnerAccess()));
            auto committed = transaction.Commit();
            REQUIRE(committed);
            root = Move(committed.Value());
        }

        auto borrowed = objects.Resolve<StructOwner>(access, created);
        REQUIRE(borrowed);
        StructOwner* object = borrowed.Value().Get().Value();
        CHECK(object->holder.middle.leaf.label.View() == StringView("staged 잎"));
        CHECK(object->plain == 5 && object->extra.count == 12);
        CHECK(object->holder.target.Handle() == created);
        borrowed.Value().Reset();
        root.Reset();
        lease.Value().Reset();
    }

    CHECK(true);
}

namespace {

struct Reentry {
    ObjectRegistry* objects = nullptr;
    ObjectHandle owner{};
    PropertyPath label{};
    PropertyPath plain{};
    ErrorCode duringDetach = ErrorCode::None;
    ErrorCode afterAttach = ErrorCode::InvalidArgument;
    bool sawNewValue = false;
};

// Detach 복사의 할당 콜백이 원본을 바꾸려 한다(고정 중이라 Busy여야 한다).
void WriteDuringDetach(void* context) noexcept {
    Reentry& reentry = *static_cast<Reentry*>(context);
    SystemAllocator local;
    FieldValue value = StringValue(local, "재진입");
    reentry.duringDetach = ErrorOf(PathWrite(*reentry.objects, reentry.objects->OwnerAccess(),
                                             reentry.owner, reentry.label, value));
}

// Attach 뒤 옛 값의 해제 콜백이 owner를 읽고 쓴다(구간을 나온 뒤라 된다).
void TouchAfterAttach(void* context) noexcept {
    Reentry& reentry = *static_cast<Reentry*>(context);
    SystemAllocator local;
    FieldValue read(local);
    if (PathRead(*reentry.objects, reentry.objects->OwnerAccess(), reentry.owner, reentry.label,
                 read)) {
        reentry.sawNewValue = read.string.View() == StringView("새 값");
    }

    FieldValue three = Unsigned(local, ValueKind::UInt32, 3);
    reentry.afterAttach = ErrorOf(PathWrite(*reentry.objects, reentry.objects->OwnerAccess(),
                                            reentry.owner, reentry.plain, three));
}

} // namespace

// 재진입이다: Detach의 깊은 복사 중 할당 콜백이 원본을 쓰면 Busy(encode 고정)이고 원본·상자가
// 일관되며, Attach 게시 뒤 상자를 비울 때 옛 값의 해제 콜백은 구간 밖에서 돌아 게시된 새 값을
// 보고 owner를 쓸 수 있다.
TEST(DetachedPath_ReentryDuringDetachAndAfterAttach) {
    SystemAllocator system;
    Modules modules;
    HookAllocator hooks(system);
    {
        World world(modules, hooks);
        ObjectRegistry& objects = world.objects;
        const auto access = objects.OwnerAccess();
        auto lease = world.types.Acquire(TypeIdOf<StructOwner>());
        auto owner = objects.NewObject<StructOwner>(access, TypeIdOf<StructOwner>());
        REQUIRE(lease && owner);
        const ObjectHandle handle = owner.Value().Handle();
        const ParsedPath extra(lease.Value(), "extra");
        const ParsedPath extraLabel(lease.Value(), "extra.label");
        const ParsedPath plain(lease.Value(), "plain");
        FieldValue original = StringValue(system, "옛 값 — 해제 콜백을 부를 만큼 긴 문자열");
        REQUIRE(PathWrite(objects, access, handle, extraLabel.View(), original));
        Reentry reentry;
        reentry.objects = &objects;
        reentry.owner = handle;
        reentry.label = extraLabel.View();
        reentry.plain = plain.View();
        hooks.context = &reentry;

        DetachedValue box(objects.ObjectAllocator());
        hooks.onAllocate = &WriteDuringDetach;
        REQUIRE(PathDetach(objects, access, handle, extra.View(), box));
        CHECK(hooks.onAllocate == nullptr && reentry.duringDetach == ErrorCode::Busy);
        FieldValue read(system);
        CHECK(ReadText(objects, PathTarget::Value(box), extraLabel.View(), read) ==
              original.string.View());
        CHECK(ReadText(objects, handle, extraLabel.View(), read) == original.string.View());

        FieldValue fresh = StringValue(system, "새 값");
        REQUIRE(PathWrite(objects, access, PathTarget::Value(box), extraLabel.View(), fresh));
        auto stamp = objects.MutationStamp(access, handle);
        PreparedAttach prepared;
        REQUIRE(PathPrepareAttach(objects, access, handle, extra.View(), box, prepared));
        {
            auto borrowed = objects.Resolve<StructOwner>(access, handle);
            REQUIRE(borrowed);
            auto batch = objects.BeginBatchPublish(access, handle, stamp.Value(), {}, {});
            REQUIRE(batch);
            prepared.Publish(batch.Value());
        }

        hooks.onFree = &TouchAfterAttach;
        box.Reset();
        CHECK(hooks.onFree == nullptr && reentry.sawNewValue);
        CHECK(reentry.afterAttach == ErrorCode::None);
        auto borrowed = objects.Resolve<StructOwner>(access, handle);
        REQUIRE(borrowed);
        CHECK(borrowed.Value().Get().Value()->plain == 3);
        borrowed.Value().Reset();
        lease.Value().Reset();
    }

    CHECK(true);
}
