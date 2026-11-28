// body 형상·층 만들기(Docs/Design/PhysicsJoltAdapter.ko.md 4절, PhysicsModule 6.1절·7.1.3절).
#include "JoltBodyBuild.h"

#include "JoltConversions.h"
#include "JoltLayers.h"

#include <Collision/CollisionTypes.h>
#include <Core/Math/MathUtility.h>
#include <Core/Math/Matrix4.h>
#include <Core/Math/Vector4.h>
#include <DeterministicMath/DeterministicMath.h>

// clang-format off
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/OffsetCenterOfMassShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/Shape/TaperedCapsuleShape.h>
// clang-format on

namespace rocks::physics {

namespace {

// 0인 scale 축을 바꾸는 값이다(UE KINDA_SMALL_NUMBER — PrimitiveComponent.cpp:909-925, Rocks는 어느
// 한 축이라도 0이면 그 축을 바꾼다 — 2.3절).
double SafeScaleAxis(double axis) noexcept {
    const double magnitude = math::Abs(axis);
    return magnitude < math::KindaSmallNumber<double> ? math::KindaSmallNumber<double> : magnitude;
}

Result<JPH::RefConst<JPH::Shape>> Invalid() noexcept {
    return Result<JPH::RefConst<JPH::Shape>>::Failure(ErrorCode::InvalidArgument);
}

// 형상 밀도다(재질 밀도 g/cm³ → kg/m³, 하한 수소 0.09 kg/m³ — UE BodyUtils.cpp:37-45).
constexpr float MinDensityKilogramsPerCubicMeter = 0.09f;

float MaterialDensity() noexcept {
    const float density = ToJoltDensity(DefaultPhysicalMaterialDensity);
    return density > MinDensityKilogramsPerCubicMeter ? density : MinDensityKilogramsPerCubicMeter;
}

// Rocks 축 r(0 x, 1 y, 2 z)의 Jolt 축 번호다(축 A — Rocks X → Jolt Z, Y → X, Z → Y).
constexpr int JoltAxisOf[3] = {2, 0, 1};

// 3×3 관성(Jolt 행렬)의 Rocks 축 값이다.
void InertiaToRocks(const JPH::Mat44& jolt, double (&rocks)[3][3]) noexcept {
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            rocks[row][column] = static_cast<double>(jolt(JoltAxisOf[row], JoltAxisOf[column]));
        }
    }
}

void InertiaFromRocks(const double (&rocks)[3][3], JPH::Mat44& jolt) noexcept {
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            jolt(JoltAxisOf[row], JoltAxisOf[column]) = static_cast<float>(rocks[row][column]);
        }
    }
}

} // namespace

namespace {

bool IsFiniteShapeVector(const Vec3d& value) noexcept {
    return math::IsFinite(value.x) && math::IsFinite(value.y) && math::IsFinite(value.z);
}

// 원소 하나의 잎 형상(user data·밀도 전)과 원소 공간 안의 잎 원점 이동(cm — 테이퍼드 캡슐의 한 구
// 대체)이다.
struct LeafShape {
    JPH::Ref<JPH::Shape> shape;
    Vec3d offset = Vec3d::Zero();
};

// 테이퍼드 캡슐(UE FKTaperedCapsuleElem::GetFinalScaled — BodySetup.cpp:2249-2290, 7.1.21절).
// axisFlipped면 원소 축이 거울 scale로 뒤집혀(Sz < 0 — UE 거울 회전의 +Z는 σz·S∘R·ẑ) 두 끝 반경을
// 바꾼다. 오류: Jolt 형상 생성 실패(InvalidArgument).
Result<LeafShape> MakeTaperedCapsule(const BodyShapeElement& element, const Vec3d& magnitude,
                                     bool axisFlipped) noexcept {
    const double halfHeight =
        math::Max(math::Abs(element.halfHeight) * magnitude.z, MinCapsuleDimension);
    const double radialScale = math::Max(magnitude.x, magnitude.y);
    double top =
        math::Clamp(math::Abs(element.radius) * radialScale, MinCapsuleDimension, halfHeight);
    double bottom =
        math::Clamp(math::Abs(element.radius1) * radialScale, MinCapsuleDimension, halfHeight);
    if (axisFlipped) {
        const double swapped = top;
        top = bottom;
        bottom = swapped;
    }

    const double length = math::Max(MinCapsuleCylinderLength, 2.0 * halfHeight - (top + bottom));
    const float joltHalf = ToJoltLength(0.5 * length);
    const float joltTop = ToJoltLength(top);
    const float joltBottom = ToJoltLength(bottom);
    LeafShape leaf;
    // 한 구가 다른 구 안이면 큰 구 하나다(Jolt TaperedCapsuleShapeSettings::IsSphere와 같은 float
    // 판정 — 그 끝 중심에 둔다).
    if (math::Abs(joltTop - joltBottom) >= 2.0f * joltHalf) {
        const bool topLarger = joltTop > joltBottom;
        leaf.shape = new JPH::SphereShape(topLarger ? joltTop : joltBottom);
        leaf.offset = Vec3d(0.0, 0.0, topLarger ? 0.5 * length : -0.5 * length);
        return Result<LeafShape>::Success(leaf);
    }

    if (joltTop == joltBottom) {
        leaf.shape = new JPH::CapsuleShape(joltHalf, joltTop);
        return Result<LeafShape>::Success(leaf);
    }

    // Jolt 테이퍼드 캡슐 형상 원점은 선분 중점이다(두 끝 중심 mTopCenter·mBottomCenter는 질량 중심
    // 공간 값이고 GetCenterOfMass가 그만큼 옮긴다 — TaperedCapsuleShape.cpp:123-124). 원점 보정이
    // 없다.
    const JPH::TaperedCapsuleShapeSettings settings(joltHalf, joltTop, joltBottom);
    JPH::Shape::ShapeResult result;
    leaf.shape = new JPH::TaperedCapsuleShape(settings, result);
    if (result.HasError()) {
        return Result<LeafShape>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<LeafShape>::Success(leaf);
}

// 거울(음수 성분) scale 아래 원소 회전이다(UE FTransform::Multiply의 음수 scale 경로 —
// TransformNonVectorized.h:1277-1304 ConstructTransformFromMatrixWithDesiredScale): 축 i =
// σᵢ·normalize(S ⊙ R·eᵢ)(σ = scale 부호, 0 축은 KindaSmallNumber 크기)를 행으로 둔 행렬의
// quaternion이다. 음수 성분이 없으면 rotation 그대로다(UE quaternion 곱 경로).
Result<Quatd> ScaledElementRotation(const Quatd& rotation, const Vec3d& scale) noexcept {
    if (scale.x >= 0.0 && scale.y >= 0.0 && scale.z >= 0.0) {
        return Result<Quatd>::Success(rotation);
    }

    const Vec3d sign(scale.x < 0.0 ? -1.0 : 1.0, scale.y < 0.0 ? -1.0 : 1.0,
                     scale.z < 0.0 ? -1.0 : 1.0);
    const Vec3d signedScale(sign.x * SafeScaleAxis(scale.x), sign.y * SafeScaleAxis(scale.y),
                            sign.z * SafeScaleAxis(scale.z));
    const Vec3d basis[3] = {Vec3d(1.0, 0.0, 0.0), Vec3d(0.0, 1.0, 0.0), Vec3d(0.0, 0.0, 1.0)};
    const double axisSign[3] = {sign.x, sign.y, sign.z};
    Vector4<double> rows[3];
    for (int axis = 0; axis < 3; ++axis) {
        const Vec3d rotated = rotation.RotateVector(basis[axis]);
        auto unit =
            Vec3d(rotated.x * signedScale.x, rotated.y * signedScale.y, rotated.z * signedScale.z)
                .TryNormalize();
        if (!unit) {
            return Result<Quatd>::Failure(unit.Error());
        }

        const Vec3d row = unit.Value() * axisSign[axis];
        rows[axis] = Vector4<double>(row.x, row.y, row.z, 0.0);
    }

    auto mirrored = Quatd::TryFromRotationMatrix(
        Matrix4<double>::FromRows(rows[0], rows[1], rows[2], Vector4<double>(0.0, 0.0, 0.0, 1.0)));
    if (!mirrored) {
        return mirrored;
    }

    return mirrored.Value().TryNormalize();
}

// 원소 하나의 잎 형상이다(UE GetFinalScaled — 7.1.21절). magnitude는 |scale|(0 축은
// KindaSmallNumber)다.
Result<LeafShape> MakeLeafShape(const BodyShapeElement& element, const Vec3d& scale,
                                const Vec3d& magnitude, const BodyShapeOptions& options) noexcept {
    const double tiny = math::KindaSmallNumber<double>;
    LeafShape leaf;
    switch (element.kind) {
    case BodyGeometryKind::Box: {
        const Vec3d half(math::Max(math::Abs(element.halfExtent.x) * magnitude.x, tiny),
                         math::Max(math::Abs(element.halfExtent.y) * magnitude.y, tiny),
                         math::Max(math::Abs(element.halfExtent.z) * magnitude.z, tiny));
        // convex radius = UE Chaos 상자 margin(7.1.15절 — 둥근 모서리, 바깥 크기는 그대로)이고
        // 가장 작은 반폭을 넘지 않는다.
        const double minHalf = math::Min(half.x, math::Min(half.y, half.z));
        const double margin = math::Min(
            math::Min(2.0 * minHalf * options.collisionMarginFraction, options.collisionMarginMax),
            minHalf);
        leaf.shape = new JPH::BoxShape(ToJoltHalfExtents(half), ToJoltLength(margin));
        return Result<LeafShape>::Success(leaf);
    }

    case BodyGeometryKind::Sphere: {
        const double radius = math::Max(
            math::Abs(element.radius) * math::Min(magnitude.x, math::Min(magnitude.y, magnitude.z)),
            tiny);
        leaf.shape = new JPH::SphereShape(ToJoltLength(radius));
        return Result<LeafShape>::Success(leaf);
    }

    case BodyGeometryKind::Capsule: {
        // UE FKSphylElem(BodySetup.cpp:2167-2182): 반높이(반구 포함) × |Z|(최소 0.1), 반경 ×
        // max(|X|, |Y|)를 [0.1, 반높이], 원통 길이 2·(반높이 − 반경)(최소 0.1).
        const double halfHeight =
            math::Max(math::Abs(element.halfHeight) * magnitude.z, MinCapsuleDimension);
        const double radius =
            math::Clamp(math::Abs(element.radius) * math::Max(magnitude.x, magnitude.y),
                        MinCapsuleDimension, halfHeight);
        const double cylinderHalf = math::Max(halfHeight - radius, MinCapsuleCylinderLength * 0.5);
        // 축: Rocks +Z → Jolt +Y(A)라 형상 회전이 없다(어댑터 경계 4절).
        leaf.shape = new JPH::CapsuleShape(ToJoltLength(cylinderHalf), ToJoltLength(radius));
        return Result<LeafShape>::Success(leaf);
    }

    case BodyGeometryKind::TaperedCapsule:
        return MakeTaperedCapsule(element, magnitude, scale.z < 0.0);

    case BodyGeometryKind::None:
        break;
    }

    return Result<LeafShape>::Failure(ErrorCode::InvalidArgument);
}

} // namespace

namespace {

bool IsFiniteElement(const BodyShapeElement& element) noexcept {
    return IsFiniteShapeVector(element.center) && IsFiniteShapeVector(element.halfExtent) &&
           math::IsFinite(element.radius) && math::IsFinite(element.radius1) &&
           math::IsFinite(element.halfHeight) && math::IsFinite(element.rotation.x) &&
           math::IsFinite(element.rotation.y) && math::IsFinite(element.rotation.z) &&
           math::IsFinite(element.rotation.w);
}

// 부분 표가 원소를 빈틈없이 순서대로 덮고 relative가 유효한지다.
bool ArePartsValid(Span<const BodyShapeElement> geometry, Span<const ShapePart> parts) noexcept {
    if (parts.IsEmpty() || parts.Size() > BodyWeldMaxParts) {
        return false;
    }

    usize next = 0;
    for (const ShapePart& part : parts) {
        if (part.first != next || part.count == 0 || !part.relative.IsValid()) {
            return false;
        }

        next += part.count;
    }

    return next == geometry.Size();
}

} // namespace

Result<JPH::RefConst<JPH::Shape>> MakeJoltShape(Span<const BodyShapeElement> geometry,
                                                Span<const ShapePart> parts, const Vec3d& scale,
                                                const BodyShapeOptions& options,
                                                Array<ElementShape>* elements,
                                                bool* mixedCollision) noexcept {
    if (geometry.IsEmpty() || geometry.Size() > BodyGeometryMaxElements ||
        !IsFiniteShapeVector(scale) || !ArePartsValid(geometry, parts)) {
        return Invalid();
    }

    if (elements != nullptr) {
        if (auto reserved = elements->TryEnsureCapacity(elements->Size() + geometry.Size());
            !reserved) {
            return Result<JPH::RefConst<JPH::Shape>>::Failure(reserved.Error());
        }
    }

    JPH::StaticCompoundShapeSettings compound;
    JPH::RefConst<JPH::Shape> single;
    Vec3d singlePosition = Vec3d::Zero();
    Quatd singleRotation = Quatd::Identity();
    bool contributes = false;
    bool mixed = false;
    CollisionEnabled firstEnabled = CollisionEnabled::NoCollision;
    for (usize partIndex = 0; partIndex < parts.Size(); ++partIndex) {
        const ShapePart& part = parts[partIndex];
        auto partRotation = part.relative.rotation.TryNormalize();
        if (!partRotation) {
            return Invalid();
        }

        // 부분 원점의 루트 공간 자리(cm — relative 이동 × 루트 scale)와 그 역회전이다(부분 공간
        // 자세 — UE RootSpaceToWeldedSpace).
        const Vec3d& relativeTranslation = part.relative.translation;
        const Vec3d partOrigin(relativeTranslation.x * scale.x, relativeTranslation.y * scale.y,
                               relativeTranslation.z * scale.z);
        const Quatd toPart = partRotation.Value().Conjugate();

        // 부분 원소의 크기 scale = 루트 scale ⊙ relative scale(UE SetupNonUniformHelper(Scale3D *
        // RelativeTM.GetScale3D())). 크기 0·음수는 거부하지 않고 UE처럼 clamp한다(형상 setter는 0을
        // 허용한다 — UE도 body를 만든다,
        // PrimitiveComponent.cpp:909-925·ChaosInterfaceUtils.cpp:139-177).
        const Vec3d& relativeScale = part.relative.scale;
        const Vec3d combined(scale.x * relativeScale.x, scale.y * relativeScale.y,
                             scale.z * relativeScale.z);
        const Vec3d magnitude(SafeScaleAxis(combined.x), SafeScaleAxis(combined.y),
                              SafeScaleAxis(combined.z));
        for (usize index = part.first; index < part.first + part.count; ++index) {
            const BodyShapeElement& element = geometry[index];
            if (!IsFiniteElement(element)) {
                return Invalid();
            }

            auto normalized = element.rotation.TryNormalize();
            if (!normalized) {
                return Invalid();
            }

            // 회전: relative scale 거울을 원소 축에, 루트 scale 거울을 relative 회전 뒤(루트 축)에
            // 적용한다(UE FKBoxElem: GetTransform() * RelativeTM * ScaleTransform(Scale3D) —
            // BodySetup.cpp:2048-2064, 7.1.22절). 루트만이면 7.1.21절 그대로다.
            auto localRotation = ScaledElementRotation(normalized.Value(), relativeScale);
            if (!localRotation) {
                return Invalid();
            }

            auto rootRotation =
                ScaledElementRotation(partRotation.Value() * localRotation.Value(), scale);
            if (!rootRotation) {
                return Invalid();
            }

            auto leaf = MakeLeafShape(element, combined, magnitude, options);
            if (!leaf) {
                return Result<JPH::RefConst<JPH::Shape>>::Failure(leaf.Error());
            }

            ElementTag tag;
            tag.index = static_cast<uint32>(index);
            tag.part = static_cast<uint32>(partIndex);
            tag.enabled = part.welded ? CollisionEnabledIntersection(part.collisionEnabled,
                                                                     element.collisionEnabled)
                                      : element.collisionEnabled;
            if (index == 0) {
                firstEnabled = tag.enabled;
            }

            mixed = mixed || tag.enabled != firstEnabled;
            contributes = contributes || element.contributeToMass;
            JPH::Shape& shape = *leaf.Value().shape;
            // 질량에 넣지 않는 원소는 밀도 0이다(Jolt compound 질량은 자식 질량 합 — 어댑터 경계
            // 4절 J21). 잎은 모두 볼록 형상이다.
            static_cast<JPH::ConvexShape&>(shape).SetDensity(
                element.contributeToMass ? MaterialDensity() : 0.0f);
            shape.SetUserData(PackElementUserData(tag));
            // 루트 공간 중심: relative로 옮긴 중심 × 루트 scale(UE
            // RelativeTM.TransformPosition(Center)
            // * Scale3D). 잎 원점 이동은 원소 회전으로 돌린다. 부분 공간 자세는 루트 공간 자세에
            // 부분 원점·회전의 역을 적용한 것이다(같은 형상 — UE RootSpaceToWeldedSpace).
            const Vec3d offset = leaf.Value().offset;
            const Vec3d relativeCenter = part.relative.TransformPoint(element.center);
            const Vec3d rootCenter(relativeCenter.x * scale.x, relativeCenter.y * scale.y,
                                   relativeCenter.z * scale.z);
            const Vec3d position = rootCenter + rootRotation.Value().RotateVector(offset);
            if (elements != nullptr) {
                ElementShape placed;
                placed.shape = leaf.Value().shape;
                placed.position = position;
                placed.rotation = rootRotation.Value();
                placed.partPosition = toPart.RotateVector(position - partOrigin);
                placed.partRotation = toPart * rootRotation.Value();
                placed.kind = element.kind;
                placed.index = static_cast<uint32>(index);
                placed.part = static_cast<uint32>(partIndex);
                // 용량을 앞에서 잡았으므로 실패하지 않는다.
                (void)elements->TryPushBack(Move(placed));
            }

            if (geometry.Size() == 1) {
                single = leaf.Value().shape;
                singlePosition = position;
                singleRotation = rootRotation.Value();
            } else {
                compound.AddShape(ToJoltDisplacement(position),
                                  ToJoltRotation(rootRotation.Value()),
                                  leaf.Value().shape.GetPtr());
            }
        }
    }

    JPH::RefConst<JPH::Shape> inner;
    if (geometry.Size() == 1) {
        // 중심 0·회전 없음이면 형상 그대로다(Jolt StaticCompoundShapeSettings::Create와 같은 선택).
        inner =
            singlePosition == Vec3d::Zero() && singleRotation == Quatd::Identity()
                ? single
                : JPH::RefConst<JPH::Shape>(new JPH::RotatedTranslatedShape(
                      ToJoltDisplacement(singlePosition), ToJoltRotation(singleRotation), single));
    } else {
        JPH::Shape::ShapeResult created = compound.Create();
        if (created.HasError()) {
            return Invalid();
        }

        inner = created.Get().GetPtr();
    }

    // 질량에 넣는 원소가 없으면 UE 대체값의 질량 중심은 body 원점이다(MassProperties.cpp:474-483).
    // Jolt 원소 하나·compound가 아닌 형상은 그 원소의 질량 중심을 두므로 원점으로 되돌린다.
    const JPH::Vec3 center = inner->GetCenterOfMass();
    if (!contributes && center != JPH::Vec3::sZero()) {
        inner = new JPH::OffsetCenterOfMassShape(inner.GetPtr(), -center);
    }

    if (mixedCollision != nullptr) {
        *mixedCollision = mixed;
    }

    return Result<JPH::RefConst<JPH::Shape>>::Success(inner);
}

namespace {

// Rocks 축 허용(이동 X·Y·Z, 회전 X·Y·Z)을 Jolt bit로 바꾼다(축 A — Rocks X → Jolt Z, Y → X, Z → Y,
// 회전 축도 같다).
uint8 JoltDofBits(const bool (&allow)[6]) noexcept {
    constexpr JPH::EAllowedDOFs JoltAxis[6] = {
        JPH::EAllowedDOFs::TranslationZ, JPH::EAllowedDOFs::TranslationX,
        JPH::EAllowedDOFs::TranslationY, JPH::EAllowedDOFs::RotationZ,
        JPH::EAllowedDOFs::RotationX,    JPH::EAllowedDOFs::RotationY};
    uint8 bits = 0;
    for (int axis = 0; axis < 6; ++axis) {
        if (allow[axis]) {
            bits |= static_cast<uint8>(JoltAxis[axis]);
        }
    }

    return bits;
}

// 법선 축 index(0 X, 1 Y, 2 Z)의 평면 잠금을 Rocks 축 허용에 적는다: 법선 이동, 다른 두 축 회전.
void LockAxisPlane(int normalAxis, bool translation, bool rotation, bool (&allow)[6]) noexcept {
    if (translation) {
        allow[normalAxis] = false;
    }

    if (rotation) {
        for (int axis = 0; axis < 3; ++axis) {
            if (axis != normalAxis) {
                allow[3 + axis] = false;
            }
        }
    }
}

// 축과 나란한 단위 법선인지(축 index, 아니면 −1)다.
constexpr double AxisAlignedTolerance = 1.0e-6;

int AlignedAxis(const Vec3d& unit) noexcept {
    const double components[3] = {unit.x, unit.y, unit.z};
    for (int axis = 0; axis < 3; ++axis) {
        if (math::Abs(math::Abs(components[axis]) - 1.0) <= AxisAlignedTolerance) {
            return axis;
        }
    }

    return -1;
}

} // namespace

bool DofLock::LocksRotation() const noexcept {
    const uint8 rotation = static_cast<uint8>(JPH::EAllowedDOFs::RotationX) |
                           static_cast<uint8>(JPH::EAllowedDOFs::RotationY) |
                           static_cast<uint8>(JPH::EAllowedDOFs::RotationZ);
    return (static_cast<uint8>(allowed) & rotation) != rotation;
}

DofLock ResolveDofLock(const BodySettings* settings, SettingsDof projectDefault) noexcept {
    DofLock lock;
    DofMode mode = settings != nullptr ? settings->dofMode : DofMode::Default;
    if (mode == DofMode::Default) {
        switch (projectDefault) {
        case SettingsDof::YZPlane:
            mode = DofMode::YZPlane;
            break;
        case SettingsDof::XZPlane:
            mode = DofMode::XZPlane;
            break;
        case SettingsDof::XYPlane:
            mode = DofMode::XYPlane;
            break;
        case SettingsDof::Full3D:
            mode = DofMode::SixDOF;
            break;
        }
    }

    bool allow[6] = {true, true, true, true, true, true};
    switch (mode) {
    case DofMode::SixDOF:
        if (settings != nullptr) {
            allow[0] = !settings->lockXTranslation;
            allow[1] = !settings->lockYTranslation;
            allow[2] = !settings->lockZTranslation;
            allow[3] = !settings->lockXRotation;
            allow[4] = !settings->lockYRotation;
            allow[5] = !settings->lockZRotation;
        }

        break;
    // 축 평면: 법선 이동과 평면 안 두 축 회전을 잠근다(표지와 상관없이 — UE :893-898).
    case DofMode::YZPlane:
        LockAxisPlane(0, true, true, allow);
        break;
    case DofMode::XZPlane:
        LockAxisPlane(1, true, true, allow);
        break;
    case DofMode::XYPlane:
        LockAxisPlane(2, true, true, allow);
        break;
    case DofMode::CustomPlane: {
        // 법선이 0이면 잠금이 없다(UE LockedAxis.IsNearlyZero — 성분마다 KindaSmallNumber).
        const Vec3d normal = settings != nullptr ? settings->customDofPlaneNormal : Vec3d::Zero();
        const double tiny = math::KindaSmallNumber<double>;
        const bool finite =
            math::IsFinite(normal.x) && math::IsFinite(normal.y) && math::IsFinite(normal.z);
        if (!finite || (math::Abs(normal.x) <= tiny && math::Abs(normal.y) <= tiny &&
                        math::Abs(normal.z) <= tiny)) {
            return lock;
        }

        const bool translation = settings->lockTranslation;
        const bool rotation = settings->lockRotation;
        if (!translation && !rotation) {
            return lock;
        }

        const Vec3d unit = normal * (1.0 / normal.Length());
        // 축과 나란하면 축 평면처럼 bit로 한다(구속보다 싸고 teleport·형상 변경에 따로 할 일이
        // 없다).
        if (const int axis = AlignedAxis(unit); axis >= 0) {
            LockAxisPlane(axis, translation, rotation, allow);
            break;
        }

        lock.constraint = true;
        lock.constraintNormal = unit;
        lock.lockNormalTranslation = translation;
        lock.lockPlaneRotation = rotation;
        return lock;
    }

    case DofMode::None:
    case DofMode::Default:
        return lock;
    }

    const uint8 bits = JoltDofBits(allow);
    // 모두 잠긴 조합은 Jolt 허용 자유도로 표현할 수 없다(AllowedDOFs.h:12) — UE처럼 6축 구속으로
    // 멈춘다(허용 자유도는 All로 두고 구속이 모두 막는다).
    if (bits == 0) {
        lock.constraint = true;
        lock.lockAll = true;
        return lock;
    }

    lock.allowed = static_cast<JPH::EAllowedDOFs>(bits);
    return lock;
}

BodyMassValues MassValuesOf(const BodySettings* settings) noexcept {
    BodyMassValues values;
    if (settings == nullptr) {
        return values;
    }

    values.overrideMass = settings->overrideMass;
    values.massOverride = static_cast<double>(settings->massInKgOverride);
    values.massScale = static_cast<double>(settings->massScale);
    values.centerOfMassOffset = settings->centerOfMassOffset;
    values.inertiaTensorScale = settings->inertiaTensorScale;
    return values;
}

bool AreMassValuesValid(const BodyMassValues& mass) noexcept {
    auto finite = [](const Vec3d& value) noexcept {
        return math::IsFinite(value.x) && math::IsFinite(value.y) && math::IsFinite(value.z);
    };
    return math::IsFinite(mass.massOverride) && math::IsFinite(mass.massScale) &&
           finite(mass.centerOfMassOffset) && finite(mass.inertiaTensorScale);
}

Result<BodyShape> MakeBodyShape(Span<const BodyShapeElement> geometry, Span<const ShapePart> parts,
                                const Vec3d& scale, const BodyShapeOptions& options,
                                Array<ElementShape>* elements) noexcept {
    BodyShape built;
    auto inner = MakeJoltShape(geometry, parts, scale, options, elements, &built.mixedCollision);
    if (!inner) {
        return Result<BodyShape>::Failure(inner.Error());
    }

    built.inner = inner.Value();
    return Result<BodyShape>::Success(built);
}

Vec3d PrincipalInertia(const JPH::Mat44& inertia) noexcept {
    // UE처럼 kg·cm²로 판정한다(대각합 하한이 절대값이다).
    double tensor[3][3];
    InertiaToRocks(inertia, tensor);
    for (auto& row : tensor) {
        for (double& value : row) {
            value = math::KilogramSquareMetersToKilogramSquareCentimeters(value);
        }
    }

    const double diagonal[3] = {tensor[0][0], tensor[1][1], tensor[2][2]};

    // UE TransformToLocalSpace(CH/Private/Chaos/MassProperties.cpp:17-48)의 고유값 식
    // 그대로다(순서도 UE: m00 최대, m11 최소, m22 가운데 — 각이 [0, π/3]이라).
    const double offDiagonal =
        tensor[1][0] * tensor[1][0] + tensor[2][0] * tensor[2][0] + tensor[2][1] * tensor[2][1];
    const double trace = (diagonal[0] + diagonal[1] + diagonal[2]) / 3.0;
    if (trace <= math::SmallNumber<double> ||
        offDiagonal / (trace * trace) < InertiaDiagonalTolerance) {
        return Vec3d(diagonal[0], diagonal[1], diagonal[2]);
    }

    const double size =
        math::Sqrt(((diagonal[0] - trace) * (diagonal[0] - trace) +
                    (diagonal[1] - trace) * (diagonal[1] - trace) +
                    (diagonal[2] - trace) * (diagonal[2] - trace) + 2.0 * offDiagonal) /
                   6.0);
    double shifted[3][3];
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            shifted[row][column] = (tensor[row][column] - (row == column ? trace : 0.0)) / size;
        }
    }

    const double determinant =
        shifted[0][0] * (shifted[1][1] * shifted[2][2] - shifted[1][2] * shifted[2][1]) -
        shifted[0][1] * (shifted[1][0] * shifted[2][2] - shifted[1][2] * shifted[2][0]) +
        shifted[0][2] * (shifted[1][0] * shifted[2][1] - shifted[1][1] * shifted[2][0]);
    const double halfDeterminant = determinant / 2.0;
    const double angle = halfDeterminant <= -1.0  ? math::Pi<double> / 3.0
                         : halfDeterminant >= 1.0 ? 0.0
                                                  : deterministic::Acos(halfDeterminant) / 3.0;
    const double largest = trace + 2.0 * size * deterministic::Cos(angle);
    const double smallest =
        trace + 2.0 * size * deterministic::Cos(angle + 2.0 * math::Pi<double> / 3.0);
    const double middle = 3.0 * trace - largest - smallest;
    return Vec3d(largest, smallest, middle);
}

namespace {

using Matrix3 = double[3][3];

// 부분 하나의 질량(질량 중심 기준 관성 — Rocks 축, kg·m²)과 질량 중심(Rocks 축, m)이다.
struct PartMass {
    double mass = 0.0;
    double inertia[3][3] = {};
    Vec3d center = Vec3d::Zero();
};

// m(|d|²E − d dᵀ)를 inertia에 더한다(평행축 정리 — Chaos CombineWorldSpace).
void AddParallelAxis(Matrix3& inertia, double mass, const Vec3d& offset, double sign) noexcept {
    const double d[3] = {offset.x, offset.y, offset.z};
    const double lengthSquared = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            const double identity = row == column ? lengthSquared : 0.0;
            inertia[row][column] += sign * mass * (identity - d[row] * d[column]);
        }
    }
}

// 원시 질량(raw — 질량 중심 기준 관성)에 UE 수정자를 적용한다(UE ApplyMassPropertiesModifiers).
// nudge는 질량 중심 이동을 body(루트) 공간 cm로 옮긴 값이다.
void ApplyMassModifiers(PartMass& part, const BodyMassValues& mass, const Vec3d& nudge) noexcept {
    const double raw = part.mass;
    double target = 0.0;
    if (!mass.overrideMass) {
        // 균일 밀도 질량을 거듭제곱해 표면 쪽이 무거운 물체를 흉내 낸다(UE RaiseMassToPower —
        // 결정적 Pow라 모든 플랫폼이 같은 질량이다).
        const double power = math::Clamp(DefaultPhysicalMaterialRaiseMassToPower,
                                         math::KindaSmallNumber<double>, 1.0);
        target = math::Max(mass.massScale * deterministic::Pow(raw, power), MinBodyMassKilograms);
    } else {
        target = math::Max(mass.massOverride, MinBodyMassKilograms);
    }

    const double ratio = raw > 0.0 ? target / raw : 1.0;
    part.mass = target;
    for (auto& row : part.inertia) {
        for (double& value : row) {
            value *= ratio;
        }
    }

    // 관성 배율(Chaos ScaleInertia, bScaleMass 거짓 — Utilities.h:537-557, Rocks 축).
    const Vec3d& factor = mass.inertiaTensorScale;
    if (math::Abs(factor.x - 1.0) > InertiaScaleTolerance ||
        math::Abs(factor.y - 1.0) > InertiaScaleTolerance ||
        math::Abs(factor.z - 1.0) > InertiaScaleTolerance) {
        auto& inertia = part.inertia;
        const double s[3] = {math::Abs(factor.x), math::Abs(factor.y), math::Abs(factor.z)};
        const double half = 0.5 * (inertia[0][0] + inertia[1][1] + inertia[2][2]);
        double squared[3];
        for (int axis = 0; axis < 3; ++axis) {
            squared[axis] = (half - inertia[axis][axis]) * s[axis] * s[axis];
        }

        const double xy = inertia[0][1] * s[0] * s[1];
        const double xz = inertia[0][2] * s[0] * s[2];
        const double yz = inertia[1][2] * s[1] * s[2];
        inertia[0][0] = squared[1] + squared[2];
        inertia[1][1] = squared[0] + squared[2];
        inertia[2][2] = squared[0] + squared[1];
        inertia[0][1] = inertia[1][0] = xy;
        inertia[0][2] = inertia[2][0] = xz;
        inertia[1][2] = inertia[2][1] = yz;
    }

    // 질량 중심 이동과 축별 대각 보정(UE BodyUtils.cpp:85-111 — m·d², m 단위).
    const Vec3d meters(math::CentimetersToMeters(nudge.x), math::CentimetersToMeters(nudge.y),
                       math::CentimetersToMeters(nudge.z));
    part.center = part.center + meters;
    part.inertia[0][0] += target * meters.x * meters.x;
    part.inertia[1][1] += target * meters.y * meters.y;
    part.inertia[2][2] += target * meters.z * meters.z;
}

// 부분 p 잎들의 원시 질량이다(Jolt 잎 질량 속성을 원소 자리로 옮겨 더함 — Jolt CompoundShape
// GetMassProperties와 같은 식). 질량이 없으면 거짓이다.
bool AccumulatePartMass(Span<const ElementShape> elements, uint32 partIndex,
                        PartMass& part) noexcept {
    double mass = 0.0;
    Vec3d moment = Vec3d::Zero();
    double inertia[3][3] = {};
    const Vec3d basis[3] = {Vec3d(1.0, 0.0, 0.0), Vec3d(0.0, 1.0, 0.0), Vec3d(0.0, 0.0, 1.0)};
    for (const ElementShape& element : elements) {
        if (element.part != partIndex) {
            continue;
        }

        const JPH::MassProperties leaf = element.shape->GetMassProperties();
        const double leafMass = static_cast<double>(leaf.mMass);
        if (!(leafMass > 0.0)) {
            continue;
        }

        double local[3][3];
        InertiaToRocks(leaf.mInertia, local);
        // 원소 회전 R(열 = R·eⱼ)로 R I Rᵀ다.
        double rotation[3][3];
        for (int column = 0; column < 3; ++column) {
            const Vec3d axis = element.rotation.RotateVector(basis[column]);
            rotation[0][column] = axis.x;
            rotation[1][column] = axis.y;
            rotation[2][column] = axis.z;
        }

        double rotated[3][3] = {};
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column) {
                for (int a = 0; a < 3; ++a) {
                    for (int b = 0; b < 3; ++b) {
                        rotated[row][column] +=
                            rotation[row][a] * local[a][b] * rotation[column][b];
                    }
                }
            }
        }

        const Vec3d leafCenter =
            element.position +
            element.rotation.RotateVector(FromJoltDisplacement(element.shape->GetCenterOfMass()));
        const Vec3d center(math::CentimetersToMeters(leafCenter.x),
                           math::CentimetersToMeters(leafCenter.y),
                           math::CentimetersToMeters(leafCenter.z));
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column) {
                inertia[row][column] += rotated[row][column];
            }
        }

        AddParallelAxis(inertia, leafMass, center, 1.0);
        mass += leafMass;
        moment = moment + center * leafMass;
    }

    if (!(mass > 0.0)) {
        return false;
    }

    part.mass = mass;
    part.center = moment * (1.0 / mass);
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            part.inertia[row][column] = inertia[row][column];
        }
    }

    // 원점 기준 관성을 질량 중심 기준으로 옮긴다.
    AddParallelAxis(part.inertia, mass, part.center, -1.0);
    return true;
}

// 질량 중심 이동을 body 공간 cm로 옮긴다(UE MassModifierTransform.TransformVector(COMNudge) —
// relative 회전 · (relative scale ⊙ 루트 scale)).
Vec3d MassNudge(const ShapePart& part, const Vec3d& scale) noexcept {
    const Vec3d& offset = part.mass.centerOfMassOffset;
    const Vec3d& relativeScale = part.relative.scale;
    const Vec3d scaled(offset.x * relativeScale.x * scale.x, offset.y * relativeScale.y * scale.y,
                       offset.z * relativeScale.z * scale.z);
    return part.relative.rotation.RotateVector(scaled);
}

} // namespace

BodyMass ComputeBodyMass(const JPH::RefConst<JPH::Shape>& inner, Span<const ElementShape> elements,
                         Span<const ShapePart> parts, const Vec3d& scale) noexcept {
    const Vec3d innerCenter = FromJoltDisplacement(inner->GetCenterOfMass());
    const Vec3d innerCenterMeters(math::CentimetersToMeters(innerCenter.x),
                                  math::CentimetersToMeters(innerCenter.y),
                                  math::CentimetersToMeters(innerCenter.z));
    PartMass total;
    usize combined = 0;
    if (parts.Size() > 1) {
        // weld: 부분마다 따로 계산해 합친다(UE UpdateMassProperties — 질량에 넣는 원소가 없는
        // 부분은 빠진다).
        double mass = 0.0;
        Vec3d moment = Vec3d::Zero();
        for (usize index = 0; index < parts.Size(); ++index) {
            PartMass part;
            if (!AccumulatePartMass(elements, static_cast<uint32>(index), part)) {
                continue;
            }

            ApplyMassModifiers(part, parts[index].mass, MassNudge(parts[index], scale));
            mass += part.mass;
            moment = moment + part.center * part.mass;
            ++combined;
        }

        if (combined != 0) {
            total.mass = mass;
            total.center = moment * (1.0 / mass);
            for (usize index = 0; index < parts.Size(); ++index) {
                PartMass part;
                if (!AccumulatePartMass(elements, static_cast<uint32>(index), part)) {
                    continue;
                }

                ApplyMassModifiers(part, parts[index].mass, MassNudge(parts[index], scale));
                for (int row = 0; row < 3; ++row) {
                    for (int column = 0; column < 3; ++column) {
                        total.inertia[row][column] += part.inertia[row][column];
                    }
                }

                AddParallelAxis(total.inertia, part.mass, part.center - total.center, 1.0);
            }
        }
    }

    if (combined == 0) {
        // 루트만(또는 모든 부분이 빠짐): 원시 질량은 inner 값이다(Jolt compound 합 — 0이면 UE
        // 대체값, 질량 중심은 원점 — 7.1.21절).
        const JPH::MassProperties raw = inner->GetMassProperties();
        if (raw.mMass > 0.0f) {
            total.mass = static_cast<double>(raw.mMass);
            InertiaToRocks(raw.mInertia, total.inertia);
            total.center = innerCenterMeters;
        } else {
            const double fallback = math::KilogramSquareCentimetersToKilogramSquareMeters(
                FallbackBodyInertiaKilogramSquareCentimeters);
            total.mass = FallbackBodyMassKilograms;
            for (int axis = 0; axis < 3; ++axis) {
                total.inertia[axis][axis] = fallback;
            }

            total.center = innerCenterMeters;
        }

        const ShapePart root = parts.IsEmpty() ? ShapePart{} : parts[0];
        ApplyMassModifiers(total, root.mass, MassNudge(root, scale));
    }

    // 0이 된 주축(관성 배율 0 등)은 아주 작은 값으로 둔다(Jolt 역관성 무한대 방지 — 위 상수).
    const double largest =
        math::Max(total.inertia[0][0], math::Max(total.inertia[1][1], total.inertia[2][2]));
    const double floor = math::Max(largest * MinPrincipalInertiaRatio,
                                   math::KindaSmallNumber<double> * MinBodyMassKilograms);
    for (int axis = 0; axis < 3; ++axis) {
        total.inertia[axis][axis] = math::Max(total.inertia[axis][axis], floor);
    }

    BodyMass result;
    result.properties.mMass = static_cast<float>(total.mass);
    InertiaFromRocks(total.inertia, result.properties.mInertia);
    // 형상 질량 중심을 합친 질량 중심으로 옮긴다(UE 질량 중심 이동·weld 가중 — 7.1.15·7.1.22절).
    const Vec3d shiftMeters = total.center - innerCenterMeters;
    const Vec3d shift(math::MetersToCentimeters(shiftMeters.x),
                      math::MetersToCentimeters(shiftMeters.y),
                      math::MetersToCentimeters(shiftMeters.z));
    result.shift = ToJoltDisplacement(shift);
    result.shape = result.shift == JPH::Vec3::sZero()
                       ? inner
                       : JPH::RefConst<JPH::Shape>(
                             new JPH::OffsetCenterOfMassShape(inner.GetPtr(), result.shift));
    return result;
}

JPH::ObjectLayer ObjectLayerFor(const ResolvedCollision& collision, BodyMotion motion,
                                uint32 responseSet) noexcept {
    return ObjectLayerFor(collision.collisionEnabled, collision.objectType, motion, responseSet);
}

JPH::ObjectLayer ObjectLayerFor(CollisionEnabled enabled, CollisionChannel objectType,
                                BodyMotion motion, uint32 responseSet) noexcept {
    uint32 participation = 0;
    if (CollisionEnabledHasPhysics(enabled)) {
        participation |= LayerParticipationPhysics;
    }

    if (CollisionEnabledHasQuery(enabled)) {
        participation |= LayerParticipationQuery;
    }

    if (CollisionEnabledHasProbe(enabled)) {
        participation |= LayerParticipationProbe;
    }

    const bool simulates =
        (participation & (LayerParticipationPhysics | LayerParticipationProbe)) != 0;
    const LayerMotion group = !simulates                     ? LayerMotion::QueryOnly
                              : motion == BodyMotion::Static ? LayerMotion::Static
                                                             : LayerMotion::Moving;
    return MakeObjectLayer(static_cast<uint32>(objectType), group, participation, responseSet);
}

JPH::EMotionType ToJoltMotion(BodyMotion motion) noexcept {
    switch (motion) {
    case BodyMotion::Kinematic:
        return JPH::EMotionType::Kinematic;
    case BodyMotion::Dynamic:
        return JPH::EMotionType::Dynamic;
    case BodyMotion::Static:
        break;
    }

    return JPH::EMotionType::Static;
}

} // namespace rocks::physics
