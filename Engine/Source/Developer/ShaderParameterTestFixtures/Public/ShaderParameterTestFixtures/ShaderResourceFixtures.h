#pragma once

#include <Base/Types.h>
#include <RenderBase/ResourceParameters/DeviceBufferParameter.h>
#include <RenderBase/ResourceParameters/DeviceTextureParameter.h>
#include <RenderGraph/Parameters/GraphBufferAccessParameter.h>
#include <RenderGraph/Parameters/GraphBufferSrvParameter.h>
#include <RenderGraph/Parameters/GraphBufferUavParameter.h>
#include <RenderGraph/Parameters/GraphRenderTargetParameter.h>
#include <RenderGraph/Parameters/GraphTextureAccessParameter.h>
#include <RenderGraph/Parameters/GraphTextureParameter.h>
#include <RenderGraph/Parameters/GraphTextureSrvParameter.h>
#include <RenderGraph/Parameters/GraphTextureUavParameter.h>
#include <ShaderCore/ShaderParameterTypes.h>
#include <ShaderCore/ShaderResourceTypes.h>

// 셰이더 파라미터 구조체 자원 멤버 시험 fixture다(Docs/Design/ShaderSystem 16.2절 SSD66). 빌드가
// ShaderResourceFixtures.shaderparams.h(생성 표 — 자원 목록)와 /Engine/Generated/
// ShaderParameterTestFixtures/<이름>.rsh(생성 HLSL — 전역 자원·짝 샘플러)를 만든다. 주석의 오프셋은
// C++ 오프셋이다(자원 틀은 alignas(16)).
namespace rocks::shader_test_fixtures {

// 포함용 자원 구조체다(상수 없음 — RenderBase 9.6절 SharedResourceStruct 모양). 바깥 구조체가
// 멤버 이름 scene으로 포함하면 scene_depth·scene_color다.
ROCKS_SHADER_PARAMETERS()
struct alignas(16) FixtureSceneTextures {
    render::GraphTextureParameter<shader::ShaderTexture2D<float>> depth; // 0 (짝 depthSampler)
    render::GraphTextureSrvParameter<shader::ShaderTexture2D<>> color;   // 짝 colorSampler
};

// 상수와 장치 자원을 함께 가진 구조체(cbuffer FixtureMaterial + 전역 자원)다.
ROCKS_SHADER_PARAMETERS()
struct alignas(16) FixtureMaterial {
    shader::ShaderFloat4 tint; // 0
    float roughness;           // 16
    // 자원 부분(상수 부분 32 byte 뒤)
    render::DeviceTextureParameter<shader::ShaderTexture2D<>> albedo;
    render::DeviceTextureParameter<shader::ShaderTexture2DArray<shader::ShaderUint4>> codes;
    render::DeviceTextureParameter<shader::ShaderTexture2D<float, shader::ShaderComparisonSampler>>
        shadow;
    render::DeviceTextureParameter<shader::ShaderTexture2DMS<>> coverage;
    render::DeviceBufferParameter<shader::ShaderBuffer<shader::ShaderFloat4>> lights;
    FixtureSceneTextures scene;
};

// compute pass 구조체(그래프 자원·UAV·접근만 선언)다.
ROCKS_SHADER_PARAMETERS(Name = "FixtureReduceConstants")
struct alignas(16) FixtureReduce {
    shader::ShaderUint4 size; // 0
    render::GraphTextureSrvParameter<shader::ShaderTexture2D<>> source;
    render::GraphTextureUavParameter<shader::ShaderRWTexture2D<float, shader::ShaderImageR32f>>
        target;
    render::GraphBufferSrvParameter<shader::ShaderStructuredBuffer<shader::ShaderFloat4x4>> bones;
    render::GraphBufferSrvParameter<shader::ShaderByteAddressBuffer> words;
    render::GraphBufferUavParameter<shader::ShaderRWStructuredBuffer<uint32>> counters;
    render::GraphBufferUavParameter<shader::ShaderRWBuffer<uint32, shader::ShaderImageR32ui>>
        histogram;
    render::GraphTextureAccessParameter history;
    render::GraphBufferAccessParameter arguments;
};

// raster pass 구조체(상수 없음, 렌더 타깃 묶음)다.
ROCKS_SHADER_PARAMETERS()
struct alignas(16) FixtureComposite {
    render::GraphTextureParameter<shader::ShaderTexture2D<>> source;
    render::DeviceTextureParameter<shader::ShaderTexture2D<>> overlay;
    render::GraphBufferSrvParameter<shader::ShaderBuffer<float>> weights;
    render::GraphRenderTargetParameter targets;
};

} // namespace rocks::shader_test_fixtures
