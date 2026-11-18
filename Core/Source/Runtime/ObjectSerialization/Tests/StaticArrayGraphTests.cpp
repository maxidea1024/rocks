#include <ReflectionTestFixtures/ReflectionTestFixtures.module.generated.h>
#include <Object/NativeMemberTraits.h>
#include <Serialization/RecordList.h>
#include <Serialization/ValueArray.h>
#include "MapTestSupport.h"

// 리플렉션 B9g 정적 배열(T x[N]) PROPERTY의 생성 코드·그래프 저장·복원 시험이다(Docs/Design/
// ReflectionExtensions.ko.md 8절, fixture ReflectionTestFixtures/StaticArrayFixture.h): 생성
// 스키마({StaticArray, 0}·원소 형식·maxElements = N·ObjectRef 원소의 FieldKind::ObjectReference)와
// 함수표, 원소 종류별 왕복·재저장 byte 동일, 생성 Trace가 ObjectRef 원소와 참조 struct 원소를
// 방문(그 원소로만 닿는 객체가 수집에서 살고 순환 그래프는 root를 놓으면 회수), count ≠ N은
// InvalidArgument, Required 원소의 null은 저장 거부, NATIVE 형질의 C 배열 특수화.
using namespace rocks;
using namespace rocks::test::map;
using reflection_fixture::FixedLink;
using reflection_fixture::FixedMode;
using reflection_fixture::FixedNeed;
using reflection_fixture::FixedOwner;
using reflection_fixture::FixedPoint;
using reflection_fixture::FixedRequired;

// NATIVE 형질은 C 배열을 원소로 판정한다(배열 멤버·배열을 가리키는 포인터·다차원·Registry 컨테이너
// 배열 — 생성 static_assert가 ROCKS_NATIVE 배열 멤버의 강한 참조를 잡는다).
static_assert(IsNativeStrongReference<ObjectRef<FixedOwner>[4]>);
static_assert(detail::NativeStrongReference<ObjectRef<FixedOwner>[4]>::value);
static_assert(detail::NativeStrongReference<ObjectRef<FixedOwner> (*)[4]>::value);
static_assert(detail::NativeStrongReference<ObjectRef<FixedOwner>[2][3]>::value);
static_assert(!detail::NativeStrongReference<WeakObjectRef<FixedOwner>[4]>::value);
static_assert(!detail::NativeStrongReference<uint32[4]>::value);
static_assert(detail::RegistryContainer<ObjectRefArray<FixedOwner>[2]>::value);
static_assert(!detail::RegistryContainer<ObjectRef<FixedOwner>[2]>::value);
static_assert(GeneratedType<FixedLink>::ContainsStrongReference);
static_assert(!GeneratedType<FixedLink>::ContainsReferenceArray);
static_assert(!GeneratedType<FixedPoint>::ContainsStrongReference);

namespace {

// 모듈 집계 등록과 Name pool 묶음이다.
struct Fixture {
    explicit Fixture(Allocator& allocator) noexcept
        : types(allocator), names(allocator), registration(types, allocator) {
        REQUIRE(names.Initialize());
        REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(registration));
    }

    ~Fixture() noexcept {
        CHECK(registration.Unregister());
        CHECK(types.Shutdown());
    }

    TypeRegistry types;
    NamePool names;
    ModuleRegistration registration;
};

Sha256Digest Digest(uint8 first) noexcept {
    Sha256Digest digest;
    for (usize index = 0; index < Sha256Digest::Bytes; ++index) {
        digest.bytes[index] = static_cast<uint8>(first + index);
    }

    return digest;
}

// holder 하나와 원소로만 닿는 객체 둘(refs[1]의 first, links[1].target의 second)을 한 트랜잭션으로
// 만든다. holder.refs[0]은 자기 자신이고 first.refs[0]이 holder라 순환이다. 모든 원소 종류를
// 채운다.
Result<RootRef<Object>> MakeGraph(ObjectRegistry& objects, NamePool& names) noexcept {
    ConstructionContext transaction(objects, objects.OwnerAccess());
    auto holder = transaction.Create<FixedOwner>(TypeIdOf<FixedOwner>());
    auto first = transaction.Create<FixedOwner>(TypeIdOf<FixedOwner>());
    auto second = transaction.Create<FixedOwner>(TypeIdOf<FixedOwner>());
    auto socket = names.TryIntern("Socket");
    if (!holder || !first || !second || !socket) {
        return Result<RootRef<Object>>::Failure(ErrorCode::OutOfMemory);
    }

    auto pointer = transaction.Get<FixedOwner>(holder.Value());
    auto firstPointer = transaction.Get<FixedOwner>(first.Value());
    if (!pointer || !firstPointer) {
        return Result<RootRef<Object>>::Failure(ErrorCode::OutOfMemory);
    }

    FixedOwner& value = *pointer.Value();
    value.flags[0] = true;
    value.flags[2] = true;
    value.bytes[1] = NumericLimits<uint8>::Max();
    value.words[1] = NumericLimits<uint16>::Max();
    value.counts[1] = NumericLimits<uint32>::Max();
    value.bigs[1] = NumericLimits<uint64>::Max();
    value.tiny[0] = NumericLimits<int8>::Min();
    value.tiny[1] = NumericLimits<int8>::Max();
    value.smalls[0] = NumericLimits<int16>::Min();
    value.smalls[1] = NumericLimits<int16>::Max();
    value.ints[0] = 1;
    value.ints[1] = -2;
    value.ints[3] = NumericLimits<int32>::Min();
    value.wides[0] = NumericLimits<int64>::Min();
    value.wides[1] = NumericLimits<int64>::Max();
    value.ratios[0] = 0.5f;
    value.ratios[1] = -1.25f;
    value.precise[1] = 1.0e300;
    value.guids[0] = Guid{1, 2};
    value.names[0] = socket.Value();
    value.watchers[0] = WeakObjectRef<FixedOwner>(first.Value());
    value.kinds[0] = ClassRef<FixedOwner>(TypeIdOf<FixedOwner>());
    value.modes[0] = FixedMode::Second;
    value.points[1].x = 7;
    value.points[1].pair[0] = -1;
    value.points[1].pair[1] = 9;
    value.links[0].weight = 11;
    value.assets[0] = AssetId{Guid{5, 6}};
    value.digests[1] = Digest(3);
    const ObjectHandle owner = holder.Value();
    const bool ok =
        value.labels[0].TryAssign("alpha").IsOk() &&
        value.softs[0].TryAssign(AssetId{Guid{7, 7}}, "Mesh.A").IsOk() &&
        transaction.SetReference(owner, value.refs[0], owner).IsOk() &&
        transaction.SetReference(owner, value.refs[1], first.Value()).IsOk() &&
        transaction.SetReference(owner, value.links[1].target, second.Value()).IsOk() &&
        transaction.SetReference(first.Value(), firstPointer.Value()->refs[0], owner).IsOk();
    if (!ok) {
        return Result<RootRef<Object>>::Failure(ErrorCode::OutOfMemory);
    }

    return transaction.Commit();
}

// 로드한 holder가 MakeGraph가 넣은 그대로인지 본다(self는 holder handle). first·second를 돌려준다.
bool CheckFilled(ObjectRegistry& objects, NamePool& names, ObjectHandle self,
                 const FixedOwner& value, ObjectHandle& first, ObjectHandle& second) noexcept {
    const auto access = objects.OwnerAccess();
    auto socket = names.TryFind(StringView("Socket"));
    first = value.refs[1].Handle();
    second = value.links[1].target.Handle();
    return socket && value.flags[0] && !value.flags[1] && value.flags[2] && value.bytes[0] == 0 &&
           value.bytes[1] == NumericLimits<uint8>::Max() &&
           value.words[1] == NumericLimits<uint16>::Max() &&
           value.counts[1] == NumericLimits<uint32>::Max() &&
           value.bigs[1] == NumericLimits<uint64>::Max() &&
           value.tiny[0] == NumericLimits<int8>::Min() &&
           value.tiny[1] == NumericLimits<int8>::Max() &&
           value.smalls[0] == NumericLimits<int16>::Min() &&
           value.smalls[1] == NumericLimits<int16>::Max() && value.ints[0] == 1 &&
           value.ints[1] == -2 && value.ints[2] == 0 &&
           value.ints[3] == NumericLimits<int32>::Min() &&
           value.wides[0] == NumericLimits<int64>::Min() &&
           value.wides[1] == NumericLimits<int64>::Max() && value.ratios[0] == 0.5f &&
           value.ratios[1] == -1.25f && value.precise[0] == 0.0 && value.precise[1] == 1.0e300 &&
           value.guids[0] == Guid{1, 2} && value.guids[1] == Guid{} &&
           value.labels[0].View() == "alpha" && value.labels[1].View().ByteCount() == 0 &&
           value.names[0] == socket.Value() && value.names[1] == Name{} &&
           value.refs[0].Handle() == self && !first.IsNull() && value.refs[2].Handle().IsNull() &&
           value.watchers[0].Handle() == first && value.watchers[1].Handle().IsNull() &&
           value.kinds[0].Id() == TypeIdOf<FixedOwner>() && value.kinds[1].Id() == TypeId{} &&
           value.modes[0] == FixedMode::Second && value.modes[1] == FixedMode::First &&
           value.points[0].x == 0 && value.points[1].x == 7 && value.points[1].pair[0] == -1 &&
           value.points[1].pair[1] == 9 && value.links[0].weight == 11 &&
           value.links[0].target.Handle().IsNull() && !second.IsNull() && second != first &&
           value.assets[0] == AssetId{Guid{5, 6}} && value.assets[1] == AssetId{} &&
           value.softs[0].asset == AssetId{Guid{7, 7}} &&
           value.softs[0].objectPath.View() == "Mesh.A" && value.softs[1].IsNull() &&
           value.digests[0] == Sha256Digest{} && value.digests[1] == Digest(3) &&
           objects.Resolve<FixedOwner>(access, first).IsOk() &&
           objects.Resolve<FixedOwner>(access, second).IsOk();
}

// FixedOwner 노드 하나짜리 그래프다(fields 밖의 필드는 누락 = 생성자 값).
Array<uint8> OwnerGraph(Allocator& allocator, Span<const FieldBytes> fields) noexcept {
    auto record = RecordOf(allocator, TypeIdOf<FixedOwner>(), TypeOf<FixedOwner>().version, fields);
    auto graph = test::graph::Single(allocator, TypeIdOf<FixedOwner>(), record.View());
    if (!graph) {
        platform::Fatal("test graph build failed");
    }

    return Move(graph.Value());
}

// count개 원소의 ValueArray payload다(I64 원소는 index, U32 참조 원소는 0 = null).
Array<uint8> ValueArrayPayload(Allocator& allocator, WireType wire, uint32 count) noexcept {
    ArchiveWriter payload(allocator);
    auto writer = ValueArrayWriter::Begin(payload, wire, count, NumericLimits<usize>::Max());
    if (!writer) {
        platform::Fatal("test value array begin failed");
    }

    for (uint32 index = 0; index < count; ++index) {
        const bool written =
            wire == WireType::I64 ? payload.WriteI64(index).IsOk() : payload.WriteU32(0).IsOk();
        if (!written) {
            platform::Fatal("test value array write failed");
        }
    }

    if (!writer.Value().End()) {
        platform::Fatal("test value array end failed");
    }

    return payload.TakeBytes();
}

// count개 빈 FixedPoint 항목의 RecordList payload다(멤버 누락 = 생성자 값).
Array<uint8> PointListPayload(Allocator& allocator, uint32 count) noexcept {
    auto item = RecordOf(allocator, TypeIdOf<FixedPoint>(), TypeOf<FixedPoint>().version, {});
    ArchiveWriter payload(allocator);
    auto list = RecordListWriter::Begin(payload, count);
    if (!list) {
        platform::Fatal("test record list begin failed");
    }

    for (uint32 index = 0; index < count; ++index) {
        if (!list.Value().BeginItem() || !payload.WriteRaw(item.View()) ||
            !list.Value().EndItem()) {
            platform::Fatal("test record list item failed");
        }
    }

    if (!list.Value().End()) {
        platform::Fatal("test record list end failed");
    }

    return payload.TakeBytes();
}

// 진단을 기록한다(경로 앞 두 단계를 복사한다).
class StaticArrayDiagnostics final : public FieldCodecHooks {
public:
    Result<void> Report(const FieldDiagnostic& diagnostic) noexcept override {
        if (count < 8) {
            Entry& entry = entries[count++];
            entry.kind = diagnostic.kind;
            entry.depth = diagnostic.path.Size();
            for (usize step = 0; step < 2 && step < diagnostic.path.Size(); ++step) {
                entry.steps[step] = diagnostic.path[step];
            }
        }

        return Result<void>::Success();
    }

    struct Entry {
        FieldDiagnosticKind kind = FieldDiagnosticKind::UnknownField;
        usize depth = 0;
        PathStep steps[2] = {};
    };

    Entry entries[8];
    usize count = 0;
};

bool SameStaticStep(const PathStep& step, Guid field, uint32 index,
                    PathSelector selector) noexcept {
    return step.fieldId == field && step.index == index && step.selector == selector;
}

} // namespace

// 모듈 집계 등록이 정적 배열 필드의 생성 스키마(value {StaticArray, 0}·원소 형식·maxElements = N·
// ObjectRef 원소면 FieldKind::ObjectReference)와 함수표(read·write·writeStaged 없음, codec, 경로
// thunk, enum·struct 원소 native 키)를 싣고, 참조 struct 원소 struct는 Registry 컨테이너 없는 참조
// struct다.
TEST(StaticArrayGraph_ModuleRegistrationDescribesStaticArrays) {
    SystemAllocator allocator;
    Fixture fixture(allocator);
    auto owner = fixture.types.Acquire(TypeIdOf<FixedOwner>());
    auto link = fixture.types.AcquireValueType(TypeIdOf<FixedLink>(), TypeKind::Struct);
    REQUIRE(owner && link);
    CHECK(link.Value().Struct().ContainsStrongReference());
    CHECK(!link.Value().Struct().ContainsReferenceArray());
    struct Expected {
        const char* name;
        ValueKind element;
        TypeId target;
        uint32 count;
        const void* native;
    };

    const Expected expected[] = {
        {"flags", ValueKind::Bool, {}, 3, nullptr},
        {"tiny", ValueKind::Int8, {}, 2, nullptr},
        {"ints", ValueKind::Int32, {}, 4, nullptr},
        {"names", ValueKind::Name, {}, 2, nullptr},
        {"refs", ValueKind::ObjectReference, TypeIdOf<FixedOwner>(), 3, nullptr},
        {"watchers", ValueKind::WeakObjectReference, TypeIdOf<FixedOwner>(), 2, nullptr},
        {"kinds", ValueKind::ClassReference, TypeIdOf<FixedOwner>(), 2, nullptr},
        {"modes", ValueKind::Enum, GeneratedType<FixedMode>::Id(), 2, NativeTypeKey<FixedMode>()},
        {"links", ValueKind::Struct, TypeIdOf<FixedLink>(), 2, NativeTypeKey<FixedLink>()},
        {"softs", ValueKind::SoftObjectReference, {}, 2, nullptr}};
    for (const Expected& entry : expected) {
        const FieldSchema& field = SchemaField(owner.Value().Schema(), StringView(entry.name));
        CHECK(field.value.kind == ValueKind::StaticArray && field.value.type == TypeId{});
        CHECK(field.element.kind == entry.element && field.element.type == entry.target);
        CHECK(field.maxElements == entry.count);
        CHECK((field.kind == FieldKind::ObjectReference) ==
              (entry.element == ValueKind::ObjectReference));
        const RuntimeFieldOps* found = nullptr;
        for (const RuntimeFieldOps& candidate : owner.Value().Ops().fields) {
            found = candidate.fieldId == field.id ? &candidate : found;
        }

        REQUIRE(found);
        CHECK(found->read == nullptr && !found->write && found->writeStaged == nullptr);
        CHECK(found->encode != nullptr && found->decode != nullptr && found->path != nullptr);
        CHECK(found->valueNativeType == entry.native);
    }

    const FieldSchema& pair = SchemaField(TypeOf<FixedPoint>(), StringView("pair"));
    CHECK(pair.value.kind == ValueKind::StaticArray && pair.element.kind == ValueKind::Int16 &&
          pair.maxElements == 2);
}

// 원소 종류마다(정수 8종·Bool·실수·Guid·String·Name·ObjectRef·WeakObjectRef·ClassRef·enum·값
// struct(정적 배열 멤버 포함)·참조 struct·AssetId·Soft·Digest) 값이 그대로 돌아오고 다른
// Registry에서 다시 저장한 byte가 같다. 생성 Trace가 ObjectRef 원소(refs[1])와 참조 struct
// 원소(links[1].target)를 방문해 그 원소로만 닿는 객체가 수집에서 살고, 자기 참조·객체 사이 순환은
// root를 놓으면 모두 회수된다.
TEST(StaticArrayGraph_RoundTripEveryElementKindTraceAndCycle) {
    SystemAllocator allocator;
    Fixture fixture(allocator);
    ObjectRegistry source(allocator, fixture.types, {0x534147, 1}, WithNames(&fixture.names));
    ObjectRegistry target(allocator, fixture.types, {0x534147, 2}, WithNames(&fixture.names));
    {
        auto root = MakeGraph(source, fixture.names);
        REQUIRE(root);
        REQUIRE(source.Collect(source.OwnerAccess()));
        CHECK(source.Stats().liveObjects == 3);
        auto bytes =
            SaveObjectGraph(allocator, source, source.OwnerAccess(), root.Value().Handle());
        REQUIRE(bytes);
        auto loaded = LoadObjectGraph(allocator, target, fixture.types, target.OwnerAccess(),
                                      bytes.Value().View());
        REQUIRE(loaded);
        const ObjectHandle self = loaded.Value().Handle();
        ObjectHandle first;
        ObjectHandle second;
        With<FixedOwner>(target, self, [&](FixedOwner& value) noexcept {
            CHECK(CheckFilled(target, fixture.names, self, value, first, second));
        });
        With<FixedOwner>(target, first, [&](FixedOwner& value) noexcept {
            CHECK(value.refs[0].Handle() == self && value.refs[1].Handle().IsNull());
        });

        REQUIRE(target.Collect(target.OwnerAccess()));
        CHECK(target.Stats().liveObjects == 3);
        auto resaved = SaveObjectGraph(allocator, target, target.OwnerAccess(), self);
        REQUIRE(resaved);
        CHECK(test::graph::SameBytes(bytes.Value().View(), resaved.Value().View()));
        loaded.Value().Reset();
        root.Value().Reset();
        REQUIRE(target.Collect(target.OwnerAccess()));
        REQUIRE(source.Collect(source.OwnerAccess()));
        CHECK(target.Stats().liveObjects == 0 && source.Stats().liveObjects == 0);
    }

    REQUIRE(source.Shutdown(source.OwnerAccess()));
    REQUIRE(target.Shutdown(target.OwnerAccess()));
}

// 정적 배열의 ObjectRef 원소는 소유 객체 저장소 안 주소라 live SetReference를 그대로 지나고(Trace가
// 방문해 대상이 산다), 저장소 밖 ObjectRef는 InvalidArgument다.
TEST(StaticArrayGraph_SetReferenceAcceptsElementSlots) {
    SystemAllocator allocator;
    Fixture fixture(allocator);
    ObjectRegistry objects(allocator, fixture.types, {0x534153, 1}, WithNames(&fixture.names));
    {
        RootRef<FixedOwner> holder = New<FixedOwner>(objects);
        ObjectHandle other;
        {
            RootRef<FixedOwner> created = New<FixedOwner>(objects);
            other = created.Handle();
            With<FixedOwner>(objects, holder.Handle(), [&](FixedOwner& value) noexcept {
                CHECK(
                    objects
                        .SetReference(objects.OwnerAccess(), holder.Handle(), value.refs[2], other)
                        .IsOk());
                CHECK(objects
                          .SetReference(objects.OwnerAccess(), holder.Handle(),
                                        value.links[0].target, other)
                          .IsOk());
                ObjectRef<FixedOwner> outside;
                CHECK(ErrorOf(objects.SetReference(objects.OwnerAccess(), holder.Handle(), outside,
                                                   other)) == ErrorCode::InvalidArgument);
            });
        }

        REQUIRE(objects.Collect(objects.OwnerAccess()));
        CHECK(objects.Stats().liveObjects == 2);
        With<FixedOwner>(objects, holder.Handle(), [&](FixedOwner& value) noexcept {
            CHECK(value.refs[2].Handle() == other && value.links[0].target.Handle() == other);
            CHECK(objects.SetReference(objects.OwnerAccess(), holder.Handle(), value.refs[2], {})
                      .IsOk());
            CHECK(
                objects
                    .SetReference(objects.OwnerAccess(), holder.Handle(), value.links[0].target, {})
                    .IsOk());
        });

        REQUIRE(objects.Collect(objects.OwnerAccess()));
        CHECK(objects.Stats().liveObjects == 1);
    }

    REQUIRE(objects.Collect(objects.OwnerAccess()));
    REQUIRE(objects.Shutdown(objects.OwnerAccess()));
}

// 저장된 원소 수가 N이 아니면 InvalidArgument다(단일 값 ValueArray·참조 ValueArray·struct
// RecordList — 짧아도 길어도). 정확히 N이면 복원되고 원소 값이 그대로다.
TEST(StaticArrayGraph_CountMismatchIsInvalidArgument) {
    SystemAllocator allocator;
    Fixture fixture(allocator);
    ObjectRegistry objects(allocator, fixture.types, {0x53414e, 1}, WithNames(&fixture.names));
    const FieldSchema& ints = SchemaField(TypeOf<FixedOwner>(), StringView("ints"));
    const FieldSchema& refs = SchemaField(TypeOf<FixedOwner>(), StringView("refs"));
    const FieldSchema& points = SchemaField(TypeOf<FixedOwner>(), StringView("points"));
    struct Case {
        Guid field;
        WireType wire;
        uint32 count;
        ErrorCode expected;
    };

    const Case cases[] = {{ints.id, WireType::I64, 3, ErrorCode::InvalidArgument},
                          {ints.id, WireType::I64, 5, ErrorCode::InvalidArgument},
                          {ints.id, WireType::I64, 0, ErrorCode::InvalidArgument},
                          {refs.id, WireType::U32, 2, ErrorCode::InvalidArgument},
                          {points.id, WireType::RecordList, 1, ErrorCode::InvalidArgument},
                          {points.id, WireType::RecordList, 3, ErrorCode::InvalidArgument},
                          {ints.id, WireType::I64, 4, ErrorCode::None},
                          {refs.id, WireType::U32, 3, ErrorCode::None},
                          {points.id, WireType::RecordList, 2, ErrorCode::None}};
    for (const Case& entry : cases) {
        Array<uint8> payload = entry.wire == WireType::RecordList
                                   ? PointListPayload(allocator, entry.count)
                                   : ValueArrayPayload(allocator, entry.wire, entry.count);
        const WireType fieldWire =
            entry.wire == WireType::RecordList ? WireType::RecordList : WireType::ValueArray;
        const FieldBytes field{entry.field, static_cast<uint32>(fieldWire), payload.View()};
        Array<uint8> graph = OwnerGraph(allocator, Span<const FieldBytes>(&field, 1));
        CHECK(LoadCode(allocator, objects, fixture.types, graph.View()) == entry.expected);
    }

    Array<uint8> payload = ValueArrayPayload(allocator, WireType::I64, 4);
    const FieldBytes exact{ints.id, static_cast<uint32>(WireType::ValueArray), payload.View()};
    Array<uint8> graph = OwnerGraph(allocator, Span<const FieldBytes>(&exact, 1));
    auto loaded =
        LoadObjectGraph(allocator, objects, fixture.types, objects.OwnerAccess(), graph.View());
    REQUIRE(loaded);
    With<FixedOwner>(objects, loaded.Value().Handle(), [](FixedOwner& value) noexcept {
        CHECK(value.ints[0] == 0 && value.ints[1] == 1 && value.ints[2] == 2 && value.ints[3] == 3);
        CHECK(value.modes[0] == FixedMode::First);
    });

    loaded.Value().Reset();
    REQUIRE(objects.Collect(objects.OwnerAccess()));
    REQUIRE(objects.Shutdown(objects.OwnerAccess()));
}

// 저장 정책 rejectMissingRequired면 Required 원소(ObjectRef·ClassRef)와 struct 원소 안 Required
// 멤버의 null이 RequiredMissing 진단(경로 [f[i]]·[f[i].m]) 뒤 InvalidArgument이고, 모두 채우면
// 저장된다.
TEST(StaticArrayGraph_RequiredElementsMustBeNonNull) {
    SystemAllocator allocator;
    Fixture fixture(allocator);
    ObjectRegistry objects(allocator, fixture.types, {0x534152, 1}, WithNames(&fixture.names));
    const Guid anchors = SchemaField(TypeOf<FixedRequired>(), StringView("anchors")).id;
    const Guid needs = SchemaField(TypeOf<FixedRequired>(), StringView("needs")).id;
    const Guid kind = SchemaField(TypeOf<FixedNeed>(), StringView("kind")).id;
    {
        RootRef<FixedRequired> holder = New<FixedRequired>(objects);
        RootRef<FixedOwner> target = New<FixedOwner>(objects);
        StaticArrayDiagnostics recorder;
        const auto save = [&]() noexcept {
            recorder.count = 0;
            ObjectGraphSaveOptions strict;
            strict.policy.rejectMissingRequired = true;
            strict.hooks = &recorder;
            auto bytes =
                SaveObjectGraph(allocator, objects, objects.OwnerAccess(), holder.Handle(), strict);
            return bytes ? ErrorCode::None : bytes.Error();
        };

        // anchors[1]만 null이다.
        With<FixedRequired>(objects, holder.Handle(), [&](FixedRequired& value) noexcept {
            value.kinds[0] = ClassRef<FixedOwner>(TypeIdOf<FixedOwner>());
            value.needs[0].kind = ClassRef<FixedOwner>(TypeIdOf<FixedOwner>());
            value.needs[1].kind = ClassRef<FixedOwner>(TypeIdOf<FixedOwner>());
            CHECK(objects
                      .SetReference(objects.OwnerAccess(), holder.Handle(), value.anchors[0],
                                    target.Handle())
                      .IsOk());
        });

        CHECK(save() == ErrorCode::InvalidArgument);
        REQUIRE(recorder.count == 1);
        CHECK(recorder.entries[0].kind == FieldDiagnosticKind::RequiredMissing);
        CHECK(recorder.entries[0].depth == 1);
        CHECK(SameStaticStep(recorder.entries[0].steps[0], anchors, 1, PathSelector::Index));

        // anchors를 채우고 needs[1].kind를 비운다.
        With<FixedRequired>(objects, holder.Handle(), [&](FixedRequired& value) noexcept {
            value.needs[1].kind = ClassRef<FixedOwner>();
            CHECK(objects
                      .SetReference(objects.OwnerAccess(), holder.Handle(), value.anchors[1],
                                    target.Handle())
                      .IsOk());
        });

        CHECK(save() == ErrorCode::InvalidArgument);
        REQUIRE(recorder.count == 1);
        CHECK(recorder.entries[0].kind == FieldDiagnosticKind::RequiredMissing);
        CHECK(recorder.entries[0].depth == 2);
        CHECK(SameStaticStep(recorder.entries[0].steps[0], needs, 1, PathSelector::Index));
        CHECK(SameStaticStep(recorder.entries[0].steps[1], kind, 0, PathSelector::None));

        With<FixedRequired>(objects, holder.Handle(), [&](FixedRequired& value) noexcept {
            value.needs[1].kind = ClassRef<FixedOwner>(TypeIdOf<FixedOwner>());
        });

        CHECK(save() == ErrorCode::None);
        CHECK(recorder.count == 0);
        With<FixedRequired>(objects, holder.Handle(), [&](FixedRequired& value) noexcept {
            value.kinds[0] = ClassRef<FixedOwner>();
        });

        CHECK(save() == ErrorCode::InvalidArgument);
        REQUIRE(recorder.count == 1);
        CHECK(SameStaticStep(recorder.entries[0].steps[0],
                             SchemaField(TypeOf<FixedRequired>(), StringView("kinds")).id, 0,
                             PathSelector::Index));
    }

    REQUIRE(objects.Collect(objects.OwnerAccess()));
    REQUIRE(objects.Shutdown(objects.OwnerAccess()));
}

// 로드의 Required 실효 값 검사(CheckRequiredFields)는 Required 원소의 null을 RequiredMissing
// [f[i]]로, struct 원소 안 Required 멤버의 null을 [f[i].m]으로 보고한다(로드는 거부하지 않는다 —
// hooks가 있을 때만 보고).
TEST(StaticArrayGraph_LoadReportsMissingRequiredElements) {
    SystemAllocator allocator;
    Fixture fixture(allocator);
    ObjectRegistry objects(allocator, fixture.types, {0x53414c, 1}, WithNames(&fixture.names));
    const Guid anchors = SchemaField(TypeOf<FixedRequired>(), StringView("anchors")).id;
    const Guid needs = SchemaField(TypeOf<FixedRequired>(), StringView("needs")).id;
    const Guid kind = SchemaField(TypeOf<FixedNeed>(), StringView("kind")).id;
    {
        RootRef<FixedRequired> holder = New<FixedRequired>(objects);
        RootRef<FixedOwner> target = New<FixedOwner>(objects);
        With<FixedRequired>(objects, holder.Handle(), [&](FixedRequired& value) noexcept {
            value.kinds[0] = ClassRef<FixedOwner>(TypeIdOf<FixedOwner>());
            value.needs[0].kind = ClassRef<FixedOwner>(TypeIdOf<FixedOwner>());
            CHECK(objects
                      .SetReference(objects.OwnerAccess(), holder.Handle(), value.anchors[0],
                                    target.Handle())
                      .IsOk());
        });

        auto bytes = SaveObjectGraph(allocator, objects, objects.OwnerAccess(), holder.Handle());
        REQUIRE(bytes);
        StaticArrayDiagnostics recorder;
        ObjectGraphLoadOptions options;
        options.hooks = &recorder;
        auto loaded = LoadObjectGraph(allocator, objects, fixture.types, objects.OwnerAccess(),
                                      bytes.Value().View(), options);
        REQUIRE(loaded);
        REQUIRE(recorder.count == 2);
        bool anchorReported = false;
        bool needReported = false;
        for (usize index = 0; index < recorder.count; ++index) {
            const StaticArrayDiagnostics::Entry& entry = recorder.entries[index];
            CHECK(entry.kind == FieldDiagnosticKind::RequiredMissing);
            anchorReported =
                anchorReported || (entry.depth == 1 &&
                                   SameStaticStep(entry.steps[0], anchors, 1, PathSelector::Index));
            needReported =
                needReported || (entry.depth == 2 &&
                                 SameStaticStep(entry.steps[0], needs, 1, PathSelector::Index) &&
                                 SameStaticStep(entry.steps[1], kind, 0, PathSelector::None));
        }

        CHECK(anchorReported && needReported);
        loaded.Value().Reset();
    }

    REQUIRE(objects.Collect(objects.OwnerAccess()));
    REQUIRE(objects.Shutdown(objects.OwnerAccess()));
}

// 정적 배열의 2단계 몫은 원소 메모리를 세지 않는다(원소가 소유 저장소 안이라 할당하지 않는다):
// wire(U32)· 원소 수(256)·payload가 같고 원소 크기만 다른 uint8[256]·uint32[256] 필드는 1단계가
// 성공하는 최소 작업량의 차이가 원소 수보다 작다(필드 위치에 따른 작은 차이뿐 — 배열의 N ×
// sizeof(원소) 몫이면 256 × 3 = 768만큼 벌어진다).
TEST(StaticArrayGraph_ApplyChargeIgnoresElementSize) {
    SystemAllocator allocator;
    Fixture fixture(allocator);
    ObjectRegistry objects(allocator, fixture.types, {0x534157, 1}, WithNames(&fixture.names));
    const auto minimum = [&](StringView name) noexcept {
        Array<uint8> payload = ValueArrayPayload(allocator, WireType::U32, 256);
        const FieldBytes field{SchemaField(TypeOf<FixedOwner>(), name).id,
                               static_cast<uint32>(WireType::ValueArray), payload.View()};
        Array<uint8> graph = OwnerGraph(allocator, Span<const FieldBytes>(&field, 1));
        return static_cast<int64>(
            MinimumValidateWork(allocator, objects, fixture.types, graph.View()));
    };

    const int64 octets = minimum(StringView("octets"));
    const int64 quads = minimum(StringView("quads"));
    const int64 difference = quads > octets ? quads - octets : octets - quads;
    CHECK(octets > 256 && quads > 256);
    CHECK(difference < 256);
    REQUIRE(objects.Shutdown(objects.OwnerAccess()));
}
