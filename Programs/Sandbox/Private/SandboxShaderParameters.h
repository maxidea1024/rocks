#pragma once

#include <Base/Types.h>
#include <RenderBase/ResourceParameters/DeviceBufferParameter.h>
#include <RenderBase/ResourceParameters/DeviceTextureParameter.h>
#include <ShaderCore/ShaderParameterTypes.h>
#include <ShaderCore/ShaderResourceTypes.h>

// Sandbox의 생성 셰이더 파라미터 구조체다(Docs/Design/ShaderSystem 16.2절 SSD58 3). 빌드가
// SandboxShaderParameters.shaderparams.h(생성 표)와 /Engine/Generated/Sandbox/<이름>.rsh(생성
// HLSL)를 만든다. 셰이더는 Shaders/GeneratedParameters.rsf이고
// self-test(ShaderParameterSelfTest.cpp)가 이 구조체를 그대로 올려 그린 픽셀을 CPU 식과 대조한다 —
// 오프셋이 하나라도 어긋나면 값이 크게 바뀐다.
namespace sandbox::shaders {

// 중첩 구조체다(3성분 뒤 스칼라 — 생성 HLSL이 padding을 넣는다).
ROCKS_SHADER_PARAMETERS()
struct alignas(16) GeneratedProbeLight {
    rocks::shader::ShaderFloat3 direction;
    float intensity;
    rocks::shader::ShaderFloat4 color;
};

// cbuffer GeneratedProbe다(행렬·padding·int 슬롯·배열·중첩).
ROCKS_SHADER_PARAMETERS(Name = "GeneratedProbe")
struct alignas(16) GeneratedProbeParameters {
    rocks::shader::ShaderFloat4x4 transform;
    rocks::shader::ShaderFloat3 tint;
    float exposure;
    rocks::shader::ShaderInt3 offsets;
    rocks::int32 layer;
    rocks::shader::ShaderFloat4 weights[3];
    GeneratedProbeLight light;
    float tail;
};

// 루트 상수 GeneratedDraw다.
ROCKS_SHADER_PARAMETERS(Name = "GeneratedDraw", RootConstants)
struct alignas(16) GeneratedProbeDraw {
    rocks::shader::ShaderFloat3 bias;
    float scale;
    rocks::shader::ShaderUint4 select;
};

// 상수와 장치 자원 멤버를 가진 구조체(cbuffer GeneratedResources +
// albedo·albedoSampler·codes·lights)다 (16.2절 SSD66 4, self-test ShaderResourceSelfTest.cpp).
// 셰이더는 Shaders/GeneratedResources.rsf다.
ROCKS_SHADER_PARAMETERS(Name = "GeneratedResources")
struct alignas(16) GeneratedResourceParameters {
    rocks::shader::ShaderFloat4 scale;
    rocks::render::DeviceTextureParameter<rocks::shader::ShaderTexture2D<>> albedo;
    rocks::render::DeviceTextureParameter<
        rocks::shader::ShaderTexture2D<rocks::shader::ShaderUint4>>
        codes;
    rocks::render::DeviceBufferParameter<rocks::shader::ShaderBuffer<rocks::shader::ShaderFloat4>>
        lights;
};

} // namespace sandbox::shaders
