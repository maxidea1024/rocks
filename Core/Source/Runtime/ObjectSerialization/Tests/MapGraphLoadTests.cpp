#include "MapTestSupport.h"
#include <Object/PropertyPath.h>
#include <TestSupport/NFailAllocator.h>

// 리플렉션 B9f2 Map·Set PROPERTY의 복원 세부 시험이다(Docs/Design/ReflectionExtensions.ko.md 1.2·
// 7.2절): 최상위 Map<K, struct> 값 Record의 1단계 선청구(2단계 몫), 값 struct의 누락 멤버 = 원소
// 기본값, 값 Record 마이그레이션, 관찰 struct 안 Map(관찰 임시 값 채우기·Name 원소 Set·Name 키
// Map을 담은 struct 거부), 중복 색인 예약의 키 영역 상한, 2단계 할당 실패 rollback.
using namespace rocks;
namespace map_graph_load_tests_file {

using namespace rocks::test::map;

namespace {

// MapBag을 관찰하고 받은 값을 기록한다.
struct BagObserver final : FieldCodecHooks {
    explicit BagObserver(TypeId type) noexcept : id(type) {
    }

    Span<const TypeId> ObservedStructs() noexcept override {
        return Span<const TypeId>(&id, 1);
    }

    Result<void> ObserveStruct(const FieldCodecFrame&, TypeId type, const void*,
                               const void* value) noexcept override {
        ++observed;
        if (type == TypeIdOf<MapBag>()) {
            const auto& bag = *static_cast<const MapBag*>(value);
            const MapPoint* point = bag.points.Find(uint32{2});
            pointX = point ? point->x : 0;
            words = static_cast<uint32>(bag.words.Count());
        }

        return Result<void>::Success();
    }

    TypeId id;
    uint32 observed = 0;
    int32 pointX = 0;
    uint32 words = 0;
};

// MapVersioned 버전 1 Record(value만)를 버전 2(value × 10, extra = 1)로 올린다(user는 받은 원본
// byte 수를 쓸 usize).
Result<Array<uint8>> MigrateVersioned(Allocator& allocator, TypeId type, uint32 sourceVersion,
                                      uint32 targetVersion, Span<const uint8> source,
                                      void* user) noexcept {
    if (type != TypeIdOf<MapVersioned>() || sourceVersion != 1 || targetVersion != 2) {
        return Result<Array<uint8>>::Failure(ErrorCode::Unsupported);
    }

    *static_cast<usize*>(user) = source.Size();
    auto reader = RecordReader::Open(allocator, source, type, 1, 1);
    if (!reader) {
        return Result<Array<uint8>>::Failure(reader.Error());
    }

    int64 value = 0;
    const Guid valueId = SchemaField(TypeOf<MapVersioned>(), "value").id;
    for (usize index = 0; index < reader.Value().FieldCount(); ++index) {
        auto field = reader.Value().Next();
        if (!field) {
            return Result<Array<uint8>>::Failure(field.Error());
        }

        if (field.Value().id == valueId) {
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

    ArchiveWriter scaled(allocator);
    ArchiveWriter extra(allocator);
    if (!scaled.WriteI64(value * 10) || !extra.WriteI64(1)) {
        return Result<Array<uint8>>::Failure(ErrorCode::OutOfMemory);
    }

    const FieldBytes fields[] = {{valueId, static_cast<uint32>(WireType::I64), scaled.View()},
                                 {SchemaField(TypeOf<MapVersioned>(), "extra").id,
                                  static_cast<uint32>(WireType::I64), extra.View()}};
    return Result<Array<uint8>>::Success(RecordOf(allocator, type, 2, fields));
}

} // namespace

// 최상위 Map<K, struct>의 값 Record는 2단계가 단독 reader로 넘기고(내용 byte 청구) 단독 Record로
// 다시 열므로(payload byte 청구) 1단계가 그 몫(Record byte의 두 배)을 미리 청구한다(1.2절 "뒤 단계
// 주의" — Optional<struct>와 같은 규칙). struct 안 Map(MapBag::points)은 2단계도 부모 Record 트리의
// 공유 예산이라 그 몫이 없다: 같은 크기만큼 값 label을 늘렸을 때 최소 1단계 작업량의 증가는 최상위
// 쪽이 struct 안 쪽보다 정확히 늘린 byte만큼 크다.
TEST(MapGraph_TopLevelStructValuesArePrechargedTwice) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(allocator, types, {0x4d4150, 20});
        constexpr usize LabelCapacity = 512;
        const auto minimum = [&](bool topLevel, usize labelBytes) noexcept -> int64 {
            char label[LabelCapacity] = {};
            for (usize index = 0; index < labelBytes && index < LabelCapacity; ++index) {
                label[index] = 'a';
            }

            Array<uint8> bytes(allocator);
            {
                RootRef<MapHolder> holder = New(objects);
                With(objects, holder.Handle(), [&](MapHolder& value) noexcept {
                    MapPoint point(objects.ObjectAllocator());
                    REQUIRE(point.label.TryAssign(StringView(label, labelBytes)));
                    if (topLevel) {
                        Put(value.points, MakeString(objects.ObjectAllocator(), "k"), Move(point));
                    } else {
                        Put(value.bag.points, uint32{1}, Move(point));
                    }
                });
                auto saved =
                    SaveObjectGraph(allocator, objects, objects.OwnerAccess(), holder.Handle());
                CHECK(saved);
                if (!saved) {
                    return 0;
                }

                bytes = Move(saved.Value());
            }

            return static_cast<int64>(MinimumValidateWork(allocator, objects, types, bytes.View()));
        };

        constexpr int64 Grown = 256;
        const int64 topLevel = minimum(true, Grown + 8) - minimum(true, 8);
        const int64 nested = minimum(false, Grown + 8) - minimum(false, 8);
        CHECK(topLevel - nested == Grown);
        REQUIRE(objects.Collect(objects.OwnerAccess()));
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// 값 struct Record에 없는 멤버는 원소 기본값이다(새 항목은 S(Allocator&)로 만든 뒤 그 자리에
// 해석한다). 복원은 컨테이너를 통째로 바꾼다 — 생성자가 넣어 둔 항목은 남지 않는다(필드가 Record에
// 없으면 생성자 값 그대로).
TEST(MapGraph_StructValueMissingMembersAreDefaults) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(allocator, types, {0x4d4150, 21});
        ArchiveWriter y(allocator);
        REQUIRE(y.WriteI64(9));
        const FieldBytes members[] = {{SchemaField(TypeOf<MapPoint>(), "y").id,
                                       static_cast<uint32>(WireType::I64), y.View()}};
        auto onlyY = RecordOf(allocator, TypeIdOf<MapPoint>(), 1, members);
        auto points = MapPayload(
            allocator, WireType::String, WireType::Record, 1, [&](ArchiveWriter& out) noexcept {
                return out.WriteString("k").IsOk() && out.WriteBytes(onlyY.View()).IsOk();
            });
        const FieldBytes fields[] = {
            {HolderField("points"), static_cast<uint32>(WireType::Map), points.View()}};
        auto graph = HolderGraph(allocator, fields);
        auto loaded =
            LoadObjectGraph(allocator, objects, types, objects.OwnerAccess(), graph.View());
        REQUIRE(loaded);
        With(objects, loaded.Value().Handle(), [](MapHolder& value) noexcept {
            REQUIRE(value.points.Count() == 1);
            const MapPoint* point = value.points.Find(StringView("k"));
            REQUIRE(point);
            CHECK(point->x == 0 && point->y == 9 && point->label.View() == "");
            CHECK(value.counts.IsEmpty());
        });
        loaded.Value().Reset();
        REQUIRE(objects.Collect(objects.OwnerAccess()));
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// 관찰 struct 안의 Map·Set은 관찰 임시 값의 컨테이너로 채워진다(1단계 — MapBag::points의 값 struct
// 멤버와 words). Name 원소 Set을 담은 struct(MapTagBag)와 Name 키 Map을 담은 struct(MapPerPlatform
// — A5 PerPlatformFloat 모양, Name은 키에만 있다)는 ContainsName이라 관찰할 수 없다(InvalidArgument
// — 1단계에는 Name 토큰이 없다).
TEST(MapGraph_ObservedStructsSeeMapMembers) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    NamePool names(allocator);
    REQUIRE(names.Initialize());
    {
        ObjectRegistry objects(allocator, types, {0x4d4150, 22}, WithNames(&names));
        Array<uint8> bytes(allocator);
        {
            RootRef<MapHolder> holder = New(objects);
            With(objects, holder.Handle(), [&](MapHolder& value) noexcept {
                MapPoint point(objects.ObjectAllocator());
                point.x = 13;
                Put(value.bag.points, uint32{2}, Move(point));
                REQUIRE(value.bag.words.Add(MakeString(objects.ObjectAllocator(), "w")));
                REQUIRE(value.bag.words.Add(MakeString(objects.ObjectAllocator(), "v")));
            });
            auto saved =
                SaveObjectGraph(allocator, objects, objects.OwnerAccess(), holder.Handle());
            REQUIRE(saved);
            bytes = Move(saved.Value());
        }

        const auto validate = [&](FieldCodecHooks& hooks) noexcept {
            ObjectGraphLoadOptions options;
            options.hooks = &hooks;
            ObjectGraphStage stage(allocator, objects, types, objects.OwnerAccess(), options);
            ObjectGraphInput input;
            input.bytes = bytes.View();
            return ErrorOf(stage.Validate(input));
        };

        BagObserver bags(TypeIdOf<MapBag>());
        CHECK(validate(bags) == ErrorCode::None);
        CHECK(bags.observed >= 1 && bags.pointX == 13 && bags.words == 2);
        auto tagBag = types.AcquireValueType(TypeIdOf<MapTagBag>(), TypeKind::Struct);
        REQUIRE(tagBag);
        CHECK(tagBag.Value().Struct().ContainsName());
        BagObserver tags(TypeIdOf<MapTagBag>());
        CHECK(validate(tags) == ErrorCode::InvalidArgument);
        CHECK(tags.observed == 0);
        auto perPlatform = types.AcquireValueType(TypeIdOf<MapPerPlatform>(), TypeKind::Struct);
        REQUIRE(perPlatform);
        CHECK(perPlatform.Value().Struct().ContainsName());
        BagObserver platforms(TypeIdOf<MapPerPlatform>());
        CHECK(validate(platforms) == ErrorCode::InvalidArgument);
        CHECK(platforms.observed == 0);
        REQUIRE(objects.Collect(objects.OwnerAccess()));
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(names.Shutdown());
    CHECK(types.Shutdown());
}

// Map 값 자리의 옛 버전 Record(MapVersioned 버전 1)는 그 Record byte로 struct 마이그레이션을 거쳐
// 현재 버전으로 해석된다(Optional 내부와 같은 규칙 — 원본은 값 Record 내용). 마이그레이션이 없으면
// Unsupported다.
TEST(MapGraph_ValueRecordMigrationUsesTheValueBytes) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(allocator, types, {0x4d4150, 26});
        ArchiveWriter value(allocator);
        REQUIRE(value.WriteI64(5));
        const FieldBytes members[] = {{SchemaField(TypeOf<MapVersioned>(), "value").id,
                                       static_cast<uint32>(WireType::I64), value.View()}};
        auto old = RecordOf(allocator, TypeIdOf<MapVersioned>(), 1, members);
        auto payload = MapPayload(
            allocator, WireType::U32, WireType::Record, 1, [&](ArchiveWriter& out) noexcept {
                return out.WriteU32(3).IsOk() && out.WriteBytes(old.View()).IsOk();
            });
        const FieldBytes fields[] = {
            {HolderField("versioned"), static_cast<uint32>(WireType::Map), payload.View()}};
        auto graph = HolderGraph(allocator, fields);
        usize received = 0;
        ObjectGraphLoadOptions options;
        options.migrateStruct = &MigrateVersioned;
        options.migrationUser = &received;
        auto loaded = LoadObjectGraph(allocator, objects, types, objects.OwnerAccess(),
                                      graph.View(), options);
        REQUIRE(loaded);
        CHECK(received == old.Size());
        With(objects, loaded.Value().Handle(), [](MapHolder& holder) noexcept {
            const MapVersioned* migrated = holder.versioned.Find(uint32{3});
            REQUIRE(migrated);
            CHECK(migrated->value == 50 && migrated->extra == 1);
        });

        CHECK(LoadCode(allocator, objects, types, graph.View()) == ErrorCode::Unsupported);
        loaded.Value().Reset();
        REQUIRE(objects.Collect(objects.OwnerAccess()));
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// 1단계 중복 색인·관찰 컨테이너의 예약은 키 값 영역으로 줄인다(KeyDomainLimit): Set<bool>에 count
// 100만을 적은 입력(약 1 MB)은 2칸만 예약하고 셋째 원소에서 중복으로 InvalidArgument다. count만큼
// 예약했다면 색인 수십 MB의 작업량 청구로 이 작업량 한도(16M)에서 CapacityExceeded가 났다.
TEST(MapGraph_KeyDomainLimitsTheDuplicateIndexReservation) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(allocator, types, {0x4d4150, 27});
        constexpr uint32 Count = 1000000;
        auto flags = SetPayload(allocator, WireType::Bool, Count, [](ArchiveWriter& out) noexcept {
            for (uint32 index = 0; index < Count; ++index) {
                if (!out.WriteBool(index % 2 == 0)) {
                    return false;
                }
            }

            return true;
        });
        const FieldBytes fields[] = {
            {HolderField("flags"), static_cast<uint32>(WireType::Set), flags.View()}};
        auto graph = HolderGraph(allocator, fields);
        ObjectGraphLoadOptions options;
        options.limits.maxWork = 16 * 1024 * 1024;
        CHECK(LoadCode(allocator, objects, types, graph.View(), options) == ErrorCode::CorruptData);

        // 영역 안의 서로 다른 원소는 그대로 왕복한다.
        auto two = SetPayload(allocator, WireType::Bool, 2, [](ArchiveWriter& out) noexcept {
            return out.WriteBool(true).IsOk() && out.WriteBool(false).IsOk();
        });
        const FieldBytes valid[] = {
            {HolderField("flags"), static_cast<uint32>(WireType::Set), two.View()}};
        auto small = HolderGraph(allocator, valid);
        auto loaded = LoadObjectGraph(allocator, objects, types, objects.OwnerAccess(),
                                      small.View(), options);
        REQUIRE(loaded);
        With(objects, loaded.Value().Handle(), [](MapHolder& holder) noexcept {
            REQUIRE(holder.flags.Count() == 2);
            CHECK(*holder.flags.begin() == true);
        });
        loaded.Value().Reset();
        REQUIRE(objects.Collect(objects.OwnerAccess()));
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// 2단계(적용)는 할당만 실패한다: 객체 allocator의 모든 할당 지점에서 실패시키면 ApplyFields가
// OutOfMemory이고 트랜잭션을 버리면 객체가 남지 않는다(String 키·값, 값 struct label, Set 원소,
// 컨테이너 저장소).
TEST(MapGraph_ApplyAllocationFailureRollsBack) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    Array<uint8> bytes(allocator);
    {
        ObjectRegistry source(allocator, types, {0x4d4150, 23});
        RootRef<MapHolder> holder = New(source);
        Allocator& objects = source.ObjectAllocator();
        With(source, holder.Handle(), [&](MapHolder& value) noexcept {
            Put(value.ratios, MakeString(objects, "allocated key"), 1.0f);
            Put(value.colorNames, MapColor::Green, MakeString(objects, "allocated value"));
            MapPoint point(objects);
            REQUIRE(point.label.TryAssign("allocated label"));
            Put(value.points, MakeString(objects, "point"), Move(point));
            REQUIRE(value.words.Add(MakeString(objects, "allocated element")));
        });

        auto saved = SaveObjectGraph(allocator, source, source.OwnerAccess(), holder.Handle());
        REQUIRE(saved);
        bytes = Move(saved.Value());
        holder.Reset();
        REQUIRE(source.Shutdown(source.OwnerAccess()));
    }

    // 객체 allocator만 실패시킨다(Stage의 임시 할당은 따로 둔 allocator다).
    auto apply = [&](test::NFailAllocator& objectAllocator, uint64 failure,
                     uint64* attempts) noexcept -> ErrorCode {
        ObjectRegistry objects(objectAllocator, types, {0x4d4150, 24});
        ErrorCode result = ErrorCode::None;
        {
            ObjectGraphStage stage(allocator, objects, types, objects.OwnerAccess());
            ConstructionContext transaction(objects, objects.OwnerAccess());
            ObjectGraphInput input;
            input.bytes = bytes.View();
            const bool prepared =
                stage.Validate(input).IsOk() && stage.CreateObjects(transaction).IsOk();
            CHECK(prepared);
            if (!prepared) {
                transaction.Rollback();
                result = ErrorCode::DependencyFailed;
            } else {
                objectAllocator.FailOnAttempt(failure);
                auto applied = stage.ApplyFields(transaction);
                if (attempts) {
                    *attempts = objectAllocator.Attempts();
                }

                objectAllocator.DisableFailure();
                result = applied ? ErrorCode::None : applied.Error();
                if (applied) {
                    CHECK(transaction.Commit());
                } else {
                    transaction.Rollback();
                }
            }
        }

        if (result != ErrorCode::None) {
            CHECK(objects.Stats().liveObjects == 0 && objects.Stats().reservedObjects == 0);
        }

        CHECK(objects.Collect(objects.OwnerAccess()));
        CHECK(objects.Shutdown(objects.OwnerAccess()));
        return result;
    };

    test::NFailAllocator objectAllocator(allocator);
    uint64 attempts = 0;
    REQUIRE(apply(objectAllocator, 0, &attempts) == ErrorCode::None);
    // 적어도 컨테이너 저장소 넷과 String 키·값·label·원소의 할당이 있다.
    CHECK(attempts >= 8);
    for (uint64 index = 1; index <= attempts; ++index) {
        CHECK(apply(objectAllocator, index, nullptr) == ErrorCode::OutOfMemory);
    }

    CHECK(types.Shutdown());
}

// Map·Set 필드는 aggregate다(리플렉션 B9f2 — 필드 통째로 저장·복원하고 항목은 key 단계 경로
// 연산 — 리플렉션 B9f3, MapPathTests.cpp): 경로 통째 읽기는 Unsupported(AggregateValue), index
// 단계는 TypeMismatch(NotArray), 등록된 write(FieldWriteOperation)는 함수표 검사 뒤 Unsupported다.
// 함수표에는 read가 없고 경로 thunk가 있다. key 없이 struct 멤버 Map 아래로 들어가는 경로는
// struct가 아니므로 TypeMismatch(NotStruct)다.
TEST(MapField_PathAndFieldValueTreatMapsAsAggregates) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(allocator, types, {0x4d4150, 25});
        RootRef<MapHolder> holder = New(objects);
        FieldValue out(allocator);
        PropertyPathFailure failure;
        const PathStep counts[] = {{HolderField("counts")}};
        CHECK(ErrorOf(PathRead(objects, objects.OwnerAccess(), holder.Handle(), {counts}, out,
                               &failure)) == ErrorCode::Unsupported);
        CHECK(failure.reason == PropertyPathError::AggregateValue);
        const PathStep words[] = {{HolderField("words"), 0, PathSelector::Index}};
        CHECK(ErrorOf(PathRead(objects, objects.OwnerAccess(), holder.Handle(), {words}, out,
                               &failure)) == ErrorCode::TypeMismatch);
        CHECK(failure.reason == PropertyPathError::NotArray);
        const PathStep bag[] = {{HolderField("bag")},
                                {SchemaField(TypeOf<MapBag>(), "points").id},
                                {SchemaField(TypeOf<MapPoint>(), "x").id}};
        CHECK(ErrorOf(PathRead(objects, objects.OwnerAccess(), holder.Handle(), {bag}, out,
                               &failure)) == ErrorCode::TypeMismatch);
        CHECK(failure.reason == PropertyPathError::NotStruct);

        auto lease = types.Acquire(TypeIdOf<MapHolder>());
        REQUIRE(lease);
        const usize index = lease.Value().FieldIndex(HolderField("counts"));
        REQUIRE(index != TypeLease::NoField);
        const RuntimeFieldOps* ops = lease.Value().FieldOps(index);
        REQUIRE(ops);
        CHECK(!ops->read && ops->path);
        FieldValue value(allocator);
        value.kind = ValueKind::Map;
        CHECK(ErrorOf(ops->write(objects, objects.OwnerAccess(), holder.Handle(), value)) ==
              ErrorCode::Unsupported);
        holder.Reset();
        REQUIRE(objects.Collect(objects.OwnerAccess()));
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}
} // namespace map_graph_load_tests_file
