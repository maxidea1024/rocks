#pragma once

#include <Collision/BodySettings.h>
#include <Collision/CollisionProfileTable.h>
#include <Collision/CollisionTypes.h>
#include <Core/Text/String.h>
#include <Meta/Annotations.h>
#include <World/Lights/LightingChannels.h>
#include <World/Physics/BodyGeometry.h>
#include <World/Physics/BodyMotion.h>
#include <World/Physics/HitResult.h>
#include <World/Physics/MoveOptions.h>
#include <World/Physics/MtdResult.h>
#include <World/Physics/PhysicsBodyHandle.h>
#include <World/Physics/PhysicsContactHit.h>
#include <World/SceneComponent.h>
#include <World/Primitives/PrimitiveTypes.h>

namespace rocks {

// 질의 형상이다(Collision/CollisionShape.h — 헤더는 구현 파일만 include한다: Mesh
// SimpleCollision.h의 같은 이름 enum CollisionShapeKind와 한 번역 단위에서 만나지 않게).
struct CollisionShape;

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
        : SceneComponent(allocator), collisionProfileName_(allocator), bodySettings_(allocator),
          moveIgnoreActors_(allocator), moveIgnoreComponents_(allocator),
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

    // body 형상(scale 전 cm)이다. 형상이 있는 파생(Box·Sphere·Capsule)이 채우고 참을 돌려준다.
    // 기본은 형상 없음(거짓 — body를 만들지 않는다).
    virtual bool BuildBodyGeometry(BodyGeometry& geometry) const noexcept;

    // 물리 body handle이다. World에 물리 장면이 있고 형상과 충돌이 있으면 등록 때 생기고 등록 해제
    // 때 없어진다(없으면 IsNull).
    PhysicsBodyHandle BodyHandle() const noexcept {
        return bodyHandle_;
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

    // body가 있으면 월드 transform으로 옮긴다(3.3절 — UE OnUpdateTransform → SendPhysicsTransform).
    // scale이 바뀌었으면 장면이 형상을 다시 만든다(7.1.5절). 파생이 재정의하면 이것을 부른다.
    void OnTransformUpdated() noexcept override;

    // 시작할 때 simulatePhysics이고 부모가 있으면 월드 transform을 유지하며 뗀다(UE
    // UPrimitiveComponent::BeginPlay — PhysicsModule 2.3.1절). 파생이 재정의하면 이것을 부른다.
    void OnStart() noexcept override;

    // 파괴될 때 generateOverlapEvents를 끄고 겹침을 모두 끝낸다(알림 있음 — UE
    // OnComponentDestroyed, 7.1.11절). 파생이 재정의하면 이것을 부른다.
    void OnDestroy(EndPlayReason reason) noexcept override;

    // 형상 값이 바뀐 뒤 파생 setter가 부른다(UE UpdateBodyScale(scale, true) — 7.1.5절). body가
    // 있으면 BuildBodyGeometry의 새 값으로 형상을 바꾼다. 실패하면 이전 형상을 두고 Warning
    // BodyShapeUpdateFailed다. 그다음 overlap을 갱신한다(UE 형상 setter의 bUpdateOverlaps —
    // 7.1.11절).
    void UpdateBodyGeometry() noexcept;

private:
    // 막는 hit를 자기 Actor와 상대 Actor에 발행한다(UE DispatchBlockingHit — 7.1.10절).
    void DispatchBlockingHit(const HitResult& hit) noexcept;

    // overlap 장부(7.1.11절 — UE UpdateOverlapsImpl·BeginComponentOverlap·EndComponentOverlap·
    // ClearComponentOverlaps). pending은 이동 sweep이 지나며 만난 겹침(시작할 후보)이다.
    void UpdateOverlapsWith(Span<const ObjectHandle> pending) noexcept;
    void RefreshOverlaps(World& world) noexcept;
    void BeginComponentOverlap(ObjectHandle other) noexcept;
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

    // 충돌 값이 바뀐 뒤 부른다(2.3.1절): NoCollision 경계를 넘으면 body를 그 자리에서 만들거나
    // 지우고 (RecreateSystemStateNow), 아니면 body 필터만 바꾼다.
    void UpdateBodyCollision() noexcept;

    // 실효 값을 편집해 바뀌면 "Custom"으로 옮긴다(위 네 setter의 공통 경로).
    template <class Edit>
    Result<void> EditCustomCollision(Edit&& edit) noexcept;

    // 빈 값 = 클래스 기본(DefaultCollisionProfileName).
    ROCKS_PROPERTY(ReadOnly, Name = "collisionProfileName") String collisionProfileName_;
    ROCKS_PROPERTY(ReadOnly, Name = "bodySettings") BodySettings bodySettings_;
    // 물리 body다(실행 상태 — 저장하지 않는다).
    ROCKS_NATIVE() PhysicsBodyHandle bodyHandle_;
    // ApplyPhysicsTransform 안이다(OnTransformUpdated가 body로 되돌려 보내지 않는다).
    ROCKS_NATIVE() bool applyingPhysicsResult_ = false;
    // 이동 sweep 무시 목록이다(UE MoveIgnoreActors·MoveIgnoreComponents — Transient).
    ROCKS_NATIVE() Array<ObjectHandle> moveIgnoreActors_;
    ROCKS_NATIVE() Array<ObjectHandle> moveIgnoreComponents_;
    // 겹친 컴포넌트다(UE OverlappingComponents — Transient, 대칭).
    ROCKS_NATIVE() Array<ObjectHandle> overlappingComponents_;
};

} // namespace rocks
