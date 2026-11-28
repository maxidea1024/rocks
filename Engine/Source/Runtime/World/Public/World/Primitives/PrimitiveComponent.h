#pragma once

#include <Collision/BodySettings.h>
#include <Collision/CollisionProfileTable.h>
#include <Collision/CollisionTypes.h>
#include <Core/Text/String.h>
#include <Meta/Annotations.h>
#include <Object/Event.h>
#include <World/Lights/LightingChannels.h>
#include <World/Physics/BodyForce.h>
#include <World/Physics/BodyGeometry.h>
#include <World/Physics/BodyMotion.h>
#include <World/Physics/BodySettingsChange.h>
#include <World/Physics/ComponentPhysicsStateChange.h>
#include <World/Physics/HitResult.h>
#include <World/Physics/MoveOptions.h>
#include <World/Physics/MtdResult.h>
#include <World/Physics/PhysicsBodyHandle.h>
#include <World/Physics/PhysicsContactHit.h>
#include <World/SceneComponent.h>
#include <World/Primitives/PrimitiveTypes.h>

namespace rocks {

struct BodyKinematics;

// 질의 형상이다(Collision/CollisionShape.h — 헤더를 가볍게 두려고 구현 파일만 include한다).
struct CollisionShape;

class Actor;

// 그리거나 충돌하는 Component의 기반이다(UE UPrimitiveComponent —
// Docs/Design/EngineRuntime 24.3절). 데이터만 있다: 렌더 proxy(RS1)·충돌(CR16) 연결 전에는 아무것도
// 그리거나 충돌하지 않는다.
//
// 규약(24.3.5절):
// - setter는 UE처럼 Mobility를 보지 않는다. 값 검증 실패는 InvalidArgument이고 값을 바꾸지 않는다.
// - 값이 바뀌면 Render 시스템 상태가 있을 때 다시 보내기를 표시한다.
// - 충돌 profile은 이름과 body 값(bodySettings)이 데이터다(24.3.4절, CollisionData 9절 CD2). 빈
//   이름이면 클래스 기본(DefaultCollisionProfileName)이다. 실효 충돌 값은 World의 profile 표로
//   해석한다(TryResolveCollision). profile 이름을 정하는 모든 경로(이름 setter·등록)는 해석한
//   profile 값을 bodySettings의 사용자 지정 칸에 옮겨 둔다 — profile이 표에서 사라지면 UE처럼
//   마지막 해석 값으로 돌아간다.
//
// 스레드: World의 게임 스레드 전용이다.
ROCKS_CLASS(Abstract)
class ROCKS_WORLD_API PrimitiveComponent : public SceneComponent {
    ROCKS_GENERATED_BODY()

public:
    // custom depth stencil 값 상한(UE SetCustomDepthStencilValue clamp 255)이다.
    static constexpr int32 MaxCustomDepthStencilValue = 255;
    // 클래스 기본 충돌 profile(UE PrimitiveComponent.cpp:361)이다.
    static constexpr const char* PrimitiveCollisionProfileName = "BlockAll";

    explicit PrimitiveComponent(Allocator& allocator) noexcept
        : SceneComponent(allocator), componentHit(allocator), componentOverlapBegan(allocator),
          componentOverlapEnded(allocator), componentWake(allocator), componentSleep(allocator),
          componentPhysicsStateChanged(allocator), collisionProfileName_(allocator),
          bodySettings_(allocator), moveIgnoreActors_(allocator), moveIgnoreComponents_(allocator),
          overlappingComponents_(allocator) {
    }

    // --- 거리 ---

    float MinDrawDistance() const noexcept {
        return minDrawDistance_;
    }

    // 오류: 음수·비유한. 아래 거리도 같다.
    Result<void> SetMinDrawDistance(float distance) noexcept;

    // 설계 최대 그리기 거리(cm, 0 = 제한 없음 — UE LDMaxDrawDistance)다.
    float DesiredMaxDrawDistance() const noexcept {
        return desiredMaxDrawDistance_;
    }

    // UE SetCullDistance다.
    Result<void> SetDesiredMaxDrawDistance(float distance) noexcept;

    // 실효 최대 거리다: neverDistanceCull이면 0, 아니면 desiredMaxDrawDistance(UE
    // CachedMaxDrawDistance에서 cull 거리 볼륨·HLOD 부모를 뺀 값 — 24.3.3절).
    float EffectiveMaxDrawDistance() const noexcept {
        return neverDistanceCull_ ? 0.0f : desiredMaxDrawDistance_;
    }

    bool NeverDistanceCull() const noexcept {
        return neverDistanceCull_;
    }

    void SetNeverDistanceCull(bool never) noexcept {
        Change(neverDistanceCull_, never);
    }

    bool AllowsCullDistanceVolume() const noexcept {
        return allowCullDistanceVolume_;
    }

    void SetAllowCullDistanceVolume(bool allow) noexcept {
        Change(allowCullDistanceVolume_, allow);
    }

    // --- 그림자 ---

    bool CastsShadow() const noexcept {
        return castShadow_;
    }

    void SetCastShadow(bool cast) noexcept {
        Change(castShadow_, cast);
    }

    bool CastsDynamicShadow() const noexcept {
        return castDynamicShadow_;
    }

    void SetCastDynamicShadow(bool cast) noexcept {
        Change(castDynamicShadow_, cast);
    }

    bool CastsStaticShadow() const noexcept {
        return castStaticShadow_;
    }

    void SetCastStaticShadow(bool cast) noexcept {
        Change(castStaticShadow_, cast);
    }

    bool CastsContactShadow() const noexcept {
        return castContactShadow_;
    }

    void SetCastContactShadow(bool cast) noexcept {
        Change(castContactShadow_, cast);
    }

    bool CastsFarShadow() const noexcept {
        return castFarShadow_;
    }

    void SetCastFarShadow(bool cast) noexcept {
        Change(castFarShadow_, cast);
    }

    bool CastsInsetShadow() const noexcept {
        return castInsetShadow_;
    }

    void SetCastInsetShadow(bool cast) noexcept {
        Change(castInsetShadow_, cast);
    }

    bool CastsHiddenShadow() const noexcept {
        return castHiddenShadow_;
    }

    void SetCastHiddenShadow(bool cast) noexcept {
        Change(castHiddenShadow_, cast);
    }

    // cinematic 전용 광원에만 그림자를 드리운다(UE bCastCinematicShadow — 편집기는 Movable일 때만
    // 고치게 한다).
    bool CastsCinematicShadow() const noexcept {
        return castCinematicShadow_;
    }

    void SetCastCinematicShadow(bool cast) noexcept {
        Change(castCinematicShadow_, cast);
    }

    bool CastsShadowAsTwoSided() const noexcept {
        return castShadowAsTwoSided_;
    }

    void SetCastShadowAsTwoSided(bool twoSided) noexcept {
        Change(castShadowAsTwoSided_, twoSided);
    }

    bool IsSelfShadowOnly() const noexcept {
        return selfShadowOnly_;
    }

    void SetSelfShadowOnly(bool selfOnly) noexcept {
        Change(selfShadowOnly_, selfOnly);
    }

    bool CastsVolumetricTranslucentShadow() const noexcept {
        return castVolumetricTranslucentShadow_;
    }

    void SetCastVolumetricTranslucentShadow(bool cast) noexcept {
        Change(castVolumetricTranslucentShadow_, cast);
    }

    bool ReceivesMobileCsmShadows() const noexcept {
        return receiveMobileCsmShadows_;
    }

    void SetReceiveMobileCsmShadows(bool receive) noexcept {
        Change(receiveMobileCsmShadows_, receive);
    }

    bool UsesSingleSampleShadowFromStationaryLights() const noexcept {
        return singleSampleShadowFromStationaryLights_;
    }

    void SetSingleSampleShadowFromStationaryLights(bool single) noexcept {
        Change(singleSampleShadowFromStationaryLights_, single);
    }

    // --- 조명·렌더 pass ---

    bool AffectsDynamicIndirectLighting() const noexcept {
        return affectDynamicIndirectLighting_;
    }

    void SetAffectDynamicIndirectLighting(bool affects) noexcept {
        Change(affectDynamicIndirectLighting_, affects);
    }

    bool AffectsDistanceFieldLighting() const noexcept {
        return affectDistanceFieldLighting_;
    }

    void SetAffectDistanceFieldLighting(bool affects) noexcept {
        Change(affectDistanceFieldLighting_, affects);
    }

    bool ReceivesDecals() const noexcept {
        return receivesDecals_;
    }

    void SetReceivesDecals(bool receives) noexcept {
        Change(receivesDecals_, receives);
    }

    bool RendersInMainPass() const noexcept {
        return renderInMainPass_;
    }

    void SetRenderInMainPass(bool render) noexcept {
        Change(renderInMainPass_, render);
    }

    bool RendersInDepthPass() const noexcept {
        return renderInDepthPass_;
    }

    void SetRenderInDepthPass(bool render) noexcept {
        Change(renderInDepthPass_, render);
    }

    bool IsVisibleInReflectionCaptures() const noexcept {
        return visibleInReflectionCaptures_;
    }

    void SetVisibleInReflectionCaptures(bool visible) noexcept {
        Change(visibleInReflectionCaptures_, visible);
    }

    bool IsVisibleInRealTimeSkyCaptures() const noexcept {
        return visibleInRealTimeSkyCaptures_;
    }

    void SetVisibleInRealTimeSkyCaptures(bool visible) noexcept {
        Change(visibleInRealTimeSkyCaptures_, visible);
    }

    bool IsVisibleInSceneCaptureOnly() const noexcept {
        return visibleInSceneCaptureOnly_;
    }

    void SetVisibleInSceneCaptureOnly(bool only) noexcept {
        Change(visibleInSceneCaptureOnly_, only);
    }

    bool IsHiddenInSceneCapture() const noexcept {
        return hiddenInSceneCapture_;
    }

    void SetHiddenInSceneCapture(bool hidden) noexcept {
        Change(hiddenInSceneCapture_, hidden);
    }

    // 소유자 시점에서 숨긴다(UE bOwnerNoSee).
    bool IsOwnerNoSee() const noexcept {
        return ownerNoSee_;
    }

    void SetOwnerNoSee(bool noSee) noexcept {
        Change(ownerNoSee_, noSee);
    }

    // 소유자 시점에서만 보인다(UE bOnlyOwnerSee).
    bool IsOnlyOwnerSee() const noexcept {
        return onlyOwnerSee_;
    }

    void SetOnlyOwnerSee(bool onlyOwner) noexcept {
        Change(onlyOwnerSee_, onlyOwner);
    }

    bool IsHoldout() const noexcept {
        return holdout_;
    }

    void SetHoldout(bool holdout) noexcept {
        Change(holdout_, holdout);
    }

    bool TreatsAsBackgroundForOcclusion() const noexcept {
        return treatAsBackgroundForOcclusion_;
    }

    void SetTreatAsBackgroundForOcclusion(bool background) noexcept {
        Change(treatAsBackgroundForOcclusion_, background);
    }

    bool UsesAsOccluder() const noexcept {
        return useAsOccluder_;
    }

    void SetUseAsOccluder(bool occluder) noexcept {
        Change(useAsOccluder_, occluder);
    }

    // --- custom depth·반투명 정렬·채널 ---

    bool RendersCustomDepth() const noexcept {
        return renderCustomDepth_;
    }

    void SetRenderCustomDepth(bool render) noexcept {
        Change(renderCustomDepth_, render);
    }

    int32 CustomDepthStencilValue() const noexcept {
        return customDepthStencilValue_;
    }

    // 오류: [0, MaxCustomDepthStencilValue] 밖(UE는 clamp).
    Result<void> SetCustomDepthStencilValue(int32 value) noexcept;

    RendererStencilMask CustomDepthStencilWriteMask() const noexcept {
        return customDepthStencilWriteMask_;
    }

    // 오류: 범위 밖 enum.
    Result<void> SetCustomDepthStencilWriteMask(RendererStencilMask mask) noexcept;

    int32 TranslucencySortPriority() const noexcept {
        return translucencySortPriority_;
    }

    void SetTranslucencySortPriority(int32 priority) noexcept {
        Change(translucencySortPriority_, priority);
    }

    float TranslucencySortDistanceOffset() const noexcept {
        return translucencySortDistanceOffset_;
    }

    // 오류: 비유한.
    Result<void> SetTranslucencySortDistanceOffset(float offset) noexcept;

    const rocks::LightingChannels& LightingChannels() const noexcept {
        return lightingChannels_;
    }

    void SetLightingChannels(const rocks::LightingChannels& channels) noexcept {
        Change(lightingChannels_, channels);
    }

    // --- bounds·충돌 ---

    // bounds 배율이다(메시 계열 CalcBounds가 곱한다 — Shape는 쓰지 않는다, UE와 같다).
    float BoundsScale() const noexcept {
        return boundsScale_;
    }

    // 오류: 0 이하·비유한.
    Result<void> SetBoundsScale(float scale) noexcept;

    bool GeneratesOverlapEvents() const noexcept {
        return generateOverlapEvents_;
    }

    void SetGenerateOverlapEvents(bool generate) noexcept {
        Change(generateOverlapEvents_, generate);
    }

    rocks::CanBeCharacterBase CanCharacterStepUpOn() const noexcept {
        return canCharacterStepUpOn_;
    }

    // 오류: 범위 밖 enum.
    Result<void> SetCanCharacterStepUpOn(rocks::CanBeCharacterBase value) noexcept;

    // 실효 충돌 profile 이름이다(저장 값이 비면 클래스 기본 — 24.3.4절).
    StringView CollisionProfileName() const noexcept;

    // 클래스 기본과 같은 이름(ASCII 대소문자 무시 — UE FName 비교)은 빈 값으로 둔다. 표에 그
    // profile이 있으면 값을 bodySettings 사용자 지정 칸에 옮긴다(UE SetCollisionProfileName →
    // ReadConfig). 없으면 body는 그대로다(마지막 해석 값 — UE와 같다). profile은 물리 데이터라
    // Render 다시 보내기를 표시하지 않는다. 오류: 할당 실패·잘못된 UTF-8(값 유지).
    Result<void> SetCollisionProfileName(StringView name) noexcept;

    // body 값이다(UE FBodyInstance 영속 필드 — CollisionData 6절).
    const BodySettings& GetBodySettings() const noexcept {
        return bodySettings_;
    }

    // --- 물리 body(Docs/Design/PhysicsModule 2.3–2.5절, 7.1.3절) ---

    // body 형상(scale 전 cm 원소 — 7.1.21절)을 geometry 끝에 더한다. 형상이 있는 파생
    // (Box·Sphere·Capsule)이 원소를 더한다. 기본은 형상 없음(원소 0개 — body를 만들지 않는다).
    // 오류: 할당 실패(geometry는 더하던 중일 수 있다).
    virtual Result<void> BuildBodyGeometry(BodyGeometry& geometry) const noexcept;

    // 형상 크기가 0인지다(UE IsZeroExtent — 기본 거짓, Box·Sphere·Capsule이 재정의). 컴포넌트
    // sweep이 line trace로 바뀐다(UE ComponentSweepMultiByChannel).
    virtual bool IsZeroExtent() const noexcept;

    // 물리 body handle이다. World에 물리 장면이 있고 형상과 충돌이 있으면 등록 때 생기고 등록 해제
    // 때 없어진다(없으면 IsNull). weld된 컴포넌트는 자기 body가 없다(PhysicsBody가 루트 body).
    PhysicsBodyHandle BodyHandle() const noexcept {
        return bodyHandle_;
    }

    // --- weld(PhysicsModule 2.8절·7.1.23절 — UE WeldTo·UnWeldFromParent) ---
    //
    // weld된 컴포넌트는 자기 원소를 루트 컴포넌트의 body에 합쳐 함께 시뮬레이션한다(자기 body
    // 없음). autoWeld이면 물리 상태를 만들 때, weldSimulatedBodies 부착이면 붙인 뒤 weld하고,
    // 떼기·상태 파괴 때 푼다. 힘·속도·질량·잠듦·body 값 API는 루트로 간다(UE GetBodyInstance() 기본
    // weld).
    //
    // 사용 예:
    //   wheel.WeldTo(chassis);   // 붙이고 chassis body에 합친다
    //   wheel.AddImpulse(kick);  // chassis body에 준다

    bool IsWelded() const noexcept {
        return !weldRoot_.IsNull();
    }

    // 내 원소를 가진 루트 컴포넌트다(weld가 아니면 null).
    ObjectHandle WeldRoot() const noexcept {
        return weldRoot_;
    }

    // weld면 루트 body, 아니면 자기 body다(UE GetBodyInstance() 기본 bGetWelded).
    PhysicsBodyHandle PhysicsBody() const noexcept {
        return IsWelded() ? weldBody_ : bodyHandle_;
    }

    // parent의 socket에 붙어 있지 않으면 월드 transform을 유지하며 붙이고 weld한다(UE WeldTo).
    // weldToKinematicParent면 시뮬레이션하지 않는 루트에도 합친다. 루트가 없거나 합칠 수 없으면
    // 자기 body로 남는다.
    void WeldTo(ObjectHandle parent, StringView socket = {},
                bool weldToKinematicParent = false) noexcept;

    // 루트 body에서 빠지고 자기 body를 다시 만든다(UE UnWeldFromParent). 같은 루트에 이 컴포넌트를
    // 거쳐 weld된 자손도 빠져 자기 body를 만든다. 이 컴포넌트가 시뮬레이션하면 그 자손을 다시 자기
    // body에 합친다.
    void UnWeldFromParent() noexcept;

    // 직접 자식마다 UnWeldFromParent다(UE UnWeldChildren).
    void UnWeldChildren() noexcept;

    // 부모 쪽 루트 body에 합칠지다(UE FBodyInstance::bAutoWeld — 영속, 기본 거짓). 물리 상태를 만들
    // 때와 루트가 시뮬레이션으로 바뀔 때(ApplyWeldOnChildren) 본다.
    void SetAutoWeld(bool autoWeld) noexcept {
        bodySettings_.autoWeld = autoWeld;
    }

    bool GetAutoWeld() const noexcept {
        return bodySettings_.autoWeld;
    }

    // body 운동 종류다(2.5절): Mobility가 Movable이 아니면 Static, Movable이고 simulatePhysics면
    // Dynamic, 그 밖은 Kinematic.
    BodyMotion PhysicsBodyMotion() const noexcept;

    // 물리 시뮬레이션을 켜거나 끈다(UE SetSimulatePhysics — bodySettings.simulatePhysics,
    // PhysicsModule 2.3.1·7.1.5절). 켤 때 등록돼 있고 부모가 있으면 먼저 월드 transform을 유지하며
    // 뗀다(시뮬레이션하는 컴포넌트는 부착될 수 없다). body가 있으면 다시 만들지 않고 Kinematic ↔
    // Dynamic으로 바꾼다: 끄면 그 자리에서 속도 0, 켜면 startAwake일 때 깨운다. Movable이 아니면
    // 값만 바꾸고 Warning SimulateOnStaticBody다(Static body 그대로).
    //
    // 사용 예:
    //   ball.SetSimulatePhysics(true);   // 다음 고정 스텝부터 떨어진다
    void SetSimulatePhysics(bool simulate) noexcept;

    // 시뮬레이션 접촉 hit를 받을지 바꾼다(UE SetNotifyRigidBodyCollision —
    // bodySettings.notifyRigidBodyCollision, PhysicsModule 5.4절·7.1.12절). 켜면 이 컴포넌트의
    // body가 닿는 스텝마다(solver 충격량이 있거나 probe 접촉) 소유 Actor의 OnHit가 불린다. body가
    // 있으면 다음 step부터 적용된다.
    //
    // 사용 예:
    //   ball.SetNotifyRigidBodyCollision(true);   // 바닥에 닿으면 Actor::OnHit
    void SetNotifyRigidBodyCollision(bool notify) noexcept;

    // --- 힘·충격량·속도(PhysicsModule 2.6절·7.1.17절 — UE PrimitiveComponentPhysics.cpp) ---
    //
    // 단위는 cm·kg·초이고 위치는 월드 cm다(…Local만 body 공간). 시뮬레이션하는 body(Dynamic)에만
    // 효력이 있고 효력이 있으면 body를 깨운다. body가 없거나 Static·Kinematic이면 무시한다(UE와
    // 같다 — 속도 setter의 Kinematic 무시만 다르다). 충격량은 다음 고정 스텝에 속도에 더해지고(바로
    // 읽으면 옛 속도 — UE). 힘·토크는 고정 스텝 밖(Update 등)에서 주면 그 프레임의 모든 고정
    // 스텝에, Fixed hook 안에서 주면 그 스텝에만 작용한다(UE bAllowSubstepping — 지속 힘은
    // 프레임마다 다시 준다). 비유한 값은 무시한다(Warning NonFiniteBodyInput). 방사가 아닌 호출은
    // UE처럼 잘못된 호출을 경고한다: Movable이 아님(StaticComponentMoved), 시뮬레이션
    // 꺼짐(PhysicsOperationWithoutSimulation), 물리 충돌 없음
    // (PhysicsOperationWithoutPhysicsCollision — 뒤 둘은 Release 빌드 밖).
    //
    // 사용 예:
    //   ball.AddImpulse(Vec3d(0.0, 0.0, 200.0));            // 2 kg이면 다음 스텝에 +100 cm/s
    //   ball.AddForce(Vec3d(0.0, 0.0, 980.0), true);        // 스텝마다 주면 중력 상쇄
    //   ball.AddRadialImpulse(blast, 500.0, 1000.0, RadialFalloff::Linear, true);

    // 질량 중심에 충격량(kg·cm/s)을 준다. velocityChange면 값이 속도 변화(cm/s)다.
    void AddImpulse(const Vec3d& impulse, bool velocityChange = false) noexcept;
    // 각충격량(kg·cm²/s — rad 기준, 월드 축)을 준다. velocityChange면 각속도 변화(rad/s)다.
    void AddAngularImpulseInRadians(const Vec3d& impulse, bool velocityChange = false) noexcept;
    // 각충격량을 도 기준으로 준다(rad로 바꿔 같은 경로).
    void AddAngularImpulseInDegrees(const Vec3d& impulse, bool velocityChange = false) noexcept;
    // location(월드 cm)에 충격량을 준다(선형 몫과 질량 중심 둘레 각 몫).
    void AddImpulseAtLocation(const Vec3d& impulse, const Vec3d& location) noexcept;
    // location에 속도 변화(cm/s)를 내는 충격량(질량 × 값)을 준다.
    void AddVelocityChangeImpulseAtLocation(const Vec3d& velocityChange,
                                            const Vec3d& location) noexcept;
    // origin에서 radius(cm) 안의 이 body를 바깥으로 민다(질량 중심 기준, 거리 < radius). strength는
    // 충격량 크기(kg·cm/s, velocityChange면 cm/s)다. ignoreRadialImpulse면 무시한다.
    void AddRadialImpulse(const Vec3d& origin, double radius, double strength,
                          RadialFalloff falloff, bool velocityChange = false) noexcept;
    // 질량 중심에 힘(kg·cm/s²)을 준다. accelerationChange면 값이 가속도(cm/s²)다.
    void AddForce(const Vec3d& force, bool accelerationChange = false) noexcept;
    // location(월드 cm)에 힘을 준다.
    void AddForceAtLocation(const Vec3d& force, const Vec3d& location) noexcept;
    // body 공간 location·힘을 준다(body 원점 transform으로 월드로 바꾼다).
    void AddForceAtLocationLocal(const Vec3d& force, const Vec3d& location) noexcept;
    // origin에서 radius 안(거리 <= radius)의 이 body를 바깥으로 미는 힘이다. strength는 힘
    // 크기(kg·cm/s², accelerationChange면 cm/s²)다. ignoreRadialForce면 무시한다.
    void AddRadialForce(const Vec3d& origin, double radius, double strength, RadialFalloff falloff,
                        bool accelerationChange = false) noexcept;
    // 토크(kg·cm²/s² — rad 기준, 월드 축)를 준다. accelerationChange면 각가속도(rad/s²)다.
    void AddTorqueInRadians(const Vec3d& torque, bool accelerationChange = false) noexcept;
    // 토크를 도 기준으로 준다.
    void AddTorqueInDegrees(const Vec3d& torque, bool accelerationChange = false) noexcept;

    // 선속도(cm/s)를 바꾼다(addToCurrent면 더한다). 허용 자유도로 거르고 상한으로 자르며, 잠든
    // body는 거의 0이 아닌 속도에 깨운다. Movable이 아니면 경고한다. Kinematic은 무시한다(UE는 값을
    // 넣지만 Kinematic이 그 속도로 움직이지 않는다 — Jolt Kinematic은 움직이므로 다르게 둔다).
    void SetPhysicsLinearVelocity(const Vec3d& velocity, bool addToCurrent = false) noexcept;
    // 선속도(cm/s)다(body가 없으면 0).
    Vec3d GetPhysicsLinearVelocity() const noexcept;
    // 월드 점 point의 속도(cm/s)다: v + ω × (point − 질량 중심)(UE GetUnrealWorldVelocityAtPoint).
    Vec3d GetPhysicsLinearVelocityAtPoint(const Vec3d& point) const noexcept;
    // 각속도(월드 축)를 바꾼다(rad/s·도/s — 규칙은 SetPhysicsLinearVelocity와 같다).
    void SetPhysicsAngularVelocityInRadians(const Vec3d& velocity,
                                            bool addToCurrent = false) noexcept;
    void SetPhysicsAngularVelocityInDegrees(const Vec3d& velocity,
                                            bool addToCurrent = false) noexcept;
    // 각속도(월드 축, rad/s·도/s)다(body가 없으면 0).
    Vec3d GetPhysicsAngularVelocityInRadians() const noexcept;
    Vec3d GetPhysicsAngularVelocityInDegrees() const noexcept;

    // --- 질량·body 값(PhysicsModule 2.6절·7.1.18절 — UE PrimitiveComponentPhysics.cpp) ---
    //
    // setter는 bodySettings(영속)를 바꾸고 body가 있으면 바로 넣는다(없으면 값만 — 다음 body가
    // 쓴다). 비유한 값·음수 감쇠·음수 각속도 상한은 값을 바꾸지 않고 Warning InvalidBodyValue다.
    //
    // 사용 예:
    //   crate.SetMassOverrideInKg(2.0f);        // 2 kg(다음 스텝부터 그 질량)
    //   crate.SetCenterOfMass(Vec3d(0.0, 0.0, -10.0));
    //   crate.SetEnableGravity(false);

    // 질량(kg)이다(UE 규칙 — 7.1.15절). body가 없거나 Static이면 0이다. 시뮬레이션하지 않으면
    // 경고한다(UE GetMass).
    float GetMass() const noexcept;
    // overrideMass면 질량을 massInKg로 둔다(아니면 형상 질량 규칙으로 돌아간다 — UE
    // SetMassOverrideInKg). 질량·관성을 다시 계산하고 깨우지 않는다.
    void SetMassOverrideInKg(float massInKg, bool overrideMass = true) noexcept;
    // 형상 질량 배율(UE MassScale)이다.
    void SetMassScale(float scale) noexcept;
    float GetMassScale() const noexcept;

    // 월드 질량 중심(cm)이다(body가 없으면 0).
    Vec3d GetCenterOfMass() const noexcept;
    // 질량 중심 이동(body 공간 cm — UE COMNudge)을 바꾼다.
    void SetCenterOfMass(const Vec3d& offset) noexcept;
    // 관성(kg·cm², body 축 대각 — UE GetInertiaTensor)이다. body가 없거나 Static이면 0이다. 지금
    // 형상(상자·구·캡슐)은 축이 정렬돼 UE 주값과 같다(비대각 형상은 PH6 — 7.1.18절).
    Vec3d GetInertiaTensor() const noexcept;

    // 각속도 상한을 덮어쓴다(UE SetPhysicsMaxAngularVelocity — overrideMaxAngularVelocity를 켠다).
    // addToCurrent면 body의 지금 실효 상한에 더한다(body가 없으면 더하지 않는다 — UE).
    void SetPhysicsMaxAngularVelocityInDegrees(float maxDegrees,
                                               bool addToCurrent = false) noexcept;
    void SetPhysicsMaxAngularVelocityInRadians(float maxRadians,
                                               bool addToCurrent = false) noexcept;
    // 실효 각속도 상한이다(덮어쓰기면 그 값, 아니면 물리 설정 — 장면이 없으면 Collision 기본).
    float GetPhysicsMaxAngularVelocityInDegrees() const noexcept;
    float GetPhysicsMaxAngularVelocityInRadians() const noexcept;

    // 중력을 켜거나 끈다(켜면 Dynamic body를 깨운다 — UE SetEnableGravity).
    void SetEnableGravity(bool enable) noexcept;
    bool IsGravityEnabled() const noexcept;

    // 선형·각 감쇠(1/s — 음수 거부)다.
    void SetLinearDamping(float damping) noexcept;
    float GetLinearDamping() const noexcept;

    void SetAngularDamping(float damping) noexcept;
    float GetAngularDamping() const noexcept;

    // CCD를 켜거나 끈다(UE SetUseCCD — Jolt LinearCast).
    void SetUseCCD(bool useCcd) noexcept;
    // gyroscopic 토크를 켜거나 끈다(회전 축을 잠근 body는 켜도 꺼진다 — 7.1.16절).
    void SetGyroscopicTorqueEnabled(bool enable) noexcept;
    bool GetGyroscopicTorqueEnabled() const noexcept;

    // 자유도 방식을 바꾸고 잠금을 다시 넣는다(UE SetConstraintMode → SetDOFLock — 잠금 표지는
    // 그대로, 7.1.16절). 모르는 값은 Warning InvalidBodyValue다.
    void SetConstraintMode(DofMode mode) noexcept;

    // --- 잠듦·깨움(PhysicsModule 2.7절·7.1.19절 — UE WakeRigidBody·PutRigidBodyToSleep) ---
    //
    // Dynamic body에만 효력이 있다. 깨움·잠듦 이벤트를 내지 않는다(UE — 깨우면 쌓인 이벤트도
    // 지운다). 재우면 속도가 0이고 대기 충격량·힘을 버린다.
    //
    // 사용 예:
    //   crate.PutRigidBodyToSleep();   // 쌓아 둔 상자를 처음부터 재운다
    //   if (!crate.RigidBodyIsAwake()) { crate.WakeRigidBody(); }
    void WakeRigidBody() noexcept;
    void PutRigidBodyToSleep() noexcept;
    // Dynamic body가 깨어 있는지다(Kinematic·Static·body 없음 거짓 — UE IsInstanceAwake).
    bool RigidBodyIsAwake() const noexcept;

    // 깨움·잠듦 이벤트(componentWake·componentSleep)를 낼지다(UE bGenerateWakeEvents — 영속, 기본
    // 거짓, 발행 때 본다).
    void SetGenerateWakeEvents(bool generate) noexcept {
        bodySettings_.generateWakeEvents = generate;
    }

    bool GetGenerateWakeEvents() const noexcept {
        return bodySettings_.generateWakeEvents;
    }

    // 깨움(awake)·잠듦 이벤트를 발행한다(UE DispatchWakeEvents — Physics의 PhysicsResults만 부른다,
    // 7.1.19절). generateWakeEvents가 거짓이거나 파괴 대기면 자기 발행은 하지 않는다. 그다음 이
    // 컴포넌트에 직접 weld된 자식에 재귀로 전한다(7.1.23절).
    void DispatchWakeEvents(bool awake) noexcept;

    // 방사 충격량·힘을 무시하는지다(UE bIgnoreRadialImpulse·bIgnoreRadialForce — 영속, 기본 거짓).
    bool GetIgnoreRadialImpulse() const noexcept {
        return ignoreRadialImpulse_;
    }

    void SetIgnoreRadialImpulse(bool ignore) noexcept {
        ignoreRadialImpulse_ = ignore;
    }

    bool GetIgnoreRadialForce() const noexcept {
        return ignoreRadialForce_;
    }

    void SetIgnoreRadialForce(bool ignore) noexcept {
        ignoreRadialForce_ = ignore;
    }

    // 이 컴포넌트를 delta만큼 옮기고 rotation으로 돌린다(UE MoveComponent —
    // Docs/Design/PhysicsModule 5.1절·7.1.10절). sweep이면 자기 body 형상으로 쓸고 가(채널·응답은
    // 자기 것, 자기 Actor와 이동 무시 목록은 뺀다) 막는 hit 앞(pullback)에 멈추고 hit를
    // 발행한다(자기 Actor·상대 Actor의 OnHit). 시작 겹침에서 빠져나가는 이동은 막히지 않는다.
    // outHit가 있으면 막는 hit를, 없으면 trace 시작·끝만 채운 빈 hit를 쓴다. 반환 값은
    // 움직였는지다(sweep 최소 이동 이하·같은 회전이면 움직이지 않고 참 — UE). 오류:
    // Static·Stationary 이동(InvalidArgument — StaticComponentMoved), 0·비유한 rotation·비유한
    // delta(InvalidArgument), 질의 할당 실패.
    //
    // 사용 예:
    //   HitResult hit;
    //   (void)box.MoveComponent(velocity * dt, box.WorldQuaternion(), true, &hit, {});
    //   if (hit.IsValidBlockingHit()) { ... }   // 벽에 닿았다
    Result<bool> MoveComponent(const Vec3d& delta, const Quatd& rotation, bool sweep,
                               HitResult* outHit, const MoveOptions& options) noexcept;

    // 주어진 형상을 rotation으로 position에 놓았을 때 이 컴포넌트의 body와 겹치면 참이고 out에
    // 형상을 밀어낼 단위 방향과 거리(cm)를 쓴다(UE ComputePenetration — Docs/Design/PhysicsModule
    // 5.2절·7.1.13절). body·물리 장면이 없거나 겹치지 않으면 거짓이다(out은 그대로).
    //
    // 사용 예:
    //   MtdResult mtd;
    //   if (wall.ComputePenetration(mtd, CollisionShape::Sphere(20.0), at, Quatd())) {
    //       at += mtd.direction * mtd.distance;   // 벽 밖으로
    //   }
    bool ComputePenetration(MtdResult& out, const CollisionShape& shape, const Vec3d& position,
                            const Quatd& rotation) const noexcept;

    // 이 컴포넌트의 충돌 형상을 inflation(cm)만큼 부풀린 질의 형상이다(UE GetCollisionShape —
    // 7.1.13절). 기본은 월드 bounds 상자이고 구·상자·캡슐은 자기 크기(scale 반영)다.
    virtual CollisionShape GetCollisionShape(double inflation) const noexcept;

    // 이동 sweep에서 무시할 Actor·컴포넌트다(UE IgnoreActorWhenMoving·IgnoreComponentWhenMoving —
    // 실행 상태, 저장하지 않는다). ignore가 참이면 더하고(이미 있으면 그대로) 거짓이면 뺀다. 오류:
    // 할당 실패.
    Result<void> IgnoreActorWhenMoving(ObjectHandle actor, bool ignore) noexcept;
    Result<void> IgnoreComponentWhenMoving(ObjectHandle component, bool ignore) noexcept;

    void ClearMoveIgnoreActors() noexcept {
        moveIgnoreActors_.Clear();
    }

    void ClearMoveIgnoreComponents() noexcept {
        moveIgnoreComponents_.Clear();
    }

    Span<const ObjectHandle> MoveIgnoreActors() const noexcept {
        return moveIgnoreActors_.View();
    }

    Span<const ObjectHandle> MoveIgnoreComponents() const noexcept {
        return moveIgnoreComponents_.View();
    }

    // --- overlap(Docs/Design/PhysicsModule 5.3절·7.1.11절) ---

    // 지금 겹친 컴포넌트다(UE GetOverlappingComponents — 실행 상태, 저장하지 않는다). 겹침은
    // 대칭이다(상대 장부에도 이 컴포넌트가 있다). 순서는 정하지 않는다(끝낸 칸은 마지막 칸으로
    // 메운다 — UE RemoveAtSwap). 이 컴포넌트의 Borrow 범위에서만 쓴다.
    //
    // 사용 예:
    //   auto inside = world.With<BoxComponent>(trigger, [&](BoxComponent& box) noexcept {
    //       return box.IsOverlappingComponent(playerCapsule);
    //   });
    Span<const ObjectHandle> OverlappingComponents() const noexcept {
        return overlappingComponents_.View();
    }

    bool IsOverlappingComponent(ObjectHandle component) const noexcept {
        return overlappingComponents_.Contains(component);
    }

    // actor의 Primitive 컴포넌트 중 other의 컴포넌트와 겹친 것이 있는지다(UE
    // AActor::IsOverlappingActor). 어느 쪽이든 없는 Actor면 거짓이다.
    static bool IsOverlappingActor(World& world, ObjectHandle actor, ObjectHandle other) noexcept;

    // 자기 장부를 지금 자리의 overlap 질의로 갱신한 뒤 자식으로 내려간다(UE UpdateOverlapsImpl —
    // 7.1.11절). generateOverlapEvents가 거짓이거나 질의 충돌이 꺼져 있으면 겹침을 모두 끝낸다.
    // transform setter·MoveComponent·충돌 설정 변경·Actor 시작이 부르므로 직접 부를 일은 드물다.
    // 할당 실패면 장부를 그대로 둔다(다음 갱신 때 다시 계산한다).
    void UpdateOverlaps() noexcept override;

    // 물리 결과를 컴포넌트에 쓴다(Docs/Design/PhysicsModule 3.4절 — Physics의 PhysicsResults만
    // 부른다). 월드 위치(cm)·회전으로 옮기되 자기 body로 되돌려 보내지 않는다(UE
    // MOVECOMP_SkipPhysicsMove). 부착된 자식은 따라가고 자기 body를 옮긴다. 오류: 이동 실패(그대로
    // 돌려준다 — SetWorldLocationAndRotation).
    Result<void> ApplyPhysicsTransform(const Vec3d& location, const Quatd& rotation) noexcept;

    // hit·overlap 이벤트다(UE OnComponentHit·OnComponentBeginOverlap·OnComponentEndOverlap —
    // Docs/Design/PhysicsModule 5.5절·7.1.14절). componentHit는 소유 Actor의 OnHit·actorHit 뒤,
    // overlap 이벤트는 World가 Playing일 때 Actor hook보다 먼저 발행한다(같은 Actor 컴포넌트끼리도
    // 난다). componentOverlapBegan의 sweepResult는 sweep으로 생긴 겹침이면 그 hit(fromSweep 참),
    // 아니면 상대 컴포넌트·Actor만 채운 hit다. 리플렉션 바인딩은 저장한다.
    //
    // 사용 예:
    //   (void)trigger.componentOverlapBegan.TryAddLambda(
    //       [](WeakObjectRef<PrimitiveComponent> self, WeakObjectRef<Actor> otherActor,
    //          WeakObjectRef<PrimitiveComponent> otherComponent, bool fromSweep,
    //          const HitResult& sweep) noexcept { ... });
    //
    // Actor·컴포넌트 인자는 WeakObjectRef다(Sparks가 객체 인자로 받는다 — SparksBinding B14).
    ROCKS_PROPERTY(Params = "hitComponent,otherActor,otherComponent,normalImpulse,hit")
    Event<WeakObjectRef<PrimitiveComponent>, WeakObjectRef<Actor>,
          WeakObjectRef<PrimitiveComponent>, Vec3d, const HitResult&>
        componentHit;
    ROCKS_PROPERTY(Params = "overlappedComponent,otherActor,otherComponent,fromSweep,sweepResult")
    Event<WeakObjectRef<PrimitiveComponent>, WeakObjectRef<Actor>,
          WeakObjectRef<PrimitiveComponent>, bool, const HitResult&>
        componentOverlapBegan;
    ROCKS_PROPERTY(Params = "overlappedComponent,otherActor,otherComponent")
    Event<WeakObjectRef<PrimitiveComponent>, WeakObjectRef<Actor>,
          WeakObjectRef<PrimitiveComponent>>
        componentOverlapEnded;

    // 깨움·잠듦 이벤트다(UE OnComponentWake·OnComponentSleep — 7.1.19절, generateWakeEvents일
    // 때만, 결과 반영 때 컴포넌트 자세 반영 뒤). 뼈 이름 인자는 skeletal(CM7) 때 더한다.
    ROCKS_PROPERTY(Params = "wakingComponent")
    Event<WeakObjectRef<PrimitiveComponent>> componentWake;
    ROCKS_PROPERTY(Params = "sleepingComponent")
    Event<WeakObjectRef<PrimitiveComponent>> componentSleep;
    // 물리 body를 만들었거나(Created) 지웠다(Destroyed — UE OnComponentPhysicsStateChanged).
    ROCKS_PROPERTY(Params = "changedComponent,stateChange")
    Event<WeakObjectRef<PrimitiveComponent>, ComponentPhysicsStateChange>
        componentPhysicsStateChanged;

    // 시뮬레이션 접촉 hit 하나를 발행한다(UE AActor::DispatchPhysicsCollisionHit — PhysicsModule
    // 7.1.12절, Physics의 PhysicsResults만 부른다). hit.myComponent나 그 소유 Actor가 사라졌거나
    // 파괴 대기면 하지 않는다. 소유 Actor의 OnHit(myComponent, otherActor, otherComponent,
    // selfMoved 참, normalImpulse, hit)다.
    static void DispatchPhysicsHit(World& world, const PhysicsContactHit& hit) noexcept;

    // 실효 충돌 값이다(CollisionData 5절 ResolveCollision — World의 profile 표, 이름은
    // CollisionProfileName). profileName은 World 표가 살아 있는 동안 유효하다. 오류: World 장부 밖
    // 객체(HasWorld 거짓 — InvalidArgument).
    Result<ResolvedCollision> TryResolveCollision() const noexcept;

    // 아래 넷은 profile 이름을 "Custom"으로 바꾸고 실효 값(바꾼 칸 포함)을 body 사용자 지정 칸에
    // 옮긴다(UE SetCollisionEnabled·SetCollisionObjectType·SetCollisionResponseToChannel·
    // SetCollisionResponseToAllChannels → InvalidateCollisionProfileName). Enabled·응답은 값이
    // 같으면 아무것도 바꾸지 않고, object type은 UE처럼 같은 값이어도 바꾼다. customResponses는
    // 실효 값의 차이 칸으로 다시 쓰므로 표에 아직 없는 채널 이름의 칸은 사라진다(UE
    // UpdateResponseContainerFromArray와 같다). 오류: World 장부 밖 객체·범위 밖
    // enum(InvalidArgument), 할당 실패(값 유지).
    //
    // 사용 예:
    //   primitive.SetCollisionResponseToChannel(CollisionChannel::Pawn,
    //   CollisionResponse::Overlap);
    //   // CollisionProfileName() == "Custom", 나머지 칸은 직전 profile 값 그대로
    Result<void> SetCollisionEnabled(CollisionEnabled enabled) noexcept;
    Result<void> SetCollisionObjectType(CollisionChannel channel) noexcept;
    Result<void> SetCollisionResponseToChannel(CollisionChannel channel,
                                               CollisionResponse response) noexcept;
    Result<void> SetCollisionResponseToAllChannels(CollisionResponse response) noexcept;

    // 클래스 기본 profile 이름이다(UE 생성자의 SetCollisionProfileName — Primitive "BlockAll").
    virtual StringView DefaultCollisionProfileName() const noexcept {
        return StringView(PrimitiveCollisionProfileName);
    }

protected:
    // 바뀌면 대입하고 Render 다시 보내기를 표시한다.
    template <typename T>
    void Change(T& field, const T& value) noexcept {
        if (!(field == value)) {
            field = value;
            MarkRenderStateDirtyIfPresent();
        }
    }

    // 파생 생성자가 기본값을 바꾼다(UE 파생 생성자처럼 — Shape castDynamicShadow 등).
    ROCKS_PROPERTY(ReadOnly, Name = "minDrawDistance") float minDrawDistance_ = 0.0f;
    ROCKS_PROPERTY(ReadOnly, Name = "desiredMaxDrawDistance") float desiredMaxDrawDistance_ = 0.0f;
    ROCKS_PROPERTY(ReadOnly, Name = "neverDistanceCull") bool neverDistanceCull_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "allowCullDistanceVolume") bool allowCullDistanceVolume_ = true;
    ROCKS_PROPERTY(ReadOnly, Name = "castShadow") bool castShadow_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "castDynamicShadow") bool castDynamicShadow_ = true;
    ROCKS_PROPERTY(ReadOnly, Name = "castStaticShadow") bool castStaticShadow_ = true;
    ROCKS_PROPERTY(ReadOnly, Name = "castContactShadow") bool castContactShadow_ = true;
    ROCKS_PROPERTY(ReadOnly, Name = "castFarShadow") bool castFarShadow_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "castInsetShadow") bool castInsetShadow_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "castHiddenShadow") bool castHiddenShadow_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "castCinematicShadow") bool castCinematicShadow_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "castShadowAsTwoSided") bool castShadowAsTwoSided_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "selfShadowOnly") bool selfShadowOnly_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "castVolumetricTranslucentShadow")
    bool castVolumetricTranslucentShadow_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "receiveMobileCsmShadows") bool receiveMobileCsmShadows_ = true;
    ROCKS_PROPERTY(ReadOnly, Name = "singleSampleShadowFromStationaryLights")
    bool singleSampleShadowFromStationaryLights_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "affectDynamicIndirectLighting")
    bool affectDynamicIndirectLighting_ = true;
    ROCKS_PROPERTY(ReadOnly, Name = "affectDistanceFieldLighting")
    bool affectDistanceFieldLighting_ = true;
    ROCKS_PROPERTY(ReadOnly, Name = "receivesDecals") bool receivesDecals_ = true;
    ROCKS_PROPERTY(ReadOnly, Name = "renderInMainPass") bool renderInMainPass_ = true;
    ROCKS_PROPERTY(ReadOnly, Name = "renderInDepthPass") bool renderInDepthPass_ = true;
    ROCKS_PROPERTY(ReadOnly, Name = "visibleInReflectionCaptures")
    bool visibleInReflectionCaptures_ = true;
    ROCKS_PROPERTY(ReadOnly, Name = "visibleInRealTimeSkyCaptures")
    bool visibleInRealTimeSkyCaptures_ = true;
    ROCKS_PROPERTY(ReadOnly, Name = "visibleInSceneCaptureOnly")
    bool visibleInSceneCaptureOnly_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "hiddenInSceneCapture") bool hiddenInSceneCapture_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "ownerNoSee") bool ownerNoSee_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "onlyOwnerSee") bool onlyOwnerSee_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "holdout") bool holdout_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "treatAsBackgroundForOcclusion")
    bool treatAsBackgroundForOcclusion_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "useAsOccluder") bool useAsOccluder_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "renderCustomDepth") bool renderCustomDepth_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "customDepthStencilValue") int32 customDepthStencilValue_ = 0;
    ROCKS_PROPERTY(ReadOnly, Name = "customDepthStencilWriteMask")
    RendererStencilMask customDepthStencilWriteMask_ = RendererStencilMask::Default;
    ROCKS_PROPERTY(ReadOnly, Name = "translucencySortPriority") int32 translucencySortPriority_ = 0;
    ROCKS_PROPERTY(ReadOnly, Name = "translucencySortDistanceOffset")
    float translucencySortDistanceOffset_ = 0.0f;
    ROCKS_PROPERTY(ReadOnly, Name = "lightingChannels") rocks::LightingChannels lightingChannels_;
    ROCKS_PROPERTY(ReadOnly, Name = "boundsScale") float boundsScale_ = 1.0f;
    ROCKS_PROPERTY(ReadOnly, Name = "generateOverlapEvents") bool generateOverlapEvents_ = true;
    ROCKS_PROPERTY(ReadOnly, Name = "canCharacterStepUpOn")
    rocks::CanBeCharacterBase canCharacterStepUpOn_ = rocks::CanBeCharacterBase::Yes;

protected:
    // SetCollisionProfileName이 성공한 뒤 부른다(파생이 profile에 딸린 자기 상태를 고친다 — UE
    // UStaticMeshComponent::SetCollisionProfileName이 bUseDefaultCollision을 끄는 것, EngineRuntime
    // 30.3절). 기본은 아무것도 하지 않는다.
    virtual void OnCollisionProfileNameSet() noexcept {
    }

    // 등록 때 이름을 해석한다: profile 값이면 body에 옮기고, 표에 없으면 경고를 한 번 남긴다
    // (CollisionProfileMissing — CollisionData 9절 CD2). 파생이 재정의하면 이것을 먼저 부른다.
    Result<void> OnRegister() noexcept override;

    // 물리 시스템 상태(2.3절): World에 장면이 있고 형상이 있으며 실효 충돌 켬이 NoCollision이
    // 아니면 body를 만들고, 등록 해제 때 지운다. 다른 종류는 기반 동작이다. 파생이 재정의하면
    // 이것을 부른다.
    bool WantsSystemState(SystemStateKind kind) const noexcept override;
    Result<void> CreateSystemState(SystemStateKind kind) noexcept override;
    void DestroySystemState(SystemStateKind kind) noexcept override;
    // 물리 상태가 확정되면 componentPhysicsStateChanged를 낸다(7.1.19절). 파생이 재정의하면 이것을
    // 부른다.
    void OnSystemStateChanged(SystemStateKind kind, bool created) noexcept override;

    // body가 있으면 월드 transform으로 옮긴다(3.3절 — UE OnUpdateTransform → SendPhysicsTransform).
    // scale이 바뀌었으면 장면이 형상을 다시 만든다(7.1.5절). 파생이 재정의하면 이것을 부른다.
    void OnTransformUpdated() noexcept override;

    // 시작할 때 simulatePhysics이고 부모가 있으면 월드 transform을 유지하며 뗀다(UE
    // UPrimitiveComponent::BeginPlay — PhysicsModule 2.3.1절). 파생이 재정의하면 이것을 부른다.
    void OnStart() noexcept override;

    // 파괴될 때 generateOverlapEvents를 끄고 겹침을 모두 끝낸다(알림 있음 — UE
    // OnComponentDestroyed, 7.1.11절). 파생이 재정의하면 이것을 부른다.
    void OnDestroy(EndPlayReason reason) noexcept override;

    // 부모에서 떼기 전 weld를 푼다(UE DetachFromComponent → UnWeldFromParent — 7.1.23절).
    void OnDetachingFromParent() noexcept override;

    // weldSimulatedBodies 부착이면 weld한다(UE AttachToComponent — 7.1.23절).
    void OnAttachedToParent(const AttachmentRules& rules) noexcept override;

    // 형상 값이 바뀐 뒤 파생 setter가 부른다(UE UpdateBodyScale(scale, true) — 7.1.5절). weld면 새
    // 형상으로 다시 weld한다(7.1.23절). body가
    // 있으면 BuildBodyGeometry의 새 값으로 형상을 바꾼다. 실패하면 이전 형상을 두고 Warning
    // BodyShapeUpdateFailed다. 그다음 overlap을 갱신한다(UE 형상 setter의 bUpdateOverlaps —
    // 7.1.11절).
    void UpdateBodyGeometry() noexcept;

private:
    // weld(7.1.23절)다. WeldToImplementation은 UE 같은 이름(부모가 지금 부모일 때 루트를 찾아 합침
    // — 합쳤으면 참). WeldInto는 root(그 body·월드 transform)에 바로 합친다(자기 body를 지움).
    // 실패하면 Warning WeldFailed와 자기 body를 다시 만든다. UnWeldInternal은 recreateBody면 자기
    // body를 만든다(상태 파괴 중에는 거짓).
    bool WeldToImplementation(ObjectHandle parent, bool weldSimulatedChild,
                              bool weldToKinematicParent) noexcept;
    bool WeldInto(ObjectHandle root, PhysicsBodyHandle rootBody,
                  const Transform& rootWorld) noexcept;
    void UnWeldInternal(bool recreateBody) noexcept;
    // 지금 루트에 지금 형상·자리로 다시 합친다(UE SetWeldedBodyTransform — 7.1.23절).
    void ReweldToRoot() noexcept;
    // 자기 body가 있고 Dynamic이면 weld됐거나 autoWeld인 자식을 따라 물리 충돌이 있는 자손을 합친다
    // (UE ApplyWeldOnChildren — 대상을 먼저 모은다).
    void ApplyWeldOnChildren() noexcept;
    // weld됐거나 autoWeld인 자식을 따라 내려가며 targets 끝에 더한다(UE GetWeldedBodies(…, true)).
    void CollectWeldTargets(Array<ObjectHandle>& targets) noexcept;
    // 내 body에 weld된 자손을 모두 푼다(상태 파괴 — body 없는 중간 컴포넌트를 거친 자손 포함).
    void UnWeldFromThis() noexcept;
    // 자기 body를 다시 만들어도 되는지다(등록·물리 상태 있음, 파괴 대기·World 정리 아님).
    bool CanRecreateBody() const noexcept;
    // weld가 아니고 body가 없으면 자기 body를 만든다(실패는 Warning WeldFailed).
    void EnsureOwnBody() noexcept;
    // 자기 body를 만든다(CreateSystemState의 body 몫).
    Result<void> CreateOwnBody() noexcept;
    // weld면 루트 컴포넌트로 call(PrimitiveComponent&)을 부르고 참이다(루트를 빌리지 못하면 call
    // 없이 참). weld가 아니면 거짓이다.
    template <class Call>
    bool ForwardToWeldRoot(Call&& call) const noexcept;

    // 막는 hit를 자기 Actor와 상대 Actor에 발행한다(UE DispatchBlockingHit — 7.1.10절).
    void DispatchBlockingHit(const HitResult& hit) noexcept;

    // 시작할 겹침 하나다(UE FOverlapInfo — 7.1.14절): 상대 컴포넌트와 그 hit(sweep이면 sweep
    // hit, 아니면 상대 컴포넌트·Actor만 채운 hit).
    struct PendingOverlap {
        ObjectHandle component;
        bool fromSweep = false;
        HitResult hit;
    };

    // overlap 장부(7.1.11절 — UE UpdateOverlapsImpl·BeginComponentOverlap·EndComponentOverlap·
    // ClearComponentOverlaps). pending은 이동 sweep이 지나며 만난 겹침(시작할 후보)이다.
    void UpdateOverlapsWith(Span<const PendingOverlap> pending) noexcept;
    void RefreshOverlaps(World& world) noexcept;
    void BeginComponentOverlap(const PendingOverlap& overlap) noexcept;
    void EndComponentOverlap(ObjectHandle other) noexcept;
    void ClearComponentOverlaps() noexcept;
    // 두 컴포넌트가 겹침을 만들 수 있는지다(UE CanComponentsGenerateOverlap).
    bool CanGenerateOverlapWith(const PrimitiveComponent& other) const noexcept;
    // 질의·sweep 결과를 장부 후보로 삼을지다(UE ShouldIgnoreOverlapResult의 반대 — 자기 아님, 상대
    // generateOverlapEvents, 상대 Actor가 있고 Constructed가 아님).
    bool IsOverlapCandidate(World& world, ObjectHandle component,
                            ObjectHandle actor) const noexcept;

    // body에 넘기는 월드 transform(위치·회전·scale 따로 — 행렬 분해 없음)이다.
    Transform BodyWorldTransform() const noexcept;

    // 힘·충격량을 body에 넘긴다(body·장면이 없으면 무시 — 7.1.17절).
    void ApplyBodyForce(const BodyForce& force) noexcept;

    // 바뀐 body 값 묶음을 body에 넣는다(body·장면이 없으면 무시 — 7.1.18절).
    void PushBodySettings(BodySettingsChange change) noexcept;

    // body 운동 상태를 읽는다(body·장면이 없으면 거짓).
    bool ReadBodyKinematics(BodyKinematics& out) const noexcept;

    // 잘못된 body 값을 경고한다(InvalidBodyValue — 7.1.18절).
    void WarnInvalidBodyValue(const char* property) const noexcept;

    // 잘못된 물리 호출을 경고한다(UE WarnInvalidPhysicsOperations — 7.1.17절). checkBody가 거짓이면
    // Mobility만 본다(UE 속도 setter).
    void WarnInvalidPhysicsOperation(const char* action, bool checkBody) const noexcept;

    // 충돌 값이 바뀐 뒤 부른다(2.3.1절): NoCollision 경계를 넘으면 body를 그 자리에서 만들거나
    // 지우고 (RecreateSystemStateNow), 아니면 body 필터만 바꾼다.
    void UpdateBodyCollision() noexcept;

    // 실효 값을 편집해 바뀌면 "Custom"으로 옮긴다(위 네 setter의 공통 경로).
    template <class Edit>
    Result<void> EditCustomCollision(Edit&& edit) noexcept;

    // 빈 값 = 클래스 기본(DefaultCollisionProfileName).
    ROCKS_PROPERTY(ReadOnly, Name = "collisionProfileName") String collisionProfileName_;
    ROCKS_PROPERTY(ReadOnly, Name = "bodySettings") BodySettings bodySettings_;
    ROCKS_PROPERTY(ReadOnly, Name = "ignoreRadialImpulse") bool ignoreRadialImpulse_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "ignoreRadialForce") bool ignoreRadialForce_ = false;
    // 물리 body다(실행 상태 — 저장하지 않는다).
    ROCKS_NATIVE() PhysicsBodyHandle bodyHandle_;
    // weld 루트 컴포넌트와 그 body다(7.1.23절 — 실행 상태).
    ROCKS_NATIVE() ObjectHandle weldRoot_;
    ROCKS_NATIVE() PhysicsBodyHandle weldBody_;
    // ApplyPhysicsTransform 안이다(OnTransformUpdated가 body로 되돌려 보내지 않는다).
    ROCKS_NATIVE() bool applyingPhysicsResult_ = false;
    // 이동 sweep 무시 목록이다(UE MoveIgnoreActors·MoveIgnoreComponents — Transient).
    ROCKS_NATIVE() Array<ObjectHandle> moveIgnoreActors_;
    ROCKS_NATIVE() Array<ObjectHandle> moveIgnoreComponents_;
    // 겹친 컴포넌트다(UE OverlappingComponents — Transient, 대칭).
    ROCKS_NATIVE() Array<ObjectHandle> overlappingComponents_;
    // OnDestroy가 겹침을 모두 끝내는 중이다(7.1.14절): 파괴 대기지만 자기 쪽·자기 Actor도 끝 알림을
    // 받는다(UE는 MarkAsGarbage 전에 끝낸다 — ActorComponent.cpp:2201-2204).
    ROCKS_NATIVE() bool clearingForDestroy_ = false;
};

} // namespace rocks
