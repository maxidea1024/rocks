#include <ReflectionTestFixtures/ReflectionFixture.generated.h>
#include <Core/Memory/SystemAllocator.h>
#include <ObjectSerialization/ObjectGraph.h>
#include <Platform/Fatal.h>
#include <Serialization/Archive.h>
#include <Serialization/Record.h>
#include <TestSupport/Test.h>

// B3b enum 필드의 그래프 저장·복원 시험이다(Docs/Design/ReflectionFoundations.ko.md 3.4절): I64
// wire 왕복과 재저장 byte 동일성, C++에서 직접 넣은 선언되지 않은 값의 저장 거부, 저장된 선언되지
// 않은 값·잘못된 wire의 복원 거부(객체를 공개하지 않는다).
using namespace rocks;
using reflection_fixture::Bias;
using reflection_fixture::Channels;
using reflection_fixture::EnumFields;
using reflection_fixture::TextureGroup;

namespace {

const FieldSchema& SchemaField(const TypeSchema& schema, StringView name) noexcept {
    for (const FieldSchema& field : schema.fields) {
        if (StringView(field.name.Data(), field.name.Size()) == name) {
            return field;
        }
    }

    platform::Fatal("fixture field missing");
}

// 픽스처 enum과 EnumFields를 등록한다. 기록은 Shutdown이 정리한다.
bool RegisterFixture(TypeRegistry& types) noexcept {
    return GeneratedType<TextureGroup>::Register(types).IsOk() &&
           GeneratedType<Bias>::Register(types).IsOk() &&
           GeneratedType<Channels>::Register(types).IsOk() &&
           GeneratedType<EnumFields>::Register(types).IsOk();
}

Result<RootRef<EnumFields>> NewFilled(ObjectRegistry& objects, TextureGroup group, Bias bias,
                                      Channels channels) noexcept {
    auto created = objects.NewObject<EnumFields>(objects.OwnerAccess(), TypeIdOf<EnumFields>());
    if (!created) {
        return created;
    }

    auto borrowed = objects.Resolve<EnumFields>(objects.OwnerAccess(), created.Value().Handle());
    if (!borrowed) {
        return Result<RootRef<EnumFields>>::Failure(borrowed.Error());
    }

    auto value = borrowed.Value().Get();
    if (!value) {
        return Result<RootRef<EnumFields>>::Failure(value.Error());
    }

    value.Value()->group = group;
    value.Value()->bias = bias;
    value.Value()->channels = channels;
    return created;
}

// EnumFields 레코드 하나짜리 graph format 1 바이트를 만든다(ObjectGraphTests의 SingleGraph와 같은
// envelope).
Result<Array<uint8>> SingleGraph(Allocator& allocator, Span<const uint8> record) noexcept {
    const uint8 magic[] = {'R', 'O', 'C', 'K', 'S', 'G', 'R', 'F'};
    ArchiveWriter writer(allocator);
    Result<void> written = writer.WriteRaw(magic);
    const uint32 header[] = {1, 1, 1};
    for (uint32 value : header) {
        if (written) {
            written = writer.WriteU32(value);
        }
    }

    if (written) {
        written = writer.WriteU64(1);
    }

    if (written) {
        written = writer.WriteGuid(TypeIdOf<EnumFields>());
    }

    if (written) {
        written = writer.WriteU32(1);
    }

    if (written) {
        written = writer.WriteBytes(record);
    }

    if (!written) {
        return Result<Array<uint8>>::Failure(written.Error());
    }

    return Result<Array<uint8>>::Success(writer.TakeBytes());
}

} // namespace

TEST(EnumGraph_RoundTripPreservesValuesAndResavesIdentically) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry source(allocator, types, {0x4547, 1});
        ObjectRegistry target(allocator, types, {0x4547, 2});
        {
            const Channels mixed = static_cast<Channels>(static_cast<uint16>(Channels::Red) |
                                                         static_cast<uint16>(Channels::Alpha));
            auto created = NewFilled(source, TextureGroup::Reserved13, Bias::Low, mixed);
            REQUIRE(created);
            auto saved =
                SaveObjectGraph(allocator, source, source.OwnerAccess(), created.Value().Handle());
            REQUIRE(saved);
            auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                          saved.Value().View());
            REQUIRE(loaded);
            {
                auto borrowed =
                    target.Resolve<EnumFields>(target.OwnerAccess(), loaded.Value().Handle());
                REQUIRE(borrowed);
                auto value = borrowed.Value().Get();
                REQUIRE(value);
                CHECK(value.Value()->group == TextureGroup::Reserved13);
                CHECK(value.Value()->bias == Bias::Low);
                CHECK(value.Value()->channels == mixed);
            }

            auto resaved =
                SaveObjectGraph(allocator, target, target.OwnerAccess(), loaded.Value().Handle());
            REQUIRE(resaved);
            bool same = resaved.Value().Size() == saved.Value().Size();
            for (usize index = 0; same && index < saved.Value().Size(); ++index) {
                same = saved.Value()[index] == resaved.Value()[index];
            }

            CHECK(same);
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// C++에서 static_cast로 넣은 선언되지 않은 값은 저장하지 않는다(임시 소유는 모두 돌려준다).
TEST(EnumGraph_SaveRejectsUndeclaredValues) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(allocator, types, {0x4547, 3});
        {
            struct Case {
                TextureGroup group;
                Bias bias;
                Channels channels;
            };

            const Case cases[] = {{static_cast<TextureGroup>(5), Bias::Zero, Channels::None},
                                  {TextureGroup::World, static_cast<Bias>(-1), Channels::None},
                                  {TextureGroup::World, Bias::Zero, static_cast<Channels>(16)}};
            for (const Case& example : cases) {
                auto created = NewFilled(objects, example.group, example.bias, example.channels);
                REQUIRE(created);
                const auto baseline = allocator.Stats();
                const auto leases = types.Stats().leases;
                auto saved = SaveObjectGraph(allocator, objects, objects.OwnerAccess(),
                                             created.Value().Handle());
                CHECK(!saved && saved.Error() == ErrorCode::InvalidArgument);
                CHECK(allocator.Stats().liveAllocations == baseline.liveAllocations);
                CHECK(types.Stats().leases == leases);
                CHECK(objects.Stats().borrows == 0);
            }
        }

        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// 저장된 선언되지 않은 값·범위 밖 값·Flags의 선언되지 않은 비트·잘못된 wire는 객체를 공개하지 않고
// 거부한다(decode 정책은 B3c — 지금은 항상 거부).
TEST(EnumGraph_LoadRejectsUndeclaredValuesAndWrongWire) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry target(allocator, types, {0x4547, 4});
        ObjectGraphLoadOptions options;
        options.allowMissingFields = true;
        const TypeSchema& schema = TypeOf<EnumFields>();
        struct Case {
            StringView field;
            WireType wire;
            int64 value;
            ErrorCode expected;
        };

        const Case cases[] = {{"group", WireType::I64, 5, ErrorCode::InvalidArgument},
                              {"group", WireType::I64, 256 + 7, ErrorCode::InvalidArgument},
                              {"group", WireType::I64, -1, ErrorCode::InvalidArgument},
                              {"bias", WireType::I64, 2, ErrorCode::InvalidArgument},
                              {"channels", WireType::I64, 16, ErrorCode::InvalidArgument},
                              {"group", WireType::U32, 7, ErrorCode::TypeMismatch}};
        for (const Case& example : cases) {
            ArchiveWriter payload(allocator);
            if (example.wire == WireType::U32) {
                REQUIRE(payload.WriteU32(static_cast<uint32>(example.value)));
            } else {
                REQUIRE(payload.WriteI64(example.value));
            }

            auto bytes = payload.TakeBytes();
            RecordBuilder record(allocator, TypeIdOf<EnumFields>(), 1);
            REQUIRE(record.AddField(SchemaField(schema, example.field).id,
                                    static_cast<uint32>(example.wire), bytes.View()));
            auto recordBytes = record.Build();
            REQUIRE(recordBytes);
            auto graph = SingleGraph(allocator, recordBytes.Value().View());
            REQUIRE(graph);
            auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                          graph.Value().View(), options);
            CHECK(!loaded && loaded.Error() == example.expected);
            CHECK(target.Stats().liveObjects == 0);
            CHECK(target.Stats().reservedObjects == 0);
        }

        // 선언된 값(Flags는 선언 값의 OR)은 받는다.
        ArchiveWriter payload(allocator);
        REQUIRE(payload.WriteI64(1 | 2 | 4));
        auto bytes = payload.TakeBytes();
        RecordBuilder record(allocator, TypeIdOf<EnumFields>(), 1);
        REQUIRE(record.AddField(SchemaField(schema, "channels").id,
                                static_cast<uint32>(WireType::I64), bytes.View()));
        auto recordBytes = record.Build();
        REQUIRE(recordBytes);
        auto graph = SingleGraph(allocator, recordBytes.Value().View());
        REQUIRE(graph);
        {
            auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                          graph.Value().View(), options);
            REQUIRE(loaded);
            auto borrowed =
                target.Resolve<EnumFields>(target.OwnerAccess(), loaded.Value().Handle());
            REQUIRE(borrowed);
            auto value = borrowed.Value().Get();
            REQUIRE(value);
            CHECK(static_cast<uint16>(value.Value()->channels) == 7);
        }

        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}
