#pragma once

#include <RenderBase/ResourceParameters/SamplerOverride.h>
#include <RenderGraph/GraphTypes.h>
#include <ShaderCore/ShaderResourceTypes.h>

// 그래프 텍스처 읽기 view 자원 멤버다(ShaderSystem 16.2절 SSD59 GraphTextureSrv, UE
// SHADER_PARAMETER_RDG_TEXTURE_SRV). view는 RenderGraph::CreateTextureView의 Read view이고 의존은
// 그 view 범위의 SRV 접근이다(mip k 읽기 + mip k + 1 쓰기 한 pass가 된다).
//
// 사용 예:
//   render::GraphTextureSrvParameter<shader::ShaderTexture2D<>> source;
//   parameters->source.view = mipK;
namespace rocks::render {

// 모든 GraphTextureSrvParameter<D>의 값 배치다.
struct GraphTextureSrvValue {
    GraphTextureView view;
    SamplerOverride sampler;
};

template <class Descriptor>
struct alignas(16) GraphTextureSrvParameter : GraphTextureSrvValue {};

} // namespace rocks::render
