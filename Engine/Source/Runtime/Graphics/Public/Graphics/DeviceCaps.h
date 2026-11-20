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
    // 상수 버퍼(cbuffer·uniform block) 수다. 루트 상수(그리기별 상수)는 들지 않는다: D3D11은 루트
    // 상수 에뮬레이션이 상수 버퍼 slot 하나를 쓰므로 14 - 1 = 13, D3D12 Tier 1·2는 루트 상수가
    // descriptor table 밖이라 table CBV 한도 14 그대로다.
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

// Rocks 루트 상수 계약 크기다(ShaderSystem 7.2절 2항: 32 DWORD = 128 byte, Vulkan
// maxPushConstantsSize 최소 보장과 같다). 루트 상수를 에뮬레이션하는 backend(D3D11·OpenGL ES)와
// D3D12가 DeviceLimits::rootConstantBytes로 낸다.
inline constexpr uint32 RootConstantContractBytes = 128;

// 장치 전역 한도다.
struct DeviceLimits {
    // 단계별 자원 한도다.
    StageResourceLimits vertex;
    StageResourceLimits pixel;
    StageResourceLimits compute;
    // 파이프라인(모든 단계 합) 한도다. Vulkan
    // maxDescriptorSetSampledImages·Samplers·StorageBuffers·StorageImages, D3D12 binding tier의
    // heap 한도, OpenGL ES GL_MAX_COMBINED_*. D3D11·D3D12 Tier 1–2는 단계마다 slot이 따로라 그래픽
    // 파이프라인(VS·PS) 기준 단계 값 × 2다(compute 파이프라인은 CS 하나라 단계 값). UAV는 모든
    // 단계가 함께 쓰는 slot이라 단계 값 그대로다.
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
    // 한 그리기가 고를 수 있는 viewport 수다(D3D 16, Vulkan은 multiViewport를 켰으면 maxViewports·
    // 아니면 1, OpenGL ES 1). SV_ViewportArrayIndex 출력은 이 안의 번호만 쓴다.
    uint32 viewports = 0;
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
    // graphics queue와 따로 도는 compute queue가 있다(QueueKind::AsyncCompute 기록·QueueWait,
    // RenderBase 8절 RG4). D3D12는 늘 참이다. Vulkan은 graphics와 다른 compute 전용 queue family와
    // timeline semaphore가 있을 때다. D3D11·OpenGL ES는 거짓이다.
    bool asyncCompute = false;
    // compute queue와 graphics queue를 겹쳐 이득이 난다고 보는 장치다(렌더 그래프
    // GraphAsyncCompute::Auto가 compute queue를 쓰는 조건, UE와 같은 판단). D3D12는 AMD, 또는
    // NVIDIA의 D3D12_OPTIONS6 PerPrimitiveShadingRateSupportedWithViewportIndexing(Ampere 이후)일
    // 때다. Vulkan은 asyncCompute와 같다. asyncCompute가 거짓이면 늘 거짓이다.
    bool efficientAsyncCompute = false;
    // compute 셰이더를 실행할 수 있다(D3D FL 11.0·Vulkan·OpenGL ES 3.1은 늘 참).
    bool computeShaders = false;
    // 깊이 clamp(깊이 clip 끄기)를 쓸 수 있다(D3D 늘 참, Vulkan depthClamp, OpenGL ES
    // GL_EXT_depth_clamp).
    bool depthClamp = false;
    // 깊이 bias 상한(RasterState::depthBiasClamp)을 쓸 수 있다(D3D 늘 참, Vulkan depthBiasClamp,
    // OpenGL ES GL_EXT_polygon_offset_clamp).
    bool depthBiasClamp = false;
    // alpha-to-coverage를 쓸 수 있다(D3D·Vulkan·OpenGL ES 3.2 모두 core라 늘 참).
    bool alphaToCoverage = false;
    // 색 타깃마다 다른 블렌드·쓰기 mask를 쓸 수 있다(D3D 늘 참, Vulkan independentBlend, OpenGL ES
    // 3.2 core). 거짓이면 모든 타깃의 ColorTargetBlend가 같아야 한다.
    bool independentBlend = false;
    // VS가 렌더 타깃 array 층(SV_RenderTargetArrayIndex)과 viewport 번호(SV_ViewportArrayIndex)를
    // 낼 수 있다(ShaderSystem 7.2절 7항 vertexViewportLayerOutput, Renderer RQ14). 셰이더 형식
    // 선택이 거르지 않는 런타임 확인 기능이다. D3D12는 늘 참이다(UE와 같다, OPTIONS의
    // ...WithoutGSEmulation은 드라이버의 GS 에뮬레이션 여부일 뿐이다). D3D11은 OPTIONS3
    // VPAndRTArrayIndexFromAnyShaderFeedingRasterizer, Vulkan은
    // VK_EXT_shader_viewport_index_layer와 multiViewport 기능(viewport 여럿, Vulkan 1.2의
    // shaderOutputLayer·shaderOutputViewportIndex에 해당)이다. OpenGL ES는 ES core에 없어
    // 거짓이다(GL_NV_viewport_array2는 쓰지 않는다).
    bool vertexViewportLayerOutput = false;
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
    // 텍스처 view swizzle(TextureDesc·TextureViewDesc::swizzle, AssetSystem A0-G)이다. D3D12
    // Shader4ComponentMapping, Vulkan VkComponentMapping, OpenGL ES TEXTURE_SWIZZLE_R/G/B/A는 늘
    // 참이고 D3D11은 view swizzle이 없어 거짓이다(항등이 아닌 swizzle은 Unsupported).
    bool textureViewSwizzle = false;
    // 텍스처 view의 mip·slice 범위(TextureViewDesc::firstMip 등)를 셰이더 읽기 view에 반영한다.
    // D3D12·D3D11·Vulkan은 참이다. OpenGL ES는 texture view가 없어(ES 3.2 core 밖) 거짓이고 Read
    // view는 텍스처 전체를 묶는다: 셰이더는 TextureViewBaseMip을 기준 mip으로 받아 명시 LOD로
    // 읽는다(RenderBase 7.5절). 쓰기 view(level 하나)는 모든 장치에서 범위를 따른다.
    bool textureViewRanges = false;
    // 이방성 + 최근접 mip 필터(Filter::AnisotropicPoint)다. D3D12는 OPTIONS19
    // AnisoFilterWithPointMipSupported(D3D12_FILTER_MIN_MAG_ANISOTROPIC_MIP_POINT)일 때다. Vulkan·
    // OpenGL ES는 늘 참이다(mipmapMode NEAREST·LINEAR_MIPMAP_NEAREST에 이방성 배율을 더하며, 배율은
    // 다른 이방성 필터처럼 장치 한도 samplerAnisotropy로 줄인다). D3D11은 대응 필터가 없어
    // 거짓이다. 거짓이면 그 필터는 UE와 같이 AnisotropicLinear로 만든다(사용자 결정 2026-11-18,
    // backend가 처음 한 번 Debug 로그를 남긴다). 쿠킹 프로필은 UE 기본값을 그대로 쓴다.
    bool anisotropicPointFilter = false;
    // sRGB 짝 형식 view(PixelFormat.ko.md 9절). OpenGL ES는 형식을 재해석하는 view가 없어 거짓이다.
    bool srgbPairView = false;
    // VS·PS가 storage 자원에 쓸 수 있다(Vulkan vertexPipelineStoresAndAtomics·
    // fragmentStoresAndAtomics, D3D11·D3D12는 PS 늘 참·VS는 FL 11.1 이상, OpenGL ES는 단계별
    // storage 한도가 0보다 클 때). 형식 정의는 UAV를 CS만 쓰므로 판단용으로만 공개한다.
    bool vertexStores = false;
    bool pixelStores = false;
    // HDR10(ST.2084·BT.2020) 출력 중인 디스플레이가 이 어댑터에 있다(DXGI IDXGIOutput6 색 공간, UE
    // WindowsD3D12Device.cpp와 같은 판정, 장치를 만들 때 한 번 본다). Vulkan·OpenGL ES는 surface
    // 질의 전에는 알 수 없어 거짓이다(스왑체인 HDR 단계에서 surface별로 판정한다).
    bool hdrOutput = false;
    // 분리 전이(CommandContext::BeginTransitions·EndTransitions)를 GPU에서 따로 실행한다(Vulkan
    // VK_KHR_synchronization2 event, UE r.Vulkan.AllowSplitBarriers). 거짓이면 backend가 end
    // 지점에서 한 번에 낮춘다(D3D12는 UE처럼 native split barrier를 쓰지 않는다, D3D11·OpenGL ES는
    // 드라이버).
    bool splitTransitions = false;
    // D3D12 resource binding tier(1–3)다. 다른 backend는 0이다.
    uint8 resourceBindingTier = 0;
    // 하드웨어 ray tracing이다(Renderer RQ4). rayTracingTier는 D3D12 OPTIONS5 RaytracingTier(major
    // × 10 + minor: 0 없음, 10 = 1.0, 11 = 1.1)이고 다른 backend는 0이다. rayTracingPipeline은 RT
    // 파이프라인(D3D12 Tier 1.0 이상, Vulkan VK_KHR_ray_tracing_pipeline +
    // VK_KHR_acceleration_structure), rayQuery는 셰이더 안 ray query(D3D12 Tier 1.1 이상, Vulkan
    // VK_KHR_ray_query + VK_KHR_acceleration_structure)다. Vulkan은 장치가 확장을 공개하는지이며
    // 확장을 켜는 일은 RT 단계가 한다. D3D11·OpenGL ES는 모두 거짓이다.
    uint8 rayTracingTier = 0;
    bool rayTracingPipeline = false;
    bool rayQuery = false;
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
