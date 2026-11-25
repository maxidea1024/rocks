// PhysicsScene의 Jolt 구현(Docs/Design/PhysicsModule 2장·3.1절, 7.1.1절).
#include "JoltPhysicsScene.h"

#include "JoltConversions.h"

#include "PhysicsLog.h"

#include <Core/Math/MathUtility.h>
#include <Platform/FloatingPointEnvironment.h>
#include <Platform/Fatal.h>

namespace rocks::physics {

namespace {

// 한 step의 collision step 수다(3.1절 — 고정 스텝마다 1).
constexpr int CollisionStepsPerStep = 1;

// Jolt body 잠금 수다(0 = Jolt 기본값).
constexpr JPH::uint BodyMutexCount = 0;

} // namespace

JoltPhysicsScene::JoltPhysicsScene(JoltRuntimeLease&& runtime, const PhysicsSettings& settings,
                                   WorkerScheduler* scheduler) noexcept
    : runtime_(Move(runtime)), temp_(settings.tempAllocatorBytes),
      jobs_(scheduler, JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers) {
    system_.Init(settings.maxBodies, BodyMutexCount, settings.maxBodyPairs,
                 settings.maxContactConstraints, broadPhaseLayers_, objectVsBroadPhase_,
                 layerPairs_);
    system_.SetGravity(
        ToJoltAcceleration(Vec3d(0.0, 0.0, static_cast<double>(settings.defaultGravityZ))));
}

JoltPhysicsScene::~JoltPhysicsScene() {
    // Jolt WaitForJobs가 끝난 뒤에도 Task가 job 참조를 놓는 중일 수 있다(6.7절).
    jobs_.WaitForOutstandingTasks();
}

Result<UniquePtr<JoltPhysicsScene>> JoltPhysicsScene::Create(Allocator& allocator,
                                                             const PhysicsSettings& settings,
                                                             WorkerScheduler* scheduler) noexcept {
    if (auto valid = PhysicsSettings::Validate(settings); !valid) {
        return Result<UniquePtr<JoltPhysicsScene>>::Failure(valid.Error());
    }

    auto runtime = JoltRuntimeLease::Acquire();
    if (!runtime) {
        return Result<UniquePtr<JoltPhysicsScene>>::Failure(runtime.Error());
    }

    return UniquePtr<JoltPhysicsScene>::TryMake(allocator, Move(runtime.Value()), settings,
                                                scheduler);
}

PhysicsSceneStats JoltPhysicsScene::Stats() const noexcept {
    PhysicsSceneStats stats;
    stats.bodies = system_.GetNumBodies();
    stats.activeBodies = system_.GetNumActiveBodies(JPH::EBodyType::RigidBody);
    stats.steps = steps_;
    return stats;
}

Result<void> JoltPhysicsScene::Step(float seconds) noexcept {
    if (!(seconds > 0.0f) || !math::IsFinite(seconds)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // Jolt는 모든 실행 thread의 FP 상태가 같아야 한다(어댑터 경계 10.4절). worker job과 같은 결정적
    // 환경에서 step한다(barrier를 기다리는 이 thread도 job을 실행한다).
    struct StepCall {
        JoltPhysicsScene* scene;
        float seconds;
        JPH::EPhysicsUpdateError error;
    };

    StepCall call{this, seconds, JPH::EPhysicsUpdateError::None};
    if (platform::RunWithDeterministicFloatingPoint(
            [](void* context) noexcept {
                auto& step = *static_cast<StepCall*>(context);
                step.error = step.scene->system_.Update(step.seconds, CollisionStepsPerStep,
                                                        &step.scene->temp_, &step.scene->jobs_);
            },
            &call)) {
        ++floatingPointCorrections_;
    }

    const JPH::EPhysicsUpdateError error = call.error;
    // step이 끝난 뒤에도 Task·미룬 목록이 job 참조를 쥐고 있을 수 있다. step마다 놓아 다음 step이
    // 쓸 job 칸이 언제나 남게 한다(타이밍과 무관한 상한 — 7.1.1절).
    jobs_.WaitForOutstandingTasks();
    ++steps_;
    if (error != JPH::EPhysicsUpdateError::None) {
        Log::Warning(LogPhysics,
                     "Physics step {Step} exceeded a Jolt capacity (error bits {Bits}); some body "
                     "pairs or contacts were ignored",
                     steps_, static_cast<uint32>(error));
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    return Result<void>::Success();
}

} // namespace rocks::physics
