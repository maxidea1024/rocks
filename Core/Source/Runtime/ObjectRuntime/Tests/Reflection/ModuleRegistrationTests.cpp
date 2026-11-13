#include <ReflectionTestFixtures/ReflectionTestFixtures.module.generated.h>
#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// 모듈 집계 헤더의 RegisterModuleTypes와 ModuleRegistration
// 시험이다(Docs/Design/ReflectionFoundations.ko.md 3.5절).
using namespace rocks;

namespace {

bool Registered(TypeRegistry& types, TypeId id) noexcept {
    return static_cast<bool>(types.Acquire(id));
}

Result<bool> RegisterActor(ModuleRegistration& registration) noexcept {
    using Actor = identity_fixture::RenamedActor;
    return registration.RegisterIfAbsent(GeneratedType<Actor>::Schema(),
                                         GeneratedType<Actor>::Ops());
}

} // namespace

TEST(ModuleRegistration_SharedModuleIsRegisteredOnceAndRetainedByEveryUser) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    ModuleRegistration first(types, allocator);
    REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(first));
    // 기록은 모듈 하나와 타입마다 하나다.
    const usize records = first.Size();
    const uint32 typeCount = types.Stats().registeredTypes;
    CHECK(typeCount >= 2);
    CHECK(records == usize(typeCount) + 1);

    // 같은 registration으로 다시 부르면(의존 그래프의 다른 경로) 모듈이 이미 들어와 있어 아무것도
    // 하지 않는다.
    REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(first));
    CHECK(first.Size() == records);

    // 다른 registration이 같은 모듈을 등록하면 새로 등록하지 않고 모든 타입의 lease를 붙잡는다.
    ModuleRegistration second(types, allocator);
    REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(second));
    CHECK(second.Size() == records);
    CHECK(types.Stats().registeredTypes == typeCount);

    // second가 붙잡은 동안 first는 아무것도 해제하지 못한다(공유 타입을 쓰는 쪽이 타입을 잃지
    // 않는다).
    auto busy = first.Unregister();
    CHECK(!busy && busy.Error() == ErrorCode::Busy);
    CHECK(first.Size() == records);
    CHECK(Registered(types, TypeIdOf<reflection_fixture::Node>()));

    REQUIRE(second.Unregister());
    CHECK(second.Size() == 0);
    CHECK(types.Stats().registeredTypes == typeCount);
    REQUIRE(first.Unregister());
    CHECK(types.Stats().registeredTypes == 0);
    CHECK(types.Shutdown());
}

TEST(ModuleRegistration_EnterModuleRecordsEachModuleOnce) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    ModuleRegistration registration(types, allocator);
    static constexpr char first = 0;
    static constexpr char second = 0;
    CHECK(registration.EnterModule(nullptr).Error() == ErrorCode::InvalidArgument);
    auto entered = registration.EnterModule(&first);
    REQUIRE(entered);
    CHECK(entered.Value());
    auto again = registration.EnterModule(&first);
    REQUIRE(again);
    CHECK(!again.Value());
    REQUIRE(registration.EnterModule(&second));
    CHECK(registration.Size() == 2);

    // 되돌린 모듈은 다시 들어올 수 있다.
    REQUIRE(registration.UnregisterFrom(1));
    auto reentered = registration.EnterModule(&second);
    REQUIRE(reentered);
    CHECK(reentered.Value());
    REQUIRE(registration.Unregister());
    CHECK(registration.Size() == 0);
    CHECK(types.Shutdown());
}

TEST(ModuleRegistration_UnregisterStopsAtBusyTypeInReverseOrder) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    ModuleRegistration registration(types, allocator);
    REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(registration));
    const usize registered = registration.Size();
    {
        auto lease = types.Acquire(TypeIdOf<reflection_fixture::Base>());
        REQUIRE(lease);
        auto busy = registration.Unregister();
        CHECK(!busy && busy.Error() == ErrorCode::Busy);
        // 역순이므로 Base 뒤에 등록된 파생 타입 Node는 이미 빠졌고, Base와 그 앞의 기록은 남는다.
        CHECK(!Registered(types, TypeIdOf<reflection_fixture::Node>()));
        CHECK(Registered(types, TypeIdOf<reflection_fixture::Base>()));
        CHECK(registration.Size() > 0 && registration.Size() < registered);
    }

    REQUIRE(registration.Unregister());
    CHECK(registration.Size() == 0);
    CHECK(registration.UnregisterFrom(1).Error() == ErrorCode::InvalidArgument);
    CHECK(types.Shutdown());
}

TEST(ModuleRegistration_SameIdWithDifferentNativeTypeIsRejected) {
    using reflection_fixture::Base;
    using reflection_fixture::Node;
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    ModuleRegistration registration(types, allocator);
    auto added =
        registration.RegisterIfAbsent(GeneratedType<Base>::Schema(), GeneratedType<Base>::Ops());
    REQUIRE(added);
    CHECK(added.Value());

    auto conflict =
        registration.RegisterIfAbsent(GeneratedType<Base>::Schema(), GeneratedType<Node>::Ops());
    CHECK(!conflict && conflict.Error() == ErrorCode::AlreadyExists);
    CHECK(registration.Size() == 1);

    auto retained =
        registration.RegisterIfAbsent(GeneratedType<Base>::Schema(), GeneratedType<Base>::Ops());
    REQUIRE(retained);
    CHECK(!retained.Value());
    CHECK(registration.Size() == 2);
    REQUIRE(registration.Unregister());
    CHECK(types.Shutdown());
}

// struct 등록: 처음은 등록(true), 같은 ID·같은 native struct는 lease를 붙잡음(false), 같은 ID의
// 다른 native struct는 AlreadyExists, 같은 ID가 enum·클래스 레코드면 TypeMismatch다. 실패는 기록을
// 남기지 않는다.
TEST(ModuleRegistration_RegisterStructIfAbsentRetainsOrRejectsConflicts) {
    using reflection_fixture::Base;
    using reflection_fixture::StructLeaf;
    using reflection_fixture::StructMiddle;
    using reflection_fixture::TextureGroup;
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    ModuleRegistration registration(types, allocator);
    REQUIRE(registration.RegisterEnumIfAbsent(
        GeneratedType<TextureGroup>::Schema(),
        RuntimeEnumOps{NativeTypeKey<TextureGroup>(), ModuleLease{}}));
    REQUIRE(
        registration.RegisterIfAbsent(GeneratedType<Base>::Schema(), GeneratedType<Base>::Ops()));
    auto added = registration.RegisterStructIfAbsent(GeneratedType<StructLeaf>::Schema(),
                                                     GeneratedType<StructLeaf>::Ops());
    REQUIRE(added);
    CHECK(added.Value());
    const usize records = registration.Size();

    auto retained = registration.RegisterStructIfAbsent(GeneratedType<StructLeaf>::Schema(),
                                                        GeneratedType<StructLeaf>::Ops());
    REQUIRE(retained);
    CHECK(!retained.Value());
    CHECK(registration.Size() == records + 1);

    auto conflict = registration.RegisterStructIfAbsent(GeneratedType<StructLeaf>::Schema(),
                                                        GeneratedType<StructMiddle>::Ops());
    CHECK(!conflict && conflict.Error() == ErrorCode::AlreadyExists);

    TypeSchema enumId = GeneratedType<StructLeaf>::Schema();
    enumId.id = TypeIdOf<TextureGroup>();
    auto enumConflict =
        registration.RegisterStructIfAbsent(enumId, GeneratedType<StructLeaf>::Ops());
    CHECK(!enumConflict && enumConflict.Error() == ErrorCode::TypeMismatch);

    TypeSchema classId = GeneratedType<StructLeaf>::Schema();
    classId.id = TypeIdOf<Base>();
    auto classConflict =
        registration.RegisterStructIfAbsent(classId, GeneratedType<StructLeaf>::Ops());
    CHECK(!classConflict && classConflict.Error() == ErrorCode::TypeMismatch);
    CHECK(registration.Size() == records + 1);

    REQUIRE(registration.Unregister());
    CHECK(types.Stats().registeredTypes == 0);
    CHECK(types.Shutdown());
}

TEST(ModuleRegistration_EveryAllocationFailureRollsBackOnlyTheFailedCall) {
    SystemAllocator backing;
    test::NFailAllocator failing(backing);

    // 성공 실행의 할당 횟수를 잰다. 호출 전 기록(mark > 0)은 실패한 호출이 되돌린 뒤에도 남아야
    // 한다.
    uint64 attempts = 0;
    {
        TypeRegistry types(failing);
        ModuleRegistration registration(types, failing);
        REQUIRE(RegisterActor(registration));
        failing.DisableFailure();
        REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(registration));
        attempts = failing.Attempts();
        REQUIRE(registration.Unregister());
        REQUIRE(types.Shutdown());
    }

    CHECK(attempts > 0);
    for (uint64 attempt = 1; attempt <= attempts; ++attempt) {
        {
            TypeRegistry types(failing);
            ModuleRegistration registration(types, failing);
            REQUIRE(RegisterActor(registration));
            failing.FailOnAttempt(attempt);
            auto registered = generated::ReflectionTestFixtures::RegisterModuleTypes(registration);
            failing.DisableFailure();
            CHECK(!registered && registered.Error() == ErrorCode::OutOfMemory);
            CHECK(registration.Size() == 1);
            CHECK(!Registered(types, TypeIdOf<reflection_fixture::Base>()));
            CHECK(Registered(types, TypeIdOf<identity_fixture::RenamedActor>()));
            CHECK(registration.Unregister());
            CHECK(types.Shutdown());
        }

        CHECK(backing.Stats().liveAllocations == 0);
    }
}
