#pragma once

#include <Base/Guid.h>
#include <Base/Types.h>
#include <MaterialCompiler/MaterialIrType.h>

// 머티리얼 IR의 텍스처 slot 하나다(Docs/Design/MaterialSystem 6.10절 MS2c3). IR 값 Texture가
// 가리키고, 모듈은 진입점에서 닿는 slot만 (출처, id, 샘플러 출처) 순으로 담는다(생성 원문
// MaterialTexture<k>의 k — 한 텍스처를 두 샘플러 출처로 샘플링하면 slot이 둘이다).
//
// 사용 예:
//   for (const IrTexture& texture : module.Textures()) {
//       Bind(texture.source, texture.id, texture.kind);
//   }
namespace rocks::material {

// 텍스처 값의 출처다.
enum class IrTextureSource : uint8 {
    // 그래프가 직접 참조하는 텍스처 에셋이다(id = AssetId Guid).
    Asset = 0,
    // 텍스처 파라미터다(id = ParameterId, 기본값 = defaultAsset).
    Parameter = 1
};

// slot의 샘플러 출처다(셰이더 시스템 7.2절 9항 — slot마다 하나, 6.10절 텍스처 표). None은 샘플하지
// 않는 값(크기·Load)이다. 그 밖은 IrSamplerSource + 1이다.
enum class IrTextureSampler : uint8 {
    None = 0,
    Own = 1,
    SharedWrap = 2,
    SharedClamp = 3
};

// Texture 값 즉시값 [2]의 묶음이다(하위 8 bit 출처, 그 위 8 bit 샘플 종류, 그 위 8 bit 샘플러
// 출처).
inline constexpr uint64 IrTextureSourceMask = 0xFF;
inline constexpr uint32 IrTextureSamplerTypeShift = 8;
inline constexpr uint64 IrTextureSamplerTypeMask = 0xFF;
inline constexpr uint32 IrTextureSamplerShift = 16;
inline constexpr uint64 IrTextureSamplerMask = 0xFF;

struct IrTexture {
    IrTextureSource source = IrTextureSource::Asset;
    Guid id;
    IrTextureKind kind = IrTextureKind::Texture2D;
    // 파라미터의 기본 텍스처 에셋이다(Asset 출처면 id와 같다).
    Guid defaultAsset;
    // slot의 샘플러 출처다(모듈 표만 — 빌더 표는 (출처, id)마다 하나라 None이다).
    IrTextureSampler sampler = IrTextureSampler::None;
};

} // namespace rocks::material
