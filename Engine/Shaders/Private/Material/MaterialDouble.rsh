// 머티리얼 Double 값(LWC)의 원문 표현과 연산이다(Docs/Design/MaterialSystem 5.3·6.9절). UE 5.8 기본
// 번역기처럼 tile + offset이다(값 = tile × MaterialDoubleTileSize + offset — UE FLWCVector,
// WSVECTOR_IS_TILEOFFSET, RE/Private/Materials/HLSLMaterialTranslator.cpp:2539). 연산 식은 UE
// SH/Private/LWCOperations.ush와 같다(결과가 UE와 같아야 한다). 생성 원문은 IR 연산의 피연산자를
// 산술 공통 타입으로 맞춘 뒤 부르므로 Double × Double 판만 둔다.
//
// 사용 예:
//   MaterialDouble3 position = MaterialDoubleAdd(c.worldPosition, MaterialDoublePromote(offset));
//   float3 relative = MaterialDoubleDemote(MaterialDoubleSubtract(position, origin));
#pragma once

#include "/Engine/Private/Material/MaterialSelect.rsh"

// tile 한 칸의 크기(cm)와 그 역수다(UE UE_LWC_RENDER_TILE_SIZE —
// Core/Public/Misc/LargeWorldRenderPosition.h:14).
static const float MaterialDoubleTileSize = 2097152.0;
static const float MaterialDoubleTileSizeRcp = 1.0 / 2097152.0;

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

// 폭마다 같은 연산 묶음이다(Double = MaterialDoubleN, Float = floatN, Bool = boolN).
#define MATERIAL_DOUBLE_OPERATIONS(Double, Float, Bool)                                              \
    Double Make##Double(Float tile, Float offset) {                                              \
        Double value;                                                                            \
        value.tile = tile;                                                                       \
        value.offset = offset;                                                                   \
        return value;                                                                            \
    }                                                                                            \
                                                                                                 \
    /* Float → Double(tile 0 — UE WSPromote). */                                                 \
    Double MaterialDoublePromote(Float value) {                                                  \
        return Make##Double((Float)0, value);                                                    \
    }                                                                                            \
                                                                                                 \
    /* Double → Float(UE WSDemote·LWCToFloat). */                                                \
    Float MaterialDoubleDemote(Double value) {                                                   \
        return value.tile * MaterialDoubleTileSize + value.offset;                               \
    }                                                                                            \
                                                                                                 \
    Double MaterialDoubleAdd(Double left, Double right) {                                        \
        return Make##Double(left.tile + right.tile, left.offset + right.offset);                 \
    }                                                                                            \
                                                                                                 \
    Double MaterialDoubleSubtract(Double left, Double right) {                                   \
        return Make##Double(left.tile - right.tile, left.offset - right.offset);                 \
    }                                                                                            \
                                                                                                 \
    /* UE LWCMultiply(LWC, LWC) — LWCOperations.ush:113–116. */                                  \
    Double MaterialDoubleMultiply(Double left, Double right) {                                   \
        return Make##Double(left.tile * (right.tile * MaterialDoubleTileSize + right.offset) +   \
                                right.tile * left.offset,                                        \
                            left.offset * right.offset);                                         \
    }                                                                                            \
                                                                                                 \
    /* UE LWCRcp(역수 — 크기를 tile 단위로 줄여 계산한다). */                                     \
    Float MaterialDoubleReciprocal(Double value) {                                               \
        return rcp(value.offset * MaterialDoubleTileSizeRcp + value.tile) *                      \
               MaterialDoubleTileSizeRcp;                                                        \
    }                                                                                            \
                                                                                                 \
    /* UE LWCDivide(LWC, LWC) = LWCMultiply(LWC, LWCRcp(LWC)). */                                \
    Double MaterialDoubleDivide(Double left, Double right) {                                     \
        const Float inverse = MaterialDoubleReciprocal(right);                                   \
        return Make##Double(left.tile * inverse, left.offset * inverse);                         \
    }                                                                                            \
                                                                                                 \
    Double MaterialDoubleNegate(Double value) {                                                  \
        return Make##Double(-value.tile, -value.offset);                                         \
    }                                                                                            \
                                                                                                 \
    /* 비교(UE LWC_COMPARE_OP — (tileL − tileR) × T 대 offsetR − offsetL). */                    \
    Bool MaterialDoubleLess(Double left, Double right) {                                         \
        return (left.tile - right.tile) * MaterialDoubleTileSize < right.offset - left.offset;   \
    }                                                                                            \
                                                                                                 \
    Bool MaterialDoubleLessEqual(Double left, Double right) {                                    \
        return (left.tile - right.tile) * MaterialDoubleTileSize <= right.offset - left.offset;  \
    }                                                                                            \
                                                                                                 \
    Bool MaterialDoubleGreater(Double left, Double right) {                                      \
        return (left.tile - right.tile) * MaterialDoubleTileSize > right.offset - left.offset;   \
    }                                                                                            \
                                                                                                 \
    Bool MaterialDoubleGreaterEqual(Double left, Double right) {                                 \
        return (left.tile - right.tile) * MaterialDoubleTileSize >= right.offset - left.offset;  \
    }                                                                                            \
                                                                                                 \
    Bool MaterialDoubleEquals(Double left, Double right) {                                       \
        return (left.tile - right.tile) * MaterialDoubleTileSize == right.offset - left.offset;  \
    }                                                                                            \
                                                                                                 \
    Bool MaterialDoubleNotEquals(Double left, Double right) {                                    \
        return (left.tile - right.tile) * MaterialDoubleTileSize != right.offset - left.offset;  \
    }                                                                                            \
                                                                                                 \
    Double MaterialDoubleSelect(Bool condition, Double whenTrue, Double whenFalse) {             \
        return Make##Double(MaterialSelect(condition, whenTrue.tile, whenFalse.tile),            \
                            MaterialSelect(condition, whenTrue.offset, whenFalse.offset));       \
    }                                                                                            \
                                                                                                 \
    Double MaterialDoubleMin(Double left, Double right) {                                        \
        return MaterialDoubleSelect(MaterialDoubleLess(left, right), left, right);               \
    }                                                                                            \
                                                                                                 \
    Double MaterialDoubleMax(Double left, Double right) {                                        \
        return MaterialDoubleSelect(MaterialDoubleGreater(left, right), left, right);            \
    }                                                                                            \
                                                                                                 \
    Double MaterialDoubleAbs(Double value) {                                                     \
        return MaterialDoubleSelect(MaterialDoubleLess(value, MaterialDoublePromote((Float)0)),  \
                                    MaterialDoubleNegate(value), value);                         \
    }                                                                                            \
                                                                                                 \
    /* UE LWCMakeIntTile(tile을 정수로 맞추고 나머지를 offset으로 — LWCOperations.ush:13–17). */  \
    Double MaterialDoubleIntegerTile(Double value) {                                             \
        const Float integer = floor(value.tile);                                                 \
        return Make##Double(integer, (value.tile - integer) * MaterialDoubleTileSize +           \
                                         value.offset);                                          \
    }                                                                                            \
                                                                                                 \
    Double MaterialDoubleFloor(Double value) {                                                   \
        const Double whole = MaterialDoubleIntegerTile(value);                                   \
        return Make##Double(whole.tile, floor(whole.offset));                                    \
    }                                                                                            \
                                                                                                 \
    Double MaterialDoubleCeil(Double value) {                                                    \
        const Double whole = MaterialDoubleIntegerTile(value);                                   \
        return Make##Double(whole.tile, ceil(whole.offset));                                     \
    }                                                                                            \
                                                                                                 \
    Double MaterialDoubleRound(Double value) {                                                   \
        const Double whole = MaterialDoubleIntegerTile(value);                                   \
        return Make##Double(whole.tile, round(whole.offset));                                    \
    }                                                                                            \
                                                                                                 \
    Double MaterialDoubleTruncate(Double value) {                                                \
        const Double whole = MaterialDoubleIntegerTile(value);                                   \
        return Make##Double(whole.tile, trunc(whole.offset));                                    \
    }

MATERIAL_DOUBLE_OPERATIONS(MaterialDouble1, float, bool)
MATERIAL_DOUBLE_OPERATIONS(MaterialDouble2, float2, bool2)
MATERIAL_DOUBLE_OPERATIONS(MaterialDouble3, float3, bool3)
MATERIAL_DOUBLE_OPERATIONS(MaterialDouble4, float4, bool4)

#undef MATERIAL_DOUBLE_OPERATIONS

// 큰 좌표 uv 성분 하나를 주소 모드대로 낮춘다(UE MaterialTemplate.ush LWCApplyAddressMode — 6.10절
// MS2c3). Wrap(1)은 tile의 소수부만 남기고(텍스처 장치가 마지막 wrap을 하므로 tile 경계 밖에서 미분이
// 맞다), Clamp(0)·Mirror(2)는 그냥 낮춘다.
float MaterialDoubleApplyAddressMode(MaterialDouble1 value, uint mode) {
    if (mode == 1u) {
        return frac(value.tile * MaterialDoubleTileSize) + value.offset;
    }

    return MaterialDoubleDemote(value);
}

float2 MaterialDoubleApplyAddressMode(MaterialDouble2 value, uint x, uint y) {
    return float2(MaterialDoubleApplyAddressMode(MakeMaterialDouble1(value.tile.x, value.offset.x), x),
                  MaterialDoubleApplyAddressMode(MakeMaterialDouble1(value.tile.y, value.offset.y), y));
}

float3 MaterialDoubleApplyAddressMode(MaterialDouble3 value, uint x, uint y, uint z) {
    return float3(MaterialDoubleApplyAddressMode(MakeMaterialDouble1(value.tile.x, value.offset.x), x),
                  MaterialDoubleApplyAddressMode(MakeMaterialDouble1(value.tile.y, value.offset.y), y),
                  MaterialDoubleApplyAddressMode(MakeMaterialDouble1(value.tile.z, value.offset.z), z));
}

float4 MaterialDoubleApplyAddressMode(MaterialDouble4 value, uint x, uint y, uint z, uint w) {
    return float4(MaterialDoubleApplyAddressMode(MakeMaterialDouble1(value.tile.x, value.offset.x), x),
                  MaterialDoubleApplyAddressMode(MakeMaterialDouble1(value.tile.y, value.offset.y), y),
                  MaterialDoubleApplyAddressMode(MakeMaterialDouble1(value.tile.z, value.offset.z), z),
                  MaterialDoubleApplyAddressMode(MakeMaterialDouble1(value.tile.w, value.offset.w), w));
}
