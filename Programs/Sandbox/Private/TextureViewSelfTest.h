#pragma once

#include <Core/Memory/Allocator.h>
#include <Graphics/Device.h>

namespace sandbox {

// 텍스처 view 범위·쓰기 텍스처·compute 텍스처 묶기(RenderBase 4.4절 텍스처 GX2 A부)를 실제 장치로
// 확인한다.
// - HZB 패턴: Storage R32Float 8×8(mip 4)의 mip 0을 compute가 쓰기 view로 채우고, mip k 읽기 view와
//   mip k + 1 쓰기 view를 한 dispatch에 묶어 2×2 최댓값으로 줄인다. 모든 mip을 ReadTexture로 읽어
//   CPU 기댓값과 대조한다. 쓰기 텍스처를 묶지 않은 dispatch는 무시된다.
// - PS가 mip 범위 읽기 view를 view 기준 mip(TextureViewBaseMip, 그리기별 상수)으로 읽은 픽셀.
// - 2D 배열 slice 범위 view: 범위 view를 지원하면 view의 첫 층을 읽고, 아니면(OpenGL ES)
//   Unsupported다.
// - 규칙 위반 거부(Sampled 텍스처의 쓰기 view, mip 둘인 쓰기 view, 범위 밖).
// 장치가 형식 능력(Sampled·StorageWrite·Mipmaps)이 없으면 Storage 생성이 Unsupported인지 단언한다.
// device는 호출 스레드가 소유한다. 실패 수를 돌려준다.
rocks::uint32 RunTextureViewChecks(const char* name, rocks::graphics::Device& device,
                                   rocks::Allocator& allocator) noexcept;

// HZB 축소 시험 조각이다(이 시험과 렌더 그래프 시험이 함께 쓴다).
namespace hzb {

// 텍스처 한 변과 mip 수(8, 4, 2, 1), 줄이기 compute의 그룹 한 변(셰이더 numthreads)이다.
inline constexpr rocks::uint32 Extent = 8;
inline constexpr rocks::uint32 Mips = 4;
inline constexpr rocks::uint32 ReduceGroup = 4;

// 줄이기의 그리기별 상수(float4)다. x는 읽기 view의 기준 mip, y는 출력 한 변이다.
struct LevelConstants {
    float values[4] = {};
};

// Storage R32Float Extent×Extent, mip Mips개다.
rocks::graphics::TextureDesc PyramidDesc() noexcept;

// HZB mip 0 채우기 compute 파이프라인이다(쓰기 slot 0 = mip 0 쓰기 view).
rocks::Result<rocks::graphics::PipelineHandle>
CreateFillPipeline(rocks::graphics::Device& device) noexcept;

// HZB mip 축소 compute 파이프라인이다(텍스처 = 읽을 mip view, 쓰기 slot 0 = 다음 mip, 그리기별
// 상수 LevelConstants).
rocks::Result<rocks::graphics::PipelineHandle>
CreateReducePipeline(rocks::graphics::Device& device) noexcept;

// pyramid의 모든 mip을 ReadTexture로 읽어 CPU 기댓값과 대조한다. 다른 mip 수를 돌려주고 다른
// texel을 출력한다. GPU 작업이 끝난 뒤 부른다.
rocks::uint32 CountMismatchedMips(rocks::graphics::Device& device, rocks::Allocator& allocator,
                                  rocks::graphics::TextureHandle pyramid) noexcept;

} // namespace hzb

} // namespace sandbox
