#include <Core/Math/MathUtility.h>
#include <Core/Math/Matrix4.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Primitives/PrimitiveComponent.generated.h>
#include <World/SceneComponent.generated.h>
#include <World/Spline/SplineTypes.generated.h>
#include <World/Spline/SplineComponent.generated.h>
#include <World/Spline/SplineTypes.h>
#include <World/Spline/SplineComponent.h>

// SplineComponent다(Docs/Design/EngineRuntime 25장, D3). UE USplineComponent의 FSplineCurves 경로를
// Core InterpCurve 위에 둔다.
namespace rocks {

namespace {

// 5점 Legendre–Gauss 적분 abscissa·가중치(UE SplineComponent.cpp GetSegmentLength, float)다.
struct LegendreGaussCoefficient {
    float abscissa;
    float weight;
};

constexpr LegendreGaussCoefficient SplineLegendreGauss[] = {{0.0f, 0.5688889f},
                                                            {-0.5384693f, 0.47862867f},
                                                            {0.5384693f, 0.47862867f},
                                                            {-0.90617985f, 0.23692688f},
                                                            {0.90617985f, 0.23692688f}};

// UE MakeFromXZ의 평행 판정(UE_SMALL_NUMBER)·축 대체 기준(1 − UE_KINDA_SMALL_NUMBER)이다.
constexpr double ParallelTolerance = 1.0e-8;
constexpr double AxisReplaceLimit = 1.0 - 1.0e-4;
// UE FindBetweenVectors의 반대 방향 판정 비율이다.
constexpr double OppositeThreshold = 1.0e-6;
// UE AddPoint의 같은 키 판정(FMath::IsNearlyEqual(float, float) 기본 UE_SMALL_NUMBER —
// UnrealMathUtility.h:388)이다.
constexpr float KeyCollisionTolerance = 1.0e-8f;
// 생성자가 넣는 기본 점 수(UE SetDefaultSpline)다.
constexpr int32 DefaultPointCount = 2;

Result<void> InvalidSplineValue() noexcept {
    return Result<void>::Failure(ErrorCode::InvalidArgument);
}

bool IsFiniteVector(const Vec3d& value) noexcept {
    return math::IsFinite(value.x) && math::IsFinite(value.y) && math::IsFinite(value.z);
}

Vec3d SafeNormal(const Vec3d& value) noexcept {
    auto normal = value.TryNormalize();
    return normal ? normal.Value() : Vec3d();
}

// UE FRotationMatrix::MakeFromXZ(RotationMatrix.h:137)의 quaternion이다. 방향이 0이면 UE처럼
// 단위다.
Quatd MakeFromXZ(const Vec3d& xAxis, const Vec3d& zAxis) noexcept {
    const Vec3d newX = SafeNormal(xAxis);
    Vec3d normal = SafeNormal(zAxis);
    if (math::Abs(math::Abs(Dot(newX, normal)) - 1.0) <= ParallelTolerance) {
        normal = math::Abs(newX.z) < AxisReplaceLimit ? Vec3d(0.0, 0.0, 1.0) : Vec3d(1.0, 0.0, 0.0);
    }

    const Vec3d newY = SafeNormal(Cross(normal, newX));
    const Vec3d newZ = Cross(newX, newY);
    const Mat4d matrix = Mat4d::FromRows(Vec4d(newX, 0.0), Vec4d(newY, 0.0), Vec4d(newZ, 0.0),
                                         Vec4d(0.0, 0.0, 0.0, 1.0));
    auto rotation = Quatd::TryFromRotationMatrix(matrix);
    return rotation ? rotation.Value() : Quatd::Identity();
}

// UE FQuat::FindBetweenVectors(UE 5.8 UnrealMath.cpp FindBetween_Helper:1402)다.
Quatd FindBetween(const Vec3d& from, const Vec3d& to) noexcept {
    const double normProduct = math::Sqrt(from.LengthSquared() * to.LengthSquared());
    const double w = normProduct + Dot(from, to);
    Quatd result;
    if (w >= OppositeThreshold * normProduct) {
        const Vec3d axis = Cross(from, to);
        result = Quatd(axis.x, axis.y, axis.z, w);
    } else {
        // 반대 방향: from × 기저(|x|가 가장 크면 +Y, 아니면 −X)로 180°.
        const double x = math::Abs(from.x);
        const double y = math::Abs(from.y);
        const double z = math::Abs(from.z);
        const Vec3d basis = (x > y && x > z) ? Vec3d(0.0, 1.0, 0.0) : Vec3d(-1.0, 0.0, 0.0);
        const Vec3d axis = Cross(from, basis);
        result = Quatd(axis.x, axis.y, axis.z, 0.0);
    }

    auto normalized = result.TryNormalize();
    return normalized ? normalized.Value() : Quatd::Identity();
}

using PositionPoint = InterpCurvePoint<Vec3d>;
using RotationPoint = InterpCurvePoint<Quatd>;

} // namespace

// --- 생성·갱신 ---

SplineComponent::SplineComponent(Allocator& allocator) noexcept
    : PrimitiveComponent(allocator), position_(allocator), rotation_(allocator), scale_(allocator),
      reparam_(allocator) {
    generateOverlapEvents_ = false;
    // UE SetDefaultSpline(SplineComponent.cpp:424): (0, 0, 0)과 (100, 0, 0), 모두 CurveAuto. 할당이
    // 실패하면 빈 스플라인이다(25.3절).
    const Vec3d zero;
    const Vec3d one = Vec3d::One();
    const Quatd identity = Quatd::Identity();
    for (int32 index = 0; index < DefaultPointCount; ++index) {
        const float key = static_cast<float>(index);
        const Vec3d location(index == 0 ? 0.0 : DefaultSecondPointX, 0.0, 0.0);
        if (!InsertPoint(
                PositionPoint(key, location, zero, zero, InterpCurveMode::CurveAuto),
                RotationPoint(key, identity, identity, identity, InterpCurveMode::CurveAuto),
                PositionPoint(key, one, zero, zero, InterpCurveMode::CurveAuto))) {
            position_.Reset();
            rotation_.Reset();
            scale_.Reset();
            break;
        }
    }

    // 접선만 계산하고 거리 표는 첫 질의가 만든다(로드는 생성자 뒤에 곡선을 덮어쓴다 — 25.4절).
    UpdateCurves();
}

void SplineComponent::ApplyLoopKey() noexcept {
    if (!closedLoop_) {
        position_.ClearLoopKey();
        rotation_.ClearLoopKey();
        scale_.ClearLoopKey();
        return;
    }

    // UE: override면 loopPosition, 아니면 마지막 키 + 1(점이 없으면 0 + 1). 마지막 키 이하이면
    // 곡선이 루프를 끈다.
    const usize count = position_.PointCount();
    const float lastKey = count == 0 ? 0.0f : position_.GetPoint(count - 1).inValue;
    const float loopKey = loopPositionOverride_ ? loopPosition_ : lastKey + 1.0f;
    (void)position_.TrySetLoopKey(loopKey);
    (void)rotation_.TrySetLoopKey(loopKey);
    (void)scale_.TrySetLoopKey(loopKey);
}

void SplineComponent::UpdateCurves() noexcept {
    ApplyLoopKey();
    position_.AutoSetTangents(0.0f, stationaryEndpoints_);
    rotation_.AutoSetTangents(0.0f, stationaryEndpoints_);
    scale_.AutoSetTangents(0.0f, stationaryEndpoints_);
}

void SplineComponent::UpdateSpline() noexcept {
    // UE FSplineCurves::UpdateSpline(SplineComponent.cpp:733) 순서: 루프 키 → 자동 접선(tension 0)
    // → 거리 표.
    UpdateCurves();
    BuildReparamTable();
    InvalidateBounds();
    MarkRenderStateDirtyIfPresent();
}

void SplineComponent::FinishEdit(bool updateSpline) noexcept {
    if (updateSpline) {
        UpdateSpline();
        return;
    }

    // UE처럼 접선·거리 표는 다음 UpdateSpline까지 그대로다.
    InvalidateBounds();
    MarkRenderStateDirtyIfPresent();
}

void SplineComponent::OnTransformUpdated() noexcept {
    PrimitiveComponent::OnTransformUpdated();
    // UE는 표를 다시 만들지 않는다 — Rocks는 월드 scale이 바뀌면 버린다(25.4절 5).
    if (reparamValid_ && !(WorldScale3D() == reparamScale_)) {
        reparamValid_ = false;
    }
}

float SplineComponent::SegmentLengthWithScale(usize segment, float param,
                                              const Vec3d& scale) const noexcept {
    // UE GetSegmentLength(SplineComponent.cpp:854–920).
    const usize count = position_.PointCount();
    const bool looped = position_.IsLooped();
    if (count == 0 || !(param >= 0.0f && param <= 1.0f) ||
        segment >= (looped ? count : count - 1)) {
        return 0.0f;
    }

    const PositionPoint& start = position_.GetPoint(segment);
    const PositionPoint& end = position_.GetPoint(segment == count - 1 ? 0 : segment + 1);
    const Vec3d& p0 = start.outValue;
    const Vec3d& p1 = end.outValue;
    if (start.mode == InterpCurveMode::Linear) {
        return static_cast<float>(((p1 - p0) * scale).Length()) * param;
    }

    if (start.mode == InterpCurveMode::Constant) {
        return param == 1.0f ? static_cast<float>(((p1 - p0) * scale).Length()) : 0.0f;
    }

    // 접선은 키 간격 1을 가정한다(UE와 같다).
    const Vec3d& t0 = start.leaveTangent;
    const Vec3d& t1 = end.arriveTangent;
    const Vec3d coeff1 = ((p0 - p1) * 2.0 + t0 + t1) * 3.0;
    const Vec3d coeff2 = (p1 - p0) * 6.0 - t0 * 4.0 - t1 * 2.0;
    const Vec3d coeff3 = t0;
    const float halfParam = param * 0.5f;
    float length = 0.0f;
    for (const LegendreGaussCoefficient& coefficient : SplineLegendreGauss) {
        const double alpha = static_cast<double>(halfParam * (1.0f + coefficient.abscissa));
        const Vec3d derivative = ((coeff1 * alpha + coeff2) * alpha + coeff3) * scale;
        length = static_cast<float>(static_cast<double>(length) +
                                    derivative.Length() * static_cast<double>(coefficient.weight));
    }

    return length * halfParam;
}

void SplineComponent::BuildReparamTable() const noexcept {
    // UE FSplineCurves::UpdateSpline의 거리 표(25.4절 3): 차례로 덧붙이고 값은 구간 번호다.
    const Vec3d scale = WorldScale3D();
    reparam_.Clear();
    reparamScale_ = scale;
    reparamValid_ = false;
    const usize count = position_.PointCount();
    const usize segments = position_.IsLooped() ? count : (count > 0 ? count - 1 : 0);
    const usize steps = static_cast<usize>(reparamStepsPerSegment_);
    if (!reparam_.TryReserve(segments * steps + 1)) {
        return;
    }

    float accumulated = 0.0f;
    for (usize segment = 0; segment < segments; ++segment) {
        for (usize step = 0; step < steps; ++step) {
            const float param = static_cast<float>(step) / static_cast<float>(steps);
            const float length = step == 0 ? 0.0f : SegmentLengthWithScale(segment, param, scale);
            (void)reparam_.TryPushBack(
                ReparamEntry{accumulated + length, static_cast<float>(segment) + param});
        }

        accumulated += SegmentLengthWithScale(segment, 1.0f, scale);
    }

    (void)reparam_.TryPushBack(ReparamEntry{accumulated, static_cast<float>(segments)});
    reparamValid_ = true;
}

void SplineComponent::EnsureReparamTable() const noexcept {
    if (!reparamValid_) {
        BuildReparamTable();
    }
}

// --- 설정 ---

Result<void> SplineComponent::SetReparamStepsPerSegment(int32 steps) noexcept {
    if (steps < MinReparamStepsPerSegment || steps > MaxReparamStepsPerSegment) {
        return InvalidSplineValue();
    }

    if (reparamStepsPerSegment_ != steps) {
        reparamStepsPerSegment_ = steps;
        UpdateSpline();
    }

    return Result<void>::Success();
}

Result<void> SplineComponent::SetDuration(float seconds) noexcept {
    if (!math::IsFinite(seconds) || seconds < 0.0f) {
        return InvalidSplineValue();
    }

    duration_ = seconds;
    return Result<void>::Success();
}

void SplineComponent::SetStationaryEndpoints(bool stationary) noexcept {
    if (stationaryEndpoints_ != stationary) {
        stationaryEndpoints_ = stationary;
        UpdateSpline();
    }
}

void SplineComponent::SetClosedLoop(bool closed, bool updateSpline) noexcept {
    closedLoop_ = closed;
    loopPositionOverride_ = false;
    FinishEdit(updateSpline);
}

Result<void> SplineComponent::SetClosedLoopAtPosition(bool closed, float key,
                                                      bool updateSpline) noexcept {
    if (!math::IsFinite(key)) {
        return InvalidSplineValue();
    }

    // UE SetClosedLoopAtPosition(SplineComponent.cpp:1479).
    closedLoop_ = closed;
    loopPositionOverride_ = closed;
    loopPosition_ = key;
    FinishEdit(updateSpline);
    return Result<void>::Success();
}

Vec3d SplineComponent::DefaultUpVector(SplineCoordinateSpace space) const noexcept {
    return space == SplineCoordinateSpace::World ? ToWorldVector(defaultUpVector_)
                                                 : defaultUpVector_;
}

Result<void> SplineComponent::SetDefaultUpVector(const Vec3d& up,
                                                 SplineCoordinateSpace space) noexcept {
    Vec3d local = up;
    if (space == SplineCoordinateSpace::World) {
        auto converted = ToLocalVector(up);
        if (!converted) {
            return Result<void>::Failure(converted.Error());
        }

        local = converted.Value();
    }

    if (!IsFiniteVector(local) || local.LengthSquared() == 0.0) {
        return InvalidSplineValue();
    }

    defaultUpVector_ = local;
    UpdateSpline();
    return Result<void>::Success();
}

// --- 좌표 변환 ---

Vec3d SplineComponent::ToWorldPoint(const Vec3d& local) const noexcept {
    return WorldMatrix().TransformPoint(local);
}

Vec3d SplineComponent::ToWorldVector(const Vec3d& local) const noexcept {
    return WorldMatrix().TransformVector(local);
}

Result<Vec3d> SplineComponent::ToLocalPoint(const Vec3d& world) const noexcept {
    auto inverse = WorldMatrix().TryInverse();
    if (!inverse) {
        return Result<Vec3d>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<Vec3d>::Success(inverse.Value().TransformPoint(world));
}

Result<Vec3d> SplineComponent::ToLocalVector(const Vec3d& world) const noexcept {
    auto inverse = WorldMatrix().TryInverse();
    if (!inverse) {
        return Result<Vec3d>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<Vec3d>::Success(inverse.Value().TransformVector(world));
}

// --- 점 읽기 ---

bool SplineComponent::IsValidPoint(int32 index) const noexcept {
    return index >= 0 && static_cast<usize>(index) < position_.PointCount();
}

int32 SplineComponent::SplineSegmentCount() const noexcept {
    const int32 count = SplinePointCount();
    if (count < 2) {
        return 0;
    }

    return position_.IsLooped() ? count : count - 1;
}

Result<float> SplineComponent::InputKeyValueAtSplinePoint(int32 index) const noexcept {
    const int32 count = SplinePointCount();
    // 닫힌 스플라인의 index = 점 수는 루프 끝 키다(UE GetInputKeyValueAtSplinePoint).
    if (closedLoop_ && count >= 2 && index == count) {
        const float lastKey = position_.GetPoint(static_cast<usize>(count - 1)).inValue;
        return Result<float>::Success(position_.IsLooped() ? lastKey + position_.LoopKeyOffset()
                                                           : 0.0f);
    }

    if (!IsValidPoint(index)) {
        return Result<float>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<float>::Success(position_.GetPoint(static_cast<usize>(index)).inValue);
}

Result<Vec3d> SplineComponent::LocationAtSplinePoint(int32 index,
                                                     SplineCoordinateSpace space) const noexcept {
    if (!IsValidPoint(index)) {
        return Result<Vec3d>::Failure(ErrorCode::InvalidArgument);
    }

    const Vec3d& local = position_.GetPoint(static_cast<usize>(index)).outValue;
    return Result<Vec3d>::Success(space == SplineCoordinateSpace::World ? ToWorldPoint(local)
                                                                        : local);
}

Result<Vec3d>
SplineComponent::ArriveTangentAtSplinePoint(int32 index,
                                            SplineCoordinateSpace space) const noexcept {
    if (!IsValidPoint(index)) {
        return Result<Vec3d>::Failure(ErrorCode::InvalidArgument);
    }

    const Vec3d& local = position_.GetPoint(static_cast<usize>(index)).arriveTangent;
    return Result<Vec3d>::Success(space == SplineCoordinateSpace::World ? ToWorldVector(local)
                                                                        : local);
}

Result<Vec3d>
SplineComponent::LeaveTangentAtSplinePoint(int32 index,
                                           SplineCoordinateSpace space) const noexcept {
    if (!IsValidPoint(index)) {
        return Result<Vec3d>::Failure(ErrorCode::InvalidArgument);
    }

    const Vec3d& local = position_.GetPoint(static_cast<usize>(index)).leaveTangent;
    return Result<Vec3d>::Success(space == SplineCoordinateSpace::World ? ToWorldVector(local)
                                                                        : local);
}

Result<Vec3d> SplineComponent::ScaleAtSplinePoint(int32 index) const noexcept {
    if (!IsValidPoint(index)) {
        return Result<Vec3d>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<Vec3d>::Success(scale_.GetPoint(static_cast<usize>(index)).outValue);
}

Result<SplinePointType> SplineComponent::SplinePointTypeAt(int32 index) const noexcept {
    if (!IsValidPoint(index)) {
        return Result<SplinePointType>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<SplinePointType>::Success(
        ToPointType(position_.GetPoint(static_cast<usize>(index)).mode));
}

Result<Quatd> SplineComponent::QuaternionAtSplinePoint(int32 index,
                                                       SplineCoordinateSpace space) const noexcept {
    if (!IsValidPoint(index)) {
        return Result<Quatd>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<Quatd>::Success(
        QuaternionAtSplineInputKey(position_.GetPoint(static_cast<usize>(index)).inValue, space));
}

Result<double> SplineComponent::DistanceAlongSplineAtSplinePoint(int32 index) const noexcept {
    const int32 count = SplinePointCount();
    if (closedLoop_ && index == count) {
        return Result<double>::Success(SplineLength());
    }

    if (!IsValidPoint(index)) {
        return Result<double>::Failure(ErrorCode::InvalidArgument);
    }

    EnsureReparamTable();
    const usize entry = static_cast<usize>(index) * static_cast<usize>(reparamStepsPerSegment_);
    return Result<double>::Success(
        entry < reparam_.Size() ? static_cast<double>(reparam_[entry].distance) : 0.0);
}

double SplineComponent::SplineLength() const noexcept {
    EnsureReparamTable();
    return reparam_.IsEmpty() ? 0.0 : static_cast<double>(reparam_[reparam_.Size() - 1].distance);
}

double SplineComponent::SegmentLength(int32 segment, float param) const noexcept {
    if (segment < 0) {
        return 0.0;
    }

    return static_cast<double>(
        SegmentLengthWithScale(static_cast<usize>(segment), param, WorldScale3D()));
}

InterpCurveMode SplineComponent::ToCurveMode(SplinePointType type) noexcept {
    switch (type) {
    case SplinePointType::Linear:
        return InterpCurveMode::Linear;
    case SplinePointType::Curve:
        return InterpCurveMode::CurveAuto;
    case SplinePointType::Constant:
        return InterpCurveMode::Constant;
    case SplinePointType::CurveClamped:
        return InterpCurveMode::CurveAutoClamped;
    case SplinePointType::CurveCustomTangent:
        return InterpCurveMode::CurveUser;
    }

    // 범위 밖 enum은 UE CIM_Unknown 자리다 — Rocks 모드에 없어 Constant로 둔다(setter가 먼저 거부).
    return InterpCurveMode::Constant;
}

SplinePointType SplineComponent::ToPointType(InterpCurveMode mode) noexcept {
    switch (mode) {
    case InterpCurveMode::Linear:
        return SplinePointType::Linear;
    case InterpCurveMode::CurveAuto:
        return SplinePointType::Curve;
    case InterpCurveMode::CurveAutoClamped:
        return SplinePointType::CurveClamped;
    case InterpCurveMode::CurveUser:
        return SplinePointType::CurveCustomTangent;
    default:
        // UE: Constant·CurveBreak·Unknown은 Constant다.
        return SplinePointType::Constant;
    }
}

// --- 점 편집 ---

Result<void> SplineComponent::InsertPoint(const InterpCurvePoint<Vec3d>& position,
                                          const InterpCurvePoint<Quatd>& rotation,
                                          const InterpCurvePoint<Vec3d>& scale) noexcept {
    auto positionIndex = position_.TryAddPoint(position);
    if (!positionIndex) {
        return Result<void>::Failure(positionIndex.Error());
    }

    auto rotationIndex = rotation_.TryAddPoint(rotation);
    if (!rotationIndex) {
        (void)position_.TryRemovePoint(positionIndex.Value());
        return Result<void>::Failure(rotationIndex.Error());
    }

    auto scaleIndex = scale_.TryAddPoint(scale);
    if (!scaleIndex) {
        (void)position_.TryRemovePoint(positionIndex.Value());
        (void)rotation_.TryRemovePoint(rotationIndex.Value());
        return Result<void>::Failure(scaleIndex.Error());
    }

    return Result<void>::Success();
}

void SplineComponent::ShiftKeysFrom(usize from, float delta) noexcept {
    const usize count = position_.PointCount();
    if (delta > 0.0f) {
        // 뒤에서부터 올린다(순서 유지).
        for (usize index = count; index > from; --index) {
            const float key = position_.GetPoint(index - 1).inValue + delta;
            (void)position_.TryMovePoint(index - 1, key);
            (void)rotation_.TryMovePoint(index - 1, key);
            (void)scale_.TryMovePoint(index - 1, key);
        }

        return;
    }

    for (usize index = from; index < count; ++index) {
        const float key = position_.GetPoint(index).inValue + delta;
        (void)position_.TryMovePoint(index, key);
        (void)rotation_.TryMovePoint(index, key);
        (void)scale_.TryMovePoint(index, key);
    }
}

void SplineComponent::ClearSplinePoints(bool updateSpline) noexcept {
    position_.Reset();
    rotation_.Reset();
    scale_.Reset();
    FinishEdit(updateSpline);
}

Result<void> SplineComponent::AddSplinePoint(const Vec3d& position, SplineCoordinateSpace space,
                                             bool updateSpline) noexcept {
    Vec3d local = position;
    if (space == SplineCoordinateSpace::World) {
        auto converted = ToLocalPoint(position);
        if (!converted) {
            return Result<void>::Failure(converted.Error());
        }

        local = converted.Value();
    }

    if (!IsFiniteVector(local)) {
        return InvalidSplineValue();
    }

    // UE AddSplinePoint(SplineComponent.cpp:1674): 키 = 마지막 + 1(빈 곡선 0).
    const usize count = position_.PointCount();
    const float key = count == 0 ? 0.0f : position_.GetPoint(count - 1).inValue + 1.0f;
    const Vec3d zero;
    const Quatd identity = Quatd::Identity();
    if (auto inserted = InsertPoint(
            PositionPoint(key, local, zero, zero, InterpCurveMode::CurveAuto),
            RotationPoint(key, identity, identity, identity, InterpCurveMode::CurveAuto),
            PositionPoint(key, Vec3d::One(), zero, zero, InterpCurveMode::CurveAuto));
        !inserted) {
        return inserted;
    }

    if (loopPositionOverride_) {
        loopPosition_ += 1.0f;
    }

    FinishEdit(updateSpline);
    return Result<void>::Success();
}

Result<void> SplineComponent::AddSplinePointAtIndex(const Vec3d& position, int32 index,
                                                    SplineCoordinateSpace space,
                                                    bool updateSpline) noexcept {
    const usize count = position_.PointCount();
    if (index < 0 || static_cast<usize>(index) > count) {
        return InvalidSplineValue();
    }

    Vec3d local = position;
    if (space == SplineCoordinateSpace::World) {
        auto converted = ToLocalPoint(position);
        if (!converted) {
            return Result<void>::Failure(converted.Error());
        }

        local = converted.Value();
    }

    if (!IsFiniteVector(local)) {
        return InvalidSplineValue();
    }

    // 실패해도 키를 되돌릴 수 있게 공간을 먼저 확보한다.
    if (!position_.TryReserve(count + 1) || !rotation_.TryReserve(count + 1) ||
        !scale_.TryReserve(count + 1)) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    // UE AddSplinePointAtIndex(SplineComponent.cpp:1731): 키 = index = 0이면 0, 아니면 앞 키 + 1,
    // 뒤 키를 1씩 민다.
    const usize at = static_cast<usize>(index);
    const float key = at == 0 ? 0.0f : position_.GetPoint(at - 1).inValue + 1.0f;
    ShiftKeysFrom(at, 1.0f);
    const Vec3d zero;
    const Quatd identity = Quatd::Identity();
    (void)InsertPoint(PositionPoint(key, local, zero, zero, InterpCurveMode::CurveAuto),
                      RotationPoint(key, identity, identity, identity, InterpCurveMode::CurveAuto),
                      PositionPoint(key, Vec3d::One(), zero, zero, InterpCurveMode::CurveAuto));
    if (loopPositionOverride_) {
        loopPosition_ += 1.0f;
    }

    FinishEdit(updateSpline);
    return Result<void>::Success();
}

Result<void> SplineComponent::AddPoint(const SplinePoint& point, bool updateSpline) noexcept {
    if (!math::IsFinite(point.inputKey) || !IsFiniteVector(point.position) ||
        !IsFiniteVector(point.arriveTangent) || !IsFiniteVector(point.leaveTangent) ||
        !IsFiniteVector(point.scale) || !math::IsFinite(point.rotation.pitch) ||
        !math::IsFinite(point.rotation.yaw) || !math::IsFinite(point.rotation.roll) ||
        static_cast<uint8>(point.type) > static_cast<uint8>(SplinePointType::CurveCustomTangent)) {
        return InvalidSplineValue();
    }

    const usize count = position_.PointCount();
    if (!position_.TryReserve(count + 1) || !rotation_.TryReserve(count + 1) ||
        !scale_.TryReserve(count + 1)) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    // UE AddPoint(SplineComponent.cpp:1577): UpperBound 자리에서 앞 키와 같으면 그 자리에 넣고 뒤를
    // 1씩 민다.
    usize upper = 0;
    while (upper < count && !(point.inputKey < position_.GetPoint(upper).inValue)) {
        ++upper;
    }

    if (upper > 0 && math::Abs(point.inputKey - position_.GetPoint(upper - 1).inValue) <=
                         KeyCollisionTolerance) {
        ShiftKeysFrom(upper - 1, 1.0f);
    }

    const Vec3d zero;
    const Quatd identity = Quatd::Identity();
    const Quatd rotation = point.rotation.ToQuaternion();
    (void)InsertPoint(
        PositionPoint(point.inputKey, point.position, point.arriveTangent, point.leaveTangent,
                      ToCurveMode(point.type)),
        RotationPoint(point.inputKey, rotation, identity, identity, InterpCurveMode::CurveAuto),
        PositionPoint(point.inputKey, point.scale, zero, zero, InterpCurveMode::CurveAuto));
    const usize newCount = position_.PointCount();
    if (loopPositionOverride_ && newCount > 0 &&
        loopPosition_ <= position_.GetPoint(newCount - 1).inValue) {
        loopPositionOverride_ = false;
    }

    FinishEdit(updateSpline);
    return Result<void>::Success();
}

Result<void> SplineComponent::RemoveSplinePoint(int32 index, bool updateSpline) noexcept {
    if (!IsValidPoint(index)) {
        return InvalidSplineValue();
    }

    // UE RemoveSplinePoint(SplineComponent.cpp:1798): 앞 간격(첫 점이면 다음 간격)만큼 뒤 키를
    // 당긴다.
    const usize at = static_cast<usize>(index);
    const usize count = position_.PointCount();
    float delta = 0.0f;
    if (at > 0) {
        delta = position_.GetPoint(at).inValue - position_.GetPoint(at - 1).inValue;
    } else if (count >= 2) {
        delta = position_.GetPoint(1).inValue - position_.GetPoint(0).inValue;
    }

    (void)position_.TryRemovePoint(at);
    (void)rotation_.TryRemovePoint(at);
    (void)scale_.TryRemovePoint(at);
    if (delta != 0.0f) {
        ShiftKeysFrom(at, -delta);
    }

    if (loopPositionOverride_) {
        loopPosition_ -= 1.0f;
    }

    FinishEdit(updateSpline);
    return Result<void>::Success();
}

Result<void> SplineComponent::SetSplinePoints(Span<const Vec3d> positions,
                                              SplineCoordinateSpace space,
                                              bool updateSpline) noexcept {
    Mat4d toLocal = Mat4d::Identity();
    if (space == SplineCoordinateSpace::World) {
        auto inverse = WorldMatrix().TryInverse();
        if (!inverse) {
            return InvalidSplineValue();
        }

        toLocal = inverse.Value();
    }

    for (const Vec3d& position : positions) {
        if (!IsFiniteVector(toLocal.TransformPoint(position))) {
            return InvalidSplineValue();
        }
    }

    // 공간을 먼저 확보해 실패하면 바꾸지 않는다.
    if (!position_.TryReserve(positions.Size()) || !rotation_.TryReserve(positions.Size()) ||
        !scale_.TryReserve(positions.Size())) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    position_.Reset();
    rotation_.Reset();
    scale_.Reset();
    const Vec3d zero;
    const Quatd identity = Quatd::Identity();
    for (usize index = 0; index < positions.Size(); ++index) {
        const float key = static_cast<float>(index);
        (void)InsertPoint(
            PositionPoint(key, toLocal.TransformPoint(positions[index]), zero, zero,
                          InterpCurveMode::CurveAuto),
            RotationPoint(key, identity, identity, identity, InterpCurveMode::CurveAuto),
            PositionPoint(key, Vec3d::One(), zero, zero, InterpCurveMode::CurveAuto));
    }

    loopPositionOverride_ = false;
    FinishEdit(updateSpline);
    return Result<void>::Success();
}

Result<void> SplineComponent::SetLocationAtSplinePoint(int32 index, const Vec3d& position,
                                                       SplineCoordinateSpace space,
                                                       bool updateSpline) noexcept {
    if (!IsValidPoint(index)) {
        return InvalidSplineValue();
    }

    Vec3d local = position;
    if (space == SplineCoordinateSpace::World) {
        auto converted = ToLocalPoint(position);
        if (!converted) {
            return Result<void>::Failure(converted.Error());
        }

        local = converted.Value();
    }

    if (!IsFiniteVector(local)) {
        return InvalidSplineValue();
    }

    position_.SetOutValue(static_cast<usize>(index), local);
    FinishEdit(updateSpline);
    return Result<void>::Success();
}

Result<void> SplineComponent::SetTangentAtSplinePoint(int32 index, const Vec3d& tangent,
                                                      SplineCoordinateSpace space,
                                                      bool updateSpline) noexcept {
    return SetTangentsAtSplinePoint(index, tangent, tangent, space, updateSpline);
}

Result<void> SplineComponent::SetTangentsAtSplinePoint(int32 index, const Vec3d& arrive,
                                                       const Vec3d& leave,
                                                       SplineCoordinateSpace space,
                                                       bool updateSpline) noexcept {
    if (!IsValidPoint(index)) {
        return InvalidSplineValue();
    }

    Vec3d localArrive = arrive;
    Vec3d localLeave = leave;
    if (space == SplineCoordinateSpace::World) {
        auto convertedArrive = ToLocalVector(arrive);
        auto convertedLeave = ToLocalVector(leave);
        if (!convertedArrive || !convertedLeave) {
            return InvalidSplineValue();
        }

        localArrive = convertedArrive.Value();
        localLeave = convertedLeave.Value();
    }

    if (!IsFiniteVector(localArrive) || !IsFiniteVector(localLeave)) {
        return InvalidSplineValue();
    }

    // UE SetTangentsAtSplinePoint: 사용자 접선 모드로 바꾼다.
    position_.SetTangents(static_cast<usize>(index), localArrive, localLeave);
    position_.SetMode(static_cast<usize>(index), InterpCurveMode::CurveUser);
    FinishEdit(updateSpline);
    return Result<void>::Success();
}

Result<void> SplineComponent::SetUpVectorAtSplinePoint(int32 index, const Vec3d& up,
                                                       SplineCoordinateSpace space,
                                                       bool updateSpline) noexcept {
    if (!IsValidPoint(index)) {
        return InvalidSplineValue();
    }

    Vec3d local = up;
    if (space == SplineCoordinateSpace::World) {
        auto converted = ToLocalVector(up);
        if (!converted) {
            return Result<void>::Failure(converted.Error());
        }

        local = converted.Value();
    }

    auto normal = local.TryNormalize();
    if (!IsFiniteVector(local) || !normal) {
        return InvalidSplineValue();
    }

    // UE SetUpVectorAtSplinePoint(SplineComponent.cpp:2011).
    rotation_.SetOutValue(static_cast<usize>(index), FindBetween(defaultUpVector_, normal.Value()));
    FinishEdit(updateSpline);
    return Result<void>::Success();
}

Result<void> SplineComponent::SetQuaternionAtSplinePoint(int32 index, const Quatd& rotation,
                                                         SplineCoordinateSpace space,
                                                         bool updateSpline) noexcept {
    if (!IsValidPoint(index)) {
        return InvalidSplineValue();
    }

    auto normalized = rotation.TryNormalize();
    if (!normalized) {
        return InvalidSplineValue();
    }

    const Quatd local = space == SplineCoordinateSpace::World
                            ? WorldQuaternion().Conjugate() * normalized.Value()
                            : normalized.Value();
    // UE SetQuaternionAtSplinePoint(SplineComponent.cpp:2043): 지금 회전에서의 상대 회전으로 접선
    // 방향을 돌리고(크기 유지 — CurveUser가 된다) up을 맞춘다.
    const usize at = static_cast<usize>(index);
    const Quatd current = LocalQuaternionAtKey(position_.GetPoint(at).inValue);
    const Quatd relative = local * current.Conjugate();
    const Vec3d arrive = relative.RotateVector(position_.GetPoint(at).arriveTangent);
    const Vec3d leave = relative.RotateVector(position_.GetPoint(at).leaveTangent);
    rotation_.SetOutValue(at,
                          FindBetween(defaultUpVector_, local.RotateVector(Vec3d(0.0, 0.0, 1.0))));
    position_.SetTangents(at, arrive, leave);
    position_.SetMode(at, InterpCurveMode::CurveUser);
    FinishEdit(updateSpline);
    return Result<void>::Success();
}

Result<void> SplineComponent::SetScaleAtSplinePoint(int32 index, const Vec3d& scale,
                                                    bool updateSpline) noexcept {
    if (!IsValidPoint(index) || !IsFiniteVector(scale)) {
        return InvalidSplineValue();
    }

    scale_.SetOutValue(static_cast<usize>(index), scale);
    FinishEdit(updateSpline);
    return Result<void>::Success();
}

Result<void> SplineComponent::SetSplinePointType(int32 index, SplinePointType type,
                                                 bool updateSpline) noexcept {
    if (!IsValidPoint(index) ||
        static_cast<uint8>(type) > static_cast<uint8>(SplinePointType::CurveCustomTangent)) {
        return InvalidSplineValue();
    }

    position_.SetMode(static_cast<usize>(index), ToCurveMode(type));
    FinishEdit(updateSpline);
    return Result<void>::Success();
}

// --- 키 질의 ---

Quatd SplineComponent::LocalQuaternionAtKey(float key) const noexcept {
    // UE GetQuaternionAtSplineInputKey(SplineComponent.cpp:1051).
    auto normalized = rotation_.Eval(key, Quatd::Identity()).TryNormalize();
    const Quatd rotation = normalized ? normalized.Value() : Quatd::Identity();
    const Vec3d direction = SafeNormal(position_.EvalDerivative(key, Vec3d()));
    const Vec3d up = rotation.RotateVector(defaultUpVector_);
    return MakeFromXZ(direction, up);
}

Vec3d SplineComponent::LocationAtSplineInputKey(float key,
                                                SplineCoordinateSpace space) const noexcept {
    const Vec3d local = position_.Eval(key, Vec3d());
    return space == SplineCoordinateSpace::World ? ToWorldPoint(local) : local;
}

Vec3d SplineComponent::TangentAtSplineInputKey(float key,
                                               SplineCoordinateSpace space) const noexcept {
    const Vec3d local = position_.EvalDerivative(key, Vec3d());
    return space == SplineCoordinateSpace::World ? ToWorldVector(local) : local;
}

Vec3d SplineComponent::DirectionAtSplineInputKey(float key,
                                                 SplineCoordinateSpace space) const noexcept {
    const Vec3d local = SafeNormal(position_.EvalDerivative(key, Vec3d()));
    return space == SplineCoordinateSpace::World ? SafeNormal(ToWorldVector(local)) : local;
}

Quatd SplineComponent::QuaternionAtSplineInputKey(float key,
                                                  SplineCoordinateSpace space) const noexcept {
    const Quatd local = LocalQuaternionAtKey(key);
    return space == SplineCoordinateSpace::World ? WorldQuaternion() * local : local;
}

Rotator<double>
SplineComponent::RotationAtSplineInputKey(float key, SplineCoordinateSpace space) const noexcept {
    auto rotator = Rotator<double>::TryFromQuaternion(QuaternionAtSplineInputKey(key, space));
    return rotator ? rotator.Value() : Rotator<double>();
}

Vec3d SplineComponent::UpVectorAtSplineInputKey(float key,
                                                SplineCoordinateSpace space) const noexcept {
    return QuaternionAtSplineInputKey(key, space).RotateVector(Vec3d(0.0, 0.0, 1.0));
}

Vec3d SplineComponent::RightVectorAtSplineInputKey(float key,
                                                   SplineCoordinateSpace space) const noexcept {
    return QuaternionAtSplineInputKey(key, space).RotateVector(Vec3d(0.0, 1.0, 0.0));
}

double SplineComponent::RollAtSplineInputKey(float key,
                                             SplineCoordinateSpace space) const noexcept {
    return RotationAtSplineInputKey(key, space).roll;
}

Vec3d SplineComponent::ScaleAtSplineInputKey(float key) const noexcept {
    return scale_.Eval(key, Vec3d::One());
}

Transform SplineComponent::TransformAtSplineInputKey(float key, SplineCoordinateSpace space,
                                                     bool useScale) const noexcept {
    const Transform local(LocalQuaternionAtKey(key), position_.Eval(key, Vec3d()),
                          useScale ? ScaleAtSplineInputKey(key) : Vec3d::One());
    if (space == SplineCoordinateSpace::Local) {
        return local;
    }

    // UE T * ComponentTransform의 성분 합성(실패하지 않는다): 회전 = 컴포넌트 회전 × 회전, scale =
    // 성분별 곱, 위치 = 컴포넌트 월드 변환으로 옮긴 점(Rocks 월드 행렬 — 25.5절).
    Transform world;
    world.rotation = WorldQuaternion() * local.rotation;
    world.scale = local.scale * WorldScale3D();
    world.translation = ToWorldPoint(local.translation);
    return world;
}

// --- 거리 질의 ---

double SplineComponent::DistanceAlongSplineAtSplineInputKey(float key) const noexcept {
    // UE GetDistanceAlongSplineAtSplineInputKey(SplineComponent.cpp:1153). 구간 수는 곡선의 실제
    // 루프 상태로 센다(25.8절 "거리·구간 길이의 구간 수").
    const int32 count = SplinePointCount();
    const int32 segments = position_.IsLooped() ? count : count - 1;
    if (key >= 0.0f && key < static_cast<float>(segments)) {
        EnsureReparamTable();
        const int32 segment = static_cast<int32>(math::Floor(key));
        const usize entry =
            static_cast<usize>(segment) * static_cast<usize>(reparamStepsPerSegment_);
        const double start =
            entry < reparam_.Size() ? static_cast<double>(reparam_[entry].distance) : 0.0;
        return start + SegmentLength(segment, key - static_cast<float>(segment));
    }

    if (key >= static_cast<float>(segments)) {
        return SplineLength();
    }

    return 0.0;
}

float SplineComponent::InputKeyValueAtDistanceAlongSpline(double distance) const noexcept {
    if (SplinePointCount() < 2) {
        return 0.0f;
    }

    EnsureReparamTable();
    if (reparam_.IsEmpty()) {
        return 0.0f;
    }

    // UE FInterpCurveFloat::Eval(Linear 키 — InterpCurve.h:310): 첫 거리 미만은 첫 값, 그 밖은 거리
    // ≤ 입력인 마지막 항목(이진 탐색 — GetPointIndexForInputValue)이고, 그것이 마지막이면 마지막
    // 값, 아니면 다음 항목과 보간한다(간격 0이면 그 항목 값). 같은 거리가 이어지면 뒤 항목이
    // 뽑힌다.
    const float input = static_cast<float>(distance);
    if (math::IsNaN(input)) {
        return 0.0f;
    }

    const usize last = reparam_.Size() - 1;
    if (input < reparam_[0].distance) {
        return reparam_[0].key;
    }

    if (input >= reparam_[last].distance) {
        return reparam_[last].key;
    }

    usize low = 0;
    usize high = reparam_.Size();
    while (high - low > 1) {
        const usize middle = (low + high) / 2;
        if (reparam_[middle].distance <= input) {
            low = middle;
        } else {
            high = middle;
        }
    }

    const ReparamEntry& from = reparam_[low];
    const ReparamEntry& to = reparam_[low + 1];
    const float span = to.distance - from.distance;
    if (!(span > 0.0f)) {
        return from.key;
    }

    const float alpha = (input - from.distance) / span;
    return from.key + alpha * (to.key - from.key);
}

Vec3d SplineComponent::LocationAtDistanceAlongSpline(double distance,
                                                     SplineCoordinateSpace space) const noexcept {
    return LocationAtSplineInputKey(InputKeyValueAtDistanceAlongSpline(distance), space);
}

Vec3d SplineComponent::TangentAtDistanceAlongSpline(double distance,
                                                    SplineCoordinateSpace space) const noexcept {
    return TangentAtSplineInputKey(InputKeyValueAtDistanceAlongSpline(distance), space);
}

Vec3d SplineComponent::DirectionAtDistanceAlongSpline(double distance,
                                                      SplineCoordinateSpace space) const noexcept {
    return DirectionAtSplineInputKey(InputKeyValueAtDistanceAlongSpline(distance), space);
}

Quatd SplineComponent::QuaternionAtDistanceAlongSpline(double distance,
                                                       SplineCoordinateSpace space) const noexcept {
    return QuaternionAtSplineInputKey(InputKeyValueAtDistanceAlongSpline(distance), space);
}

Vec3d SplineComponent::UpVectorAtDistanceAlongSpline(double distance,
                                                     SplineCoordinateSpace space) const noexcept {
    return UpVectorAtSplineInputKey(InputKeyValueAtDistanceAlongSpline(distance), space);
}

Vec3d SplineComponent::RightVectorAtDistanceAlongSpline(
    double distance, SplineCoordinateSpace space) const noexcept {
    return RightVectorAtSplineInputKey(InputKeyValueAtDistanceAlongSpline(distance), space);
}

double SplineComponent::RollAtDistanceAlongSpline(double distance,
                                                  SplineCoordinateSpace space) const noexcept {
    return RollAtSplineInputKey(InputKeyValueAtDistanceAlongSpline(distance), space);
}

Vec3d SplineComponent::ScaleAtDistanceAlongSpline(double distance) const noexcept {
    return ScaleAtSplineInputKey(InputKeyValueAtDistanceAlongSpline(distance));
}

Transform SplineComponent::TransformAtDistanceAlongSpline(double distance,
                                                          SplineCoordinateSpace space,
                                                          bool useScale) const noexcept {
    return TransformAtSplineInputKey(InputKeyValueAtDistanceAlongSpline(distance), space, useScale);
}

// --- 시간 질의 ---

Interval<float> SplineComponent::InputKeyRange() const noexcept {
    const usize count = position_.PointCount();
    if (count == 0) {
        return Interval<float>(0.0f, 0.0f);
    }

    const float first = position_.GetPoint(0).inValue;
    const float last = position_.GetPoint(count - 1).inValue +
                       (position_.IsLooped() ? position_.LoopKeyOffset() : 0.0f);
    return Interval<float>(first, last);
}

float SplineComponent::InputKeyValueAtTime(float time) const noexcept {
    // UE GetInputKeyValueAtTime(SplineComponent.cpp:2570).
    const Interval<float> range = InputKeyRange();
    if (!(duration_ > 0.0f)) {
        return range.min;
    }

    const float alpha = math::Clamp(time, 0.0f, duration_) / duration_;
    return range.Interpolate(alpha);
}

float SplineComponent::TimeAtDistanceAlongSpline(double distance) const noexcept {
    // UE GetTimeAtDistanceAlongSpline(SplineComponent.cpp:2600): 점 2개 미만 0, 아니면 duration ×
    // GetT(키)(GetT — BoxTypes.h:168: ≤ min 0, ≥ max 1, 그 사이 비율).
    if (SplinePointCount() < 2) {
        return 0.0f;
    }

    const Interval<float> range = InputKeyRange();
    const float key = InputKeyValueAtDistanceAlongSpline(distance);
    float alpha = 0.0f;
    if (key <= range.min) {
        alpha = 0.0f;
    } else if (key >= range.max) {
        alpha = 1.0f;
    } else {
        alpha = (key - range.min) / (range.max - range.min);
    }

    return duration_ * alpha;
}

bool SplineComponent::KeyAtTime(float time, bool useConstantVelocity, float& key) const noexcept {
    if (!(duration_ > 0.0f)) {
        return false;
    }

    key = useConstantVelocity ? InputKeyValueAtDistanceAlongSpline(
                                    static_cast<double>(time / duration_) * SplineLength())
                              : InputKeyValueAtTime(time);
    return true;
}

Vec3d SplineComponent::LocationAtTime(float time, SplineCoordinateSpace space,
                                      bool useConstantVelocity) const noexcept {
    float key = 0.0f;
    return KeyAtTime(time, useConstantVelocity, key) ? LocationAtSplineInputKey(key, space)
                                                     : Vec3d();
}

Vec3d SplineComponent::DirectionAtTime(float time, SplineCoordinateSpace space,
                                       bool useConstantVelocity) const noexcept {
    float key = 0.0f;
    return KeyAtTime(time, useConstantVelocity, key) ? DirectionAtSplineInputKey(key, space)
                                                     : Vec3d();
}

Vec3d SplineComponent::TangentAtTime(float time, SplineCoordinateSpace space,
                                     bool useConstantVelocity) const noexcept {
    float key = 0.0f;
    return KeyAtTime(time, useConstantVelocity, key) ? TangentAtSplineInputKey(key, space)
                                                     : Vec3d();
}

Quatd SplineComponent::QuaternionAtTime(float time, SplineCoordinateSpace space,
                                        bool useConstantVelocity) const noexcept {
    float key = 0.0f;
    return KeyAtTime(time, useConstantVelocity, key) ? QuaternionAtSplineInputKey(key, space)
                                                     : Quatd::Identity();
}

Vec3d SplineComponent::UpVectorAtTime(float time, SplineCoordinateSpace space,
                                      bool useConstantVelocity) const noexcept {
    float key = 0.0f;
    return KeyAtTime(time, useConstantVelocity, key) ? UpVectorAtSplineInputKey(key, space)
                                                     : Vec3d();
}

Vec3d SplineComponent::RightVectorAtTime(float time, SplineCoordinateSpace space,
                                         bool useConstantVelocity) const noexcept {
    float key = 0.0f;
    return KeyAtTime(time, useConstantVelocity, key) ? RightVectorAtSplineInputKey(key, space)
                                                     : Vec3d();
}

Vec3d SplineComponent::ScaleAtTime(float time, bool useConstantVelocity) const noexcept {
    float key = 0.0f;
    return KeyAtTime(time, useConstantVelocity, key) ? ScaleAtSplineInputKey(key) : Vec3d::One();
}

Transform SplineComponent::TransformAtTime(float time, SplineCoordinateSpace space,
                                           bool useConstantVelocity, bool useScale) const noexcept {
    float key = 0.0f;
    return KeyAtTime(time, useConstantVelocity, key)
               ? TransformAtSplineInputKey(key, space, useScale)
               : Transform::Identity();
}

// --- 최근접 ---

float SplineComponent::FindInputKeyClosestToWorldLocation(
    const Vec3d& worldLocation) const noexcept {
    auto local = ToLocalPoint(worldLocation);
    if (!local) {
        return 0.0f;
    }

    auto nearest = position_.TryFindNearest(local.Value());
    return nearest ? nearest.Value().input : 0.0f;
}

Vec3d SplineComponent::FindLocationClosestToWorldLocation(
    const Vec3d& worldLocation, SplineCoordinateSpace space) const noexcept {
    return LocationAtSplineInputKey(FindInputKeyClosestToWorldLocation(worldLocation), space);
}

double SplineComponent::DistanceAlongSplineAtLocation(const Vec3d& location,
                                                      SplineCoordinateSpace space) const noexcept {
    if (space == SplineCoordinateSpace::World) {
        return DistanceAlongSplineAtSplineInputKey(FindInputKeyClosestToWorldLocation(location));
    }

    // Local 위치는 그대로 찾는다(UE와 같다 — 월드를 거치지 않는다).
    auto nearest = position_.TryFindNearest(location);
    return DistanceAlongSplineAtSplineInputKey(nearest ? nearest.Value().input : 0.0f);
}

// --- bounds ---

bool SplineComponent::TryLocalBounds(ComponentBounds& local) const noexcept {
    // UE CalcBounds(SplineComponent.cpp:3925): 구간 극값 상자(점 1개면 그 점, 0개면 원점).
    local = ComponentBounds{};
    auto bounds = position_.TryCalcBounds();
    if (bounds) {
        local.origin = (bounds.Value().minimum + bounds.Value().maximum) * 0.5;
        local.boxExtent = (bounds.Value().maximum - bounds.Value().minimum) * 0.5;
        local.sphereRadius = local.boxExtent.Length();
    }

    return true;
}

} // namespace rocks
