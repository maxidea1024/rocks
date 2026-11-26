#pragma once

#include <Core/Error.h>
#include <GraphicsTypes/ChannelSwizzle.h>
#include <GraphicsTypes/PixelFormat.h>
#include <GraphicsTypes/TextureDimension.h>
#include <RenderBase/CookedTexture.h>
#include <RenderBase/RenderHandle.h>

// 텍스처 렌더 캐시(TextureRenderCache)가 쓰는 쪽에 주는 값이다(Docs/Design/RenderBase 18.5절 8항).
namespace rocks {

// 텍스처 렌더 캐시 항목의 상태다(Docs/Design/RenderBase 18.5절 8항).
enum class TextureRenderState : uint8 {
    // 에셋을 요청했고 lease를 기다린다(handle 없음).
    Loading,
    // handle을 예약해 업로드를 게시했다(그리면 typed 대체 — RenderBase 6.2절).
    Uploading,
    // ResourceReady 통지를 받았다.
    Ready,
    // 실패했다. 로드·변환·검사·게시 실패면 handle이 없고, ResourceFailed면 handle이 있다(그리면
    // typed 대체, Release가 Retire한다).
    Failed,
    // CPU Availability 텍스처다(올리지 않는다 — handle 없음, 그리는 쪽이 Black2D로 대체).
    CpuPlaceholder
};

// 상태 이름이다(열거자 이름, 정적 수명, 범위 밖 "Unknown").
ROCKS_WORLDINTEGRATION_API const char* TextureRenderStateName(TextureRenderState state) noexcept;

// 캐시 항목의 표시 값 사본이다(프레임 packet에 담는 값 — 캐시 포인터는 렌더 lane에 넘기지 않는다).
// 차원·형식·크기는 Uploading·Ready·Failed(handle 있음)에서 채워지고, 그 밖에서는 기본값이다.
struct TextureRenderInfo {
    render::RenderHandle handle;
    TextureRenderState state = TextureRenderState::Loading;
    // Failed면 그 오류다(로드·변환·검사·게시 오류, ResourceFailed의 장치 오류).
    ErrorCode error = ErrorCode::None;
    // 검사(CheckCookedTexture)가 거부했으면 그 원인이다.
    render::CookedTextureIssue issue = render::CookedTextureIssue::None;
    graphics::TextureDimension dimension = graphics::TextureDimension::Texture2D;
    graphics::PixelFormat format = graphics::PixelFormat::Unknown;
    // 쿠킹 swizzle(sampler type 계약 — 셰이더가 적용한다, RenderBase 6.5절)이다.
    graphics::ChannelSwizzle swizzle;
    // 기준 크기(mip 0)다. depth는 volume만(아니면 1), slices는 배열 층 수·cube 면 수(6 × cube
    // 수)다(2D·volume은 1).
    uint32 width = 0;
    uint32 height = 0;
    uint32 depth = 1;
    uint32 slices = 1;
    uint32 mips = 0;
};

} // namespace rocks
