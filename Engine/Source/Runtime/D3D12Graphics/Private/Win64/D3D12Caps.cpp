#include "D3D12Caps.h"

#include <DxgiGraphics/DxgiAdapter.h>

namespace rocks::graphics::d3d12 {

namespace {

// resource binding tier의 단계별 한도다(D3D12 "Hardware tiers" 표, d3d12.h 상수).
StageResourceLimits TierStageLimits(D3D12_RESOURCE_BINDING_TIER tier, uint32 uavs) noexcept {
    StageResourceLimits limits;
    limits.constantBuffers = tier == D3D12_RESOURCE_BINDING_TIER_3
                                 ? D3D12_MAX_SHADER_VISIBLE_DESCRIPTOR_HEAP_SIZE_TIER_2
                                 : D3D12_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT;
    limits.sampledTextures = tier == D3D12_RESOURCE_BINDING_TIER_1
                                 ? D3D12_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT
                                 : D3D12_MAX_SHADER_VISIBLE_DESCRIPTOR_HEAP_SIZE_TIER_1;
    limits.samplers = tier == D3D12_RESOURCE_BINDING_TIER_1
                          ? D3D12_COMMONSHADER_SAMPLER_SLOT_COUNT
                          : D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE;
    // 구조화·raw SRV는 SRV 칸을, UAV는 UAV 칸을 쓴다. 형식 정의와 같이 SRV 한도를 구조화 버퍼
    // 한도로, UAV 한도를 storage image 한도로 둔다.
    limits.storageBuffers = limits.sampledTextures;
    limits.storageImages = uavs;
    limits.uniformVectors = NotApplicableLimit;
    return limits;
}

uint32 Capped(uint64 value, uint32 cap) noexcept {
    return value > cap ? cap : static_cast<uint32>(value);
}

} // namespace

DeviceCaps QueryDeviceCaps(ID3D12Device* device, IDXGIAdapter1* adapter,
                           uint32& featureLevel) noexcept {
    DeviceCaps caps;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_11_1,
                                        D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_12_1,
                                        D3D_FEATURE_LEVEL_12_2};
    // 12_2를 모르는 구 런타임은 목록 전체를 E_INVALIDARG로 거부하므로 그때는 12_2를 빼고 다시
    // 묻는다.
    featureLevel = D3D_FEATURE_LEVEL_11_0;
    const UINT levelCount = static_cast<UINT>(sizeof(levels) / sizeof(levels[0]));
    for (UINT count = levelCount; count + 1 >= levelCount; --count) {
        D3D12_FEATURE_DATA_FEATURE_LEVELS featureLevels = {};
        featureLevels.NumFeatureLevels = count;
        featureLevels.pFeatureLevelsRequested = levels;
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &featureLevels,
                                                  sizeof(featureLevels)))) {
            featureLevel = featureLevels.MaxSupportedFeatureLevel;
            break;
        }
    }

    // 지원하지 않는 최고값을 물으면 E_INVALIDARG이므로 높은 것부터 내려가며 묻는다.
    const D3D_SHADER_MODEL models[] = {
        D3D_SHADER_MODEL_6_7, D3D_SHADER_MODEL_6_6, D3D_SHADER_MODEL_6_5,
        D3D_SHADER_MODEL_6_4, D3D_SHADER_MODEL_6_3, D3D_SHADER_MODEL_6_2,
        D3D_SHADER_MODEL_6_1, D3D_SHADER_MODEL_6_0, D3D_SHADER_MODEL_5_1};
    for (const D3D_SHADER_MODEL model : models) {
        D3D12_FEATURE_DATA_SHADER_MODEL shaderModel = {model};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shaderModel,
                                                  sizeof(shaderModel)))) {
            const uint32 value = shaderModel.HighestShaderModel;
            caps.features.shaderModel = static_cast<uint8>((value >> 4) * 10 + (value & 0xF));
            break;
        }
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
    const bool optionsKnown = SUCCEEDED(
        device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)));
    const D3D12_RESOURCE_BINDING_TIER tier =
        optionsKnown ? options.ResourceBindingTier : D3D12_RESOURCE_BINDING_TIER_1;
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {};
    const bool options5Known = SUCCEEDED(
        device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5)));
    D3D12_FEATURE_DATA_D3D12_OPTIONS9 options9 = {};
    const bool options9Known = SUCCEEDED(
        device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS9, &options9, sizeof(options9)));

    // UAV 합 한도: Tier 1은 FL 11.0이면 8, 그 위는 64. Tier 2는 64, Tier 3은 heap 전체다.
    uint32 uavs = D3D12_UAV_SLOT_COUNT;
    if (tier == D3D12_RESOURCE_BINDING_TIER_1 && featureLevel == D3D_FEATURE_LEVEL_11_0) {
        uavs = D3D12_PS_CS_UAV_REGISTER_COUNT;
    } else if (tier == D3D12_RESOURCE_BINDING_TIER_3) {
        uavs = D3D12_MAX_SHADER_VISIBLE_DESCRIPTOR_HEAP_SIZE_TIER_2;
    }

    DeviceLimits& limits = caps.limits;
    limits.vertex = TierStageLimits(tier, uavs);
    limits.pixel = limits.vertex;
    limits.compute = limits.vertex;
    // FL 11.0의 VS에는 UAV가 없다(vertexStores와 같은 조건).
    if (featureLevel < D3D_FEATURE_LEVEL_11_1) {
        limits.vertex.storageImages = 0;
    }
    // 그래픽 파이프라인은 VS·PS가 각자 slot을 가지므로 단계 값의 두 배이고, heap 크기를 넘지
    // 않는다.
    limits.pipelineSampledTextures = Capped(uint64{limits.pixel.sampledTextures} * 2,
                                            D3D12_MAX_SHADER_VISIBLE_DESCRIPTOR_HEAP_SIZE_TIER_1);
    limits.pipelineSamplers =
        Capped(uint64{limits.pixel.samplers} * 2, D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE);
    limits.pipelineStorageBuffers = limits.pipelineSampledTextures;
    limits.pipelineStorageImages = uavs;
    limits.perStageResources = NotApplicableLimit;
    limits.pipelineUavs = uavs;
    limits.boundDescriptorSets = NotApplicableLimit;
    limits.colorAttachments = D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT;
    limits.viewports = D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    limits.constantBufferBytes = D3D12_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 16;
    // Rocks 루트 상수 계약(ShaderSystem 7.2절 2항, 32 DWORD)이다.
    limits.rootConstantBytes = RootConstantContractBytes;
    limits.constantBufferOffsetAlignment = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
    // buffer view의 FirstElement는 원소 단위라 원소 정렬 외 제약이 없다.
    limits.storageBufferOffsetAlignment = 1;
    limits.texelBufferOffsetAlignment = 1;
    limits.texelBufferElements = 1u << D3D12_REQ_BUFFER_RESOURCE_TEXEL_COUNT_2_TO_EXP;
    limits.storageBufferBytes = uint64{limits.texelBufferElements} * 4;
    DXGI_ADAPTER_DESC1 adapterDesc = {};
    if (adapter != nullptr && SUCCEEDED(adapter->GetDesc1(&adapterDesc))) {
        limits.bufferBytes = dxgi::GuaranteedResourceBytes(adapterDesc.DedicatedVideoMemory);
    } else {
        limits.bufferBytes = dxgi::GuaranteedResourceBytes(0);
    }

    limits.vertexAttributes = D3D12_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT;
    limits.vertexStreams = D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT;
    limits.interStageVectors = D3D12_VS_OUTPUT_REGISTER_COUNT;
    for (uint32& count : limits.computeGroupCount) {
        count = D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION;
    }

    limits.computeGroupSize[0] = D3D12_CS_THREAD_GROUP_MAX_X;
    limits.computeGroupSize[1] = D3D12_CS_THREAD_GROUP_MAX_Y;
    limits.computeGroupSize[2] = D3D12_CS_THREAD_GROUP_MAX_Z;
    limits.computeGroupInvocations = D3D12_CS_THREAD_GROUP_MAX_THREADS_PER_GROUP;
    limits.computeSharedMemoryBytes = D3D12_CS_TGSM_REGISTER_COUNT * 4;
    limits.textureSize2D = D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
    limits.textureSize3D = D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION;
    limits.textureSizeCube = D3D12_REQ_TEXTURECUBE_DIMENSION;
    limits.textureArrayLayers = D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION;
    limits.samplerAnisotropy = D3D12_MAX_MAXANISOTROPY;

    DeviceFeatures& features = caps.features;
    features.computeShaders = true;
    features.depthClamp = true;
    features.depthBiasClamp = true;
    features.alphaToCoverage = true;
    features.independentBlend = true;
    // D3D12는 VS의 layer·viewport 출력을 늘 받는다. OPTIONS의
    // VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation은 드라이버가 GS
    // 에뮬레이션 없이 하는지일 뿐이다(UE 5.8도 D3D12에서 무조건 참: D3D12Device.cpp:272,
    // D3D12RHI.cpp:135).
    features.vertexViewportLayerOutput = true;
    features.atomic64 = options9Known && options9.AtomicInt64OnTypedResourceSupported;
    features.indirectFirstInstance = true;
    features.multiDrawIndirect = true;
    features.textureCubeArray = true;
    features.srgbPairView = true;
    // VS UAV는 FL 11.1 이상이다(FL 11.0은 PS·CS만).
    features.vertexStores = featureLevel >= D3D_FEATURE_LEVEL_11_1;
    features.pixelStores = true;
    features.hdrOutput = dxgi::AdapterHasHdrOutput(adapter);
    features.resourceBindingTier = static_cast<uint8>(tier);
    // D3D12_RAYTRACING_TIER는 1.0 = 10, 1.1 = 11이다(major × 10 + minor와 같다).
    const D3D12_RAYTRACING_TIER rayTracing =
        options5Known ? options5.RaytracingTier : D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
    features.rayTracingTier = static_cast<uint8>(rayTracing);
    features.rayTracingPipeline = rayTracing >= D3D12_RAYTRACING_TIER_1_0;
    features.rayQuery = rayTracing >= D3D12_RAYTRACING_TIER_1_1;
    return caps;
}

} // namespace rocks::graphics::d3d12
