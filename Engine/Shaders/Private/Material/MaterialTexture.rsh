// 머티리얼 텍스처 샘플 함수와 샘플 종류 처리다(Docs/Design/MaterialSystem 6.10절 MS2c3). 생성 원문이
// MaterialProcess*Lookup(MaterialSample<종류><방식>(텍스처, 샘플러, uv, …))로 부른다. 식은 UE 5.8
// SH/Private/Common.ush 샘플 함수(Texture2DSample·…Level·…Bias·…Grad·…_Decal·Gather)와
// SH/Private/MaterialTemplate.ush Process*TextureLookup·UnpackNormalMap과 같다.
//
// 사용 예(생성 원문):
//   const float4 m7 = MaterialUnpackNormalMap(MaterialSample2D(MaterialTexture0, MaterialTexture0Sampler, m5));
#pragma once

// decal 샘플을 mip 0으로 읽는 형식인가(UE Texture2DSample_Decal의 METAL_ES3_1_PROFILE·
// COMPILER_GLSL_ES3_1 — 형식 정의가 켠다).
#ifndef MATERIAL_DECAL_SAMPLE_LEVEL_ZERO
#define MATERIAL_DECAL_SAMPLE_LEVEL_ZERO 0
#endif

// 단일 채널 sRGB 형식이 없어 Grayscale을 수동으로 선형화하는 형식인가(UE
// ProcessMaterialGreyscaleTextureLookup의 ES3_1·Vulkan 경로 — 형식 정의가 켠다).
#ifndef MATERIAL_MANUAL_GRAYSCALE_SRGB
#define MATERIAL_MANUAL_GRAYSCALE_SRGB 0
#endif

// 종류마다 같은 샘플 함수 묶음이다(Texture = HLSL 자원 타입, Name = 함수 이름 조각, Uv·Gradient =
// 좌표·미분 타입).
#define MATERIAL_TEXTURE_SAMPLES(Texture, Name, Uv, Gradient)                                       \
    float4 MaterialSample##Name(Texture t, SamplerState s, Uv uv) {                                \
        return t.Sample(s, uv);                                                                    \
    }                                                                                              \
                                                                                                   \
    float4 MaterialSample##Name##Level(Texture t, SamplerState s, Uv uv, float level) {             \
        return t.SampleLevel(s, uv, level);                                                        \
    }                                                                                              \
                                                                                                   \
    float4 MaterialSample##Name##Bias(Texture t, SamplerState s, Uv uv, float bias) {               \
        return t.SampleBias(s, uv, bias);                                                          \
    }                                                                                              \
                                                                                                   \
    float4 MaterialSample##Name##Grad(Texture t, SamplerState s, Uv uv, Gradient dx, Gradient dy) { \
        return t.SampleGrad(s, uv, dx, dy);                                                        \
    }                                                                                              \
                                                                                                   \
    /* DeferredDecal 픽셀 샘플(UE *Sample_Decal). */                                               \
    float4 MaterialSample##Name##_Decal(Texture t, SamplerState s, Uv uv) {                        \
        if (MATERIAL_DECAL_SAMPLE_LEVEL_ZERO) {                                                    \
            return t.SampleLevel(s, uv, 0.0);                                                      \
        }                                                                                          \
                                                                                                   \
        return t.Sample(s, uv);                                                                    \
    }

MATERIAL_TEXTURE_SAMPLES(Texture2D, 2D, float2, float2)
MATERIAL_TEXTURE_SAMPLES(TextureCube, Cube, float3, float3)
MATERIAL_TEXTURE_SAMPLES(Texture2DArray, 2DArray, float3, float2)
MATERIAL_TEXTURE_SAMPLES(TextureCubeArray, CubeArray, float4, float3)
MATERIAL_TEXTURE_SAMPLES(Texture3D, 3D, float3, float3)

#undef MATERIAL_TEXTURE_SAMPLES

// gather 함수 묶음이다(UE Texture2DGatherRed 등 — 2D·Cube·2DArray·CubeArray).
#define MATERIAL_TEXTURE_GATHERS(Texture, Name, Uv)                                                 \
    float4 MaterialSample##Name##GatherRed(Texture t, SamplerState s, Uv uv) {                     \
        return t.GatherRed(s, uv);                                                                 \
    }                                                                                              \
                                                                                                   \
    float4 MaterialSample##Name##GatherGreen(Texture t, SamplerState s, Uv uv) {                   \
        return t.GatherGreen(s, uv);                                                               \
    }                                                                                              \
                                                                                                   \
    float4 MaterialSample##Name##GatherBlue(Texture t, SamplerState s, Uv uv) {                    \
        return t.GatherBlue(s, uv);                                                                \
    }                                                                                              \
                                                                                                   \
    float4 MaterialSample##Name##GatherAlpha(Texture t, SamplerState s, Uv uv) {                   \
        return t.GatherAlpha(s, uv);                                                               \
    }

MATERIAL_TEXTURE_GATHERS(Texture2D, 2D, float2)
MATERIAL_TEXTURE_GATHERS(TextureCube, Cube, float3)
MATERIAL_TEXTURE_GATHERS(Texture2DArray, 2DArray, float3)
MATERIAL_TEXTURE_GATHERS(TextureCubeArray, CubeArray, float4)

#undef MATERIAL_TEXTURE_GATHERS

// 샘플 종류 처리다(UE MaterialTemplate.ush Process*TextureLookup). Color·LinearColor는 그대로다.
float4 MaterialProcessColorLookup(float4 value) {
    return value;
}

float4 MaterialProcessLinearColorLookup(float4 value) {
    return value;
}

// Alpha·DistanceFieldFont: 단일 채널 형식의 r을 모든 채널로 편다.
float4 MaterialProcessAlphaLookup(float4 value) {
    return value.rrrr;
}

float4 MaterialProcessGrayscaleLookup(float4 value) {
    if (MATERIAL_MANUAL_GRAYSCALE_SRGB) {
        // UE: sRGB 단일 채널 형식이 없으면 감마 2.0 근사로 선형화한다.
        return (value.r * value.r).rrrr;
    }

    return value.rrrr;
}

float4 MaterialProcessLinearGrayscaleLookup(float4 value) {
    return value.rrrr;
}

// 노멀맵(XY를 RG 두 채널에 둔다 — 6.10절, 16절 MAS3)을 푼다(UE UnpackNormalMap).
float4 MaterialUnpackNormalMap(float4 value) {
    const float2 xy = value.rg * float2(2.0, 2.0) - float2(1.0, 1.0);
    const float z = sqrt(saturate(1.0 - dot(xy, xy)));
    return float4(xy, z, 1.0);
}
