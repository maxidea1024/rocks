#pragma once

#include <Base/NumericLimits.h>
#include <Base/Types.h>

// IR Library 연산의 함수 표다(Docs/Design/MaterialSystem 6.13절 MS2f3). 즉시값 [0]이 함수 번호,
// [1]–[3]이 함수마다 정한 정적 인자 묶음이다. 생성 원문은 UE 함수 이름·인자 순서 그대로 부르고 함수
// 원문은 Engine/Shaders/Private/Material/MaterialNoise.rsh다. 저장 형식(IR 해시)이라 번호를 바꾸지
// 않는다.
namespace rocks::material {

enum class IrLibraryFunction : uint8 {
    // MaterialExpressionNoise — [1] function | quality << 8 | turbulence << 16 | tiling << 17 |
    // levels << 24 | repeatSize << 32, [2] scale bit | levelScale bit << 32, [3] outputMin bit |
    // outputMax bit << 32. 피연산자 position(float3·Double3)·filterWidth(float), 결과 float.
    Noise = 0,
    // MaterialExpressionVectorNoise — [1] function | quality << 8 | tiling << 16 | tileSize << 32.
    // 피연산자 position(float3), 결과 float4.
    VectorNoise = 1
};

// 함수 수다(마지막 값 + 1).
inline constexpr uint32 IrLibraryFunctionCount = 2;

// 즉시값 묶음의 bit 위치·폭이다.
inline constexpr uint32 IrLibraryByteBits = 8;
inline constexpr uint64 IrLibraryByteMask = 0xFFu;
inline constexpr uint32 IrLibraryQualityShift = 8;
inline constexpr uint32 IrLibraryNoiseTurbulenceShift = 16;
inline constexpr uint32 IrLibraryNoiseTilingShift = 17;
inline constexpr uint32 IrLibraryNoiseLevelsShift = 24;
inline constexpr uint32 IrLibraryVectorNoiseTilingShift = 16;
inline constexpr uint32 IrLibraryHighWordShift = 32;
inline constexpr uint64 IrLibraryWordMask = NumericLimits<uint32>::Max();

} // namespace rocks::material
