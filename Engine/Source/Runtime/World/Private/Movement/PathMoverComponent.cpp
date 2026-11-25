#include <Core/Math/MathUtility.h>
#include <World/Component.generated.h>
#include <World/Movement/MovementComponent.generated.h>
#include <World/Movement/PathMoverComponent.generated.h>
#include <World/Movement/PathMoverComponent.h>
#include <World/SceneComponent.generated.h>
#include <World/SceneComponent.h>
#include <World/World.h>

// PathMoverComponent다(UE UInterpToMovementComponent — Docs/Design/EngineRuntime 26.6절, D4).
namespace rocks {

namespace {

Result<void> InvalidPathValue() noexcept {
    return Result<void>::Failure(ErrorCode::InvalidArgument);
}

} // namespace

PathMoverComponent::PathMoverComponent(Allocator& allocator) noexcept
    : MovementComponent(allocator), controlPoints_(allocator), segments_(allocator) {
}

Result<void> PathMoverComponent::AddControlPoint(const Vec3d& position,
                                                 bool positionIsRelative) noexcept {
    if (!math::IsFinite(position.x) || !math::IsFinite(position.y) || !math::IsFinite(position.z)) {
        return InvalidPathValue();
    }

    PathControlPoint point;
    point.position = position;
    point.positionIsRelative = positionIsRelative;
    return controlPoints_.TryPushBack(point);
}

void PathMoverComponent::ResetControlPoints() noexcept {
    // UE ResetControlPoints(InterpToMovementComponent.cpp:543).
    stopped_ = true;
    controlPoints_.Clear();
    segments_.Clear();
    pointsFinalized_ = false;
}

Result<void> PathMoverComponent::SetDuration(float seconds) noexcept {
    if (!math::IsFinite(seconds) || seconds < MinDuration) {
        return InvalidPathValue();
    }

    duration_ = seconds;
    return Result<void>::Success();
}

Result<void> PathMoverComponent::SetBehaviour(PathMoverBehaviour behaviour) noexcept {
    if (static_cast<uint8>(behaviour) > static_cast<uint8>(PathMoverBehaviour::PingPong)) {
        return InvalidPathValue();
    }

    behaviour_ = behaviour;
    return Result<void>::Success();
}

Result<void> PathMoverComponent::SetSpeedMultiplier(float multiplier) noexcept {
    if (!math::IsFinite(multiplier)) {
        return InvalidPathValue();
    }

    speedMultiplier_ = multiplier;
    return Result<void>::Success();
}

Result<void> PathMoverComponent::SetMaxSimulationTimeStep(double seconds) noexcept {
    if (!math::IsFinite(seconds) || seconds < MinSimulationTimeStep ||
        seconds > MaxSimulationTimeStepLimit) {
        return InvalidPathValue();
    }

    maxSimulationTimeStep_ = seconds;
    return Result<void>::Success();
}

Result<void> PathMoverComponent::SetMaxSimulationIterations(int32 iterations) noexcept {
    if (iterations < 1 || iterations > MaxSimulationIterationsLimit) {
        return InvalidPathValue();
    }

    maxSimulationIterations_ = iterations;
    return Result<void>::Success();
}

void PathMoverComponent::RestartMovement(float initialDirection) noexcept {
    // UE RestartMovement(:575).
    currentDirection_ = initialDirection;
    currentTime_ = 0.0f;
    stopped_ = false;
}

void PathMoverComponent::StopMovementImmediately() noexcept {
    // UE StopMovementImmediately(:36): 정지 + 정지 hook, 그다음 velocity 0.
    stopped_ = true;
    OnPathStop(currentTime_);
    MovementComponent::StopMovementImmediately();
}

void PathMoverComponent::UpdateControlPoints() noexcept {
    // UE UpdateControlPoints(true)(:475–523): 거리는 회전 없이(상대 점은 지금 위치를 더해) 재고,
    // 거리 비율로 시간 몫을 나눈다. 길이 0인 중간 구간은 몫 0(UE는 초기화하지 않는다)이다.
    const usize count = controlPoints_.Size();
    segments_.Clear();
    if (count == 0 || !segments_.TryReserve(count)) {
        return;
    }

    const Vec3d base = UpdatedLocation();
    totalDistance_ = 0.0;
    Vec3d current = controlPoints_[0].position;
    if (controlPoints_[0].positionIsRelative) {
        current = current + base;
    }

    for (usize index = 0; index < count; ++index) {
        Segment segment;
        if (index + 1 < count) {
            Vec3d next = controlPoints_[index + 1].position;
            if (controlPoints_[index + 1].positionIsRelative) {
                next = next + base;
            }

            segment.distanceToNext = (next - current).Length();
            totalDistance_ += segment.distanceToNext;
            current = next;
        } else {
            segment.distanceToNext = 0.0;
            segment.percentage = 1.0f;
            segment.startTime = 1.0f;
        }

        (void)segments_.TryPushBack(segment);
    }

    float percent = 0.0f;
    for (Segment& segment : segments_) {
        segment.startTime = percent;
        if (segment.distanceToNext != 0.0) {
            segment.percentage = static_cast<float>(segment.distanceToNext / totalDistance_);
            percent += segment.percentage;
        }
    }
}

void PathMoverComponent::FinaliseControlPoints() noexcept {
    // UE FinaliseControlPoints(:550).
    if (UpdatedComponent().IsNull() || pointsFinalized_) {
        return;
    }

    startLocation_ = UpdatedLocation();
    // 로드 값이 SetDuration 하한을 거치지 않았을 수 있다(0으로 나누지 않게 하한을 건다).
    timeMultiplier_ = 1.0f / math::Max(duration_, MinDuration);
    if (controlPoints_.IsEmpty()) {
        return;
    }

    UpdateControlPoints();
    // 대상을 점 0에 맞춘다(UE는 평면 제약 없이 MoveComponent).
    const Vec3d delta = ComputeMoveDelta(0.0f);
    const Vec3d location = UpdatedLocation() + delta;
    const Quatd rotation = UpdatedRotation();
    (void)GetWorld().With<SceneComponent>(UpdatedComponent(), [&](SceneComponent& scene) noexcept {
        return scene.SetWorldLocationAndRotation(location, rotation);
    });
    pointsFinalized_ = true;
}

void PathMoverComponent::OnStart() noexcept {
    MovementComponent::OnStart();
    FinaliseControlPoints();
}

Vec3d PathMoverComponent::ComputeMoveDelta(float time) const noexcept {
    // UE ComputeMoveDelta(:248–295).
    const usize count = segments_.Size();
    usize current = count;
    if (count != 0 && time >= 1.0f) {
        current = count - 1;
    } else {
        float accumulated = 0.0f;
        for (usize index = 0; index < count; ++index) {
            const float next = accumulated + segments_[index].percentage;
            if (time < next) {
                current = index;
                break;
            }

            accumulated = next;
        }
    }

    Vec3d newPosition;
    if (current < count && current < controlPoints_.Size()) {
        const Quatd rotation = UpdatedRotation();
        const Segment& segment = segments_[current];
        const float alpha = (time - segment.startTime) / segment.percentage;
        const usize nextIndex = current + 1 < controlPoints_.Size() ? current + 1 : current;
        const PathControlPoint& from = controlPoints_[current];
        const PathControlPoint& to = controlPoints_[nextIndex];
        const Vec3d begin = rotation.RotateVector(from.position) +
                            (from.positionIsRelative ? startLocation_ : Vec3d());
        const Vec3d end =
            rotation.RotateVector(to.position) + (to.positionIsRelative ? startLocation_ : Vec3d());
        newPosition = begin + (end - begin) * static_cast<double>(alpha);
    }

    const Vec3d currentPosition = UpdatedLocation();
    return currentPosition == newPosition ? Vec3d() : newPosition - currentPosition;
}

float PathMoverComponent::CalculateNewTime(float timeNow, float delta, bool& stopped,
                                           float& timeRemainder) noexcept {
    // UE CalculateNewTime(:190–246).
    timeRemainder = 0.0f;
    stopped = false;
    float newTime = timeNow + delta * timeMultiplier_ * currentDirection_;
    if (newTime >= 1.0f) {
        timeRemainder = newTime - 1.0f;
        if (behaviour_ == PathMoverBehaviour::OneShot) {
            newTime = 1.0f;
            stopped = true;
            OnPathStop(newTime);
        } else if (behaviour_ == PathMoverBehaviour::LoopReset) {
            newTime = 0.0f;
            OnPathReset(newTime);
        } else {
            newTime = 1.0f;
            OnPathReverse(newTime);
            currentDirection_ = -currentDirection_;
        }
    } else if (newTime < 0.0f) {
        timeRemainder = -newTime;
        if (behaviour_ == PathMoverBehaviour::OneShotReverse) {
            newTime = 0.0f;
            stopped = true;
            OnPathStop(newTime);
        } else if (behaviour_ == PathMoverBehaviour::PingPong) {
            newTime = 0.0f;
            OnPathReverse(newTime);
            currentDirection_ = -currentDirection_;
        }
    }

    return newTime;
}

double PathMoverComponent::SimulationTimeStep(double remainingSeconds,
                                              int32 iteration) const noexcept {
    // UE GetSimulationTimeStep(:423) — 하한 MIN_TICK_TIME.
    if (remainingSeconds > maxSimulationTimeStep_ && iteration < maxSimulationIterations_) {
        remainingSeconds = math::Min(maxSimulationTimeStep_, remainingSeconds * 0.5);
    }

    return math::Max(MinTickTime, remainingSeconds);
}

void PathMoverComponent::OnTick(const TickContext& context) noexcept {
    // UE TickComponent(:47–188), 충돌 분기 제외(충돌 시간은 늘 1).
    MovementComponent::OnTick(context);
    if (UpdatedComponent().IsNull() || ShouldSkipUpdate() || stopped_ || controlPoints_.IsEmpty()) {
        return;
    }

    double remaining = context.deltaSeconds;
    if (speedMultiplier_ > 0.0f) {
        remaining *= static_cast<double>(speedMultiplier_);
    }

    int32 bounces = 0;
    int32 iterations = 0;
    while (remaining >= MinTickTime && iterations < maxSimulationIterations_ &&
           !UpdatedComponent().IsNull() && IsEnabled()) {
        ++iterations;
        const double step =
            forceSubStepping_ ? SimulationTimeStep(remaining, iterations) : remaining;
        remaining -= step;
        const float target = math::Clamp(currentTime_ + static_cast<float>(step) * timeMultiplier_ *
                                                            currentDirection_,
                                         0.0f, 1.0f);
        const Vec3d delta = ComputeMoveDelta(target);
        velocity_ = delta * (1.0 / step);
        (void)MoveUpdatedComponent(delta, UpdatedRotation());
        if (UpdatedComponent().IsNull() || !IsEnabled()) {
            return;
        }

        float alphaRemainder = 0.0f;
        currentTime_ =
            CalculateNewTime(currentTime_, static_cast<float>(step), stopped_, alphaRemainder);
        if (stopped_) {
            velocity_ = Vec3d();
            break;
        }

        double subTickRemaining = 0.0;
        if (alphaRemainder != 0.0f) {
            ++bounces;
            subTickRemaining = static_cast<double>(alphaRemainder * duration_);
        }

        if (bounces <= MaxBounces && subTickRemaining >= MinTickTime) {
            remaining += subTickRemaining;
            --iterations;
        }
    }

    UpdateComponentVelocity();
}

} // namespace rocks
