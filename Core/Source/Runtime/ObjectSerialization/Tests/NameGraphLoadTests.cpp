#include "NameGraphTestSupport.h"

// 리플렉션 B9c2 Name 필드의 복원 단계 시험이다(Docs/Design/ReflectionExtensions.ko.md 4·4.1절,
// 17절 B9c 항목): 1단계가 손상 표시 문자열을 거부하고(UE 5.8 길이 규칙 경계 포함) pool에 닿지
// 않으며, None 문자열은 intern하지 않고, 2a intern의 pool 한도 실패가 생성자 전에 로드를 끝내고
// 사전 확인 실패면 intern하지 않으며, 노드·struct 마이그레이션 보관본의 Name이 순서대로 소비되고,
// 1단계가 2a 몫을 미리 청구하며, Name을 담는 struct는 관찰 대상이 될 수 없음을 본다.
using namespace rocks;
namespace name_graph_load_tests_file {

using namespace rocks::test::names;

// 1단계가 손상 표시 문자열을 객체 공개 없이 거부한다: 3,072 byte 초과·NUL·분해형(NFC 아님)·잘못된
// UTF-8은 InvalidArgument(단일 필드·배열 원소·struct 멤버), String이 아닌 wire는 TypeMismatch다.
// 3,072 byte와 비ASCII NFC 값은 받는다.
TEST(NameGraph_LoadRejectsInvalidNameText) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    NamePool names(allocator);
    REQUIRE(names.Initialize());
    {
        ObjectRegistry target(allocator, types, {0x4e4d47, 6}, WithNames(&names));
        // UE 5.8 길이 규칙: 번호 접미를 뗀 base 1,023 UTF-16 단위(3 byte 글자 1,023개 + 최대 번호 =
        // 3,080 byte 허용), base 1,024단위 거부.
        const Array<char> wideBase = Repeat(allocator, "\xEA\xB0\x80", MaxNameBaseUtf16Units);
        Array<char> longest = Repeat(allocator, "\xEA\xB0\x80", MaxNameBaseUtf16Units);
        for (char c : StringView("_2147483646")) {
            REQUIRE(longest.TryPushBack(c));
        }

        const Array<char> tooLong = Repeat(allocator, "a", MaxNameBaseUtf16Units + 1);
        REQUIRE(longest.Size() == MaxNameFieldBytes && wideBase.Size() == 3069);
        struct Case {
            StringView text;
            ErrorCode expected;
        };

        const Case cases[] = {{"Socket_7", ErrorCode::None},
                              {"\xC3\xA9t\xC3\xA9", ErrorCode::None},
                              {ViewOf(longest), ErrorCode::None},
                              {ViewOf(tooLong), ErrorCode::CorruptData},
                              {StringView("a\0b", 3), ErrorCode::CorruptData},
                              {"e\xCC\x81", ErrorCode::CorruptData},
                              {StringView("\xFF", 1), ErrorCode::CorruptData}};
        for (const Case& example : cases) {
            auto payload = StringPayload(allocator, example.text);
            auto graph = SingleFieldGraph(allocator, "label", WireType::String, payload.View());
            CHECK(LoadGraph(allocator, types, target, graph.View()) == example.expected);
            if (example.expected == ErrorCode::CorruptData) {
                auto elements = StringArrayPayload(allocator, example.text, 2);
                auto array =
                    SingleFieldGraph(allocator, "names", WireType::ValueArray, elements.View());
                CHECK(LoadGraph(allocator, types, target, array.View()) == example.expected);
                RecordBuilder socket(allocator, GeneratedType<NameSocket>::Id(), 1);
                REQUIRE(
                    socket.AddField(SchemaField(GeneratedType<NameSocket>::Schema(), "socket").id,
                                    WireType::String, payload.View()));
                auto socketBytes = socket.Build();
                REQUIRE(socketBytes);
                auto nested = SingleFieldGraph(allocator, "socket", WireType::Record,
                                               socketBytes.Value().View());
                CHECK(LoadGraph(allocator, types, target, nested.View()) == example.expected);
            }
        }

        ArchiveWriter u32(allocator);
        REQUIRE(u32.WriteU32(1));
        auto u32Bytes = u32.TakeBytes();
        auto wrongWire = SingleFieldGraph(allocator, "label", WireType::U32, u32Bytes.View());
        CHECK(LoadGraph(allocator, types, target, wrongWire.View()) == ErrorCode::TypeMismatch);
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(names.Shutdown());
    CHECK(types.Shutdown());
}

// 1단계(Validate)는 pool에 닿지 않고 2a(CreateObjects)가 대상 Registry의 pool로 intern한다. pool
// 한도(CapacityExceeded)는 2a 실패로 로드를 끝낸다: 생성자를 실행하지 않고(예약·공개 객체 없음)
// 트랜잭션을 고정하지 않으며, 이미 intern한 entry는 pool에 남는다. 편의 복원도 같은 오류다.
TEST(NameGraph_InternFailureEndsLoadBeforeConstruction) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    NamePool sourceNames(allocator);
    NamePool small(allocator);
    REQUIRE(sourceNames.Initialize());
    NamePoolConfig config;
    config.maxEntries = 2;
    REQUIRE(small.Initialize(config));
    {
        ObjectRegistry source(allocator, types, {0x4e4d47, 7}, WithNames(&sourceNames));
        ObjectRegistry target(allocator, types, {0x4e4d47, 8}, WithNames(&small));
        {
            auto created = NewFields(source, [&](NameFields& value) noexcept {
                value.label = Intern(sourceNames, "Alpha");
                value.other = Intern(sourceNames, "Beta");
                return value.names.TryPushBack(Intern(sourceNames, "Gamma")).IsOk() &&
                       value.names.TryPushBack(Name{}).IsOk();
            });
            REQUIRE(created);
            auto saved =
                SaveObjectGraph(allocator, source, source.OwnerAccess(), created.Value().Handle());
            REQUIRE(saved);
            ObjectGraphInput input;
            input.bytes = saved.Value().View();
            {
                ObjectGraphStage stage(allocator, target, types, target.OwnerAccess());
                REQUIRE(stage.Validate(input));
                // 1단계는 pool(과 ObjectRegistry)에 닿지 않는다.
                const NamePoolStats validated = small.Stats();
                CHECK(validated.entryCount == 0 && validated.pendingEntries == 0 &&
                      validated.activeOperations == 0);
                ConstructionContext transaction(target, target.OwnerAccess());
                CHECK(ErrorOf(stage.CreateObjects(transaction)) == ErrorCode::CapacityExceeded);
                CHECK(stage.Nodes().Size() == 0);
                CHECK(target.Stats().reservedObjects == 0 && target.Stats().liveObjects == 0);
                CHECK(transaction.CheckOpen());
                CHECK(small.Stats().entryCount == 2);
            }

            CHECK(LoadGraph(allocator, types, target, saved.Value().View()) ==
                  ErrorCode::CapacityExceeded);
            CHECK(small.Stats().entryCount == 2);
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(sourceNames.Shutdown());
    CHECK(small.Shutdown());
    CHECK(types.Shutdown());
}

// 1단계가 2a intern 몫(Name마다 표시 byte + 상수 16 — GraphInternal.h NameInternWork)을 미리
// 청구한다. 같은 wire(String 원소 ValueArray)의 String 배열과 비교한다: 원소 하나의 1단계 작업량은
// Name이 내용 검사(byte, 2단계 몫까지 두 배) + intern 몫(byte + 16) + 원소 메모리, String이 2단계
// 문자열 복사(byte 두 배) + 원소 메모리다(reader 작업은 같다). 그래서 원소 하나의 차이는 byte + 16
// + (sizeof(Name) − sizeof(String))이고, 길이를 늘리면 byte마다 1씩 더 벌어진다.
TEST(NameGraph_ValidateChargesInternWork) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry target(allocator, types, {0x4e4d47, 9});
        constexpr int64 InternWorkPerName = 16;
        const auto minimum = [&](StringView field, StringView text, uint32 count) noexcept {
            auto elements = StringArrayPayload(allocator, text, count);
            auto graph = SingleFieldGraph(allocator, field, WireType::ValueArray, elements.View());
            return static_cast<int64>(MinimumValidateWork(allocator, target, types, graph.View()));
        };

        const StringView shortText = "Socket";
        const StringView longText = "Socket_Long_Display_Name";
        const int64 nameElement = minimum("names", shortText, 2) - minimum("names", shortText, 1);
        const int64 textElement = minimum("texts", shortText, 2) - minimum("texts", shortText, 1);
        CHECK(nameElement - textElement ==
              static_cast<int64>(shortText.ByteCount()) + InternWorkPerName +
                  static_cast<int64>(sizeof(Name)) - static_cast<int64>(sizeof(String)));
        const int64 nameBytes = minimum("names", longText, 1) - minimum("names", shortText, 1);
        const int64 textBytes = minimum("texts", longText, 1) - minimum("texts", shortText, 1);
        CHECK(nameBytes - textBytes ==
              static_cast<int64>(longText.ByteCount() - shortText.ByteCount()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// 관찰 struct(FieldCodecHooks::ObservedStructs)는 Name을 담을 수 없다: 직접 담는 NameSocket과 원소
// struct로 간접 담는 NameGroup은 1단계 시작에서 InvalidArgument(객체 없음)이고, Name이 없는
// NamePlain은 관찰된다. 등록 형질 ContainsName이 그 판정이다.
TEST(NameGraph_ObservedStructsMustNotContainNames) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    NamePool names(allocator);
    REQUIRE(names.Initialize());
    {
        ObjectRegistry objects(allocator, types, {0x4e4d47, 10}, WithNames(&names));
        {
            const TypeId socketId = GeneratedType<NameSocket>::Id();
            const TypeId groupId = GeneratedType<NameGroup>::Id();
            const TypeId plainId = GeneratedType<NamePlain>::Id();
            auto socket = types.AcquireValueType(socketId, TypeKind::Struct);
            auto group = types.AcquireValueType(groupId, TypeKind::Struct);
            auto plainType = types.AcquireValueType(plainId, TypeKind::Struct);
            REQUIRE(socket && group && plainType);
            CHECK(socket.Value().Struct().ContainsName());
            CHECK(group.Value().Struct().ContainsName());
            CHECK(!plainType.Value().Struct().ContainsName());
            // 배열이 아닌 struct 멤버(NameWrapper::socket)로도 전파된다.
            auto wrapper =
                types.AcquireValueType(GeneratedType<NameWrapper>::Id(), TypeKind::Struct);
            REQUIRE(wrapper);
            CHECK(wrapper.Value().Struct().ContainsName());

            auto created = NewFields(objects, [&](NameFields& value) noexcept {
                value.label = Intern(names, "Grip");
                value.plain.value = 41;
                return true;
            });
            REQUIRE(created);
            auto saved = SaveObjectGraph(allocator, objects, objects.OwnerAccess(),
                                         created.Value().Handle());
            REQUIRE(saved);
            const TypeId rejected[] = {socketId, groupId};
            for (TypeId id : rejected) {
                ObserveOne hooks(id);
                ObjectGraphLoadOptions options;
                options.hooks = &hooks;
                CHECK(LoadGraph(allocator, types, objects, saved.Value().View(), options) ==
                      ErrorCode::InvalidArgument);
                CHECK(hooks.observed == 0);
            }

            ObserveOne hooks(plainId);
            ObjectGraphLoadOptions options;
            options.hooks = &hooks;
            CHECK(LoadGraph(allocator, types, objects, saved.Value().View(), options) ==
                  ErrorCode::None);
            CHECK(hooks.observed == 1 && hooks.observedValue == 41);
        }

        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(names.Shutdown());
    CHECK(types.Shutdown());
}

// 2a는 None 문자열을 intern하지 않는다: wire에 "NONE"·"none"·"_7"·""이 와도 기본 None이 되고 pool
// entry가 늘지 않으며 다시 저장하면 빈 문자열이다(기본값 객체를 저장한 byte와 같다). pool 있는·없는
// Registry 모두 같다. 번호 있는 "None_3"은 None이 아니라 intern한다.
TEST(NameGraph_NoneTextsLoadAsDefaultNoneWithoutEntries) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    NamePool names(allocator);
    REQUIRE(names.Initialize());
    {
        ObjectRegistry named(allocator, types, {0x4e4d47, 11}, WithNames(&names));
        ObjectRegistry plain(allocator, types, {0x4e4d47, 12});
        ObjectRegistry* registries[] = {&named, &plain};
        for (ObjectRegistry* target : registries) {
            auto fresh = NewFields(*target, [](NameFields&) noexcept { return true; });
            REQUIRE(fresh);
            auto defaults =
                SaveObjectGraph(allocator, *target, target->OwnerAccess(), fresh.Value().Handle());
            REQUIRE(defaults);
            const char* texts[] = {"NONE", "none", "_7", ""};
            for (const char* text : texts) {
                auto payload = StringPayload(allocator, text);
                auto graph = SingleFieldGraph(allocator, "label", WireType::String, payload.View());
                const uint64 entries = names.Stats().entryCount;
                auto loaded =
                    LoadObjectGraph(allocator, *target, types, target->OwnerAccess(), graph.View());
                REQUIRE(loaded);
                CHECK(names.Stats().entryCount == entries);
                Look(*target, loaded.Value().Handle(), [](const NameFields& value) noexcept {
                    CHECK(value.label.kind == NameKind::None);
                });
                auto resaved = SaveObjectGraph(allocator, *target, target->OwnerAccess(),
                                               loaded.Value().Handle());
                REQUIRE(resaved);
                CHECK(test::graph::SameBytes(defaults.Value().View(), resaved.Value().View()));
            }
        }

        // 번호 있는 None은 None이 아니다 — pool이 있으면 intern하고 없으면 Unsupported다.
        auto numbered = StringPayload(allocator, "None_3");
        auto graph = SingleFieldGraph(allocator, "label", WireType::String, numbered.View());
        const uint64 entries = names.Stats().entryCount;
        CHECK(LoadGraph(allocator, types, named, graph.View()) == ErrorCode::None);
        CHECK(names.Stats().entryCount == entries + 1);
        CHECK(LoadGraph(allocator, types, plain, graph.View()) == ErrorCode::Unsupported);
        REQUIRE(named.Shutdown(named.OwnerAccess()));
        REQUIRE(plain.Shutdown(plain.OwnerAccess()));
    }

    CHECK(names.Shutdown());
    CHECK(types.Shutdown());
}

// 노드·struct 마이그레이션 결과에서 읽은 Name: 1단계는 마이그레이션 보관본(node.migrated·struct
// 보관본)을 빌려 기록하고, 2단계는 보관본을 같은 순서로 다시 써서 토큰을 소비한다(EndNode가 남은
// 기록이 없음을 본다). 버전 1 노드 Record(버전 1 struct Record 포함)를 버전 2로 올려 복원한다.
TEST(NameGraph_MigratedRecordsKeepNameOrder) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    NamePool names(allocator);
    REQUIRE(names.Initialize());
    {
        ObjectRegistry target(allocator, types, {0x4e4d47, 15}, WithNames(&names));
        {
            const TypeSchema& schema = TypeOf<NameMigrated>();
            const TypeSchema& versioned = GeneratedType<NameVersioned>::Schema();
            auto tag = StringPayload(allocator, "Grip_3");
            RecordBuilder inner(allocator, GeneratedType<NameVersioned>::Id(), 1);
            REQUIRE(inner.AddField(SchemaField(versioned, "tag").id, WireType::String, tag.View()));
            auto innerBytes = inner.Build();
            REQUIRE(innerBytes);
            auto label = StringPayload(allocator, "Old");
            auto elements = StringArrayPayload(allocator, "Grip_3", 2);
            RecordBuilder outer(allocator, TypeIdOf<NameMigrated>(), 1);
            REQUIRE(
                outer.AddField(SchemaField(schema, "label").id, WireType::String, label.View()));
            REQUIRE(outer.AddField(SchemaField(schema, "versioned").id, WireType::Record,
                                   innerBytes.Value().View()));
            REQUIRE(outer.AddField(SchemaField(schema, "names").id, WireType::ValueArray,
                                   elements.View()));
            auto outerBytes = outer.Build();
            REQUIRE(outerBytes);
            auto graph =
                test::graph::Single(allocator, TypeIdOf<NameMigrated>(), outerBytes.Value().View());
            REQUIRE(graph);

            uint32 calls = 0;
            ObjectGraphLoadOptions options;
            options.migrate = &CopyFields;
            options.migrateStruct = &CopyFields;
            options.migrationUser = &calls;
            auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                          graph.Value().View(), options);
            REQUIRE(loaded);
            CHECK(calls == 2);
            // 같은 표시 셋("Old"·"Grip_3" 셋)은 entry 둘이다.
            CHECK(names.Stats().entryCount == 2);
            auto borrowed =
                target.Resolve<NameMigrated>(target.OwnerAccess(), loaded.Value().Handle());
            REQUIRE(borrowed);
            auto value = borrowed.Value().Get();
            REQUIRE(value);
            CHECK(DisplayIs(names, value.Value()->label, "Old"));
            CHECK(DisplayIs(names, value.Value()->versioned.tag, "Grip_3"));
            REQUIRE(value.Value()->names.Size() == 2);
            CHECK(value.Value()->names[0] == value.Value()->versioned.tag &&
                  value.Value()->names[1] == value.Value()->versioned.tag);
        }

        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(names.Shutdown());
    CHECK(types.Shutdown());
}

// 2a 사전 확인이 실패하면(이름 점유 AlreadyExists) intern하지 않는다 — pool이 늘지 않는다.
TEST(NameGraph_PreconditionFailureInternsNothing) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    NamePool sourceNames(allocator);
    NamePool targetNames(allocator);
    REQUIRE(sourceNames.Initialize());
    REQUIRE(targetNames.Initialize());
    {
        ObjectRegistry source(allocator, types, {0x4e4d47, 16}, WithNames(&sourceNames));
        ObjectRegistry target(allocator, types, {0x4e4d47, 17}, WithNames(&targetNames));
        {
            auto created = source.NewObject<NameFields>(
                source.OwnerAccess(), TypeIdOf<NameFields>(), ObjectHandle{}, "Taken");
            REQUIRE(created);
            Look(source, created.Value().Handle(),
                 [&](NameFields& value) noexcept { value.label = Intern(sourceNames, "Grip"); });
            auto saved =
                SaveObjectGraph(allocator, source, source.OwnerAccess(), created.Value().Handle());
            REQUIRE(saved);
            auto occupant = target.NewObject<NameFields>(
                target.OwnerAccess(), TypeIdOf<NameFields>(), ObjectHandle{}, "Taken");
            REQUIRE(occupant);
            ObjectGraphStage stage(allocator, target, types, target.OwnerAccess());
            ObjectGraphInput input;
            input.bytes = saved.Value().View();
            REQUIRE(stage.Validate(input));
            ConstructionContext transaction(target, target.OwnerAccess());
            CHECK(ErrorOf(stage.CreateObjects(transaction)) == ErrorCode::AlreadyExists);
            CHECK(targetNames.Stats().entryCount == 0);
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(sourceNames.Shutdown());
    CHECK(targetNames.Shutdown());
    CHECK(types.Shutdown());
}
} // namespace name_graph_load_tests_file
