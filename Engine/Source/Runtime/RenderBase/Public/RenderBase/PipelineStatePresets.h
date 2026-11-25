#pragma once

#include <Graphics/DeviceTypes.h>
#include <Graphics/PipelineState.h>

// 파이프라인 상태 프리셋이다(Docs/Design/RenderBase 11.2절 "상태 프리셋", 11.4절 1항). 자주 쓰는
// 블렌드·래스터·깊이 조합을 Graphics 값 타입으로 돌려준다. 상태는 파이프라인에 들어가므로 UE 정적
// 상태 템플릿(RHIStaticStates.h)과 달리 캐시가 없다 — 같은 값이면 파이프라인 캐시 키가 같다.
//
// 사용 예(알파 블렌드 UI, 깊이 끔, 컬 없음):
//   graphics::PipelineDesc desc;
//   render::SetBlend(desc, render::AlphaBlend());
//   desc.raster = render::NoCull();
//   desc.depthStencil = render::DepthDisabled();
//
// 사용 예(불투명 메시, reversed-Z 깊이 읽기쓰기, 뒤면 컬):
//   desc.depthFormat = PixelFormat::D32Float;
//   desc.depthStencil = render::DepthReadWrite();
//   desc.raster = render::BackFaceCull();      // 행렬식이 음수면 FrontFaceCull()
namespace rocks::render {

// --- 블렌드 ---

// 불투명이다(블렌드 끔, 모든 채널 쓰기 — UE TStaticBlendState<> 기본값).
constexpr graphics::ColorTargetBlend OpaqueBlend() noexcept {
    return graphics::ColorTargetBlend{};
}

// 소스 알파로 섞는 일반 알파 블렌드다(premultiplied 아님). 색은 SourceAlpha·InverseSourceAlpha,
// 알파는 One·InverseSourceAlpha다(대상 알파가 덮인 비율로 쌓인다).
constexpr graphics::ColorTargetBlend AlphaBlend() noexcept {
    graphics::ColorTargetBlend blend;
    blend.enable = true;
    blend.sourceColor = graphics::BlendFactor::SourceAlpha;
    blend.destinationColor = graphics::BlendFactor::InverseSourceAlpha;
    blend.sourceAlpha = graphics::BlendFactor::One;
    blend.destinationAlpha = graphics::BlendFactor::InverseSourceAlpha;
    return blend;
}

// 미리 곱한 알파 블렌드다(소스 색이 이미 알파를 곱했다). 색·알파 모두 One·InverseSourceAlpha다.
constexpr graphics::ColorTargetBlend PremultipliedAlphaBlend() noexcept {
    graphics::ColorTargetBlend blend;
    blend.enable = true;
    blend.sourceColor = graphics::BlendFactor::One;
    blend.destinationColor = graphics::BlendFactor::InverseSourceAlpha;
    blend.sourceAlpha = graphics::BlendFactor::One;
    blend.destinationAlpha = graphics::BlendFactor::InverseSourceAlpha;
    return blend;
}

// 가산 블렌드다(색·알파 모두 One·One). 빛·입자 누적에 쓴다.
constexpr graphics::ColorTargetBlend AdditiveBlend() noexcept {
    graphics::ColorTargetBlend blend;
    blend.enable = true;
    blend.sourceColor = graphics::BlendFactor::One;
    blend.destinationColor = graphics::BlendFactor::One;
    blend.sourceAlpha = graphics::BlendFactor::One;
    blend.destinationAlpha = graphics::BlendFactor::One;
    return blend;
}

// 모든 색 타깃(MaxColorTargets개)에 같은 블렌드를 넣는다.
constexpr void SetBlend(graphics::PipelineDesc& desc,
                        const graphics::ColorTargetBlend& blend) noexcept {
    for (graphics::ColorTargetBlend& target : desc.blend) {
        target = blend;
    }
}

// --- 래스터 ---

// 컬링 없음(양면)이다. 화면 채우기·양면 재질에 쓴다.
constexpr graphics::RasterState NoCull() noexcept {
    return graphics::RasterState{};
}

// 뒤면 컬이다(일반 메시 — 앞면은 화면 반시계, ShaderSystem 7.2절 8항).
constexpr graphics::RasterState BackFaceCull() noexcept {
    graphics::RasterState raster;
    raster.cullMode = graphics::CullMode::Back;
    return raster;
}

// 앞면 컬이다(물체·view 행렬식이 음수인 그리기의 반전 컬).
constexpr graphics::RasterState FrontFaceCull() noexcept {
    graphics::RasterState raster;
    raster.cullMode = graphics::CullMode::Front;
    return raster;
}

// --- 깊이 ---

// 깊이 시험·쓰기를 모두 끈다(깊이 attachment 없는 파이프라인의 값).
constexpr graphics::DepthStencilState DepthDisabled() noexcept {
    return graphics::DepthStencilState{};
}

// 깊이 읽기만 한다. reversed-Z에서 GreaterEqual이 "가깝거나 같음"이다(UE CF_DepthNearOrEqual).
constexpr graphics::DepthStencilState DepthRead() noexcept {
    graphics::DepthStencilState depth;
    depth.depthTest = true;
    depth.depthWrite = false;
    depth.depthCompare = graphics::CompareFunction::GreaterEqual;
    return depth;
}

// 깊이를 읽고 쓴다(시험 GreaterEqual — 불투명 메시의 기본).
constexpr graphics::DepthStencilState DepthReadWrite() noexcept {
    graphics::DepthStencilState depth;
    depth.depthTest = true;
    depth.depthWrite = true;
    depth.depthCompare = graphics::CompareFunction::GreaterEqual;
    return depth;
}

} // namespace rocks::render
