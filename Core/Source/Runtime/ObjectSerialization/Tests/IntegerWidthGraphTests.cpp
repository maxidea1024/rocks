#include <ReflectionTestFixtures/IntegerWidthFixture.generated.h>
#include <Core/Memory/SystemAllocator.h>
#include <Object/ConstructionContext.h>
#include <Object/PropertyPath.h>
#include <ObjectSerialization/ObjectGraph.h>
#include <Platform/Fatal.h>
#include <Serialization/Archive.h>
#include <Serialization/Record.h>
#include <Serialization/ValueArray.h>
#include <TestSupport/Test.h>
#include "GraphTestSupport.h"

// 리플렉션 B9a2 정수 폭의 그래프·경로 시험이다(Docs/Design/ReflectionExtensions.ko.md 2절, 17절
// B9a 항목): MetaCompiler 생성 fixture(IntegerWidthFixture.h)로 int8·int16 클래스 필드·struct
// 멤버·배열 원소와 underlying int8·int16·uint32·int64·uint64 enum(Flags uint64 포함)의 경계값을 I64
// wire로 왕복하고 다시 저장한 byte가 같으며, 범위 밖 I64·다른 wire·선언되지 않은 enum 값의 로드를
// 객체 공개 없이 거부하고, 직접 넣은 선언되지 않은 enum 값의 저장을 거부하며, 필드·경로 쓰기가 폭을
// 검사한다.
using namespace rocks;
namespace integer_width_graph_tests_file {

using reflection_fixture::ByteOffset;
using reflection_fixture::CountLimit;
using reflection_fixture::HugeIndex;
using reflection_fixture::HugeMask;
using reflection_fixture::IntegerWidthFields;
using reflection_fixture::IntegerWidthPair;
using reflection_fixture::ShortOffset;
using reflection_fixture::WideOffset;

namespace {

ErrorCode ErrorOf(const Result<void>& result) noexcept {
    return result ? ErrorCode::None : result.Error();
}

const FieldSchema& SchemaField(const TypeSchema& schema, StringView name) noexcept {
    for (const FieldSchema& field : schema.fields) {
        if (StringView(field.name.Data(), field.name.Size()) == name) {
            return field;
        }
    }

    platform::Fatal("fixture field missing");
}

// fixture enum 여섯·struct·클래스를 쓰는 순서대로 등록한다. 기록은 Shutdown이 정리한다.
bool RegisterFixture(TypeRegistry& types) noexcept {
    return GeneratedType<ByteOffset>::Register(types).IsOk() &&
           GeneratedType<ShortOffset>::Register(types).IsOk() &&
           GeneratedType<WideOffset>::Register(types).IsOk() &&
           GeneratedType<CountLimit>::Register(types).IsOk() &&
           GeneratedType<HugeIndex>::Register(types).IsOk() &&
           GeneratedType<HugeMask>::Register(types).IsOk() &&
           GeneratedType<IntegerWidthPair>::Register(types).IsOk() &&
           GeneratedType<IntegerWidthFields>::Register(types).IsOk();
}

HugeMask Mask(HugeMask left, HugeMask right) noexcept {
    return static_cast<HugeMask>(static_cast<uint64>(left) | static_cast<uint64>(right));
}

IntegerWidthPair Pair(int8 tiny, int16 narrow, ByteOffset offset) noexcept {
    IntegerWidthPair pair;
    pair.tiny = tiny;
    pair.narrow = narrow;
    pair.offset = offset;
    return pair;
}

// 모든 필드를 경계값으로 채운다(배열은 최소·0·최대).
bool Fill(IntegerWidthFields& value) noexcept {
    value.tiny = NumericLimits<int8>::Min();
    value.narrow = NumericLimits<int16>::Max();
    value.pair = Pair(NumericLimits<int8>::Max(), NumericLimits<int16>::Min(), ByteOffset::High);
    value.byteOffset = ByteOffset::Low;
    value.shortOffset = ShortOffset::High;
    value.wideOffset = WideOffset::Low;
    value.countLimit = CountLimit::Top;
    value.hugeIndex = HugeIndex::Top;
    value.hugeMask = Mask(HugeMask::Bit0, HugeMask::Bit62);
    const int8 tinies[] = {NumericLimits<int8>::Min(), 0, NumericLimits<int8>::Max()};
    const int16 narrows[] = {NumericLimits<int16>::Min(), 1, NumericLimits<int16>::Max()};
    const WideOffset wides[] = {WideOffset::High, WideOffset::Low, WideOffset::Zero};
    for (usize index = 0; index < 3; ++index) {
        if (!value.tinies.TryPushBack(tinies[index]) ||
            !value.narrows.TryPushBack(narrows[index]) ||
            !value.wideOffsets.TryPushBack(wides[index])) {
            return false;
        }
    }

    return value.pairs.TryPushBack(Pair(-1, -2, ByteOffset::Low)).IsOk() &&
           value.pairs.TryPushBack(Pair(1, 2, ByteOffset::Zero)).IsOk();
}

bool SamePair(const IntegerWidthPair& pair, int8 tiny, int16 narrow, ByteOffset offset) noexcept {
    return pair.tiny == tiny && pair.narrow == narrow && pair.offset == offset;
}

// Fill이 넣은 값인지 본다.
void CheckFilled(const IntegerWidthFields& value) noexcept {
    CHECK(value.tiny == NumericLimits<int8>::Min());
    CHECK(value.narrow == NumericLimits<int16>::Max());
    CHECK(SamePair(value.pair, NumericLimits<int8>::Max(), NumericLimits<int16>::Min(),
                   ByteOffset::High));
    CHECK(value.byteOffset == ByteOffset::Low);
    CHECK(value.shortOffset == ShortOffset::High);
    CHECK(value.wideOffset == WideOffset::Low);
    CHECK(static_cast<int64>(value.wideOffset) == NumericLimits<int64>::Min());
    CHECK(value.countLimit == CountLimit::Top);
    CHECK(value.hugeIndex == HugeIndex::Top);
    CHECK(static_cast<uint64>(value.hugeIndex) == static_cast<uint64>(NumericLimits<int64>::Max()));
    CHECK(value.hugeMask == Mask(HugeMask::Bit0, HugeMask::Bit62));
    REQUIRE(value.tinies.Size() == 3 && value.narrows.Size() == 3 && value.wideOffsets.Size() == 3);
    CHECK(value.tinies[0] == NumericLimits<int8>::Min() && value.tinies[1] == 0 &&
          value.tinies[2] == NumericLimits<int8>::Max());
    CHECK(value.narrows[0] == NumericLimits<int16>::Min() && value.narrows[1] == 1 &&
          value.narrows[2] == NumericLimits<int16>::Max());
    CHECK(value.wideOffsets[0] == WideOffset::High && value.wideOffsets[1] == WideOffset::Low &&
          value.wideOffsets[2] == WideOffset::Zero);
    REQUIRE(value.pairs.Size() == 2);
    CHECK(SamePair(value.pairs[0], -1, -2, ByteOffset::Low));
    CHECK(SamePair(value.pairs[1], 1, 2, ByteOffset::Zero));
}

Result<RootRef<IntegerWidthFields>> NewFilled(ObjectRegistry& objects) noexcept {
    auto created = objects.NewObject<IntegerWidthFields>(objects.OwnerAccess(),
                                                         TypeIdOf<IntegerWidthFields>());
    if (!created) {
        return created;
    }

    auto borrowed =
        objects.Resolve<IntegerWidthFields>(objects.OwnerAccess(), created.Value().Handle());
    if (!borrowed) {
        return Result<RootRef<IntegerWidthFields>>::Failure(borrowed.Error());
    }

    auto value = borrowed.Value().Get();
    if (!value) {
        return Result<RootRef<IntegerWidthFields>>::Failure(value.Error());
    }

    if (!Fill(*value.Value())) {
        return Result<RootRef<IntegerWidthFields>>::Failure(ErrorCode::OutOfMemory);
    }

    return created;
}

// I64 값 하나의 payload다.
Array<uint8> I64Payload(Allocator& allocator, int64 value) noexcept {
    ArchiveWriter payload(allocator);
    if (!payload.WriteI64(value)) {
        platform::Fatal("test payload write failed");
    }

    return payload.TakeBytes();
}

// I64 원소 ValueArray payload다.
Array<uint8> I64ArrayPayload(Allocator& allocator, Span<const int64> values) noexcept {
    ArchiveWriter payload(allocator);
    auto writer = ValueArrayWriter::Begin(
        payload, WireType::I64, static_cast<uint32>(values.Size()), NumericLimits<usize>::Max());
    if (!writer) {
        platform::Fatal("test value array begin failed");
    }

    for (int64 value : values) {
        if (!payload.WriteI64(value)) {
            platform::Fatal("test value array write failed");
        }
    }

    if (!writer.Value().End()) {
        platform::Fatal("test value array end failed");
    }

    return payload.TakeBytes();
}

// 필드 field 하나만 담은 IntegerWidthFields Record의 단일 노드 그래프를 로드한 결과다. 실패하면
// 객체가 공개·예약되지 않았는지 본다.
ErrorCode LoadSingleField(Allocator& allocator, TypeRegistry& types, ObjectRegistry& target,
                          StringView field, WireType wire, Span<const uint8> payload) noexcept {
    RecordBuilder record(allocator, TypeIdOf<IntegerWidthFields>(), 1);
    if (!record.AddField(SchemaField(TypeOf<IntegerWidthFields>(), field).id,
                         static_cast<uint32>(wire), payload)) {
        platform::Fatal("test record field add failed");
    }

    auto recordBytes = record.Build();
    if (!recordBytes) {
        platform::Fatal("test record build failed");
    }

    auto graph =
        test::graph::Single(allocator, TypeIdOf<IntegerWidthFields>(), recordBytes.Value().View());
    if (!graph) {
        platform::Fatal("test graph build failed");
    }

    const auto before = target.Stats().liveObjects;
    auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                  graph.Value().View(), ObjectGraphLoadOptions{});
    if (!loaded) {
        CHECK(target.Stats().liveObjects == before);
        CHECK(target.Stats().reservedObjects == 0);
        return loaded.Error();
    }

    return ErrorCode::None;
}

FieldValue Signed(Allocator& allocator, ValueKind kind, int64 value) noexcept {
    FieldValue result(allocator);
    result.kind = kind;
    result.signedInteger = value;
    return result;
}

FieldValue EnumValue(Allocator& allocator, TypeId type, int64 value) noexcept {
    FieldValue result(allocator);
    result.kind = ValueKind::Enum;
    result.type = type;
    result.signedInteger = value;
    return result;
}

} // namespace

// 경계값이 I64 wire로 왕복하고(int64 최솟값·uint64 enum의 int64 최댓값·Flags 비트 62 포함) 다시
// 저장한 byte가 같다.
TEST(IntegerWidthGraph_RoundTripKeepsBoundaryValuesAndResavesIdentically) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry source(allocator, types, {0x4957, 1});
        ObjectRegistry target(allocator, types, {0x4957, 2});
        {
            auto created = NewFilled(source);
            REQUIRE(created);
            auto saved =
                SaveObjectGraph(allocator, source, source.OwnerAccess(), created.Value().Handle());
            REQUIRE(saved);
            auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                          saved.Value().View());
            REQUIRE(loaded);
            {
                auto borrowed = target.Resolve<IntegerWidthFields>(target.OwnerAccess(),
                                                                   loaded.Value().Handle());
                REQUIRE(borrowed);
                auto value = borrowed.Value().Get();
                REQUIRE(value);
                CheckFilled(*value.Value());
            }

            auto resaved =
                SaveObjectGraph(allocator, target, target.OwnerAccess(), loaded.Value().Handle());
            REQUIRE(resaved);
            CHECK(test::graph::SameBytes(saved.Value().View(), resaved.Value().View()));
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// 범위 밖 I64(단일 필드·배열 원소·struct 멤버)는 Overflow, 다른 wire는 TypeMismatch, underlying
// 범위 밖·선언되지 않은 enum 값은 InvalidArgument로 객체를 공개하지 않고 거부한다. 경계값은 받는다.
TEST(IntegerWidthGraph_LoadRejectsOutOfRangeValues) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry target(allocator, types, {0x4957, 3});
        struct Case {
            StringView field;
            int64 value;
            ErrorCode expected;
        };

        const Case cases[] = {
            {"tiny", int64{NumericLimits<int8>::Max()} + 1, ErrorCode::Overflow},
            {"tiny", int64{NumericLimits<int8>::Min()} - 1, ErrorCode::Overflow},
            {"tiny", NumericLimits<int64>::Max(), ErrorCode::Overflow},
            {"narrow", int64{NumericLimits<int16>::Max()} + 1, ErrorCode::Overflow},
            {"narrow", int64{NumericLimits<int16>::Min()} - 1, ErrorCode::Overflow},
            {"byteOffset", int64{NumericLimits<int8>::Max()} + 1, ErrorCode::InvalidArgument},
            {"shortOffset", 1, ErrorCode::InvalidArgument},
            {"wideOffset", 1, ErrorCode::InvalidArgument},
            {"countLimit", int64{NumericLimits<uint32>::Max()} + 1, ErrorCode::InvalidArgument},
            {"countLimit", -1, ErrorCode::InvalidArgument},
            {"hugeIndex", -1, ErrorCode::InvalidArgument},
            {"hugeIndex", NumericLimits<int64>::Min(), ErrorCode::InvalidArgument},
            {"hugeMask", 2, ErrorCode::InvalidArgument},
            {"hugeMask", -1, ErrorCode::InvalidArgument},
            {"tiny", NumericLimits<int8>::Min(), ErrorCode::None},
            {"narrow", NumericLimits<int16>::Max(), ErrorCode::None},
            {"wideOffset", NumericLimits<int64>::Min(), ErrorCode::None},
            {"countLimit", NumericLimits<uint32>::Max(), ErrorCode::None},
            {"hugeIndex", NumericLimits<int64>::Max(), ErrorCode::None},
            {"hugeMask", (int64{1} << 62) | 1, ErrorCode::None}};
        for (const Case& example : cases) {
            auto payload = I64Payload(allocator, example.value);
            CHECK(LoadSingleField(allocator, types, target, example.field, WireType::I64,
                                  payload.View()) == example.expected);
        }

        // 부호 있는 정수는 I64만 받는다(폭이 좁아도 U32가 아니다).
        ArchiveWriter u32(allocator);
        REQUIRE(u32.WriteU32(1));
        auto u32Bytes = u32.TakeBytes();
        CHECK(LoadSingleField(allocator, types, target, "tiny", WireType::U32, u32Bytes.View()) ==
              ErrorCode::TypeMismatch);
        CHECK(LoadSingleField(allocator, types, target, "narrow", WireType::U32, u32Bytes.View()) ==
              ErrorCode::TypeMismatch);

        // 배열 원소: 범위 밖 원소 하나가 배열 전체를 거부한다.
        const int64 tinyElements[] = {0, int64{NumericLimits<int8>::Max()} + 1};
        auto tinyArray = I64ArrayPayload(allocator, Span<const int64>(tinyElements, 2));
        CHECK(LoadSingleField(allocator, types, target, "tinies", WireType::ValueArray,
                              tinyArray.View()) == ErrorCode::Overflow);
        const int64 narrowElements[] = {int64{NumericLimits<int16>::Min()} - 1};
        auto narrowArray = I64ArrayPayload(allocator, Span<const int64>(narrowElements, 1));
        CHECK(LoadSingleField(allocator, types, target, "narrows", WireType::ValueArray,
                              narrowArray.View()) == ErrorCode::Overflow);
        const int64 wideElements[] = {NumericLimits<int64>::Min(), 1};
        auto wideArray = I64ArrayPayload(allocator, Span<const int64>(wideElements, 2));
        CHECK(LoadSingleField(allocator, types, target, "wideOffsets", WireType::ValueArray,
                              wideArray.View()) == ErrorCode::InvalidArgument);

        // struct 멤버(중첩 Record).
        auto member = I64Payload(allocator, int64{NumericLimits<int8>::Max()} + 1);
        RecordBuilder pair(allocator, GeneratedType<IntegerWidthPair>::Id(), 1);
        REQUIRE(pair.AddField(SchemaField(GeneratedType<IntegerWidthPair>::Schema(), "tiny").id,
                              WireType::I64, member.View()));
        auto pairBytes = pair.Build();
        REQUIRE(pairBytes);
        CHECK(LoadSingleField(allocator, types, target, "pair", WireType::Record,
                              pairBytes.Value().View()) == ErrorCode::Overflow);

        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// C++에서 직접 넣은 선언되지 않은 enum 값(int8 범위 안의 미선언 값, int64로 음수가 되는 uint64 값,
// 선언되지 않은 Flags 비트)은 저장하지 않는다.
TEST(IntegerWidthGraph_SaveRejectsUndeclaredWideEnums) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(allocator, types, {0x4957, 4});
        {
            using Mutation = void (*)(IntegerWidthFields&) noexcept;
            const Mutation mutations[] = {
                [](IntegerWidthFields& value) noexcept {
                    value.byteOffset = static_cast<ByteOffset>(5);
                },
                [](IntegerWidthFields& value) noexcept {
                    value.hugeIndex = static_cast<HugeIndex>(uint64{1} << 63);
                },
                [](IntegerWidthFields& value) noexcept {
                    value.hugeMask = static_cast<HugeMask>(uint64{1} << 63);
                },
                [](IntegerWidthFields& value) noexcept {
                    if (!value.wideOffsets.TryPushBack(static_cast<WideOffset>(7))) {
                        platform::Fatal("test array push failed");
                    }
                }};
            for (Mutation mutate : mutations) {
                auto created = NewFilled(objects);
                REQUIRE(created);
                {
                    auto borrowed = objects.Resolve<IntegerWidthFields>(objects.OwnerAccess(),
                                                                        created.Value().Handle());
                    REQUIRE(borrowed);
                    auto value = borrowed.Value().Get();
                    REQUIRE(value);
                    mutate(*value.Value());
                }

                auto saved = SaveObjectGraph(allocator, objects, objects.OwnerAccess(),
                                             created.Value().Handle());
                CHECK(!saved && saved.Error() == ErrorCode::InvalidArgument);
            }
        }

        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// 필드 쓰기(클래스 단일 값 필드)·staged 쓰기(생성 중 객체)와 경로 쓰기(struct 멤버·배열 원소
// 교체·삽입)가 int8·int16 폭을 검사한다: 범위 밖은 Overflow이고 값은 그대로다. enum 멤버는
// underlying 범위 밖 값을 InvalidArgument로 거부한다.
TEST(IntegerWidthGraph_FieldAndPathWritesCheckWidth) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(allocator, types, {0x4957, 5});
        const auto access = objects.OwnerAccess();
        {
            auto lease = types.Acquire(TypeIdOf<IntegerWidthFields>());
            auto created =
                objects.NewObject<IntegerWidthFields>(access, TypeIdOf<IntegerWidthFields>());
            REQUIRE(lease && created);
            const ObjectHandle handle = created.Value().Handle();
            auto write = [&](const char* text, const FieldValue& value,
                             PropertyPathFailure* failure = nullptr) noexcept {
                PropertyPathBuffer path;
                if (auto parsed = ParsePropertyPath(lease.Value(), StringView(text), path, failure);
                    !parsed) {
                    return parsed;
                }

                return PathWrite(objects, access, handle, path.View(), value, nullptr, failure);
            };
            auto read = [&](const char* text, FieldValue& out) noexcept {
                PropertyPathBuffer path;
                if (auto parsed = ParsePropertyPath(lease.Value(), StringView(text), path);
                    !parsed) {
                    return parsed;
                }

                return PathRead(objects, access, handle, path.View(), out);
            };

            struct Case {
                const char* path;
                ValueKind kind;
                int64 accepted;
                int64 rejected;
            };

            const Case cases[] = {{"tiny", ValueKind::Int8, NumericLimits<int8>::Min(),
                                   int64{NumericLimits<int8>::Max()} + 1},
                                  {"narrow", ValueKind::Int16, NumericLimits<int16>::Max(),
                                   int64{NumericLimits<int16>::Min()} - 1},
                                  {"pair.tiny", ValueKind::Int8, NumericLimits<int8>::Max(),
                                   int64{NumericLimits<int8>::Min()} - 1},
                                  {"pair.narrow", ValueKind::Int16, NumericLimits<int16>::Min(),
                                   int64{NumericLimits<int16>::Max()} + 1}};
            for (const Case& example : cases) {
                REQUIRE(write(example.path, Signed(allocator, example.kind, example.accepted)));
                PropertyPathFailure failure;
                CHECK(ErrorOf(write(example.path, Signed(allocator, example.kind, example.rejected),
                                    &failure)) == ErrorCode::Overflow);
                FieldValue current(allocator);
                REQUIRE(read(example.path, current));
                CHECK(current.kind == example.kind && current.signedInteger == example.accepted);
            }

            // 배열 원소: 삽입과 교체가 같은 폭 검사를 지난다.
            PropertyPathBuffer tinies;
            REQUIRE(ParsePropertyPath(lease.Value(), StringView("tinies[0]"), tinies));
            const FieldValue wideTiny = Signed(allocator, ValueKind::Int8, 200);
            CHECK(ErrorOf(PathInsertElement(objects, access, handle, tinies.View(), &wideTiny)) ==
                  ErrorCode::Overflow);
            const FieldValue first = Signed(allocator, ValueKind::Int8, -7);
            REQUIRE(PathInsertElement(objects, access, handle, tinies.View(), &first));
            CHECK(ErrorOf(write("tinies[0]", Signed(allocator, ValueKind::Int8, -200))) ==
                  ErrorCode::Overflow);
            const FieldValue wideNarrow = Signed(allocator, ValueKind::Int16, 40000);
            PropertyPathBuffer narrows;
            REQUIRE(ParsePropertyPath(lease.Value(), StringView("narrows[0]"), narrows));
            CHECK(ErrorOf(PathInsertElement(objects, access, handle, narrows.View(),
                                            &wideNarrow)) == ErrorCode::Overflow);
            FieldValue element(allocator);
            REQUIRE(read("tinies[0]", element));
            CHECK(element.kind == ValueKind::Int8 && element.signedInteger == -7);
            {
                auto borrowed = objects.Resolve<IntegerWidthFields>(access, handle);
                REQUIRE(borrowed);
                auto value = borrowed.Value().Get();
                REQUIRE(value);
                CHECK(value.Value()->tinies.Size() == 1 && value.Value()->narrows.IsEmpty());
            }

            // int8 enum 멤버: underlying 범위 밖 값은 선언되지 않은 값이다.
            PropertyPathFailure failure;
            CHECK(ErrorOf(write("pair.offset",
                                EnumValue(allocator, TypeIdOf<ByteOffset>(),
                                          int64{NumericLimits<int8>::Max()} + 1),
                                &failure)) == ErrorCode::InvalidArgument);
            CHECK(failure.reason == PropertyPathError::InvalidValue);
            REQUIRE(write("pair.offset", EnumValue(allocator, TypeIdOf<ByteOffset>(),
                                                   NumericLimits<int8>::Min())));
            FieldValue offset(allocator);
            REQUIRE(read("pair.offset", offset));
            CHECK(offset.kind == ValueKind::Enum && offset.signedInteger == -128);

            // staged 쓰기(writeStaged)도 같은 폭 검사를 지나고 실패하면 값이 그대로다.
            const auto fieldOps = [&](StringView name) noexcept -> const RuntimeFieldOps* {
                const Guid id = SchemaField(TypeOf<IntegerWidthFields>(), name).id;
                for (const RuntimeFieldOps& ops : lease.Value().Ops().fields) {
                    if (ops.fieldId == id) {
                        return &ops;
                    }
                }

                return nullptr;
            };
            const RuntimeFieldOps* tiny = fieldOps("tiny");
            const RuntimeFieldOps* narrow = fieldOps("narrow");
            REQUIRE(tiny && narrow && tiny->writeStaged && narrow->writeStaged && tiny->read);
            ConstructionContext construction(objects, access);
            auto staged = construction.Create<IntegerWidthFields>(TypeIdOf<IntegerWidthFields>());
            REQUIRE(staged);
            REQUIRE(tiny->writeStaged(construction, staged.Value(),
                                      Signed(allocator, ValueKind::Int8, -5)));
            CHECK(ErrorOf(tiny->writeStaged(
                      construction, staged.Value(),
                      Signed(allocator, ValueKind::Int8, int64{NumericLimits<int8>::Max()} + 1))) ==
                  ErrorCode::Overflow);
            CHECK(ErrorOf(narrow->writeStaged(construction, staged.Value(),
                                              Signed(allocator, ValueKind::Int16,
                                                     int64{NumericLimits<int16>::Min()} - 1))) ==
                  ErrorCode::Overflow);
            auto committed = construction.Commit();
            REQUIRE(committed);
            FieldValue stagedTiny(allocator);
            REQUIRE(tiny->read(objects, access, committed.Value().Handle(), stagedTiny));
            CHECK(stagedTiny.kind == ValueKind::Int8 && stagedTiny.signedInteger == -5);
        }

        REQUIRE(objects.Shutdown(access));
    }

    CHECK(types.Shutdown());
}
} // namespace integer_width_graph_tests_file
