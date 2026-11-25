#include <ReflectionTestFixtures/ClassDefaultsFixture.generated.h>
#include <ReflectionTestFixtures/ReflectionTestFixtures.module.generated.h>
#include <Core/Memory/SystemAllocator.h>
#include <Object/ModuleRegistration.h>
#include <Object/OverrideSet.h>
#include <ObjectSerialization/ClassDefaults.h>
#include <ObjectSerialization/ObjectGraph.h>
#include <Platform/Fatal.h>
#include <Serialization/Record.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>
#include "GraphTestSupport.h"

// 차이 쓰기·override 집합 시험이다(Docs/Design/RuntimeClassMetadata.ko.md '클래스 기본값 구현 명세
// — A5b', fixture ReflectionTestFixtures/ClassDefaultsFixture.h): override 단위 정규화(원소·key
// 자르기· Atomic·비Atomic struct 펼침), 집합 정렬·지우기·복사, 차이 저장의 생략(필드·비Atomic
// struct 멤버· 모든 멤버 같음 → struct 생략·3단계 struct), 통째 비교(Atomic·배열·정적
// 배열·Map·Optional·bitfield), override 단위는 기준과 같아도 씀, Include weak 지연 항목이 걸린
// 필드는 생략하지 않음, 정책 위반, 레코드 없는 타입은 전부.
using namespace rocks;
namespace diff_save_tests_file {

using reflection_fixture::DefaultsCounter;
using reflection_fixture::DefaultsHolder;
using reflection_fixture::DefaultsNest;
using reflection_fixture::DefaultsPair;
using reflection_fixture::DefaultsPoint;

namespace {

struct DiffFixture {
    explicit DiffFixture(Allocator& allocator) noexcept
        : types(allocator), registration(types, allocator) {
        REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(registration));
    }

    ~DiffFixture() noexcept {
        CHECK(registration.Unregister());
        CHECK(types.Shutdown());
    }

    TypeRegistry types;
    ModuleRegistration registration;
};

template <class T>
Guid FieldNamed(StringView name) noexcept {
    for (const FieldSchema& field : TypeOf<T>().fields) {
        if (StringView(field.name.Data(), field.name.Size()) == name) {
            return field.id;
        }
    }

    platform::Fatal("fixture field missing");
}

// 노드 하나짜리 그래프(NodeTable 없음)의 노드 Record다.
Span<const uint8> NodeRecord(Span<const uint8> graph) noexcept {
    constexpr usize LengthBytes = 8;
    uint64 length = 0;
    for (usize index = 0; index < LengthBytes; ++index) {
        length |= static_cast<uint64>(graph[test::graph::HeaderBytes + index]) << (8 * index);
    }

    return graph.Slice(test::graph::HeaderBytes + LengthBytes, static_cast<usize>(length));
}

// Record의 필드 ID 목록이다(저장 순서).
Array<Guid> FieldsOf(Allocator& allocator, Span<const uint8> record, Guid schema,
                     uint32 version) noexcept {
    Array<Guid> ids(allocator);
    auto reader = RecordReader::Open(allocator, record, schema, version, version);
    if (!reader) {
        platform::Fatal("test record open failed");
    }

    for (;;) {
        auto field = reader.Value().Next();
        if (!field) {
            break;
        }

        if (!ids.TryPushBack(field.Value().id)) {
            platform::Fatal("test push failed");
        }
    }

    return ids;
}

bool Has(const Array<Guid>& ids, Guid id) noexcept {
    for (Guid one : ids) {
        if (one == id) {
            return true;
        }
    }

    return false;
}

// field(struct 값)의 멤버 ID 목록이다.
Array<Guid> MembersOf(Allocator& allocator, Span<const uint8> record, Guid schema, uint32 version,
                      Guid field, Guid structId, uint32 structVersion) noexcept {
    auto reader = RecordReader::Open(allocator, record, schema, version, version);
    if (!reader) {
        platform::Fatal("test record open failed");
    }

    auto found = reader.Value().Find(field);
    if (!found) {
        platform::Fatal("test struct field missing");
    }

    auto nested = reader.Value().OpenRecordField(allocator, found.Value(), structId, structVersion,
                                                 structVersion, 64);
    if (!nested) {
        platform::Fatal("test nested open failed");
    }

    Array<Guid> ids(allocator);
    for (;;) {
        auto member = nested.Value().Next();
        if (!member) {
            break;
        }

        if (!ids.TryPushBack(member.Value().id)) {
            platform::Fatal("test push failed");
        }
    }

    return ids;
}

// 시험 override 조회다(객체 하나).
class OneOverrides final : public GraphOverrides {
public:
    OneOverrides(ObjectHandle object, const OverrideSet& set) noexcept
        : object_(object), set_(set) {
    }

    const OverrideSet* Find(ObjectHandle object) const noexcept override {
        return object == object_ ? &set_ : nullptr;
    }

private:
    ObjectHandle object_;
    const OverrideSet& set_;
};

// 객체 하나를 차이 저장한 노드 Record 필드 목록이다(edit가 값을 바꾼다).
template <class Edit>
Array<Guid> DiffFields(Allocator& allocator, TypeRegistry& types, Edit&& edit,
                       const OverrideSet* overrides = nullptr) noexcept {
    ObjectRegistry objects(allocator, types, {0x4446, 1});
    Array<Guid> fields(allocator);
    {
        auto created =
            objects.NewObject<DefaultsHolder>(objects.OwnerAccess(), TypeIdOf<DefaultsHolder>());
        if (!created) {
            platform::Fatal("diff test object creation failed");
        }

        {
            auto borrowed =
                objects.Resolve<DefaultsHolder>(objects.OwnerAccess(), created.Value().Handle());
            if (!borrowed) {
                platform::Fatal("diff test object resolve failed");
            }

            auto value = borrowed.Value().Get();
            if (!value) {
                platform::Fatal("diff test object borrow failed");
            }

            edit(*value.Value());
        }

        ObjectGraphSaveOptions options;
        options.baseline = GraphBaseline::Editor;
        const OverrideSet empty(allocator);
        OneOverrides lookup(created.Value().Handle(), overrides != nullptr ? *overrides : empty);
        options.overrides = overrides != nullptr ? &lookup : nullptr;
        auto saved = SaveObjectGraph(allocator, objects, objects.OwnerAccess(),
                                     created.Value().Handle(), options);
        if (!saved) {
            platform::Fatal("diff test graph save failed");
        }

        const Array<uint8> bytes = Move(saved.Value());
        const TypeSchema& schema = TypeOf<DefaultsHolder>();
        fields = FieldsOf(allocator, NodeRecord(bytes.View()), schema.id, schema.version);
        created.Value().Reset();
    }

    CHECK(objects.Shutdown(objects.OwnerAccess()));
    return fields;
}

PathStep Step(Guid field, PathSelector selector = PathSelector::None, uint32 index = 0) noexcept {
    PathStep step;
    step.fieldId = field;
    step.selector = selector;
    step.index = index;
    return step;
}

} // namespace

// override 단위 정규화: 단일 값은 그대로, 원소·key 단계는 그 필드, Atomic struct 멤버는 struct
// 경로, 비Atomic struct 끝은 잎 멤버(Transient 제외)로 펼침, 두 단계 멤버는 잎까지. 없는 필드
// NotFound, struct가 아닌 값 뒤 단계 TypeMismatch, 빈 경로 InvalidArgument. present 판정은 비Atomic
// struct 자체를 버리고 모르는 필드는 무시한다.
TEST(OverrideSet_UnitNormalization) {
    SystemAllocator allocator;
    DiffFixture fixture(allocator);
    auto lease = fixture.types.Acquire(TypeIdOf<DefaultsHolder>());
    REQUIRE(lease);
    const Guid count = FieldNamed<DefaultsHolder>("count");
    const Guid values = FieldNamed<DefaultsHolder>("values");
    const Guid point = FieldNamed<DefaultsHolder>("point");
    const Guid pair = FieldNamed<DefaultsHolder>("pair");
    const Guid nest = FieldNamed<DefaultsHolder>("nest");
    const Guid x = FieldNamed<DefaultsPoint>("x");
    const Guid y = FieldNamed<DefaultsPoint>("y");
    const Guid note = FieldNamed<DefaultsPoint>("note");
    const Guid u = FieldNamed<DefaultsPair>("u");
    const Guid inner = FieldNamed<DefaultsNest>("inner");
    const Guid depth = FieldNamed<DefaultsNest>("depth");

    OverrideSet set(allocator);
    const PathStep single[] = {Step(count)};
    REQUIRE(AddOverrideUnits(lease.Value(), single, set));
    const PathStep element[] = {Step(values, PathSelector::Index, 2)};
    REQUIRE(AddOverrideUnits(lease.Value(), element, set));
    const PathStep atomicMember[] = {Step(pair), Step(u)};
    REQUIRE(AddOverrideUnits(lease.Value(), atomicMember, set));
    const PathStep wholeStruct[] = {Step(point)};
    REQUIRE(AddOverrideUnits(lease.Value(), wholeStruct, set));
    const PathStep deep[] = {Step(nest), Step(inner), Step(x)};
    REQUIRE(AddOverrideUnits(lease.Value(), deep, set));
    const Guid countUnit[] = {count};
    const Guid valuesUnit[] = {values};
    const Guid pairUnit[] = {pair};
    const Guid pointX[] = {point, x};
    const Guid pointY[] = {point, y};
    const Guid pointNote[] = {point, note};
    const Guid nestX[] = {nest, inner, x};
    const Guid nestDepth[] = {nest, depth};
    CHECK(set.Contains(Span<const Guid>(countUnit)) && set.Contains(Span<const Guid>(valuesUnit)) &&
          set.Contains(Span<const Guid>(pairUnit)) && set.Contains(Span<const Guid>(pointX)) &&
          set.Contains(Span<const Guid>(pointY)) && set.Contains(Span<const Guid>(pointNote)) &&
          set.Contains(Span<const Guid>(nestX)) && !set.Contains(Span<const Guid>(nestDepth)));
    CHECK(set.Count() == 7);
    // 정렬 순(사전 순)이다.
    for (usize index = 1; index < set.Count(); ++index) {
        const Span<const Guid> left = set.At(index - 1);
        const Span<const Guid> right = set.At(index);
        CHECK(GuidLess(left[0], right[0]) ||
              (left[0] == right[0] && (left.Size() < right.Size() || GuidLess(left[1], right[1]))));
    }

    // 같은 단위를 다시 더해도 하나다.
    REQUIRE(AddOverrideUnits(lease.Value(), single, set));
    CHECK(set.Count() == 7);
    // 지우기: 비Atomic struct 끝은 잎 모두, RemoveUnder는 접두 아래 모두.
    REQUIRE(RemoveOverrideUnits(lease.Value(), wholeStruct, set));
    CHECK(!set.Contains(Span<const Guid>(pointX)) && set.Count() == 4);
    const Guid nestPrefix[] = {nest};
    set.RemoveUnder(Span<const Guid>(nestPrefix));
    CHECK(!set.Contains(Span<const Guid>(nestX)) && set.Count() == 3);
    OverrideSet copy(allocator);
    REQUIRE(copy.TryAssign(set));
    CHECK(copy.Count() == 3 && copy.Contains(Span<const Guid>(pairUnit)));

    // 거부.
    const PathStep unknown[] = {Step(Guid{0xDEAD, 0xBEEF})};
    CHECK(AddOverrideUnits(lease.Value(), unknown, set).Error() == ErrorCode::NotFound);
    // 단일 struct가 아닌 칸 뒤 단계(InstancedStruct 내용·Optional 안과 같은 모양)는 그 필드에서
    // 자른다.
    const usize before = set.Count();
    const PathStep afterScalar[] = {Step(count), Step(x)};
    REQUIRE(AddOverrideUnits(lease.Value(), afterScalar, set));
    CHECK(set.Count() == before && set.Contains(Span<const Guid>(countUnit)));
    CHECK(AddOverrideUnits(lease.Value(), {}, set).Error() == ErrorCode::InvalidArgument);

    // present: 비Atomic struct 자체는 버리고, 잎은 담고, 모르는 필드는 무시한다.
    OverrideSet present(allocator);
    const Guid pointOnly[] = {point};
    REQUIRE(AddPresentUnit(lease.Value(), Span<const Guid>(pointOnly), present));
    CHECK(present.IsEmpty());
    REQUIRE(AddPresentUnit(lease.Value(), Span<const Guid>(pointX), present));
    const Guid pairMember[] = {pair, u};
    REQUIRE(AddPresentUnit(lease.Value(), Span<const Guid>(pairMember), present));
    const Guid stranger[] = {Guid{0xDEAD, 0xBEEF}};
    REQUIRE(AddPresentUnit(lease.Value(), Span<const Guid>(stranger), present));
    CHECK(present.Count() == 2 && present.Contains(Span<const Guid>(pointX)) &&
          present.Contains(Span<const Guid>(pairUnit)));
}

// 차이 저장: 생성자 값 그대로면 Transient 말고 모두 기준과 같아 필드가 없다. 바꾼 단일 값만,
// 비Atomic struct는 바꾼 멤버만(3단계 struct는 잎까지 내려간다), Atomic·배열·정적
// 배열·Map·Optional·bitfield는 통째로 쓴다.
TEST(DiffSave_OmitsBaselineValues) {
    SystemAllocator allocator;
    DiffFixture fixture(allocator);
    const TypeSchema& schema = TypeOf<DefaultsHolder>();
    CHECK(DiffFields(allocator, fixture.types, [](DefaultsHolder&) noexcept {}).IsEmpty());

    auto changed = DiffFields(allocator, fixture.types,
                              [](DefaultsHolder& value) noexcept { value.count = 7; });
    CHECK(changed.Size() == 1 && changed[0] == FieldNamed<DefaultsHolder>("count"));

    // 비Atomic struct: 바꾼 멤버 하나만.
    {
        ObjectRegistry objects(allocator, fixture.types, {0x4446, 2});
        auto created =
            objects.NewObject<DefaultsHolder>(objects.OwnerAccess(), TypeIdOf<DefaultsHolder>());
        REQUIRE(created);
        {
            auto borrowed =
                objects.Resolve<DefaultsHolder>(objects.OwnerAccess(), created.Value().Handle());
            REQUIRE(borrowed);
            auto value = borrowed.Value().Get();
            REQUIRE(value);
            value.Value()->point.y = 9.0f;
            value.Value()->nest.inner.x = 11;
        }

        ObjectGraphSaveOptions options;
        options.baseline = GraphBaseline::Editor;
        auto saved = SaveObjectGraph(allocator, objects, objects.OwnerAccess(),
                                     created.Value().Handle(), options);
        REQUIRE(saved);
        const Array<uint8> bytes = Move(saved.Value());
        const Span<const uint8> record = NodeRecord(bytes.View());
        const Array<Guid> fields = FieldsOf(allocator, record, schema.id, schema.version);
        CHECK(fields.Size() == 2);
        const TypeSchema& point = TypeOf<DefaultsPoint>();
        const Array<Guid> members =
            MembersOf(allocator, record, schema.id, schema.version,
                      FieldNamed<DefaultsHolder>("point"), point.id, point.version);
        CHECK(members.Size() == 1 && members[0] == FieldNamed<DefaultsPoint>("y"));
        const TypeSchema& nest = TypeOf<DefaultsNest>();
        const Array<Guid> nestMembers =
            MembersOf(allocator, record, schema.id, schema.version,
                      FieldNamed<DefaultsHolder>("nest"), nest.id, nest.version);
        CHECK(nestMembers.Size() == 1 && nestMembers[0] == FieldNamed<DefaultsNest>("inner"));
        created.Value().Reset();
        CHECK(objects.Shutdown(objects.OwnerAccess()));
    }

    // 통째 비교.
    auto atomic = DiffFields(allocator, fixture.types,
                             [](DefaultsHolder& value) noexcept { value.pair.u = 70; });
    CHECK(atomic.Size() == 1 && atomic[0] == FieldNamed<DefaultsHolder>("pair"));
    auto fixed = DiffFields(allocator, fixture.types,
                            [](DefaultsHolder& value) noexcept { value.fixed[1] = 20; });
    CHECK(fixed.Size() == 1 && fixed[0] == FieldNamed<DefaultsHolder>("fixed"));
    auto array = DiffFields(allocator, fixture.types, [](DefaultsHolder& value) noexcept {
        CHECK(value.values.TryPushBack(5));
    });
    CHECK(array.Size() == 1 && array[0] == FieldNamed<DefaultsHolder>("values"));
    auto map = DiffFields(allocator, fixture.types,
                          [](DefaultsHolder& value) noexcept { CHECK(value.table.Insert(1, 2)); });
    CHECK(map.Size() == 1 && map[0] == FieldNamed<DefaultsHolder>("table"));
    auto optional = DiffFields(allocator, fixture.types,
                               [](DefaultsHolder& value) noexcept { value.maybe.Reset(); });
    CHECK(optional.Size() == 1 && optional[0] == FieldNamed<DefaultsHolder>("maybe"));
    auto bits =
        DiffFields(allocator, fixture.types, [](DefaultsHolder& value) noexcept { value.dim = 1; });
    CHECK(bits.Size() == 1 && bits[0] == FieldNamed<DefaultsHolder>("dim"));
    // -0.0 기본값에 +0.0은 byte가 달라 쓴다(같음 = wire byte).
    auto zero = DiffFields(allocator, fixture.types,
                           [](DefaultsHolder& value) noexcept { value.precise = 0.0; });
    CHECK(zero.Size() == 1 && zero[0] == FieldNamed<DefaultsHolder>("precise"));
}

// override 단위는 기준과 같아도 쓴다: 단일 값(count), 비Atomic struct 잎 멤버 하나(point.x — struct
// Record에 그 멤버만), Atomic struct(pair), 배열(values).
TEST(DiffSave_WritesOverrideUnits) {
    SystemAllocator allocator;
    DiffFixture fixture(allocator);
    auto lease = fixture.types.Acquire(TypeIdOf<DefaultsHolder>());
    REQUIRE(lease);
    OverrideSet set(allocator);
    const PathStep count[] = {Step(FieldNamed<DefaultsHolder>("count"))};
    const PathStep pointX[] = {Step(FieldNamed<DefaultsHolder>("point")),
                               Step(FieldNamed<DefaultsPoint>("x"))};
    const PathStep pair[] = {Step(FieldNamed<DefaultsHolder>("pair"))};
    const PathStep values[] = {Step(FieldNamed<DefaultsHolder>("values"))};
    REQUIRE(AddOverrideUnits(lease.Value(), count, set));
    REQUIRE(AddOverrideUnits(lease.Value(), pointX, set));
    REQUIRE(AddOverrideUnits(lease.Value(), pair, set));
    REQUIRE(AddOverrideUnits(lease.Value(), values, set));
    auto fields = DiffFields(allocator, fixture.types, [](DefaultsHolder&) noexcept {}, &set);
    CHECK(fields.Size() == 4 && Has(fields, FieldNamed<DefaultsHolder>("count")) &&
          Has(fields, FieldNamed<DefaultsHolder>("point")) &&
          Has(fields, FieldNamed<DefaultsHolder>("pair")) &&
          Has(fields, FieldNamed<DefaultsHolder>("values")));
}

// Include 정책의 집합 밖 weak 대상은 자리값 0(null과 같은 byte)을 쓰고 끝에 덮어쓴다 — 그 필드는
// 생략하지 않는다(위치·지연 항목 판정, A5b 교차 검증 필수 1). 정책 위반 셋은 InvalidArgument다.
// 레코드가 없는 타입(비결정 생성자)은 저장 실패다.
TEST(DiffSave_WeakDeferralAndPolicy) {
    SystemAllocator allocator;
    DiffFixture fixture(allocator);
    ObjectRegistry objects(allocator, fixture.types, {0x4446, 3});
    {
        auto holder =
            objects.NewObject<DefaultsHolder>(objects.OwnerAccess(), TypeIdOf<DefaultsHolder>());
        auto other =
            objects.NewObject<DefaultsHolder>(objects.OwnerAccess(), TypeIdOf<DefaultsHolder>());
        REQUIRE(holder && other);
        {
            auto borrowed =
                objects.Resolve<DefaultsHolder>(objects.OwnerAccess(), holder.Value().Handle());
            REQUIRE(borrowed);
            auto value = borrowed.Value().Get();
            REQUIRE(value);
            value.Value()->weak = WeakObjectRef<Object>(other.Value().Handle());
        }

        ObjectGraphSaveOptions options;
        options.baseline = GraphBaseline::Editor;
        options.externals = GraphExternalPolicy::Include;
        auto saved = SaveObjectGraph(allocator, objects, objects.OwnerAccess(),
                                     holder.Value().Handle(), options);
        REQUIRE(saved);
        const Array<uint8> bytes = Move(saved.Value());
        const TypeSchema& schema = TypeOf<DefaultsHolder>();
        const Array<Guid> fields =
            FieldsOf(allocator, NodeRecord(bytes.View()), schema.id, schema.version);
        CHECK(Has(fields, FieldNamed<DefaultsHolder>("weak")));

        // 정책 위반.
        const auto refuses = [&](GraphBaseline baseline, uint32 excluded, uint32 include) noexcept {
            ObjectGraphSaveOptions bad;
            bad.baseline = baseline;
            bad.policy.excludedFlags = excluded;
            bad.policy.includeFlags = include;
            return SaveObjectGraph(allocator, objects, objects.OwnerAccess(),
                                   holder.Value().Handle(), bad)
                       .Error() == ErrorCode::InvalidArgument;
        };
        constexpr uint32 EditorOnly = static_cast<uint32>(FieldFlags::EditorOnly);
        CHECK(refuses(GraphBaseline::Editor, EditorOnly, 0));
        CHECK(refuses(GraphBaseline::Cooked, 0, 0));
        CHECK(refuses(GraphBaseline::Editor, 0, static_cast<uint32>(FieldFlags::SaveGame)));

        // 레코드 실패(비결정 생성자)는 저장 실패다(전부 저장으로 대체하지 않는다).
        auto counter =
            objects.NewObject<DefaultsCounter>(objects.OwnerAccess(), TypeIdOf<DefaultsCounter>());
        REQUIRE(counter);
        ObjectGraphSaveOptions diff;
        diff.baseline = GraphBaseline::Editor;
        CHECK(SaveObjectGraph(allocator, objects, objects.OwnerAccess(), counter.Value().Handle(),
                              diff)
                  .Error() == ErrorCode::InvalidArgument);
        counter.Value().Reset();
        holder.Value().Reset();
        other.Value().Reset();
    }

    CHECK(objects.Shutdown(objects.OwnerAccess()));
}

} // namespace diff_save_tests_file
