#include <ReflectionTestFixtures/EventFixture.generated.h>
#include <ReflectionTestFixtures/EventRelayLeafFixture.generated.h>
#include <ReflectionTestFixtures/FunctionFixture.generated.h>
#include <ReflectionTestFixtures/ReflectionTestFixtures.module.generated.h>
#include <Base/Compiler.h>
#include <Core/Memory/SystemAllocator.h>
#include <Object/EventOps.h>
#include <Object/ModuleRegistration.h>
#include <Object/ObjectRegistry.h>
#include <Object/TypeRegistry.h>
#include <TestSupport/Test.h>

// 생성 이벤트 필드 시험이다(Docs/Design/EventReflection.ko.md EV2b — ED15–ED18, fixture
// ReflectionTestFixtures/EventFixture.h): 생성 schema(이벤트 필드 종류·flag·인자 이름·형식·
// ObjectHandle flag·offset), 값 연산표와 이벤트 연산표의 분리, EditorOnly 이벤트, 상속 이벤트를
// 다시 싣는 파생, 생성 연산표로 바인딩·발행, 로그 방문(VisitFields)에서 빠짐, encode 고정 중 쓰기
// 연산의 Busy.
using namespace rocks;
using reflection_fixture::CallMode;
using reflection_fixture::CallPoint;
using reflection_fixture::CallTarget;
using reflection_fixture::EventPublisher;
using reflection_fixture::EventPublisherChild;

namespace event_field_generated_tests_file {

namespace {

const FieldSchema* FieldNamed(const TypeSchema& schema, StringView name) noexcept {
    for (const FieldSchema& field : schema.fields) {
        if (StringView(field.name.Data(), field.name.Size()) == name) {
            return &field;
        }
    }

    return nullptr;
}

StringView View(Span<const char> text) noexcept {
    return StringView(text.Data(), text.Size());
}

Guid FunctionIdOf(TypeRegistry& types, const char* name) noexcept {
    auto function = types.FindFunctionByName(TypeIdOf<CallTarget>(), name);
    return function ? function.Value().Schema().id : Guid{};
}

int32 TotalOf(ObjectRegistry& registry, ObjectHandle handle) noexcept {
    auto borrow = registry.Resolve<CallTarget>(registry.OwnerAccess(), handle);
    if (!borrow) {
        return -1;
    }

    auto target = borrow.Value().Get();
    return target ? target.Value()->total : -1;
}

// 로그 방문이 받은 필드 이름을 센다.
struct NameCounter {
    template <class Value>
    constexpr void Field(StringView name, const Value&) noexcept {
        ++fields;
        events += name == StringView("changed") || name == StringView("pinged") ? 1 : 0;
    }

    int fields = 0;
    int events = 0;
};

} // namespace

TEST(EventFieldGenerated_SchemaDescribesEvents) {
    const TypeSchema& schema = GeneratedType<EventPublisher>::Schema();
    const RuntimeTypeOps ops = GeneratedType<EventPublisher>::Ops();
    usize events = 0;
    for (const FieldSchema& field : schema.fields) {
        events += IsEventField(field) ? 1 : 0;
    }

#if ROCKS_WITH_EDITOR_DATA
    constexpr usize ExpectedEvents = 5;
#else
    constexpr usize ExpectedEvents = 4;
#endif
    CHECK(events == ExpectedEvents && ops.events.Size() == ExpectedEvents);
    CHECK(ops.fields.Size() == 3 && schema.fields.Size() == ExpectedEvents + 3);
    CHECK(ops.completeFieldLayout);

    // 값 인자·Transient·SaveGame·인자 없음.
    const FieldSchema* changed = FieldNamed(schema, "changed");
    REQUIRE(changed != nullptr && IsEventField(*changed));
    REQUIRE(changed->eventParameters.Size() == 1);
    CHECK(View(changed->eventParameters[0].name) == StringView("amount"));
    CHECK(changed->eventParameters[0].value.kind == ValueKind::Int32);
    const FieldSchema* named = FieldNamed(schema, "named");
    REQUIRE(named != nullptr && named->eventParameters.Size() == 2);
    CHECK(named->flags == static_cast<uint32>(FieldFlags::Transient));
    CHECK(named->eventParameters[0].value.kind == ValueKind::Utf8String);
    const FieldSchema* pinged = FieldNamed(schema, "pinged");
    REQUIRE(pinged != nullptr && pinged->eventParameters.Size() == 0);
    CHECK(pinged->flags == static_cast<uint32>(FieldFlags::SaveGame));

    // struct·enum·ObjectHandle 인자(형식 ID·ObjectHandle flag·frame offset).
    using PlacedShape = detail::EventFrameShape<Event<const CallPoint&, CallMode, ObjectHandle>>;
    const FieldSchema* placed = FieldNamed(schema, "placed");
    REQUIRE(placed != nullptr && placed->eventParameters.Size() == 3);
    CHECK(placed->eventParameters[0].value.kind == ValueKind::Struct);
    CHECK(placed->eventParameters[0].value.type == TypeIdOf<CallPoint>());
    CHECK(placed->eventParameters[1].value.kind == ValueKind::Enum);
    CHECK(placed->eventParameters[2].value.kind == ValueKind::WeakObjectReference);
    CHECK(placed->eventParameters[2].flags == static_cast<uint32>(ParameterFlags::ObjectHandle));
    for (uint32 index = 0; index < 3; ++index) {
        CHECK(placed->eventParameters[index].offset == PlacedShape::Value.offsets[index]);
    }

#if ROCKS_WITH_EDITOR_DATA
    const FieldSchema* edited = FieldNamed(schema, "edited");
    REQUIRE(edited != nullptr && IsEventField(*edited));
    CHECK(edited->flags == static_cast<uint32>(FieldFlags::EditorOnly));
#else
    CHECK(FieldNamed(schema, "edited") == nullptr);
#endif
    CHECK(schema.editorOnlyFieldIds.Size() == 1);

    // 파생은 상속 이벤트를 다시 싣고 하나를 더한다.
    const TypeSchema& child = GeneratedType<EventPublisherChild>::Schema();
    CHECK(child.fields.Size() == schema.fields.Size() + 1);
    CHECK(GeneratedType<EventPublisherChild>::Ops().events.Size() == ExpectedEvents + 1);
    const FieldSchema* inherited = FieldNamed(child, "placed");
    REQUIRE(inherited != nullptr && inherited->id == placed->id);
    CHECK(inherited->eventParameters[2].flags == placed->eventParameters[2].flags);

    // 이벤트를 선언한 조상이 바로 위 base와 다른 헤더에 있는 파생도 상속 이벤트 연산을 싣는다(생성
    // 소스가 선언 클래스의 생성 헤더를 include한다 — PhysicsModule 7.1.14절 회귀).
    CHECK(GeneratedType<reflection_fixture::EventRelayLeaf>::Ops().events.Size() ==
          ExpectedEvents + 1);
}

// 생성 연산표로 바인딩·발행하고, 파생 객체에 상속 이벤트 연산을 쓴다.
TEST(EventFieldGenerated_OpsBindAndBroadcast) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        ModuleRegistration registration(types, allocator);
        REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(registration));
        ObjectRegistry registry(allocator, types, {0x45564742, 1});
        const ObjectAccessContext access = registry.OwnerAccess();
        auto child = registry.NewObject<EventPublisherChild>(
            access, TypeIdOf<EventPublisherChild>(), {}, {});
        REQUIRE(child);
        auto listener = registry.NewObject<CallTarget>(access, TypeIdOf<CallTarget>(), {}, {});
        REQUIRE(listener);
        const ObjectHandle publisher = child.Value().Handle();
        const ObjectHandle target = listener.Value().Handle();
        auto lease = types.Acquire(TypeIdOf<EventPublisherChild>());
        REQUIRE(lease);
        const usize changedIndex =
            lease.Value().FieldIndex(FieldNamed(lease.Value().Schema(), "changed")->id);
        const usize extraIndex =
            lease.Value().FieldIndex(FieldNamed(lease.Value().Schema(), "extra")->id);
        const RuntimeEventOps* changed = lease.Value().EventOps(changedIndex);
        const RuntimeEventOps* extra = lease.Value().EventOps(extraIndex);
        REQUIRE(changed != nullptr && extra != nullptr);
        CHECK(lease.Value().FieldOps(changedIndex) == nullptr);

        const Guid add = FunctionIdOf(types, "Add");
        REQUIRE(changed->bind(registry, access, publisher, target, add));
        REQUIRE(extra->bind(registry, access, publisher, target, add));
        alignas(16) unsigned char frame[64];
        REQUIRE(changed->frameSize <= sizeof(frame));
        REQUIRE(changed->construct(frame, allocator));
        FieldValue amount(allocator);
        amount.kind = ValueKind::Int32;
        amount.signedInteger = 5;
        REQUIRE(changed->parameters[0].write(
            registry, access,
            frame + FieldNamed(lease.Value().Schema(), "changed")->eventParameters[0].offset,
            amount, allocator));
        REQUIRE(changed->broadcastFromFrame(registry, access, publisher, frame));
        REQUIRE(extra->broadcastFromFrame(registry, access, publisher, frame));
        changed->destroy(frame);
        CHECK(TotalOf(registry, target) == 10);

        // C++에서 발행해도 같은 바인딩이 불린다.
        {
            auto borrow = registry.ResolveForWrite<EventPublisherChild>(access, publisher);
            REQUIRE(borrow);
            auto object = borrow.Value().Get();
            REQUIRE(object);
            REQUIRE(
                object.Value()->changed.TryBroadcast(EventContext{registry, access, publisher}, 1));
        }
        CHECK(TotalOf(registry, target) == 11);

        // encode 고정 중이면 바인딩을 바꾸는 연산은 Busy다(발행은 읽기라 된다).
        {
            auto pin = registry.PinForEncode(access, publisher);
            REQUIRE(pin);
            auto busy = changed->clear(registry, access, publisher);
            CHECK(!busy && busy.Error() == ErrorCode::Busy);
            REQUIRE(changed->construct(frame, allocator));
            CHECK(changed->broadcastFromFrame(registry, access, publisher, frame));
            changed->destroy(frame);
        }
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 로그 방문(VisitFields — {@Obj})은 값 필드만 지난다.
TEST(EventFieldGenerated_VisitFieldsSkipsEvents) {
    SystemAllocator allocator;
    EventPublisher publisher(allocator);
    NameCounter counter;
    GeneratedType<EventPublisher>::VisitFields(publisher, counter);
    CHECK(counter.fields == 3 && counter.events == 0);
}

} // namespace event_field_generated_tests_file
