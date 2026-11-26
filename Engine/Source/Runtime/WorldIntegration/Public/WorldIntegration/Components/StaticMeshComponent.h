#pragma once

#include <Core/Math/Color.h>
#include <Mesh/StaticMesh/StaticMesh.h>
#include <Meta/Annotations.h>
#include <Object/SoftObjectRef.h>
#include <WorldIntegration/Components/MeshComponent.h>

namespace rocks {

// 정적 메시를 그리는 컴포넌트다(UE UStaticMeshComponent — Docs/Design/EngineRuntime 30.3절).
// 데이터만 있다: 메시 로드·렌더 proxy·충돌은 나중이다(30.6절). 메시 참조는
// SoftObjectRef<StaticMesh>다.
//
// 로드 정보: 메시 로드 시스템이 lease를 얻은 뒤 ApplyLoadedMeshInfo로 확장 bounds와 슬롯 수만
// 값으로 복사한다(에셋 객체를 쥐지 않는다 — Registry 수명이 다르다). 저장하지 않는 런타임 상태이고
// SetStaticMesh가 바뀌면 지운다. 정보가 없으면 bounds는 위치의 점이다(UE CalcBounds — 메시 없음).
//
// 규약: setter는 값 검증 실패면 InvalidArgument이고 값을 바꾸지 않는다. Mobility는 SetStaticMesh만
// 본다(UE SetStaticMesh — 게임 초기화 뒤 Static이면 거부, 진단 StaticComponentDataChanged). 바뀌면
// Render 다시 보내기를 표시한다.
//
// 사용 예:
//   auto mesh = world.AddComponent<StaticMeshComponent>(actor);
//   world.With<StaticMeshComponent>(mesh.Value(), [&](StaticMeshComponent& component) noexcept {
//       SoftObjectRef<StaticMesh> chair(allocator);
//       (void)chair.TryAssign(chairAsset, {});
//       return component.SetStaticMesh(chair);
//   });
//
// 스레드: World의 게임 스레드 전용이다.
ROCKS_CLASS()
class ROCKS_WORLDINTEGRATION_API StaticMeshComponent : public MeshComponent {
    ROCKS_GENERATED_BODY()

public:
    // 클래스 기본 충돌 profile(UE StaticMeshComponent.cpp:206 BlockAllDynamic)이다.
    static constexpr const char* StaticMeshCollisionProfileName = "BlockAllDynamic";
    // UE 생성자 기본값(StaticMeshComponent.cpp:208–231)이다.
    static constexpr Color DefaultWireframeColor = Color(255, 255, 255, 255);
    static constexpr int32 DefaultOverriddenLightMapRes = 64;
    static constexpr int32 DefaultSubDivisionStepSize = 32;
    // UE 편집 뒤 clamp(StaticMeshComponent.cpp:1956·2019, ClampMax 4096)를 Rocks는 거부 조건으로
    // 둔다 (24.2.3절): lightmap 해상도는 [4, 4096]의 4의 배수, 세분 step은 [1, 128]이다.
    static constexpr int32 MinOverriddenLightMapRes = 4;
    static constexpr int32 MaxOverriddenLightMapRes = 4096;
    static constexpr int32 OverriddenLightMapResAlignment = 4;
    static constexpr int32 MinSubDivisionStepSize = 1;
    static constexpr int32 MaxSubDivisionStepSize = 128;
    static constexpr float DefaultDistanceFieldIndirectShadowMinVisibility = 0.1f;

    explicit StaticMeshComponent(Allocator& allocator) noexcept
        : MeshComponent(allocator), staticMesh_(allocator) {
    }

    StringView DefaultCollisionProfileName() const noexcept override {
        return StringView(StaticMeshCollisionProfileName);
    }

    // --- 메시 ---

    const SoftObjectRef<StaticMesh>& GetStaticMesh() const noexcept {
        return staticMesh_;
    }

    // 메시를 바꾼다(같은 값이면 아무것도 하지 않는다). 바뀌면 로드 정보를 지우고 bounds를
    // 무효화한다. 오류: 게임 초기화 뒤 Static(InvalidArgument + StaticComponentDataChanged), 잘못된
    // soft 참조 값, 할당 실패.
    Result<void> SetStaticMesh(const SoftObjectRef<StaticMesh>& mesh) noexcept;

    // 로드된 메시의 확장 bounds(StaticMeshExtendedBounds)와 슬롯 수를 복사한다(30.3절). 메시 로드
    // 시스템이 lease를 얻은 뒤 요청에 쓴 참조(requested)와 함께 부른다. mesh를 쥐지 않는다. 오류:
    // requested가 null이거나 지금 staticMesh와 다름(InvalidArgument — 늦게 끝난 옛 요청, 값 유지).
    Result<void> ApplyLoadedMeshInfo(const SoftObjectRef<StaticMesh>& requested,
                                     const StaticMesh& mesh) noexcept;

    // 로드 정보를 지운다(bounds 무효화·Render 다시 보내기). 메시를 놓을 때 로드 시스템이 부른다.
    void ClearLoadedMeshInfo() noexcept;

    bool HasLoadedMeshInfo() const noexcept {
        return loadedMeshInfo_;
    }

    usize LoadedMeshMaterialSlotCount() const noexcept {
        return loadedMeshSlotCount_;
    }

    // 로드 정보가 있으면 메시 슬롯 수, 없으면 0이다(UE GetNumMaterials — 메시가 없으면 0).
    usize NumMaterials() const noexcept override {
        return loadedMeshInfo_ ? loadedMeshSlotCount_ : 0;
    }

    // --- LOD ---

    int32 ForcedLodModel() const noexcept {
        return forcedLodModel_;
    }

    // 0 = 자동, n ≥ 1 = LOD n − 1 고정. 오류: 음수. minLod도 같다.
    Result<void> SetForcedLodModel(int32 model) noexcept;

    int32 MinLod() const noexcept {
        return minLod_;
    }

    Result<void> SetMinLod(int32 lod) noexcept;

    bool OverridesMinLod() const noexcept {
        return overrideMinLod_;
    }

    void SetOverrideMinLod(bool enabled) noexcept {
        Change(overrideMinLod_, enabled);
    }

    // --- wireframe ---

    Color WireframeColorOverride() const noexcept {
        return wireframeColorOverride_;
    }

    void SetWireframeColorOverride(Color color) noexcept {
        Change(wireframeColorOverride_, color);
    }

    bool OverridesWireframeColor() const noexcept {
        return overrideWireframeColor_;
    }

    void SetOverrideWireframeColor(bool enabled) noexcept {
        Change(overrideWireframeColor_, enabled);
    }

    // --- World Position Offset ---

    int32 WorldPositionOffsetDisableDistance() const noexcept {
        return worldPositionOffsetDisableDistance_;
    }

    // 0 = 끄지 않음(cm). 오류: 음수.
    Result<void> SetWorldPositionOffsetDisableDistance(int32 distance) noexcept;

    bool EvaluatesWorldPositionOffset() const noexcept {
        return evaluateWorldPositionOffset_;
    }

    void SetEvaluateWorldPositionOffset(bool evaluate) noexcept {
        Change(evaluateWorldPositionOffset_, evaluate);
    }

    bool WorldPositionOffsetWritesVelocity() const noexcept {
        return worldPositionOffsetWritesVelocity_;
    }

    void SetWorldPositionOffsetWritesVelocity(bool writes) noexcept {
        Change(worldPositionOffsetWritesVelocity_, writes);
    }

    bool EvaluatesWorldPositionOffsetInRayTracing() const noexcept {
        return evaluateWorldPositionOffsetInRayTracing_;
    }

    void SetEvaluateWorldPositionOffsetInRayTracing(bool evaluate) noexcept {
        Change(evaluateWorldPositionOffsetInRayTracing_, evaluate);
    }

    // --- 가상 기하(Nanite) ---

    bool ForcesNaniteForMasked() const noexcept {
        return forceNaniteForMasked_;
    }

    void SetForceNaniteForMasked(bool force) noexcept {
        Change(forceNaniteForMasked_, force);
    }

    bool DisallowsNanite() const noexcept {
        return disallowNanite_;
    }

    void SetDisallowNanite(bool disallow) noexcept {
        Change(disallowNanite_, disallow);
    }

    bool ForcesDisableNanite() const noexcept {
        return forceDisableNanite_;
    }

    void SetForceDisableNanite(bool disable) noexcept {
        Change(forceDisableNanite_, disable);
    }

    float NanitePixelProgrammableDistance() const noexcept {
        return nanitePixelProgrammableDistance_;
    }

    // 0 = 끄지 않음(cm). 오류: 음수·비유한.
    Result<void> SetNanitePixelProgrammableDistance(float distance) noexcept;

    // --- 조명 ---

    bool OverridesLightMapRes() const noexcept {
        return overrideLightMapRes_;
    }

    void SetOverrideLightMapRes(bool enabled) noexcept {
        Change(overrideLightMapRes_, enabled);
    }

    int32 OverriddenLightMapRes() const noexcept {
        return overriddenLightMapRes_;
    }

    // 오류: [4, 4096] 밖·4의 배수가 아님(UE는 4의 배수로 올리고 자른다).
    Result<void> SetOverriddenLightMapRes(int32 resolution) noexcept;

    bool CastsDistanceFieldIndirectShadow() const noexcept {
        return castDistanceFieldIndirectShadow_;
    }

    void SetCastDistanceFieldIndirectShadow(bool cast) noexcept {
        Change(castDistanceFieldIndirectShadow_, cast);
    }

    float DistanceFieldIndirectShadowMinVisibility() const noexcept {
        return distanceFieldIndirectShadowMinVisibility_;
    }

    // 오류: [0, 1] 밖·비유한(UE는 UIMin·UIMax이고 proxy가 [0, 1]로 자른다 — StaticMeshSceneProxy).
    Result<void> SetDistanceFieldIndirectShadowMinVisibility(float visibility) noexcept;

    bool OverridesDistanceFieldSelfShadowBias() const noexcept {
        return overrideDistanceFieldSelfShadowBias_;
    }

    void SetOverrideDistanceFieldSelfShadowBias(bool enabled) noexcept {
        Change(overrideDistanceFieldSelfShadowBias_, enabled);
    }

    float DistanceFieldSelfShadowBias() const noexcept {
        return distanceFieldSelfShadowBias_;
    }

    // 오류: 음수·비유한(UE는 proxy가 0 이상으로 자른다 — StaticMeshSceneProxy).
    Result<void> SetDistanceFieldSelfShadowBias(float bias) noexcept;

    bool UsesSubDivisions() const noexcept {
        return useSubDivisions_;
    }

    void SetUseSubDivisions(bool use) noexcept {
        Change(useSubDivisions_, use);
    }

    int32 SubDivisionStepSize() const noexcept {
        return subDivisionStepSize_;
    }

    // 오류: [1, 128] 밖(UE 편집 뒤 clamp).
    Result<void> SetSubDivisionStepSize(int32 step) noexcept;

    // --- 충돌·그리기 ---

    bool UsesDefaultCollision() const noexcept {
        return useDefaultCollision_;
    }

    // 메시 BodySetup의 기본 충돌을 쓴다(충돌 데이터 — Render 다시 보내기를 표시하지 않는다).
    // SetCollisionProfileName이 끈다(UE StaticMeshComponent.cpp:2621).
    void SetUseDefaultCollision(bool use) noexcept {
        useDefaultCollision_ = use;
    }

    bool SortsTriangles() const noexcept {
        return sortTriangles_;
    }

    void SetSortTriangles(bool sort) noexcept {
        Change(sortTriangles_, sort);
    }

    bool ReversesCulling() const noexcept {
        return reverseCulling_;
    }

    void SetReverseCulling(bool reverse) noexcept {
        Change(reverseCulling_, reverse);
    }

    // 지역 bounds를 Transform 행렬로 옮긴다(UE CalcBounds(LocalToWorld) — 정보가 없으면 그 위치의
    // 점).
    ComponentBounds CalcBounds(const Transform& world) const noexcept override;

    // --- 텍스처 스트리밍 ---

    bool IgnoresInstanceForTextureStreaming() const noexcept {
        return ignoreInstanceForTextureStreaming_;
    }

    void SetIgnoreInstanceForTextureStreaming(bool ignore) noexcept {
        Change(ignoreInstanceForTextureStreaming_, ignore);
    }

    float StreamingDistanceMultiplier() const noexcept {
        return streamingDistanceMultiplier_;
    }

    // 오류: 음수·비유한(UE ClampMin 0).
    Result<void> SetStreamingDistanceMultiplier(float multiplier) noexcept;

protected:
    // 로드 정보가 있으면 메시 슬롯 수, 없으면 메시 슬롯 상한(30.2절 ERD62).
    usize SlotOverlayLimit() const noexcept override {
        return loadedMeshInfo_ ? loadedMeshSlotCount_ : static_cast<usize>(MaxMaterialSlots);
    }

    // 로드 정보의 확장 bounds × boundsScale이다(UE CalcBounds — StaticMeshComponent.cpp:662).
    // 정보가 없으면 거짓(위치의 점).
    bool TryLocalBounds(ComponentBounds& local) const noexcept override;

    // profile을 바꾸면 메시 기본 충돌을 끈다(UE SetCollisionProfileName — :2621).
    void OnCollisionProfileNameSet() noexcept override {
        useDefaultCollision_ = false;
    }

private:
    ROCKS_PROPERTY(ReadOnly, Name = "staticMesh") SoftObjectRef<StaticMesh> staticMesh_;
    ROCKS_PROPERTY(ReadOnly, Name = "forcedLodModel") int32 forcedLodModel_ = 0;
    ROCKS_PROPERTY(ReadOnly, Name = "minLod") int32 minLod_ = 0;
    ROCKS_PROPERTY(ReadOnly, Name = "overrideMinLod") bool overrideMinLod_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "wireframeColorOverride")
    Color wireframeColorOverride_ = DefaultWireframeColor;
    ROCKS_PROPERTY(ReadOnly, Name = "overrideWireframeColor") bool overrideWireframeColor_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "worldPositionOffsetDisableDistance")
    int32 worldPositionOffsetDisableDistance_ = 0;
    ROCKS_PROPERTY(ReadOnly, Name = "evaluateWorldPositionOffset")
    bool evaluateWorldPositionOffset_ = true;
    ROCKS_PROPERTY(ReadOnly, Name = "worldPositionOffsetWritesVelocity")
    bool worldPositionOffsetWritesVelocity_ = true;
    ROCKS_PROPERTY(ReadOnly, Name = "evaluateWorldPositionOffsetInRayTracing")
    bool evaluateWorldPositionOffsetInRayTracing_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "forceNaniteForMasked") bool forceNaniteForMasked_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "disallowNanite") bool disallowNanite_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "forceDisableNanite") bool forceDisableNanite_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "nanitePixelProgrammableDistance")
    float nanitePixelProgrammableDistance_ = 0.0f;
    ROCKS_PROPERTY(ReadOnly, Name = "overrideLightMapRes") bool overrideLightMapRes_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "overriddenLightMapRes")
    int32 overriddenLightMapRes_ = DefaultOverriddenLightMapRes;
    ROCKS_PROPERTY(ReadOnly, Name = "castDistanceFieldIndirectShadow")
    bool castDistanceFieldIndirectShadow_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "distanceFieldIndirectShadowMinVisibility")
    float distanceFieldIndirectShadowMinVisibility_ =
        DefaultDistanceFieldIndirectShadowMinVisibility;
    ROCKS_PROPERTY(ReadOnly, Name = "overrideDistanceFieldSelfShadowBias")
    bool overrideDistanceFieldSelfShadowBias_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "distanceFieldSelfShadowBias")
    float distanceFieldSelfShadowBias_ = 0.0f;
    ROCKS_PROPERTY(ReadOnly, Name = "useSubDivisions") bool useSubDivisions_ = true;
    ROCKS_PROPERTY(ReadOnly, Name = "subDivisionStepSize")
    int32 subDivisionStepSize_ = DefaultSubDivisionStepSize;
    ROCKS_PROPERTY(ReadOnly, Name = "useDefaultCollision") bool useDefaultCollision_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "sortTriangles") bool sortTriangles_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "reverseCulling") bool reverseCulling_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "ignoreInstanceForTextureStreaming")
    bool ignoreInstanceForTextureStreaming_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "streamingDistanceMultiplier")
    float streamingDistanceMultiplier_ = 1.0f;

    // 로드 정보(저장하지 않는다 — 30.3절).
    ROCKS_NATIVE() bool loadedMeshInfo_ = false;
    ROCKS_NATIVE() usize loadedMeshSlotCount_ = 0;
    ROCKS_NATIVE() ComponentBounds loadedMeshBounds_ {};
};

} // namespace rocks
