#pragma once

#include <RenderGraph/GraphTypes.h>
#include <ShaderCore/ShaderResourceTypes.h>

// 그래프 텍스처 쓰기 view 자원 멤버다(ShaderSystem 16.2절 SSD59 GraphTextureUav, UE
// SHADER_PARAMETER_RDG_TEXTURE_UAV). Descriptor는 ShaderRWTexture2D<원소, 형식> 등이다. view는
// RenderGraph::CreateTextureView의 ReadWrite view이고 의존은 UavCompute다(Compute pass만 —
// ShaderSystem 7.2절 4항). 쓰기 slot은 파이프라인 이름 표 writableTextures의 멤버 이름이다.
//
// 사용 예:
//   render::GraphTextureUavParameter<shader::ShaderRWTexture2D<float, shader::ShaderImageR32f>>
//   target; parameters->target.view = mipK1Write;
namespace rocks::render {

// 모든 GraphTextureUavParameter<D>의 값 배치다.
struct GraphTextureUavValue {
    GraphTextureView view;
};

template <class Descriptor>
struct alignas(16) GraphTextureUavParameter : GraphTextureUavValue {};

} // namespace rocks::render
