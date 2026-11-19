#include <ReflectionTestFixtures/ReflectionTestFixtures.module.generated.h>
#include <ReflectionTestFixtures/StaticArrayFixture.generated.h>
#include "MapTestSupport.h"

// 리플렉션 B9g 정적 배열(T x[N]) 경로 연산 시험이다(Docs/Design/ReflectionExtensions.ko.md 8절,
// fixture ReflectionTestFixtures/StaticArrayFixture.h): [i] 읽기·쓰기(단일 값·String·ObjectRef —
// 옛 값 반환), struct 원소의 멤버(struct 원소 안 정적 배열 포함)·참조 struct 원소의 참조 멤버, 원소
// 수 = N, 범위 밖 index, 삽입·삭제·비우기의 Unsupported(FixedLength). B6 쓰기 규칙(할당은 게시 전,
// 게시는 무할당)을 객체 allocator의 명시 hook으로 고정한다: String 원소 쓰기의 할당 콜백에서 같은
// 객체에 쓰면 Busy이고, 할당 실패는 옛 값을 그대로 둔다(타이밍에 기대지 않는다).
using namespace rocks;
namespace static_array_path_tests_file {

using namespace rocks::test::map;
using reflection_fixture::FixedOwner;

namespace {

// 지정한 할당 직전에 hook을 한 번 실행하거나 그 할당을 실패시킨다.
class HookAllocator final : public Allocator {
public:
    explicit HookAllocator(Allocator& backing) noexcept : backing_(backing) {
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag) noexcept override {
        ++allocations;
        if (allocations == hookAt) {
            hookAt = 0;
            if (fail) {
                fail = false;
                return Result<void*>::Failure(ErrorCode::OutOfMemory);
            }

            if (hook != nullptr) {
                auto run = hook;
                hook = nullptr;
                run(hookContext);
            }
        }

        return backing_.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        return backing_.Deallocate(address);
    }

    // 지금부터 다음 할당 직전에 run을 한 번 실행한다.
    void ArmHook(void (*run)(void*) noexcept, void* context) noexcept {
        hook = run;
        hookContext = context;
        hookAt = allocations + 1;
    }

    // 지금부터 다음 할당을 OutOfMemory로 실패시킨다.
    void ArmFailure() noexcept {
        fail = true;
        hookAt = allocations + 1;
    }

    uint64 allocations = 0;

private:
    Allocator& backing_;
    uint64 hookAt = 0;
    bool fail = false;
    void (*hook)(void*) noexcept = nullptr;
    void* hookContext = nullptr;
};

// 힙 버퍼가 필요한 긴 텍스트다.
constexpr const char* LongText =
    "static_array_label_long_enough_to_need_a_heap_buffer_xxxxxxxxxxxx";

// 시험 하나의 Registry 묶음이다(객체 allocator는 hook allocator).
struct World {
    explicit World(Allocator& system) noexcept
        : hooked(system), types(system), names(system), registration(types, system),
          objects(hooked, types, {0x534150, 1}, WithNames(&names)), values(system) {
        REQUIRE(names.Initialize());
        REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(registration));
        lease = types.Acquire(TypeIdOf<FixedOwner>());
        REQUIRE(lease);
        holder = New<FixedOwner>(objects);
        owner = holder.Handle();
    }

    ~World() noexcept {
        holder.Reset();
        lease = Result<TypeLease>::Failure(ErrorCode::NotFound);
        CHECK(objects.Collect(objects.OwnerAccess()));
        CHECK(objects.Shutdown(objects.OwnerAccess()));
        CHECK(registration.Unregister());
        CHECK(types.Shutdown());
    }

    HookAllocator hooked;
    TypeRegistry types;
    NamePool names;
    ModuleRegistration registration;
    ObjectRegistry objects;
    Allocator& values;
    Result<TypeLease> lease = Result<TypeLease>::Failure(ErrorCode::NotFound);
    RootRef<FixedOwner> holder;
    ObjectHandle owner;
    ErrorCode reentered = ErrorCode::DependencyFailed;
};

ErrorCode Write(World& world, const char* path, const FieldValue& value,
                FieldValue* previous = nullptr, PropertyPathFailure* failure = nullptr) noexcept {
    const PropertyPathBuffer parsed = PathText(world.lease.Value(), path);
    return ErrorOf(PathWrite(world.objects, world.objects.OwnerAccess(), world.owner, parsed.View(),
                             value, previous, failure));
}

ErrorCode Read(World& world, const char* path, FieldValue& out,
               PropertyPathFailure* failure = nullptr) noexcept {
    const PropertyPathBuffer parsed = PathText(world.lease.Value(), path);
    return ErrorOf(PathRead(world.objects, world.objects.OwnerAccess(), world.owner, parsed.View(),
                            out, failure));
}

FieldValue Reference(Allocator& allocator, ObjectHandle target) noexcept {
    FieldValue value(allocator);
    value.kind = ValueKind::ObjectReference;
    value.reference = target;
    return value;
}

// 콜백: 같은 객체의 ints[0]에 쓴다(고정 중이라 Busy여야 한다).
void Reenter(void* argument) noexcept {
    World& world = *static_cast<World*>(argument);
    world.reentered = Write(world, "ints[0]", Scalar(world.values, ValueKind::Int32, 99));
}

} // namespace

// [i] 읽기·쓰기가 원소 자리에 닿는다: 단일 값·String·ObjectRef(옛 값 반환 — live SetReference),
// struct 원소의 멤버와 그 안의 정적 배열, 참조 struct 원소의 참조 멤버. 원소 수는 N이고 범위 밖
// index는 IndexOutOfRange, 원소 통째 struct 읽기는 AggregateValue다.
TEST(StaticArrayPath_IndexReadWriteAndMembers) {
    SystemAllocator system;
    World world(system);
    RootRef<FixedOwner> other = New<FixedOwner>(world.objects);
    CHECK(Write(world, "ints[2]", Scalar(world.values, ValueKind::Int32, -5)) == ErrorCode::None);
    CHECK(Write(world, "labels[1]", TextField(world.values, LongText)) == ErrorCode::None);
    FieldValue previous(world.values);
    CHECK(Write(world, "refs[1]", Reference(world.values, other.Handle()), &previous) ==
          ErrorCode::None);
    CHECK(previous.kind == ValueKind::ObjectReference && previous.reference.IsNull());
    CHECK(Write(world, "refs[1]", Reference(world.values, {}), &previous) == ErrorCode::None);
    CHECK(previous.reference == other.Handle());
    CHECK(Write(world, "refs[2]", Reference(world.values, other.Handle())) == ErrorCode::None);
    CHECK(Write(world, "points[1].x", Scalar(world.values, ValueKind::Int32, 8)) ==
          ErrorCode::None);
    CHECK(Write(world, "points[0].pair[1]", Scalar(world.values, ValueKind::Int16, -3)) ==
          ErrorCode::None);
    CHECK(Write(world, "links[1].target", Reference(world.values, other.Handle())) ==
          ErrorCode::None);
    With<FixedOwner>(world.objects, world.owner, [&](FixedOwner& value) noexcept {
        CHECK(value.ints[2] == -5 && value.ints[1] == 0);
        CHECK(value.labels[1].View() == LongText);
        CHECK(value.refs[1].Handle().IsNull() && value.refs[2].Handle() == other.Handle());
        CHECK(value.points[1].x == 8 && value.points[0].pair[1] == -3);
        CHECK(value.links[1].target.Handle() == other.Handle());
    });

    FieldValue out(world.values);
    CHECK(Read(world, "labels[1]", out) == ErrorCode::None);
    CHECK(out.kind == ValueKind::Utf8String && out.string.View() == LongText);
    CHECK(Read(world, "points[0].pair[1]", out) == ErrorCode::None);
    CHECK(out.kind == ValueKind::Int16 && out.signedInteger == -3);
    auto count = PathCount(world.objects, world.objects.OwnerAccess(), world.owner,
                           PathText(world.lease.Value(), "ints").View());
    CHECK(count && count.Value() == 4);
    PropertyPathFailure failure;
    CHECK(Read(world, "ints[4]", out, &failure) == ErrorCode::InvalidArgument);
    CHECK(failure.reason == PropertyPathError::IndexOutOfRange);
    CHECK(Read(world, "points[0]", out, &failure) == ErrorCode::Unsupported);
    CHECK(failure.reason == PropertyPathError::AggregateValue);

    // 대상 객체가 원소로만 닿아도 산다(Trace) — root를 놓은 뒤에도 refs[2]·links[1]이 쥔다.
    other.Reset();
    REQUIRE(world.objects.Collect(world.objects.OwnerAccess()));
    CHECK(world.objects.Stats().liveObjects == 2);
}

// 정적 배열은 길이가 N으로 고정이다: 원소 삽입·삭제·비우기는 Unsupported(FixedLength)이고 값은
// 그대로다.
TEST(StaticArrayPath_InsertRemoveClearAreUnsupported) {
    SystemAllocator system;
    World world(system);
    CHECK(Write(world, "ints[0]", Scalar(world.values, ValueKind::Int32, 4)) == ErrorCode::None);
    const FieldValue value = Scalar(world.values, ValueKind::Int32, 1);
    const char* paths[] = {"ints[1]", "points[0]", "refs[0]"};
    for (const char* path : paths) {
        PropertyPathFailure failure;
        const PropertyPathBuffer parsed = PathText(world.lease.Value(), path);
        const bool structElement = path[0] == 'p';
        CHECK(ErrorOf(PathInsertElement(world.objects, world.objects.OwnerAccess(), world.owner,
                                        parsed.View(), structElement ? nullptr : &value,
                                        &failure)) == ErrorCode::Unsupported);
        CHECK(failure.reason == PropertyPathError::FixedLength);
        CHECK(ErrorOf(PathRemoveElement(world.objects, world.objects.OwnerAccess(), world.owner,
                                        parsed.View(), nullptr, &failure)) ==
              ErrorCode::Unsupported);
        CHECK(failure.reason == PropertyPathError::FixedLength);
    }

    PropertyPathFailure failure;
    CHECK(ErrorOf(PathClear(world.objects, world.objects.OwnerAccess(), world.owner,
                            PathText(world.lease.Value(), "ints").View(), &failure)) ==
          ErrorCode::Unsupported);
    CHECK(failure.reason == PropertyPathError::FixedLength);
    With<FixedOwner>(world.objects, world.owner, [](FixedOwner& value) noexcept {
        CHECK(value.ints[0] == 4 && value.ints[1] == 0);
    });
}

// String 원소 쓰기는 새 값을 게시 전에 할당한다: 그 할당 콜백에서 같은 객체에 쓰면 고정 때문에
// Busy이고 바깥 쓰기는 성공한다. 할당이 실패하면 OutOfMemory이고 원소는 옛 값 그대로다. 옛 값
// 반환은 게시된 옛 텍스트다.
TEST(StaticArrayPath_WriteReentryAndAllocationFailureKeepOldValue) {
    SystemAllocator system;
    World world(system);
    REQUIRE(Write(world, "labels[0]", TextField(world.values, "first")) == ErrorCode::None);
    world.hooked.ArmHook(&Reenter, &world);
    FieldValue previous(world.values);
    CHECK(Write(world, "labels[0]", TextField(world.values, LongText), &previous) ==
          ErrorCode::None);
    CHECK(world.reentered == ErrorCode::Busy);
    CHECK(previous.kind == ValueKind::Utf8String && previous.string.View() == "first");

    world.hooked.ArmFailure();
    CHECK(Write(world, "labels[0]",
                TextField(world.values, "second_text_that_is_long_enough_xxxxxxxxxxxx")) ==
          ErrorCode::OutOfMemory);
    With<FixedOwner>(world.objects, world.owner, [](FixedOwner& value) noexcept {
        CHECK(value.labels[0].View() == LongText);
        CHECK(value.ints[0] == 0);
    });
}
} // namespace static_array_path_tests_file
