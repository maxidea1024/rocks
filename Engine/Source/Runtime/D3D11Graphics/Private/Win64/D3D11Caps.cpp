#include "D3D11Caps.h"

#include "D3D11Common.h"

namespace rocks::graphics::d3d11 {

namespace {

// HDR10(ST.2084·BT.2020)으로 출력 중인 디스플레이가 adapter에 있는지다(UE
// WindowsD3D11Device.cpp:453과 같은 판정).
bool AdapterHasHdrOutput(IDXGIAdapter* adapter) noexcept {
    if (adapter == nullptr) {
        return false;
    }

    for (UINT index = 0;; ++index) {
        ComRef<IDXGIOutput> output;
        if (adapter->EnumOutputs(index, output.Out()) != S_OK) {
            return false;
        }

        ComRef<IDXGIOutput6> output6;
        DXGI_OUTPUT_DESC1 desc = {};
        if (SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(output6.Out()))) &&
            SUCCEEDED(output6->GetDesc1(&desc)) &&
            desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) {
            return true;
        }
    }
}

// D3D 보장 최대 자원 크기(byte)다: min(max(128, 전용 VRAM MiB × 0.25), 2048) MiB.
uint64 GuaranteedResourceBytes(uint64 dedicatedVideoMemory) noexcept {
    const uint64 videoMiB = dedicatedVideoMemory >> 20;
    uint64 mib = static_cast<uint64>(static_cast<double>(videoMiB) *
                                     D3D11_REQ_RESOURCE_SIZE_IN_MEGABYTES_EXPRESSION_B_TERM);
    mib = mib < D3D11_REQ_RESOURCE_SIZE_IN_MEGABYTES_EXPRESSION_A_TERM
              ? D3D11_REQ_RESOURCE_SIZE_IN_MEGABYTES_EXPRESSION_A_TERM
              : mib;
    mib = mib > D3D11_REQ_RESOURCE_SIZE_IN_MEGABYTES_EXPRESSION_C_TERM
              ? D3D11_REQ_RESOURCE_SIZE_IN_MEGABYTES_EXPRESSION_C_TERM
              : mib;
    return mib << 20;
}

} // namespace

DeviceCaps QueryDeviceCaps(ID3D11Device* device, IDXGIAdapter1* adapter) noexcept {
    DeviceCaps caps;
    const D3D_FEATURE_LEVEL featureLevel = device->GetFeatureLevel();
    const bool level11_1 = featureLevel >= D3D_FEATURE_LEVEL_11_1;
    const uint32 uavs = level11_1 ? D3D11_1_UAV_SLOT_COUNT : D3D11_PS_CS_UAV_REGISTER_COUNT;

    StageResourceLimits stage;
    stage.constantBuffers = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT;
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
    limits.constantBufferBytes = D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 16;
    // 그리기 상수는 상수 버퍼 b13 에뮬레이션이라 Rocks 루트 상수 계약(128 byte)이다.
    limits.rootConstantBytes = 128;
    // D3D11.1 상수 버퍼 묶기 시작은 상수 16개(256 byte) 단위다.
    limits.constantBufferOffsetAlignment = 256;
    limits.storageBufferOffsetAlignment = 1;
    limits.texelBufferOffsetAlignment = 1;
    limits.texelBufferElements = 1u << D3D11_REQ_BUFFER_RESOURCE_TEXEL_COUNT_2_TO_EXP;
    limits.storageBufferBytes = uint64{limits.texelBufferElements} * 4;
    DXGI_ADAPTER_DESC1 adapterDesc = {};
    const bool described = adapter != nullptr && SUCCEEDED(adapter->GetDesc1(&adapterDesc));
    limits.bufferBytes = GuaranteedResourceBytes(described ? adapterDesc.DedicatedVideoMemory : 0);
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
    features.vertexShaderLayerOutput =
        options3Known && options3.VPAndRTArrayIndexFromAnyShaderFeedingRasterizer;
    // SM 5.0에는 64 bit atomic이 없다(UE는 벤더 확장으로만 켠다, 쓰지 않는다).
    features.atomic64 = false;
    features.indirectFirstInstance = true;
    features.multiDrawIndirect = false;
    features.textureCubeArray = true;
    features.srgbPairView = true;
    features.vertexStores = level11_1;
    features.pixelStores = true;
    features.hdrOutput = AdapterHasHdrOutput(adapter);
    features.resourceBindingTier = 0;
    features.shaderModel = 50;
    return caps;
}

} // namespace rocks::graphics::d3d11
