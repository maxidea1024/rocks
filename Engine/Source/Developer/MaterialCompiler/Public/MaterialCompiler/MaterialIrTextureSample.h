#pragma once

#include <Base/Types.h>

// 머티리얼 IR TextureSample 값의 샘플 방식이다(Docs/Design/MaterialSystem 6.10절 MS2c3). lowering이
// UE 규칙(정점 문맥 Level, view mip bias 조건, decal)을 적용한 뒤의 최종 방식이고, 생성 원문은 이
// 값대로 샘플 함수를 고른다.
//
// 사용 예:
//   IrTextureSampleDesc desc;
//   desc.samplerType = static_cast<uint8>(SamplerType::Normal);
//   desc.mipMode = IrTextureMipMode::Level;
//   auto sample = builder.TextureSample(desc, texture, uv, level, NoIrValue);
namespace rocks::material {

// mip 방식이다(UE ETextureMipValueMode의 최종 형태).
enum class IrTextureMipMode : uint8 {
    // 하드웨어 미분(픽셀 문맥).
    Implicit = 0,
    Level = 1,
    Bias = 2,
    // 명시 미분(ddx·ddy 피연산자).
    Gradient = 3
};

// 샘플러 출처다(UE ESamplerSourceMode — 공유 샘플러는 셰이더 시스템 7.2절 9항 예약 이름).
enum class IrSamplerSource : uint8 {
    Own = 0,
    SharedWrap = 1,
    SharedClamp = 2
};

// gather 채널이다(UE ETextureGatherMode).
enum class IrTextureGather : uint8 {
    None = 0,
    Red = 1,
    Green = 2,
    Blue = 3,
    Alpha = 4
};

// TextureSample 값 즉시값 [3]의 묶음이다(하위 4 bit gather 채널, bit 4 decal).
inline constexpr uint64 IrTextureGatherMask = 0xF;
inline constexpr uint32 IrTextureDecalShift = 4;

// AddressDemote 즉시값의 성분당 주소 모드다(UE LWCADDRESSMODE_* — MaterialDouble.rsh
// MATERIAL_ADDRESS_MODE_*와 같은 값).
inline constexpr uint32 IrAddressModeClamp = 0;
inline constexpr uint32 IrAddressModeWrap = 1;
inline constexpr uint32 IrAddressModeMirror = 2;
inline constexpr uint32 IrAddressModeBits = 2;
inline constexpr uint64 IrAddressModeMask = 3;

struct IrTextureSampleDesc {
    // 샘플 종류 처리다(MaterialSchema SamplerType 값 — 생성 원문 MaterialProcess*Lookup).
    uint8 samplerType = 0;
    IrTextureMipMode mipMode = IrTextureMipMode::Implicit;
    IrSamplerSource samplerSource = IrSamplerSource::Own;
    IrTextureGather gather = IrTextureGather::None;
    // DeferredDecal 픽셀 문맥의 Implicit 샘플이다(UE Texture2DSample_Decal).
    bool decal = false;
};

} // namespace rocks::material
