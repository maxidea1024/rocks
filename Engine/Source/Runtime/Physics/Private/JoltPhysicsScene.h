#pragma once

#include "JoltLayers.h"
#include "JoltResponseSets.h"
#include "JoltRuntime.h"
#include "TaskJobSystem.h"

#include <Core/Containers/Array.h>
#include <Core/Memory/UniquePtr.h>
#include <Object/ObjectHandle.h>
#include <Physics/PhysicsSettings.h>
#include <Platform/Sync.h>
#include <World/Physics/PhysicsScene.h>

// Jolt.h가 Jolt의 모든 헤더보다 먼저 와야 한다(Core.h의 설정).
// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/PhysicsSystem.h>
// clang-format on

namespace rocks {

class WorkerScheduler;

namespace physics {

// 결과 반영 한 건이다(3.4절): body를 가진 컴포넌트와 Jolt가 계산한 월드 위치(cm)·회전이다.
struct MovedBody {
    ObjectHandle component;
    Vec3d location;
    Quatd rotation;
};

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
    Result<PhysicsBodyHandle> CreateBody(const BodyCreateDesc& desc) noexcept override;
    void DestroyBody(PhysicsBodyHandle body) noexcept override;
    void SetBodyTransform(PhysicsBodyHandle body, const Transform& worldTransform,
                          TeleportMode mode) noexcept override;
    void SetBodyMotion(PhysicsBodyHandle body, BodyMotion motion, bool wake) noexcept override;
    void SetBodyCollision(PhysicsBodyHandle body,
                          const ResolvedCollision& collision) noexcept override;
    Result<void> SetBodyGeometry(PhysicsBodyHandle body,
                                 const BodyGeometry& geometry) noexcept override;
    Result<BodyKinematics> ReadKinematics(PhysicsBodyHandle body) const noexcept override;

    // 한 step(seconds초, collision step 1)을 진행한다. 오류: seconds가 양의 유한값이 아님
    // (InvalidArgument), Jolt 용량 초과(CapacityExceeded — step은 진행했고 경고 로그를 남긴다).
    Result<void> Step(float seconds) noexcept;

    JPH::PhysicsSystem& System() noexcept {
        return system_;
    }

    TaskJobSystem& Jobs() noexcept {
        return jobs_;
    }

    // 이 장면이 step하는지다(Game·Tryout — PhysicsWorldSubsystem이 정한다). step하지 않는 장면의
    // Kinematic 이동은 언제나 teleport다(PMD16).
    void SetSimulates(bool simulates) noexcept {
        simulates_ = simulates;
    }

    // 지난 꺼냄 뒤 step에서 움직였거나 잠든 Dynamic body를 칸 번호 순으로 꺼낸다(3.4절 —
    // PhysicsResults가 부른다). 돌려준 Span은 다음 호출까지 유효하다. 오류: 할당 실패(목록을 그대로
    // 두어 다음 호출이 다시 꺼낸다).
    Result<Span<const MovedBody>> TakeMovedBodies() noexcept;

    // step 전에 게임 thread의 FP 환경을 결정적 환경으로 바꿔야 했던 횟수다(누적 — 진단).
    uint64 FloatingPointCorrections() const noexcept {
        return floatingPointCorrections_;
    }

    // UniquePtr::TryMake가 부르는 생성자다(Create만 쓴다).
    JoltPhysicsScene(Allocator& allocator, JoltRuntimeLease&& runtime,
                     const PhysicsSettings& settings, WorkerScheduler* scheduler) noexcept;

private:
    // body 칸이다. 세대 0은 쓰지 않는다(PhysicsBodyHandle의 "없음").
    struct BodySlot {
        JPH::BodyID body;
        ObjectHandle component;
        uint32 generation = 1;
        BodyMotion motion = BodyMotion::Static;
        bool live = false;
        // 응답 묶음 표 색인이다(object 층 bit 11–31 — 7.1.6절).
        uint32 responseSet = 0;
        // 형상을 다시 만들 때 쓰는 값이다(7.1.5절): scale 전 형상, 지금 형상에 구운 월드 scale,
        // 형상 교체 때 질량을 다시 계산할지(BodySettings::updateMassWhenScaleChanges).
        BodyGeometry geometry;
        Vec3d scale = Vec3d::One();
        bool updateMassOnShapeChange = true;
        // 결과 목록에 이미 있는지다(중복 방지).
        bool pendingResult = false;
        // kinematicTargets_에 이미 있는지다(중복 방지 — hasTarget과 따로: teleport가 목표를 지워도
        // 목록에는 남는다).
        bool queuedTarget = false;
        // Kinematic 대기 목표(3.3절)다: hasTarget이면 다음 step 직전에
        // startPosition·startRotation으로 되돌린 뒤 targetPosition·targetRotation으로
        // MoveKinematic한다.
        bool hasTarget = false;
        // 지난 step에 MoveKinematic했는지다(목표 없는 다음 step에서 속도를 0으로 둔다).
        bool movedLastStep = false;
        JPH::RVec3 startPosition = JPH::RVec3::sZero();
        JPH::Quat startRotation = JPH::Quat::sIdentity();
        JPH::RVec3 targetPosition = JPH::RVec3::sZero();
        JPH::Quat targetRotation = JPH::Quat::sIdentity();
    };

    // 잠든 body를 모은다. Jolt가 활성 목록 잠금 아래(job thread 또는 RemoveBody를 부른 게임
    // thread)에서 부르므로 할당하지 않는다: 용량은 CreateBody가 Reserve로 미리 잡고, 넘치면 센다.
    class ActivationListener final : public JPH::BodyActivationListener {
    public:
        explicit ActivationListener(Allocator& allocator) noexcept : deactivated_(allocator) {
        }

        void OnBodyActivated(const JPH::BodyID&, JPH::uint64) override {
        }

        void OnBodyDeactivated(const JPH::BodyID&, JPH::uint64 userData) override;

        // 용량을 capacity 이상으로 잡는다(게임 thread, step 밖). 오류: 할당 실패.
        Result<void> Reserve(usize capacity) noexcept;

        // 쌓인 user data를 out 끝에 옮기고 비운다(step 밖). 돌려주는 값은 용량이 모자라 놓친 수다.
        uint64 TakeInto(Array<JPH::uint64>& out) noexcept;

    private:
        platform::Mutex mutex_;
        Array<JPH::uint64> deactivated_;
        uint64 dropped_ = 0;
    };

    const BodySlot* FindSlot(PhysicsBodyHandle body) const noexcept;

    // 접점 쓰기가 step 밖인지 확인한다(3.2절 — step 안의 job thread가 응답 묶음 표·body를 읽는다,
    // 7.1.6절). step 안에서 부르면 Fatal이다.
    void AssertNotStepping() const noexcept;

    // 형상 값·scale로 형상을 다시 만들어 바꾸고 깨운다(7.1.5절). 실패하면 이전 형상을 둔다.
    // 오류: 형상 없음·비유한 값(InvalidArgument).
    Result<void> ReplaceShape(BodySlot& slot, const BodyGeometry& geometry,
                              const Vec3d& scale) noexcept;

    // body user data(칸 번호·세대)의 칸을 결과 목록에 한 번만 넣는다(살아 있는 Dynamic만).
    void MarkMoved(JPH::uint64 userData) noexcept;

    // step 직전 Kinematic 목표 적용과 목표 없는 Kinematic 정지(3.3절)다.
    void ApplyKinematicTargets(float seconds) noexcept;

    // step 직후 활성·잠든 Dynamic body를 결과 목록에 더한다(3.4절).
    void CollectMovedBodies() noexcept;

    // 멤버 순서가 수명 순서다: Jolt 공유 자원 참조가 가장 먼저 생기고 가장 늦게 사라진다.
    JoltRuntimeLease runtime_;
    BroadPhaseLayers broadPhaseLayers_;
    ObjectVsBroadPhaseFilter objectVsBroadPhase_;
    // 쌍 필터가 빌리므로 그보다 먼저 생기고 늦게 사라진다.
    ResponseSetTable responseSets_;
    ObjectLayerPairFilter layerPairs_;
    JPH::TempAllocatorImpl temp_;
    TaskJobSystem jobs_;
    // PhysicsSystem이 가리키므로 그보다 먼저 생기고 늦게 사라진다.
    ActivationListener activations_;
    JPH::PhysicsSystem system_;
    uint64 steps_ = 0;
    uint64 floatingPointCorrections_ = 0;
    // 지역 원점(cm — 큰 월드 PhysicsRegion은 계획 16.5절 PC07 뒤, 지금은 월드 원점).
    Vec3d origin_ = Vec3d::Zero();
    Array<BodySlot> slots_;
    Array<uint32> freeSlots_;
    // 결과 목록(handle — 칸 재사용을 세대로 거른다)·Kinematic 목표가 있는 칸·지난 step에 움직인
    // Kinematic 칸이다.
    Array<PhysicsBodyHandle> pendingResults_;
    Array<uint32> kinematicTargets_;
    Array<uint32> kinematicMoving_;
    // 재사용하는 임시 목록이다: step 직후 활성 body(Jolt 배열 — Jolt 할당 hook)·잠든 body user
    // data·꺼낸 결과.
    JPH::BodyIDVector activeBodies_;
    Array<JPH::uint64> slept_;
    Array<MovedBody> moved_;
    bool simulates_ = false;
    // Step의 Update·Kinematic 목표 적용 중이다(AssertNotStepping).
    bool stepping_ = false;
};

} // namespace physics

} // namespace rocks
