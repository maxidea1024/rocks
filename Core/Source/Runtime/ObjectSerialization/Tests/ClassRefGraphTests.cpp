#include "ClassRefTestSupport.h"
#include <Serialization/ValueArray.h>

// 리플렉션 B9d2 ClassRef PROPERTY의 그래프 저장·복원
// 시험이다(Docs/Design/ReflectionExtensions.ko.md 5.2절, 17절 B9d 항목): 클래스 TypeId 왕복(추상
// 클래스·자기 클래스 Base·배열 원소·struct 멤버), 미등록 클래스의 decode 정책(Reject·ReportAndNull
// — 진단 경로와 Required 진단), 클래스가 아니거나 Base의 파생이 아닌 값의 decode 거부, 저장의
// 재검사(C++가 직접 넣은 값·Required null).
using namespace rocks;
using namespace rocks::test::classref;

namespace {

bool SameBytes(Span<const uint8> left, Span<const uint8> right) noexcept {
    if (left.Size() != right.Size()) {
        return false;
    }

    for (usize index = 0; index < left.Size(); ++index) {
        if (left[index] != right[index]) {
            return false;
        }
    }

    return true;
}

// 모든 ClassRef 자리를 채운 holder다(ClassLeaf 값은 required·derived·list[2]·slots[0].kind).
void Fill(ObjectRegistry& objects, ObjectHandle holder) noexcept {
    With(objects, holder, [](ClassHolder& value) noexcept {
        value.any = ClassRef<ClassBase>::Of<ClassBase>();
        value.required = ClassRef<ClassBase>::Of<ClassLeaf>();
        value.derived = ClassRef<ClassDerived>::Of<ClassLeaf>();
        value.self = ClassRef<ClassHolder>::Of<ClassHolder>();
        REQUIRE(value.list.TryPushBack(ClassRef<ClassBase>::Of<ClassDerived>()));
        REQUIRE(value.list.TryPushBack(ClassRef<ClassBase>()));
        REQUIRE(value.list.TryPushBack(ClassRef<ClassBase>::Of<ClassLeaf>()));
        value.slot.kind = ClassRef<ClassBase>::Of<ClassDerived>();
        value.slot.count = 2;
        REQUIRE(value.slots.TryPushBack(ClassSlot{ClassRef<ClassBase>::Of<ClassLeaf>(), 3}));
    });
}

// 노드마다 Record 하나인 NodeTable 그래프다.
Array<uint8> Graph(Allocator& allocator, Span<const uint8> record) noexcept {
    const test::graph::Node node{TypeIdOf<ClassHolder>(), 0, {}, record};
    auto built = test::graph::Build(allocator, Span<const test::graph::Node>(&node, 1));
    if (!built) {
        platform::Fatal("test graph build failed");
    }

    return Move(built.Value());
}

// 단계형 복원 1단계(Validate)의 결과다.
ErrorCode Validate(Allocator& allocator, ObjectRegistry& objects, TypeRegistry& types,
                   Span<const uint8> bytes, ObjectGraphLoadOptions options = {}) noexcept {
    ObjectGraphStage stage(allocator, objects, types, objects.OwnerAccess(), options);
    ObjectGraphInput input;
    input.bytes = bytes;
    return ErrorOf(stage.Validate(input));
}

// Guid 원소 ValueArray payload다.
Array<uint8> GuidArrayPayload(Allocator& allocator, Span<const Guid> values) noexcept {
    ArchiveWriter payload(allocator);
    auto writer = ValueArrayWriter::Begin(
        payload, WireType::Guid, static_cast<uint32>(values.Size()), NumericLimits<usize>::Max());
    if (!writer) {
        platform::Fatal("test value array begin failed");
    }

    for (Guid value : values) {
        if (!payload.WriteGuid(value)) {
            platform::Fatal("test value array write failed");
        }
    }

    if (!writer.Value().End()) {
        platform::Fatal("test value array end failed");
    }

    return payload.TakeBytes();
}

} // namespace

// 클래스 TypeId가 그대로 왕복한다: 추상 클래스(ClassBase)·자기 클래스 Base(self)·null 원소·
// Array<ClassRef> 원소·struct 멤버·struct 원소 배열의 멤버. 복원한 그래프를 다시 저장하면 byte가
// 같다(결정성).
TEST(ClassRefGraph_RoundTripKeepsClassIds) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry source(allocator, types, {0x435247, 1});
        ObjectRegistry target(allocator, types, {0x435247, 2});
        RootRef<ClassHolder> holder = New<ClassHolder>(source);
        Fill(source, holder.Handle());
        auto bytes = SaveObjectGraph(allocator, source, source.OwnerAccess(), holder.Handle());
        REQUIRE(bytes);
        auto loaded =
            LoadObjectGraph(allocator, target, types, target.OwnerAccess(), bytes.Value().View());
        REQUIRE(loaded);
        With(target, loaded.Value().Handle(), [](ClassHolder& value) noexcept {
            CHECK(value.any.Id() == TypeIdOf<ClassBase>());
            CHECK(value.required.Id() == TypeIdOf<ClassLeaf>());
            CHECK(value.derived.Id() == TypeIdOf<ClassLeaf>());
            CHECK(value.self.Id() == TypeIdOf<ClassHolder>());
            REQUIRE(value.list.Size() == 3);
            CHECK(value.list[0].Id() == TypeIdOf<ClassDerived>());
            CHECK(value.list[1].IsNull());
            CHECK(value.list[2].Id() == TypeIdOf<ClassLeaf>());
            CHECK(value.slot.kind.Id() == TypeIdOf<ClassDerived>() && value.slot.count == 2);
            REQUIRE(value.slots.Size() == 1);
            CHECK(value.slots[0].kind.Id() == TypeIdOf<ClassLeaf>() && value.slots[0].count == 3);
        });
        auto again =
            SaveObjectGraph(allocator, target, target.OwnerAccess(), loaded.Value().Handle());
        REQUIRE(again);
        CHECK(SameBytes(bytes.Value().View(), again.Value().View()));
        holder.Reset();
        loaded.Value().Reset();
        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// 저장한 클래스(ClassLeaf)가 복원 쪽 TypeRegistry에 없으면 decode 정책을 따른다: Reject(기본)는
// NotFound이고, ReportAndNull은 진단 hook이 없으면 NotFound(진단 없이 값을 바꾸지 않는다), 있으면
// 그 값마다 UnknownClassReference(valueType = 저장된 클래스, 경로 = 단일 필드·[list[2]]·
// [slots[0], kind])를 내고 null로 읽는다 — 등록된 클래스 값은 그대로다. null이 된 Required 필드는
// 적용 뒤 RequiredMissing으로 보고된다(로드는 거부하지 않는다).
TEST(ClassRefGraph_UnknownClassFollowsDecodePolicy) {
    SystemAllocator allocator;
    TypeRegistry sourceTypes(allocator);
    TypeRegistry targetTypes(allocator);
    REQUIRE(RegisterFixture(sourceTypes));
    REQUIRE(RegisterFixture(targetTypes, false));
    {
        ObjectRegistry source(allocator, sourceTypes, {0x435247, 3});
        ObjectRegistry target(allocator, targetTypes, {0x435247, 4});
        RootRef<ClassHolder> holder = New<ClassHolder>(source);
        Fill(source, holder.Handle());
        auto bytes = SaveObjectGraph(allocator, source, source.OwnerAccess(), holder.Handle());
        REQUIRE(bytes);

        auto rejected = LoadObjectGraph(allocator, target, targetTypes, target.OwnerAccess(),
                                        bytes.Value().View());
        CHECK(!rejected && rejected.Error() == ErrorCode::NotFound);

        ObjectGraphLoadOptions silent;
        silent.policy.unknownClass = UnknownClassPolicy::ReportAndNull;
        auto unreported = LoadObjectGraph(allocator, target, targetTypes, target.OwnerAccess(),
                                          bytes.Value().View(), silent);
        CHECK(!unreported && unreported.Error() == ErrorCode::NotFound);
        CHECK(target.Stats().liveObjects == 0);

        Recorder recorder;
        ObjectGraphLoadOptions options;
        options.policy.unknownClass = UnknownClassPolicy::ReportAndNull;
        options.hooks = &recorder;
        auto loaded = LoadObjectGraph(allocator, target, targetTypes, target.OwnerAccess(),
                                      bytes.Value().View(), options);
        REQUIRE(loaded);
        const TypeId leaf = TypeIdOf<ClassLeaf>();
        CHECK(recorder.Count(FieldDiagnosticKind::UnknownClassReference) == 4);
        CHECK(recorder.HasUnknownClass(leaf, HolderField("required"), false, 0));
        CHECK(recorder.HasUnknownClass(leaf, HolderField("derived"), false, 0));
        CHECK(recorder.HasUnknownClass(leaf, HolderField("list"), true, 2));
        CHECK(recorder.HasUnknownClass(leaf, HolderField("slots"), true, 0,
                                       SchemaField(TypeOf<ClassSlot>(), "kind").id));
        CHECK(recorder.Count(FieldDiagnosticKind::RequiredMissing) == 1);
        With(target, loaded.Value().Handle(), [](ClassHolder& value) noexcept {
            CHECK(value.any.Id() == TypeIdOf<ClassBase>());
            CHECK(value.required.IsNull());
            CHECK(value.derived.IsNull());
            CHECK(value.self.Id() == TypeIdOf<ClassHolder>());
            REQUIRE(value.list.Size() == 3);
            CHECK(value.list[0].Id() == TypeIdOf<ClassDerived>());
            CHECK(value.list[1].IsNull() && value.list[2].IsNull());
            CHECK(value.slot.kind.Id() == TypeIdOf<ClassDerived>());
            REQUIRE(value.slots.Size() == 1);
            CHECK(value.slots[0].kind.IsNull() && value.slots[0].count == 3);
        });
        holder.Reset();
        loaded.Value().Reset();
        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(sourceTypes.Shutdown());
    CHECK(targetTypes.Shutdown());
}

// 등록된 레코드라도 클래스가 아니거나(struct ID) Base의 파생이 아니면(무관한 클래스, ClassDerived
// 필드의 ClassBase) 정책과 무관하게 InvalidArgument다(단일 필드·배열 원소 — ReportAndNull과 진단
// hook이 있어도 null로 바꾸지 않고 UnknownClassReference를 내지 않는다). 추상 클래스와 자기 클래스
// Base는 받는다. 미등록 ID는 기본 정책에서 NotFound(ReportAndNull이면 받는다), Guid가 아닌 wire는
// TypeMismatch다.
TEST(ClassRefGraph_DecodeRejectsNonClassAndBaseViolations) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(allocator, types, {0x435247, 5});
        struct Case {
            const char* field;
            TypeId value;
            ErrorCode expected;
        };

        const Case cases[] = {{"any", TypeIdOf<ClassStranger>(), ErrorCode::InvalidArgument},
                              {"any", TypeIdOf<ClassValue>(), ErrorCode::InvalidArgument},
                              {"any", TypeIdOf<ClassBase>(), ErrorCode::None},
                              {"any", TypeId{0x435247, 0x99}, ErrorCode::NotFound},
                              {"derived", TypeIdOf<ClassBase>(), ErrorCode::InvalidArgument},
                              {"derived", TypeIdOf<ClassLeaf>(), ErrorCode::None},
                              {"self", TypeIdOf<ClassHolder>(), ErrorCode::None},
                              {"self", TypeIdOf<ClassDerived>(), ErrorCode::InvalidArgument}};
        for (const Case& item : cases) {
            Array<uint8> payload = GuidPayload(allocator, item.value);
            Array<uint8> record =
                OneFieldRecord(allocator, TypeIdOf<ClassHolder>(), 1,
                               HolderField(StringView(item.field)), WireType::Guid, payload.View());
            Array<uint8> graph = Graph(allocator, record.View());
            CHECK(Validate(allocator, objects, types, graph.View()) == item.expected);
            Recorder recorder;
            ObjectGraphLoadOptions replacing;
            replacing.policy.unknownClass = UnknownClassPolicy::ReportAndNull;
            replacing.hooks = &recorder;
            const ErrorCode replaced =
                item.expected == ErrorCode::NotFound ? ErrorCode::None : item.expected;
            CHECK(Validate(allocator, objects, types, graph.View(), replacing) == replaced);
            CHECK(recorder.Count(FieldDiagnosticKind::UnknownClassReference) ==
                  (item.expected == ErrorCode::NotFound ? 1u : 0u));
        }

        const Guid good[] = {TypeIdOf<ClassDerived>(), Guid{}, TypeIdOf<ClassBase>()};
        const Guid bad[] = {TypeIdOf<ClassDerived>(), TypeIdOf<ClassStranger>()};
        const Span<const Guid> arrays[] = {Span<const Guid>(good, 3), Span<const Guid>(bad, 2)};
        const ErrorCode expected[] = {ErrorCode::None, ErrorCode::InvalidArgument};
        for (usize index = 0; index < 2; ++index) {
            Array<uint8> payload = GuidArrayPayload(allocator, arrays[index]);
            Array<uint8> record =
                OneFieldRecord(allocator, TypeIdOf<ClassHolder>(), 1, HolderField("list"),
                               WireType::ValueArray, payload.View());
            Array<uint8> graph = Graph(allocator, record.View());
            CHECK(Validate(allocator, objects, types, graph.View()) == expected[index]);
            Recorder recorder;
            ObjectGraphLoadOptions replacing;
            replacing.policy.unknownClass = UnknownClassPolicy::ReportAndNull;
            replacing.hooks = &recorder;
            CHECK(Validate(allocator, objects, types, graph.View(), replacing) == expected[index]);
            CHECK(recorder.Count(FieldDiagnosticKind::UnknownClassReference) == 0);
        }

        ArchiveWriter wrongWire(allocator);
        REQUIRE(wrongWire.WriteU32(1));
        Array<uint8> payload = wrongWire.TakeBytes();
        Array<uint8> record = OneFieldRecord(allocator, TypeIdOf<ClassHolder>(), 1,
                                             HolderField("any"), WireType::U32, payload.View());
        Array<uint8> graph = Graph(allocator, record.View());
        CHECK(Validate(allocator, objects, types, graph.View()) == ErrorCode::TypeMismatch);
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// 저장은 C++가 직접 넣은 값을 다시 판정한다: 무관한 클래스·struct ID는 InvalidArgument, 미등록 ID는
// NotFound로 저장이 실패한다. null Required는 rejectMissingRequired일 때 RequiredMissing 진단 뒤
// InvalidArgument이고, 그 정책이 없으면 저장된다.
TEST(ClassRefGraph_SaveRechecksDirectValuesAndRequired) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(allocator, types, {0x435247, 6});
        const auto access = objects.OwnerAccess();
        RootRef<ClassHolder> holder = New<ClassHolder>(objects);
        With(objects, holder.Handle(), [](ClassHolder& value) noexcept {
            value.required = ClassRef<ClassBase>::Of<ClassDerived>();
        });
        const struct {
            TypeId value;
            ErrorCode expected;
        } cases[] = {{TypeIdOf<ClassStranger>(), ErrorCode::InvalidArgument},
                     {TypeIdOf<ClassValue>(), ErrorCode::InvalidArgument},
                     {TypeId{0x435247, 0x98}, ErrorCode::NotFound}};
        for (const auto& item : cases) {
            With(objects, holder.Handle(),
                 [&](ClassHolder& value) noexcept { value.any = ClassRef<ClassBase>(item.value); });
            auto saved = SaveObjectGraph(allocator, objects, access, holder.Handle());
            CHECK(!saved && saved.Error() == item.expected);
            // 배열 원소도 같은 판정이다.
            With(objects, holder.Handle(), [&](ClassHolder& value) noexcept {
                value.any = ClassRef<ClassBase>();
                REQUIRE(value.list.TryPushBack(ClassRef<ClassBase>(item.value)));
            });
            saved = SaveObjectGraph(allocator, objects, access, holder.Handle());
            CHECK(!saved && saved.Error() == item.expected);
            // struct 멤버도 같은 판정이다.
            With(objects, holder.Handle(), [&](ClassHolder& value) noexcept {
                value.list.Clear();
                value.slot.kind = ClassRef<ClassBase>(item.value);
            });
            saved = SaveObjectGraph(allocator, objects, access, holder.Handle());
            CHECK(!saved && saved.Error() == item.expected);
            With(objects, holder.Handle(),
                 [](ClassHolder& value) noexcept { value.slot.kind = ClassRef<ClassBase>(); });
        }

        With(objects, holder.Handle(),
             [](ClassHolder& value) noexcept { value.required = ClassRef<ClassBase>(); });
        Recorder recorder;
        ObjectGraphSaveOptions strict;
        strict.policy.rejectMissingRequired = true;
        strict.hooks = &recorder;
        auto rejected = SaveObjectGraph(allocator, objects, access, holder.Handle(), strict);
        CHECK(!rejected && rejected.Error() == ErrorCode::InvalidArgument);
        CHECK(recorder.Count(FieldDiagnosticKind::RequiredMissing) == 1);
        CHECK(SaveObjectGraph(allocator, objects, access, holder.Handle()));
        holder.Reset();
        REQUIRE(objects.Shutdown(access));
    }

    CHECK(types.Shutdown());
}

// 2단계는 TypeRegistry를 다시 조회하지 않고 1단계 판정 기록을 쓴다(리뷰 재현 시나리오를 명시적
// 순서로 고정한다): ClassLeaf가 없는 Registry에서 ReportAndNull로 Validate한 뒤(값 4개 null
// 판정·진단), ApplyFields 전에 Base와 무관한 클래스(ClassStranger 별칭)를 ClassLeaf의 TypeId로
// 등록해도 적용 결과는 1단계 판정대로 null이고 객체는 Base의 파생이 아닌 클래스 ID를 쥐지 않는다.
// 등록된 값은 그대로다.
TEST(ClassRefGraph_ApplyUsesValidateDecisionsDespiteRegistryChange) {
    SystemAllocator allocator;
    TypeRegistry sourceTypes(allocator);
    TypeRegistry targetTypes(allocator);
    REQUIRE(RegisterFixture(sourceTypes));
    REQUIRE(RegisterFixture(targetTypes, false));
    {
        ObjectRegistry source(allocator, sourceTypes, {0x435247, 7});
        ObjectRegistry target(allocator, targetTypes, {0x435247, 8});
        RootRef<ClassHolder> holder = New<ClassHolder>(source);
        Fill(source, holder.Handle());
        auto bytes = SaveObjectGraph(allocator, source, source.OwnerAccess(), holder.Handle());
        REQUIRE(bytes);

        Recorder recorder;
        ObjectGraphLoadOptions options;
        options.policy.unknownClass = UnknownClassPolicy::ReportAndNull;
        options.hooks = &recorder;
        RootRef<Object> loaded[1];
        {
            ObjectGraphStage stage(allocator, target, targetTypes, target.OwnerAccess(), options);
            ObjectGraphInput input;
            input.bytes = bytes.Value().View();
            REQUIRE(stage.Validate(input));
            CHECK(recorder.Count(FieldDiagnosticKind::UnknownClassReference) == 4);

            // 1단계와 2단계 사이의 등록 변경: ClassLeaf의 TypeId가 이제 Base와 무관한 클래스다.
            TypeSchema stranger = TypeOf<ClassStranger>();
            stranger.id = TypeIdOf<ClassLeaf>();
            stranger.toolName = {};
            REQUIRE(targetTypes.Register(stranger, GeneratedType<ClassStranger>::Ops()));

            ConstructionContext transaction(target, target.OwnerAccess());
            REQUIRE(stage.CreateObjects(transaction, {}));
            REQUIRE(stage.ApplyFields(transaction));
            REQUIRE(stage.CheckRequired(transaction));
            const ObjectHandle roots[] = {stage.Nodes()[0]};
            REQUIRE(transaction.Commit(roots, loaded));
        }

        With(target, loaded[0].Handle(), [](ClassHolder& value) noexcept {
            CHECK(value.any.Id() == TypeIdOf<ClassBase>());
            CHECK(value.required.IsNull());
            CHECK(value.derived.IsNull());
            REQUIRE(value.list.Size() == 3);
            CHECK(value.list[0].Id() == TypeIdOf<ClassDerived>());
            CHECK(value.list[2].IsNull());
            REQUIRE(value.slots.Size() == 1);
            CHECK(value.slots[0].kind.IsNull());
        });
        holder.Reset();
        loaded[0].Reset();
        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(sourceTypes.Shutdown());
    CHECK(targetTypes.Shutdown());
}

// 정적 Base도 decode가 본다: ClassHolder의 별칭 스키마가 derived 필드의 Base를 ClassBase로 넓혀도
// (스키마 판정은 통과) 생성 thunk의 정적 Base(ClassDerived)의 파생이 아닌 ClassBase 값은
// InvalidArgument다. ClassLeaf는 받는다.
TEST(ClassRefGraph_DecodeChecksStaticBase) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        const TypeSchema& generated = TypeOf<ClassHolder>();
        REQUIRE(generated.fields.Size() <= 16);
        FieldSchema fields[16];
        const Guid derived = HolderField("derived");
        for (usize index = 0; index < generated.fields.Size(); ++index) {
            fields[index] = generated.fields[index];
            if (fields[index].id == derived) {
                fields[index].value.type = TypeIdOf<ClassBase>();
            }
        }

        TypeSchema alias = generated;
        alias.id = TypeId{0x435247, 0x50};
        alias.toolName = {};
        alias.fields = Span<const FieldSchema>(fields, generated.fields.Size());
        REQUIRE(types.Register(alias, GeneratedType<ClassHolder>::Ops()));
        ObjectRegistry objects(allocator, types, {0x435247, 9});
        const TypeId values[] = {TypeIdOf<ClassBase>(), TypeIdOf<ClassLeaf>()};
        const ErrorCode expected[] = {ErrorCode::InvalidArgument, ErrorCode::None};
        for (usize index = 0; index < 2; ++index) {
            Array<uint8> payload = GuidPayload(allocator, values[index]);
            Array<uint8> record =
                OneFieldRecord(allocator, alias.id, 1, derived, WireType::Guid, payload.View());
            const test::graph::Node node{alias.id, 0, {}, record.View()};
            auto built = test::graph::Build(allocator, Span<const test::graph::Node>(&node, 1));
            REQUIRE(built);
            CHECK(Validate(allocator, objects, types, built.Value().View()) == expected[index]);
        }

        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}
