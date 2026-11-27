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
#include <World/Physics/PhysicsContactHit.h>
#include <World/Physics/PhysicsScene.h>

// Jolt.h가 Jolt의 모든 헤더보다 먼저 와야 한다(Core.h의 설정).
// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/Shape/SubShapeID.h>
#include <Jolt/Physics/PhysicsSystem.h>
// clang-format on

namespace rocks {

class WorkerScheduler;

namespace physics {

// Jolt body user data에 칸 번호(위 32 bit)와 세대(아래 32 bit)를 넣는다(장면·질의가 같이 쓴다).
inline constexpr uint32 UserDataIndexShift = 32;

inline JPH::uint64 PackUserData(uint32 index, uint32 generation) noexcept {
    return (static_cast<JPH::uint64>(index) << UserDataIndexShift) | generation;
}

inline uint32 UserDataIndex(JPH::uint64 userData) noexcept {
    return static_cast<uint32>(userData >> UserDataIndexShift);
}

inline uint32 UserDataGeneration(JPH::uint64 userData) noexcept {
    return static_cast<uint32>(userData);
}

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
    void SetBodyNotifyContacts(PhysicsBodyHandle body, bool notify) noexcept override;
    bool ComputePenetration(PhysicsBodyHandle body, const CollisionShape& shape,
                            const Vec3d& position, const Quatd& rotation,
                            MtdResult& out) const noexcept override;
    Result<BodyKinematics> ReadKinematics(PhysicsBodyHandle body) const noexcept override;
    Result<bool> TraceLine(const Vec3d& start, const Vec3d& end, const QueryFilter& filter,
                           const QueryParams& params, TraceMode mode, HitResult* first,
                           Array<HitResult>* all) const noexcept override;
    Result<bool> TraceShape(const Vec3d& start, const Vec3d& end, const Quatd& rotation,
                            const CollisionShape& shape, const QueryFilter& filter,
                            const QueryParams& params, TraceMode mode, HitResult* first,
                            Array<HitResult>* all) const noexcept override;
    Result<bool> OverlapShape(const Vec3d& position, const Quatd& rotation,
                              const CollisionShape& shape, const QueryFilter& filter,
                              const QueryParams& params, OverlapMode mode,
                              Array<OverlapResult>* all) const noexcept override;
    Result<bool> TraceBody(PhysicsBodyHandle body, const Vec3d& start, const Vec3d& end,
                           const Quatd& rotation, const QueryFilter& filter,
                           const QueryParams& params,
                           Array<HitResult>& all) const noexcept override;
    Result<bool> OverlapBody(PhysicsBodyHandle body, const Vec3d& position, const Quatd& rotation,
                             const QueryFilter& filter, const QueryParams& params,
                             Array<OverlapResult>& all) const noexcept override;

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

    // 지난 꺼냄 뒤 step들이 쌓은 시뮬레이션 접촉 hit를 스텝 순서대로 꺼낸다(7.1.12절 —
    // PhysicsResults가 부른다). 돌려준 Span은 다음 호출까지 유효하다(발행 중 hook이 장면을 바꿔도
    // 그대로다).
    Span<const PhysicsContactHit> TakeContactHits() noexcept;

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
        // 컴포넌트의 소유 Actor다(질의 hit·무시 목록 — 7.1.7절).
        ObjectHandle actor;
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
        // 시뮬레이션 접촉 hit를 모으는지다(BodySettings::notifyRigidBodyCollision — 7.1.12절).
        // step 동안 접촉 listener가 worker에서 읽는다(쓰기는 step 밖).
        bool notifyContacts = false;
        // hit 문턱(7.1.12절)이 쓰는 step 시작 선속도(Update 전 — 중력 적분 전, UE PreV)와 그
        // 값을 찍은 step 표지(steps_ + 1 — 다르면 step 시작에 잠들어 있어 0이다)다.
        JPH::Vec3 stepStartVelocity = JPH::Vec3::sZero();
        uint64 stepStartStamp = 0;
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

    // step 안에서 모은 접촉 하나다(7.1.12절 — Jolt 순서: body 1 ID < body 2 ID). 위치는 Jolt 좌표,
    // 법선은 body 1에서 body 2 쪽, 추정 충격량은 body 2가 받는 값이다(LinearCast body가 낀 접촉만
    // 계산한다).
    struct ContactNotice {
        JPH::uint64 userData1 = 0;
        JPH::uint64 userData2 = 0;
        JPH::SubShapeID subShape1;
        JPH::SubShapeID subShape2;
        JPH::RVec3 point = JPH::RVec3::sZero();
        JPH::Vec3 normal = JPH::Vec3::sZero();
        float depth = 0.0f;
        bool probe = false;
        JPH::Vec3 estimatedImpulse = JPH::Vec3::sZero();
    };

    // 접촉 listener다(6.4절·7.1.12절). probe 접촉의 반작용을 끄고, 알림 표지가 있는 body가 낀
    // 접촉을 미리 잡은 배열에 복사한다. Jolt가 step 중 여러 worker에서 동시에 부르므로 할당하지
    // 않는다(용량이 모자라면 센다). 게임 코드를 부르지 않는다.
    class ContactCollector final : public JPH::ContactListener {
    public:
        ContactCollector(const JoltPhysicsScene& scene, Allocator& allocator) noexcept
            : scene_(scene), notices_(allocator) {
        }

        void OnContactAdded(const JPH::Body& body1, const JPH::Body& body2,
                            const JPH::ContactManifold& manifold,
                            JPH::ContactSettings& settings) override;
        void OnContactPersisted(const JPH::Body& body1, const JPH::Body& body2,
                                const JPH::ContactManifold& manifold,
                                JPH::ContactSettings& settings) override;

        // 용량을 capacity 이상으로 잡는다(게임 thread, step 밖). 오류: 할당 실패.
        Result<void> Reserve(usize capacity) noexcept;

        // 모은 접촉이다(게임 thread, step 밖에서만 읽고 비운다).
        Array<ContactNotice>& Notices() noexcept {
            return notices_;
        }

        // 지난 꺼냄 뒤 용량이 모자라 놓친 수를 돌려주고 0으로 둔다(step 밖).
        uint64 TakeDropped() noexcept;

    private:
        void Record(const JPH::Body& body1, const JPH::Body& body2,
                    const JPH::ContactManifold& manifold, JPH::ContactSettings& settings) noexcept;

        const JoltPhysicsScene& scene_;
        platform::Mutex mutex_;
        Array<ContactNotice> notices_;
        uint64 dropped_ = 0;
    };

    const BodySlot* FindSlot(PhysicsBodyHandle body) const noexcept;

    // body user data의 칸이 살아 있고 접촉 hit를 모으는지다(listener — step 중 읽기만).
    bool WantsContacts(JPH::uint64 userData) const noexcept;

    // 질의 수집기가 모은 hit 하나다(7.1.7·7.1.8절). stops는 자르기 기준(시작 겹침이 아닌 막음 —
    // ObjectTypes All은 거짓), reportBlocking은 결과의 blockingHit이다. order는 First의 순서
    // key(Jolt early-out fraction — 시작 겹침은 −깊이)다. shape hit는 접촉점·penetration
    // axis·깊이를 더 쓴다.
    struct QueryHitRecord {
        float fraction = 0.0f;
        float order = 0.0f;
        JPH::BodyID body;
        JPH::SubShapeID subShape;
        uint32 slot = 0;
        bool stops = false;
        bool reportBlocking = false;
        bool initial = false;
        JPH::RVec3 contact = JPH::RVec3::sZero();
        JPH::Vec3 axis = JPH::Vec3::sZero();
        float depth = 0.0f;
    };

    // 질의 Jolt 필터·수집기다(JoltPhysicsSceneQueries.cpp — 칸을 읽는다).
    class QueryBodyFilter;
    class RayHitCollector;
    template <class CollectorBase>
    class ShapeHitCollector;
    class OverlapCollector;

    // 질의에서 더 뺄 body다(컴포넌트 질의 — 7.1.9절). null이면 빼지 않는다.
    struct QueryExclusion {
        ObjectHandle actor;
        ObjectHandle component;
    };

    // Jolt 형상(Jolt 좌표·scale이 구워진 형상)으로 하는 shape trace다(TraceShape·TraceBody의 공통
    // 경로 — 7.1.8절). queryKind는 impactNormal 규칙에 쓴다. rotation은 단위 quaternion이다.
    Result<bool> TraceJoltShape(const JPH::Shape& shape, CollisionShapeKind queryKind,
                                const Vec3d& start, const Vec3d& end, const Quatd& rotation,
                                const QueryFilter& filter, const QueryParams& params,
                                TraceMode mode, HitResult* first, Array<HitResult>* all,
                                const QueryExclusion& exclusion) const noexcept;

    // Jolt 형상으로 하는 overlap이다(OverlapShape·OverlapBody의 공통 경로 — 7.1.9절).
    Result<bool> OverlapJoltShape(const JPH::Shape& shape, const Vec3d& position,
                                  const Quatd& rotation, const QueryFilter& filter,
                                  const QueryParams& params, OverlapMode mode,
                                  Array<OverlapResult>* all,
                                  const QueryExclusion& exclusion) const noexcept;

    // 모은 hit 하나를 HitResult로 바꾼다(UE ConvertQueryImpactHit — line, shape는 시작 겹침의
    // ConvertOverlappedShapeToImpactHit 포함).
    HitResult MakeLineHit(const Vec3d& start, const Vec3d& end,
                          const QueryHitRecord& record) const noexcept;
    HitResult MakeShapeHit(const Vec3d& start, const Vec3d& end, CollisionShapeKind queryKind,
                           const QueryHitRecord& record) const noexcept;

    // All이 모은 queryHits_를 자르고 정렬해 all 끝에 덧붙이고 비운다(4.5절). queryKind가 Line이면
    // line hit로 바꾼다. 반환 값은 보고된 막는 hit가 있는지다. 오류: all 할당 실패.
    Result<bool> FlushAllHits(const Vec3d& start, const Vec3d& end, CollisionShapeKind queryKind,
                              Array<HitResult>& all) const noexcept;

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

    // step 직후 이번 step의 접촉을 정렬·합쳐 hit 목록에 더한다(7.1.12절).
    void CollectContactHits() noexcept;

    // 같은 body 쌍의 접촉 묶음 하나를 hit로 바꾼다(알림 표지가 있는 쪽마다 하나).
    void EmitContactPair(Span<const ContactNotice> group) noexcept;

    // hit 문턱이 있으면 step 직전 활성 Dynamic body의 선속도를 칸에 찍는다(7.1.12절).
    void RecordStepStartVelocities() noexcept;

    // 칸 body의 이번 step 선속도 변화가 성분마다 문턱 이하인지다(Dynamic만 — 그 밖은 0).
    bool IsVelocityChangeUnderThreshold(const BodySlot& slot) const noexcept;

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
    ContactCollector contacts_;
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
    // 쌓는 접촉 hit 목록과 꺼내 준 목록이다(TakeContactHits가 두 버퍼를 바꿔 쥔다 — 용량 재사용).
    Array<PhysicsContactHit> contactHits_;
    Array<PhysicsContactHit> takenContactHits_;
    // 지난 step이 모은 접촉 수(놓친 수 포함)다 — 다음 step 전 listener 용량을 정한다.
    usize lastContactNotices_ = 0;
    // 알림 표지가 켜진 살아 있는 칸 수다(listener 용량 하한 — 7.1.12절).
    usize notifyBodies_ = 0;
    // 이번 step에 할당 실패로 저장하지 못한 hit 수다(step마다 Warning 한 번).
    uint64 lostContactHits_ = 0;
    // 접촉 hit 문턱(cm/s — PhysicsSettings::minDeltaVelocityForHitEvents)이다.
    double minDeltaVelocityForHitEvents_ = 0.0;
    // 질의 All이 모으는 임시 목록이다(재사용 — 질의는 게임 thread에서 하나씩).
    mutable Array<QueryHitRecord> queryHits_;
    bool simulates_ = false;
    // Step의 Update·Kinematic 목표 적용 중이다(AssertNotStepping).
    bool stepping_ = false;
};

} // namespace physics

} // namespace rocks
