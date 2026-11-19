#include "D3D11Caps.h"

#include "D3D11Common.h"

#include <DxgiGraphics/DxgiAdapter.h>

namespace rocks::graphics::d3d11 {

namespace {

// D3D11.1 VSSetConstantBuffers1의 FirstConstant 단위(상수 16개 × 16 byte)다.
constexpr uint32 D3D11ConstantBufferOffsetAlignment = 16 * 16;

} // namespace

DeviceCaps QueryDeviceCaps(ID3D11Device* device, IDXGIAdapter1* adapter) noexcept {
    DeviceCaps caps;
    const D3D_FEATURE_LEVEL featureLevel = device->GetFeatureLevel();
    const bool level11_1 = featureLevel >= D3D_FEATURE_LEVEL_11_1;
    const uint32 uavs = level11_1 ? D3D11_1_UAV_SLOT_COUNT : D3D11_PS_CS_UAV_REGISTER_COUNT;

    StageResourceLimits stage;
    // 루트 상수 에뮬레이션이 상수 버퍼 slot 하나를 쓴다(DeviceCaps 계약: 루트 상수는 들지 않는다).
    stage.constantBuffers = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1;
    stage.sampledTextures = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;
    stage.samplers = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT;
    // 구조화·raw SRV는 SRV slot을, UAV는 UAV slot을 쓴다(D3D12 질의와 같은 대응).
    stage.storageBuffers = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;
    stage.storageImages = uavs;
    stage.uniformVectors = NotApplicableLimit;
    DeviceLimits& limits = caps.limits;
    limits.vertex = stage;
    // FL 11.0의 VS에는 UAV가 없다.
    limits.vertex.storageImages = level11_1 ? uavs : 0;
    limits.pixel = stage;
    limits.compute = stage;
    // 그래픽 파이프라인은 VS·PS가 각자 slot을 가진다.
    limits.pipelineSampledTextures = stage.sampledTextures * 2;
    limits.pipelineSamplers = stage.samplers * 2;
    limits.pipelineStorageBuffers = stage.storageBuffers * 2;
    limits.pipelineStorageImages = uavs;
    limits.perStageResources = NotApplicableLimit;
    limits.pipelineUavs = uavs;
    limits.boundDescriptorSets = NotApplicableLimit;
    limits.colorAttachments = D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;
    limits.viewports = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    limits.constantBufferBytes = D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 16;
    // 그리기 상수는 상수 버퍼 에뮬레이션이라 Rocks 루트 상수 계약이다.
    limits.rootConstantBytes = RootConstantContractBytes;
    // D3D11.1 상수 버퍼 묶기 시작은 상수 16개(256 byte) 단위다(VSSetConstantBuffers1).
    limits.constantBufferOffsetAlignment = D3D11ConstantBufferOffsetAlignment;
    limits.storageBufferOffsetAlignment = 1;
    limits.texelBufferOffsetAlignment = 1;
    limits.texelBufferElements = 1u << D3D11_REQ_BUFFER_RESOURCE_TEXEL_COUNT_2_TO_EXP;
    limits.storageBufferBytes = uint64{limits.texelBufferElements} * 4;
    DXGI_ADAPTER_DESC1 adapterDesc = {};
    const bool described = adapter != nullptr && SUCCEEDED(adapter->GetDesc1(&adapterDesc));
    limits.bufferBytes =
        dxgi::GuaranteedResourceBytes(described ? adapterDesc.DedicatedVideoMemory : 0);
    limits.vertexAttributes = D3D11_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT;
    limits.vertexStreams = D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT;
    limits.interStageVectors = D3D11_VS_OUTPUT_REGISTER_COUNT;
    for (uint32& count : limits.computeGroupCount) {
        count = D3D11_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION;
    }

    limits.computeGroupSize[0] = D3D11_CS_THREAD_GROUP_MAX_X;
    limits.computeGroupSize[1] = D3D11_CS_THREAD_GROUP_MAX_Y;
    limits.computeGroupSize[2] = D3D11_CS_THREAD_GROUP_MAX_Z;
    limits.computeGroupInvocations = D3D11_CS_THREAD_GROUP_MAX_THREADS_PER_GROUP;
    limits.computeSharedMemoryBytes = D3D11_CS_TGSM_REGISTER_COUNT * 4;
    limits.textureSize2D = D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
    limits.textureSize3D = D3D11_REQ_TEXTURE3D_U_V_OR_W_DIMENSION;
    limits.textureSizeCube = D3D11_REQ_TEXTURECUBE_DIMENSION;
    limits.textureArrayLayers = D3D11_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION;
    limits.samplerAnisotropy = D3D11_MAX_MAXANISOTROPY;

    D3D11_FEATURE_DATA_D3D11_OPTIONS3 options3 = {};
    const bool options3Known = SUCCEEDED(
        device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS3, &options3, sizeof(options3)));
    DeviceFeatures& features = caps.features;
    features.computeShaders = true;
    features.depthClamp = true;
    features.depthBiasClamp = true;
    features.alphaToCoverage = true;
    features.independentBlend = true;
    features.vertexViewportLayerOutput =
        options3Known && options3.VPAndRTArrayIndexFromAnyShaderFeedingRasterizer;
    // SM 5.0에는 64 bit atomic이 없다(UE는 벤더 확장으로만 켠다, 쓰지 않는다).
    features.atomic64 = false;
    features.indirectFirstInstance = true;
    features.multiDrawIndirect = false;
    features.textureCubeArray = true;
    // D3D11 SRV에는 성분 대응이 없고 이방성 필터의 mip은 늘 선형이다(D3D11_FILTER_ANISOTROPIC).
    features.textureViewSwizzle = false;
    features.textureViewRanges = true;
    features.anisotropicPointFilter = false;
    features.srgbPairView = true;
    features.vertexStores = level11_1;
    features.pixelStores = true;
    features.hdrOutput = dxgi::AdapterHasHdrOutput(adapter);
    features.resourceBindingTier = 0;
    features.shaderModel = 50;
    return caps;
}

} // namespace rocks::graphics::d3d11
