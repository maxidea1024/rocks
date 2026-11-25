// SurfaceAttributes → 셰이딩 파라미터다(Docs/Design/EngineShaders 4.4절 A04·7.5절 — 골격). 고전 속성
// (baseColor·metallic·specular·roughness)을 단일 slab 파라미터(확산 albedo·F0·F90·α)로 바꾼다(결정
// SLD3, MS MD1). 식: DiffuseColor = baseColor·(1 − metallic), F0 = lerp(0.08·specular, baseColor,
// metallic)(Karis 2013 — specular 0.5 → 4%), F90 = 1(Schlick).
//
// shading model 분기: SL0에서 동작하는 모델은 Unlit·DefaultLit뿐이다. 다른 번호는 #error가 아니라
// DefaultLit 값으로 채우고 supported = false로 알린다(진단 — 호출자가 디버그 색으로 표시할 수 있다).
// MATERIAL_SINGLE_SHADING_MODEL 퍼뮤테이션 분기는 머티리얼 pass(SL2·SL3)가 정의를 넣을 때 쓴다.
#pragma once

#include "/Engine/Private/Shading/MicrofacetTerms.rsh"
#include "/Engine/Private/Shading/SurfaceAttributes.rsh"

// 비금속 F0 배율(specular 1 → 8%)이다.
static const float DielectricSpecularScale = 0.08;

struct ShadingParameters {
    float3 diffuseColor;
    float3 f0;
    float f90;
    // 지각 roughness(하한 적용)와 GGX α다.
    float roughness;
    float alpha;
    uint shadingModel;
    // 이 모델을 SL0 골격이 평가할 수 있는가(아니면 DefaultLit 값이다).
    bool supported;
};

bool IsShadingModelSupported(uint shadingModel) {
    return shadingModel == ShadingModelUnlit || shadingModel == ShadingModelDefaultLit;
}

ShadingParameters MakeShadingParameters(SurfaceAttributes surface) {
    ShadingParameters shading;
    const float metallic = saturate(surface.metallic);
    const float dielectricF0 = DielectricSpecularScale * saturate(surface.specular);
    shading.diffuseColor = surface.baseColor * (1.0 - metallic);
    shading.f0 = lerp(float3(dielectricF0, dielectricF0, dielectricF0), surface.baseColor, metallic);
    shading.f90 = 1.0;
    shading.roughness = max(saturate(surface.roughness), MinimumGgxRoughness);
    shading.alpha = RoughnessToAlpha(surface.roughness);
    shading.shadingModel = surface.shadingModel;
    shading.supported = IsShadingModelSupported(surface.shadingModel);
    if (surface.shadingModel == ShadingModelUnlit) {
        // Unlit은 조명을 받지 않는다(emissive만).
        shading.diffuseColor = float3(0.0, 0.0, 0.0);
        shading.f0 = float3(0.0, 0.0, 0.0);
        shading.f90 = 0.0;
    }

    return shading;
}
