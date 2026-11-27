#pragma once

#include <Texture/TextureBlockDecode.h>

// BC6H·BC7 디코더 내부 함수다(Docs/Design/AssetSystem 12.17.7). 공개 DecodeTextureBlock·
// DecodeTextureBlockHalf가 형식을 나눠 부른다.
namespace rocks::texture_block_detail {

// BC7 블록 하나를 RGBA8로 푼다(D3D11 BC7 — Microsoft DirectXTex D3DX_BC7::Decode와 같은 절차).
// 예약 모드(첫 byte가 0)는 투명 검정이다.
void DecodeBc7Block(const uint8* block, uint8 (&texels)[TextureBlockRgbaBytes]) noexcept;

// BC6H Ufloat 블록 하나를 RGBA16F half bit로 푼다(D3D11 BC6H 부호 없는 형식 — DirectXTex
// D3DX_BC6H::Decode의 unsigned 경로). 알파는 1.0(0x3C00)이다. 예약·무효 모드는 불투명 검정이다.
void DecodeBc6hBlock(const uint8* block, uint16 (&halfs)[TextureBlockTexels * 4]) noexcept;

} // namespace rocks::texture_block_detail
