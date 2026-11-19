#include "InstancedStructTestSupport.h"
#include <Core/Text/NamePool.h>

// 리플렉션 B9j InstancedStruct PROPERTY의 그래프 저장·복원 시험이다(Docs/Design/
// ReflectionExtensions.ko.md 1.2·11절, 17절 B9j 항목): 비어 있음·값 왕복(필드·Array·정적 배열 원소·
// Map 값·struct 멤버 자리)과 다시 저장한 byte 같음, 필드 자리 wire(빈 payload·Record), 옛 버전
// 내용의 마이그레이션, 미등록 struct 정책 두 가지, 담을 수 없는 struct의 decode·Emplace 거부, 깊이
// 합 규칙 (encode·decode), 타입 lease의 Unregister Busy, 관찰 struct 제한.
using namespace rocks;
namespace instanced_struct_graph_tests_file {

using namespace rocks::test::instanced;

namespace {

// 모든 InstancedStruct 자리를 채운다: value = Point{1, -2, "spot"}, spare 비어 있음, list =
// [Scale{2.5, [4, 5]}, 비어 있음, Point{7}], fixed = [Point{8}, 비어 있음], table = {3: Point{9},
// 5: 비어 있음}, host = {Scale{0.5}, 6}.
void Fill(ObjectRegistry& objects, ObjectHandle holder) noexcept {
    With(objects, holder, [&](InstancedHolder& value) noexcept {
        InstancedPoint& point = Make<InstancedPoint>(objects, value.value);
        point.x = 1;
        point.y = -2;
        REQUIRE(point.label.TryAssign("spot"));
        REQUIRE(value.list.TryEmplaceBack());
        REQUIRE(value.list.TryEmplaceBack());
        REQUIRE(value.list.TryEmplaceBack());
        InstancedScale& scale = Make<InstancedScale>(objects, value.list[0]);
        scale.factor = 2.5f;
        REQUIRE(scale.steps.TryPushBack(4u));
        REQUIRE(scale.steps.TryPushBack(5u));
        Make<InstancedPoint>(objects, value.list[2]).x = 7;
        Make<InstancedPoint>(objects, value.fixed[0]).x = 8;
        auto added = value.table.Insert(3u, InstancedStruct<>());
        REQUIRE(added);
        Make<InstancedPoint>(objects, *value.table.Find(3u)).x = 9;
        REQUIRE(value.table.Insert(5u, InstancedStruct<>()));
        Make<InstancedScale>(objects, value.host.value).factor = 0.5f;
        value.host.count = 6;
    });
}

// Fill의 값을 확인한다.
void CheckFilled(ObjectRegistry& objects, ObjectHandle holder) noexcept {
    With(objects, holder, [](InstancedHolder& value) noexcept {
        const InstancedPoint* point = value.value.Get<InstancedPoint>();
        REQUIRE(point);
        CHECK(point->x == 1 && point->y == -2 && point->label.View() == "spot");
        CHECK(value.spare.IsEmpty());
        REQUIRE(value.list.Size() == 3);
        const InstancedScale* scale = value.list[0].Get<InstancedScale>();
        REQUIRE(scale);
        CHECK(scale->factor == 2.5f && scale->steps.Size() == 2 && scale->steps[1] == 5u);
        CHECK(value.list[1].IsEmpty());
        CHECK(value.list[2].Get<InstancedPoint>() && value.list[2].Get<InstancedPoint>()->x == 7);
        CHECK(value.fixed[0].Get<InstancedPoint>() && value.fixed[0].Get<InstancedPoint>()->x == 8);
        CHECK(value.fixed[1].IsEmpty());
        REQUIRE(value.table.Count() == 2);
        CHECK(value.table.Find(3u) && value.table.Find(3u)->Get<InstancedPoint>() &&
              value.table.Find(3u)->Get<InstancedPoint>()->x == 9);
        CHECK(value.table.Find(5u) && value.table.Find(5u)->IsEmpty());
        CHECK(value.host.value.Get<InstancedScale>() &&
              value.host.value.Get<InstancedScale>()->factor == 0.5f);
        CHECK(value.host.count == 6u);
        CHECK(value.locked.IsEmpty());
    });
}

// 옛 버전 1 InstancedVersioned Record를 버전 2로 올린다: value를 10배하고 extra를 1로 둔다.
Result<Array<uint8>> MigrateVersioned(Allocator& allocator, TypeId type, uint32 sourceVersion,
                                      uint32 targetVersion, Span<const uint8> source,
                                      void*) noexcept {
    if (type != TypeIdOf<InstancedVersioned>() || sourceVersion != 1 || targetVersion != 2) {
        return Result<Array<uint8>>::Failure(ErrorCode::Unsupported);
    }

    auto reader = RecordReader::Open(allocator, source, type, 1, 1);
    if (!reader) {
        return Result<Array<uint8>>::Failure(reader.Error());
    }

    int64 value = 0;
    for (usize index = 0; index < reader.Value().FieldCount(); ++index) {
        auto field = reader.Value().Next();
        if (!field) {
            return Result<Array<uint8>>::Failure(field.Error());
        }

        if (field.Value().id == FieldOf<InstancedVersioned>("value")) {
            auto archive = reader.Value().OpenValue(field.Value());
            if (!archive) {
                return Result<Array<uint8>>::Failure(archive.Error());
            }

            auto read = archive.Value().ReadI64();
            if (!read) {
                return Result<Array<uint8>>::Failure(read.Error());
            }

            value = read.Value();
        }
    }

    auto scaled = I64Payload(allocator, value * 10);
    auto extra = I64Payload(allocator, 1);
    const FieldBytes fields[] = {
        {FieldOf<InstancedVersioned>("value"), static_cast<uint32>(WireType::I64), scaled.View()},
        {FieldOf<InstancedVersioned>("extra"), static_cast<uint32>(WireType::I64), extra.View()}};
    return Result<Array<uint8>>::Success(RecordOf(allocator, type, 2, fields));
}

// value 필드 payload가 content인 InstancedHolder 그래프다.
Array<uint8> ValueGraph(Allocator& allocator, Span<const uint8> content) noexcept {
    const FieldBytes fields[] = {{FieldOf<InstancedHolder>("value"),
                                  static_cast<uint32>(WireType::InstancedStruct), content}};
    return HolderGraph(allocator, fields);
}

} // namespace

// 모든 자리의 비어 있음·값이 그대로 왕복하고, 복원한 그래프를 다시 저장하면 byte가 같다(원소·Map
// 값 자리는 u64 길이 + 내용, 비어 있음은 길이 0).
TEST(InstancedStructGraph_RoundTripEmptyAndValues) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry source(allocator, types, {0x495347, 1});
        ObjectRegistry target(allocator, types, {0x495347, 2});
        {
            RootRef<InstancedHolder> holder = New(source);
            Fill(source, holder.Handle());
            auto saved = SaveObjectGraph(allocator, source, source.OwnerAccess(), holder.Handle());
            REQUIRE(saved);
            auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                          saved.Value().View());
            REQUIRE(loaded);
            CheckFilled(target, loaded.Value().Handle());
            auto again =
                SaveObjectGraph(allocator, target, target.OwnerAccess(), loaded.Value().Handle());
            REQUIRE(again);
            CHECK(test::graph::SameBytes(saved.Value().View(), again.Value().View()));
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// 필드 자리 wire: 빈 payload는 비어 있음이고, Record 하나는 그 struct 값이다(Record에 없는 멤버는
// 그 struct의 기본 값). Record 뒤에 byte가 붙거나 Record가 아닌 payload는 InvalidArgument다.
TEST(InstancedStructGraph_FieldWireIsEmptyPayloadOrRecord) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(allocator, types, {0x495347, 3});
        auto empty = ValueGraph(allocator, Span<const uint8>());
        auto loaded =
            LoadObjectGraph(allocator, objects, types, objects.OwnerAccess(), empty.View());
        REQUIRE(loaded);
        With(objects, loaded.Value().Handle(),
             [](InstancedHolder& value) noexcept { CHECK(value.value.IsEmpty()); });
        loaded.Value().Reset();

        auto x = I64Payload(allocator, 42);
        const FieldBytes members[] = {
            {FieldOf<InstancedPoint>("x"), static_cast<uint32>(WireType::I64), x.View()}};
        auto record = RecordOf(allocator, TypeIdOf<InstancedPoint>(), 1, members);
        auto point = ValueGraph(allocator, record.View());
        auto withPoint =
            LoadObjectGraph(allocator, objects, types, objects.OwnerAccess(), point.View());
        REQUIRE(withPoint);
        With(objects, withPoint.Value().Handle(), [](InstancedHolder& value) noexcept {
            const InstancedPoint* loadedPoint = value.value.Get<InstancedPoint>();
            REQUIRE(loadedPoint);
            CHECK(loadedPoint->x == 42 && loadedPoint->y == 0 && loadedPoint->label.View() == "");
        });
        withPoint.Value().Reset();

        Array<uint8> trailing(allocator);
        REQUIRE(trailing.TryAppend(record.View()));
        REQUIRE(trailing.TryPushBack(uint8{0}));
        auto bad = ValueGraph(allocator, trailing.View());
        auto trailed =
            LoadObjectGraph(allocator, objects, types, objects.OwnerAccess(), bad.View());
        CHECK(!trailed && trailed.Error() == ErrorCode::InvalidArgument);
        const uint8 junk[] = {1, 2, 3};
        auto notRecord = ValueGraph(allocator, Span<const uint8>(junk, 3));
        CHECK(!LoadObjectGraph(allocator, objects, types, objects.OwnerAccess(), notRecord.View()));
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// 담긴 struct의 옛 버전 Record는 중첩 struct처럼 마이그레이션한다(로드 옵션 migrateStruct).
// 마이그레이션이 없으면 Unsupported다.
TEST(InstancedStructGraph_OldContentVersionIsMigrated) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(allocator, types, {0x495347, 4});
        auto five = I64Payload(allocator, 5);
        const FieldBytes members[] = {{FieldOf<InstancedVersioned>("value"),
                                       static_cast<uint32>(WireType::I64), five.View()}};
        auto old = RecordOf(allocator, TypeIdOf<InstancedVersioned>(), 1, members);
        auto graph = ValueGraph(allocator, old.View());
        ObjectGraphLoadOptions options;
        options.migrateStruct = &MigrateVersioned;
        auto loaded = LoadObjectGraph(allocator, objects, types, objects.OwnerAccess(),
                                      graph.View(), options);
        REQUIRE(loaded);
        With(objects, loaded.Value().Handle(), [](InstancedHolder& value) noexcept {
            const InstancedVersioned* versioned = value.value.Get<InstancedVersioned>();
            REQUIRE(versioned);
            CHECK(versioned->value == 50 && versioned->extra == 1);
        });

        loaded.Value().Reset();
        auto unmigrated =
            LoadObjectGraph(allocator, objects, types, objects.OwnerAccess(), graph.View());
        CHECK(!unmigrated && unmigrated.Error() == ErrorCode::Unsupported);
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// 담긴 struct가 복원 쪽 TypeRegistry에 없으면 decode 정책 unknownStructType을 따른다:
// Reject(기본)는 NotFound, ReportAndClear는 진단 hook이 없으면 InvalidArgument이고, 있으면 그
// 값마다 UnknownStructType(valueType = 저장 struct ID, 경로 =
// 필드·[list[2]]·[fixed[0]]·[table[0]])을 내고 비운다 — 등록된 struct 값은 그대로다.
TEST(InstancedStructGraph_UnknownStructTypeFollowsDecodePolicy) {
    SystemAllocator allocator;
    TypeRegistry sourceTypes(allocator);
    TypeRegistry targetTypes(allocator);
    REQUIRE(RegisterFixture(sourceTypes));
    REQUIRE(RegisterFixture(targetTypes, false));
    {
        ObjectRegistry source(allocator, sourceTypes, {0x495347, 5});
        ObjectRegistry target(allocator, targetTypes, {0x495347, 6});
        RootRef<InstancedHolder> holder = New(source);
        Fill(source, holder.Handle());
        auto bytes = SaveObjectGraph(allocator, source, source.OwnerAccess(), holder.Handle());
        REQUIRE(bytes);

        auto rejected = LoadObjectGraph(allocator, target, targetTypes, target.OwnerAccess(),
                                        bytes.Value().View());
        CHECK(!rejected && rejected.Error() == ErrorCode::NotFound);

        ObjectGraphLoadOptions silent;
        silent.policy.unknownStructType = UnknownStructTypePolicy::ReportAndClear;
        auto unreported = LoadObjectGraph(allocator, target, targetTypes, target.OwnerAccess(),
                                          bytes.Value().View(), silent);
        CHECK(!unreported && unreported.Error() == ErrorCode::InvalidArgument);
        CHECK(target.Stats().liveObjects == 0);

        Recorder recorder;
        ObjectGraphLoadOptions reported = silent;
        reported.hooks = &recorder;
        auto loaded = LoadObjectGraph(allocator, target, targetTypes, target.OwnerAccess(),
                                      bytes.Value().View(), reported);
        REQUIRE(loaded);
        CHECK(recorder.count == 4);
        for (uint32 index = 0; index < recorder.count; ++index) {
            CHECK(recorder.entries[index].kind == FieldDiagnosticKind::UnknownStructType);
            CHECK(recorder.entries[index].valueType == TypeIdOf<InstancedPoint>());
            CHECK(recorder.entries[index].steps == 1);
        }

        With(target, loaded.Value().Handle(), [](InstancedHolder& value) noexcept {
            CHECK(value.value.IsEmpty());
            REQUIRE(value.list.Size() == 3);
            CHECK(value.list[0].Get<InstancedScale>() && value.list[2].IsEmpty());
            CHECK(value.fixed[0].IsEmpty());
            CHECK(value.table.Find(3u) && value.table.Find(3u)->IsEmpty());
            CHECK(value.host.value.Get<InstancedScale>());
        });

        loaded.Value().Reset();
        holder.Reset();
        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(sourceTypes.Shutdown());
    CHECK(targetTypes.Shutdown());
}

// 담을 수 없는 struct(강한 참조·Required·InstancedStruct를 담음)와 struct가 아닌 레코드(클래스)는
// decode가 정책과 무관하게 InvalidArgument이고, Emplace(lease 판)도 InvalidArgument다(값은 그대로).
TEST(InstancedStructGraph_UncontainableStructsAreRejected) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(allocator, types, {0x495347, 7});
        const TypeId rejected[] = {TypeIdOf<InstancedRefHolder>(),
                                   TypeIdOf<InstancedRequiredLink>(), TypeIdOf<InstancedNested>(),
                                   TypeIdOf<InstancedHolder>()};
        ObjectGraphLoadOptions lenient;
        lenient.policy.unknownStructType = UnknownStructTypePolicy::ReportAndClear;
        Recorder recorder;
        lenient.hooks = &recorder;
        for (const TypeId& type : rejected) {
            auto record = RecordOf(allocator, type, 1, {});
            auto graph = ValueGraph(allocator, record.View());
            auto loaded = LoadObjectGraph(allocator, objects, types, objects.OwnerAccess(),
                                          graph.View(), lenient);
            CHECK(!loaded && loaded.Error() == ErrorCode::InvalidArgument);
        }

        CHECK(recorder.count == 0);
        CHECK(objects.Stats().liveObjects == 0);

        InstancedStruct<> value;
        REQUIRE(value.Emplace<InstancedPoint>(types, objects.ObjectAllocator()));
        const TypeId leases[] = {TypeIdOf<InstancedRefHolder>(), TypeIdOf<InstancedRequiredLink>(),
                                 TypeIdOf<InstancedNested>()};
        for (const TypeId& type : leases) {
            auto lease = types.AcquireValueType(type, TypeKind::Struct);
            REQUIRE(lease);
            auto made = value.Emplace(Move(lease.Value()), objects.ObjectAllocator());
            CHECK(!made && made.Error() == ErrorCode::InvalidArgument);
            CHECK(value.Get<InstancedPoint>() != nullptr);
        }

        auto classLease = types.Acquire(TypeIdOf<InstancedHolder>());
        REQUIRE(classLease);
        CHECK(ErrorOf(value.Emplace(Move(classLease.Value()), objects.ObjectAllocator())) ==
              ErrorCode::InvalidArgument);
        CHECK(ErrorOf(value.Emplace(TypeLease{}, objects.ObjectAllocator())) ==
              ErrorCode::InvalidArgument);
        value.Reset();
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// 깊이 합: 위치 깊이 + 담긴 struct 깊이 ≤ 16. 최상위 필드(위치 0)는 깊이 16 struct를 담아 왕복하고,
// struct 멤버 host.value(위치 1)는 깊이 15까지다. C++가 Emplace로 host.value에 깊이 16을 넣으면
// 저장이 Fatal 없이 InvalidArgument이고, 같은 내용을 담은 파일은 decode 1단계가 InvalidArgument로
// 거부한다.
TEST(InstancedStructGraph_DepthSumIsCheckedOnEncodeAndDecode) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(allocator, types, {0x495347, 8});
        RootRef<InstancedHolder> holder = New(objects);
        With(objects, holder.Handle(), [&](InstancedHolder& value) noexcept {
            Make<InstancedDepth16>(objects, value.value);
            Make<InstancedDepth15>(objects, value.host.value);
        });

        auto saved = SaveObjectGraph(allocator, objects, objects.OwnerAccess(), holder.Handle());
        REQUIRE(saved);
        auto loaded =
            LoadObjectGraph(allocator, objects, types, objects.OwnerAccess(), saved.Value().View());
        REQUIRE(loaded);
        With(objects, loaded.Value().Handle(), [](InstancedHolder& value) noexcept {
            CHECK(value.value.Get<InstancedDepth16>() != nullptr);
            CHECK(value.host.value.Get<InstancedDepth15>() != nullptr);
        });

        loaded.Value().Reset();
        With(objects, holder.Handle(), [&](InstancedHolder& value) noexcept {
            Make<InstancedDepth16>(objects, value.host.value);
        });

        auto deep = SaveObjectGraph(allocator, objects, objects.OwnerAccess(), holder.Handle());
        CHECK(!deep && deep.Error() == ErrorCode::InvalidArgument);

        // host = {value: 깊이 16 Record}를 직접 조립한다(깊이 16 Record는 위치 0에서 저장한 값의
        // 내용을 재조립한다 — InstancedDepth1의 v부터 감싼다).
        Array<uint8> content(allocator);
        {
            auto v = I64Payload(allocator, 3);
            const FieldBytes leaf[] = {
                {FieldOf<InstancedDepth1>("v"), static_cast<uint32>(WireType::I64), v.View()}};
            content = RecordOf(allocator, TypeIdOf<InstancedDepth1>(), 1, leaf);
        }

        const TypeId chain[] = {TypeIdOf<reflection_fixture::InstancedDepth2>(),
                                TypeIdOf<reflection_fixture::InstancedDepth3>(),
                                TypeIdOf<reflection_fixture::InstancedDepth4>(),
                                TypeIdOf<reflection_fixture::InstancedDepth5>(),
                                TypeIdOf<reflection_fixture::InstancedDepth6>(),
                                TypeIdOf<reflection_fixture::InstancedDepth7>(),
                                TypeIdOf<reflection_fixture::InstancedDepth8>(),
                                TypeIdOf<reflection_fixture::InstancedDepth9>(),
                                TypeIdOf<reflection_fixture::InstancedDepth10>(),
                                TypeIdOf<reflection_fixture::InstancedDepth11>(),
                                TypeIdOf<reflection_fixture::InstancedDepth12>(),
                                TypeIdOf<reflection_fixture::InstancedDepth13>(),
                                TypeIdOf<reflection_fixture::InstancedDepth14>(),
                                TypeIdOf<reflection_fixture::InstancedDepth15>(),
                                TypeIdOf<reflection_fixture::InstancedDepth16>()};
        for (const TypeId& type : chain) {
            auto lease = types.AcquireValueType(type, TypeKind::Struct);
            REQUIRE(lease);
            const Guid inner = lease.Value().Schema().fields[0].id;
            const FieldBytes wrap[] = {
                {inner, static_cast<uint32>(WireType::Record), content.View()}};
            content = RecordOf(allocator, type, 1, wrap);
        }

        const FieldBytes hostFields[] = {{FieldOf<InstancedHost>("value"),
                                          static_cast<uint32>(WireType::InstancedStruct),
                                          content.View()}};
        auto host = RecordOf(allocator, TypeIdOf<InstancedHost>(), 1, hostFields);
        const FieldBytes holderFields[] = {
            {FieldOf<InstancedHolder>("host"), static_cast<uint32>(WireType::Record), host.View()}};
        auto graph = HolderGraph(allocator, holderFields);
        auto rejected =
            LoadObjectGraph(allocator, objects, types, objects.OwnerAccess(), graph.View());
        CHECK(!rejected && rejected.Error() == ErrorCode::InvalidArgument);

        // 같은 깊이 16 내용은 최상위 필드(위치 0)에서 받는다.
        auto top = ValueGraph(allocator, content.View());
        auto accepted =
            LoadObjectGraph(allocator, objects, types, objects.OwnerAccess(), top.View());
        REQUIRE(accepted);
        accepted.Value().Reset();
        holder.Reset();
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// 값은 담긴 struct의 타입 lease를 쥐므로 그 struct의 Unregister는 Busy이고, 비운 뒤에는 된다.
TEST(InstancedStructGraph_TypeLeaseKeepsRegistration) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    auto token = GeneratedType<InstancedPoint>::Register(types);
    REQUIRE(token);
    REQUIRE(RegisterFixture(types, false));
    {
        ObjectRegistry objects(allocator, types, {0x495347, 9});
        InstancedStruct<> value;
        REQUIRE(value.Emplace<InstancedPoint>(types, objects.ObjectAllocator()));
        auto copy = value.TryClone(objects.ObjectAllocator());
        REQUIRE(copy);
        CHECK(copy.Value().Get<InstancedPoint>() != value.Get<InstancedPoint>());
        CHECK(ErrorOf(types.Unregister(token.Value())) == ErrorCode::Busy);
        value.Reset();
        CHECK(ErrorOf(types.Unregister(token.Value())) == ErrorCode::Busy);
        copy.Value().Reset();
        CHECK(types.Unregister(token.Value()));
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// InstancedStruct를 담은 struct(InstancedHost)는 관찰 대상이 될 수 없다(Validate InvalidArgument).
TEST(InstancedStructGraph_ObservedStructCannotHoldInstancedStruct) {
    struct ObserveHost final : FieldCodecHooks {
        Span<const TypeId> ObservedStructs() noexcept override {
            return Span<const TypeId>(&type, 1);
        }

        TypeId type = TypeIdOf<InstancedHost>();
    };

    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(allocator, types, {0x495347, 10});
        RootRef<InstancedHolder> holder = New(objects);
        auto saved = SaveObjectGraph(allocator, objects, objects.OwnerAccess(), holder.Handle());
        REQUIRE(saved);
        ObserveHost hooks;
        ObjectGraphLoadOptions options;
        options.hooks = &hooks;
        auto loaded = LoadObjectGraph(allocator, objects, types, objects.OwnerAccess(),
                                      saved.Value().View(), options);
        CHECK(!loaded && loaded.Error() == ErrorCode::InvalidArgument);
        holder.Reset();
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}
} // namespace instanced_struct_graph_tests_file
