#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Math/InterpCurve.h>
#include <Core/Math/Interval.h>
#include <Core/Math/Quaternion.h>
#include <Core/Math/Rotator.h>
#include <Core/Math/Transform.h>
#include <Core/Math/Vector3.h>
#include <Meta/Annotations.h>
#include <World/PrimitiveComponent.h>

namespace rocks {

// 스플라인 점의 보간 종류다(UE ESplinePointType — SplineComponent.h:24). 위치 곡선의 키 모드로
// 저장한다(Docs/Design/EngineRuntime 25.2절).
ROCKS_ENUM()
enum class SplinePointType : uint8 {
    Linear,
    Curve,
    Constant,
    CurveClamped,
    CurveCustomTangent
};

// 스플라인 질의·편집의 좌표계다(UE ESplineCoordinateSpace).
ROCKS_ENUM()
enum class SplineCoordinateSpace : uint8 {
    Local,
    World
};

// 점 하나를 통째로 넣을 때의 값이다(UE FSplinePoint — SplineComponent.h:156). 위치·접선은 Local
// 공간(cm), 회전은 도 단위 Rotator다.
struct SplinePoint {
    float inputKey = 0.0f;
    Vec3d position;
    Vec3d arriveTangent;
    Vec3d leaveTangent;
    Rotator<double> rotation;
    Vec3d scale = Vec3d::One();
    SplinePointType type = SplinePointType::Curve;
};

// 위치·회전·scale 곡선으로 된 스플라인이다(UE USplineComponent의 FSplineCurves 경로 — Docs/Design/
// EngineRuntime 25장). 렌더·물리 없이 끝까지 동작한다(디버그 그리기만 RS 단계). 기본은 (0, 0, 0)과
// (100, 0, 0)의 두 점(길이 100)이고 충돌 profile은 "NoCollision"이다.
//
// 규약:
// - 키 i는 점 i의 입력값이다. 편집 API는 UE처럼 키를 0, 1, 2, …로 유지한다(25.6절).
// - 편집 API의 updateSpline이 거짓이면 접선·거리 표를 다시 계산하지 않는다. 여러 점을 바꾼 뒤
//   UpdateSpline()을 한 번 부른다(UE와 같다).
// - 길이·거리는 월드 scale을 곱한 값(cm)이다. 월드 scale이 바뀌면 거리 표를 다시 만든다(UE는
//   낡은 표를 둔다 — 25.4절).
// - 범위 밖 점 index는 InvalidArgument이고 바꾸지 않는다(UE는 조용히 무시).
// - World 공간 입력은 컴포넌트 월드 행렬의 역으로 바꾼다. 역행렬이 없으면 InvalidArgument다.
//
// 사용 예:
//   world.With<SplineComponent>(spline, [](SplineComponent& path) noexcept {
//       (void)path.AddSplinePoint(Vec3d(200.0, 100.0, 0.0), SplineCoordinateSpace::Local);
//       const double length = path.SplineLength();
//       return path.LocationAtDistanceAlongSpline(length * 0.5, SplineCoordinateSpace::World);
//   });
//
// 스레드: World의 게임 스레드 전용이다.
ROCKS_CLASS()
class ROCKS_WORLD_API SplineComponent : public PrimitiveComponent {
    ROCKS_GENERATED_BODY()

public:
    // 구간당 거리 표 단계 수 기본·범위(UE ReparamStepsPerSegment 10, ClampMin 4·ClampMax 100)다.
    static constexpr int32 DefaultReparamStepsPerSegment = 10;
    static constexpr int32 MinReparamStepsPerSegment = 4;
    static constexpr int32 MaxReparamStepsPerSegment = 100;
    // 기본 재생 시간(초, UE Duration 1)과 기본 두 번째 점 위치(cm, UE SetDefaultSpline)다.
    static constexpr float DefaultDuration = 1.0f;
    static constexpr double DefaultSecondPointX = 100.0;
    // 클래스 기본 충돌 profile(UE SplineComponent.cpp:358)이다.
    static constexpr const char* SplineCollisionProfileName = "NoCollision";

    explicit SplineComponent(Allocator& allocator) noexcept;

    StringView DefaultCollisionProfileName() const noexcept override {
        return StringView(SplineCollisionProfileName);
    }

    // --- 갱신·설정 ---

    // 루프 키·자동 접선·거리 표를 다시 계산한다(UE UpdateSpline — 25.4절).
    void UpdateSpline() noexcept;

    int32 ReparamStepsPerSegment() const noexcept {
        return reparamStepsPerSegment_;
    }

    // 오류: [MinReparamStepsPerSegment, MaxReparamStepsPerSegment] 밖. 바뀌면 UpdateSpline.
    Result<void> SetReparamStepsPerSegment(int32 steps) noexcept;

    float Duration() const noexcept {
        return duration_;
    }

    // 오류: 음수·비유한.
    Result<void> SetDuration(float seconds) noexcept;

    bool HasStationaryEndpoints() const noexcept {
        return stationaryEndpoints_;
    }

    // 바뀌면 UpdateSpline(끝점 접선 0).
    void SetStationaryEndpoints(bool stationary) noexcept;

    bool IsClosedLoop() const noexcept {
        return closedLoop_;
    }

    // loopPositionOverride를 끈다(UE SetClosedLoop).
    void SetClosedLoop(bool closed, bool updateSpline = true) noexcept;

    // 루프 키를 key로 정한다(UE SetClosedLoopAtPosition). 오류: 비유한 key.
    Result<void> SetClosedLoopAtPosition(bool closed, float key, bool updateSpline = true) noexcept;

    bool HasLoopPositionOverride() const noexcept {
        return loopPositionOverride_;
    }

    float LoopPosition() const noexcept {
        return loopPosition_;
    }

    // 회전 곡선이 단위일 때의 up 방향이다(Local 공간 기준으로 저장한다).
    Vec3d DefaultUpVector(SplineCoordinateSpace space) const noexcept;

    // 오류: 0·비유한 벡터, World 역변환 실패. 늘 UpdateSpline(UE와 같다).
    Result<void> SetDefaultUpVector(const Vec3d& up, SplineCoordinateSpace space) noexcept;

    // --- 곡선 ---

    const InterpCurve<Vec3d>& PositionCurve() const noexcept {
        return position_;
    }

    const InterpCurve<Quatd>& RotationCurve() const noexcept {
        return rotation_;
    }

    const InterpCurve<Vec3d>& ScaleCurve() const noexcept {
        return scale_;
    }

    // --- 점 개수·점 단위 읽기 ---

    int32 SplinePointCount() const noexcept {
        return static_cast<int32>(position_.PointCount());
    }

    // 점 2개 미만이면 0, 곡선이 루프면 점 수, 아니면 점 수 − 1이다(UE GetNumberOfSplineSegments).
    int32 SplineSegmentCount() const noexcept;

    // 점의 입력 키다. 닫힌 스플라인은 index = 점 수가 루프 끝 키다. 오류: 범위 밖.
    Result<float> InputKeyValueAtSplinePoint(int32 index) const noexcept;

    // 오류(아래 점 읽기 공통): 범위 밖 index.
    Result<Vec3d> LocationAtSplinePoint(int32 index, SplineCoordinateSpace space) const noexcept;
    Result<Vec3d> ArriveTangentAtSplinePoint(int32 index,
                                             SplineCoordinateSpace space) const noexcept;
    Result<Vec3d> LeaveTangentAtSplinePoint(int32 index,
                                            SplineCoordinateSpace space) const noexcept;
    Result<Vec3d> ScaleAtSplinePoint(int32 index) const noexcept;
    Result<SplinePointType> SplinePointTypeAt(int32 index) const noexcept;
    Result<Quatd> QuaternionAtSplinePoint(int32 index, SplineCoordinateSpace space) const noexcept;

    // 점까지 거리다(닫힌 스플라인 index = 점 수면 전체 길이 — UE
    // GetDistanceAlongSplineAtSplinePoint).
    Result<double> DistanceAlongSplineAtSplinePoint(int32 index) const noexcept;

    // 전체 길이(cm)다(UE GetSplineLength — 거리 표 마지막 거리).
    double SplineLength() const noexcept;

    // --- 점 편집(25.6절) ---

    void ClearSplinePoints(bool updateSpline = true) noexcept;

    // 키 = 마지막 키 + 1(빈 곡선 0), CurveAuto·단위 회전·scale 1. 오류: 할당 실패, World 역변환
    // 실패(바뀌지 않는다).
    Result<void> AddSplinePoint(const Vec3d& position, SplineCoordinateSpace space,
                                bool updateSpline = true) noexcept;

    // index(0 ≤ index ≤ 점 수)에 넣고 뒤 키를 1씩 민다. 오류: 범위 밖, 할당 실패, 역변환 실패.
    Result<void> AddSplinePointAtIndex(const Vec3d& position, int32 index,
                                       SplineCoordinateSpace space,
                                       bool updateSpline = true) noexcept;

    // 점 하나를 통째로 넣는다(UE AddPoint — 같은 키가 있으면 그 자리에 넣고 뒤를 민다). 오류:
    // 비유한 키, 할당 실패.
    Result<void> AddPoint(const SplinePoint& point, bool updateSpline = true) noexcept;

    // 점을 지우고 뒤 키를 앞 간격만큼 당긴다. 오류: 범위 밖.
    Result<void> RemoveSplinePoint(int32 index, bool updateSpline = true) noexcept;

    // 점들을 키 0, 1, 2, …로 다시 만든다(loopPositionOverride 끔). 오류: 할당 실패, 역변환 실패
    // (실패하면 바뀌지 않는다).
    Result<void> SetSplinePoints(Span<const Vec3d> positions, SplineCoordinateSpace space,
                                 bool updateSpline = true) noexcept;

    // 오류(아래 점 편집 공통): 범위 밖 index, 비유한 값, World 역변환 실패.
    Result<void> SetLocationAtSplinePoint(int32 index, const Vec3d& position,
                                          SplineCoordinateSpace space,
                                          bool updateSpline = true) noexcept;

    // 도착·출발 접선을 같은 값으로 두고 모드를 CurveUser로 바꾼다(UE SetTangentAtSplinePoint).
    Result<void> SetTangentAtSplinePoint(int32 index, const Vec3d& tangent,
                                         SplineCoordinateSpace space,
                                         bool updateSpline = true) noexcept;

    Result<void> SetTangentsAtSplinePoint(int32 index, const Vec3d& arrive, const Vec3d& leave,
                                          SplineCoordinateSpace space,
                                          bool updateSpline = true) noexcept;

    // 회전 키 = FindBetween(defaultUpVector, 정규화(up))다. 0 벡터는 InvalidArgument.
    Result<void> SetUpVectorAtSplinePoint(int32 index, const Vec3d& up, SplineCoordinateSpace space,
                                          bool updateSpline = true) noexcept;

    // 점의 회전을 rotation으로 맞춘다(UE SetQuaternionAtSplinePoint — up을 맞추고 접선 방향을 같은
    // 상대 회전으로 돌린다, 모드는 CurveUser).
    Result<void> SetQuaternionAtSplinePoint(int32 index, const Quatd& rotation,
                                            SplineCoordinateSpace space,
                                            bool updateSpline = true) noexcept;

    // 도 단위 Rotator 판이다(UE SetRotationAtSplinePoint).
    Result<void> SetRotationAtSplinePoint(int32 index, const Rotator<double>& rotation,
                                          SplineCoordinateSpace space,
                                          bool updateSpline = true) noexcept {
        return SetQuaternionAtSplinePoint(index, rotation.ToQuaternion(), space, updateSpline);
    }

    Result<void> SetScaleAtSplinePoint(int32 index, const Vec3d& scale,
                                       bool updateSpline = true) noexcept;

    // 위치 곡선의 모드만 바꾼다. 오류: 범위 밖 index·enum.
    Result<void> SetSplinePointType(int32 index, SplinePointType type,
                                    bool updateSpline = true) noexcept;

    // --- 키 질의(25.5절) ---

    Vec3d LocationAtSplineInputKey(float key, SplineCoordinateSpace space) const noexcept;
    Vec3d TangentAtSplineInputKey(float key, SplineCoordinateSpace space) const noexcept;
    Vec3d DirectionAtSplineInputKey(float key, SplineCoordinateSpace space) const noexcept;
    Quatd QuaternionAtSplineInputKey(float key, SplineCoordinateSpace space) const noexcept;
    Rotator<double> RotationAtSplineInputKey(float key, SplineCoordinateSpace space) const noexcept;
    Vec3d UpVectorAtSplineInputKey(float key, SplineCoordinateSpace space) const noexcept;
    Vec3d RightVectorAtSplineInputKey(float key, SplineCoordinateSpace space) const noexcept;
    double RollAtSplineInputKey(float key, SplineCoordinateSpace space) const noexcept;
    Vec3d ScaleAtSplineInputKey(float key) const noexcept;
    Transform TransformAtSplineInputKey(float key, SplineCoordinateSpace space,
                                        bool useScale = false) const noexcept;

    // --- 거리 질의 ---

    double DistanceAlongSplineAtSplineInputKey(float key) const noexcept;
    float InputKeyValueAtDistanceAlongSpline(double distance) const noexcept;

    Vec3d LocationAtDistanceAlongSpline(double distance,
                                        SplineCoordinateSpace space) const noexcept;
    Vec3d TangentAtDistanceAlongSpline(double distance, SplineCoordinateSpace space) const noexcept;
    Vec3d DirectionAtDistanceAlongSpline(double distance,
                                         SplineCoordinateSpace space) const noexcept;
    Quatd QuaternionAtDistanceAlongSpline(double distance,
                                          SplineCoordinateSpace space) const noexcept;
    Vec3d UpVectorAtDistanceAlongSpline(double distance,
                                        SplineCoordinateSpace space) const noexcept;
    Vec3d RightVectorAtDistanceAlongSpline(double distance,
                                           SplineCoordinateSpace space) const noexcept;
    double RollAtDistanceAlongSpline(double distance, SplineCoordinateSpace space) const noexcept;
    Vec3d ScaleAtDistanceAlongSpline(double distance) const noexcept;
    Transform TransformAtDistanceAlongSpline(double distance, SplineCoordinateSpace space,
                                             bool useScale = false) const noexcept;

    // --- 시간 질의(duration 기준 — 25.5절) ---

    // 키 범위다(첫 키, 마지막 키 + 루프 오프셋). 점이 없으면 [0, 0]이다.
    Interval<float> InputKeyRange() const noexcept;
    float InputKeyValueAtTime(float time) const noexcept;
    float TimeAtDistanceAlongSpline(double distance) const noexcept;

    // duration이 0이면 0 벡터·단위 회전·scale 1이다. useConstantVelocity면 거리 기준이다.
    Vec3d LocationAtTime(float time, SplineCoordinateSpace space,
                         bool useConstantVelocity = false) const noexcept;
    Vec3d DirectionAtTime(float time, SplineCoordinateSpace space,
                          bool useConstantVelocity = false) const noexcept;
    Vec3d TangentAtTime(float time, SplineCoordinateSpace space,
                        bool useConstantVelocity = false) const noexcept;
    Quatd QuaternionAtTime(float time, SplineCoordinateSpace space,
                           bool useConstantVelocity = false) const noexcept;
    Vec3d UpVectorAtTime(float time, SplineCoordinateSpace space,
                         bool useConstantVelocity = false) const noexcept;
    Vec3d RightVectorAtTime(float time, SplineCoordinateSpace space,
                            bool useConstantVelocity = false) const noexcept;
    Vec3d ScaleAtTime(float time, bool useConstantVelocity = false) const noexcept;
    Transform TransformAtTime(float time, SplineCoordinateSpace space,
                              bool useConstantVelocity = false,
                              bool useScale = false) const noexcept;

    // --- 최근접 ---

    // 월드 위치에 가장 가까운 키다(UE FindInputKeyClosestToWorldLocation, 빈 스플라인 0). 역변환
    // 실패도 0이다.
    float FindInputKeyClosestToWorldLocation(const Vec3d& worldLocation) const noexcept;
    Vec3d FindLocationClosestToWorldLocation(const Vec3d& worldLocation,
                                             SplineCoordinateSpace space) const noexcept;
    // location(space 기준)에 가장 가까운 점까지 거리다(UE GetDistanceAlongSplineAtLocation).
    double DistanceAlongSplineAtLocation(const Vec3d& location,
                                         SplineCoordinateSpace space) const noexcept;

    // 구간 index의 앞부분 param ∈ [0, 1] 길이(cm, 월드 scale 포함 — UE GetSegmentLength)다. 범위 밖
    // index·param은 0이다.
    double SegmentLength(int32 segment, float param) const noexcept;

    // 위치 곡선 모드 ↔ 점 종류다(UE ConvertInterpCurveModeToSplinePointType 등).
    static InterpCurveMode ToCurveMode(SplinePointType type) noexcept;
    static SplinePointType ToPointType(InterpCurveMode mode) noexcept;

protected:
    bool TryLocalBounds(ComponentBounds& local) const noexcept override;

    BoundsSphereRule LocalBoundsSphereRule() const noexcept override {
        return BoundsSphereRule::BoxExtent;
    }

    // 월드 scale이 바뀌었으면 거리 표를 버린다(다음 질의가 다시 만든다).
    void OnTransformUpdated() noexcept override;

private:
    // 거리 표 항목이다(UE ReparamTable의 Linear 키 — 거리 → 구간 번호, float).
    struct ReparamEntry {
        float distance = 0.0f;
        float key = 0.0f;
    };

    // 루프 키·자동 접선만 다시 계산한다(UpdateSpline의 1–2단계).
    void UpdateCurves() noexcept;
    // 거리 표가 지금 곡선·월드 scale과 맞게 있는지 보고, 아니면 만든다.
    void EnsureReparamTable() const noexcept;
    void BuildReparamTable() const noexcept;
    float SegmentLengthWithScale(usize segment, float param, const Vec3d& scale) const noexcept;
    // 범위 밖이면 거짓이다.
    bool IsValidPoint(int32 index) const noexcept;
    // 루프 키를 건다(25.4절 1).
    void ApplyLoopKey() noexcept;
    // 시간 → 키다. duration이 0이면 거짓이다.
    bool KeyAtTime(float time, bool useConstantVelocity, float& key) const noexcept;
    // 편집 뒤 처리다(updateSpline이면 UpdateSpline, 아니면 bounds·다시 보내기만).
    void FinishEdit(bool updateSpline) noexcept;
    // 세 곡선에 같은 키의 점을 넣는다(하나라도 실패하면 넣은 것을 되돌린다).
    Result<void> InsertPoint(const InterpCurvePoint<Vec3d>& position,
                             const InterpCurvePoint<Quatd>& rotation,
                             const InterpCurvePoint<Vec3d>& scale) noexcept;
    // from 이후(뒤에서부터) 점의 키에 delta를 더한다(순서 유지).
    void ShiftKeysFrom(usize from, float delta) noexcept;

    Vec3d ToWorldPoint(const Vec3d& local) const noexcept;
    Vec3d ToWorldVector(const Vec3d& local) const noexcept;
    Result<Vec3d> ToLocalPoint(const Vec3d& world) const noexcept;
    Result<Vec3d> ToLocalVector(const Vec3d& world) const noexcept;
    Quatd LocalQuaternionAtKey(float key) const noexcept;

    ROCKS_PROPERTY(ReadOnly, Name = "position") InterpCurve<Vec3d> position_;
    ROCKS_PROPERTY(ReadOnly, Name = "rotation") InterpCurve<Quatd> rotation_;
    ROCKS_PROPERTY(ReadOnly, Name = "scale") InterpCurve<Vec3d> scale_;
    ROCKS_PROPERTY(ReadOnly, Name = "reparamStepsPerSegment")
    int32 reparamStepsPerSegment_ = DefaultReparamStepsPerSegment;
    ROCKS_PROPERTY(ReadOnly, Name = "duration") float duration_ = DefaultDuration;
    ROCKS_PROPERTY(ReadOnly, Name = "stationaryEndpoints") bool stationaryEndpoints_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "closedLoop") bool closedLoop_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "loopPositionOverride") bool loopPositionOverride_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "loopPosition") float loopPosition_ = 0.0f;
    ROCKS_PROPERTY(ReadOnly, Name = "defaultUpVector")
    Vec3d defaultUpVector_ = Vec3d(0.0, 0.0, 1.0);
    // 유도 값이다(25.4절 — 영속하지 않는다).
    ROCKS_NATIVE() mutable Array<ReparamEntry> reparam_;
    ROCKS_NATIVE() mutable Vec3d reparamScale_;
    ROCKS_NATIVE() mutable bool reparamValid_ = false;
};

} // namespace rocks
