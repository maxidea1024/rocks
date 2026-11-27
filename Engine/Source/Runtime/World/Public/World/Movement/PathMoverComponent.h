#pragma once

#include <Core/Containers/Array.h>
#include <Meta/Annotations.h>
#include <Object/Event.h>
#include <World/Movement/MovementComponent.h>
#include <World/Physics/HitResult.h>
#include <World/Movement/RotatingMovementComponent.h>

namespace rocks {

// PathMover의 끝 동작이다(UE EInterpToBehaviourType).
ROCKS_ENUM()
enum class PathMoverBehaviour : uint8 {
    // 끝에서 멈춘다.
    OneShot,
    // 끝에서 돌아와 시작에서 멈춘다.
    OneShotReverse,
    // 끝에서 시작으로 돌아가 다시 간다.
    LoopReset,
    // 끝과 시작 사이를 오간다.
    PingPong
};

// PathMover 제어점이다(UE FInterpControlPoint). 상대면 시작 위치 기준이다(cm).
ROCKS_STRUCT()
struct PathControlPoint {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() Vec3d position;
    ROCKS_PROPERTY() bool positionIsRelative = true;
};

// 제어점 사이를 일정 시간에 선형으로 오가는 이동이다(UE UInterpToMovementComponent — 이름은
// Docs/Design/ComponentCatalog CD6, 동작은 Docs/Design/EngineRuntime 26.6절). 충돌 sweep 없이
// 움직인다 (pauseOnImpact·sweep은 데이터만 — 물리 CR16 때 쓴다).
//
// 사용 예(Actor OnAwake):
//   auto mover = GetWorld().AddComponent<PathMoverComponent>(Handle());
//   GetWorld().With<PathMoverComponent>(mover.Value(), [](PathMoverComponent& path) noexcept {
//       (void)path.AddControlPoint(Vec3d(), true);
//       (void)path.AddControlPoint(Vec3d(0.0, 0.0, 300.0), true);   // 위로 3 m
//       (void)path.SetBehaviour(PathMoverBehaviour::PingPong);
//       return path.SetDuration(2.0f);
//   });
//
// 스레드: World의 게임 스레드 전용이다.
ROCKS_CLASS()
class ROCKS_WORLD_API PathMoverComponent : public MovementComponent {
    ROCKS_GENERATED_BODY()

public:
    // UE 생성자 기본값·범위다(InterpToMovementComponent.cpp:14–34, 헤더 ClampMin).
    static constexpr float DefaultDuration = 1.0f;
    static constexpr float MinDuration = 0.1f;
    static constexpr double DefaultMaxSimulationTimeStep = 0.05;
    static constexpr double MinSimulationTimeStep = 0.0166;
    static constexpr double MaxSimulationTimeStepLimit = 0.50;
    static constexpr int32 DefaultMaxSimulationIterations = 8;
    static constexpr int32 MaxSimulationIterationsLimit = 25;
    // 시뮬레이션 단계의 최소 시간(UE MIN_TICK_TIME 0.0002)과 끝 반사 재시도 상한(2)이다.
    static constexpr double MinTickTime = 0.0002;
    static constexpr int32 MaxBounces = 2;

    explicit PathMoverComponent(Allocator& allocator) noexcept;

    // --- 제어점 ---

    Span<const PathControlPoint> ControlPoints() const noexcept {
        return controlPoints_.View();
    }

    // 오류: 비유한 위치, 할당 실패(UE AddControlPointPosition).
    Result<void> AddControlPoint(const Vec3d& position, bool positionIsRelative = true) noexcept;

    // 멈추고 제어점을 비운다(UE ResetControlPoints).
    void ResetControlPoints() noexcept;

    // 제어점 캐시를 만들고 대상을 점 0으로 옮긴다(UE FinaliseControlPoints — OnStart가 부른다, 이미
    // 했으면 아무것도 하지 않는다).
    void FinaliseControlPoints() noexcept;

    // --- 설정 ---

    float Duration() const noexcept {
        return duration_;
    }

    // 오류: MinDuration 미만·비유한. 시간 배율은 다음 FinaliseControlPoints부터다(UE와 같다).
    Result<void> SetDuration(float seconds) noexcept;

    PathMoverBehaviour Behaviour() const noexcept {
        return behaviour_;
    }

    // 오류: 범위 밖 enum.
    Result<void> SetBehaviour(PathMoverBehaviour behaviour) noexcept;

    float SpeedMultiplier() const noexcept {
        return speedMultiplier_;
    }

    // 0 이하는 곱하지 않는다(UE와 같다). 오류: 비유한.
    Result<void> SetSpeedMultiplier(float multiplier) noexcept;

    bool ForcesSubStepping() const noexcept {
        return forceSubStepping_;
    }

    void SetForceSubStepping(bool force) noexcept {
        forceSubStepping_ = force;
    }

    double MaxSimulationTimeStep() const noexcept {
        return maxSimulationTimeStep_;
    }

    Result<void> SetMaxSimulationTimeStep(double seconds) noexcept;

    int32 MaxSimulationIterations() const noexcept {
        return maxSimulationIterations_;
    }

    Result<void> SetMaxSimulationIterations(int32 iterations) noexcept;

    bool PausesOnImpact() const noexcept {
        return pauseOnImpact_;
    }

    bool Sweeps() const noexcept {
        return sweep_;
    }

    // --- 상태 ---

    // 경로 위 시간 비율 [0, 1]이다.
    float CurrentTime() const noexcept {
        return currentTime_;
    }

    // 1(정방향) 또는 −1이다.
    float CurrentDirection() const noexcept {
        return currentDirection_;
    }

    bool IsStopped() const noexcept {
        return stopped_;
    }

    // 처음부터 다시 간다(UE RestartMovement — direction 1이 정방향).
    void RestartMovement(float initialDirection = 1.0f) noexcept;

    // 멈추고 OnPathStop(지금 시간)·pathStopped를 낸다(UE StopMovementImmediately).
    void StopMovementImmediately() noexcept override;

    // UE delegate다(26.1절 — OnInterpToReverse·OnInterpToStop·OnResetDelegate). 같은 자리의 hook
    // 바로 뒤에 발행한다. 충돌 없는 이동이라 impactResult는 빈 HitResult, time은 경로 시간 [0,
    // 1]이다.
    ROCKS_PROPERTY(Params = "impactResult,time") Event<const HitResult&, float> pathReversed;
    ROCKS_PROPERTY(Params = "impactResult,time") Event<const HitResult&, float> pathStopped;
    ROCKS_PROPERTY(Params = "impactResult,time") Event<const HitResult&, float> pathReset;

    // 시간 t의 위치까지의 이동량이다(UE ComputeMoveDelta — 점은 지금 월드 회전으로 돌린다).
    Vec3d ComputeMoveDelta(float time) const noexcept;

protected:
    void OnStart() noexcept override;
    void OnTick(const TickContext& context) noexcept override;

    // UE delegate 자리다(26.1절): 방향 반전·정지·처음으로 되돌림.
    virtual void OnPathReverse(float time) noexcept {
        (void)time;
    }

    virtual void OnPathStop(float time) noexcept {
        (void)time;
    }

    virtual void OnPathReset(float time) noexcept {
        (void)time;
    }

private:
    // hook을 부르고 같은 이벤트를 낸다(World 장부 밖이면 이벤트 없음).
    void NotifyPathReverse(float time) noexcept;
    void NotifyPathStop(float time) noexcept;
    void NotifyPathReset(float time) noexcept;

    // 점별 캐시다(UE FInterpControlPoint의 StartTime·DistanceToNext·Percentage).
    struct Segment {
        float startTime = 0.0f;
        double distanceToNext = 0.0;
        float percentage = 0.0f;
    };

    void UpdateControlPoints() noexcept;
    float CalculateNewTime(float timeNow, float delta, bool& stopped,
                           float& timeRemainder) noexcept;
    double SimulationTimeStep(double remainingSeconds, int32 iteration) const noexcept;

    ROCKS_PROPERTY(ReadOnly, Name = "controlPoints") Array<PathControlPoint> controlPoints_;
    ROCKS_PROPERTY(ReadOnly, Name = "duration") float duration_ = DefaultDuration;
    ROCKS_PROPERTY(ReadOnly, Name = "behaviour")
    PathMoverBehaviour behaviour_ = PathMoverBehaviour::OneShot;
    ROCKS_PROPERTY(ReadOnly, Name = "pauseOnImpact") bool pauseOnImpact_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "sweep") bool sweep_ = true;
    ROCKS_PROPERTY(ReadOnly, Name = "forceSubStepping") bool forceSubStepping_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "maxSimulationTimeStep")
    double maxSimulationTimeStep_ = DefaultMaxSimulationTimeStep;
    ROCKS_PROPERTY(ReadOnly, Name = "maxSimulationIterations")
    int32 maxSimulationIterations_ = DefaultMaxSimulationIterations;
    ROCKS_PROPERTY(ReadOnly, Name = "speedMultiplier") float speedMultiplier_ = 0.0f;
    ROCKS_NATIVE() Array<Segment> segments_;
    ROCKS_NATIVE() Vec3d startLocation_;
    ROCKS_NATIVE() double totalDistance_ = 0.0;
    ROCKS_NATIVE() float currentTime_ = 0.0f;
    ROCKS_NATIVE() float timeMultiplier_ = 1.0f;
    ROCKS_NATIVE() float currentDirection_ = 1.0f;
    ROCKS_NATIVE() bool stopped_ = false;
    ROCKS_NATIVE() bool pointsFinalized_ = false;
};

} // namespace rocks
