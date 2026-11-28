#include <Core/Memory/SystemAllocator.h>
#include <Object/GeneratedFieldCodec.h>
#include <ObjectSerialization/ObjectGraph.h>
#include <TestSupport/Test.h>
#include "GraphTestSupport.h"

// graph v2 형식 손상 시험이다(Docs/Design/ObjectFoundations.ko.md 5.1·5.7절, 9절 Od1 "형식 손상"):
// formatVersion·flags·개수·길이·NodeTable·Record schema·참조 값 경계·남는 byte. 모든 거부는 생성자
// 전이고 객체를 남기지 않는다.
using namespace rocks;
namespace graph_format_tests_file {

namespace {

constexpr TypeId CellType{0x4746524d54ULL, 1};
constexpr TypeId OtherType{0x4746524d54ULL, 2};
constexpr Guid ValueId{0x4746524d46ULL, 1};
constexpr Guid NextId{0x4746524d46ULL, 2};

uint32 ConstructedCells = 0;

struct Cell final : Object {
    explicit Cell(Allocator&) noexcept {
        ++ConstructedCells;
    }

    void Trace(TraceVisitor& visitor) const noexcept {
        visitor.Visit(next);
    }

    uint32 value = 0;
    ObjectRef<Cell> next;
};

template <auto Member>
RuntimeFieldOps CellField(Guid id) noexcept {
    return {id,
            [](ObjectRegistry& registry, const ObjectAccessContext& access, ObjectHandle object,
               FieldValue& value) noexcept {
                return detail::ReadGeneratedField<Cell>(registry, access, object, Member, value);
            },
            [](ObjectRegistry& registry, const ObjectAccessContext& access, ObjectHandle object,
               const FieldValue& value) noexcept {
                return detail::WriteGeneratedField<Cell>(registry, access, object, Member, value);
            },
            [](ConstructionContext& transaction, ObjectHandle object,
               const FieldValue& value) noexcept {
                return detail::WriteGeneratedStagedField<Cell>(transaction, object, Member, value);
            },
            &detail::EncodeThroughFieldValue,
            &detail::DecodeThroughFieldValue};
}

// Cell과, 같은 native의 별명 타입 Other를 등록한다(Record schema 불일치 시험).
bool RegisterTypes(TypeRegistry& types) noexcept {
    FieldSchema fields[2];
    fields[0].id = ValueId;
    fields[0].name = {"value", 5};
    fields[0].value.kind = ValueKind::UInt32;
    fields[1].id = NextId;
    fields[1].name = {"next", 4};
    fields[1].kind = FieldKind::ObjectReference;
    fields[1].value = {ValueKind::ObjectReference, CellType};
    const RuntimeFieldOps ops[] = {CellField<&Cell::value>(ValueId),
                                   CellField<&Cell::next>(NextId)};
    auto cell = MakeRuntimeTypeOps<Cell>();
    cell.fields = ops;
    cell.completeFieldLayout = true;
    auto other = cell;
    return types.Register({CellType, 1, fields, {"Cell", 4}}, Move(cell)).IsOk() &&
           types.Register({OtherType, 1, fields, {"Other", 5}}, Move(other)).IsOk();
}

// next 참조 값 하나(U32)를 담은 Cell Record다.
Result<Array<uint8>> CellRecord(Allocator& allocator, uint32 next, TypeId schema = CellType,
                                uint32 version = 1) noexcept {
    ArchiveWriter payload(allocator);
    if (auto written = payload.WriteU32(next); !written) {
        return Result<Array<uint8>>::Failure(written.Error());
    }

    RecordBuilder record(allocator, schema, version);
    if (auto added = record.AddField(NextId, WireType::U32, payload.View()); !added) {
        return Result<Array<uint8>>::Failure(added.Error());
    }

    return record.Build();
}

// 로드가 error로 실패하고 생성자가 실행되지 않았으며 객체가 남지 않았는지 본다.
bool Rejects(Allocator& allocator, ObjectRegistry& registry, TypeRegistry& types,
             Span<const uint8> bytes, ErrorCode error,
             const ObjectGraphLoadOptions& options = {}) noexcept {
    const uint32 constructed = ConstructedCells;
    auto loaded =
        LoadObjectGraph(allocator, registry, types, registry.OwnerAccess(), bytes, options);
    return !loaded && loaded.Error() == error && ConstructedCells == constructed &&
           registry.Stats().liveObjects == 0 && registry.Stats().reservedObjects == 0;
}

} // namespace

// header: formatVersion이 3이 아니면 Unsupported(이전 형식은 읽지 않는다 — 하위호환 없음), 정의되지
// 않은 flags·NodeTable 없는 BaseOuter·NodeTable 없음(편의 복원)·nodeCount 0은 InvalidArgument,
// nodeCount·externalCount 상한 초과는 CapacityExceeded, externalCount가 imports 수(여기서는 0)와
// 다르면 InvalidArgument다.
TEST(GraphFormat_HeaderVersionFlagsAndCounts) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry target(allocator, types, {0x474631, 1});
        {
            auto record = CellRecord(allocator, 0);
            REQUIRE(record);
            auto valid = test::graph::Single(allocator, CellType, record.Value().View());
            REQUIRE(valid);
            {
                auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                              valid.Value().View());
                REQUIRE(loaded);
            }

            REQUIRE(target.Collect(target.OwnerAccess()));
            struct Case {
                usize offset;
                uint32 value;
                ErrorCode error;
            };

            const Case cases[] = {
                {test::graph::VersionOffset, 1, ErrorCode::Unsupported},
                {test::graph::VersionOffset, 2, ErrorCode::Unsupported},
                {test::graph::VersionOffset, 4, ErrorCode::Unsupported},
                {test::graph::FlagsOffset, 4, ErrorCode::CorruptData},
                {test::graph::FlagsOffset, 1 | 8, ErrorCode::CorruptData},
                {test::graph::FlagsOffset, test::graph::BaseOuterFlag, ErrorCode::CorruptData},
                {test::graph::NodeCountOffset, 0, ErrorCode::CorruptData},
                {test::graph::NodeCountOffset, 4097, ErrorCode::CapacityExceeded},
                // 노드 수에 비해 짧은 입력은 할당 전에 InvalidArgument다.
                {test::graph::NodeCountOffset, 2, ErrorCode::CorruptData},
                {test::graph::NodeCountOffset, 4096, ErrorCode::CorruptData},
                {test::graph::ExternalCountOffset, 65537, ErrorCode::CapacityExceeded},
                {test::graph::ExternalCountOffset, 1, ErrorCode::InvalidArgument}};
            for (const Case& example : cases) {
                Array<uint8> bytes(allocator);
                REQUIRE(bytes.TryAppend(valid.Value().View()));
                test::graph::PatchU32(bytes, example.offset, example.value);
                CHECK(Rejects(allocator, target, types, bytes.View(), example.error));
            }

            // NodeTable 없는 그래프는 복원 호출자가 노드 표를 줘야 한다(편의 복원은 거부).
            const test::graph::Node bare{CellType, 0, {}, record.Value().View()};
            test::graph::Header header;
            header.flags = 0;
            auto noTable =
                test::graph::Build(allocator, Span<const test::graph::Node>(&bare, 1), header);
            REQUIRE(noTable);
            CHECK(Rejects(allocator, target, types, noTable.Value().View(),
                          ErrorCode::InvalidArgument));

            // magic이 다르면 InvalidArgument, header보다 짧으면 InvalidArgument다.
            Array<uint8> magic(allocator);
            REQUIRE(magic.TryAppend(valid.Value().View()));
            magic[0] = 'X';
            CHECK(Rejects(allocator, target, types, magic.View(), ErrorCode::CorruptData));
            CHECK(Rejects(allocator, target, types, Span<const uint8>(valid.Value().Data(), 20),
                          ErrorCode::CorruptData));
            // maxObjects는 local ID가 외부 참조 비트와 겹치지 않게 2^31 − 1 이하다(옵션 오류).
            ObjectGraphLoadOptions huge;
            huge.limits.maxObjects = test::graph::ExternalBit;
            CHECK(Rejects(allocator, target, types, valid.Value().View(),
                          ErrorCode::InvalidArgument, huge));
        }

        REQUIRE(target.Shutdown(target.OwnerAccess()));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// NodeTable·Record: 노드 타입 0은 InvalidArgument, 미등록 NotFound, Record schema가 노드 타입과
// 다르면 InvalidArgument(같은 native 별명이어도), Record 길이가 남은 byte를 넘거나 남는 byte가
// 있으면 InvalidArgument다.
TEST(GraphFormat_NodeTableAndRecordBoundaries) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry target(allocator, types, {0x474632, 1});
        {
            auto record = CellRecord(allocator, 0);
            REQUIRE(record);
            auto zero = test::graph::Single(allocator, TypeId{}, record.Value().View());
            REQUIRE(zero);
            CHECK(Rejects(allocator, target, types, zero.Value().View(), ErrorCode::CorruptData));
            auto unknown = test::graph::Single(allocator, TypeId{0x4746, 9}, record.Value().View());
            REQUIRE(unknown);
            CHECK(Rejects(allocator, target, types, unknown.Value().View(), ErrorCode::NotFound));
            auto alias = test::graph::Single(allocator, OtherType, record.Value().View());
            REQUIRE(alias);
            CHECK(Rejects(allocator, target, types, alias.Value().View(), ErrorCode::CorruptData));
            // 버전은 Record header가 정한다: 높은 버전은 Unsupported, 0은 InvalidArgument다.
            auto newer = CellRecord(allocator, 0, CellType, 2);
            REQUIRE(newer);
            auto newerGraph = test::graph::Single(allocator, CellType, newer.Value().View());
            REQUIRE(newerGraph);
            CHECK(Rejects(allocator, target, types, newerGraph.Value().View(),
                          ErrorCode::Unsupported));
            // Record header의 버전 칸(magic 8 + envelope 4 + schema 16 뒤)을 0으로 바꾼다.
            Array<uint8> zeroVersion(allocator);
            REQUIRE(zeroVersion.TryAppend(record.Value().View()));
            test::graph::PatchU32(zeroVersion, 28, 0);
            auto zeroGraph = test::graph::Single(allocator, CellType, zeroVersion.View());
            REQUIRE(zeroGraph);
            CHECK(Rejects(allocator, target, types, zeroGraph.Value().View(),
                          ErrorCode::CorruptData));

            auto valid = test::graph::Single(allocator, CellType, record.Value().View());
            REQUIRE(valid);
            // Record 길이 prefix는 NodeTable 항목(종류 1 + Guid 16 + outer 4 + 빈 이름 8) 뒤다.
            const usize lengthAt = test::graph::HeaderBytes + 29;
            Array<uint8> longer(allocator);
            REQUIRE(longer.TryAppend(valid.Value().View()));
            test::graph::PatchU32(longer, lengthAt, static_cast<uint32>(record.Value().Size() + 1));
            CHECK(Rejects(allocator, target, types, longer.View(), ErrorCode::CorruptData));
            Array<uint8> shorter(allocator);
            REQUIRE(shorter.TryAppend(valid.Value().View()));
            test::graph::PatchU32(shorter, lengthAt,
                                  static_cast<uint32>(record.Value().Size() - 1));
            CHECK(Rejects(allocator, target, types, shorter.View(), ErrorCode::CorruptData));
            Array<uint8> trailing(allocator);
            REQUIRE(trailing.TryAppend(valid.Value().View()));
            REQUIRE(trailing.TryPushBack(uint8{0}));
            CHECK(Rejects(allocator, target, types, trailing.View(), ErrorCode::CorruptData));
            // 이름이 잘못된 UTF-8이면 InvalidArgument다(이름 byte는 header + 종류 1 + Guid 16 +
            // outer 4 + 길이 8 뒤다 — writer는 잘못된 UTF-8을 쓰지 않으므로 쓴 뒤 바꾼다).
            const test::graph::Node named{CellType, 0, "x", record.Value().View()};
            auto badUtf8 = test::graph::Build(allocator, Span<const test::graph::Node>(&named, 1));
            REQUIRE(badUtf8);
            badUtf8.Value()[test::graph::HeaderBytes + 29] = 0xff;
            CHECK(
                Rejects(allocator, target, types, badUtf8.Value().View(), ErrorCode::CorruptData));
        }

        REQUIRE(target.Shutdown(target.OwnerAccess()));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 참조 값(U32) 경계: 0 = null, 1..nodeCount = 노드, nodeCount + 1·0x80000000·외부
// 비트(externalCount 0이므로 모두 범위 밖)는 InvalidArgument다. 옛 U64 wire는 TypeMismatch다.
TEST(GraphFormat_ReferenceValueBoundaries) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry target(allocator, types, {0x474633, 1});
        {
            auto tail = CellRecord(allocator, 0);
            REQUIRE(tail);
            const uint32 accepted[] = {0, 1, 2};
            for (uint32 value : accepted) {
                auto head = CellRecord(allocator, value);
                REQUIRE(head);
                const test::graph::Node nodes[] = {{CellType, 0, {}, head.Value().View()},
                                                   {CellType, 0, {}, tail.Value().View()}};
                auto graph = test::graph::Build(allocator, nodes);
                REQUIRE(graph);
                auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                              graph.Value().View());
                // 2가 아니면 노드 2에 닿지 않으므로 도달 검사가 거부한다.
                CHECK(loaded.IsOk() == (value == 2));
                CHECK(loaded || loaded.Error() == ErrorCode::CorruptData);
            }

            REQUIRE(target.Collect(target.OwnerAccess()));
            const uint32 rejected[] = {3, test::graph::ExternalBit, test::graph::ExternalBit | 1,
                                       test::graph::ExternalBit | 2, NumericLimits<uint32>::Max()};
            for (uint32 value : rejected) {
                auto head = CellRecord(allocator, value);
                REQUIRE(head);
                const test::graph::Node nodes[] = {{CellType, 0, {}, head.Value().View()},
                                                   {CellType, 0, {}, tail.Value().View()}};
                auto graph = test::graph::Build(allocator, nodes);
                REQUIRE(graph);
                CHECK(Rejects(allocator, target, types, graph.Value().View(),
                              ErrorCode::CorruptData));
            }

            ArchiveWriter wide(allocator);
            REQUIRE(wide.WriteU64(1));
            RecordBuilder record(allocator, CellType, 1);
            REQUIRE(record.AddField(NextId, WireType::U64, wide.View()));
            auto oldWire = record.Build();
            REQUIRE(oldWire);
            auto graph = test::graph::Single(allocator, CellType, oldWire.Value().View());
            REQUIRE(graph);
            CHECK(Rejects(allocator, target, types, graph.Value().View(), ErrorCode::TypeMismatch));
        }

        REQUIRE(target.Shutdown(target.OwnerAccess()));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 저장이 쓰는 byte가 명세와 같다: header 24 byte(formatVersion 3, flags, nodeCount, externalCount
// 0), NodeTable 항목, Bytes(Record)이고 참조는 U32 local ID다.
TEST(GraphFormat_SavedBytesFollowTheSpecification) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry source(allocator, types, {0x474634, 1});
        {
            auto first = source.NewObject<Cell>(source.OwnerAccess(), CellType, {}, "First");
            auto second = source.NewObject<Cell>(source.OwnerAccess(), CellType);
            REQUIRE(first && second);
            {
                auto borrowed = source.Resolve<Cell>(source.OwnerAccess(), first.Value().Handle());
                REQUIRE(borrowed);
                auto cell = borrowed.Value().Get();
                REQUIRE(cell);
                cell.Value()->value = 5;
                REQUIRE(source.SetReference(source.OwnerAccess(), first.Value().Handle(),
                                            cell.Value()->next, second.Value().Handle()));
            }

            auto saved =
                SaveObjectGraph(allocator, source, source.OwnerAccess(), first.Value().Handle());
            REQUIRE(saved);
            // 손으로 조립한 같은 그래프와 byte가 같다(Record는 필드 순서 value, next).
            ArchiveWriter five(allocator);
            REQUIRE(five.WriteU32(5));
            ArchiveWriter two(allocator);
            REQUIRE(two.WriteU32(2));
            ArchiveWriter zero(allocator);
            REQUIRE(zero.WriteU32(0));
            RecordBuilder head(allocator, CellType, 1);
            REQUIRE(head.AddField(ValueId, WireType::U32, five.View()));
            REQUIRE(head.AddField(NextId, WireType::U32, two.View()));
            RecordBuilder tail(allocator, CellType, 1);
            REQUIRE(tail.AddField(ValueId, WireType::U32, zero.View()));
            REQUIRE(tail.AddField(NextId, WireType::U32, zero.View()));
            auto headBytes = head.Build();
            auto tailBytes = tail.Build();
            REQUIRE(headBytes && tailBytes);
            const test::graph::Node nodes[] = {{CellType, 0, "First", headBytes.Value().View()},
                                               {CellType, 0, {}, tailBytes.Value().View()}};
            auto expected = test::graph::Build(allocator, nodes);
            REQUIRE(expected);
            CHECK(test::graph::SameBytes(saved.Value().View(), expected.Value().View()));
            CHECK(test::graph::ReadU32(saved.Value().View(), test::graph::VersionOffset) == 3);
            CHECK(test::graph::ReadU32(saved.Value().View(), test::graph::ExternalCountOffset) ==
                  0);
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}
} // namespace graph_format_tests_file
