#include <Meta/LaneTransfer.h>
#include <Meta/TypeSchema.h>
#include <ReflectionTestFixtures/ArrayFixture.generated.h>
#include <ReflectionTestFixtures/MapFixture.generated.h>
#include <ReflectionTestFixtures/ObjectRefMapFixture.generated.h>
#include <ReflectionTestFixtures/WeakObjectRefFixture.generated.h>
#include <TestSupport/Test.h>

using namespace rocks;

// lane 전달 등록·개념·스키마 검사 시험이다(Docs/Design/RenderBase 5.2절 "전달 가능한 타입", 개정
// 66 — RenderBase에서 MetaSchema로 옮김). 렌더 lane 게시 경로의 거부는 RenderBase
// RenderLane_OnlyRegisteredTypesCrossTheLane이 본다.
namespace {

// 리플렉션 없는 등록 값이다.
struct PlainPacket {
    uint32 value = 0;
};

// 등록하지 않은 값이다.
struct Unregistered {
    uint32 value = 0;
};

// 이동 생성이 noexcept가 아닌 등록 값이다(개념이 거부한다).
struct ThrowingMove {
    ThrowingMove() noexcept = default;

    ThrowingMove(ThrowingMove&&) noexcept(false) {
    }
};

// 소멸이 noexcept가 아닌 등록 값이다(개념이 거부한다).
struct ThrowingDestroy {
    ~ThrowingDestroy() noexcept(false) {
    }
};

constexpr char ValueName[] = "value";
constexpr char LinkName[] = "link";

// 이름 하나·값 형식 하나인 손으로 만든 필드다.
constexpr FieldSchema MakeField(uint64 id, FieldKind kind, ValueType value,
                                ValueType element = {}) noexcept {
    FieldSchema field;
    field.id = Guid{1, id};
    field.kind = kind;
    field.name = Span<const char>(LinkName, sizeof(LinkName) - 1);
    field.value = value;
    field.element = element;
    return field;
}

// 값 필드 하나 뒤에 field를 둔 스키마가 허용되는지다.
constexpr bool AllowedAfterValue(FieldSchema field) noexcept {
    FieldSchema fields[2];
    fields[0].id = Guid{1, 100};
    fields[0].kind = FieldKind::Value;
    fields[0].name = Span<const char>(ValueName, sizeof(ValueName) - 1);
    fields[0].value = ValueType{ValueKind::UInt32, TypeId{}};
    fields[1] = field;
    TypeSchema schema;
    schema.fields = Span<const FieldSchema>(fields, 2);
    return detail::LaneTransferSchemaAllowed(schema);
}

} // namespace

ROCKS_LANE_TRANSFERABLE(PlainPacket);
ROCKS_LANE_TRANSFERABLE(ThrowingMove);
ROCKS_LANE_TRANSFERABLE(ThrowingDestroy);
ROCKS_LANE_TRANSFERABLE(PlainPacket*);
// 리플렉션 struct: ArrayTag·MapPoint는 참조가 없고, WeakLink는 WeakObjectRef 필드를, ArraySlot은
// ObjectRef 필드를 가진다. WeakLinkGroup은 WeakLink 원소 배열이다(중첩 struct 안은 보지 않는다).
ROCKS_LANE_TRANSFERABLE(rocks::reflection_fixture::ArrayTag);
ROCKS_LANE_TRANSFERABLE(rocks::reflection_fixture::MapPoint);
ROCKS_LANE_TRANSFERABLE(rocks::reflection_fixture::WeakLink);
ROCKS_LANE_TRANSFERABLE(rocks::reflection_fixture::WeakLinkGroup);
ROCKS_LANE_TRANSFERABLE(rocks::reflection_fixture::ArraySlot);

// 개념은 등록·참조 아님·포인터 아님·nothrow 이동·nothrow 소멸을 모두 요구한다.
TEST(LaneTransfer_ConceptRequiresRegistrationAndNothrowValueType) {
    static_assert(LaneTransferable<PlainPacket>);
    static_assert(!LaneTransferable<Unregistered>);
    static_assert(!LaneTransferable<PlainPacket&>);
    static_assert(!LaneTransferable<PlainPacket&&>);
    // 포인터 타입 자체는 등록해도 받지 않는다.
    static_assert(LaneTransferRegistered<PlainPacket*> && !LaneTransferable<PlainPacket*>);
    static_assert(!LaneTransferable<ThrowingMove>);
    static_assert(!LaneTransferable<ThrowingDestroy>);
    static_assert(LaneTransferable<rocks::reflection_fixture::ArrayTag>);
    static_assert(LaneTransferable<rocks::reflection_fixture::WeakLink>);
    // ObjectRef는 이동할 수 없으므로 그 필드를 가진 struct는 등록해도 C++ 타입 검사가 거부한다.
    static_assert(LaneTransferRegistered<rocks::reflection_fixture::ArraySlot> &&
                  !LaneTransferable<rocks::reflection_fixture::ArraySlot>);
    CHECK(true);
}

// 리플렉션 스키마가 없는 등록 타입과 참조 없는 리플렉션 struct는 통과하고, WeakObjectRef 필드가
// 있는 리플렉션 struct는 Development에서 InvalidArgument다(Release는 검사 생략).
TEST(LaneTransfer_CheckUsesReflectionSchemaInDevelopment) {
    CHECK(CheckLaneTransfer<PlainPacket>());
    CHECK(CheckLaneTransfer<rocks::reflection_fixture::ArrayTag>());
    CHECK(CheckLaneTransfer<rocks::reflection_fixture::MapPoint>());
    const Result<void> weak = CheckLaneTransfer<rocks::reflection_fixture::WeakLink>();
    if constexpr (IsReleaseBuild) {
        CHECK(weak);
    } else {
        CHECK(!weak && weak.Error() == ErrorCode::InvalidArgument);
    }

    // 한계: 중첩 struct 원소(Array<WeakLink>) 안의 WeakObjectRef는 보지 않는다(헤더의 "강제 범위").
    CHECK(CheckLaneTransfer<rocks::reflection_fixture::WeakLinkGroup>());
}

// 생성 스키마 판정: 강한 참조(ObjectRef·ObjectRefArray·ObjectRefMap·StructRefArray)와 약한 참조
// 필드를 가진 스키마는 거부하고 값만 있는 스키마는 허용한다(개념을 만족하지 못하는 타입도 스키마는
// 직접 볼 수 있다).
TEST(LaneTransfer_SchemaRejectsGeneratedReferenceFields) {
    using namespace rocks::reflection_fixture;
    CHECK(detail::LaneTransferSchemaAllowed(GeneratedType<ArrayTag>::Schema()));
    CHECK(detail::LaneTransferSchemaAllowed(GeneratedType<MapPoint>::Schema()));
    CHECK(!detail::LaneTransferSchemaAllowed(GeneratedType<WeakLink>::Schema()));
    CHECK(!detail::LaneTransferSchemaAllowed(GeneratedType<ArraySlot>::Schema()));
    CHECK(!detail::LaneTransferSchemaAllowed(GeneratedType<ArrayBundle>::Schema()));
    CHECK(!detail::LaneTransferSchemaAllowed(GeneratedType<RefMapBundle>::Schema()));
}

// 손으로 만든 스키마로 판정 갈래마다 본다: 참조 필드 종류, 값·원소 형식의 강한·약한 참조, 이벤트
// 필드(FieldKind::Event — 구독 대상·발행자를 붙잡는다, 개정 61). 생성 fixture의 이벤트 필드는
// 클래스에만 있어(struct에는 이벤트가 없다) 전달 대상이 될 수 없으므로 손으로 만든 스키마를 쓴다.
TEST(LaneTransfer_RejectsEventFields) {
    constexpr ValueType UInt32Value{ValueKind::UInt32, TypeId{}};
    constexpr ValueType StrongValue{ValueKind::ObjectReference, TypeId{}};
    constexpr ValueType WeakValue{ValueKind::WeakObjectReference, TypeId{}};
    constexpr ValueType ArrayValue{ValueKind::Array, TypeId{}};
    constexpr ValueType MapValue{ValueKind::Map, TypeId{}};
    constexpr ValueType SoftValue{ValueKind::SoftObjectReference, TypeId{}};

    // 빈 스키마와 값 필드만 있는 스키마는 허용한다.
    static_assert(detail::LaneTransferSchemaAllowed(TypeSchema{}));
    static_assert(AllowedAfterValue(MakeField(1, FieldKind::Value, UInt32Value)));
    // 소프트 참조는 경로 값이어서 게임 객체를 붙잡지 않는다.
    static_assert(AllowedAfterValue(MakeField(2, FieldKind::Value, SoftValue)));
    static_assert(AllowedAfterValue(MakeField(3, FieldKind::Value, ArrayValue, UInt32Value)));
    // 필드 종류가 Object 참조이거나 이벤트면 값 형식과 상관없이 거부한다.
    static_assert(!AllowedAfterValue(MakeField(4, FieldKind::ObjectReference, StrongValue)));
    static_assert(!AllowedAfterValue(MakeField(5, FieldKind::Event, ValueType{})));
    // 값 형식이 강한·약한 참조면 거부한다.
    static_assert(!AllowedAfterValue(MakeField(6, FieldKind::Value, StrongValue)));
    static_assert(!AllowedAfterValue(MakeField(7, FieldKind::Value, WeakValue)));
    // 컨테이너 원소·값 형식이 참조면 거부한다(Array<WeakObjectRef>·OrderedMap<K, WeakObjectRef>).
    static_assert(!AllowedAfterValue(MakeField(8, FieldKind::Value, ArrayValue, WeakValue)));
    static_assert(!AllowedAfterValue(MakeField(9, FieldKind::Value, MapValue, WeakValue)));
    static_assert(!AllowedAfterValue(MakeField(10, FieldKind::Value, ArrayValue, StrongValue)));

    // 실행 중 판정도 같다(이벤트 필드 하나만 있는 스키마 포함).
    FieldSchema fields[2] = {MakeField(11, FieldKind::Value, UInt32Value),
                             MakeField(12, FieldKind::Event, ValueType{})};
    TypeSchema values;
    values.fields = Span<const FieldSchema>(fields, 1);
    CHECK(detail::LaneTransferSchemaAllowed(values));
    TypeSchema events;
    events.fields = Span<const FieldSchema>(fields, 2);
    CHECK(IsEventField(fields[1]) && !detail::LaneTransferSchemaAllowed(events));
    TypeSchema onlyEvent;
    onlyEvent.fields = Span<const FieldSchema>(fields + 1, 1);
    CHECK(!detail::LaneTransferSchemaAllowed(onlyEvent));
}
