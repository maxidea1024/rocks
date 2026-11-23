#include <ReflectionTestFixtures/ClassRefFixture.generated.h>
#include <ReflectionTestFixtures/MapFixture.generated.h>
#include <ReflectionTestFixtures/OptionalFixture.generated.h>
#include <ReflectionTestFixtures/StructFixture.generated.h>
#include <ReflectionTestFixtures/TextFieldFixture.generated.h>
#include <ReflectionTestFixtures/WeakObjectRefFixture.generated.h>
#include <Core/Text/NamePool.h>
#include <ObjectSerialization/StructRecord.h>
#include "InstancedStructTestSupport.h"
#include "NameGraphTestSupport.h"

// 독립 struct Record 시험이다(Docs/Design/ReflectionExtensions.ko.md 26절): 멤버 종류별 왕복, 객체
// 그래프 InstancedStruct 필드 내용과 바이트 일치, 담기 조건·weak 참조, 마이그레이션·newer 정책,
// 손상· 다른 schema, 없는 멤버는 기본값, 한도, 실패 때 값 불변(할당 실패 주입 포함), 타입 열기.
using namespace rocks;
namespace struct_record_tests_file {

using namespace rocks::test::instanced;
using reflection_fixture::ClassBase;
using reflection_fixture::ClassDerived;
using reflection_fixture::ClassSlot;
using reflection_fixture::MapBag;
using reflection_fixture::MapPoint;
using reflection_fixture::OptionalPoint;
using reflection_fixture::OptionalSlot;
using reflection_fixture::OptionalTag;
using reflection_fixture::StructLeaf;
using reflection_fixture::StructMiddle;
using reflection_fixture::TextFieldCaption;
using reflection_fixture::TextureGroup;
using reflection_fixture::WeakLink;
using reflection_fixture::WeakTarget;

// 이 파일이 쓰는 타입을 등록한다(InstancedStruct fixture 포함).
bool RegisterAll(TypeRegistry& types) noexcept {
    return RegisterFixture(types) && GeneratedType<ClassBase>::Register(types).IsOk() &&
           GeneratedType<ClassDerived>::Register(types).IsOk() &&
           GeneratedType<ClassSlot>::Register(types).IsOk() &&
           GeneratedType<MapPoint>::Register(types).IsOk() &&
           GeneratedType<MapBag>::Register(types).IsOk() &&
           GeneratedType<OptionalPoint>::Register(types).IsOk() &&
           GeneratedType<OptionalSlot>::Register(types).IsOk() &&
           GeneratedType<OptionalTag>::Register(types).IsOk() &&
           GeneratedType<TextFieldCaption>::Register(types).IsOk() &&
           GeneratedType<WeakTarget>::Register(types).IsOk() &&
           GeneratedType<WeakLink>::Register(types).IsOk() &&
           GeneratedType<TextureGroup>::Register(types).IsOk() &&
           GeneratedType<StructLeaf>::Register(types).IsOk() &&
           GeneratedType<StructMiddle>::Register(types).IsOk();
}

// 시험 환경이다: 타입 Registry, NamePool이 있는 객체 Registry.
struct Env {
    Env() noexcept
        : types(allocator), names(allocator),
          objects(allocator, types, {0x535243, 1}, test::names::WithNames(&names)) {
        ready = names.Initialize().IsOk() && RegisterAll(types);
    }

    ~Env() {
        CHECK(objects.Shutdown(objects.OwnerAccess()));
        CHECK(types.Shutdown());
    }

    ObjectRegistry& Objects() noexcept {
        return objects;
    }

    template <class S>
    TypeLease Lease() noexcept {
        auto lease = types.AcquireValueType(TypeIdOf<S>(), TypeKind::Struct);
        if (!lease) {
            platform::Fatal("test struct lease failed");
        }

        return Move(lease.Value());
    }

    template <class S>
    Result<Array<uint8>> Save(const S& value, const StructRecordOptions& options = {}) noexcept {
        TypeLease lease = Lease<S>();
        return SaveStructRecord(allocator, Objects(), Objects().OwnerAccess(), lease, &value,
                                options);
    }

    template <class S>
    Result<void> Load(Span<const uint8> record, S& value,
                      const StructRecordOptions& options = {}) noexcept {
        TypeLease lease = Lease<S>();
        return LoadStructRecord(allocator, Objects(), Objects().OwnerAccess(), record, lease,
                                &value, options);
    }

    SystemAllocator allocator;
    TypeRegistry types;
    NamePool names;
    ObjectRegistry objects;
    bool ready = false;
};

TEST(StructRecord_RoundTripMemberKinds) {
    Env env;
    REQUIRE(env.ready);
    ObjectRegistry& objects = env.Objects();
    Allocator& values = objects.ObjectAllocator();

    // 정수·String.
    InstancedPoint point(values);
    point.x = -7;
    point.y = 12;
    REQUIRE(point.label.TryAssign("pin"));
    auto pointBytes = env.Save(point);
    REQUIRE(pointBytes);
    InstancedPoint pointBack(values);
    REQUIRE(env.Load(pointBytes.Value().View(), pointBack));
    CHECK(pointBack.x == -7 && pointBack.y == 12 && pointBack.label.View() == "pin");
    auto pointAgain = env.Save(pointBack);
    REQUIRE(pointAgain);
    CHECK(test::graph::SameBytes(pointBytes.Value().View(), pointAgain.Value().View()));

    // float·Array.
    InstancedScale scale(values);
    scale.factor = 2.5f;
    REQUIRE(scale.steps.TryPushBack(3u) && scale.steps.TryPushBack(9u));
    auto scaleBytes = env.Save(scale);
    REQUIRE(scaleBytes);
    InstancedScale scaleBack(values);
    REQUIRE(env.Load(scaleBytes.Value().View(), scaleBack));
    CHECK(scaleBack.factor == 2.5f && scaleBack.steps.Size() == 2 && scaleBack.steps[1] == 9u);

    // Optional(값·String·struct).
    OptionalSlot slot;
    slot.count = Optional<uint32>(4u);
    String note(values);
    REQUIRE(note.TryAssign("n"));
    slot.note = Optional<String>(Move(note));
    OptionalPoint inner(values);
    inner.x = 3;
    inner.weight = Optional<uint32>(8u);
    slot.point = Optional<OptionalPoint>(Move(inner));
    auto slotBytes = env.Save(slot);
    REQUIRE(slotBytes);
    OptionalSlot slotBack;
    REQUIRE(env.Load(slotBytes.Value().View(), slotBack));
    CHECK(slotBack.count.HasValue() && slotBack.count.Value() == 4u);
    CHECK(slotBack.note.HasValue() && slotBack.note.Value().View() == "n");
    REQUIRE(slotBack.point.HasValue());
    CHECK(slotBack.point.Value().x == 3 && slotBack.point.Value().weight.Value() == 8u);

    // Map<uint32, struct>·Set<String>.
    MapBag bag(values);
    MapPoint mapPoint(values);
    mapPoint.y = 6;
    REQUIRE(bag.points.Insert(5u, Move(mapPoint)));
    String word(values);
    REQUIRE(word.TryAssign("w"));
    REQUIRE(bag.words.Add(Move(word)));
    auto bagBytes = env.Save(bag);
    REQUIRE(bagBytes);
    MapBag bagBack(values);
    REQUIRE(env.Load(bagBytes.Value().View(), bagBack));
    REQUIRE(bagBack.points.Count() == 1 && bagBack.words.Count() == 1);
    const MapPoint* found = bagBack.points.Find(5u);
    REQUIRE(found);
    CHECK(found->y == 6);

    // Optional<Name>: 이 Registry의 pool에 intern한다.
    OptionalTag tag;
    auto tagName = env.names.TryIntern("Tag_3");
    REQUIRE(tagName);
    tag.tag = Optional<Name>(tagName.Value());
    auto tagBytes = env.Save(tag);
    REQUIRE(tagBytes);
    OptionalTag tagBack;
    REQUIRE(env.Load(tagBytes.Value().View(), tagBack));
    CHECK(tagBack.tag.HasValue() && tagBack.tag.Value() == tagName.Value());

    // Text(Invariant).
    TextFieldCaption caption;
    auto label = Text::Invariant(values, "Caption");
    REQUIRE(label);
    caption.label = Move(label.Value());
    caption.order = 2;
    auto captionBytes = env.Save(caption);
    REQUIRE(captionBytes);
    TextFieldCaption captionBack;
    REQUIRE(env.Load(captionBytes.Value().View(), captionBack));
    CHECK(captionBack.order == 2);
    auto captionAgain = env.Save(captionBack);
    REQUIRE(captionAgain);
    CHECK(test::graph::SameBytes(captionBytes.Value().View(), captionAgain.Value().View()));

    // ClassRef.
    ClassSlot classSlot;
    classSlot.kind = ClassRef<ClassBase>::Of<ClassDerived>();
    classSlot.count = 1;
    auto classBytes = env.Save(classSlot);
    REQUIRE(classBytes);
    ClassSlot classBack;
    REQUIRE(env.Load(classBytes.Value().View(), classBack));
    CHECK(classBack.kind.Id() == TypeIdOf<ClassDerived>() && classBack.count == 1);
}

// 바이트는 같은 값을 객체 그래프의 InstancedStruct 필드에 담은 내용 Record와 같다.
TEST(StructRecord_MatchesInstancedStructContent) {
    Env env;
    REQUIRE(env.ready);
    ObjectRegistry& objects = env.Objects();
    auto holder = New(objects);
    With(objects, holder.Handle(), [&](InstancedHolder& value) noexcept {
        InstancedPoint& point = Make<InstancedPoint>(objects, value.value);
        point.x = 41;
        REQUIRE(point.label.TryAssign("content"));
    });

    ObjectGraphSaveOptions save;
    save.writeNodeTable = false;
    const ObjectHandle nodes[] = {holder.Handle()};
    auto graph = SaveObjectGraphSet(env.allocator, objects, objects.OwnerAccess(),
                                    Span<const ObjectHandle>(nodes, 1), save);
    REQUIRE(graph);

    // header 24 byte, Record 길이 8 byte, holder Record.
    const Span<const uint8> bytes = graph.Value().Bytes();
    REQUIRE(bytes.Size() > 32);
    const Span<const uint8> holderRecord = bytes.Slice(32, bytes.Size() - 32);
    auto reader = RecordReader::Open(env.allocator, holderRecord, TypeIdOf<InstancedHolder>(), 1,
                                     NumericLimits<uint32>::Max());
    REQUIRE(reader);
    Span<const uint8> content;
    for (usize index = 0; index < reader.Value().FieldCount(); ++index) {
        auto field = reader.Value().Next();
        REQUIRE(field);
        if (field.Value().id == FieldOf<InstancedHolder>("value")) {
            content = field.Value().payload;
        }
    }

    REQUIRE(!content.IsEmpty());
    InstancedPoint point(objects.ObjectAllocator());
    point.x = 41;
    REQUIRE(point.label.TryAssign("content"));
    auto standalone = env.Save(point);
    REQUIRE(standalone);
    CHECK(test::graph::SameBytes(standalone.Value().View(), content));
    holder.Reset();
}

// 담을 수 없는 struct(강한 참조·Registry 컨테이너·InstancedStruct·Required·WorldOnly)와 struct가
// 아닌 lease는 InvalidArgument다.
TEST(StructRecord_RejectsUncontainableStructs) {
    Env env;
    REQUIRE(env.ready);
    ObjectRegistry& objects = env.Objects();
    InstancedPoint point(objects.ObjectAllocator());
    auto bytes = env.Save(point);
    REQUIRE(bytes);
    const TypeId rejected[] = {TypeIdOf<InstancedRefHolder>(), TypeIdOf<InstancedRefArrayHolder>(),
                               TypeIdOf<InstancedNested>(), TypeIdOf<InstancedRequiredLink>(),
                               TypeIdOf<InstancedWorldBound>()};
    for (const TypeId id : rejected) {
        auto lease = env.types.AcquireValueType(id, TypeKind::Struct);
        REQUIRE(lease);
        alignas(16) uint8 storage[256] = {};
        auto saved =
            SaveStructRecord(env.allocator, objects, objects.OwnerAccess(), lease.Value(), storage);
        CHECK(!saved && saved.Error() == ErrorCode::InvalidArgument);
        auto loaded = LoadStructRecord(env.allocator, objects, objects.OwnerAccess(),
                                       bytes.Value().View(), lease.Value(), storage);
        CHECK(!loaded && loaded.Error() == ErrorCode::InvalidArgument);
    }

    auto holder = env.types.Acquire(TypeIdOf<InstancedHolder>());
    REQUIRE(holder);
    auto classSaved =
        SaveStructRecord(env.allocator, objects, objects.OwnerAccess(), holder.Value(), &point);
    CHECK(!classSaved && classSaved.Error() == ErrorCode::InvalidArgument);
    auto empty =
        SaveStructRecord(env.allocator, objects, objects.OwnerAccess(), TypeLease{}, &point);
    CHECK(!empty && empty.Error() == ErrorCode::InvalidArgument);
    TypeLease pointLease = env.Lease<InstancedPoint>();
    auto nullValue =
        SaveStructRecord(env.allocator, objects, objects.OwnerAccess(), pointLease, nullptr);
    CHECK(!nullValue && nullValue.Error() == ErrorCode::InvalidArgument);
}

// weak 참조: 저장은 그래프 집합 밖 Reject 규칙과 같이 null(살아 있는 대상은 ExternalReference
// 진단), 읽기는 0만 받는다.
TEST(StructRecord_WeakReferencesAreWrittenNull) {
    Env env;
    REQUIRE(env.ready);
    ObjectRegistry& objects = env.Objects();
    WeakLink link;
    link.slot = 5;
    auto bytes = env.Save(link);
    REQUIRE(bytes);
    WeakLink back;
    REQUIRE(env.Load(bytes.Value().View(), back));
    CHECK(back.target.Handle().IsNull() && back.slot == 5);

    auto target = objects.NewObject<WeakTarget>(objects.OwnerAccess(), TypeIdOf<WeakTarget>());
    REQUIRE(target);
    WeakLink live;
    live.target = WeakObjectRef<WeakTarget>(target.Value().Handle());
    live.slot = 5;
    Recorder recorder;
    StructRecordOptions report;
    report.hooks = &recorder;
    auto written = env.Save(live, report);
    REQUIRE(written);
    CHECK(test::graph::SameBytes(written.Value().View(), bytes.Value().View()));
    CHECK(recorder.count == 1 &&
          recorder.entries[0].kind == FieldDiagnosticKind::ExternalReference);
    auto quiet = env.Save(live);
    REQUIRE(quiet);
    CHECK(test::graph::SameBytes(quiet.Value().View(), bytes.Value().View()));

    // 파괴 요청된 대상도 진단 없이 0이다.
    const ObjectHandle handle = target.Value().Handle();
    REQUIRE(objects.RequestDestroy(objects.OwnerAccess(), handle));
    WeakLink dead;
    dead.target = WeakObjectRef<WeakTarget>(handle);
    dead.slot = 5;
    auto deadBytes = env.Save(dead);
    REQUIRE(deadBytes);
    CHECK(test::graph::SameBytes(deadBytes.Value().View(), bytes.Value().View()));
    target.Value().Reset();

    // weak 값 1은 읽기가 거부하고 값은 그대로다.
    ArchiveWriter writer(env.allocator);
    REQUIRE(writer.WriteU32(1));
    Array<uint8> payload = writer.TakeBytes();
    const FieldBytes fields[] = {
        {FieldOf<WeakLink>("target"), static_cast<uint32>(WireType::U32), payload.View()}};
    auto bad = RecordOf(env.allocator, TypeIdOf<WeakLink>(), 1, fields);
    WeakLink kept;
    kept.slot = 9;
    auto loaded = env.Load(bad.View(), kept);
    CHECK(!loaded && loaded.Error() == ErrorCode::InvalidArgument);
    CHECK(kept.slot == 9);
}

Result<Array<uint8>> MigrateVersionedOne(Allocator& allocator, TypeId type, uint32 sourceVersion,
                                         uint32 targetVersion, Span<const uint8> source,
                                         void* user) noexcept {
    static_cast<void>(source);
    if (type != TypeIdOf<InstancedVersioned>() || sourceVersion != 1 || targetVersion != 2) {
        return Result<Array<uint8>>::Failure(ErrorCode::Unsupported);
    }

    ++*static_cast<uint32*>(user);
    auto value = I64Payload(allocator, 77);
    const FieldBytes fields[] = {
        {FieldOf<InstancedVersioned>("value"), static_cast<uint32>(WireType::I64), value.View()}};
    return Result<Array<uint8>>::Success(RecordOf(allocator, type, 2, fields));
}

// 옛 버전은 override 마이그레이션으로 읽고, 없으면 Unsupported다. newer는 정책을 따른다.
TEST(StructRecord_VersionsFollowPolicies) {
    Env env;
    REQUIRE(env.ready);
    auto five = I64Payload(env.allocator, 5);
    const FieldBytes fields[] = {
        {FieldOf<InstancedVersioned>("value"), static_cast<uint32>(WireType::I64), five.View()}};
    auto old = RecordOf(env.allocator, TypeIdOf<InstancedVersioned>(), 1, fields);
    InstancedVersioned value;
    value.extra = 3;
    auto missing = env.Load(old.View(), value);
    CHECK(!missing && missing.Error() == ErrorCode::Unsupported);
    CHECK(value.extra == 3);

    uint32 calls = 0;
    StructRecordOptions options;
    options.migrateStruct = &MigrateVersionedOne;
    options.migrationUser = &calls;
    REQUIRE(env.Load(old.View(), value, options));
    CHECK(calls == 1 && value.value == 77 && value.extra == 0);

    auto newer = RecordOf(env.allocator, TypeIdOf<InstancedVersioned>(), 3, fields);
    InstancedVersioned kept;
    kept.value = 1;
    auto rejected = env.Load(newer.View(), kept);
    CHECK(!rejected && rejected.Error() == ErrorCode::Unsupported);
    CHECK(kept.value == 1);
    Recorder recorder;
    StructRecordOptions skip;
    skip.decodePolicy.newerVersion = NewerVersionPolicy::LoadNewerSkippingUnknown;
    skip.hooks = &recorder;
    REQUIRE(env.Load(newer.View(), kept, skip));
    CHECK(kept.value == 5 && recorder.count == 1);
}

// 손상·잘림·뒤 쓰레기·다른 schema는 거부하고 값은 그대로다. 없는 멤버는 기본 생성 값이다.
TEST(StructRecord_CorruptMismatchedAndMissingMembers) {
    Env env;
    REQUIRE(env.ready);
    Allocator& values = env.Objects().ObjectAllocator();
    InstancedPoint point(values);
    point.x = 1;
    point.y = 2;
    REQUIRE(point.label.TryAssign("p"));
    auto bytes = env.Save(point);
    REQUIRE(bytes);

    InstancedPoint kept(values);
    kept.x = 9;
    REQUIRE(kept.label.TryAssign("kept"));
    for (usize cut = 0; cut < bytes.Value().Size(); cut += 7) {
        CHECK(!env.Load(bytes.Value().View().Slice(0, cut), kept));
    }

    Array<uint8> trailing(env.allocator);
    REQUIRE(trailing.TryAppend(bytes.Value().View()) && trailing.TryPushBack(uint8{0}));
    CHECK(!env.Load(trailing.View(), kept));
    InstancedScale scale(values);
    auto scaleBytes = env.Save(scale);
    REQUIRE(scaleBytes);
    CHECK(!env.Load(scaleBytes.Value().View(), kept));
    CHECK(kept.x == 9 && kept.label.View() == "kept");

    // x만 담은 Record: y·label은 기본값이 된다(호출 전 값이 아니다).
    auto x = I64Payload(env.allocator, 4);
    const FieldBytes onlyX[] = {
        {FieldOf<InstancedPoint>("x"), static_cast<uint32>(WireType::I64), x.View()}};
    auto partial = RecordOf(env.allocator, TypeIdOf<InstancedPoint>(), 1, onlyX);
    kept.y = 8;
    REQUIRE(env.Load(partial.View(), kept));
    CHECK(kept.x == 4 && kept.y == 0 && kept.label.View() == "");
}

TEST(StructRecord_Limits) {
    Env env;
    REQUIRE(env.ready);
    InstancedPoint point(env.Objects().ObjectAllocator());
    REQUIRE(point.label.TryAssign("a label longer than the limit"));
    auto bytes = env.Save(point);
    REQUIRE(bytes);
    StructRecordOptions small;
    small.limits.maxValueBytes = 32;
    auto saved = env.Save(point, small);
    CHECK(!saved && saved.Error() == ErrorCode::CapacityExceeded);
    InstancedPoint kept(env.Objects().ObjectAllocator());
    auto loaded = env.Load(bytes.Value().View(), kept, small);
    CHECK(!loaded && loaded.Error() == ErrorCode::CapacityExceeded);
    StructRecordOptions fields;
    fields.limits.maxTotalFields = 1;
    auto tooMany = env.Load(bytes.Value().View(), kept, fields);
    CHECK(!tooMany && tooMany.Error() == ErrorCode::CapacityExceeded);
    StructRecordOptions invalid;
    invalid.limits.maxValueBytes = 0;
    auto rejected = env.Save(point, invalid);
    CHECK(!rejected && rejected.Error() == ErrorCode::InvalidArgument);
}

// attempt번째부터 모든 할당이 실패하는 allocator다.
class FailFromAllocator final : public Allocator {
public:
    explicit FailFromAllocator(Allocator& inner) noexcept : inner_(inner) {
    }

    void FailFrom(uint64 attempt) noexcept {
        attempts_ = 0;
        failFrom_ = attempt;
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        ++attempts_;
        if (failFrom_ != 0 && attempts_ >= failFrom_) {
            return Result<void*>::Failure(ErrorCode::OutOfMemory);
        }

        return inner_.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        return inner_.Deallocate(address);
    }

private:
    Allocator& inner_;
    uint64 attempts_ = 0;
    uint64 failFrom_ = 0;
};

// 작업 allocator와 객체 allocator의 k번째 할당부터 실패시켜도 값(미리 채운 내용)은 그대로이고, 결국
// 성공한다.
TEST(StructRecord_AllocationFailureKeepsValue) {
    Env env;
    REQUIRE(env.ready);
    FailFromAllocator failingObjects(env.allocator);
    ObjectRegistry objects(failingObjects, env.types, {0x535243, 2},
                           test::names::WithNames(&env.names));
    {
        MapBag bag(objects.ObjectAllocator());
        MapPoint mapPoint(objects.ObjectAllocator());
        REQUIRE(mapPoint.label.TryAssign("x"));
        REQUIRE(bag.points.Insert(2u, Move(mapPoint)));
        String word(objects.ObjectAllocator());
        REQUIRE(word.TryAssign("w"));
        REQUIRE(bag.words.Add(Move(word)));
        TypeLease lease = env.Lease<MapBag>();
        auto bytes = SaveStructRecord(env.allocator, objects, objects.OwnerAccess(), lease, &bag);
        REQUIRE(bytes);
        FailFromAllocator failing(env.allocator);
        bool saved = false;
        bool loaded = false;
        uint64 failures = 0;
        for (uint64 attempt = 1; attempt < 4096 && !(saved && loaded); ++attempt) {
            if (!saved) {
                failing.FailFrom(attempt);
                auto result =
                    SaveStructRecord(failing, objects, objects.OwnerAccess(), lease, &bag);
                failing.FailFrom(0);
                saved = static_cast<bool>(result);
                if (saved) {
                    CHECK(test::graph::SameBytes(result.Value().View(), bytes.Value().View()));
                } else {
                    CHECK(result.Error() == ErrorCode::OutOfMemory);
                }
            }

            // 다른 내용(키 9)을 미리 채운 값이다.
            MapBag kept(objects.ObjectAllocator());
            MapPoint old(objects.ObjectAllocator());
            old.x = 1;
            REQUIRE(kept.points.Insert(9u, Move(old)));
            failing.FailFrom(attempt);
            failingObjects.FailFrom(attempt);
            auto result = LoadStructRecord(failing, objects, objects.OwnerAccess(),
                                           bytes.Value().View(), lease, &kept);
            failing.FailFrom(0);
            failingObjects.FailFrom(0);
            if (result) {
                loaded = true;
                CHECK(kept.points.Count() == 1 && kept.points.Find(2u) && kept.words.Count() == 1);
            } else {
                ++failures;
                CHECK(result.Error() == ErrorCode::OutOfMemory);
                CHECK(kept.points.Count() == 1 && kept.points.Find(9u) && kept.words.Count() == 0);
            }
        }

        CHECK(saved && loaded && failures > 0);
    }

    CHECK(objects.Shutdown(objects.OwnerAccess()));
}

// 중첩 struct 멤버·enum·EditorOnly 멤버 왕복과 타입이 등록한 마이그레이션(StructLeaf::Migrate).
TEST(StructRecord_NestedStructEnumAndTypeMigration) {
    Env env;
    REQUIRE(env.ready);
    Allocator& values = env.Objects().ObjectAllocator();
    StructMiddle middle(values);
    middle.leaf.count = 3;
    middle.leaf.group = TextureGroup::EightBitData;
    REQUIRE(middle.leaf.label.TryAssign("leaf"));
    middle.offset = -40;
    middle.note = 7;
    auto bytes = env.Save(middle);
    REQUIRE(bytes);
    StructMiddle back(values);
    REQUIRE(env.Load(bytes.Value().View(), back));
    CHECK(back.leaf.count == 3 && back.leaf.group == TextureGroup::EightBitData &&
          back.leaf.label.View() == "leaf" && back.offset == -40 && back.note == 7);
    auto again = env.Save(back);
    REQUIRE(again);
    CHECK(test::graph::SameBytes(bytes.Value().View(), again.Value().View()));

    // v1 StructLeaf Record는 타입의 마이그레이션으로 읽는다.
    ArchiveWriter writer(env.allocator);
    REQUIRE(writer.WriteU32(12));
    Array<uint8> count = writer.TakeBytes();
    const FieldBytes fields[] = {
        {FieldOf<StructLeaf>("count"), static_cast<uint32>(WireType::U32), count.View()}};
    auto old = RecordOf(env.allocator, TypeIdOf<StructLeaf>(), 1, fields);
    const uint32 before = StructLeaf::migrations;
    StructLeaf leaf(values);
    REQUIRE(env.Load(old.View(), leaf));
    CHECK(StructLeaf::migrations == before + 1 && leaf.count == 12);
}

// NamePool이 없는 Registry는 None이 아닌 Name을 읽지 못한다(Unsupported). 빈 입력은
// InvalidArgument다.
TEST(StructRecord_NameWithoutPoolAndEmptyInput) {
    Env env;
    REQUIRE(env.ready);
    OptionalTag tag;
    auto name = env.names.TryIntern("Tag");
    REQUIRE(name);
    tag.tag = Optional<Name>(name.Value());
    auto bytes = env.Save(tag);
    REQUIRE(bytes);
    {
        ObjectRegistry bare(env.allocator, env.types, {0x535243, 3});
        TypeLease lease = env.Lease<OptionalTag>();
        OptionalTag kept;
        auto loaded = LoadStructRecord(env.allocator, bare, bare.OwnerAccess(),
                                       bytes.Value().View(), lease, &kept);
        CHECK(!loaded && loaded.Error() == ErrorCode::Unsupported);
        CHECK(!kept.tag.HasValue());
        CHECK(bare.Shutdown(bare.OwnerAccess()));
    }

    OptionalTag kept;
    auto empty = env.Load(Span<const uint8>(), kept);
    CHECK(!empty && empty.Error() == ErrorCode::InvalidArgument);
    StructRecordOptions noHooks;
    noHooks.decodePolicy.newerVersion = NewerVersionPolicy::LoadNewerSkippingUnknown;
    auto rejected = env.Load(bytes.Value().View(), kept, noHooks);
    CHECK(!rejected && rejected.Error() == ErrorCode::InvalidArgument);
}

TEST(StructRecord_AcquireType) {
    Env env;
    REQUIRE(env.ready);
    InstancedPoint point(env.Objects().ObjectAllocator());
    auto bytes = env.Save(point);
    REQUIRE(bytes);
    auto type = AcquireStructRecordType(env.types, bytes.Value().View());
    REQUIRE(type);
    CHECK(type.Value().Schema().id == TypeIdOf<InstancedPoint>());
    auto unknown = RecordOf(env.allocator, Guid{7, 7}, 1, Span<const FieldBytes>());
    auto missing = AcquireStructRecordType(env.types, unknown.View());
    CHECK(!missing && missing.Error() == ErrorCode::NotFound);
    auto classRecord =
        RecordOf(env.allocator, TypeIdOf<InstancedHolder>(), 1, Span<const FieldBytes>());
    auto mismatch = AcquireStructRecordType(env.types, classRecord.View());
    CHECK(!mismatch && mismatch.Error() == ErrorCode::TypeMismatch);
    const uint8 junk[] = {1, 2, 3};
    CHECK(!AcquireStructRecordType(env.types, Span<const uint8>(junk, 3)));
}

} // namespace struct_record_tests_file
