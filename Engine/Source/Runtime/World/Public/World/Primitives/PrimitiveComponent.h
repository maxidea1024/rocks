#pragma once

#include <Collision/BodySettings.h>
#include <Collision/CollisionProfileTable.h>
#include <Collision/CollisionTypes.h>
#include <Core/Text/String.h>
#include <Meta/Annotations.h>
#include <World/Lights/LightingChannels.h>
#include <World/SceneComponent.h>
#include <World/Primitives/PrimitiveTypes.h>

namespace rocks {

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
        : SceneComponent(allocator), collisionProfileName_(allocator), bodySettings_(allocator) {
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
    // 등록 때 이름을 해석한다: profile 값이면 body에 옮기고, 표에 없으면 경고를 한 번 남긴다
    // (CollisionProfileMissing — CollisionData 9절 CD2). 파생이 재정의하면 이것을 먼저 부른다.
    Result<void> OnRegister() noexcept override;

private:
    // 실효 값을 편집해 바뀌면 "Custom"으로 옮긴다(위 네 setter의 공통 경로).
    template <class Edit>
    Result<void> EditCustomCollision(Edit&& edit) noexcept;

    // 빈 값 = 클래스 기본(DefaultCollisionProfileName).
    ROCKS_PROPERTY(ReadOnly, Name = "collisionProfileName") String collisionProfileName_;
    ROCKS_PROPERTY(ReadOnly, Name = "bodySettings") BodySettings bodySettings_;
};

} // namespace rocks
