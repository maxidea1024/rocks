#pragma once

#include <Base/Guid.h>
#include <Base/Types.h>
#include <MaterialCompiler/MaterialIrType.h>

// 머티리얼 IR의 텍스처 slot 하나다(Docs/Design/MaterialSystem 6.10절 MS2c3). IR 값 Texture가
// 가리키고, 모듈은 진입점에서 닿는 slot만 (출처, id) 순으로 담는다(생성 원문 MaterialTexture<k>의
// k).
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

// Texture 값 즉시값 [2]의 묶음이다(하위 8 bit 출처, 그 위 샘플 종류).
inline constexpr uint64 IrTextureSourceMask = 0xFF;
inline constexpr uint32 IrTextureSamplerTypeShift = 8;

struct IrTexture {
    IrTextureSource source = IrTextureSource::Asset;
    Guid id;
    IrTextureKind kind = IrTextureKind::Texture2D;
    // 파라미터의 기본 텍스처 에셋이다(Asset 출처면 id와 같다).
    Guid defaultAsset;
};

} // namespace rocks::material
