// 머티리얼 Double 값(LWC)의 원문 표현이다(Docs/Design/MaterialSystem 5.3·6.9절). UE 5.8 기본
// 번역기처럼 tile + offset이다(값 = tile × MaterialDoubleTileSize + offset — UE FLWCVector,
// WSVECTOR_IS_TILEOFFSET, RE/Private/Materials/HLSLMaterialTranslator.cpp:2539). 연산은 MS2b2가
// 더한다.
//
// 사용 예:
//   MaterialDouble3 position = c.worldPosition;
//   float3 approximate = position.tile * MaterialDoubleTileSize + position.offset;
#pragma once

// tile 한 칸의 크기(cm)다(UE UE_LWC_RENDER_TILE_SIZE —
// Core/Public/Misc/LargeWorldRenderPosition.h:14).
static const float MaterialDoubleTileSize = 2097152.0;

struct MaterialDouble1 {
    float tile;
    float offset;
};

struct MaterialDouble2 {
    float2 tile;
    float2 offset;
};

struct MaterialDouble3 {
    float3 tile;
    float3 offset;
};

struct MaterialDouble4 {
    float4 tile;
    float4 offset;
};
