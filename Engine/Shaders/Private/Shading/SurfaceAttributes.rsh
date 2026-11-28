// 표면 속성 SurfaceAttributes다(Docs/Design/Renderer 14.1절 — 렌더러가 머티리얼에서 읽는 유일한
// 구조체, 엔진 셰이더 4.4절 A04·7.5절·SLR6). 필드 순서·타입은 RN 14.1절 표 순서다. 값은 선형이고
// 법선·tangent는 world 공간 단위 벡터다(머티리얼 번역기가 tangent 공간 노멀을 바꿔 넣는다).
// Material/MaterialCommon.rsh가 include한다(MS 7.2절).
//
// shading model 번호(RN 14.2절 결정 RN5)는 SL2에서 Shared/SurfaceEnums.h로 옮겨 C++ RenderData와
// 공유한다. 그 전에는 여기 상수가 RN 14.2절 번호다.
#pragma once

static const uint ShadingModelUnlit = 0u;
static const uint ShadingModelDefaultLit = 1u;

// 기본값은 UE 5.8 머티리얼 속성 기본값과 같다(SRC/Runtime/Engine/Private/Materials/
// MaterialAttributeDefinitionMap.cpp — BaseColor 0, Metallic 0, Specular 0.5, Roughness 0.5,
// Anisotropy 0, Emissive 0, Opacity 1, OpacityMask 1, Normal (0,0,1), Tangent (1,0,0), CustomData0 1,
// CustomData1 0.1, AmbientOcclusion 1, SubsurfaceColor 1)이다.
static const float DefaultSurfaceSpecular = 0.5;
static const float DefaultSurfaceRoughness = 0.5;
static const float DefaultSurfaceCustomData0 = 1.0;
static const float DefaultSurfaceCustomData1 = 0.1;

struct SurfaceAttributes {
    float3 baseColor;
    float metallic;
    float specular;
    float roughness;
    float3 normal;
    float3 emissive;
    float ambientOcclusion;
    float opacity;
    float opacityMask;
    uint shadingModel;
    float customData0;
    float customData1;
    float3 tangent;
    float anisotropy;
    // UE MP_SubsurfaceColor(Subsurface·TwoSidedFoliage·Cloth의 색 — Docs/Design/MaterialSystem 6.11절,
    // 16절 MRD17).
    float3 subsurfaceColor;
};

// 기본 표면(DefaultLit)이다. normal·tangent 기본값은 world 축이다 — 머티리얼 생성 코드가 정점 법선·
// tangent로 바꾼다.
SurfaceAttributes DefaultSurfaceAttributes() {
    SurfaceAttributes surface;
    surface.baseColor = float3(0.0, 0.0, 0.0);
    surface.metallic = 0.0;
    surface.specular = DefaultSurfaceSpecular;
    surface.roughness = DefaultSurfaceRoughness;
    surface.normal = float3(0.0, 0.0, 1.0);
    surface.emissive = float3(0.0, 0.0, 0.0);
    surface.ambientOcclusion = 1.0;
    surface.opacity = 1.0;
    surface.opacityMask = 1.0;
    surface.shadingModel = ShadingModelDefaultLit;
    surface.customData0 = DefaultSurfaceCustomData0;
    surface.customData1 = DefaultSurfaceCustomData1;
    surface.tangent = float3(1.0, 0.0, 0.0);
    surface.anisotropy = 0.0;
    surface.subsurfaceColor = float3(1.0, 1.0, 1.0);
    return surface;
}
