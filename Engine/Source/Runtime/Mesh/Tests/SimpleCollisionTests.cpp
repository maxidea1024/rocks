#include <AssetRuntime/Tests/AssetTestSupport.h>
#include <AssetTestFixtures/MeshAssets.generated.h>
#include <AssetTestFixtures/MeshAssets.h>
#include <Base/Endian.h>
#include <Core/Math/RandomStream.h>
#include <Mesh/SimpleCollision.generated.h>
#include <Mesh/SimpleCollision.h>
#include <Platform/Memory.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>
#include <stdio.h>

// 단순 충돌 값 타입 시험이다(Docs/Design/MeshAnimationAssets 4.6·4.8절, 12.6절): UE 기본값,
// 형상 규칙마다 하나씩 어기기, 볼록 형상 점·index 규칙, 패키지 왕복(볼록 bulk 포함)·변조, 할당
// 실패, 퍼징, 진단 이름.
using namespace rocks;
namespace simple_collision_tests_file {

using asset_fixture::TestCollisionAsset;

constexpr uint64 CollisionFuzzSeed = 0x4d4133614b4f4c4cull;
constexpr uint32 CollisionFuzzCount = 400;

inline TypeId CollisionAssetType() noexcept {
    return GeneratedType<TestCollisionAsset>::Id();
}

double NaN() noexcept {
    return platform::BitCast<double>(0x7ff8000000000000ull);
}

double Infinity() noexcept {
    return platform::BitCast<double>(0x7ff0000000000000ull);
}

// 정육면체 꼭짓점 8개와 삼각형 12개(닫힌 hull — 2 × 8 − 4 = 12)다.
bool BuildCube(ConvexGeometry& geometry, double half) noexcept {
    for (uint32 corner = 0; corner < 8; ++corner) {
        const Vec3d point((corner & 1) != 0 ? half : -half, (corner & 2) != 0 ? half : -half,
                          (corner & 4) != 0 ? half : -half);
        if (!geometry.vertices.TryPushBack(point)) {
            return false;
        }
    }

    const uint32 triangles[] = {0, 2, 1, 1, 2, 3, 4, 5, 6, 5, 7, 6, 0, 1, 4, 1, 5, 4,
                                2, 6, 3, 3, 6, 7, 0, 4, 2, 2, 4, 6, 1, 3, 5, 3, 7, 5};
    return geometry.indices.TryAppend(Span<const uint32>(triangles, 36)).IsOk();
}

// 모든 형상 종류를 담은 정상 충돌이다(볼록 형상 둘은 bulk에 저장한다).
bool FillCollision(asset_test::AssetEnv& env, SimpleCollision& collision,
                   MemoryBulkSource& bulk) noexcept {
    auto name = [&](StringView text) noexcept { return env.names.TryIntern(text).Value(); };
    CollisionSphere sphere;
    sphere.name = name("Head");
    sphere.center = Vec3d(0.0, 0.0, 150.0);
    sphere.radius = 12.5f;
    sphere.restOffset = -0.5f;
    CollisionBox box;
    box.center = Vec3d(1.0, 2.0, 3.0);
    box.rotation = Rotator<double>(10.0, 400.0, -30.0);
    box.size = Vec3d(100.0, 50.0, 20.0);
    box.collisionEnabled = CollisionEnabled::QueryOnly;
    box.contributeToMass = false;
    CollisionCapsule capsule;
    capsule.radius = 30.0f;
    capsule.length = 0.0f;
    capsule.collisionEnabled = CollisionEnabled::QueryAndProbe;
    CollisionTaperedCapsule tapered;
    tapered.radius0 = 5.0f;
    tapered.radius1 = 2.0f;
    tapered.length = 40.0f;
    CollisionCylinder cylinder;
    cylinder.name = name("Barrel");
    cylinder.radius = 25.0f;
    cylinder.length = 80.0f;
    cylinder.rotation = Rotator<double>(0.0, 0.0, 90.0);
    if (!collision.spheres.TryPushBack(sphere) || !collision.boxes.TryPushBack(box) ||
        !collision.boxes.TryPushBack(CollisionBox{}) || !collision.capsules.TryPushBack(capsule) ||
        !collision.taperedCapsules.TryPushBack(tapered) ||
        !collision.cylinders.TryPushBack(cylinder)) {
        return false;
    }

    for (uint32 at = 0; at < 2; ++at) {
        ConvexGeometry geometry(env.allocator);
        if (!BuildCube(geometry, 10.0 + at)) {
            return false;
        }

        if (at == 1) {
            geometry.indices.Clear();
        }

        CollisionConvex convex;
        convex.name = name(at == 0 ? "Hull" : "Points");
        convex.transform = Transform(Quatd(0.0, 0.0, 0.70710678118654752, 0.70710678118654752),
                                     Vec3d(5.0, 0.0, -2.0), Vec3d(1.0, -1.0, 2.0));
        if (!StoreConvexGeometry(geometry, bulk, convex) ||
            !collision.convexes.TryPushBack(convex)) {
            return false;
        }
    }

    return true;
}

// UE 기본값과 열거 값(4.8절 1).
TEST(Collision_Defaults) {
    const CollisionSphere sphere;
    CHECK(sphere.radius == 1.0f && sphere.contributeToMass && sphere.restOffset == 0.0f &&
          sphere.collisionEnabled == CollisionEnabled::QueryAndPhysics && sphere.name.IsNone());
    const CollisionBox box;
    CHECK(box.size == Vec3d(1.0, 1.0, 1.0));
    const CollisionCapsule capsule;
    CHECK(capsule.radius == 1.0f && capsule.length == 1.0f);
    const CollisionTaperedCapsule tapered;
    CHECK(tapered.radius0 == 1.0f && tapered.radius1 == 1.0f && tapered.length == 1.0f);
    const CollisionCylinder cylinder;
    CHECK(cylinder.radius == 1.0f && cylinder.length == 1.0f);
    const CollisionConvex convex;
    CHECK(convex.vertices.bulkId == 0 && convex.transform.scale == Vec3d(1.0, 1.0, 1.0));
    CHECK(static_cast<uint32>(CollisionEnabled::QueryAndProbe) == 5 &&
          static_cast<uint32>(CollisionComplexity::ComplexAsSimple) == 3);
    SystemAllocator allocator;
    SimpleCollision empty(allocator);
    CHECK(empty.ElementCount() == 0 && ValidateSimpleCollision(empty));
}

// 형상 규칙마다 하나씩 어기기(4.8절 2·3·4): 위치·회전·restOffset 비유한, 크기 0·음수·비유한(성분
// 번호), 길이 음수(0은 받음), 실린더 길이 0, 볼록 transform(비유한·정규화·크기 0), 형상 수 상한.
TEST(Collision_ValidateRules) {
    asset_test::AssetEnv env;
    REQUIRE(env.Ready());
    MemoryBulkSource bulk(env.allocator);
    SimpleCollision good(env.allocator);
    REQUIRE(FillCollision(env, good, bulk));
    REQUIRE(ValidateSimpleCollision(good));
    struct Case {
        CollisionIssue issue;
        SimpleCollisionShapeKind shape;
        uint32 index;
        uint32 related;
    };

    auto expect = [&](auto&& mutate, Case expected) noexcept {
        SimpleCollision collision(env.allocator);
        REQUIRE(FillCollision(env, collision, bulk));
        mutate(collision);
        CollisionDiagnostic failure;
        auto result = ValidateSimpleCollision(collision, &failure);
        CHECK(!result && result.Error() == ErrorCode::InvalidArgument);
        CHECK(failure.code == AssetDiagnosticCode::InvalidCollisionData &&
              failure.issue == expected.issue && failure.shape == expected.shape &&
              failure.index == expected.index && failure.related == expected.related);
        CHECK(!ValidateSimpleCollision(collision));
    };

    constexpr uint32 none = CollisionNoIndex;
    expect([](SimpleCollision& c) noexcept { c.spheres[0].center.y = NaN(); },
           {CollisionIssue::NonFiniteValue, SimpleCollisionShapeKind::Sphere, 0, none});
    expect([](SimpleCollision& c) noexcept { c.spheres[0].restOffset = static_cast<float>(NaN()); },
           {CollisionIssue::NonFiniteValue, SimpleCollisionShapeKind::Sphere, 0, none});
    expect([](SimpleCollision& c) noexcept { c.spheres[0].radius = 0.0f; },
           {CollisionIssue::NonPositiveSize, SimpleCollisionShapeKind::Sphere, 0, 0});
    expect(
        [](SimpleCollision& c) noexcept { c.spheres[0].radius = static_cast<float>(Infinity()); },
        {CollisionIssue::NonFiniteValue, SimpleCollisionShapeKind::Sphere, 0, 0});
    expect([](SimpleCollision& c) noexcept { c.boxes[1].rotation.roll = Infinity(); },
           {CollisionIssue::NonFiniteValue, SimpleCollisionShapeKind::Box, 1, none});
    expect([](SimpleCollision& c) noexcept { c.boxes[1].size.z = -1.0; },
           {CollisionIssue::NonPositiveSize, SimpleCollisionShapeKind::Box, 1, 2});
    expect([](SimpleCollision& c) noexcept { c.boxes[0].size.y = NaN(); },
           {CollisionIssue::NonFiniteValue, SimpleCollisionShapeKind::Box, 0, 1});
    expect([](SimpleCollision& c) noexcept { c.capsules[0].center.z = Infinity(); },
           {CollisionIssue::NonFiniteValue, SimpleCollisionShapeKind::Capsule, 0, none});
    expect([](SimpleCollision& c) noexcept { c.capsules[0].radius = -2.0f; },
           {CollisionIssue::NonPositiveSize, SimpleCollisionShapeKind::Capsule, 0, 0});
    expect([](SimpleCollision& c) noexcept { c.capsules[0].length = -0.5f; },
           {CollisionIssue::NegativeLength, SimpleCollisionShapeKind::Capsule, 0, none});
    expect([](SimpleCollision& c) noexcept { c.capsules[0].length = static_cast<float>(NaN()); },
           {CollisionIssue::NonFiniteValue, SimpleCollisionShapeKind::Capsule, 0, none});
    expect([](SimpleCollision& c) noexcept { c.taperedCapsules[0].radius1 = 0.0f; },
           {CollisionIssue::NonPositiveSize, SimpleCollisionShapeKind::TaperedCapsule, 0, 1});
    expect([](SimpleCollision& c) noexcept { c.taperedCapsules[0].length = -1.0f; },
           {CollisionIssue::NegativeLength, SimpleCollisionShapeKind::TaperedCapsule, 0, none});
    expect([](SimpleCollision& c) noexcept { c.taperedCapsules[0].center.x = Infinity(); },
           {CollisionIssue::NonFiniteValue, SimpleCollisionShapeKind::TaperedCapsule, 0, none});
    expect([](SimpleCollision& c) noexcept { c.cylinders[0].length = 0.0f; },
           {CollisionIssue::NonPositiveSize, SimpleCollisionShapeKind::Cylinder, 0, 1});
    expect([](SimpleCollision& c) noexcept { c.cylinders[0].rotation.pitch = NaN(); },
           {CollisionIssue::NonFiniteValue, SimpleCollisionShapeKind::Cylinder, 0, none});
    expect(
        [](SimpleCollision& c) noexcept { c.convexes[1].restOffset = static_cast<float>(NaN()); },
        {CollisionIssue::NonFiniteValue, SimpleCollisionShapeKind::Convex, 1, none});
    expect([](SimpleCollision& c) noexcept { c.convexes[0].transform.translation.z = NaN(); },
           {CollisionIssue::NonFiniteValue, SimpleCollisionShapeKind::Convex, 0, none});
    expect([](SimpleCollision& c) noexcept { c.convexes[0].transform.scale.x = Infinity(); },
           {CollisionIssue::NonFiniteValue, SimpleCollisionShapeKind::Convex, 0, none});
    expect([](SimpleCollision& c) noexcept { c.convexes[0].transform.rotation.w = NaN(); },
           {CollisionIssue::NonFiniteValue, SimpleCollisionShapeKind::Convex, 0, none});
    expect(
        [](SimpleCollision& c) noexcept { c.convexes[1].transform.rotation = Quatd(0, 0, 0, 1.2); },
        {CollisionIssue::TransformRotation, SimpleCollisionShapeKind::Convex, 1, none});
    expect([](SimpleCollision& c) noexcept { c.convexes[1].transform.scale.y = 0.0; },
           {CollisionIssue::TransformScale, SimpleCollisionShapeKind::Convex, 1, 1});
    // 허용 경계: 길이 0 캡슐·테이퍼드, 음수 restOffset, 큰 회전 값, 정규화 허용치 안쪽 회전, 음수
    // 크기.
    SimpleCollision edge(env.allocator);
    REQUIRE(FillCollision(env, edge, bulk));
    edge.taperedCapsules[0].length = 0.0f;
    edge.convexes[0].transform.rotation = Quatd(0, 0, 0, 1.004);
    edge.convexes[0].transform.scale = Vec3d(-1.0, 1.0, 1.0);
    CHECK(ValidateSimpleCollision(edge));
    // 형상 수 상한(경계 포함).
    SimpleCollision many(env.allocator);
    REQUIRE(many.spheres.TryResize(CollisionMaxElements));
    CHECK(ValidateSimpleCollision(many));
    REQUIRE(many.boxes.TryResize(1));
    CollisionDiagnostic failure;
    auto result = ValidateSimpleCollision(many, &failure);
    CHECK(!result && result.Error() == ErrorCode::CapacityExceeded);
    CHECK(failure.issue == CollisionIssue::TooManyElements && failure.index == CollisionNoIndex);
}

// 볼록 형상 점·index 규칙(4.8절 3): 점 수 하한·상한(경계 포함), 비유한 점(점 번호), index 수,
// 삼각형 수 상한(2V − 4), index 범위(index 위치). index가 없어도 된다.
TEST(Collision_ConvexGeometryRules) {
    SystemAllocator allocator;
    auto expect = [&](auto&& mutate, CollisionIssue issue, uint32 related,
                      ErrorCode error) noexcept {
        ConvexGeometry geometry(allocator);
        REQUIRE(BuildCube(geometry, 1.0));
        mutate(geometry);
        CollisionDiagnostic failure;
        auto result = ValidateConvexGeometry(geometry, &failure);
        CHECK(!result && result.Error() == error);
        CHECK(failure.code == AssetDiagnosticCode::InvalidCollisionData && failure.issue == issue &&
              failure.shape == SimpleCollisionShapeKind::Convex && failure.related == related);
        CHECK(!ValidateConvexGeometry(geometry));
    };

    ConvexGeometry cube(allocator);
    REQUIRE(BuildCube(cube, 1.0));
    CHECK(ValidateConvexGeometry(cube));
    cube.indices.Clear();
    CHECK(ValidateConvexGeometry(cube));
    expect(
        [](ConvexGeometry& g) noexcept {
            (void)g.vertices.TryResize(3);
            g.indices.Clear();
        },
        CollisionIssue::TooFewVertices, CollisionNoIndex, ErrorCode::InvalidArgument);
    expect([](ConvexGeometry& g) noexcept { g.vertices[5].z = Infinity(); },
           CollisionIssue::NonFiniteVertex, 5, ErrorCode::InvalidArgument);
    expect([](ConvexGeometry& g) noexcept { (void)g.indices.TryPushBack(0); },
           CollisionIssue::IndexCount, CollisionNoIndex, ErrorCode::InvalidArgument);
    expect([](ConvexGeometry& g) noexcept { g.indices[7] = 8; }, CollisionIssue::IndexRange, 7,
           ErrorCode::InvalidArgument);
    expect(
        [](ConvexGeometry& g) noexcept {
            const uint32 extra[] = {0, 1, 2};
            (void)g.indices.TryAppend(Span<const uint32>(extra, 3));
        },
        CollisionIssue::TooManyTriangles, CollisionNoIndex, ErrorCode::InvalidArgument);
    // 점 수 상한(경계 포함): 같은 점을 반복해도 데이터 규칙은 수만 본다.
    ConvexGeometry big(allocator);
    REQUIRE(big.vertices.TryResize(CollisionConvexMaxVertices));
    CHECK(ValidateConvexGeometry(big));
    REQUIRE(big.vertices.TryPushBack(Vec3d(0, 0, 0)));
    expect(
        [&](ConvexGeometry& g) noexcept {
            (void)g.vertices.TryResize(CollisionConvexMaxVertices + 1);
        },
        CollisionIssue::TooManyVertices, CollisionNoIndex, ErrorCode::CapacityExceeded);
}

// 패키지 왕복: 에디터·쿠킹 저장 → 로드 → 재저장 byte 일치, 두 번 쓴 byte 일치, 형상 값과 볼록 bulk
// (점 double bit·index) 왕복, 쿠킹 패키지의 EditorOnly 사본 제외, 로드 뒤 검증.
TEST(Collision_PackageRoundTripResaveIdentical) {
    const PackageKind kinds[] = {PackageKind::Editor, PackageKind::Cooked};
    for (PackageKind kind : kinds) {
        asset_test::AssetEnv env;
        REQUIRE(env.Ready());
        ObjectRegistry& registry = *env.source;
        auto package =
            NewPackage(registry, registry.OwnerAccess(), asset_test::FixedId(0x700), kind);
        REQUIRE(package);
        const ObjectHandle handle = asset_test::Make<TestCollisionAsset>(
            registry, CollisionAssetType(), package.Value().Handle(), "");
        TestCollisionAsset* asset = asset_test::Pointer<TestCollisionAsset>(registry, handle);
        REQUIRE(asset != nullptr);
        MemoryBulkSource bulk(env.allocator);
        REQUIRE(FillCollision(env, asset->collision, bulk));
        REQUIRE(FillCollision(env, asset->editorCollision, bulk));
        SimpleCollision variant(env.allocator);
        REQUIRE(FillCollision(env, variant, bulk));
        REQUIRE(asset->variants.TryPushBack(Move(variant)));
        asset->complexity = CollisionComplexity::ComplexAsSimple;
        PackageWriteOptions options =
            kind == PackageKind::Cooked ? asset_test::CookedOptions() : asset_test::EditorOptions();
        options.bulkSource = &bulk;
        Array<uint8> first =
            asset_test::Write(env.allocator, registry, package.Value().Handle(), options);
        REQUIRE(!first.IsEmpty());
        const Array<uint8> again =
            asset_test::Write(env.allocator, registry, package.Value().Handle(), options);
        CHECK(asset_test::SameBytes(first.View(), again.View()));
        Array<uint8> files[1] = {asset_test::Copy(env.allocator, first.View())};
        RootRef<Object> roots[1];
        ObjectHandle handles[1];
        REQUIRE(asset_test::LoadAll(env, *env.target, files, kind, nullptr, roots, handles));
        auto found = env.target->FindObject(env.target->OwnerAccess(), handles[0], StringView());
        REQUIRE(found);
        TestCollisionAsset* loaded =
            asset_test::Pointer<TestCollisionAsset>(*env.target, found.Value());
        Package* loadedPackage = asset_test::Pointer<Package>(*env.target, handles[0]);
        REQUIRE(loaded != nullptr && loadedPackage != nullptr);
        const SimpleCollision& read = loaded->collision;
        CHECK(ValidateSimpleCollision(read));
        CHECK(ValidateSimpleCollisionBulk(read, *loadedPackage, env.allocator));
        CHECK(loaded->complexity == CollisionComplexity::ComplexAsSimple);
        REQUIRE(read.spheres.Size() == 1 && read.boxes.Size() == 2 && read.capsules.Size() == 1 &&
                read.taperedCapsules.Size() == 1 && read.cylinders.Size() == 1 &&
                read.convexes.Size() == 2);
        CHECK(read.spheres[0].name == asset->collision.spheres[0].name &&
              read.spheres[0].restOffset == -0.5f && read.spheres[0].radius == 12.5f);
        CHECK(read.boxes[0].rotation.yaw == 400.0 && !read.boxes[0].contributeToMass &&
              read.boxes[0].collisionEnabled == CollisionEnabled::QueryOnly);
        CHECK(read.capsules[0].length == 0.0f && read.taperedCapsules[0].radius1 == 2.0f &&
              read.cylinders[0].rotation.roll == 90.0);
        CHECK(platform::MemoryEquals(&read.convexes[0].transform,
                                     &asset->collision.convexes[0].transform, sizeof(Transform)));
        ConvexGeometry hull(env.allocator);
        REQUIRE(LoadConvexGeometry(read.convexes[0], *loadedPackage, hull));
        ConvexGeometry expected(env.allocator);
        REQUIRE(BuildCube(expected, 10.0));
        CHECK(hull.vertices.Size() == 8 &&
              platform::MemoryEquals(hull.vertices.Data(), expected.vertices.Data(),
                                     8 * sizeof(Vec3d)));
        CHECK(hull.indices.Size() == 36 &&
              platform::MemoryEquals(hull.indices.Data(), expected.indices.Data(),
                                     36 * sizeof(uint32)));
        ConvexGeometry points(env.allocator);
        REQUIRE(LoadConvexGeometry(read.convexes[1], *loadedPackage, points));
        CHECK(points.vertices.Size() == 8 && points.indices.IsEmpty());
        REQUIRE(loaded->variants.Size() == 1);
        CHECK(ValidateSimpleCollisionBulk(loaded->variants[0], *loadedPackage, env.allocator));
        CHECK(loaded->editorCollision.convexes.Size() == (kind == PackageKind::Editor ? 2u : 0u));
        auto contents =
            ReadPackage(env.allocator, first.View(), asset_test::LoadOptions(kind).read);
        REQUIRE(contents);
        const Array<uint8> resaved =
            asset_test::ResaveAsRead(env.allocator, *env.target, handles[0], contents.Value());
        CHECK(asset_test::SameBytes(first.View(), resaved.View()));
    }
}

// 볼록 bulk 변조(로드 — CorruptPackage): 점·index bulk 길이가 원소 크기의 배수가 아님, 빈 ref,
// 내용 규칙 위반(점 수·index 범위). ValidateSimpleCollisionBulk는 볼록 형상 번호를 index로 준다.
TEST(Collision_LoadRejectsConvexBytes) {
    asset_test::AssetEnv env;
    REQUIRE(env.Ready());
    ObjectRegistry& registry = *env.source;
    auto package = NewPackage(registry, registry.OwnerAccess(), asset_test::FixedId(0x710),
                              PackageKind::Editor);
    REQUIRE(package);
    const ObjectHandle handle = asset_test::Make<TestCollisionAsset>(registry, CollisionAssetType(),
                                                                     package.Value().Handle(), "");
    TestCollisionAsset* asset = asset_test::Pointer<TestCollisionAsset>(registry, handle);
    REQUIRE(asset != nullptr);
    MemoryBulkSource bulk(env.allocator);
    ConvexGeometry cube(env.allocator);
    REQUIRE(BuildCube(cube, 1.0));
    CollisionConvex good;
    REQUIRE(StoreConvexGeometry(cube, bulk, good));
    const uint8 odd[5] = {1, 2, 3, 4, 5};
    const BulkDataRef oddRef = bulk.AddCopy(Span<const uint8>(odd, 5)).Value();
    const uint8 threePoints[3 * CollisionVertexBytes] = {};
    const BulkDataRef fewRef =
        bulk.AddCopy(Span<const uint8>(threePoints, sizeof(threePoints))).Value();
    const uint8 badTriangle[12] = {0, 0, 0, 0, 1, 0, 0, 0, 9, 0, 0, 0};
    const BulkDataRef badIndexRef = bulk.AddCopy(Span<const uint8>(badTriangle, 12)).Value();
    struct Case {
        BulkDataRef vertices;
        BulkDataRef indices;
        CollisionIssue issue;
        uint32 related;
    };

    const Case cases[] = {
        {good.vertices, good.indices, CollisionIssue::None, CollisionNoIndex},
        {oddRef, good.indices, CollisionIssue::BulkLength, 0},
        {good.vertices, oddRef, CollisionIssue::BulkLength, 1},
        {BulkDataRef{}, good.indices, CollisionIssue::BulkUnreadable, 0},
        {good.vertices, BulkDataRef{}, CollisionIssue::BulkUnreadable, 1},
        {fewRef, good.indices, CollisionIssue::TooFewVertices, CollisionNoIndex},
        {good.vertices, badIndexRef, CollisionIssue::IndexRange, 2},
    };
    for (const Case& item : cases) {
        CollisionConvex convex = good;
        convex.vertices = item.vertices;
        convex.indices = item.indices;
        REQUIRE(asset->collision.convexes.TryPushBack(convex));
    }

    PackageWriteOptions options = asset_test::EditorOptions();
    options.bulkSource = &bulk;
    Array<uint8> bytes =
        asset_test::Write(env.allocator, registry, package.Value().Handle(), options);
    REQUIRE(!bytes.IsEmpty());
    Array<uint8> files[1] = {Move(bytes)};
    RootRef<Object> roots[1];
    ObjectHandle handles[1];
    REQUIRE(
        asset_test::LoadAll(env, *env.target, files, PackageKind::Editor, nullptr, roots, handles));
    auto found = env.target->FindObject(env.target->OwnerAccess(), handles[0], StringView());
    REQUIRE(found);
    TestCollisionAsset* loaded =
        asset_test::Pointer<TestCollisionAsset>(*env.target, found.Value());
    Package* raw = asset_test::Pointer<Package>(*env.target, handles[0]);
    REQUIRE(loaded != nullptr && raw != nullptr);
    for (uint32 at = 0; at < 7; ++at) {
        ConvexGeometry out(env.allocator);
        CollisionDiagnostic failure;
        auto result = LoadConvexGeometry(loaded->collision.convexes[at], *raw, out, &failure);
        if (cases[at].issue == CollisionIssue::None) {
            CHECK(result && out.vertices.Size() == 8);
            continue;
        }

        CHECK(!result && out.vertices.IsEmpty());
        CHECK(failure.code == AssetDiagnosticCode::CorruptPackage &&
              failure.issue == cases[at].issue && failure.related == cases[at].related &&
              failure.shape == SimpleCollisionShapeKind::Convex);
        CHECK(!LoadConvexGeometry(loaded->collision.convexes[at], *raw, out));
    }

    CollisionDiagnostic failure;
    CHECK(!ValidateSimpleCollisionBulk(loaded->collision, *raw, env.allocator, &failure));
    CHECK(failure.index == 1 && failure.issue == CollisionIssue::BulkLength);
    CHECK(!ValidateSimpleCollisionBulk(loaded->collision, *raw, env.allocator));
    // 저장 거부: 검증 실패면 convex는 그대로다.
    ConvexGeometry broken(env.allocator);
    REQUIRE(broken.vertices.TryResize(2));
    CollisionConvex untouched = good;
    CollisionDiagnostic storeFailure;
    CHECK(!StoreConvexGeometry(broken, bulk, untouched, &storeFailure));
    CHECK(storeFailure.issue == CollisionIssue::TooFewVertices &&
          untouched.vertices.bulkId == good.vertices.bulkId);
}

// 할당 실패 주입: 저장·로드·bulk 검증은 몇 번째 할당이 실패해도 그 오류이고 out은 그대로이며,
// 끝까지 간 호출은 기준과 같다.
TEST(Collision_AllocationFailures) {
    asset_test::AssetEnv env;
    REQUIRE(env.Ready());
    ConvexGeometry cube(env.allocator);
    REQUIRE(BuildCube(cube, 3.0));
    MemoryBulkSource reference(env.allocator);
    CollisionConvex expected;
    REQUIRE(StoreConvexGeometry(cube, reference, expected));
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        test::NFailAllocator failing(env.allocator);
        ConvexGeometry copy(failing);
        MemoryBulkSource bulk(failing);
        failing.FailOnAttempt(attempt);
        // 복사(시험 준비)도 실패할 수 있으므로 성공한 경우만 저장을 본다.
        if (!copy.vertices.TryAppend(cube.vertices.View()) ||
            !copy.indices.TryAppend(cube.indices.View())) {
            continue;
        }

        CollisionConvex convex;
        auto result = StoreConvexGeometry(copy, bulk, convex);
        if (!result) {
            CHECK(result.Error() == ErrorCode::OutOfMemory && convex.vertices.bulkId == 0);
            ++failures;
            continue;
        }

        CHECK(convex.vertices.sha256 == expected.vertices.sha256 &&
              convex.indices.sha256 == expected.indices.sha256);
        if (failing.Attempts() < attempt) {
            break;
        }
    }

    ObjectRegistry& registry = *env.source;
    auto package = NewPackage(registry, registry.OwnerAccess(), asset_test::FixedId(0x720),
                              PackageKind::Editor);
    REQUIRE(package);
    const ObjectHandle handle = asset_test::Make<TestCollisionAsset>(registry, CollisionAssetType(),
                                                                     package.Value().Handle(), "");
    TestCollisionAsset* asset = asset_test::Pointer<TestCollisionAsset>(registry, handle);
    REQUIRE(asset != nullptr);
    MemoryBulkSource bulk(env.allocator);
    REQUIRE(FillCollision(env, asset->collision, bulk));
    PackageWriteOptions options = asset_test::EditorOptions();
    options.bulkSource = &bulk;
    Array<uint8> files[1] = {
        asset_test::Write(env.allocator, registry, package.Value().Handle(), options)};
    RootRef<Object> roots[1];
    ObjectHandle handles[1];
    REQUIRE(
        asset_test::LoadAll(env, *env.target, files, PackageKind::Editor, nullptr, roots, handles));
    auto found = env.target->FindObject(env.target->OwnerAccess(), handles[0], StringView());
    REQUIRE(found);
    TestCollisionAsset* loaded =
        asset_test::Pointer<TestCollisionAsset>(*env.target, found.Value());
    Package* raw = asset_test::Pointer<Package>(*env.target, handles[0]);
    REQUIRE(loaded != nullptr && raw != nullptr);
    for (uint64 attempt = 1;; ++attempt) {
        test::NFailAllocator failing(env.allocator);
        ConvexGeometry out(failing);
        failing.FailOnAttempt(attempt);
        auto loadResult = LoadConvexGeometry(loaded->collision.convexes[0], *raw, out);
        auto bulkResult = ValidateSimpleCollisionBulk(loaded->collision, *raw, failing);
        if (!loadResult || !bulkResult) {
            CHECK((!loadResult && loadResult.Error() == ErrorCode::OutOfMemory &&
                   out.vertices.IsEmpty()) ||
                  (!bulkResult && bulkResult.Error() == ErrorCode::OutOfMemory));
            ++failures;
            continue;
        }

        CHECK(out.vertices.Size() == 8);
        if (failing.Attempts() < attempt) {
            break;
        }
    }

    CHECK(failures > 6);
    printf("Collision_AllocationFailures: %u failures\n", failures);
}

// 고정 seed 퍼징: 무작위 형상 값(가끔 비유한·0·음수)과 무작위 볼록 점·index. 검증은 같은 입력에
// 같은 진단을 내고, 통과한 볼록 형상은 저장 byte가 결정적이다.
TEST(Collision_FuzzValues) {
    SystemAllocator allocator;
    RandomStream random(CollisionFuzzSeed);
    const double specials[] = {0.0, -1.0, NaN(), Infinity(), 1.0e-30, 5.0};
    auto value = [&]() noexcept {
        return random.NextUInt32Below(8) == 0
                   ? specials[random.NextUInt32Below(6)]
                   : static_cast<double>(random.NextFloatInRange(0.1f, 100.0f));
    };
    uint32 valid = 0;
    uint32 invalid = 0;
    for (uint32 iteration = 0; iteration < CollisionFuzzCount; ++iteration) {
        SimpleCollision collision(allocator);
        const uint32 shapes = random.NextUInt32Below(6);
        for (uint32 at = 0; at < shapes; ++at) {
            switch (random.NextUInt32Below(5)) {
            case 0: {
                CollisionSphere sphere;
                sphere.radius = static_cast<float>(value());
                sphere.center = Vec3d(value(), 0.0, 0.0);
                REQUIRE(collision.spheres.TryPushBack(sphere));
                break;
            }
            case 1: {
                CollisionBox box;
                box.size = Vec3d(value(), value(), value());
                box.rotation = Rotator<double>(value(), 0.0, 0.0);
                REQUIRE(collision.boxes.TryPushBack(box));
                break;
            }
            case 2: {
                CollisionCapsule capsule;
                capsule.radius = static_cast<float>(value());
                capsule.length = static_cast<float>(value() - 1.0);
                REQUIRE(collision.capsules.TryPushBack(capsule));
                break;
            }
            case 3: {
                CollisionTaperedCapsule tapered;
                tapered.radius0 = static_cast<float>(value());
                tapered.radius1 = static_cast<float>(value());
                REQUIRE(collision.taperedCapsules.TryPushBack(tapered));
                break;
            }
            default: {
                CollisionCylinder cylinder;
                cylinder.radius = static_cast<float>(value());
                cylinder.length = static_cast<float>(value());
                REQUIRE(collision.cylinders.TryPushBack(cylinder));
                break;
            }
            }
        }

        CollisionDiagnostic first;
        CollisionDiagnostic second;
        const bool ok = ValidateSimpleCollision(collision, &first).IsOk();
        CHECK(ok == ValidateSimpleCollision(collision, &second).IsOk());
        if (!ok) {
            CHECK(first.issue == second.issue && first.index == second.index &&
                  first.shape == second.shape);
        }

        ConvexGeometry geometry(allocator);
        const uint32 points = random.NextUInt32Below(10);
        for (uint32 at = 0; at < points; ++at) {
            REQUIRE(geometry.vertices.TryPushBack(Vec3d(value(), value(), value())));
        }

        const uint32 indices = random.NextUInt32Below(6) == 0 ? random.NextUInt32Below(13)
                                                              : 3 * random.NextUInt32Below(5);
        for (uint32 at = 0; at < indices; ++at) {
            REQUIRE(geometry.indices.TryPushBack(
                random.NextUInt32Below(random.NextUInt32Below(8) == 0 ? points + 2 : points + 1)));
        }

        MemoryBulkSource a(allocator);
        MemoryBulkSource b(allocator);
        CollisionConvex left;
        CollisionConvex right;
        const bool stored = StoreConvexGeometry(geometry, a, left).IsOk();
        CHECK(stored == ValidateConvexGeometry(geometry).IsOk());
        if (stored) {
            REQUIRE(StoreConvexGeometry(geometry, b, right));
            CHECK(left.vertices.sha256 == right.vertices.sha256 &&
                  left.indices.sha256 == right.indices.sha256);
            ++valid;
        } else {
            ++invalid;
        }
    }

    printf("Collision_FuzzValues: %u valid convexes, %u invalid\n", valid, invalid);
    CHECK(valid > 20 && invalid > 100);
}

// 진단 이름과 받지 않는 형상 경고(4.8절 6), 4.6절 변환.
TEST(Collision_DiagnosticNames) {
    constexpr uint32 last = static_cast<uint32>(CollisionIssue::UnsupportedFeature);
    for (uint32 index = 0; index <= last; ++index) {
        const StringView name(CollisionIssueName(static_cast<CollisionIssue>(index)));
        CHECK(name != StringView("Unknown"));
        for (uint32 other = 0; other < index; ++other) {
            CHECK(name != StringView(CollisionIssueName(static_cast<CollisionIssue>(other))));
        }
    }

    CHECK(StringView(CollisionIssueName(static_cast<CollisionIssue>(last + 1))) ==
          StringView("Unknown"));
    for (uint32 kind = 0; kind <= static_cast<uint32>(SimpleCollisionShapeKind::Convex); ++kind) {
        CHECK(StringView(SimpleCollisionShapeKindName(
                  static_cast<SimpleCollisionShapeKind>(kind))) != StringView("Unknown"));
    }

    CHECK(StringView(SimpleCollisionShapeKindName(static_cast<SimpleCollisionShapeKind>(7))) ==
          StringView("Unknown"));
    for (uint32 feature = 0;
         feature <= static_cast<uint32>(UnsupportedCollisionFeature::TaperedCapsuleOneSided);
         ++feature) {
        CHECK(StringView(UnsupportedCollisionFeatureName(
                  static_cast<UnsupportedCollisionFeature>(feature))) != StringView("Unknown"));
    }

    CHECK(StringView(UnsupportedCollisionFeatureName(
              static_cast<UnsupportedCollisionFeature>(6))) == StringView("Unknown"));
    const CollisionDiagnostic warning =
        MakeUnsupportedCollisionWarning(UnsupportedCollisionFeature::MLLevelSet, 3);
    CHECK(warning.code == AssetDiagnosticCode::UnsupportedCollisionShape &&
          warning.issue == CollisionIssue::UnsupportedFeature &&
          warning.related == static_cast<uint32>(UnsupportedCollisionFeature::MLLevelSet) &&
          warning.count == 3 && warning.error == ErrorCode::None);
    CHECK(ToAssetDiagnostic(warning, AssetId{}).detail == 3);
    CollisionDiagnostic placed = warning;
    placed.index = 4;
    CHECK(ToAssetDiagnostic(placed, asset_test::FixedId(2)).detail == 4);
    CHECK(StringView(AssetDiagnosticName(AssetDiagnosticCode::InvalidCollisionData)) ==
          StringView("InvalidCollisionData"));
    CHECK(StringView(AssetDiagnosticName(AssetDiagnosticCode::UnsupportedCollisionShape)) ==
          StringView("UnsupportedCollisionShape"));
}

} // namespace simple_collision_tests_file
