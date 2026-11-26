#pragma once

#include <RenderBase/ResourceParameters/SamplerOverride.h>
#include <RenderGraph/GraphTypes.h>
#include <ShaderCore/ShaderResourceTypes.h>

// 그래프 텍스처 전체 SRV 자원 멤버다(ShaderSystem 16.2절 SSD59 GraphTexture, UE
// SHADER_PARAMETER_RDG_TEXTURE). Descriptor는 SRV 텍스처 서술자(ShaderTexture2D<> 등)다. 수집기
// (CollectGraphPassAccesses)가 텍스처 전체의 SRV 접근(Raster SrvGraphics·Compute SrvCompute)으로
// 의존을 만들고, GraphPassContext::SetParameters가 pass의 장치 텍스처와 짝 샘플러를 묶는다.
// null이면 의존이 아니다(셰이더가 읽으면 수집기가 거부한다).
//
// 사용 예:
//   render::GraphTextureParameter<shader::ShaderTexture2D<>> sceneColor;
//   parameters->sceneColor.texture = sceneColorTexture;
namespace rocks::render {

// 모든 GraphTextureParameter<D>의 값 배치다.
struct GraphTextureValue {
    GraphTexture texture;
    SamplerOverride sampler;
};

template <class Descriptor>
struct alignas(16) GraphTextureParameter : GraphTextureValue {};

} // namespace rocks::render
