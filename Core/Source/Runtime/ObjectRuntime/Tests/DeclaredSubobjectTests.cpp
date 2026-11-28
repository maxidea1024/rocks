#include <ReflectionTestFixtures/SubobjectFixture.generated.h>
#include <Base/InitializerList.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/String.h>
#include <Object/ConstructionContext.h>
#include <Object/ObjectRegistry.h>
#include <Object/SubobjectDefaultsContext.h>
#include <Object/TypeRegistry.h>
#include <TestSupport/Test.h>

// 선언 subobject 생성 시험이다(Docs/Design/Prefab 10.3·10.8절 PF1b): ConstructionContext::
// CreateDeclaredSubobjects가 표 순서로 만들고(Outer = 소유 객체, 이름 = 필드 도구 이름, 생성 출처
// ClassSubobject) 소유 필드·소유 목록·Root·부착 부모를 배선하며 base 사슬의 기본값 덮기를 가장 먼
// base부터 부르는지, 생성자는 subobject를 만들지 않는지, 인스턴스마다 따로 만드는지, 실패가 전부
// 되돌려지는지다.
using namespace rocks;
using reflection_fixture::SubobjectFailingOwner;
using reflection_fixture::SubobjectOwner;
using reflection_fixture::SubobjectOwnerChild;
using reflection_fixture::SubobjectPart;
using reflection_fixture::SubobjectScenePart;

namespace declared_subobject_tests_file {

namespace {

struct Fixture {
    explicit Fixture(Allocator& allocator) noexcept
        : allocator(allocator), types(allocator), registry(allocator, types, {0x53554230, 1}) {
    }

    bool Start() noexcept {
        access = registry.OwnerAccess();
        return GeneratedType<SubobjectPart>::Register(types) &&
               GeneratedType<SubobjectScenePart>::Register(types) &&
               GeneratedType<SubobjectOwner>::Register(types) &&
               GeneratedType<SubobjectOwnerChild>::Register(types) &&
               GeneratedType<SubobjectFailingOwner>::Register(types);
    }

    // T 소유 객체와 그 선언 subobject를 한 트랜잭션에서 만들어 공개한다.
    template <class T>
    RootRef<Object> Spawn(usize* made = nullptr) noexcept {
        ConstructionContext transaction(registry, access);
        auto owner = transaction.Create<T>(TypeIdOf<T>());
        if (!owner) {
            return {};
        }

        ObjectHandle parts[MaxClassSubobjects];
        auto created = transaction.CreateDeclaredSubobjects(owner.Value(), parts);
        if (!created) {
            return {};
        }

        if (made != nullptr) {
            *made = created.Value();
        }

        auto committed = transaction.Commit();
        return committed ? Move(committed.Value()) : RootRef<Object>();
    }

    int32 CountOf(ObjectHandle handle) noexcept {
        auto part = registry.Resolve<SubobjectPart>(access, handle);
        return part ? part.Value().Get().Value()->count : -1;
    }

    ObjectHandle ParentOf(ObjectHandle handle) noexcept {
        auto part = registry.Resolve<SubobjectScenePart>(access, handle);
        return part ? part.Value().Get().Value()->parent.Handle() : ObjectHandle{};
    }

    bool NameIs(ObjectHandle handle, StringView expected) noexcept {
        String name(allocator);
        return registry.GetName(access, handle, name) && name.View() == expected;
    }

    Allocator& allocator;
    TypeRegistry types;
    ObjectRegistry registry;
    ObjectAccessContext access;
};

} // namespace

TEST(DefaultSubobject_CreatedByInstancingNotConstructor) {
    SystemAllocator allocator;
    Fixture fixture(allocator);
    REQUIRE(fixture.Start());
    // 생성자만 실행한 소유 객체에는 subobject가 없다(생성자는 다른 객체를 만들지 않는다 — RCM
    // 59행).
    auto plain = fixture.registry.NewObject<SubobjectOwner>(fixture.access,
                                                            TypeIdOf<SubobjectOwner>(), {}, {});
    REQUIRE(plain);
    auto bare = fixture.registry.Resolve<SubobjectOwner>(fixture.access, plain.Value().Handle());
    REQUIRE(bare);
    CHECK(bare.Value().Get().Value()->parts.Size() == 0);
    CHECK(bare.Value().Get().Value()->Body().IsNull());
}

TEST(DefaultSubobject_TableReferencesResolve) {
    SystemAllocator allocator;
    Fixture fixture(allocator);
    REQUIRE(fixture.Start());
    usize made = 0;
    RootRef<Object> owner = fixture.Spawn<SubobjectOwner>(&made);
    REQUIRE(!owner.Handle().IsNull());
    CHECK(made == 3);
    auto view = fixture.registry.Resolve<SubobjectOwner>(fixture.access, owner.Handle());
    REQUIRE(view);
    const SubobjectOwner& value = *view.Value().Get().Value();
    const ObjectHandle body = value.Body();
    const ObjectHandle arm = value.Arm();
    const ObjectHandle plain = value.Plain();
    // 소유 목록은 표 순서, Root 필드는 Root subobject, arm은 body에 붙고 body·plain은 붙지 않는다.
    REQUIRE(value.parts.Size() == 3);
    CHECK(value.parts[0] == body && value.parts[1] == arm && value.parts[2] == plain);
    CHECK(value.root.Handle() == body);
    CHECK(fixture.ParentOf(arm) == body);
    CHECK(fixture.ParentOf(body).IsNull());
    // Outer = 소유 객체, 이름 = 필드 도구 이름, 생성 출처.
    for (const ObjectHandle part : {body, arm, plain}) {
        auto outer = fixture.registry.GetOuter(fixture.access, part);
        CHECK(outer && outer.Value() == owner.Handle());
        auto origin = fixture.registry.GetOrigin(fixture.access, part);
        CHECK(origin && origin.Value() == ObjectOrigin::ClassSubobject);
    }

    CHECK(fixture.NameIs(body, "body"));
    CHECK(fixture.NameIs(arm, "arm"));
    CHECK(fixture.NameIs(plain, "plain"));
    auto ownerOrigin = fixture.registry.GetOrigin(fixture.access, owner.Handle());
    CHECK(ownerOrigin && ownerOrigin.Value() == ObjectOrigin::Default);
    // 기본값 덮기(base hook).
    CHECK(fixture.CountOf(body) == 1);
    CHECK(fixture.CountOf(arm) == 2);
    CHECK(fixture.CountOf(plain) == 0);
}

TEST(DefaultSubobject_DerivedClassOverridesInheritedDefaults) {
    SystemAllocator allocator;
    Fixture fixture(allocator);
    REQUIRE(fixture.Start());
    usize made = 0;
    RootRef<Object> owner = fixture.Spawn<SubobjectOwnerChild>(&made);
    REQUIRE(!owner.Handle().IsNull());
    CHECK(made == 4);
    auto view = fixture.registry.Resolve<SubobjectOwnerChild>(fixture.access, owner.Handle());
    REQUIRE(view);
    const SubobjectOwnerChild& value = *view.Value().Get().Value();
    REQUIRE(value.parts.Size() == 4);
    CHECK(value.parts[3] == value.Hand());
    // hand는 Name="hand"가 객체 이름이고 Root에 붙는다.
    CHECK(fixture.NameIs(value.Hand(), "hand"));
    CHECK(fixture.ParentOf(value.Hand()) == value.Body());
    // base hook 뒤 파생 hook: body는 base 값, arm은 파생이 덮은 값, hand는 파생 값.
    CHECK(fixture.CountOf(value.Body()) == 1);
    CHECK(fixture.CountOf(value.Arm()) == 20);
    CHECK(fixture.CountOf(value.Hand()) == 3);
}

TEST(DefaultSubobject_NoSharedMutableComponent) {
    SystemAllocator allocator;
    Fixture fixture(allocator);
    REQUIRE(fixture.Start());
    RootRef<Object> first = fixture.Spawn<SubobjectOwner>();
    RootRef<Object> second = fixture.Spawn<SubobjectOwner>();
    REQUIRE(!first.Handle().IsNull() && !second.Handle().IsNull());
    ObjectHandle firstBody;
    ObjectHandle secondBody;
    {
        auto a = fixture.registry.Resolve<SubobjectOwner>(fixture.access, first.Handle());
        auto b = fixture.registry.Resolve<SubobjectOwner>(fixture.access, second.Handle());
        REQUIRE(a && b);
        firstBody = a.Value().Get().Value()->Body();
        secondBody = b.Value().Get().Value()->Body();
    }

    CHECK(firstBody != secondBody);
    // 한 인스턴스의 값을 바꿔도 다른 인스턴스는 그대로다.
    {
        auto part = fixture.registry.ResolveForWrite<SubobjectPart>(fixture.access, firstBody);
        REQUIRE(part);
        part.Value().Get().Value()->count = 99;
    }

    CHECK(fixture.CountOf(secondBody) == 1);
}

TEST(DefaultSubobject_FailureRollsBackEverything) {
    SystemAllocator allocator;
    Fixture fixture(allocator);
    REQUIRE(fixture.Start());
    const auto existing = [&]() noexcept {
        const ObjectStats stats = fixture.registry.Stats();
        return stats.liveObjects + stats.reservedObjects;
    };
    const uint32 before = existing();

    // 결과 칸이 모자라면 InvalidArgument이고 트랜잭션이 그 오류로 고정된다.
    {
        ConstructionContext transaction(fixture.registry, fixture.access);
        auto owner = transaction.Create<SubobjectOwner>(TypeIdOf<SubobjectOwner>());
        REQUIRE(owner);
        ObjectHandle few[2];
        auto created = transaction.CreateDeclaredSubobjects(owner.Value(), few);
        CHECK(!created && created.Error() == ErrorCode::InvalidArgument);
        CHECK(!transaction.Commit());
    }

    // 표 이름과 같은 이름의 객체가 먼저 있으면 Create가 AlreadyExists로 실패하고 전부 되돌린다.
    {
        ConstructionContext transaction(fixture.registry, fixture.access);
        auto owner = transaction.Create<SubobjectOwner>(TypeIdOf<SubobjectOwner>());
        REQUIRE(owner);
        REQUIRE(transaction.Create<SubobjectPart>(TypeIdOf<SubobjectPart>(), owner.Value(), "arm"));
        ObjectHandle parts[MaxClassSubobjects];
        auto created = transaction.CreateDeclaredSubobjects(owner.Value(), parts);
        CHECK(!created && created.Error() == ErrorCode::AlreadyExists);
        CHECK(!transaction.Commit());
    }

    // 같은 소유 객체에 두 번 부르면(목록이 이미 차 있다) InvalidArgument다.
    {
        ConstructionContext transaction(fixture.registry, fixture.access);
        auto owner = transaction.Create<SubobjectOwner>(TypeIdOf<SubobjectOwner>());
        REQUIRE(owner);
        ObjectHandle parts[MaxClassSubobjects];
        REQUIRE(transaction.CreateDeclaredSubobjects(owner.Value(), parts));
        auto again = transaction.CreateDeclaredSubobjects(owner.Value(), parts);
        CHECK(!again && again.Error() == ErrorCode::InvalidArgument);
        CHECK(!transaction.Commit());
    }

    // 기본값 덮기 hook의 실패는 그 오류로 트랜잭션을 고정한다(base hook은 이미 돌았어도 전부
    // 되돌린다).
    {
        ConstructionContext transaction(fixture.registry, fixture.access);
        auto owner = transaction.Create<SubobjectFailingOwner>(TypeIdOf<SubobjectFailingOwner>());
        REQUIRE(owner);
        ObjectHandle parts[MaxClassSubobjects];
        auto created = transaction.CreateDeclaredSubobjects(owner.Value(), parts);
        CHECK(!created && created.Error() == ErrorCode::Unsupported);
        CHECK(!transaction.Commit());
    }

    CHECK(existing() == before);
}

TEST(DefaultSubobject_DefaultsContextRecordMode) {
    SystemAllocator allocator;
    Fixture fixture(allocator);
    REQUIRE(fixture.Start());
    // 기록 모드(Registry 밖 임시 인스턴스 — subobject 기본값 레코드, PF1c): 같은 hook이 임시
    // subobject에 값을 준다. 소유 필드는 비어 있어도 주소 대조로 항목을 고른다.
    auto lease = fixture.types.Acquire(TypeIdOf<SubobjectOwner>());
    REQUIRE(lease);
    SubobjectOwner owner(allocator);
    SubobjectScenePart body;
    SubobjectScenePart arm;
    SubobjectPart plain;
    Object* const parts[] = {&body, &arm, &plain};
    SubobjectDefaultsContext context(owner, lease.Value(), parts);
    CHECK(SubobjectOwner::InitSubobjectDefaults(context));
    CHECK(body.count == 1 && arm.count == 2 && plain.count == 0);
    // subobject가 아닌 ObjectRef 필드는 InvalidArgument이고 fn을 부르지 않는다.
    bool called = false;
    auto rejected = context.With(&SubobjectOwner::root, [&](SubobjectScenePart&) noexcept {
        called = true;
        return Result<void>::Success();
    });
    CHECK(!rejected && rejected.Error() == ErrorCode::InvalidArgument && !called);
}

} // namespace declared_subobject_tests_file
