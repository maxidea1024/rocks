#pragma once

#include <Base/Guid.h>
#include <MaterialCompiler/MaterialIrType.h>
#include <Texture/TextureEnums.h>

// 번역 입력의 텍스처 에셋 사실 하나다(Docs/Design/MaterialSystem 6.10절 MS2c3, 16절 MAS3). 쿠킹·
// 에디터가 텍스처 에셋에서 채운다. 번역은 참조 해석, 샘플 종류 검사(V10 — UE
// GetSamplerTypeForTexture·VerifySamplerType), 큰 좌표 uv 주소 모드에 쓰고 IR에는 넣지 않는다.
// 번역 너겟 키는 참조 텍스처의 사실을 담는다(12.8절 — 사실이 바뀌면 번역 결과가 바뀔 수 있다).
//
// 사용 예:
//   MaterialTextureFacts facts;
//   facts.asset = brickId;
//   facts.compression = TextureCompressionSettings::Normalmap;
//   facts.srgb = false;
//   input.textures = Span<const MaterialTextureFacts>(&facts, 1);
namespace rocks::material {

// 주소 모드 칸 수(X·Y·Z)다.
inline constexpr usize MaterialTextureAddressAxes = 3;

struct MaterialTextureFacts {
    // 텍스처 에셋의 AssetId Guid다.
    Guid asset;
    IrTextureKind kind = IrTextureKind::Texture2D;
    TextureCompressionSettings compression = TextureCompressionSettings::Default;
    TextureGroup lodGroup = TextureGroup::World;
    bool srgb = true;
    bool virtualTexture = false;
    // X·Y·Z 주소 모드다(2D는 Z를 쓰지 않는다).
    TextureAddress address[MaterialTextureAddressAxes] = {
        TextureAddress::Wrap, TextureAddress::Wrap, TextureAddress::Wrap};
};

} // namespace rocks::material
