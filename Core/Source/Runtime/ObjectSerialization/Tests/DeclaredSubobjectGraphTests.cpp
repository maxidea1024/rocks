#include <ReflectionTestFixtures/SubobjectFixture.generated.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/String.h>
#include <Object/ConstructionContext.h>
#include <Object/ObjectRegistry.h>
#include <Object/TypeRegistry.h>
#include <ObjectSerialization/ObjectGraph.h>
#include <Serialization/Archive.h>
#include <TestSupport/Test.h>

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
    needle[0] = 1;
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

} // namespace declared_subobject_graph_tests_file
