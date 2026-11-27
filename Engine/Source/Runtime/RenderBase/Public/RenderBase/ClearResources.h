#pragma once

#include <Core/Result.h>
#include <Graphics/Device.h>
#include <Graphics/PipelineState.h>
#include <RenderBase/PipelineCache.h>
#include <RenderBase/ShaderSet.h>

// 자원 clear 도우미다(Docs/Design/RenderBase 11.2절 RR17, 엔진 셰이더 14.4절 4항·14.5절 3항, UE
// ClearUnusedGraphResources·AddClearUAVPass·DrawClearQuad 대응). 셰이더는 /Engine/Private/Utility/
// ClearResources.rsf다. 모두 그래프 밖 기록(호출자가 연 기록)에 명령을 더한다.
//
// - 전체 attachment clear는 이 도우미가 아니라 pass의 load 동작(LoadAction::Clear)이다.
// - R32 정수·raw 버퍼 view를 word 하나로 채우는 것은 장치 ClearWritableBufferView가 더 싸다.
//
// 사용 예(compute 기록에서 storage 텍스처 mip 하나를 0으로):
//   render::WritableTextureRegion region;
//   region.width = mipWidth;
//   region.height = mipHeight;
//   auto cleared = render::ClearWritableTexture(device, commands, set, cache, writeView,
//                                               render::ClearValue::Float(0, 0, 0, 0), region);
//
// 사용 예(raster pass 안 사각형 부분 clear):
//   render::ClearRegionDesc clear;
//   clear.rect = {16, 16, 64, 32};
//   clear.targetWidth = 256;
//   clear.targetHeight = 256;
//   clear.colorFormat = PixelFormat::RGBA8Unorm;
//   clear.clearColor = true;
//   clear.color = render::ClearValue::Float(1, 0, 0, 1);
//   auto cleared = render::ClearRegion(commands, set, cache, clear);
namespace rocks::render {

// clear 값이다: 32-bit word 넷. float 형식은 float bit, 정수 형식은 정수다(대상 형식의 값 계열로
// 읽는다). 성분이 모자란 형식은 앞 성분만 쓴다.
struct ClearValue {
    uint32 words[4] = {};

    ROCKS_RENDERBASE_API static ClearValue Float(float r, float g, float b, float a) noexcept;
    ROCKS_RENDERBASE_API static ClearValue Uint(uint32 r, uint32 g, uint32 b, uint32 a) noexcept;
    ROCKS_RENDERBASE_API static ClearValue Sint(int32 r, int32 g, int32 b, int32 a) noexcept;
};

// 쓰기 텍스처 view 안의 범위다(texel). 2D는 z·depth를 0·1로, 2D 배열은 z·depth가 층, 3D는 깊이다.
// width·height·depth가 0이면 빈 범위다(InvalidArgument).
struct WritableTextureRegion {
    uint32 x = 0;
    uint32 y = 0;
    uint32 z = 0;
    uint32 width = 0;
    uint32 height = 0;
    uint32 depth = 1;
};

// 쓰기 텍스처 view(ViewAccess::ReadWrite — Storage 텍스처의 mip 하나)의 범위를 value로 채운다
// (ClearWritableCompute, 형식·차원으로 퍼뮤테이션을 고른다). commands는 compute 명령을 받는
// 기록이다. 묶은 파이프라인·쓰기 slot 0·그리기별 상수를 바꾼다. 실패: view가 쓰기 view가 아님·빈
// 범위 InvalidArgument, 형식에 storage image 대응이 없음·cube 계열 view
// Unsupported(StorageImageFormatOf), 셰이더 집합에 그 퍼뮤테이션이 없음 NotFound(GLES의 core 밖
// 형식 등), 장치 질의·파이프라인 생성 실패는 그 오류.
ROCKS_RENDERBASE_API Result<void>
ClearWritableTexture(graphics::Device& device, graphics::CommandContext& commands, ShaderSet& set,
                     PipelineCache& cache, graphics::TextureHandle view, const ClearValue& value,
                     const WritableTextureRegion& region) noexcept;

// typed 쓰기 버퍼 view(BufferViewKind::Typed, ReadWrite, 형식 format)의 원소 [first, first +
// count)를 value로 채운다(큰 범위는 그룹 수를 감싸 낸다 — WrapGroupCount). 실패: view가 typed 쓰기
// view가 아님·format의 값 계열이 view와 다름·count 0 InvalidArgument, format에 storage image 대응이
// 없음 Unsupported, 그 밖은 ClearWritableTexture와 같다.
ROCKS_RENDERBASE_API Result<void>
ClearWritableBuffer(graphics::Device& device, graphics::CommandContext& commands, ShaderSet& set,
                    PipelineCache& cache, graphics::BufferViewHandle view,
                    graphics::PixelFormat format, const ClearValue& value, uint32 first,
                    uint32 count) noexcept;

// raster pass 안 사각형 부분 clear 설정이다. 형식은 pass attachment 형식과 같아야 한다(파이프라인이
// 그 형식으로 만들어진다). 색은 attachment 0만 지운다.
struct ClearRegionDesc {
    // 지울 사각형(px, 대상 왼쪽 위 원점)과 대상 전체 크기(그린 뒤 viewport·scissor를 되돌린다)다.
    graphics::ScissorRect rect;
    uint32 targetWidth = 0;
    uint32 targetHeight = 0;
    // pass의 색 attachment 0 형식(없으면 Unknown)과 지울지·값이다.
    graphics::PixelFormat colorFormat = graphics::PixelFormat::Unknown;
    bool clearColor = false;
    ClearValue color;
    // pass의 깊이 attachment 형식(없으면 Unknown)과 깊이·스텐실을 지울지·값이다.
    graphics::PixelFormat depthFormat = graphics::PixelFormat::Unknown;
    bool clearDepth = false;
    float depth = 0.0f;
    bool clearStencil = false;
    uint8 stencil = 0;
};

// 사각형을 지운다(viewport·scissor를 사각형으로 두고 ClearRegionVertex + ClearRegionPixel·
// DepthOnlyPixel 전체 화면 삼각형 하나 — 깊이 비교 Always, 스텐실 Always·Replace). 그린 뒤
// viewport· scissor를 대상 전체로, 스텐실 기준을 0으로 되돌린다. 실패: 지울 것이 없음·빈
// 사각형·사각형이 대상 밖·형식 없이 그 clear·스텐실 plane 없는 깊이 형식에 스텐실·색 형식이 색이
// 아님 InvalidArgument, 셰이더 집합에 항목이 없음 NotFound, 파이프라인 생성 실패는 그 오류.
ROCKS_RENDERBASE_API Result<void> ClearRegion(graphics::CommandContext& commands, ShaderSet& set,
                                              PipelineCache& cache,
                                              const ClearRegionDesc& desc) noexcept;

// raw 쓰기 버퍼 view(Indirect 용도 버퍼의 BufferViewKind::Raw, ReadWrite)의 byteOffset부터
// DispatchIndirectArguments(12 byte) count개를 arguments로
// 채운다(InitializeDispatchArgumentsCompute). 쓴 인자를 같은 기록에서 간접 dispatch에 쓰면
// IndirectArgs 전이는 장치가 메운다(그래프 밖). 실패: view가 raw 쓰기 view가 아님·byteOffset이 4
// byte 배수가 아님 InvalidArgument, count 0은 아무것도 하지 않는다. 그 밖은 ClearWritableTexture와
// 같다.
ROCKS_RENDERBASE_API Result<void>
InitializeDispatchArguments(graphics::Device& device, graphics::CommandContext& commands,
                            ShaderSet& set, PipelineCache& cache, graphics::BufferViewHandle view,
                            uint32 byteOffset, uint32 count,
                            const graphics::DispatchIndirectArguments& arguments) noexcept;

} // namespace rocks::render
