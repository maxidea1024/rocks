#pragma once

#include <Core/Containers/Span.h>
#include <Core/Result.h>
#include <TextureTools/TextureBlockEncode.h>

// BC6H·BC7 인코더 내부 함수다(Docs/Design/AssetSystem 12.17.6). 공개 EncodeTextureBlocks가 형식을
// 나눠 부른다(디코더는 Texture 모듈 — 12.17.7).
namespace rocks::texture_block_detail {

// 검증을 마친 그림을 ISPC Texture Compressor로 압축한다(BC6H·BC7만). base는 slice 0의 첫 byte,
// pitch는 byte 단위다. out은 출력 크기와 같다(호출자가 확인했다). Android 대상과 SSE4.2가 없는 x64
// CPU는 Unsupported다.
Result<void> EncodeIspcBlocks(TextureBlockFormat format, const uint8* base, uint32 sizeX,
                              uint32 sizeY, uint32 numSlices, uint64 rowPitch, uint64 slicePitch,
                              const TextureBlockOptions& options, Span<uint8> out) noexcept;

} // namespace rocks::texture_block_detail
