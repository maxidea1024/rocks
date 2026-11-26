#pragma once

#include <RenderGraph/GraphTypes.h>
#include <ShaderCore/ShaderResourceTypes.h>

// 그래프 버퍼 쓰기 view 자원 멤버다(ShaderSystem 16.2절 SSD59 GraphBufferUav, UE
// SHADER_PARAMETER_RDG_BUFFER_UAV). Descriptor는
// ShaderRWStructuredBuffer<T>·ShaderRWByteAddressBuffer· ShaderRWBuffer<T, 형식>이다. view는
// ReadWrite 버퍼 view이고 의존은 UavCompute다(Compute pass만). 쓰기 slot은 파이프라인 이름 표
// writableBuffers의 멤버 이름이다.
//
// 사용 예:
//   render::GraphBufferUavParameter<shader::ShaderRWByteAddressBuffer> counters;
namespace rocks::render {

// 모든 GraphBufferUavParameter<D>의 값 배치다.
struct GraphBufferUavValue {
    GraphBufferView view;
};

template <class Descriptor>
struct alignas(16) GraphBufferUavParameter : GraphBufferUavValue {};

} // namespace rocks::render
