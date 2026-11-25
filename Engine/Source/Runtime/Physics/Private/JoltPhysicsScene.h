#pragma once

#include "JoltLayers.h"
#include "JoltRuntime.h"
#include "TaskJobSystem.h"

#include <Core/Memory/UniquePtr.h>
#include <Physics/PhysicsSettings.h>
#include <World/Physics/PhysicsScene.h>

// Jolt.h가 Jolt의 모든 헤더보다 먼저 와야 한다(Core.h의 설정).
// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/PhysicsSystem.h>
// clang-format on

namespace rocks {

class WorkerScheduler;

namespace physics {

// PhysicsScene의 Jolt 구현이다(Docs/Design/PhysicsModule 2장, 7.1.1절). Jolt PhysicsSystem·임시
// 할당기·job 시스템·층 판정 객체를 소유하고, 만드는 동안 Jolt 공유 자원 참조(JoltRuntimeLease)를
// 쥔다. 파괴는 남은 Task가 끝나기를 기다린 뒤 Jolt 객체를 정리하고 마지막에 참조를 놓는다.
//
// 스레드: World의 게임 스레드 전용이다(Step 동안 Jolt가 job을 scheduler worker에서 돌린다).
class JoltPhysicsScene final : public PhysicsScene {
public:
    // 설정으로 장면을 만든다. scheduler는 빌린다(nullptr이면 job을 제출 thread에서 실행).
    // 오류: 설정 검증 실패·Jolt 버전 불일치(InvalidArgument), 할당 실패(OutOfMemory).
    static Result<UniquePtr<JoltPhysicsScene>> Create(Allocator& allocator,
                                                      const PhysicsSettings& settings,
                                                      WorkerScheduler* scheduler) noexcept;

    ~JoltPhysicsScene() override;

    PhysicsSceneStats Stats() const noexcept override;

    // 한 step(seconds초, collision step 1)을 진행한다. 오류: seconds가 양의 유한값이 아님
    // (InvalidArgument), Jolt 용량 초과(CapacityExceeded — step은 진행했고 경고 로그를 남긴다).
    Result<void> Step(float seconds) noexcept;

    JPH::PhysicsSystem& System() noexcept {
        return system_;
    }

    TaskJobSystem& Jobs() noexcept {
        return jobs_;
    }

    // step 전에 게임 thread의 FP 환경을 결정적 환경으로 바꿔야 했던 횟수다(누적 — 진단).
    uint64 FloatingPointCorrections() const noexcept {
        return floatingPointCorrections_;
    }

    // UniquePtr::TryMake가 부르는 생성자다(Create만 쓴다).
    JoltPhysicsScene(JoltRuntimeLease&& runtime, const PhysicsSettings& settings,
                     WorkerScheduler* scheduler) noexcept;

private:
    // 멤버 순서가 수명 순서다: Jolt 공유 자원 참조가 가장 먼저 생기고 가장 늦게 사라진다.
    JoltRuntimeLease runtime_;
    BroadPhaseLayers broadPhaseLayers_;
    ObjectVsBroadPhaseFilter objectVsBroadPhase_;
    ObjectLayerPairFilter layerPairs_;
    JPH::TempAllocatorImpl temp_;
    TaskJobSystem jobs_;
    JPH::PhysicsSystem system_;
    uint64 steps_ = 0;
    uint64 floatingPointCorrections_ = 0;
};

} // namespace physics

} // namespace rocks
