#pragma once

#include <AssetRuntime/AssetDiagnostic.h>
#include <AssetRuntime/BulkData.h>
#include <AssetRuntime/MemoryBulkSource.h>
#include <AssetRuntime/Package.h>
#include <Base/NumericLimits.h>
#include <Core/Containers/Array.h>
#include <Core/Math/Rotator.h>
#include <Core/Math/Transform.h>
#include <Core/Math/Vector3.h>
#include <Meta/Annotations.h>

// 메시 단순 충돌 값 타입과 검증이다(Docs/Design/MeshAnimationAssets.ko.md 4.6·4.8절, UE
// FKAggregateGeom·FKShapeElem 대응). 물리 형상으로 옮기는 것은 물리 어댑터(PhysicsJoltAdapter.ko.md
// 4절), StaticMesh 필드 연결은 MA2b다.
namespace rocks {

// 형상 수 합의 상한이다(compound 자식 번호를 16비트로 담는다 — 4.8절 4).
inline constexpr uint32 CollisionMaxElements = 65535;

// 볼록 형상 점 수의 하한(부피가 있는 hull)과 상한(데이터 상한 — 4.8절 3)이다.
inline constexpr uint32 CollisionConvexMinVertices = 4;
inline constexpr uint32 CollisionConvexMaxVertices = 65535;

// 볼록 형상 transform 회전의 정규화 허용치다(|길이² − 1| — UE THRESH_QUAT_NORMALIZED).
inline constexpr double CollisionRotationNormalizedTolerance = 0.01;

// 볼록 형상 bulk 원소 크기다(little-endian, 패딩 없음).
inline constexpr usize CollisionVertexBytes = 3 * sizeof(double);
inline constexpr usize CollisionIndexBytes = sizeof(uint32);

// 형상이 물리 엔진에서 하는 일이다(UE ECollisionEnabled — 같은 번호).
ROCKS_ENUM()
enum class CollisionEnabled : uint8 {
    NoCollision = 0,
    QueryOnly = 1,
    PhysicsOnly = 2,
    QueryAndPhysics = 3,
    ProbeOnly = 4,
    QueryAndProbe = 5
};

// 단순·복잡 충돌의 쓰임이다(UE ECollisionTraceFlag — 4.6절).
ROCKS_ENUM()
enum class CollisionComplexity : uint8 {
    Default = 0,
    SimpleAndComplex = 1,
    SimpleAsComplex = 2,
    ComplexAsSimple = 3
};

// 구다(UE FKSphereElem). center는 cm, radius > 0.
ROCKS_STRUCT()
struct CollisionSphere {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() Name name;
    ROCKS_PROPERTY() bool contributeToMass = true;
    ROCKS_PROPERTY() CollisionEnabled collisionEnabled = CollisionEnabled::QueryAndPhysics;
    ROCKS_PROPERTY() float restOffset = 0.0f;
    ROCKS_PROPERTY() Vec3d center;
    ROCKS_PROPERTY() float radius = 1.0f;
};

// 상자다(UE FKBoxElem). size는 축별 전체 길이(UE X·Y·Z)이고 세 성분 > 0, rotation은 도다.
ROCKS_STRUCT()
struct CollisionBox {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() Name name;
    ROCKS_PROPERTY() bool contributeToMass = true;
    ROCKS_PROPERTY() CollisionEnabled collisionEnabled = CollisionEnabled::QueryAndPhysics;
    ROCKS_PROPERTY() float restOffset = 0.0f;
    ROCKS_PROPERTY() Vec3d center;
    ROCKS_PROPERTY() Rotator<double> rotation;
    ROCKS_PROPERTY() Vec3d size = Vec3d(1.0, 1.0, 1.0);
};

// 캡슐이다(UE FKSphylElem). 축은 로컬 +Z, length는 원기둥 부분 길이(≥ 0), radius > 0.
ROCKS_STRUCT()
struct CollisionCapsule {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() Name name;
    ROCKS_PROPERTY() bool contributeToMass = true;
    ROCKS_PROPERTY() CollisionEnabled collisionEnabled = CollisionEnabled::QueryAndPhysics;
    ROCKS_PROPERTY() float restOffset = 0.0f;
    ROCKS_PROPERTY() Vec3d center;
    ROCKS_PROPERTY() Rotator<double> rotation;
    ROCKS_PROPERTY() float radius = 1.0f;
    ROCKS_PROPERTY() float length = 1.0f;
};

// 테이퍼드 캡슐이다(UE FKTaperedCapsuleElem). 두 끝 반경 > 0, length는 선분 길이(≥ 0). UE의
// Width·bOneSidedCollision은 두지 않는다(임포터가 UnsupportedCollisionShape로 알린다).
ROCKS_STRUCT()
struct CollisionTaperedCapsule {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() Name name;
    ROCKS_PROPERTY() bool contributeToMass = true;
    ROCKS_PROPERTY() CollisionEnabled collisionEnabled = CollisionEnabled::QueryAndPhysics;
    ROCKS_PROPERTY() float restOffset = 0.0f;
    ROCKS_PROPERTY() Vec3d center;
    ROCKS_PROPERTY() Rotator<double> rotation;
    ROCKS_PROPERTY() float radius0 = 1.0f;
    ROCKS_PROPERTY() float radius1 = 1.0f;
    ROCKS_PROPERTY() float length = 1.0f;
};

// 실린더다(Rocks 추가 형상 — CylinderShape.ko.md CY2). 축은 로컬 +Z, radius > 0, length > 0.
ROCKS_STRUCT()
struct CollisionCylinder {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() Name name;
    ROCKS_PROPERTY() bool contributeToMass = true;
    ROCKS_PROPERTY() CollisionEnabled collisionEnabled = CollisionEnabled::QueryAndPhysics;
    ROCKS_PROPERTY() float restOffset = 0.0f;
    ROCKS_PROPERTY() Vec3d center;
    ROCKS_PROPERTY() Rotator<double> rotation;
    ROCKS_PROPERTY() float radius = 1.0f;
    ROCKS_PROPERTY() float length = 1.0f;
};

// 볼록 형상이다(UE FKConvexElem). 점(Vec3d)·index(uint32 삼각형) bulk와 자체 transform을 갖는다.
// 메모리 형태는 ConvexGeometry다.
ROCKS_STRUCT()
struct CollisionConvex {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() Name name;
    ROCKS_PROPERTY() bool contributeToMass = true;
    ROCKS_PROPERTY() CollisionEnabled collisionEnabled = CollisionEnabled::QueryAndPhysics;
    ROCKS_PROPERTY() float restOffset = 0.0f;
    ROCKS_PROPERTY() BulkDataRef vertices;
    ROCKS_PROPERTY() BulkDataRef indices;
    ROCKS_PROPERTY() Transform transform;
};

// 단순 충돌 형상 묶음이다(UE FKAggregateGeom에서 받는 형상들 — 4.6절). 값 struct(참조 없음)다.
//
// 사용 예:
//   SimpleCollision collision(allocator);
//   CollisionBox box;
//   box.size = Vec3d(100.0, 50.0, 20.0);
//   collision.boxes.TryPushBack(box);
//   if (auto valid = ValidateSimpleCollision(collision, &failure); !valid) { ... }
ROCKS_STRUCT()
struct SimpleCollision {
    ROCKS_GENERATED_BODY()

    explicit SimpleCollision(Allocator& allocator) noexcept
        : spheres(allocator), boxes(allocator), capsules(allocator), taperedCapsules(allocator),
          cylinders(allocator), convexes(allocator) {
    }

    ROCKS_PROPERTY() Array<CollisionSphere> spheres;
    ROCKS_PROPERTY() Array<CollisionBox> boxes;
    ROCKS_PROPERTY() Array<CollisionCapsule> capsules;
    ROCKS_PROPERTY() Array<CollisionTaperedCapsule> taperedCapsules;
    ROCKS_PROPERTY() Array<CollisionCylinder> cylinders;
    ROCKS_PROPERTY() Array<CollisionConvex> convexes;

    // 형상 수 합이다.
    usize ElementCount() const noexcept {
        return spheres.Size() + boxes.Size() + capsules.Size() + taperedCapsules.Size() +
               cylinders.Size() + convexes.Size();
    }
};

// 볼록 형상 점·index의 메모리 형태다(4.8절 3).
struct ConvexGeometry {
    explicit ConvexGeometry(Allocator& allocator) noexcept
        : vertices(allocator), indices(allocator) {
    }

    Array<Vec3d> vertices;
    Array<uint32> indices;
};

// 진단이 가리키는 형상 종류다.
enum class CollisionShapeKind : uint8 {
    None,
    Sphere,
    Box,
    Capsule,
    TaperedCapsule,
    Cylinder,
    Convex
};

// 받지 않는 UE 충돌 형상·값이다(UnsupportedCollisionShape의 related — 4.8절 6).
enum class UnsupportedCollisionFeature : uint8 {
    LevelSet,
    SkinnedLevelSet,
    MLLevelSet,
    SkinnedTriangleMesh,
    TaperedCapsuleWidth,
    TaperedCapsuleOneSided
};

// 진단이 가리키는 세부 규칙이다. 값은 저장 형식이 아니다.
enum class CollisionIssue : uint8 {
    None,
    TooManyElements,   // 형상 수 합 > CollisionMaxElements
    NonFiniteValue,    // 중심·회전·크기·restOffset·transform 성분이 비유한
    NonPositiveSize,   // 반경·상자 size·실린더 길이가 0 이하
    NegativeLength,    // 캡슐·테이퍼드 캡슐 length < 0
    TransformRotation, // 볼록 transform 회전이 정규화되지 않음
    TransformScale,    // 볼록 transform 크기 성분이 0
    TooFewVertices,    // 볼록 점 수 < CollisionConvexMinVertices
    TooManyVertices,   // 볼록 점 수 > CollisionConvexMaxVertices
    NonFiniteVertex,   // 볼록 점 성분이 비유한(index = 형상, related = 점)
    IndexCount,        // index 수가 3의 배수가 아님
    IndexRange,        // index >= 점 수(related = index 위치)
    TooManyTriangles,  // 삼각형 수 > 2 × 점 수 − 4
    BulkUnreadable,    // bulk를 읽을 수 없음(로드 — CorruptPackage)
    BulkLength,        // bulk 길이가 원소 크기의 배수가 아님(로드 — CorruptPackage)
    UnsupportedFeature // 경고: 받지 않는 형상·값(related = UnsupportedCollisionFeature)
};

// 이름이다(정적 수명, 모르는 값은 "Unknown").
ROCKS_MESH_API const char* CollisionIssueName(CollisionIssue issue) noexcept;
ROCKS_MESH_API const char* CollisionShapeKindName(CollisionShapeKind kind) noexcept;
ROCKS_MESH_API const char*
UnsupportedCollisionFeatureName(UnsupportedCollisionFeature feature) noexcept;

// 진단이 위치를 가리키지 않을 때의 번호다.
inline constexpr uint32 CollisionNoIndex = NumericLimits<uint32>::Max();

// 단순 충돌 진단 하나다. 실패한 호출이 out 인자로 채운다(성공하면 건드리지 않는다).
struct CollisionDiagnostic {
    // AssetSystem 4.6절 진단
    // 이름이다(InvalidCollisionData·CorruptPackage·UnsupportedCollisionShape). 할당 오류는
    // None이다.
    AssetDiagnosticCode code = AssetDiagnosticCode::None;
    ErrorCode error = ErrorCode::None;
    CollisionIssue issue = CollisionIssue::None;
    CollisionShapeKind shape = CollisionShapeKind::None;
    // 그 형상 배열의 번호다. 없으면 CollisionNoIndex다.
    uint32 index = CollisionNoIndex;
    // 관련 번호(성분·점·index 위치·feature)다. 없으면 CollisionNoIndex다.
    uint32 related = CollisionNoIndex;
    // 경고가 센 수다.
    uint64 count = 0;
};

// diagnostic을 AssetSystem 4.6절 형식으로 옮긴다: code·error는 그대로, asset은 인자, section은 0,
// detail은 index(CollisionNoIndex면 count)다.
ROCKS_MESH_API AssetDiagnostic ToAssetDiagnostic(const CollisionDiagnostic& diagnostic,
                                                 AssetId asset) noexcept;

// 임포터가 받지 않는 형상·값을 버릴 때 낼 경고다(code UnsupportedCollisionShape, issue
// UnsupportedFeature, related = feature, count = 버린 수).
ROCKS_MESH_API CollisionDiagnostic
MakeUnsupportedCollisionWarning(UnsupportedCollisionFeature feature, uint64 count) noexcept;

// collision의 값 필드가 4.8절 2–4 규칙을 지키는지 본다(형상 수, 그다음
// 구·상자·캡슐·테이퍼드·실린더· 볼록 순, 배열 번호 순, 첫 위반에서 멈춘다). 볼록 형상의 bulk는 보지
// 않는다.
//
// 오류: 위반은 failure->code InvalidCollisionData, ErrorCode InvalidArgument(형상 수만
// CapacityExceeded)와 issue·shape·index·related다. 스레드: collision을 읽기만 한다.
ROCKS_MESH_API Result<void>
ValidateSimpleCollision(const SimpleCollision& collision,
                        CollisionDiagnostic* failure = nullptr) noexcept;

// geometry가 4.8절 3의 점·index 규칙을 지키는지 본다(점 수, 점 유한, index 수, 범위, 삼각형 수).
// 오류: InvalidCollisionData·InvalidArgument(점 수 상한만 CapacityExceeded)와
// issue·related다(shape는 Convex, index는 CollisionNoIndex — 호출자가 채운다).
ROCKS_MESH_API Result<void> ValidateConvexGeometry(const ConvexGeometry& geometry,
                                                   CollisionDiagnostic* failure = nullptr) noexcept;

// geometry를 bulk entry 둘(점·index)로 쓰고 convex의 vertices·indices를 바꾼다. 먼저
// ValidateConvexGeometry로 본다. 같은 내용이면 같은 byte다.
//
// 오류: 검증 실패는 그 진단, 할당 실패는 그 오류다. 실패하면 convex는 그대로다(bulk에 더한 entry는
// 남을 수 있다 — 참조가 없어 저장되지 않는다).
ROCKS_MESH_API Result<void> StoreConvexGeometry(const ConvexGeometry& geometry,
                                                MemoryBulkSource& bulk, CollisionConvex& convex,
                                                CollisionDiagnostic* failure = nullptr) noexcept;

// convex의 bulk를 package에서 읽어 out을 다시 만든다. 배치(원소 크기의 배수)와
// ValidateConvexGeometry를 본다.
//
// 오류: 위반은 CorruptPackage·InvalidArgument(원래 판정은 issue), bulk를 못 읽으면
// CorruptPackage(BulkUnreadable)와 Package::BulkBytes의 오류, 할당 실패는 그 오류다. 실패하면 out은
// 그대로다. 스레드: package를 읽으므로 그 Registry 소유 스레드에서 부른다.
ROCKS_MESH_API Result<void> LoadConvexGeometry(const CollisionConvex& convex,
                                               const Package& package, ConvexGeometry& out,
                                               CollisionDiagnostic* failure = nullptr) noexcept;

// collision의 볼록 형상마다 LoadConvexGeometry로 bulk를 본다(진단 index = 볼록 형상 번호).
// scratch는 임시 점·index 배열의 할당기다. 오류: LoadConvexGeometry와 같다.
ROCKS_MESH_API Result<void>
ValidateSimpleCollisionBulk(const SimpleCollision& collision, const Package& package,
                            Allocator& scratch, CollisionDiagnostic* failure = nullptr) noexcept;

} // namespace rocks
