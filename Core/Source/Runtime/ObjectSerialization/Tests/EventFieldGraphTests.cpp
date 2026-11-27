#include <ReflectionTestFixtures/EventFixture.generated.h>
#include <ReflectionTestFixtures/FunctionFixture.generated.h>
#include <ReflectionTestFixtures/ReflectionTestFixtures.module.generated.h>
#include <Core/Memory/SystemAllocator.h>
#include <Object/EventOps.h>
#include <Object/ModuleRegistration.h>
#include <Object/PropertyPath.h>
#include <ObjectSerialization/ClassDefaults.h>
#include <ObjectSerialization/ObjectGraph.h>
#include <Platform/Fatal.h>
#include <TestSupport/Test.h>
#include "GraphTestSupport.h"

// 이벤트 필드의 값 경로 시험이다(Docs/Design/EventReflection.ko.md 3.1절 — EV2b, fixture
// ReflectionTestFixtures/EventFixture.h): 값 필드와 이벤트 필드가 섞인 클래스의 그래프 저장·로드
// 왕복(이벤트는 값으로 저장되지 않고 바인딩은 로드한 객체에 없다, 다시 저장한 byte 같음), 바인딩이
// 있어도 저장 byte가 같음, 클래스 기본값 레코드·바뀐 필드 판정, 속성 경로가 이벤트 필드를 찾지
// 않음.
using namespace rocks;
using reflection_fixture::CallTarget;
using reflection_fixture::EventPublisher;
using reflection_fixture::EventPublisherChild;

namespace event_field_graph_tests_file {

namespace {

struct Registries {
    explicit Registries(Allocator& allocator) noexcept
        : types(allocator), registration(types, allocator),
          source(allocator, types, {0x45564701, 1}), target(allocator, types, {0x45564701, 2}) {
        if (!generated::ReflectionTestFixtures::RegisterModuleTypes(registration)) {
            platform::Fatal("fixture registration failed");
        }
    }

    TypeRegistry types;
    ModuleRegistration registration;
    ObjectRegistry source;
    ObjectRegistry target;
};

template <class T>
RootRef<T> New(ObjectRegistry& objects) noexcept {
    auto created = objects.NewObject<T>(objects.OwnerAccess(), TypeIdOf<T>());
    if (!created) {
        platform::Fatal("test object creation failed");
    }

    return Move(created.Value());
}

template <class T, class Edit>
void With(ObjectRegistry& objects, ObjectHandle handle, Edit&& edit) noexcept {
    auto borrowed = objects.ResolveForWrite<T>(objects.OwnerAccess(), handle);
    REQUIRE(borrowed);
    auto value = borrowed.Value().Get();
    REQUIRE(value);
    edit(*value.Value());
}

Guid FunctionIdOf(TypeRegistry& types, const char* name) noexcept {
    auto function = types.FindFunctionByName(TypeIdOf<CallTarget>(), name);
    return function ? function.Value().Schema().id : Guid{};
}

} // namespace

TEST(EventFieldGraph_RoundTripSkipsEvents) {
    SystemAllocator allocator;
    {
        Registries registries(allocator);
        ObjectRegistry& source = registries.source;
        const ObjectAccessContext access = source.OwnerAccess();
        RootRef<EventPublisherChild> publisher = New<EventPublisherChild>(source);
        RootRef<CallTarget> listener = New<CallTarget>(source);
        With<EventPublisherChild>(source, publisher.Handle(),
                                  [&](EventPublisherChild& value) noexcept {
                                      value.count = 7;
                                      REQUIRE(value.label.TryAssign("seven"));
                                  });

        // 바인딩이 없을 때와 있을 때 저장 byte가 같다(이벤트는 값으로 저장하지 않는다).
        auto plain = SaveObjectGraph(allocator, source, access, publisher.Handle());
        REQUIRE(plain);
        With<EventPublisherChild>(
            source, publisher.Handle(), [&](EventPublisherChild& value) noexcept {
                REQUIRE(value.changed.TryBind(source, access, listener.Handle(),
                                              FunctionIdOf(registries.types, "Add")));
                REQUIRE(value.extra.TryAddLambda([](int32) noexcept {}));
            });
        auto bound = SaveObjectGraph(allocator, source, access, publisher.Handle());
        REQUIRE(bound);
        CHECK(test::graph::SameBytes(plain.Value().View(), bound.Value().View()));

        // 로드한 객체는 값 필드만 되살리고 이벤트는 비어 있다. 다시 저장한 byte가 같다.
        ObjectRegistry& target = registries.target;
        auto loaded = LoadObjectGraph(allocator, target, registries.types, target.OwnerAccess(),
                                      bound.Value().View());
        REQUIRE(loaded);
        With<EventPublisherChild>(
            target, loaded.Value().Handle(), [](EventPublisherChild& value) noexcept {
                CHECK(value.count == 7 && value.label.View() == StringView("seven"));
                CHECK(!value.changed.IsBound() && !value.extra.IsBound() &&
                      !value.pinged.IsBound());
            });
        auto again =
            SaveObjectGraph(allocator, target, target.OwnerAccess(), loaded.Value().Handle());
        REQUIRE(again);
        CHECK(test::graph::SameBytes(bound.Value().View(), again.Value().View()));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 클래스 기본값 레코드가 만들어지고(이벤트 필드는 레코드에 없다), 바뀐 필드 판정은 값 필드만 본다.
// 속성 경로는 이벤트 필드 이름을 찾지 않는다.
TEST(EventFieldGraph_ClassDefaultsAndPathsSkipEvents) {
    SystemAllocator allocator;
    {
        Registries registries(allocator);
        ObjectRegistry& objects = registries.source;
        const ObjectAccessContext access = objects.OwnerAccess();
        auto lease = registries.types.Acquire(TypeIdOf<EventPublisher>());
        REQUIRE(lease);
        ClassDefaultsDiagnostic why;
        auto defaults = AcquireClassDefaults(lease.Value(), allocator, &why);
        REQUIRE(defaults);

        RootRef<EventPublisher> publisher = New<EventPublisher>(objects);
        RootRef<CallTarget> listener = New<CallTarget>(objects);
        With<EventPublisher>(objects, publisher.Handle(), [&](EventPublisher& value) noexcept {
            value.count = 3;
            REQUIRE(value.changed.TryBind(objects, access, listener.Handle(),
                                          FunctionIdOf(registries.types, "Add")));
        });
        Array<PropertyPathBuffer> changed(allocator);
        REQUIRE(FindChangedFields(objects, access, publisher.Handle(), allocator, changed));
        CHECK(changed.Size() == 1);

        PropertyPathBuffer path;
        CHECK(!ParsePropertyPath(objects, access, publisher.Handle(), "changed", path));
        CHECK(ParsePropertyPath(objects, access, publisher.Handle(), "count", path));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

} // namespace event_field_graph_tests_file
