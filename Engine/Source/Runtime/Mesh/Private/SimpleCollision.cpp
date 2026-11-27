#include <Base/Endian.h>
#include <Core/Math/MathUtility.h>
#include <Mesh/SimpleCollision.h>

// 단순 충돌 검증과 볼록 형상 bulk 저장·로드다(Docs/Design/MeshAnimationAssets 4.6·4.8절).
namespace rocks {

namespace {

// 성분 차의 합이 0이면 모두 유한하다(v − v는 유한이면 0, NaN·±Inf면 NaN — 분기 없이 한 번에).
bool AllFinite(double x, double y, double z) noexcept {
    return (x - x) + (y - y) + (z - z) == 0.0;
}

bool FiniteVector(const Vec3d& value) noexcept {
    return AllFinite(value.x, value.y, value.z);
}

bool FiniteRotator(const Rotator<double>& value) noexcept {
    return AllFinite(value.pitch, value.yaw, value.roll);
}

Result<void> Fail(CollisionDiagnostic* failure, AssetDiagnosticCode code, ErrorCode error,
                  CollisionIssue issue, SimpleCollisionShapeKind shape, uint32 index,
                  uint32 related = CollisionNoIndex) noexcept {
    if (failure != nullptr) {
        CollisionDiagnostic diagnostic;
        diagnostic.code = code;
        diagnostic.error = error;
        diagnostic.issue = issue;
        diagnostic.shape = shape;
        diagnostic.index = index;
        diagnostic.related = related;
        *failure = diagnostic;
    }

    return Result<void>::Failure(error);
}

Result<void> Invalid(CollisionDiagnostic* failure, CollisionIssue issue,
                     SimpleCollisionShapeKind shape, uint32 index,
                     uint32 related = CollisionNoIndex) noexcept {
    return Fail(failure, AssetDiagnosticCode::InvalidCollisionData, ErrorCode::InvalidArgument,
                issue, shape, index, related);
}

// 형상 하나의 검사 결과다(첫 위반 규칙과 관련 성분).
struct ShapeCheck {
    CollisionIssue issue = CollisionIssue::None;
    uint32 related = CollisionNoIndex;
};

// 공통 필드와 위치(restOffset·center·rotation — 구는 회전이 없어 null)를 본다.
ShapeCheck CheckPlacement(float restOffset, const Vec3d& center,
                          const Rotator<double>* rotation) noexcept {
    const bool finite = math::IsFinite(restOffset) && FiniteVector(center) &&
                        (rotation == nullptr || FiniteRotator(*rotation));
    return finite ? ShapeCheck{} : ShapeCheck{CollisionIssue::NonFiniteValue};
}

// 양수여야 하는 크기(유한 > 0)다. 차례로 본다(첫 위반의 related = 그 순번).
ShapeCheck CheckPositive(const double* values, uint32 count) noexcept {
    for (uint32 at = 0; at < count; ++at) {
        if (!math::IsFinite(values[at])) {
            return ShapeCheck{CollisionIssue::NonFiniteValue, at};
        }

        if (!(values[at] > 0.0)) {
            return ShapeCheck{CollisionIssue::NonPositiveSize, at};
        }
    }

    return ShapeCheck{};
}

// 0 이상이어야 하는 길이다.
ShapeCheck CheckLength(float length) noexcept {
    if (!math::IsFinite(length)) {
        return ShapeCheck{CollisionIssue::NonFiniteValue};
    }

    return length < 0.0f ? ShapeCheck{CollisionIssue::NegativeLength} : ShapeCheck{};
}

// 위치 → 양수 크기(sizes) → 0 이상 길이(length가 있으면) 순으로 본다.
ShapeCheck CheckSized(float restOffset, const Vec3d& center, const Rotator<double>* rotation,
                      const double* sizes, uint32 count, const float* length) noexcept {
    ShapeCheck check = CheckPlacement(restOffset, center, rotation);
    if (check.issue == CollisionIssue::None) {
        check = CheckPositive(sizes, count);
    }

    if (check.issue == CollisionIssue::None && length != nullptr) {
        check = CheckLength(*length);
    }

    return check;
}

ShapeCheck CheckShape(const CollisionSphere& sphere) noexcept {
    const double sizes[] = {sphere.radius};
    return CheckSized(sphere.restOffset, sphere.center, nullptr, sizes, 1, nullptr);
}

ShapeCheck CheckShape(const CollisionBox& box) noexcept {
    const double sizes[] = {box.size.x, box.size.y, box.size.z};
    return CheckSized(box.restOffset, box.center, &box.rotation, sizes, 3, nullptr);
}

ShapeCheck CheckShape(const CollisionCapsule& capsule) noexcept {
    const double sizes[] = {capsule.radius};
    return CheckSized(capsule.restOffset, capsule.center, &capsule.rotation, sizes, 1,
                      &capsule.length);
}

ShapeCheck CheckShape(const CollisionTaperedCapsule& capsule) noexcept {
    const double sizes[] = {capsule.radius0, capsule.radius1};
    return CheckSized(capsule.restOffset, capsule.center, &capsule.rotation, sizes, 2,
                      &capsule.length);
}

ShapeCheck CheckShape(const CollisionCylinder& cylinder) noexcept {
    const double sizes[] = {cylinder.radius, cylinder.length};
    return CheckSized(cylinder.restOffset, cylinder.center, &cylinder.rotation, sizes, 2, nullptr);
}

ShapeCheck CheckShape(const CollisionConvex& convex) noexcept {
    if (!math::IsFinite(convex.restOffset)) {
        return ShapeCheck{CollisionIssue::NonFiniteValue};
    }

    const Transform& transform = convex.transform;
    const Quatd& rotation = transform.rotation;
    const double rotationSum = (rotation.x - rotation.x) + (rotation.y - rotation.y) +
                               (rotation.z - rotation.z) + (rotation.w - rotation.w);
    if (!FiniteVector(transform.translation) || !FiniteVector(transform.scale) ||
        !(rotationSum == 0.0)) {
        return ShapeCheck{CollisionIssue::NonFiniteValue};
    }

    const double length = rotation.x * rotation.x + rotation.y * rotation.y +
                          rotation.z * rotation.z + rotation.w * rotation.w;
    if (math::Abs(length - 1.0) > CollisionRotationNormalizedTolerance) {
        return ShapeCheck{CollisionIssue::TransformRotation};
    }

    const double scale[] = {transform.scale.x, transform.scale.y, transform.scale.z};
    for (uint32 axis = 0; axis < 3; ++axis) {
        if (scale[axis] == 0.0) {
            return ShapeCheck{CollisionIssue::TransformScale, axis};
        }
    }

    return ShapeCheck{};
}

template <class Shape>
Result<void> CheckShapes(const Array<Shape>& shapes, SimpleCollisionShapeKind kind,
                         CollisionDiagnostic* failure) noexcept {
    for (uint32 index = 0; index < shapes.Size(); ++index) {
        const ShapeCheck check = CheckShape(shapes[index]);
        if (check.issue != CollisionIssue::None) {
            return Invalid(failure, check.issue, kind, index, check.related);
        }
    }

    return Result<void>::Success();
}

} // namespace

const char* CollisionIssueName(CollisionIssue issue) noexcept {
    switch (issue) {
    case CollisionIssue::None:
        return "None";
    case CollisionIssue::TooManyElements:
        return "TooManyElements";
    case CollisionIssue::NonFiniteValue:
        return "NonFiniteValue";
    case CollisionIssue::NonPositiveSize:
        return "NonPositiveSize";
    case CollisionIssue::NegativeLength:
        return "NegativeLength";
    case CollisionIssue::TransformRotation:
        return "TransformRotation";
    case CollisionIssue::TransformScale:
        return "TransformScale";
    case CollisionIssue::TooFewVertices:
        return "TooFewVertices";
    case CollisionIssue::TooManyVertices:
        return "TooManyVertices";
    case CollisionIssue::NonFiniteVertex:
        return "NonFiniteVertex";
    case CollisionIssue::IndexCount:
        return "IndexCount";
    case CollisionIssue::IndexRange:
        return "IndexRange";
    case CollisionIssue::TooManyTriangles:
        return "TooManyTriangles";
    case CollisionIssue::BulkUnreadable:
        return "BulkUnreadable";
    case CollisionIssue::BulkLength:
        return "BulkLength";
    case CollisionIssue::UnsupportedFeature:
        return "UnsupportedFeature";
    }

    return "Unknown";
}

const char* SimpleCollisionShapeKindName(SimpleCollisionShapeKind kind) noexcept {
    switch (kind) {
    case SimpleCollisionShapeKind::None:
        return "None";
    case SimpleCollisionShapeKind::Sphere:
        return "Sphere";
    case SimpleCollisionShapeKind::Box:
        return "Box";
    case SimpleCollisionShapeKind::Capsule:
        return "Capsule";
    case SimpleCollisionShapeKind::TaperedCapsule:
        return "TaperedCapsule";
    case SimpleCollisionShapeKind::Cylinder:
        return "Cylinder";
    case SimpleCollisionShapeKind::Convex:
        return "Convex";
    }

    return "Unknown";
}

const char* UnsupportedCollisionFeatureName(UnsupportedCollisionFeature feature) noexcept {
    switch (feature) {
    case UnsupportedCollisionFeature::LevelSet:
        return "LevelSet";
    case UnsupportedCollisionFeature::SkinnedLevelSet:
        return "SkinnedLevelSet";
    case UnsupportedCollisionFeature::MLLevelSet:
        return "MLLevelSet";
    case UnsupportedCollisionFeature::SkinnedTriangleMesh:
        return "SkinnedTriangleMesh";
    case UnsupportedCollisionFeature::TaperedCapsuleWidth:
        return "TaperedCapsuleWidth";
    case UnsupportedCollisionFeature::TaperedCapsuleOneSided:
        return "TaperedCapsuleOneSided";
    }

    return "Unknown";
}

AssetDiagnostic ToAssetDiagnostic(const CollisionDiagnostic& diagnostic, AssetId asset) noexcept {
    AssetDiagnostic out;
    out.code = diagnostic.code;
    out.error = diagnostic.error;
    out.asset = asset;
    out.detail = diagnostic.index == CollisionNoIndex ? diagnostic.count : diagnostic.index;
    return out;
}

CollisionDiagnostic MakeUnsupportedCollisionWarning(UnsupportedCollisionFeature feature,
                                                    uint64 count) noexcept {
    CollisionDiagnostic warning;
    warning.code = AssetDiagnosticCode::UnsupportedCollisionShape;
    warning.issue = CollisionIssue::UnsupportedFeature;
    warning.related = static_cast<uint32>(feature);
    warning.count = count;
    return warning;
}

Result<void> ValidateSimpleCollision(const SimpleCollision& collision,
                                     CollisionDiagnostic* failure) noexcept {
    if (collision.ElementCount() > CollisionMaxElements) {
        return Fail(failure, AssetDiagnosticCode::InvalidCollisionData, ErrorCode::CapacityExceeded,
                    CollisionIssue::TooManyElements, SimpleCollisionShapeKind::None,
                    CollisionNoIndex);
    }

    if (auto checked = CheckShapes(collision.spheres, SimpleCollisionShapeKind::Sphere, failure);
        !checked) {
        return checked;
    }

    if (auto checked = CheckShapes(collision.boxes, SimpleCollisionShapeKind::Box, failure);
        !checked) {
        return checked;
    }

    if (auto checked = CheckShapes(collision.capsules, SimpleCollisionShapeKind::Capsule, failure);
        !checked) {
        return checked;
    }

    if (auto checked = CheckShapes(collision.taperedCapsules,
                                   SimpleCollisionShapeKind::TaperedCapsule, failure);
        !checked) {
        return checked;
    }

    if (auto checked =
            CheckShapes(collision.cylinders, SimpleCollisionShapeKind::Cylinder, failure);
        !checked) {
        return checked;
    }

    return CheckShapes(collision.convexes, SimpleCollisionShapeKind::Convex, failure);
}

Result<void> ValidateConvexGeometry(const ConvexGeometry& geometry,
                                    CollisionDiagnostic* failure) noexcept {
    const usize vertices = geometry.vertices.Size();
    if (vertices < CollisionConvexMinVertices) {
        return Invalid(failure, CollisionIssue::TooFewVertices, SimpleCollisionShapeKind::Convex,
                       CollisionNoIndex);
    }

    if (vertices > CollisionConvexMaxVertices) {
        return Fail(failure, AssetDiagnosticCode::InvalidCollisionData, ErrorCode::CapacityExceeded,
                    CollisionIssue::TooManyVertices, SimpleCollisionShapeKind::Convex,
                    CollisionNoIndex);
    }

    for (uint32 at = 0; at < vertices; ++at) {
        if (!FiniteVector(geometry.vertices[at])) {
            return Invalid(failure, CollisionIssue::NonFiniteVertex,
                           SimpleCollisionShapeKind::Convex, CollisionNoIndex, at);
        }
    }

    const usize indices = geometry.indices.Size();
    if (indices % 3 != 0) {
        return Invalid(failure, CollisionIssue::IndexCount, SimpleCollisionShapeKind::Convex,
                       CollisionNoIndex);
    }

    // 닫힌 볼록 다면체 삼각 분할의 삼각형 수 상한(Euler: F = 2V − 4).
    if (indices / 3 > 2 * vertices - 4) {
        return Invalid(failure, CollisionIssue::TooManyTriangles, SimpleCollisionShapeKind::Convex,
                       CollisionNoIndex);
    }

    for (uint32 at = 0; at < indices; ++at) {
        if (geometry.indices[at] >= vertices) {
            return Invalid(failure, CollisionIssue::IndexRange, SimpleCollisionShapeKind::Convex,
                           CollisionNoIndex, at);
        }
    }

    return Result<void>::Success();
}

Result<void> StoreConvexGeometry(const ConvexGeometry& geometry, MemoryBulkSource& bulk,
                                 CollisionConvex& convex, CollisionDiagnostic* failure) noexcept {
    if (auto valid = ValidateConvexGeometry(geometry, failure); !valid) {
        return valid;
    }

    Allocator& allocator = geometry.vertices.GetAllocator();
    Array<uint8> points(allocator);
    if (auto resized = points.TryResize(geometry.vertices.Size() * CollisionVertexBytes);
        !resized) {
        return resized;
    }

    uint8* at = points.Data();
    for (const Vec3d& vertex : geometry.vertices) {
        const double components[] = {vertex.x, vertex.y, vertex.z};
        for (double component : components) {
            StoreLittleEndian<uint64>(at, platform::BitCast<uint64>(component));
            at += sizeof(double);
        }
    }

    Array<uint8> triangles(allocator);
    if (auto resized = triangles.TryResize(geometry.indices.Size() * CollisionIndexBytes);
        !resized) {
        return resized;
    }

    at = triangles.Data();
    for (uint32 index : geometry.indices) {
        StoreLittleEndian<uint32>(at, index);
        at += CollisionIndexBytes;
    }

    // 점·index를 차례로 entry로 더한다(둘 다 성공해야 convex를 바꾼다).
    Array<uint8>* parts[] = {&points, &triangles};
    BulkDataRef refs[2];
    for (uint32 part = 0; part < 2; ++part) {
        auto ref = bulk.Add(Move(*parts[part]));
        if (!ref) {
            return Result<void>::Failure(ref.Error());
        }

        refs[part] = ref.Value();
    }

    convex.vertices = refs[0];
    convex.indices = refs[1];
    return Result<void>::Success();
}

namespace {

Result<Span<const uint8>> ReadConvexBulk(const Package& package, const BulkDataRef& ref,
                                         usize element, uint32 component,
                                         CollisionDiagnostic* failure) noexcept {
    auto bytes = package.BulkBytes(ref);
    if (!bytes) {
        (void)Fail(failure, AssetDiagnosticCode::CorruptPackage, bytes.Error(),
                   CollisionIssue::BulkUnreadable, SimpleCollisionShapeKind::Convex,
                   CollisionNoIndex, component);
        return bytes;
    }

    if (bytes.Value().Size() % element != 0) {
        (void)Fail(failure, AssetDiagnosticCode::CorruptPackage, ErrorCode::CorruptData,
                   CollisionIssue::BulkLength, SimpleCollisionShapeKind::Convex, CollisionNoIndex,
                   component);
        return Result<Span<const uint8>>::Failure(ErrorCode::CorruptData);
    }

    return bytes;
}

} // namespace

Result<void> LoadConvexGeometry(const CollisionConvex& convex, const Package& package,
                                ConvexGeometry& out, CollisionDiagnostic* failure) noexcept {
    auto points = ReadConvexBulk(package, convex.vertices, CollisionVertexBytes, 0, failure);
    if (!points) {
        return Result<void>::Failure(points.Error());
    }

    auto triangles = ReadConvexBulk(package, convex.indices, CollisionIndexBytes, 1, failure);
    if (!triangles) {
        return Result<void>::Failure(triangles.Error());
    }

    Allocator& allocator = out.vertices.GetAllocator();
    ConvexGeometry loaded(allocator);
    if (auto resized = loaded.vertices.TryResize(points.Value().Size() / CollisionVertexBytes);
        !resized) {
        return resized;
    }

    if (auto resized = loaded.indices.TryResize(triangles.Value().Size() / CollisionIndexBytes);
        !resized) {
        return resized;
    }

    const uint8* at = points.Value().Data();
    for (Vec3d& vertex : loaded.vertices) {
        double components[3];
        for (double& component : components) {
            component = platform::BitCast<double>(LoadLittleEndian<uint64>(at));
            at += sizeof(double);
        }

        vertex = Vec3d(components[0], components[1], components[2]);
    }

    at = triangles.Value().Data();
    for (uint32& index : loaded.indices) {
        index = LoadLittleEndian<uint32>(at);
        at += CollisionIndexBytes;
    }

    CollisionDiagnostic validation;
    if (auto valid = ValidateConvexGeometry(loaded, &validation); !valid) {
        // 형식 위반은 손상, 한도 초과는 그대로다(AssetSystem 19장).
        return Fail(failure, AssetDiagnosticCode::CorruptPackage,
                    valid.Error() == ErrorCode::CapacityExceeded ? ErrorCode::CapacityExceeded
                                                                 : ErrorCode::CorruptData,
                    validation.issue, SimpleCollisionShapeKind::Convex, CollisionNoIndex,
                    validation.related);
    }

    out = Move(loaded);
    return Result<void>::Success();
}

Result<void> ValidateSimpleCollisionBulk(const SimpleCollision& collision, const Package& package,
                                         Allocator& scratch,
                                         CollisionDiagnostic* failure) noexcept {
    for (uint32 index = 0; index < collision.convexes.Size(); ++index) {
        ConvexGeometry geometry(scratch);
        if (auto loaded = LoadConvexGeometry(collision.convexes[index], package, geometry, failure);
            !loaded) {
            if (failure != nullptr) {
                failure->index = index;
            }

            return loaded;
        }
    }

    return Result<void>::Success();
}

} // namespace rocks
