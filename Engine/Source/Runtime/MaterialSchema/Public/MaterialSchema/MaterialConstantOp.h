#pragma once

#include <Base/Types.h>
#include <Meta/Annotations.h>

// 상수 식 프로그램 명령의 연산이다(Docs/Design/MaterialSystem 9.4절·6.10절, 12.8절 MS3d2). 컴파일러
// 상수 빌더가 프로그램에 넣는 IR 연산 집합(IsMaterialConstantProgramOp)과 Constant·Parameter·
// Texture다. 실행 시 상수 블록 평가가 읽는다(컴파일러 IR 연산 번호와 독립 — 쿠킹이 옮긴다).
//
// UE EPreshaderOpcode(RE/Public/Shader/Preshader.h:19–75) 대응: Constant = ConstantZero·Constant,
// Parameter = Parameter, TextureSize = TextureSize·TexelSize, 사칙·Min·Max·Clamp·Abs·Floor·Ceil·
// Round·Trunc·Frac·Saturate·Sign·Sqrt·Rcp·Length·Normalize·Dot·Cross·Sin·Cos·Tan·Asin·Acos·Atan·
// Atan2·Exp·Exp2·Log·Log2·Log10·Fmod·Modulo·Neg,
// Splat·ComponentSwizzle(Swizzle)·AppendVector(Append), 형 바꿈(Convert). Texture는 TextureSize의
// 피연산자 slot 값이다(UE는 텍스처 index 즉시값). UE에만 있는 것:
// Fractional·비교·Assign·PushValue(UE도 접지 않거나 평가 내부용 — 9.4절),
// ExternalTextureCoordinateScaleRotation·Offset(MPQ-ExternalTexture), RuntimeVirtualTextureUniform
// (MPQ-VirtualTexture), SparseVolumeTextureUniform(MPQ-SparseVolumeTexture).
namespace rocks::material {

ROCKS_ENUM()
enum class MaterialConstantOp : uint8 {
    Constant = 0,
    Parameter = 1,
    Texture = 2,
    TextureSize = 3,
    Add = 4,
    Subtract = 5,
    Multiply = 6,
    Divide = 7,
    Min = 8,
    Max = 9,
    Abs = 10,
    Floor = 11,
    Ceil = 12,
    Round = 13,
    Truncate = 14,
    Frac = 15,
    Saturate = 16,
    Sign = 17,
    Sqrt = 18,
    Reciprocal = 19,
    Length = 20,
    Normalize = 21,
    Dot = 22,
    Cross = 23,
    Sin = 24,
    Cos = 25,
    Tan = 26,
    ASin = 27,
    ACos = 28,
    ATan = 29,
    ATan2 = 30,
    Exponential = 31,
    Exponential2 = 32,
    Logarithm = 33,
    Logarithm2 = 34,
    Logarithm10 = 35,
    Fmod = 36,
    Modulo = 37,
    Negate = 38,
    Clamp = 39,
    Splat = 40,
    Swizzle = 41,
    Append = 42,
    Convert = 43
};

// 마지막 연산이다(해석의 범위 검사).
inline constexpr MaterialConstantOp LastMaterialConstantOp = MaterialConstantOp::Convert;

} // namespace rocks::material
