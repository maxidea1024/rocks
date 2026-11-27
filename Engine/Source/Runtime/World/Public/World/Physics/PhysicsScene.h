#pragma once

#include <Base/Types.h>
#include <Collision/CollisionShape.h>
#include <Core/Containers/Array.h>
#include <Core/Math/Transform.h>
#include <Core/Result.h>
#include <World/Physics/BodyCreateDesc.h>
#include <World/Physics/BodyForce.h>
#include <World/Physics/BodyKinematics.h>
#include <World/Physics/HitResult.h>
#include <World/Physics/MtdResult.h>
#include <World/Physics/OverlapMode.h>
#include <World/Physics/OverlapResult.h>
#include <World/Physics/PhysicsBodyHandle.h>
#include <World/Physics/QueryFilter.h>
#include <World/Physics/QueryParams.h>
#include <World/Physics/TeleportMode.h>
#include <World/Physics/TraceMode.h>

namespace rocks {

// 물리 장면의 진단 수치다(PhysicsScene::Stats). 값은 호출 시점 스냅샷이다.
struct PhysicsSceneStats {
    // 장면에 있는 body 수와 그중 깨어 있는 body 수다.
    uint32 bodies = 0;
    uint32 activeBodies = 0;
    // 장면을 만든 뒤 진행한 step 수다.
    uint64 steps = 0;
    // 쓰는 충돌 응답 묶음 수다(같은 응답의 body가 공유 — 늘 있는 모두 Block 묶음 포함,
    // PhysicsModule 7.1.6절).
    uint32 responseSets = 0;
};

// World가 부르는 물리 장면 접점이다(Docs/Design/PhysicsModule 2.2절 — UE UWorld::GetPhysicsScene이
// 돌려주는 자리). World 모듈은 물리 엔진을 모르므로 이 추상 클래스만 알고, 구현은 Physics 모듈의
// Jolt 장면이다. 접점은 Physics 모듈의 PhysicsWorldSubsystem이 World 초기화 때 꽂고(World::
// SetPhysicsScene) 정리 때 뗀다. 장면이 없는 World(물리 없이 만든 시험·도구 World)에서
// World::GetPhysicsScene()은 nullptr이다.
//
// 함수는 구현하는 단계마다 더한다(PH1 Stats, PH2b body 수명·이동·운동 읽기, PH2d 운동 종류·필터·
// 형상 갱신, PH3b line trace — PhysicsModule 7.1.1·7.1.3·7.1.5·7.1.7절 PMD22). 질의는 World의
// WorldCollision.h 함수가 부른다. 공개 인자는 Rocks 단위(cm·kg·초)이고 결과는
// 값, 실패는 Result다.
//
// 사용 예:
//   if (PhysicsScene* scene = world.GetPhysicsScene()) {
//       PhysicsSceneStats stats = scene->Stats();
//   }
//
// 스레드: World의 게임 스레드에서만 부른다(PhysicsModule 3.2절).
class PhysicsScene {
public:
    virtual ~PhysicsScene() = default;

    // 장면의 body·step 수다.
    virtual PhysicsSceneStats Stats() const noexcept = 0;

    // body를 만들어 장면에 넣는다(2.3절 — PrimitiveComponent::CreateSystemState(Physics)가 부른다).
    // 오류: 형상 없음·잘못된 값(InvalidArgument), 장면 body 용량 초과(CapacityExceeded), 할당 실패.
    virtual Result<PhysicsBodyHandle> CreateBody(const BodyCreateDesc& desc) noexcept = 0;

    // body를 장면에서 빼고 지운다. 없는 handle·옛 세대는 무시한다.
    virtual void DestroyBody(PhysicsBodyHandle body) noexcept = 0;

    // body를 월드 transform(cm)으로 옮긴다(3.3절). scale이 body의 지금 scale과 다르면 먼저 형상을
    // 새 scale로 다시 만든다(SetBodyGeometry와 같은 규칙 — 7.1.5절). 없는 handle·옛 세대·잘못된
    // transform은 무시한다.
    virtual void SetBodyTransform(PhysicsBodyHandle body, const Transform& worldTransform,
                                  TeleportMode mode) noexcept = 0;

    // 운동 종류를 Kinematic ↔ Dynamic으로 바꾼다(2.3.1절 simulatePhysics — 다시 만들지 않는다).
    // Kinematic이 되면 속도가 0이고(UE SetIsKinematic), Dynamic이 되면 wake일 때 깨운다(UE
    // startAwake). Static이 끼는 전환·같은 종류·없는 handle·옛 세대는 무시한다(Static ↔ Movable은
    // Mobility 변경이라 body를 다시 만든다).
    virtual void SetBodyMotion(PhysicsBodyHandle body, BodyMotion motion, bool wake) noexcept = 0;

    // 실효 충돌 값(object type·충돌 켬)으로 body 필터를 바꾼다(2.3.1절 — body·속도는 그대로).
    // NoCollision 경계를 넘는 변경은 body를 만들거나 지우는 쪽(컴포넌트)이 다룬다. 없는 handle·옛
    // 세대는 무시한다.
    virtual void SetBodyCollision(PhysicsBodyHandle body,
                                  const ResolvedCollision& collision) noexcept = 0;

    // 형상 값(scale 전 cm)을 바꾼다(2.3.1절 — body의 지금 scale로 형상을 다시 만들고 깨운다).
    // 질량은 body를 만들 때의 BodySettings::updateMassWhenScaleChanges가 참이면 새 형상으로 다시
    // 계산하고, 거짓이면 그대로 둔다(UE UpdateBodyScale). 오류: 없는 handle·옛 세대(StaleHandle),
    // 형상 없음·비유한 값(InvalidArgument — 이전 형상을 둔다).
    virtual Result<void> SetBodyGeometry(PhysicsBodyHandle body,
                                         const BodyGeometry& geometry) noexcept = 0;

    // 시뮬레이션 접촉 hit를 모을지 바꾼다(BodySettings::notifyRigidBodyCollision — 7.1.12절).
    // 다음 step부터 적용된다. 없는 handle·옛 세대는 무시한다.
    virtual void SetBodyNotifyContacts(PhysicsBodyHandle body, bool notify) noexcept = 0;

    // rotation으로 position에 놓은 shape가 body와 겹치면 참이고 out에 shape를 밀어낼 방향·거리를
    // 쓴다(UE FBodyInstance::OverlapTest의 MTD — 7.1.13절). 닿기만 해도 겹침이다(거리 0). 없는
    // handle·옛 세대, Line·Cylinder(CY2 전)·비유한 값·0 회전이면 거짓이다(out은 그대로).
    virtual bool ComputePenetration(PhysicsBodyHandle body, const CollisionShape& shape,
                                    const Vec3d& position, const Quatd& rotation,
                                    MtdResult& out) const noexcept = 0;

    // body의 현재 운동 상태다. 오류: 없는 handle·옛 세대(StaleHandle).
    virtual Result<BodyKinematics> ReadKinematics(PhysicsBodyHandle body) const noexcept = 0;

    // body에 힘·충격량·토크를 준다(PhysicsModule 2.6절·7.1.17절 — UE FBodyInstance::Add…). Dynamic
    // body에만 효력이 있고(Static·Kinematic은 무시) 효력이 있으면 깨운다. 충격량은 다음 step에
    // 속도에 더해지고(바로 읽으면 옛 속도 — UE), 힘·토크는 다음 step 동안(everyFixedStep이면 이번
    // 프레임의 모든 고정 스텝 동안) 작용한다. 없는 handle·옛 세대는 무시하고, 비유한 값(Jolt
    // float로 바꾼 뒤 포함)은 무시하고 Warning NonFiniteBodyInput이다. 사용 예는 BodyForce.h다.
    virtual void ApplyBodyForce(PhysicsBodyHandle body, const BodyForce& force) noexcept = 0;

    // Dynamic body의 선속도(cm/s)·각속도(rad/s, 월드 축)를 바꾼다(UE SetLinearVelocity·
    // SetAngularVelocityInRadians — 7.1.17절). addToCurrent면 지금 속도에 더한다. 최종 속도는
    // 허용 자유도로 거르고 상한으로 자른다. 잠든 body는 최종 속도가 거의 0이 아니면 깨운다.
    // Static·Kinematic(UE와 다르다 — 7.1.17절)·없는 handle·옛 세대는 무시하고, 비유한 값은 무시하고
    // Warning이다. 상한을 넘는 값은 바로 잘린다(UE는 다음 적분 때 자른다).
    //
    // 사용 예:
    //   scene.SetBodyLinearVelocity(body, Vec3d(0.0, 0.0, 300.0), false);   // 위로 3 m/s
    //   scene.SetBodyAngularVelocity(body, Vec3d(0.0, 0.0, 1.0), true);     // Z 둘레 1 rad/s 더함
    virtual void SetBodyLinearVelocity(PhysicsBodyHandle body, const Vec3d& velocity,
                                       bool addToCurrent) noexcept = 0;
    virtual void SetBodyAngularVelocity(PhysicsBodyHandle body, const Vec3d& velocityRadians,
                                        bool addToCurrent) noexcept = 0;

    // start → end line trace다(4.4·4.5절, 7.1.7절). 끝점은 유한하고 길이가 0이 아니어야
    // 한다(World가 거른다). filter.kind는 Channel·ObjectTypes다(Profile은 World가 풀어 넘긴다).
    // mode가 Test면 결과를 쓰지 않고, First면 가장 가까운 막는 hit를 first에, All이면 all
    // 끝에 4.5절 순서로 덧붙인다. 반환 값은 막는 hit가 있는지다(ObjectTypes All은 hit가 하나라도
    // 있는지). 오류: Profile 필터·mode에 맞는 결과 포인터 없음(InvalidArgument), all 할당 실패.
    virtual Result<bool> TraceLine(const Vec3d& start, const Vec3d& end, const QueryFilter& filter,
                                   const QueryParams& params, TraceMode mode, HitResult* first,
                                   Array<HitResult>* all) const noexcept = 0;

    // rotation으로 놓은 shape를 start → end로 쓸고 가는 trace다(4.4·4.5절, 7.1.8절 — 시작 겹침 규칙
    // 포함). 값은 유한하고 shape는 Line이 아니며 거의 0이 아니어야 한다(World가 line trace로
    // 바꾼다). rotation은 정규화해 쓴다. 길이 0이면 시작 겹침만 찾는다. 결과 포인터·반환 값은
    // TraceLine과 같다(All의 반환 값은 보고된 막는 hit가 있는지). 오류: TraceLine의 오류, 0·비유한
    // rotation(InvalidArgument), Cylinder(CY2 전 — Unsupported).
    virtual Result<bool> TraceShape(const Vec3d& start, const Vec3d& end, const Quatd& rotation,
                                    const CollisionShape& shape, const QueryFilter& filter,
                                    const QueryParams& params, TraceMode mode, HitResult* first,
                                    Array<HitResult>* all) const noexcept = 0;

    // rotation으로 position에 놓은 shape의 overlap이다(4.4절, 7.1.9절). 값은 유한하고 shape는
    // Line이 아니어야 한다. mode가 All이면 all 끝에 겹친 컴포넌트를 body handle 순으로
    // 덧붙인다(같은 컴포넌트는 하나). 반환 값은 Blocking이면 막는 겹침, Any면 겹침 하나라도,
    // All이면 막는 겹침이 있는지다. 오류: Profile 필터·All에 all 없음·0 rotation(InvalidArgument),
    // Cylinder(Unsupported), 할당 실패.
    virtual Result<bool> OverlapShape(const Vec3d& position, const Quatd& rotation,
                                      const CollisionShape& shape, const QueryFilter& filter,
                                      const QueryParams& params, OverlapMode mode,
                                      Array<OverlapResult>* all) const noexcept = 0;

    // body의 형상(scale이 구워진 그대로)으로 하는 shape trace All이다(UE ComponentSweepMulti —
    // 7.1.9절). 그 body를 가진 컴포넌트의 소유 Actor를 뺀다. 반환 값은 막는 hit가 있는지다. 오류:
    // 없는 handle·옛 세대(StaleHandle), TraceShape의 오류.
    virtual Result<bool> TraceBody(PhysicsBodyHandle body, const Vec3d& start, const Vec3d& end,
                                   const Quatd& rotation, const QueryFilter& filter,
                                   const QueryParams& params,
                                   Array<HitResult>& all) const noexcept = 0;

    // body의 형상으로 하는 overlap All이다(UE ComponentOverlapMulti). 그 body의 컴포넌트만 뺀다.
    // 반환 값은 막는 겹침이 있는지다. 오류: 없는 handle·옛 세대(StaleHandle), OverlapShape의 오류.
    virtual Result<bool> OverlapBody(PhysicsBodyHandle body, const Vec3d& position,
                                     const Quatd& rotation, const QueryFilter& filter,
                                     const QueryParams& params,
                                     Array<OverlapResult>& all) const noexcept = 0;

protected:
    PhysicsScene() noexcept = default;
    PhysicsScene(const PhysicsScene&) = delete;
    PhysicsScene& operator=(const PhysicsScene&) = delete;
};

} // namespace rocks
