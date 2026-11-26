#pragma once

#include <Graphics/DeviceTypes.h>
#include <ShaderCore/ShaderResourceTypes.h>

// 장치 버퍼 view SRV 자원 멤버다(ShaderSystem 16.2절 SSD59 DeviceBuffer, UE SHADER_PARAMETER_SRV).
// 그래프 밖 영구 버퍼의 Read view를 담는다. Descriptor는 ShaderBuffer<T>·ShaderStructuredBuffer<T>·
// ShaderByteAddressBuffer이고 생성 HLSL은 Buffer<T>·StructuredBuffer<T>·ByteAddressBuffer
// <멤버>;다. 파이프라인 이름 표(PipelineBindingNames::bufferViews)에 멤버 이름이 있어야
// 묶인다(RenderBase 9.13절 7항).
//
// 사용 예:
//   render::DeviceBufferParameter<shader::ShaderStructuredBuffer<shader::ShaderFloat4>> lights;
//   parameters.lights.view = lightView;
namespace rocks::render {

// 모든 DeviceBufferParameter<D>의 값 배치다.
struct DeviceBufferValue {
    graphics::BufferViewHandle view;
};

template <class Descriptor>
struct alignas(16) DeviceBufferParameter : DeviceBufferValue {};

} // namespace rocks::render
