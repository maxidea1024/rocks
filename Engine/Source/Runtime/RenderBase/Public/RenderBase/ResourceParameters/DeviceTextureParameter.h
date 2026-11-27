#pragma once

#include <Graphics/DeviceTypes.h>
#include <RenderBase/ResourceParameters/SamplerOverride.h>
#include <ShaderCore/ShaderResourceTypes.h>

// 장치 SRV 텍스처 자원 멤버다(ShaderSystem 16.2절 SSD59 DeviceTexture, UE
// SHADER_PARAMETER_TEXTURE와 SHADER_PARAMETER_SAMPLER). 그래프 밖 영구 텍스처(또는 Read view)를
// 담는다. Descriptor는 ShaderCore/ShaderResourceTypes.h의 SRV 텍스처 서술자(ShaderTexture2D<>
// 등)이고 생성 HLSL은 Texture2D<float4> <멤버>;와(float 원소면) 짝 SamplerState <멤버>Sampler;다.
//
// 값은 ParameterBatch::SetParameters가 읽는다(소유 스레드에서 Validate한다). 렌더 그래프 pass
// 구조체에 두면 GraphPassContext::SetParameters가 검증 없이 묶는다(그래프 의존이 아니다).
//
// 사용 예:
//   ROCKS_SHADER_PARAMETERS()
//   struct alignas(16) DecalParameters {
//       shader::ShaderFloat4 tint;
//       render::DeviceTextureParameter<shader::ShaderTexture2D<>> albedo;
//   };
//   parameters.albedo.texture = albedoTexture;
namespace rocks::render {

// 모든 DeviceTextureParameter<D>의 값 배치다(묶는 쪽이 서술자와 무관하게 읽는다).
struct DeviceTextureValue {
    graphics::TextureHandle texture;
    SamplerOverride sampler;
};

template <class Descriptor>
struct alignas(16) DeviceTextureParameter : DeviceTextureValue {};

} // namespace rocks::render
