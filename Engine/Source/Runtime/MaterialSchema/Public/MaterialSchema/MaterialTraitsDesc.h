#pragma once

#include <MaterialSchema/MaterialDisplacementTraits.h>
#include <MaterialSchema/MaterialForwardTraits.h>
#include <MaterialSchema/MaterialPostProcessTraits.h>
#include <MaterialSchema/MaterialTraitEnums.h>
#include <MaterialSchema/MaterialTranslucencyTraits.h>
#include <Meta/Annotations.h>
#include <RenderData/MaterialEnums.h>

// 머티리얼 기본 속성(traits)이다(Docs/Design/MaterialSystem 4.2·4.6·19.9절). UE UMaterial의 그래프
// 밖 속성 가운데 19.9절에서 "필드"인 것 전부다. 번역(MATERIAL_* 정의)·셰이더
// 특성(MaterialShaderTraits)· 렌더 상태가 읽는다. 기본값은 UE UMaterial
// 생성자(RE/Private/Materials/Material.cpp:1137–)와 헤더 초기값과 같다. 이름은 camelCase이고 bool에
// b 접두를 쓰지 않는다.
//
// 사용 예:
//   MaterialTraitsDesc traits;
//   traits.blendMode = BlendMode::Masked;
//   traits.opacityMaskClipValue = 0.5f;
namespace rocks::material {

// UE OpacityMaskClipValue 기본값이다.
inline constexpr float DefaultOpacityMaskClipValue = 0.3333f;

ROCKS_STRUCT()
struct MaterialTraitsDesc {
    ROCKS_GENERATED_BODY()

    // 영역·블렌드 모드(Renderer 14.2절 번호표).
    ROCKS_PROPERTY() MaterialDomain domain = MaterialDomain::Surface;
    ROCKS_PROPERTY() BlendMode blendMode = BlendMode::Opaque;
    ROCKS_PROPERTY() float opacityMaskClipValue = DefaultOpacityMaskClipValue;
    ROCKS_PROPERTY() bool twoSided = false;
    // Substrate thin 표면(UE bIsThinSurface).
    ROCKS_PROPERTY() bool thinSurface = false;
    // 출력 노드 Attributes 핀을 쓴다(UE bUseMaterialAttributes, 5.6절).
    ROCKS_PROPERTY() bool useMaterialAttributes = false;
    // 노멀 입력이 tangent 공간이다(UE bTangentSpaceNormal).
    ROCKS_PROPERTY() bool tangentSpaceNormal = true;
    ROCKS_PROPERTY() bool allowNegativeEmissive = false;
    ROCKS_PROPERTY() bool fullyRough = false;
    ROCKS_PROPERTY() bool normalCurvatureToRoughness = false;
    ROCKS_PROPERTY() bool roughnessTracking = true;
    // 엔진 기본 머티리얼(11.3절)이라 모든 사용 대상을 쿠킹한다(UE bUsedAsSpecialEngineMaterial).
    ROCKS_PROPERTY() bool engineDefaultMaterial = false;
    // Masked를 불투명으로 가정해도 되는가(UE bCanMaskedBeAssumedOpaque).
    ROCKS_PROPERTY() bool maskedAssumedOpaque = false;
    ROCKS_PROPERTY() bool ditherOpacityMask = false;
    ROCKS_PROPERTY() bool ditheredLodTransition = false;
    ROCKS_PROPERTY() bool castDynamicShadowAsMasked = false;
    ROCKS_PROPERTY() bool castRayTracedShadows = true;
    // 머티리얼 정점 입력 UV 채널 수(UE NumCustomizedUVs, 0–8).
    ROCKS_PROPERTY() uint8 customizedUvCount = 0;
    ROCKS_PROPERTY()
    PixelDepthOffsetMode pixelDepthOffsetMode = PixelDepthOffsetMode::AlongCameraVector;
    // WPO 상한(cm, 0 = 상한 없음)과 primitive 설정과 무관한 평가(UE
    // MaxWorldPositionOffsetDisplacement· bAlwaysEvaluateWorldPositionOffset).
    ROCKS_PROPERTY() float maxWorldPositionOffset = 0.0f;
    ROCKS_PROPERTY() bool alwaysEvaluateWorldPositionOffset = false;
    ROCKS_PROPERTY() bool hasPixelAnimation = false;
    ROCKS_PROPERTY() bool outputTranslucentVelocity = false;
    ROCKS_PROPERTY() bool translucencyVelocityFromDepth = false;
    ROCKS_PROPERTY() DecalResponse decalResponse = DecalResponse::ColorNormalRoughness;
    ROCKS_PROPERTY() bool responsiveAntialiasing = false;
    ROCKS_PROPERTY() bool screenSpaceReflections = false;
    ROCKS_PROPERTY() bool contactShadows = false;
    ROCKS_PROPERTY() bool disableDepthTest = false;
    ROCKS_PROPERTY() bool writeOnlyAlpha = false;
    ROCKS_PROPERTY() bool sphericalParticleNormals = false;
    ROCKS_PROPERTY() bool emissiveForAreaLighting = false;
    ROCKS_PROPERTY() bool lightmapDirectionality = true;
    ROCKS_PROPERTY() bool usesDistortion = false;
    ROCKS_PROPERTY() RefractionMethod refractionMethod = RefractionMethod::None;
    ROCKS_PROPERTY()
    RefractionCoverageMode refractionCoverageMode = RefractionCoverageMode::CoverageAccountedFor;
    ROCKS_PROPERTY() float refractionDepthBias = 0.0f;
    ROCKS_PROPERTY() bool lightFunctionAtlasCompatible = false;
    ROCKS_PROPERTY() bool alphaToCoverage = false;
    ROCKS_PROPERTY() bool translucentCustomDepthWrites = false;
    ROCKS_PROPERTY() bool frontLayerTranslucency = true;
    // GI 표면 캐시 공유(UE bCompatibleWithLumenCardSharing).
    ROCKS_PROPERTY() bool surfaceCacheSharing = false;
    ROCKS_PROPERTY() bool wireframe = false;
    ROCKS_PROPERTY() MaterialShadingRate shadingRate = MaterialShadingRate::Rate1x1;
    ROCKS_PROPERTY() bool allowVariableRateShading = true;
    ROCKS_PROPERTY() bool translucencyVertexFog = true;
    ROCKS_PROPERTY() bool applyCloudFogging = false;
    ROCKS_PROPERTY() bool isSky = false;
    ROCKS_PROPERTY() bool fogPerPixel = false;
    // TODO(MPQ-VirtualTexture): 가상 텍스처 단계가 읽는다(UE
    // bRelaxRuntimeVirtualTextureRestrictions).
    ROCKS_PROPERTY() bool relaxVirtualTextureRestrictions = false;
    ROCKS_PROPERTY() MaterialDisplacementTraits displacement;
    ROCKS_PROPERTY() MaterialTranslucencyTraits translucency;
    ROCKS_PROPERTY() MaterialForwardTraits forward;
    ROCKS_PROPERTY() MaterialPostProcessTraits postProcess;
};

} // namespace rocks::material
