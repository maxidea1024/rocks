#pragma once

#include <Base/Types.h>

// 장치 능력(RenderBase 4.4절 GX1)이다. 셰이더 형식 정의(ShaderSystem 12.1절)의 상한이 "쿠킹 때
// 가정하는 값"이고 이 값은 "실행 장치의 실제 값"이다. backend가 장치를 만들 때 한 번 채우고 장치
// 수명 동안 바꾸지 않는다(Device::Caps, 잠금 없이 여러 스레드가 읽는다). 픽셀 형식별 능력은
// Device::GetPixelFormatCaps·GetPixelFormatSampleCounts다(PixelFormat.ko.md 7·8절).
//
// 값의 출처는 backend마다 다르다(Docs/Design/RenderBase.ko.md 4.4절 GX1 "구현" 표).
// - D3D12·D3D11: API 상수(d3d12.h·d3d11.h의 D3D*_REQ_*·COMMONSHADER_*)와 CheckFeatureSupport.
// - Vulkan: VkPhysicalDeviceLimits·기능 질의(장치가 켠 기능만 참이다).
// - OpenGL ES: glGetIntegerv 한도와 확장 문자열.
// 장치가 알려 주지 않는 값은 보수적인 값(사양 최소 보장)이며 필드 주석에 그렇게 적는다. 가짜로
// 넓은 값을 만들지 않는다.
//
// 사용 예(형식 정의 상한과 장치 값 대조, Renderer RQ11·RQ22):
//   const DeviceCaps& caps = device.Caps();
//   if (caps.limits.colorAttachments >= 8 && caps.limits.pixel.sampledTextures >= 128) { ... }
namespace rocks::graphics {

// 셰이더 단계 하나가 동시에 묶을 수 있는 자원 수다. 종류는 ShaderSystem 7.2절 3항의 구분이다.
struct StageResourceLimits {
    // 상수 버퍼(cbuffer·uniform block) 수다. 루트 상수(그리기별 상수)는 들지 않는다.
    uint32 constantBuffers = 0;
    // SRV 텍스처와 typed buffer SRV 수다. OpenGL ES는 typed buffer SRV(samplerBuffer)도 텍스처
    // 유닛을 차지하므로 둘의 합이 이 한도 안이다(ShaderSystem 7.2절 4항, RQ10).
    uint32 sampledTextures = 0;
    uint32 samplers = 0;
    // 구조화·raw 버퍼(SRV·UAV 합) 수다. OpenGL ES 3.2의 VS·PS 최소 보장은 0이다.
    uint32 storageBuffers = 0;
    // storage image(UAV 텍스처·typed buffer UAV) 수다.
    uint32 storageImages = 0;
    // OpenGL ES 상수 에뮬레이션 vec4 수다(GL_MAX_*_UNIFORM_VECTORS, ShaderSystem 7.2절 4항). 다른
    // backend는 상수 버퍼 한도로 판단하므로 NotApplicableLimit다.
    uint32 uniformVectors = 0;
};

// 해당 backend에서 의미가 없는 한도 값이다(그 한도로 판단하지 않는다).
inline constexpr uint32 NotApplicableLimit = 0;

// 장치 전역 한도다.
struct DeviceLimits {
    // 단계별 자원 한도다.
    StageResourceLimits vertex;
    StageResourceLimits pixel;
    StageResourceLimits compute;
    // 파이프라인(모든 단계 합) 한도다. Vulkan
    // maxDescriptorSetSampledImages·Samplers·StorageBuffers· StorageImages, D3D12 binding tier의
    // heap 한도, OpenGL ES GL_MAX_COMBINED_*. D3D11은 단계별 한도의 합이 곧 한도라 단계 값 × 단계
    // 수(VS·PS·CS 3)다.
    uint32 pipelineSampledTextures = 0;
    uint32 pipelineSamplers = 0;
    uint32 pipelineStorageBuffers = 0;
    uint32 pipelineStorageImages = 0;
    // 단계 하나의 모든 자원(상수 버퍼·SRV·storage·color attachment) 합 한도다(Vulkan
    // maxPerStageResources, ShaderSystem 개정 13). 다른 backend는 NotApplicableLimit다.
    uint32 perStageResources = 0;
    // 모든 단계 UAV(storage buffer·image) 합 한도다(형식 정의 uavTotal). D3D12 Tier 1·D3D11은
    // FL 11.0 8, FL 11.1 이상 64. Vulkan·OpenGL ES는 단계별 한도로 판단하므로 NotApplicableLimit다.
    uint32 pipelineUavs = 0;
    // 동시에 묶는 descriptor set 수다(Vulkan maxBoundDescriptorSets). 다른 backend는
    // NotApplicableLimit다.
    uint32 boundDescriptorSets = 0;
    // color attachment(MRT) 수다(D3D 8, Vulkan maxColorAttachments, OpenGL ES GL_MAX_DRAW_BUFFERS와
    // GL_MAX_COLOR_ATTACHMENTS의 작은 값).
    uint32 colorAttachments = 0;
    // 상수 버퍼 하나의 최대 byte다(D3D 65536, Vulkan maxUniformBufferRange, OpenGL ES
    // GL_MAX_UNIFORM_BLOCK_SIZE).
    uint32 constantBufferBytes = 0;
    // 그리기별 루트 상수(push constant) 최대 byte다(D3D12 루트 상수 64 DWORD 중 Rocks 배치가 쓰는
    // 몫, Vulkan maxPushConstantsSize). D3D11·OpenGL ES는 상수 버퍼·uniform 에뮬레이션이라 형식
    // 정의의 128이다.
    uint32 rootConstantBytes = 0;
    // 버퍼 view·상수 묶기의 시작 byte 정렬이다. 원소 크기 배수 규칙(ResolveBufferView)에 더해 이
    // 배수여야 한다. D3D는 원소 정렬 외 제약이 없어 1(raw는 공통 규칙 16), 상수는 256(D3D12
    // D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, D3D11.1 상수 16개 단위).
    uint32 constantBufferOffsetAlignment = 0;
    uint32 storageBufferOffsetAlignment = 0;
    uint32 texelBufferOffsetAlignment = 0;
    // 구조화·raw view 하나의 최대 byte다(Vulkan maxStorageBufferRange, OpenGL ES
    // GL_MAX_SHADER_STORAGE_BLOCK_SIZE, D3D는 원소 2^27개 × 4 byte).
    uint64 storageBufferBytes = 0;
    // typed buffer view 하나의 최대 원소 수다(D3D 2^27, Vulkan maxTexelBufferElements, OpenGL ES
    // GL_MAX_TEXTURE_BUFFER_SIZE).
    uint32 texelBufferElements = 0;
    // 버퍼 하나의 최대 byte다. D3D는 보장식 min(max(128, 전용 VRAM × 0.25), 2048) MiB(d3d12.h
    // D3D12_REQ_RESOURCE_SIZE_IN_MEGABYTES_EXPRESSION_*), Vulkan은 maxMemoryAllocationSize(1.1
    // maintenance3). OpenGL ES는 조회 수단이 없어 보수적으로 128 MiB(D3D 보장식의 하한)다.
    uint64 bufferBytes = 0;
    // 정점 속성·stream 수다(Graphics 정점 배치는 이와 따로 16·8로 묶는다, RenderBase 10.4절).
    uint32 vertexAttributes = 0;
    uint32 vertexStreams = 0;
    // 단계 사이 vec4 수다(형식 정의 interStageVectors. D3D 32 register, Vulkan
    // maxVertexOutputComponents / 4, OpenGL ES GL_MAX_VARYING_VECTORS).
    uint32 interStageVectors = 0;
    // compute dispatch 그룹 수(축별)·그룹 크기(축별)·그룹 하나의 invocation 수·group shared 메모리
    // byte다.
    uint32 computeGroupCount[3] = {};
    uint32 computeGroupSize[3] = {};
    uint32 computeGroupInvocations = 0;
    uint32 computeSharedMemoryBytes = 0;
    // 텍스처 크기 한도다(2D 한 변, 3D 한 변, cube 한 변, array 층 수).
    uint32 textureSize2D = 0;
    uint32 textureSize3D = 0;
    uint32 textureSizeCube = 0;
    uint32 textureArrayLayers = 0;
    // 샘플러 최대 이방성이다(1이면 이방성 없음).
    uint32 samplerAnisotropy = 0;
};

// 장치 기능 표시다. 장치가 지원하고 이 backend가 켠 것만 참이다.
struct DeviceFeatures {
    // compute 셰이더를 실행할 수 있다(D3D FL 11.0·Vulkan·OpenGL ES 3.1은 늘 참).
    bool computeShaders = false;
    // 깊이 clamp(깊이 clip 끄기)를 쓸 수 있다(D3D 늘 참, Vulkan depthClamp, OpenGL ES
    // GL_EXT_depth_clamp).
    bool depthClamp = false;
    // VS가 렌더 타깃 array 층(SV_RenderTargetArrayIndex)을 낼 수 있다(D3D12 OPTIONS
    // VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation, D3D11 OPTIONS3,
    // Vulkan VK_EXT_shader_viewport_index_layer). OpenGL ES는 ES 확장이 없어 거짓이다.
    bool vertexShaderLayerOutput = false;
    // 64 bit atomic(typed 자원·버퍼)을 쓸 수 있다(D3D12 OPTIONS9
    // AtomicInt64OnTypedResourceSupported, Vulkan VK_KHR_shader_atomic_int64 +
    // VK_EXT_shader_image_atomic_int64. UE와 같은 조건). D3D11 SM 5.0·OpenGL ES는 거짓이다(UE의
    // 벤더 확장 경로는 쓰지 않는다).
    bool atomic64 = false;
    // 간접 그리기 인자의 첫 인스턴스가 0이 아니어도 된다(Vulkan drawIndirectFirstInstance, D3D 늘
    // 참, OpenGL ES 3.2는 0이어야 한다).
    bool indirectFirstInstance = false;
    // 간접 그리기 여러 개를 한 호출로 낸다(Vulkan multiDrawIndirect, D3D12 ExecuteIndirect 늘 참,
    // D3D11·OpenGL ES 3.2 core는 거짓).
    bool multiDrawIndirect = false;
    // cube array 텍스처(Vulkan imageCubeArray, D3D·OpenGL ES 3.2 늘 참).
    bool textureCubeArray = false;
    // sRGB 짝 형식 view(PixelFormat.ko.md 9절). OpenGL ES는 형식을 재해석하는 view가 없어 거짓이다.
    bool srgbPairView = false;
    // VS·PS가 storage 자원에 쓸 수 있다(Vulkan vertexPipelineStoresAndAtomics·
    // fragmentStoresAndAtomics, D3D11.1 이상 VS·PS UAV, D3D12 늘 참, OpenGL ES는 단계별 storage
    // 한도가 0보다 클 때). 형식 정의는 UAV를 CS만 쓰므로 판단용으로만 공개한다.
    bool vertexStores = false;
    bool pixelStores = false;
    // HDR10(ST.2084·BT.2020) 출력 중인 디스플레이가 이 어댑터에 있다(DXGI IDXGIOutput6 색 공간, UE
    // WindowsD3D12Device.cpp와 같은 판정, 장치를 만들 때 한 번 본다). Vulkan·OpenGL ES는 surface
    // 질의 전에는 알 수 없어 거짓이다(스왑체인 HDR 단계에서 surface별로 판정한다).
    bool hdrOutput = false;
    // D3D12 resource binding tier(1–3)다. 다른 backend는 0이다.
    uint8 resourceBindingTier = 0;
    // 장치가 받는 최고 셰이더 모델(major × 10 + minor, 예: 66)이다. D3D12는
    // D3D12_FEATURE_SHADER_MODEL, D3D11은 FL 11.0의 50이다. Vulkan·OpenGL ES는 0이다(SPIR-V·ESSL
    // 버전은 형식 정의가 정한다).
    uint8 shaderModel = 0;
};

// 장치 능력 전체다(Device::Caps).
struct DeviceCaps {
    DeviceLimits limits;
    DeviceFeatures features;
};

// MSAA 표본 수 집합의 비트다(Device::GetPixelFormatSampleCounts, 비트 n = 2^n 표본). 1표본(비트
// 0)은 형식을 렌더 타깃·깊이로 만들 수 있을 때 늘 포함한다.
constexpr uint32 SampleCountBit(uint32 samples) noexcept {
    uint32 bit = 0;
    while (samples > 1) {
        samples >>= 1;
        ++bit;
    }

    return 1u << bit;
}

// 장치가 질의하는 MSAA 표본 수의 최대값이다(2·4·8·16·32).
inline constexpr uint32 MaxSampleCount = 32;

} // namespace rocks::graphics
