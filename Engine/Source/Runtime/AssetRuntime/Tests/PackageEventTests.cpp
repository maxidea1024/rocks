#include "AssetTestSupport.h"
#include "PackageEditing.h"

#include <ReflectionTestFixtures/EventFixture.generated.h>
#include <ReflectionTestFixtures/FunctionFixture.generated.h>
#include <ReflectionTestFixtures/ReflectionTestFixtures.module.generated.h>

// 패키지의 이벤트 바인딩 시험이다(Docs/Design/EventReflection.ko.md EV3a2 — ED21·ED24): 에디터
// 패키지 왕복이 Commit 뒤 바인딩을 다시 만들고(다시 쓴 byte 같음), 에디터 패키지 로드는 풀리지 않은
// 바인딩(함수 없음)을 미해결 레코드로 보존해 다시 쓴 byte가 읽은 byte와 같으며, 그 객체를 쿠킹
// 쓰기하면 실패한다. 쿠킹 패키지 왕복도 바인딩을 다시 만든다.
using namespace rocks;
namespace package_event_tests_file {

using namespace rocks::asset_test;
using reflection_fixture::CallTarget;
using reflection_fixture::EventPublisher;

namespace {

Guid AddId(TypeRegistry& types) noexcept {
    auto function = types.FindFunctionByName(TypeIdOf<CallTarget>(), "Add");
    return function ? function.Value().Schema().id : Guid{};
}

// 대표 TestAsset 아래 발행자 Publisher와 listener Listener를 두고 Publisher.changed를
// Listener.Add에 바인딩한 패키지다.
struct EventScene {
    EventScene(ObjectRegistry& registry, TypeRegistry& types, PackageKind kind) noexcept {
        auto made = NewPackage(registry, registry.OwnerAccess(), FixedId(940), kind);
        if (!made) {
            return;
        }

        package = Move(made.Value());
        const ObjectHandle asset = Make<TestAsset>(registry, TestAssetType(), package.Handle(), "");
        publisher = Make<EventPublisher>(registry, TypeIdOf<EventPublisher>(), asset, "Publisher");
        listener = Make<CallTarget>(registry, TypeIdOf<CallTarget>(), asset, "Listener");
        const Guid add = AddId(types);
        ready = !asset.IsNull() && !publisher.IsNull() && !listener.IsNull() && add != Guid{};
        if (!ready) {
            return;
        }

        auto borrow = registry.ResolveForWrite<EventPublisher>(registry.OwnerAccess(), publisher);
        if (!borrow) {
            ready = false;
            return;
        }

        auto value = borrow.Value().Get();
        ready =
            value &&
            value.Value()->changed.TryBind(registry, registry.OwnerAccess(), listener, add).IsOk();
    }

    RootRef<Package> package;
    ObjectHandle publisher{};
    ObjectHandle listener{};
    bool ready = false;
};

// 로드한 패키지의 Publisher·Listener다.
struct Loaded {
    ObjectHandle publisher{};
    ObjectHandle listener{};
};

Loaded FindLoaded(ObjectRegistry& registry, ObjectHandle package) noexcept {
    Loaded loaded;
    auto asset = registry.FindObject(registry.OwnerAccess(), package, StringView());
    if (!asset) {
        return loaded;
    }

    auto publisher = registry.FindChild(registry.OwnerAccess(), asset.Value(), "Publisher");
    auto listener = registry.FindChild(registry.OwnerAccess(), asset.Value(), "Listener");
    if (publisher && listener) {
        loaded.publisher = publisher.Value();
        loaded.listener = listener.Value();
    }

    return loaded;
}

int32 TotalOf(ObjectRegistry& registry, ObjectHandle handle) noexcept {
    CallTarget* target = Pointer<CallTarget>(registry, handle);
    return target != nullptr ? target->total : -1;
}

// Publisher.changed를 amount로 발행하고 바인딩 수·미해결 레코드 수를 돌려준다(found = 발행자를
// 찾아 발행했는지).
struct EventState {
    bool found = false;
    usize bindings = 0;
    usize unresolved = 0;
};

EventState Broadcast(ObjectRegistry& registry, ObjectHandle publisher, int32 amount) noexcept {
    EventState state;
    auto borrow = registry.ResolveForWrite<EventPublisher>(registry.OwnerAccess(), publisher);
    if (!borrow) {
        return state;
    }

    auto value = borrow.Value().Get();
    if (!value) {
        return state;
    }

    state.bindings = value.Value()->changed.BindingCount();
    state.unresolved = value.Value()->changed.UnresolvedCount();
    state.found =
        value.Value()
            ->changed
            .TryBroadcast(EventContext{registry, registry.OwnerAccess(), publisher}, amount)
            .IsOk();
    return state;
}

// 바이트 열의 needle Guid(16 byte LE 표현)를 모두 replacement로 바꾸고 바꾼 수를 돌려준다.
usize PatchGuid(Array<uint8>& bytes, Guid needle, Guid replacement) noexcept {
    constexpr usize HalfBytes = sizeof(uint64);
    uint8 from[sizeof(Guid)];
    uint8 to[sizeof(Guid)];
    for (usize index = 0; index < HalfBytes; ++index) {
        from[index] = static_cast<uint8>(needle.high >> (8 * index));
        from[HalfBytes + index] = static_cast<uint8>(needle.low >> (8 * index));
        to[index] = static_cast<uint8>(replacement.high >> (8 * index));
        to[HalfBytes + index] = static_cast<uint8>(replacement.low >> (8 * index));
    }

    usize patched = 0;
    for (usize at = 0; at + sizeof(Guid) <= bytes.Size(); ++at) {
        bool same = true;
        for (usize index = 0; index < sizeof(Guid) && same; ++index) {
            same = bytes[at + index] == from[index];
        }

        if (same) {
            for (usize index = 0; index < sizeof(Guid); ++index) {
                bytes[at + index] = to[index];
            }

            ++patched;
        }
    }

    return patched;
}

// 패키지 Objects 섹션의 needle Guid를 replacement로 바꿔 다시 배치한다(CRC 포함). 실패하면 빈
// 배열이다.
Array<uint8> WithObjectsGuid(Allocator& allocator, const Array<uint8>& file, Guid needle,
                             Guid replacement) noexcept {
    EditablePackage edit(allocator);
    if (!edit.Parse(file.View())) {
        return Array<uint8>(allocator);
    }

    EditableSection* objects = edit.Find(PackageSectionKind::Objects);
    if (objects == nullptr || PatchGuid(objects->bytes, needle, replacement) != 1) {
        return Array<uint8>(allocator);
    }

    return edit.Build();
}

// 이 시험의 타입 등록(AssetEnv의 AssetRuntime·AssetTestFixtures + 이벤트 fixture 모듈)이다.
bool RegisterEventFixtures(AssetEnv& env) noexcept {
    return generated::ReflectionTestFixtures::RegisterModuleTypes(env.modules).IsOk();
}

// env와 같은 정책의 패키지 Registry를 하나 더 만든다(로드를 여러 번 하는 시험).
UniquePtr<ObjectRegistry> NewTarget(AssetEnv& env, uint64 serial) noexcept {
    RegistryOptions options;
    options.names = &env.names;
    options.policy.packageNativeType = NativeTypeKey<Package>();
    options.policy.rejectWorldOnly = true;
    auto made = UniquePtr<ObjectRegistry>::TryMake(env.allocator, env.allocator, env.types,
                                                   ContextId{0x4556504bull, serial}, options);
    return made ? Move(made.Value()) : UniquePtr<ObjectRegistry>();
}

} // namespace

// 에디터·쿠킹 패키지 왕복이 Commit 뒤 바인딩을 다시 만든다(로드한 객체 사이), 다시 쓴 byte가 같다.
TEST(AssetPackage_EventBindingsRoundTrip) {
    const PackageKind kinds[] = {PackageKind::Editor, PackageKind::Cooked};
    for (PackageKind kind : kinds) {
        AssetEnv env;
        REQUIRE(env.Ready());
        REQUIRE(RegisterEventFixtures(env));
        EventScene scene(*env.source, env.types, kind);
        REQUIRE(scene.ready);
        const PackageWriteOptions options =
            kind == PackageKind::Cooked ? CookedOptions() : EditorOptions();
        Array<uint8> bytes = Write(env.allocator, *env.source, scene.package.Handle(), options);
        REQUIRE(!bytes.IsEmpty());
        const Array<uint8> original = Copy(env.allocator, bytes.View());

        Array<uint8> files[1] = {Move(bytes)};
        RootRef<Object> roots[1];
        ObjectHandle handles[1];
        REQUIRE(LoadAll(env, *env.target, files, kind, nullptr, roots, handles));
        const Loaded loaded = FindLoaded(*env.target, handles[0]);
        REQUIRE(!loaded.publisher.IsNull());
        Array<uint8> again = Write(env.allocator, *env.target, handles[0], options);
        CHECK(SameBytes(again.View(), original.View()));

        const EventState state = Broadcast(*env.target, loaded.publisher, 6);
        CHECK(state.found && state.bindings == 1 && state.unresolved == 0);
        CHECK(TotalOf(*env.target, loaded.listener) == 6);
        CHECK(TotalOf(*env.source, scene.listener) == 0);
    }
}

// 에디터 패키지 로드는 함수 없는 바인딩을 미해결 레코드로 보존한다: 발행되지 않고, 다시 쓴 byte가
// 읽은 byte와 같으며, 쿠킹 쓰기는 실패한다. 쿠킹 패키지 로드는 버린다(같은 기록을 쿠킹 패키지로
// 만들어 본다).
TEST(AssetPackage_EditorKeepsUnresolvedEventBindings) {
    AssetEnv env;
    REQUIRE(env.Ready());
    REQUIRE(RegisterEventFixtures(env));
    const Guid add = AddId(env.types);
    const Guid absent{0x4556504b00000001ull, 0x0000000000000002ull};
    const PackageKind kinds[] = {PackageKind::Editor, PackageKind::Cooked};
    uint64 serial = 1;
    for (PackageKind kind : kinds) {
        // 종류마다 새 Registry 둘(쓰기·읽기)을 쓴다(같은 패키지 id).
        UniquePtr<ObjectRegistry> source = NewTarget(env, serial++);
        REQUIRE(source.Get() != nullptr);
        EventScene scene(*source, env.types, kind);
        REQUIRE(scene.ready);
        const PackageWriteOptions options =
            kind == PackageKind::Cooked ? CookedOptions() : EditorOptions();
        Array<uint8> bytes = Write(env.allocator, *source, scene.package.Handle(), options);
        REQUIRE(!bytes.IsEmpty());
        // 에디터 패키지의 풀리는 바인딩은 쿠킹 쓰기가 된다(아래 실패가 미해결 레코드 때문임을
        // 가른다).
        if (kind == PackageKind::Editor) {
            CHECK(
                !Write(env.allocator, *source, scene.package.Handle(), CookedOptions()).IsEmpty());
        }
        Array<uint8> missing = WithObjectsGuid(env.allocator, bytes, add, absent);
        REQUIRE(!missing.IsEmpty());
        const Array<uint8> original = Copy(env.allocator, missing.View());

        UniquePtr<ObjectRegistry> target = NewTarget(env, serial++);
        REQUIRE(target.Get() != nullptr);
        Array<uint8> files[1] = {Move(missing)};
        RootRef<Object> roots[1];
        ObjectHandle handles[1];
        REQUIRE(LoadAll(env, *target, files, kind, nullptr, roots, handles));
        const Loaded loaded = FindLoaded(*target, handles[0]);
        REQUIRE(!loaded.publisher.IsNull());
        const EventState state = Broadcast(*target, loaded.publisher, 6);
        CHECK(state.found && state.bindings == 0);
        CHECK(TotalOf(*target, loaded.listener) == 0);
        if (kind == PackageKind::Editor) {
            // 보존: 다시 쓴 byte가 읽은 byte와 같고, 쿠킹 쓰기는 미해결 레코드를 거부한다.
            CHECK(state.unresolved == 1);
            Array<uint8> again = Write(env.allocator, *target, handles[0], options);
            CHECK(SameBytes(again.View(), original.View()));
            AssetDiagnostic failure;
            Array<uint8> cooked =
                Write(env.allocator, *target, handles[0], CookedOptions(), &failure);
            CHECK(cooked.IsEmpty());
            CHECK(failure.code == AssetDiagnosticCode::EventBindingNotCookable &&
                  failure.error == ErrorCode::InvalidArgument &&
                  failure.section == static_cast<uint32>(PackageSectionKind::Objects) &&
                  failure.detail == static_cast<uint64>(EventBindingNotCookableUnresolved));
        } else {
            CHECK(state.unresolved == 0);
        }

        roots[0].Reset();
        scene.package.Reset();
    }
}

} // namespace package_event_tests_file
