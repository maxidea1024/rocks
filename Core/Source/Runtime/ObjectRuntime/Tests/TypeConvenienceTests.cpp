#include "ReflectionFixture.generated.h"
#include <Core/Memory/SystemAllocator.h>
#include <Object/ConstructionContext.h>
#include <Object/ObjectRegistry.h>
#include <Platform/Fatal.h>
#include <Platform/Thread.h>
#include <TestSupport/HostFailure.h>
#include <TestSupport/Test.h>

using namespace rocks;
using reflection_fixture::Base;
using reflection_fixture::Node;

namespace {

constexpr Guid ProbeType{0x5459504551554552ull, 1};
ErrorCode constructionQuery = ErrorCode::None;
ErrorCode destructorQuery = ErrorCode::None;
ErrorCode traceQuery = ErrorCode::None;
bool observeCallbacks = false;

template <typename T>
concept LvalueMetadataAllowed = requires(T& value) {
    value.Name();
    value.Schema();
    value.Ops();
};

template <typename T>
concept RvalueNameAllowed = requires(T&& value) { static_cast<T&&>(value).Name(); };

template <typename T>
concept RvalueSchemaAllowed = requires(T&& value) { static_cast<T&&>(value).Schema(); };

template <typename T>
concept RvalueOpsAllowed = requires(T&& value) { static_cast<T&&>(value).Ops(); };

// 이름·스키마·콜백은 lease가 필요하므로 임시 객체에서 참조를 꺼낼 수 없다.
static_assert(LvalueMetadataAllowed<TypeLease>);
static_assert(!RvalueNameAllowed<TypeLease>);
static_assert(!RvalueSchemaAllowed<TypeLease>);
static_assert(!RvalueOpsAllowed<TypeLease>);

struct TypeConvenienceProbe final : Object {
    TypeConvenienceProbe() noexcept {
        if (observeCallbacks) {
            constructionQuery = TryGetType().Error();
        }
    }

    ~TypeConvenienceProbe() noexcept {
        if (observeCallbacks) {
            destructorQuery = TryGetType().Error();
        }
    }

    void Trace(TraceVisitor&) const noexcept {
        if (observeCallbacks) {
            traceQuery = TryGetType().Error();
        }
    }
};

struct CallbackReset {
    ~CallbackReset() noexcept {
        observeCallbacks = false;
    }
};

struct ForeignTypeQuery {
    const Object* object = nullptr;
    ObjectAccessContext wrongAccess{};
    ErrorCode error = ErrorCode::None;
};

void QueryTypeFromForeignThread(void* raw) noexcept {
    auto& query = *static_cast<ForeignTypeQuery*>(raw);
    ObjectAccessScope scope(query.wrongAccess);
    query.error = query.object->TryGetType().Error();
}

struct ModuleCounter {
    uint32 references = 0;
    uint32 releases = 0;
};

void RetainModule(void* raw) noexcept {
    ++static_cast<ModuleCounter*>(raw)->references;
}

void ReleaseModule(void* raw) noexcept {
    auto& counter = *static_cast<ModuleCounter*>(raw);
    --counter.references;
    ++counter.releases;
}

} // namespace

TEST(ObjectTypeConvenienceReturnsActualTypeAndRetainsLease) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        auto base = GeneratedType<Base>::Register(types);
        auto derived = GeneratedType<Node>::Register(types);
        REQUIRE(base && derived);

        // 정적 타입 정보는 호출한 C++ 타입을, 객체 조회는 실제 파생 타입을 가리킨다.
        CHECK(TypeIdOf<Base>() == GeneratedType<Base>::Id());
        CHECK(TypeIdOf<Node>() == GeneratedType<Node>::Id());
        CHECK(TypeIdOf<Base>() != TypeIdOf<Node>());
        CHECK(TypeOf<Base>().id == TypeIdOf<Base>());
        CHECK(TypeOf<Node>().id == TypeIdOf<Node>());

        ObjectRegistry objects(allocator, types, {932, 1});
        const auto access = objects.OwnerAccess();
        auto root = objects.NewObject<Node>(access, TypeIdOf<Node>());
        REQUIRE(root);
        const ObjectHandle handle = root.Value().Handle();
        auto borrowed = objects.Resolve<Object>(access, handle);
        REQUIRE(borrowed);
        auto pointer = borrowed.Value().Get();
        REQUIRE(pointer);

        TypeLease direct = pointer.Value()->GetType();
        CHECK(direct.Schema().id == TypeIdOf<Node>());
        CHECK(direct.Id() == TypeIdOf<Node>());
        CHECK(pointer.Value()->GetType().Id() == TypeIdOf<Node>());
        CHECK(pointer.Value()->GetType().IsA(TypeIdOf<Base>()));
        CHECK(direct.Name() == StringView("rocks::reflection_fixture::Node"));
        CHECK(direct.IsA(TypeIdOf<Base>()));
        CHECK(direct.IsA(TypeIdOf<Node>()));
        auto checked = pointer.Value()->TryGetType();
        REQUIRE(checked);
        CHECK(checked.Value().Schema().id == TypeIdOf<Node>());

        // 등록된 상속 사슬에 있는 기반 타입으로도 Resolve한다. 포인터는 Base 하위 객체를 가리킨다.
        {
            auto asBase = objects.Resolve<Base>(access, handle);
            REQUIRE(asBase);
            auto basePointer = asBase.Value().Get();
            REQUIRE(basePointer);
            CHECK(static_cast<Base*>(pointer.Value()) == basePointer.Value());
        }
        borrowed.Value().Reset();
        root.Value().Reset();
        REQUIRE(objects.Collect(access));
        CHECK(objects.Stats().liveObjects == 0);

        // 객체가 회수된 후에도 보유 중인 메타데이터 lease는 등록 해제를 막는다.
        CHECK(direct.Schema().id == TypeIdOf<Node>());
        CHECK(types.Unregister(derived.Value()).Error() == ErrorCode::Busy);
        checked.Value().Reset();
        direct.Reset();
        REQUIRE(types.Unregister(derived.Value()));
        REQUIRE(types.Unregister(base.Value()));
        REQUIRE(objects.Shutdown(access));
        REQUIRE(types.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
    CHECK(allocator.Stats().liveBytes == 0);
}

TEST(ObjectTypeConvenienceRejectsWrongContextThreadAndLifetime) {
    SystemAllocator allocator;
    {
        TypeConvenienceProbe unattached;
        CHECK(unattached.TryGetType().Error() == ErrorCode::StaleHandle);

        TypeRegistry types(allocator);
        auto registered =
            types.Register({ProbeType, 1, {}, {}}, MakeRuntimeTypeOps<TypeConvenienceProbe>());
        REQUIRE(registered);
        ObjectRegistry objects(allocator, types, {933, 1});
        const auto access = objects.OwnerAccess();
        const ObjectAccessContext wrong{{933, 2}};
        auto root = objects.NewObject<TypeConvenienceProbe>(access, ProbeType);
        REQUIRE(root);
        auto borrowed = objects.Resolve<TypeConvenienceProbe>(access, root.Value().Handle());
        REQUIRE(borrowed);
        auto pointer = borrowed.Value().Get();
        REQUIRE(pointer);

        // 기본 모드에서는 별도 scope 없이 사용할 수 있다.
        auto alive = pointer.Value()->TryGetType();
        REQUIRE(alive);
        CHECK(alive.Value().Schema().id == ProbeType);
        {
            ObjectAccessScope other(wrong);
            CHECK(pointer.Value()->TryGetType().Error() == ErrorCode::WrongContext);
        }
        ForeignTypeQuery query{pointer.Value(), wrong};
        platform::Thread thread;
        if (thread.Start(QueryTypeFromForeignThread, &query) != platform::ThreadStatus::Success ||
            thread.WaitForCompletion(10000) != platform::WaitResult::Signaled ||
            thread.Join() != platform::ThreadStatus::Success) {
            platform::Fatal("Type convenience test worker did not finish");
        }
        CHECK(query.error == ErrorCode::WrongThread);

        // root가 물리 수명을 보장하는 음성 대조에서만 Borrow 없는 호출을 검사한다.
        TypeConvenienceProbe* stillAllocated = pointer.Value();
        borrowed.Value().Reset();
        CHECK(stillAllocated->TryGetType().Error() == ErrorCode::InvalidArgument);
        borrowed = objects.Resolve<TypeConvenienceProbe>(access, root.Value().Handle());
        REQUIRE(borrowed);
        pointer = borrowed.Value().Get();
        REQUIRE(pointer);
        REQUIRE(pointer.Value()->Destroy());
        CHECK(pointer.Value()->TryGetType().Error() == ErrorCode::Destroyed);
        CHECK(alive.Value().Schema().id == ProbeType);

        borrowed.Value().Reset();
        root.Value().Reset();
        REQUIRE(objects.Collect(access));
        alive.Value().Reset();
        REQUIRE(types.Unregister(registered.Value()));
        REQUIRE(objects.Shutdown(access));
        REQUIRE(types.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
    CHECK(allocator.Stats().liveBytes == 0);
}

TEST(ObjectTypeConvenienceHonorsStrictScopeAndShutdown) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(types.Register({ProbeType, 1, {}, {}}, MakeRuntimeTypeOps<TypeConvenienceProbe>()));
        ObjectRegistry objects(allocator, types, {934, 1}, {8, 0xffffffffu, true});
        const auto access = objects.OwnerAccess();
        auto root = objects.NewObject<TypeConvenienceProbe>(access, ProbeType);
        REQUIRE(root);
        auto borrowed = objects.Resolve<TypeConvenienceProbe>(access, root.Value().Handle());
        REQUIRE(borrowed);
        auto pointer = borrowed.Value().Get();
        REQUIRE(pointer);
        CHECK(pointer.Value()->TryGetType().Error() == ErrorCode::WrongContext);
        {
            ObjectAccessScope scope(access);
            auto result = pointer.Value()->TryGetType();
            REQUIRE(result);
            CHECK(result.Value().Schema().id == ProbeType);
        }

        // 등록된 Borrow가 종료를 막아도 이후 조회는 ShuttingDown을 반환해야 한다.
        CHECK(objects.Shutdown(access).Error() == ErrorCode::Busy);
        {
            ObjectAccessScope scope(access);
            CHECK(pointer.Value()->TryGetType().Error() == ErrorCode::ShuttingDown);
        }
        borrowed.Value().Reset();
        root.Value().Reset();
        REQUIRE(objects.Shutdown(access));
        REQUIRE(types.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ObjectTypeConvenienceRejectsConstructionAndCollectorReentry) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(types.Register({ProbeType, 1, {}, {}}, MakeRuntimeTypeOps<TypeConvenienceProbe>()));
        ObjectRegistry objects(allocator, types, {935, 1});
        const auto access = objects.OwnerAccess();
        CallbackReset reset;
        observeCallbacks = true;
        constructionQuery = ErrorCode::None;
        destructorQuery = ErrorCode::None;
        traceQuery = ErrorCode::None;

        // 미공개 생성 트랜잭션의 포인터는 일반 생명주기 타입 조회에 사용할 수 없다.
        {
            ConstructionContext construction(objects, access);
            auto staged = construction.Create<TypeConvenienceProbe>(ProbeType);
            REQUIRE(staged);
            auto pointer = construction.Get<TypeConvenienceProbe>(staged.Value());
            REQUIRE(pointer);
            CHECK(pointer.Value()->TryGetType().Error() == ErrorCode::StaleHandle);
            construction.Rollback();
        }
        destructorQuery = ErrorCode::None;
        auto root = objects.NewObject<TypeConvenienceProbe>(access, ProbeType);
        REQUIRE(root);
        CHECK(constructionQuery == ErrorCode::StaleHandle);
        REQUIRE(objects.Collect(access));
        CHECK(traceQuery == ErrorCode::Busy);
        root.Value().Reset();
        REQUIRE(objects.Collect(access));
        CHECK(destructorQuery == ErrorCode::Busy);
        observeCallbacks = false;
        REQUIRE(objects.Shutdown(access));
        REQUIRE(types.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ObjectTypeConvenienceUsesItsOwnRegistryAndPreservesCodeLease) {
    SystemAllocator allocator;
    {
        ModuleCounter module;
        TypeRegistry firstTypes(allocator);
        TypeRegistry secondTypes(allocator);
        auto code = ModuleLease::Acquire(&module, RetainModule, ReleaseModule);
        REQUIRE(code);
        auto first =
            firstTypes.Register({ProbeType, 1, {}, {"first", 5}},
                                MakeRuntimeTypeOps<TypeConvenienceProbe>(Move(code.Value())));
        auto second = secondTypes.Register({ProbeType, 1, {}, {"second", 6}},
                                           MakeRuntimeTypeOps<TypeConvenienceProbe>());
        REQUIRE(first && second);
        ObjectRegistry firstObjects(allocator, firstTypes, {936, 1});
        ObjectRegistry secondObjects(allocator, secondTypes, {936, 2});
        auto firstRoot =
            firstObjects.NewObject<TypeConvenienceProbe>(firstObjects.OwnerAccess(), ProbeType);
        auto secondRoot =
            secondObjects.NewObject<TypeConvenienceProbe>(secondObjects.OwnerAccess(), ProbeType);
        REQUIRE(firstRoot && secondRoot);
        auto firstBorrow = firstObjects.Resolve<TypeConvenienceProbe>(firstObjects.OwnerAccess(),
                                                                      firstRoot.Value().Handle());
        auto secondBorrow = secondObjects.Resolve<TypeConvenienceProbe>(
            secondObjects.OwnerAccess(), secondRoot.Value().Handle());
        REQUIRE(firstBorrow && secondBorrow);
        auto firstObject = firstBorrow.Value().Get();
        auto secondObject = secondBorrow.Value().Get();
        REQUIRE(firstObject && secondObject);
        TypeLease firstLease = firstObject.Value()->GetType();
        TypeLease secondLease = secondObject.Value()->GetType();
        CHECK(firstLease.Schema().name.Size() == 5);
        CHECK(secondLease.Schema().name.Size() == 6);
        CHECK(firstLease.Schema().name[0] == 'f');
        CHECK(secondLease.Schema().name[0] == 's');
        CHECK(firstLease.Id() == ProbeType);
        CHECK(secondLease.Id() == ProbeType);
        CHECK(firstLease.Name() == StringView("first"));
        CHECK(secondLease.Name() == StringView("second"));

        firstBorrow.Value().Reset();
        secondBorrow.Value().Reset();
        firstRoot.Value().Reset();
        secondRoot.Value().Reset();
        REQUIRE(firstObjects.Collect(firstObjects.OwnerAccess()));
        REQUIRE(secondObjects.Collect(secondObjects.OwnerAccess()));
        CHECK(firstTypes.Unregister(first.Value()).Error() == ErrorCode::Busy);
        CHECK(module.references == 1);
        CHECK(module.releases == 0);
        firstLease.Reset();
        secondLease.Reset();
        REQUIRE(firstTypes.Unregister(first.Value()));
        REQUIRE(secondTypes.Unregister(second.Value()));
        CHECK(module.references == 0);
        CHECK(module.releases == 1);
        REQUIRE(firstObjects.Shutdown(firstObjects.OwnerAccess()));
        REQUIRE(secondObjects.Shutdown(secondObjects.OwnerAccess()));
        REQUIRE(firstTypes.Shutdown());
        REQUIRE(secondTypes.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
    CHECK(allocator.Stats().liveBytes == 0);
}

namespace rocks::test {

void RegisterTypeConvenienceNegativeControl(int (*function)(uint32) noexcept) noexcept;

// 별도 프로세스에서 실행한다. Fatal이 발생하지 않으면 0을 반환해 음성 대조가 실패한다.
int RunTypeConvenienceNegativeControl(uint32 operation) noexcept {
    PrepareFatalTestProcess();
    if (operation == 2) {
        TypeLease empty;
        (void)empty.IsA(ProbeType);
        return 0;
    }
    if (operation == 0) {
        TypeConvenienceProbe unattached;
        auto type = unattached.GetType();
        (void)type;
        return 0;
    }

    SystemAllocator allocator;
    TypeRegistry types(allocator);
    if (!types.Register({ProbeType, 1, {}, {}}, MakeRuntimeTypeOps<TypeConvenienceProbe>())) {
        return 0;
    }
    ObjectRegistry objects(allocator, types, {937, 1});
    const auto access = objects.OwnerAccess();
    auto root = objects.NewObject<TypeConvenienceProbe>(access, ProbeType);
    if (!root) {
        return 0;
    }
    auto borrowed = objects.Resolve<TypeConvenienceProbe>(access, root.Value().Handle());
    if (!borrowed) {
        return 0;
    }
    auto object = borrowed.Value().Get();
    if (!object) {
        return 0;
    }
    ObjectAccessScope wrong({{937, 2}});
    auto type = object.Value()->GetType();
    (void)type;
    return 0;
}

namespace {
struct TypeConvenienceNegativeRegistration {
    TypeConvenienceNegativeRegistration() noexcept {
        RegisterTypeConvenienceNegativeControl(&RunTypeConvenienceNegativeControl);
    }
};
TypeConvenienceNegativeRegistration typeConvenienceNegativeRegistration;
} // namespace

} // namespace rocks::test
