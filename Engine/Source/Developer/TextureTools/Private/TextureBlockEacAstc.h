#pragma once

#include <Core/Containers/Span.h>
#include <Core/Result.h>
#include <TextureTools/TextureBlockEncode.h>

// EAC R11·ASTC 인코더 내부 함수다(Docs/Design/AssetSystem 12.17.8). 공개 EncodeTextureBlocks가
// 형식을 나눠 부른다.
namespace rocks::texture_block_detail {

// 검증을 마친 R16 그림(texel 2 byte, little-endian)을 EAC R11 블록으로 out에 쓴다. base는 slice 0의
// 첫 byte, pitch는 byte 단위다. out은 출력 크기와 같다(호출자가 확인했다). options.scheduler가
// 있으면 블록 행 병렬이다(12.17.9 2 — 직렬이면 할당하지 않는다).
Result<void> EncodeEacBlocks(const uint8* base, uint32 sizeX, uint32 sizeY, uint32 numSlices,
                             uint64 rowPitch, uint64 slicePitch, const TextureBlockOptions& options,
                             Span<uint8> out) noexcept;

// 검증을 마친 RGBA8 그림을 astcenc로 format(ASTC) 블록으로 out에 쓴다. 행 사이 여백이 있으면
// options.scratch로 빽빽한 사본을 만든다(직렬은 slice 하나, 병렬은 그림 전체 — 없으면
// InvalidArgument, 할당 실패는 OutOfMemory). 병렬(options.scheduler, 블록 16개 넘게)은 slice 포인터
// 표도 scratch로 할당한다. slice 하나의 블록 수가 astcenc 호출 상한(2^27 − 1)을 넘으면
// CapacityExceeded다. astcenc 컨텍스트 할당 실패·오류는 12.17.8 3의 오류 표대로 바꾼다.
Result<void> EncodeAstcBlocks(TextureBlockFormat format, const uint8* base, uint32 sizeX,
                              uint32 sizeY, uint32 numSlices, uint64 rowPitch, uint64 slicePitch,
                              const TextureBlockOptions& options, Span<uint8> out) noexcept;

} // namespace rocks::texture_block_detail
