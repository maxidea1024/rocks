#include <ReflectionTestFixtures/SubobjectFixture.generated.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/String.h>
#include <Object/ConstructionContext.h>
#include <Object/ObjectRegistry.h>
#include <Object/TypeRegistry.h>
#include <ObjectSerialization/ClassDefaults.h>
#include <ObjectSerialization/ObjectGraph.h>
#include <Serialization/Archive.h>
#include <TestSupport/Test.h>

#include "GraphTestSupport.h"
#include <Serialization/Record.h>

// 선언 subobject의 graph 형식 3 시험이다(Docs/Design/Prefab 10.4·10.8절 PF1c): 저장이 선언
// subobject를 NodeTable 종류 1 노드로 쓰고, 복원이 새로 만들지 않고 소유 노드가 만든 subobject에
// 묶으며(이름·생성 출처·소유 구조가 그대로), 저장된 값(덮은 기본값)과 실행 중 더한 원소가 남는지,
// 사용자가 지운 Root가 표 대상으로 되살아나지 않는지, 클래스 표에 없는 SubobjectGuid를
// 거부하는지다.
using namespace rocks;
using reflection_fixture::SubobjectOwner;
using reflection_fixture::SubobjectPart;
using reflection_fixture::SubobjectScenePart;

namespace declared_subobject_graph_tests_file {

namespace {

struct Fixture {
    explicit Fixture(Allocator& allocator) noexcept
        : allocator(allocator), types(allocator), registry(allocator, types, {0x53554231, 1}) {
    }

    bool Start() noexcept {
        access = registry.OwnerAccess();
        return GeneratedType<SubobjectPart>::Register(types) &&
               GeneratedType<SubobjectScenePart>::Register(types) &&
               GeneratedType<SubobjectOwner>::Register(types);
    }

    RootRef<Object> SpawnOwner() noexcept {
        ConstructionContext transaction(registry, access);
        auto owner = transaction.Create<SubobjectOwner>(TypeIdOf<SubobjectOwner>());
        if (!owner) {
            return {};
        }

        ObjectHandle parts[MaxClassSubobjects];
        if (!transaction.CreateDeclaredSubobjects(owner.Value(), parts)) {
            return {};
        }

        auto committed = transaction.Commit();
        return committed ? Move(committed.Value()) : RootRef<Object>();
    }

    // owner의 parts·root와 선언 subobject handle을 읽는다.
    struct View {
        ObjectHandle body;
        ObjectHandle arm;
        ObjectHandle plain;
        ObjectHandle root;
        usize parts = 0;
        ObjectHandle lastPart;
    };

    View Read(ObjectHandle owner) noexcept {
        View view;
        auto borrowed = registry.Resolve<SubobjectOwner>(access, owner);
        if (!borrowed) {
            return view;
        }

        const SubobjectOwner& value = *borrowed.Value().Get().Value();
        view.body = value.Body();
        view.arm = value.Arm();
        view.plain = value.Plain();
        view.root = value.root.Handle();
        view.parts = value.parts.Size();
        view.lastPart = view.parts != 0 ? value.parts[view.parts - 1] : ObjectHandle{};
        return view;
    }

    int32 CountOf(ObjectHandle handle) noexcept {
        auto part = registry.Resolve<SubobjectPart>(access, handle);
        return part ? part.Value().Get().Value()->count : -1;
    }

    bool IsClassSubobject(ObjectHandle handle) noexcept {
        auto origin = registry.GetOrigin(access, handle);
        return origin && origin.Value() == ObjectOrigin::ClassSubobject;
    }

    Allocator& allocator;
    TypeRegistry types;
    ObjectRegistry registry;
    ObjectAccessContext access;
};

// bytes 안 needle의 첫 위치다(없으면 bytes 크기).
usize FindBytes(Span<const uint8> bytes, Span<const uint8> needle) noexcept {
    for (usize at = 0; at + needle.Size() <= bytes.Size(); ++at) {
        bool same = true;
        for (usize index = 0; same && index < needle.Size(); ++index) {
            same = bytes[at + index] == needle[index];
        }

        if (same) {
            return at;
        }
    }

    return bytes.Size();
}

} // namespace

TEST(GraphV3_DeclaredSubobjectNodeRoundTrip) {
    SystemAllocator allocator;
    Fixture fixture(allocator);
    REQUIRE(fixture.Start());
    RootRef<Object> owner = fixture.SpawnOwner();
    REQUIRE(!owner.Handle().IsNull());
    const Fixture::View before = fixture.Read(owner.Handle());

    // 저장 전 바꾼 값: arm의 count(덮은 기본값 2 → 7), 실행 중 더한 부품 하나(선언 subobject가
    // 아니다).
    {
        auto arm = fixture.registry.ResolveForWrite<SubobjectPart>(fixture.access, before.arm);
        REQUIRE(arm);
        arm.Value().Get().Value()->count = 7;
    }

    auto extra = fixture.registry.NewObject<SubobjectPart>(
        fixture.access, TypeIdOf<SubobjectPart>(), owner.Handle(), "extra");
    REQUIRE(extra);
    {
        auto view =
            fixture.registry.ResolveForWrite<SubobjectOwner>(fixture.access, owner.Handle());
        REQUIRE(view);
        REQUIRE(fixture.registry.AddReference(fixture.access, owner.Handle(),
                                              view.Value().Get().Value()->parts,
                                              extra.Value().Handle()));
    }

    auto bytes = SaveObjectGraph(allocator, fixture.registry, fixture.access, owner.Handle());
    REQUIRE(bytes);
    auto loaded = LoadObjectGraph(allocator, fixture.registry, fixture.types, fixture.access,
                                  bytes.Value().View());
    REQUIRE(loaded);
    const Fixture::View after = fixture.Read(loaded.Value().Handle());
    // 새 선언 subobject가 묶였다(원본과 다른 객체, 생성 출처 ClassSubobject, Outer = 새 소유 객체).
    CHECK(!after.body.IsNull() && after.body != before.body);
    CHECK(fixture.IsClassSubobject(after.body) && fixture.IsClassSubobject(after.arm) &&
          fixture.IsClassSubobject(after.plain));
    auto outer = fixture.registry.GetOuter(fixture.access, after.arm);
    CHECK(outer && outer.Value() == loaded.Value().Handle());
    // 소유 구조: 목록은 선언 subobject 셋 + 더한 부품(한 번씩), Root는 body.
    CHECK(after.parts == 4);
    CHECK(after.root == after.body);
    CHECK(!fixture.IsClassSubobject(after.lastPart));
    // 저장한 값: arm은 7, body는 덮은 기본값 1.
    CHECK(fixture.CountOf(after.arm) == 7);
    CHECK(fixture.CountOf(after.body) == 1);
}

TEST(DefaultSubobject_ClearedRootIsSaved) {
    SystemAllocator allocator;
    Fixture fixture(allocator);
    REQUIRE(fixture.Start());
    RootRef<Object> owner = fixture.SpawnOwner();
    REQUIRE(!owner.Handle().IsNull());
    {
        auto view =
            fixture.registry.ResolveForWrite<SubobjectOwner>(fixture.access, owner.Handle());
        REQUIRE(view);
        REQUIRE(fixture.registry.SetReference(fixture.access, owner.Handle(),
                                              view.Value().Get().Value()->root, ObjectHandle{}));
    }

    // 차이 쓰기(Editor 기준)에서도 Root 필드는 기준(null)과 같아도 쓴다 — 복원이 표 대상(body)을
    // 다시 써 넣지 않는다.
    ObjectGraphSaveOptions options;
    options.baseline = GraphBaseline::Editor;
    auto bytes =
        SaveObjectGraph(allocator, fixture.registry, fixture.access, owner.Handle(), options);
    REQUIRE(bytes);
    auto loaded = LoadObjectGraph(allocator, fixture.registry, fixture.types, fixture.access,
                                  bytes.Value().View());
    REQUIRE(loaded);
    CHECK(fixture.Read(loaded.Value().Handle()).root.IsNull());
}

TEST(DefaultSubobject_MissingSubobjectRejected) {
    SystemAllocator allocator;
    Fixture fixture(allocator);
    REQUIRE(fixture.Start());
    RootRef<Object> owner = fixture.SpawnOwner();
    REQUIRE(!owner.Handle().IsNull());
    auto bytes = SaveObjectGraph(allocator, fixture.registry, fixture.access, owner.Handle());
    REQUIRE(bytes);
    // arm 항목(종류 1)의 SubobjectGuid를 클래스 표에 없는 값으로 바꾼다.
    const Guid arm = GeneratedType<SubobjectOwner>::Schema().subobjects[1].id;
    uint8 needle[1 + sizeof(Guid)];
    needle[0] = test::graph::NodeKindDeclaredSubobject;
    ArchiveWriter guidWriter(allocator);
    REQUIRE(guidWriter.WriteGuid(arm));
    for (usize index = 0; index < sizeof(Guid); ++index) {
        needle[1 + index] = guidWriter.View()[index];
    }

    Array<uint8> corrupted(allocator);
    REQUIRE(corrupted.TryAppend(bytes.Value().View()));
    const usize at = FindBytes(corrupted.View(), needle);
    REQUIRE(at < corrupted.Size());
    corrupted[at + 1] ^= 0xFF;
    auto loaded = LoadObjectGraph(allocator, fixture.registry, fixture.types, fixture.access,
                                  corrupted.View());
    CHECK(!loaded && loaded.Error() == ErrorCode::InvalidArgument);
}

TEST(DefaultSubobject_EditorBaselineRoundTrip) {
    SystemAllocator allocator;
    Fixture fixture(allocator);
    REQUIRE(fixture.Start());
    RootRef<Object> owner = fixture.SpawnOwner();
    REQUIRE(!owner.Handle().IsNull());
    ObjectGraphSaveOptions options;
    options.baseline = GraphBaseline::Editor;
    auto bytes =
        SaveObjectGraph(allocator, fixture.registry, fixture.access, owner.Handle(), options);
    REQUIRE(bytes);
    auto loaded = LoadObjectGraph(allocator, fixture.registry, fixture.types, fixture.access,
                                  bytes.Value().View());
    REQUIRE(loaded);
    const Fixture::View after = fixture.Read(loaded.Value().Handle());
    CHECK(after.parts == 3 && after.root == after.body);
    CHECK(fixture.CountOf(after.body) == 1 && fixture.CountOf(after.arm) == 2);
}

TEST(DefaultSubobject_DestroyedSubobjectIsNotSaved) {
    SystemAllocator allocator;
    Fixture fixture(allocator);
    REQUIRE(fixture.Start());
    RootRef<Object> owner = fixture.SpawnOwner();
    REQUIRE(!owner.Handle().IsNull());
    {
        auto view =
            fixture.registry.ResolveForWrite<SubobjectOwner>(fixture.access, owner.Handle());
        REQUIRE(view);
        REQUIRE(fixture.registry.SetReference(fixture.access, owner.Handle(),
                                              view.Value().Get().Value()->ArmField(),
                                              ObjectHandle{}));
    }

    // 전부 저장(소유 필드 null을 쓴다)해도 복원은 그 subobject로 되돌린다(UE default subobject처럼
    // 선언 subobject의 파괴는 저장하지 않는다).
    auto bytes = SaveObjectGraph(allocator, fixture.registry, fixture.access, owner.Handle());
    REQUIRE(bytes);
    auto loaded = LoadObjectGraph(allocator, fixture.registry, fixture.types, fixture.access,
                                  bytes.Value().View());
    REQUIRE(loaded);
    const Fixture::View after = fixture.Read(loaded.Value().Handle());
    CHECK(!after.arm.IsNull() && fixture.IsClassSubobject(after.arm));
    CHECK(after.parts == 3);
}

TEST(DefaultSubobject_MalformedNodesRejected) {
    SystemAllocator allocator;
    Fixture fixture(allocator);
    REQUIRE(fixture.Start());
    const TypeSchema& ownerSchema = GeneratedType<SubobjectOwner>::Schema();
    RecordBuilder ownerBuilder(allocator, ownerSchema.id, ownerSchema.version);
    RecordBuilder sceneBuilder(allocator, GeneratedType<SubobjectScenePart>::Schema().id,
                               GeneratedType<SubobjectScenePart>::Schema().version);
    auto ownerRecord = ownerBuilder.Build();
    auto sceneRecord = sceneBuilder.Build();
    REQUIRE(ownerRecord && sceneRecord);
    const Guid body = ownerSchema.subobjects[0].id;
    const auto node = [&](uint8 kind, TypeId type, uint32 outer, StringView name,
                          Span<const uint8> record) noexcept {
        test::graph::Node value{type, outer, name, record};
        value.kind = kind;
        return value;
    };
    const test::graph::Node ownerNode =
        node(test::graph::NodeKindObject, ownerSchema.id, 0, {}, ownerRecord.Value().View());
    const auto load = [&](Span<const test::graph::Node> nodes) noexcept {
        auto bytes = test::graph::Build(allocator, nodes);
        if (!bytes) {
            return bytes.Error();
        }

        auto loaded = LoadObjectGraph(allocator, fixture.registry, fixture.types, fixture.access,
                                      bytes.Value().View());
        return loaded ? ErrorCode::None : loaded.Error();
    };

    // 양성 대조: 소유 노드 + body 종류 1 항목.
    const test::graph::Node valid[] = {ownerNode, node(test::graph::NodeKindDeclaredSubobject, body,
                                                       1, {}, sceneRecord.Value().View())};
    CHECK(load(valid) == ErrorCode::None);
    // 알 수 없는 종류 byte.
    const test::graph::Node unknownKind[] = {ownerNode,
                                             node(2, body, 1, {}, sceneRecord.Value().View())};
    CHECK(load(unknownKind) == ErrorCode::CorruptData);
    // 종류 1에 이름이 있다, 소유 노드가 없다.
    const test::graph::Node named[] = {ownerNode, node(test::graph::NodeKindDeclaredSubobject, body,
                                                       1, "x", sceneRecord.Value().View())};
    CHECK(load(named) == ErrorCode::CorruptData);
    const test::graph::Node orphan[] = {ownerNode, node(test::graph::NodeKindDeclaredSubobject,
                                                        body, 0, {}, sceneRecord.Value().View())};
    CHECK(load(orphan) == ErrorCode::CorruptData);
    // 소유 노드가 종류 1 노드다.
    const test::graph::Node nested[] = {
        ownerNode,
        node(test::graph::NodeKindDeclaredSubobject, body, 1, {}, sceneRecord.Value().View()),
        node(test::graph::NodeKindDeclaredSubobject, ownerSchema.subobjects[1].id, 2, {},
             sceneRecord.Value().View())};
    CHECK(load(nested) == ErrorCode::CorruptData);
    // 같은 (소유 노드, SubobjectGuid) 두 번(형제 유일성).
    const test::graph::Node twice[] = {
        ownerNode,
        node(test::graph::NodeKindDeclaredSubobject, body, 1, {}, sceneRecord.Value().View()),
        node(test::graph::NodeKindDeclaredSubobject, body, 1, {}, sceneRecord.Value().View())};
    CHECK(load(twice) == ErrorCode::CorruptData);
    // 표 이름(arm)과 같은 이름의 객체 노드는 선언 subobject 생성과 겹친다(2a AlreadyExists).
    // 도달하도록 소유 노드의 root 필드가 그 노드(local ID 2)를 가리킨다.
    ArchiveWriter rootValue(allocator);
    REQUIRE(rootValue.WriteU32(2));
    RecordBuilder clashOwnerBuilder(allocator, ownerSchema.id, ownerSchema.version);
    REQUIRE(clashOwnerBuilder.AddField(ownerSchema.subobjectRootField, WireType::U32,
                                       rootValue.View()));
    auto clashOwner = clashOwnerBuilder.Build();
    REQUIRE(clashOwner);
    const test::graph::Node clash[] = {
        node(test::graph::NodeKindObject, ownerSchema.id, 0, {}, clashOwner.Value().View()),
        node(test::graph::NodeKindObject, GeneratedType<SubobjectScenePart>::Schema().id, 1, "arm",
             sceneRecord.Value().View())};
    CHECK(load(clash) == ErrorCode::AlreadyExists);

    // NodeTable 없이(writeNodeTable 거짓) 소유 객체와 선언 subobject를 함께 저장할 수 없다.
    RootRef<Object> owner = fixture.SpawnOwner();
    REQUIRE(!owner.Handle().IsNull());
    ObjectGraphSaveOptions options;
    options.externals = GraphExternalPolicy::Include;
    const ObjectHandle set[] = {owner.Handle()};
    auto saved = SaveObjectGraphSet(allocator, fixture.registry, fixture.access, set, options);
    CHECK(!saved && saved.Error() == ErrorCode::InvalidArgument);
}

TEST(DefaultSubobject_RecordIsDeterministic) {
    SystemAllocator allocator;
    Fixture fixture(allocator);
    REQUIRE(fixture.Start());
    auto owner = fixture.types.Acquire(TypeIdOf<SubobjectOwner>());
    auto scene = fixture.types.Acquire(TypeIdOf<SubobjectScenePart>());
    auto part = fixture.types.Acquire(TypeIdOf<SubobjectPart>());
    REQUIRE(owner && scene && part);
    // 소유 클래스 표와 같은 수의 레코드를 두 벌 대조로 만들고 타입 레코드에 게시한다(두 번째 조회는
    // 같은 byte).
    auto entries = AcquireSubobjectDefaults(owner.Value(), allocator);
    REQUIRE(entries);
    REQUIRE(entries.Value().Size() == 3);
    auto again = AcquireSubobjectDefaults(owner.Value(), allocator);
    REQUIRE(again);
    CHECK(entries.Value().Data() == again.Value().Data());
    auto sceneDefaults = AcquireClassDefaults(scene.Value(), allocator);
    auto partDefaults = AcquireClassDefaults(part.Value(), allocator);
    REQUIRE(sceneDefaults && partDefaults);
    const auto same = [](Span<const uint8> left, Span<const uint8> right) noexcept {
        return test::graph::SameBytes(left, right);
    };
    // body·arm은 hook이 덮어 클래스 레코드와 다르고, 덮지 않은 plain은 클래스 레코드와 같다.
    const auto editor = ClassDefaultsConfiguration::Editor;
    CHECK(!same(entries.Value()[0].Record(editor), sceneDefaults.Value()->Record(editor)));
    CHECK(!same(entries.Value()[1].Record(editor), sceneDefaults.Value()->Record(editor)));
    CHECK(same(entries.Value()[2].Record(editor), partDefaults.Value()->Record(editor)));
}

TEST(DefaultSubobject_TableReferencesEqualBaseline) {
    SystemAllocator allocator;
    Fixture fixture(allocator);
    REQUIRE(fixture.Start());
    RootRef<Object> owner = fixture.SpawnOwner();
    REQUIRE(!owner.Handle().IsNull());
    const Fixture::View before = fixture.Read(owner.Handle());
    // body를 클래스 기본값(0)으로 되돌린다 — subobject 기본값(1)과 다르므로 차이 쓰기가 남겨야 한다
    // (클래스 레코드를 기준으로 쓰면 생략되어 복원이 hook 값 1을 다시 준다).
    {
        auto body = fixture.registry.ResolveForWrite<SubobjectPart>(fixture.access, before.body);
        REQUIRE(body);
        body.Value().Get().Value()->count = 0;
    }

    ObjectGraphSaveOptions options;
    options.baseline = GraphBaseline::Editor;
    auto bytes =
        SaveObjectGraph(allocator, fixture.registry, fixture.access, owner.Handle(), options);
    REQUIRE(bytes);
    auto loaded = LoadObjectGraph(allocator, fixture.registry, fixture.types, fixture.access,
                                  bytes.Value().View());
    REQUIRE(loaded);
    const Fixture::View after = fixture.Read(loaded.Value().Handle());
    CHECK(fixture.CountOf(after.body) == 0);
    CHECK(fixture.CountOf(after.arm) == 2);
}

TEST(DefaultSubobject_NewClassSubobjectAppearsOnLoad) {
    SystemAllocator allocator;
    Fixture fixture(allocator);
    REQUIRE(fixture.Start());
    // 클래스에 plain이 생기기 전에 저장한 것처럼 body·arm 항목만 있는 그래프다.
    const TypeSchema& ownerSchema = GeneratedType<SubobjectOwner>::Schema();
    RecordBuilder ownerBuilder(allocator, ownerSchema.id, ownerSchema.version);
    RecordBuilder sceneBuilder(allocator, GeneratedType<SubobjectScenePart>::Schema().id,
                               GeneratedType<SubobjectScenePart>::Schema().version);
    auto ownerRecord = ownerBuilder.Build();
    auto sceneRecord = sceneBuilder.Build();
    REQUIRE(ownerRecord && sceneRecord);
    test::graph::Node nodes[] = {{ownerSchema.id, 0, {}, ownerRecord.Value().View()},
                                 {ownerSchema.subobjects[0].id, 1, {}, sceneRecord.Value().View()},
                                 {ownerSchema.subobjects[1].id, 1, {}, sceneRecord.Value().View()}};
    nodes[1].kind = test::graph::NodeKindDeclaredSubobject;
    nodes[2].kind = test::graph::NodeKindDeclaredSubobject;
    auto bytes = test::graph::Build(allocator, nodes);
    REQUIRE(bytes);
    auto loaded = LoadObjectGraph(allocator, fixture.registry, fixture.types, fixture.access,
                                  bytes.Value().View());
    REQUIRE(loaded);
    const Fixture::View after = fixture.Read(loaded.Value().Handle());
    // 그래프에 없던 plain도 만들어져 목록에 든다.
    CHECK(!after.plain.IsNull() && fixture.IsClassSubobject(after.plain));
    CHECK(after.parts == 3 && after.root == after.body);
}

} // namespace declared_subobject_graph_tests_file
