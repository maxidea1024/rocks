#include <ReflectionTestFixtures/FunctionFixture.generated.h>
#include <Base/Compiler.h>
#include <Core/Memory/SystemAllocator.h>
#include <Object/EventOps.h>
#include <Object/ObjectRegistry.h>
#include <Object/TypeRegistry.h>
#include <TestSupport/Test.h>

// 리플렉션 이벤트 필드의 런타임 시험이다(Docs/Design/EventReflection.ko.md EV2a — ED15–ED17):
// 손으로 만든 schema·연산표(detail::MakeEventOps)로 등록 규칙(깊은 복사·거부 표), TypeLease 조회,
// 연산표 thunk(bind·unbind·unbindAll·clear·compact·broadcastFromFrame·frame construct·destroy)와
// 상속 필드 대조를 본다. 값 경로(저장·클래스 기본값·텍스트)는 생성 fixture로 EV2b가 본다.
using namespace rocks;
using reflection_fixture::CallTarget;

namespace event_field_tests_file {

namespace {

// 이벤트 필드 셋을 가진 손 등록 클래스다(인자 없음·int32·const String&+int32).
class EventCarrier : public Object {
public:
    explicit EventCarrier(Allocator& allocator) noexcept
        : pinged(allocator), changed(allocator), named(allocator) {
    }

    Event<> pinged;
    Event<int32> changed;
    Event<const String&, int32> named;
};

// 이벤트 필드 하나를 더한 파생이다(상속 필드 대조).
class EventCarrierChild : public EventCarrier {
public:
    using EventCarrier::EventCarrier;
};

constexpr TypeId CarrierType{0x45564643, 1};
constexpr TypeId ChildType{0x45564643, 2};
constexpr Guid PingedId{0x45564601, 1};
constexpr Guid ChangedId{0x45564602, 1};
constexpr Guid NamedId{0x45564603, 1};

using NamedShape = detail::EventFrameShape<decltype(EventCarrier::named)>;
using ChangedShape = detail::EventFrameShape<decltype(EventCarrier::changed)>;

Span<const char> Text(const char* text) noexcept {
    return Span<const char>(text, StringView(text).ByteCount());
}

// schema·연산표 한 벌이다(시험이 고쳐 거부를 본다).
struct CarrierTables {
    CarrierTables() noexcept {
        changedParameters[0] = {
            Text("amount"), {ValueKind::Int32, {}}, {}, {}, 0, 0, ChangedShape::Value.offsets[0]};
        namedParameters[0] = {
            Text("label"), {ValueKind::Utf8String, {}}, {}, {}, 0, 0, NamedShape::Value.offsets[0]};
        namedParameters[1] = {
            Text("count"), {ValueKind::Int32, {}}, {}, {}, 0, 0, NamedShape::Value.offsets[1]};
        fields[0] = EventField(PingedId, "pinged", {});
        fields[1] =
            EventField(ChangedId, "changed", Span<const ParameterSchema>(changedParameters, 1));
        fields[2] = EventField(NamedId, "named", Span<const ParameterSchema>(namedParameters, 2));
        events[0] = detail::MakeEventOps<&EventCarrier::pinged>(PingedId);
        events[1] = detail::MakeEventOps<&EventCarrier::changed>(ChangedId);
        events[2] = detail::MakeEventOps<&EventCarrier::named>(NamedId);
    }

    static FieldSchema EventField(Guid id, const char* name,
                                  Span<const ParameterSchema> parameters) noexcept {
        FieldSchema field;
        field.id = id;
        field.kind = FieldKind::Event;
        field.name = Text(name);
        field.eventParameters = parameters;
        return field;
    }

    TypeSchema Schema() const noexcept {
        TypeSchema schema{CarrierType, 1, Span<const FieldSchema>(fields, 3), Text("EventCarrier")};
        return schema;
    }

    RuntimeTypeOps Ops() const noexcept {
        RuntimeTypeOps ops = MakeRuntimeTypeOps<EventCarrier>();
        ops.events = Span<const RuntimeEventOps>(events, 3);
        ops.completeFieldLayout = true;
        return ops;
    }

    ParameterSchema changedParameters[1];
    ParameterSchema namedParameters[2];
    FieldSchema fields[3];
    RuntimeEventOps events[3];
};

ErrorCode RegisterError(TypeRegistry& types, const TypeSchema& schema,
                        RuntimeTypeOps ops) noexcept {
    auto registered = types.Register(schema, Move(ops));
    return registered ? ErrorCode::None : registered.Error();
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

template <class T>
ErrorCode ErrorOf(const Result<T>& result) noexcept {
    return result ? ErrorCode::None : result.Error();
}

// 발행 frame 저장소다(시험 이벤트들의 frame 크기·정렬 상한 — static_assert로 대조한다).
constexpr usize FrameStorageBytes = 128;
constexpr usize FrameStorageAlignment = 16;

struct alignas(FrameStorageAlignment) FrameStorage {
    unsigned char bytes[FrameStorageBytes];
};

static_assert(NamedShape::Value.size <= FrameStorageBytes &&
                  NamedShape::Value.alignment <= FrameStorageAlignment,
              "test frame storage must hold the named event frame");
static_assert(ChangedShape::Value.size <= FrameStorageBytes &&
                  ChangedShape::Value.alignment <= FrameStorageAlignment,
              "test frame storage must hold the changed event frame");

} // namespace

TEST(EventField_RegistersAndCopiesParameters) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        CarrierTables tables;
        REQUIRE(types.Register(tables.Schema(), tables.Ops()));
        REQUIRE(GeneratedType<CallTarget>::Register(types));
        auto lease = types.Acquire(CarrierType);
        REQUIRE(lease);
        const TypeSchema& schema = lease.Value().Schema();
        REQUIRE(schema.fields.Size() == 3);

        // 인자 서술과 이름은 깊게 복사되고 연산표는 필드 index로 찾는다(값 연산표는 없다).
        const usize named = lease.Value().FieldIndex(NamedId);
        REQUIRE(named != TypeLease::NoField);
        const FieldSchema& field = schema.fields[named];
        CHECK(IsEventField(field) && field.value.kind == ValueKind::Opaque);
        REQUIRE(field.eventParameters.Size() == 2);
        CHECK(field.eventParameters.Data() != tables.namedParameters);
        CHECK(field.eventParameters[0].name.Data() != tables.namedParameters[0].name.Data());
        CHECK(StringView(field.eventParameters[1].name.Data(),
                         field.eventParameters[1].name.Size()) == StringView("count"));
        CHECK(field.eventParameters[1].offset == NamedShape::Value.offsets[1]);
        CHECK(lease.Value().FieldOps(named) == nullptr);
        const RuntimeEventOps* ops = lease.Value().EventOps(named);
        REQUIRE(ops != nullptr);
        CHECK(ops != &tables.events[2] && ops->fieldId == NamedId);
        CHECK(ops->frameSize == NamedShape::Value.size && ops->parameters.Size() == 2);
        CHECK(ops->parameters.Data() != tables.events[2].parameters.Data());
        REQUIRE(ops->frameLayout != nullptr);
        CHECK(ops->frameLayout->digest == (detail::FrameLayoutOf<const String&, int32>().digest));

        // 이벤트가 아닌 필드의 EventOps는 nullptr다.
        auto target = types.Acquire(TypeIdOf<CallTarget>());
        REQUIRE(target);
        CHECK(target.Value().EventOps(0) == nullptr);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 등록 거부 표(ED15): 값 모양·flag·이름, 연산표 수·ID·thunk·인자, typed 연산표, 인자 이름 겹침.
TEST(EventField_RejectsBadSchemas) {
    SystemAllocator allocator;
    {
        struct Case {
            const char* what;
            void (*mutate)(CarrierTables& tables, RuntimeTypeOps& ops) noexcept;
            ErrorCode expected;
        };
        static RuntimeFieldOps stray;
        stray.fieldId = ChangedId;
        const Case cases[] = {
            {"typed value",
             [](CarrierTables& t, RuntimeTypeOps&) noexcept {
                 t.fields[1].value = {ValueKind::Int32, {}};
             },
             ErrorCode::InvalidArgument},
            {"read-only flag",
             [](CarrierTables& t, RuntimeTypeOps&) noexcept {
                 t.fields[1].flags = static_cast<uint32>(FieldFlags::ReadOnly);
             },
             ErrorCode::InvalidArgument},
            {"required flag",
             [](CarrierTables& t, RuntimeTypeOps&) noexcept {
                 t.fields[1].flags = static_cast<uint32>(FieldFlags::Required);
             },
             ErrorCode::InvalidArgument},
            {"text-export-transient flag",
             [](CarrierTables& t, RuntimeTypeOps&) noexcept {
                 t.fields[1].flags = static_cast<uint32>(FieldFlags::TextExportTransient);
             },
             ErrorCode::InvalidArgument},
            {"name is not an identifier",
             [](CarrierTables& t, RuntimeTypeOps&) noexcept {
                 t.fields[0].name = Text("on pinged");
             },
             ErrorCode::InvalidArgument},
            {"missing frame layout",
             [](CarrierTables& t, RuntimeTypeOps&) noexcept { t.events[2].frameLayout = nullptr; },
             ErrorCode::InvalidArgument},
            {"missing unbindFunction",
             [](CarrierTables& t, RuntimeTypeOps&) noexcept {
                 t.events[0].unbindFunction = nullptr;
             },
             ErrorCode::InvalidArgument},
            {"empty name",
             [](CarrierTables& t, RuntimeTypeOps&) noexcept { t.fields[0].name = {}; },
             ErrorCode::InvalidArgument},
            {"asset bundles",
             [](CarrierTables& t, RuntimeTypeOps&) noexcept {
                 t.fields[0].assetBundles = Text("Ui");
             },
             ErrorCode::InvalidArgument},
            {"missing event ops",
             [](CarrierTables& t, RuntimeTypeOps& ops) noexcept {
                 ops.events = Span<const RuntimeEventOps>(t.events, 2);
             },
             ErrorCode::InvalidArgument},
            {"event ops order",
             [](CarrierTables& t, RuntimeTypeOps&) noexcept {
                 const RuntimeEventOps first = t.events[0];
                 t.events[0] = t.events[1];
                 t.events[1] = first;
             },
             ErrorCode::InvalidArgument},
            {"null thunk",
             [](CarrierTables& t, RuntimeTypeOps&) noexcept {
                 t.events[1].broadcastFromFrame = nullptr;
             },
             ErrorCode::InvalidArgument},
            {"parameter count",
             [](CarrierTables& t, RuntimeTypeOps&) noexcept {
                 t.fields[2].eventParameters = Span<const ParameterSchema>(t.namedParameters, 1);
             },
             ErrorCode::InvalidArgument},
            {"parameter offset outside frame",
             [](CarrierTables& t, RuntimeTypeOps&) noexcept { t.namedParameters[1].offset = 4096; },
             ErrorCode::InvalidArgument},
            {"parameter without name",
             [](CarrierTables& t, RuntimeTypeOps&) noexcept { t.namedParameters[0].name = {}; },
             ErrorCode::InvalidArgument},
            {"strong reference parameter",
             [](CarrierTables& t, RuntimeTypeOps&) noexcept {
                 t.changedParameters[0].value = {ValueKind::ObjectReference, {}};
             },
             ErrorCode::InvalidArgument},
            {"parameter kind differs from the frame slot",
             [](CarrierTables& t, RuntimeTypeOps&) noexcept {
                 t.changedParameters[0].value = {ValueKind::Float32, {}};
             },
             ErrorCode::InvalidArgument},
            {"frame size differs from the layout",
             [](CarrierTables& t, RuntimeTypeOps&) noexcept { t.events[1].frameSize += 8; },
             ErrorCode::InvalidArgument},
            {"frame alignment not a power of two",
             [](CarrierTables& t, RuntimeTypeOps&) noexcept { t.events[1].frameAlignment = 3; },
             ErrorCode::InvalidArgument},
            {"duplicate parameter name",
             [](CarrierTables& t, RuntimeTypeOps&) noexcept {
                 t.namedParameters[1].name = Text("label");
             },
             ErrorCode::AlreadyExists},
            {"value field with parameters",
             [](CarrierTables& t, RuntimeTypeOps& ops) noexcept {
                 t.fields[1].kind = FieldKind::Value;
                 ops.events = Span<const RuntimeEventOps>(t.events, 2);
             },
             ErrorCode::InvalidArgument},
            {"value ops entry without a typed field",
             [](CarrierTables&, RuntimeTypeOps& ops) noexcept {
                 ops.fields = Span<const RuntimeFieldOps>(&stray, 1);
             },
             ErrorCode::InvalidArgument},
        };
        for (const Case& item : cases) {
            TypeRegistry types(allocator);
            CarrierTables tables;
            RuntimeTypeOps ops = tables.Ops();
            item.mutate(tables, ops);
            const ErrorCode error = RegisterError(types, tables.Schema(), Move(ops));
            if (error != item.expected) {
                // 어긋난 경우의 이름을 실패 문구로 남긴다.
                ::rocks::test::RecordFailure(item.what, __FILE__, __LINE__);
            }
        }

        // 바뀌지 않은 표는 받는다(거부가 표의 다른 이유에서 나지 않았다). 저장·복제 flag
        // (Transient·SaveGame·DuplicateTransient·NonTryoutDuplicateTransient)와 EditorOnly(ID
        // 목록과 함께)는 받는다.
        TypeRegistry types(allocator);
        CarrierTables tables;
        tables.fields[0].flags =
            static_cast<uint32>(FieldFlags::Transient) | static_cast<uint32>(FieldFlags::SaveGame);
        tables.fields[1].flags = static_cast<uint32>(FieldFlags::DuplicateTransient) |
                                 static_cast<uint32>(FieldFlags::NonTryoutDuplicateTransient);
        CHECK(RegisterError(types, tables.Schema(), tables.Ops()) == ErrorCode::None);
        TypeRegistry editorTypes(allocator);
        CarrierTables editorTables;
        editorTables.fields[2].flags = static_cast<uint32>(FieldFlags::EditorOnly);
        const Guid editorOnly[] = {NamedId};
        TypeSchema editorSchema = editorTables.Schema();
        editorSchema.editorOnlyFieldIds = Span<const Guid>(editorOnly, 1);
        CHECK(RegisterError(editorTypes, editorSchema, editorTables.Ops()) == ErrorCode::None);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 연산표 thunk로 바인딩·발행·해제한다(ED16).
TEST(EventField_OpsBindBroadcastAndUnbind) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        CarrierTables tables;
        REQUIRE(types.Register(tables.Schema(), tables.Ops()));
        REQUIRE(GeneratedType<CallTarget>::Register(types));
        ObjectRegistry registry(allocator, types, {0x45564654, 1});
        const ObjectAccessContext access = registry.OwnerAccess();
        auto carrier = registry.NewObject<EventCarrier>(access, CarrierType, {}, {});
        REQUIRE(carrier);
        auto listener = registry.NewObject<CallTarget>(access, TypeIdOf<CallTarget>(), {}, {});
        REQUIRE(listener);
        const ObjectHandle publisher = carrier.Value().Handle();
        const ObjectHandle target = listener.Value().Handle();
        auto lease = types.Acquire(CarrierType);
        REQUIRE(lease);
        const RuntimeEventOps& changed =
            *lease.Value().EventOps(lease.Value().FieldIndex(ChangedId));
        const RuntimeEventOps& named = *lease.Value().EventOps(lease.Value().FieldIndex(NamedId));
        const RuntimeEventOps& pinged = *lease.Value().EventOps(lease.Value().FieldIndex(PingedId));

        // int32 인자: frame을 만들고 매개변수 연산으로 값을 쓴 뒤 발행한다.
        auto bound = changed.bind(registry, access, publisher, target, FunctionIdOf(types, "Add"));
        REQUIRE(bound);
        CHECK(ErrorOf(changed.bind(registry, access, publisher, target,
                                   FunctionIdOf(types, "Add"))) == ErrorCode::AlreadyExists);
        CHECK(ErrorOf(changed.bind(registry, access, publisher, target,
                                   FunctionIdOf(types, "Bump"))) == ErrorCode::TypeMismatch);
        FrameStorage frame;
        REQUIRE(changed.frameSize <= sizeof(frame.bytes));
        REQUIRE(changed.construct(frame.bytes, allocator));
        FieldValue amount(allocator);
        amount.kind = ValueKind::Int32;
        amount.signedInteger = 7;
        REQUIRE(changed.parameters[0].write(
            registry, access, frame.bytes + tables.changedParameters[0].offset, amount, allocator));
        REQUIRE(changed.broadcastFromFrame(registry, access, publisher, frame.bytes));
        REQUIRE(changed.broadcastFromFrame(registry, access, publisher, frame.bytes));
        CHECK(TotalOf(registry, target) == 14);
        changed.destroy(frame.bytes);

        // const String&·int32 인자(값 연산으로 String 칸을 채운다).
        usize received = 0;
        // Note는 (const String&)라 (const String&, int32)와 서명이 다르다.
        CHECK(ErrorOf(named.bind(registry, access, publisher, target,
                                 FunctionIdOf(types, "Note"))) == ErrorCode::TypeMismatch);
        REQUIRE(named.construct(frame.bytes, allocator));
        FieldValue label(allocator);
        label.kind = ValueKind::Utf8String;
        REQUIRE(label.string.TryAssign("four"));
        REQUIRE(named.parameters[0].write(
            registry, access, frame.bytes + tables.namedParameters[0].offset, label, allocator));
        FieldValue count(allocator);
        count.kind = ValueKind::Int32;
        count.signedInteger = 3;
        REQUIRE(named.parameters[1].write(
            registry, access, frame.bytes + tables.namedParameters[1].offset, count, allocator));
        // 칸의 값이 그대로 전달되는지 native 구독으로 본다(const String& 칸 포함).
        {
            auto writable = registry.ResolveForWrite<EventCarrier>(access, publisher);
            REQUIRE(writable);
            auto object = writable.Value().Get();
            REQUIRE(object);
            REQUIRE(
                object.Value()->named.TryAddLambda([&](const String& text, int32 times) noexcept {
                    received = text.View().ByteCount() * 10 + static_cast<usize>(times);
                }));
        }
        REQUIRE(named.broadcastFromFrame(registry, access, publisher, frame.bytes));
        CHECK(received == 43);
        named.destroy(frame.bytes);

        // 인자 없는 이벤트와 해제 연산.
        auto bump = pinged.bind(registry, access, publisher, target, FunctionIdOf(types, "Bump"));
        REQUIRE(bump);
        REQUIRE(pinged.construct(frame.bytes, allocator));
        REQUIRE(pinged.broadcastFromFrame(registry, access, publisher, frame.bytes));
        CHECK(TotalOf(registry, target) == 114);
        REQUIRE(pinged.unbind(registry, access, publisher, bump.Value()));
        CHECK(ErrorOf(pinged.unbind(registry, access, publisher, bump.Value())) ==
              ErrorCode::NotFound);
        REQUIRE(pinged.broadcastFromFrame(registry, access, publisher, frame.bytes));
        CHECK(TotalOf(registry, target) == 114);
        CHECK(ErrorOf(pinged.broadcastFromFrame(registry, access, publisher, nullptr)) ==
              ErrorCode::InvalidArgument);
        pinged.destroy(frame.bytes);

        // 연산표 unbind는 리플렉션 바인딩만 지운다(native 구독 handle은 NotFound).
        auto borrow = registry.ResolveForWrite<EventCarrier>(access, publisher);
        REQUIRE(borrow);
        auto carrierObject = borrow.Value().Get();
        REQUIRE(carrierObject);
        int32 natives = 0;
        auto native =
            carrierObject.Value()->changed.TryAddLambda([&](int32) noexcept { ++natives; });
        REQUIRE(native);
        borrow.Value().Reset();
        CHECK(ErrorOf(changed.unbind(registry, access, publisher, native.Value())) ==
              ErrorCode::NotFound);
        REQUIRE(changed.construct(frame.bytes, allocator));
        REQUIRE(changed.broadcastFromFrame(registry, access, publisher, frame.bytes));
        changed.destroy(frame.bytes);
        CHECK(natives == 1);

        // (대상, 함수)로 지운다.
        REQUIRE(changed.unbindFunction(registry, access, publisher, target,
                                       FunctionIdOf(types, "Add")));
        CHECK(ErrorOf(changed.unbindFunction(registry, access, publisher, target,
                                             FunctionIdOf(types, "Add"))) == ErrorCode::NotFound);
        REQUIRE(changed.bind(registry, access, publisher, target, FunctionIdOf(types, "Add")));

        auto removed = changed.unbindAll(registry, access, publisher, target);
        REQUIRE(removed);
        CHECK(removed.Value() == 1);
        REQUIRE(changed.bind(registry, access, publisher, target, FunctionIdOf(types, "Add")));
        REQUIRE(changed.clear(registry, access, publisher));
        auto none = changed.unbindAll(registry, access, publisher, target);
        REQUIRE(none);
        CHECK(none.Value() == 0);

        // 대상이 사라지면 compact가 지운다.
        REQUIRE(changed.bind(registry, access, publisher, target, FunctionIdOf(types, "Add")));
        REQUIRE(registry.RequestDestroy(access, target));
        auto compacted = changed.compact(registry, access, publisher);
        REQUIRE(compacted);
        CHECK(compacted.Value() == 1);

        // 발행자 해석 오류: 다른 타입 handle·다른 문맥·파괴 요청된 발행자.
        auto other = registry.NewObject<CallTarget>(access, TypeIdOf<CallTarget>(), {}, {});
        REQUIRE(other);
        CHECK(ErrorOf(changed.clear(registry, access, other.Value().Handle())) ==
              ErrorCode::TypeMismatch);
        CHECK(ErrorOf(changed.clear(registry, ObjectAccessContext{{0x9999, 1}}, publisher)) ==
              ErrorCode::WrongContext);
        REQUIRE(registry.RequestDestroy(access, publisher));
        CHECK(ErrorOf(changed.clear(registry, access, publisher)) == ErrorCode::Destroyed);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 파생 클래스는 기반 이벤트 필드를 같은 인자 서술로 담아야 한다(ED15 상속 대조).
TEST(EventField_InheritedEventsMustMatch) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        CarrierTables tables;
        REQUIRE(types.Register(tables.Schema(), tables.Ops()));

        auto childOps = [&]() noexcept {
            RuntimeTypeOps ops = MakeRuntimeTypeOps<EventCarrierChild>();
            ops.events = Span<const RuntimeEventOps>(tables.events, 3);
            ops.completeFieldLayout = true;
            ops.nativeBaseType = NativeTypeKey<EventCarrier>();
            return ops;
        };
        TypeSchema child{ChildType, 1, Span<const FieldSchema>(tables.fields, 3),
                         Text("EventCarrierChild")};
        child.baseType = CarrierType;

        // 인자 이름이 다르면 상속 대조가 TypeMismatch다.
        ParameterSchema renamed[2] = {tables.namedParameters[0], tables.namedParameters[1]};
        renamed[1].name = Text("total");
        FieldSchema fields[3] = {tables.fields[0], tables.fields[1], tables.fields[2]};
        fields[2].eventParameters = Span<const ParameterSchema>(renamed, 2);
        TypeSchema mismatched = child;
        mismatched.fields = Span<const FieldSchema>(fields, 3);
        CHECK(RegisterError(types, mismatched, childOps()) == ErrorCode::TypeMismatch);

        // 필드 flag가 다르면(기반은 flag 없음) 상속 대조가 TypeMismatch다.
        FieldSchema flagged[3] = {tables.fields[0], tables.fields[1], tables.fields[2]};
        flagged[1].flags = static_cast<uint32>(FieldFlags::Transient);
        TypeSchema flaggedChild = child;
        flaggedChild.fields = Span<const FieldSchema>(flagged, 3);
        CHECK(RegisterError(types, flaggedChild, childOps()) == ErrorCode::TypeMismatch);

        // 같은 서술이면 받고, 기반 연산표를 파생 객체에 쓸 수 있다.
        REQUIRE(types.Register(child, childOps()));
        ObjectRegistry registry(allocator, types, {0x45564655, 1});
        const ObjectAccessContext access = registry.OwnerAccess();
        auto object = registry.NewObject<EventCarrierChild>(access, ChildType, {}, {});
        REQUIRE(object);
        auto lease = types.Acquire(ChildType);
        REQUIRE(lease);
        const RuntimeEventOps* ops = lease.Value().EventOps(lease.Value().FieldIndex(PingedId));
        REQUIRE(ops != nullptr);
        CHECK(ops->clear(registry, access, object.Value().Handle()));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

} // namespace event_field_tests_file
