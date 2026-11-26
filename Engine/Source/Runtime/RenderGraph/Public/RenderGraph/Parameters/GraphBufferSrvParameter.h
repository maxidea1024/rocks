#pragma once

#include <RenderGraph/GraphTypes.h>
#include <ShaderCore/ShaderResourceTypes.h>

// 그래프 버퍼 읽기 view 자원 멤버다(ShaderSystem 16.2절 SSD59 GraphBufferSrv, UE
// SHADER_PARAMETER_RDG_BUFFER_SRV). Descriptor는 ShaderBuffer<T>·ShaderStructuredBuffer<T>·
// ShaderByteAddressBuffer다. view는 RenderGraph::CreateBufferView의 Read view이고 의존은 버퍼
// 전체의 SRV 접근이다. slot은 파이프라인 이름 표 bufferViews의 멤버 이름이다.
//
// 사용 예:
//   render::GraphBufferSrvParameter<shader::ShaderStructuredBuffer<shader::ShaderFloat4>> lights;
namespace rocks::render {

// 모든 GraphBufferSrvParameter<D>의 값 배치다.
struct GraphBufferSrvValue {
    GraphBufferView view;
};

template <class Descriptor>
struct alignas(16) GraphBufferSrvParameter : GraphBufferSrvValue {};

} // namespace rocks::render
