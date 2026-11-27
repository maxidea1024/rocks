#include "RecordingDevice.h"
#include "RegistryShaderSet.h"

#include <Base/InitializerList.h>
#include <Core/Containers/Array.h>
#include <Core/Math/Float16.h>
#include <Core/Memory/SystemAllocator.h>
#include <RenderBase/ClearResources.h>
#include <RenderBase/ComputeDispatch.h>
#include <RenderBase/DefaultTextureCache.h>
#include <RenderBase/EngineShaderTypes.h>
#include <RenderBase/GenerateMips.h>
#include <RenderBase/UtilityShaderTypes.h>
#include <ShaderCore/ShaderFormatFacts.h>
#include <ShaderCore/ShaderTypeRegistry.h>
#include <TestSupport/Test.h>
#include <math.h>
#include <string.h>

// 엔진 셰이더 SL1 유틸리티 C++ 도우미 시험이다(Docs/Design/EngineShaders 14.5·14.6절 4항,
// RenderBase RR17): WrapGroupCount 경계, storage image 형식 대응, 유틸리티 셰이더 타입
// 등록(퍼뮤테이션·거르기), 밉 생성 경로 선택표와 오류, clear 도우미의 기록 열과 오류, 기본 텍스처
// 캐시(부호화·키·상한·Retire). 실제 픽셀은 Sandbox self-test(UtilityPassSelfTest.cpp)가 본다.
using namespace rocks;
using namespace rocks::render;
using rocks::render::testing::RecordingDevice;
using rocks::render::testing::RegistryShaderSet;

namespace utility_shader_tests_file {

using graphics::PixelFormat;
using graphics::TextureDimension;

SystemAllocator& Allocator_() noexcept {
    static SystemAllocator allocator;
    return allocator;
}

const shader::ShaderTypeDesc* FindType(uint64 hash) noexcept {
    for (const shader::ShaderTypeDesc* type : EngineShaderTypes()) {
        if (type->nameHash == hash) {
            return type;
        }
    }

    return nullptr;
}

uint32 DomainSize(const shader::ShaderTypeDesc& type) noexcept {
    auto size = shader::ValidateShaderPermutationDomain(type.permutation);
    return size ? size.Value() : 0u;
}

usize CountOps(const RecordingDevice& device, RecordingDevice::Op op) noexcept {
    usize count = 0;
    for (const RecordingDevice::Call& call : device.Calls()) {
        count += call.op == op ? 1u : 0u;
    }

    return count;
}

const RecordingDevice::Call* LastOp(const RecordingDevice& device,
                                    RecordingDevice::Op op) noexcept {
    const RecordingDevice::Call* last = nullptr;
    for (const RecordingDevice::Call& call : device.Calls()) {
        last = call.op == op ? &call : last;
    }

    return last;
}

// 감싸기는 65,535 고정 폭이고 0은 빈 dispatch, uint32 끝도 넘치지 않는다.
TEST(RenderBase_WrapGroupCount) {
    CHECK(WrapGroupCount(0) == (ComputeGroupCount{0, 1, 1}) && WrapGroupCount(0).IsEmpty());
    CHECK(WrapGroupCount(1) == (ComputeGroupCount{1, 1, 1}));
    CHECK(WrapGroupCount(65535) == (ComputeGroupCount{65535, 1, 1}));
    CHECK(WrapGroupCount(65536) == (ComputeGroupCount{65535, 2, 1}));
    CHECK(WrapGroupCount(65535u * 3u) == (ComputeGroupCount{65535, 3, 1}));
    CHECK(WrapGroupCount(65535u * 3u + 1u) == (ComputeGroupCount{65535, 4, 1}));
    // 2^32 − 1 = 65,535 × 65,537이다.
    CHECK(WrapGroupCount(0xffffffffu) == (ComputeGroupCount{65535, 65537, 1}));
    // 감싼 그룹 수는 늘 원래 그룹 수 이상이고 한 행 미만만 남는다.
    for (const uint32 groups : {1u, 1000u, 65535u, 65536u, 131071u, 4000000u}) {
        const ComputeGroupCount wrapped = WrapGroupCount(groups);
        const uint64 covered = static_cast<uint64>(wrapped.x) * wrapped.y;
        CHECK(covered >= groups && covered - groups < MaxWrappedGroupsPerAxis);
    }

    graphics::DeviceLimits limits;
    limits.computeGroupCount[0] = limits.computeGroupCount[1] = limits.computeGroupCount[2] = 65535;
    CHECK(ValidateGroupCount(limits, WrapGroupCount(70000)));
}

// 쓰기 view 형식 → storage image 형식(셰이더 퍼뮤테이션 번호)이다.
TEST(RenderBase_StorageImageFormatOf) {
    struct Row {
        PixelFormat format;
        StorageImageFormat storage;
    };

    constexpr Row Rows[] = {{PixelFormat::RGBA8Unorm, StorageImageFormat::Rgba8},
                            {PixelFormat::RGBA8Snorm, StorageImageFormat::Rgba8Snorm},
                            {PixelFormat::RGBA16Float, StorageImageFormat::Rgba16f},
                            {PixelFormat::RGBA32Float, StorageImageFormat::Rgba32f},
                            {PixelFormat::R32Float, StorageImageFormat::R32f},
                            {PixelFormat::R32Uint, StorageImageFormat::R32ui},
                            {PixelFormat::R32Sint, StorageImageFormat::R32i},
                            {PixelFormat::RGBA32Uint, StorageImageFormat::Rgba32ui},
                            {PixelFormat::RG16Float, StorageImageFormat::Rg16f},
                            {PixelFormat::R16Float, StorageImageFormat::R16f},
                            {PixelFormat::RG32Float, StorageImageFormat::Rg32f},
                            {PixelFormat::RG11B10Ufloat, StorageImageFormat::R11g11b10f}};
    for (const Row& row : Rows) {
        auto storage = StorageImageFormatOf(row.format);
        CHECK(storage && storage.Value() == row.storage);
    }

    for (const PixelFormat format :
         {PixelFormat::RGBA8UnormSrgb, PixelFormat::BGRA8Unorm, PixelFormat::R8Unorm,
          PixelFormat::RGBA16Uint, PixelFormat::D32Float, PixelFormat::BC1Unorm,
          PixelFormat::Unknown}) {
        CHECK(StorageImageFormatOf(format).Error() == ErrorCode::Unsupported);
    }

    CHECK(IsFloatStorageImageFormat(StorageImageFormat::Rgba8) &&
          IsFloatStorageImageFormat(StorageImageFormat::R11g11b10f) &&
          !IsFloatStorageImageFormat(StorageImageFormat::R32ui) &&
          !IsFloatStorageImageFormat(StorageImageFormat::R32i) &&
          !IsFloatStorageImageFormat(StorageImageFormat::Rgba32ui));
}

// 유틸리티 셰이더 타입이 등록 목록에 있고 퍼뮤테이션 도메인·거르기·ID 도우미가 맞는다.
TEST(RenderBase_UtilityShaderTypesRegistered) {
    shader::ShaderTypeRegistry registry(Allocator_());
    REQUIRE(registry.Register(EngineShaderTypes()));
    REQUIRE(registry.Freeze());
    const uint64 hashes[] = {
        GenerateMipsComputeType.hash,   GenerateMipsPixelType.hash,
        ClearWritableComputeType.hash,  ClearRegionVertexType.hash,
        ClearRegionPixelType.hash,      InitializeDispatchArgumentsComputeType.hash,
        CopyDepthToColorPixelType.hash, CopyDepthPixelType.hash,
        SolidColorPixelType.hash,       DepthOnlyPixelType.hash,
        MissingShaderPixelType.hash};
    for (const uint64 hash : hashes) {
        CHECK(registry.Find(hash) != nullptr);
    }

    const shader::ShaderTypeDesc* mips = FindType(GenerateMipsComputeType.hash);
    const shader::ShaderTypeDesc* mipsPixel = FindType(GenerateMipsPixelType.hash);
    const shader::ShaderTypeDesc* clear = FindType(ClearWritableComputeType.hash);
    const shader::ShaderTypeDesc* region = FindType(ClearRegionPixelType.hash);
    REQUIRE(mips != nullptr && mipsPixel != nullptr && clear != nullptr && region != nullptr);
    CHECK(DomainSize(*mips) == GenerateMipsComputeDimensionCount * StorageImageFormatCount);
    CHECK(DomainSize(*mipsPixel) == GenerateMipsPixelDimensionCount);
    CHECK(DomainSize(*clear) == ClearDimensionCount * StorageImageFormatCount);
    CHECK(DomainSize(*region) == ClearValueTypeCount);
    CHECK(mips->stage == shader::ShaderStageCompute && clear->stage == shader::ShaderStageCompute &&
          mipsPixel->stage == shader::ShaderStagePixel);

    // ID 도우미 = 도메인 부호화(첫 차원이 낮은 자리).
    const uint32 mipValues[2] = {static_cast<uint32>(GenerateMipsComputeDimension::Texture3D),
                                 static_cast<uint32>(StorageImageFormat::Rgba16f)};
    CHECK(shader::EncodeShaderPermutation(mips->permutation, mipValues).Value() ==
          GenerateMipsComputePermutation(GenerateMipsComputeDimension::Texture3D,
                                         StorageImageFormat::Rgba16f));
    const uint32 clearValues[2] = {static_cast<uint32>(ClearDimension::Texture2DArray),
                                   static_cast<uint32>(StorageImageFormat::R11g11b10f)};
    CHECK(shader::EncodeShaderPermutation(clear->permutation, clearValues).Value() ==
          ClearWritablePermutation(ClearDimension::Texture2DArray, StorageImageFormat::R11g11b10f));

    // 거르기: 밉 생성은 float 형식만, GLES는 core image 형식(앞 여덟)만.
    shader::ShaderFormatFacts d3d;
    d3d.backend = shader::ShaderBackend::D3D12;
    shader::ShaderFormatFacts gles;
    gles.backend = shader::ShaderBackend::OpenGL;
    uint32 mipsD3D = 0;
    uint32 mipsGles = 0;
    for (uint32 id = 0; id < DomainSize(*mips); ++id) {
        mipsD3D += mips->shouldCompile(shader::NoVertexInputType, id, d3d) ? 1u : 0u;
        mipsGles += mips->shouldCompile(shader::NoVertexInputType, id, gles) ? 1u : 0u;
    }

    CHECK(mipsD3D == GenerateMipsComputeDimensionCount * 9u);
    CHECK(mipsGles == GenerateMipsComputeDimensionCount * 5u);
    CHECK(
        !mips->shouldCompile(shader::NoVertexInputType,
                             GenerateMipsComputePermutation(GenerateMipsComputeDimension::Texture2D,
                                                            StorageImageFormat::R32ui),
                             d3d));
    uint32 clearGles = 0;
    for (uint32 id = 0; id < DomainSize(*clear); ++id) {
        CHECK(clear->shouldCompile(shader::NoVertexInputType, id, d3d));
        clearGles += clear->shouldCompile(shader::NoVertexInputType, id, gles) ? 1u : 0u;
    }

    CHECK(clearGles == ClearDimensionCount * GlesStorageImageFormatCount);
    // 상수 구조체 크기(셰이더 생성 원문과 같은 배치).
    CHECK(sizeof(GenerateMipsConstants) == 32 && sizeof(ClearWritableConstants) == 48 &&
          sizeof(ClearRegionConstants) == 32 && sizeof(DispatchArgumentsInitConstants) == 32 &&
          sizeof(SolidColorConstants) == 16);
}

graphics::TextureDesc MipDesc(TextureDimension dimension, PixelFormat format,
                              graphics::TextureUsage usage) noexcept {
    graphics::TextureDesc desc;
    desc.dimension = dimension;
    desc.width = desc.height = 8;
    desc.depth = dimension == TextureDimension::Texture3D ? 8u : 1u;
    desc.arraySize = dimension == TextureDimension::Texture2DArray ? 2u : 1u;
    desc.format = format;
    desc.usage = usage;
    desc.mipLevels = graphics::TextureFullMipCount(desc);
    return desc;
}

// 경로 선택표: compute(Storage·float storage 형식·cube 계열 아님)를 먼저, raster(RenderTarget)를
// 다음으로.
TEST(RenderBase_MipGenerationPathSelection) {
    using graphics::TextureUsage;
    RecordingDevice device(Allocator_());
    device.SetPixelFormatCaps(graphics::PixelFormatCaps::Sampled |
                              graphics::PixelFormatCaps::RenderTarget |
                              graphics::PixelFormatCaps::StorageWrite);
    const TextureUsage both = TextureUsage::RenderTarget | TextureUsage::Storage;
    struct Row {
        TextureDimension dimension;
        PixelFormat format;
        TextureUsage usage;
        ErrorCode error;
        MipGenerationPath path;
    };

    const Row rows[] = {{TextureDimension::Texture2D, PixelFormat::RGBA16Float, both,
                         ErrorCode::None, MipGenerationPath::Compute},
                        {TextureDimension::Texture2DArray, PixelFormat::RGBA8Unorm,
                         TextureUsage::Storage, ErrorCode::None, MipGenerationPath::Compute},
                        {TextureDimension::Texture3D, PixelFormat::R32Float, both, ErrorCode::None,
                         MipGenerationPath::Compute},
                        {TextureDimension::Texture2D, PixelFormat::RGBA8UnormSrgb,
                         TextureUsage::RenderTarget, ErrorCode::None, MipGenerationPath::Raster},
                        {TextureDimension::Texture2D, PixelFormat::RGBA16Float,
                         TextureUsage::RenderTarget, ErrorCode::None, MipGenerationPath::Raster},
                        {TextureDimension::TextureCube, PixelFormat::RGBA16Float,
                         TextureUsage::RenderTarget, ErrorCode::None, MipGenerationPath::Raster},
                        {TextureDimension::Texture2D, PixelFormat::R32Uint, both, ErrorCode::None,
                         MipGenerationPath::Raster},
                        {TextureDimension::Texture2D, PixelFormat::BGRA8Unorm, both,
                         ErrorCode::None, MipGenerationPath::Raster},
                        {TextureDimension::TextureCubeArray, PixelFormat::RGBA16Float,
                         TextureUsage::RenderTarget, ErrorCode::Unsupported,
                         MipGenerationPath::Raster},
                        {TextureDimension::Texture2D, PixelFormat::RGBA8Unorm,
                         TextureUsage::Sampled, ErrorCode::Unsupported, MipGenerationPath::Raster},
                        {TextureDimension::Texture2D, PixelFormat::R32Uint, TextureUsage::Storage,
                         ErrorCode::Unsupported, MipGenerationPath::Raster}};
    for (const Row& row : rows) {
        const graphics::TextureDesc desc = MipDesc(row.dimension, row.format, row.usage);
        auto path = SelectMipGenerationPath(device, desc);
        if (row.error == ErrorCode::None) {
            CHECK(path && path.Value() == row.path);
        } else {
            CHECK(!path && path.Error() == row.error);
        }
    }

    // mip 하나·MSAA는 호출자 오류, 능력 없는 장치는 Unsupported.
    graphics::TextureDesc single =
        MipDesc(TextureDimension::Texture2D, PixelFormat::RGBA8Unorm, both);
    single.mipLevels = 1;
    CHECK(SelectMipGenerationPath(device, single).Error() == ErrorCode::InvalidArgument);
    graphics::TextureDesc multisampled = single;
    multisampled.mipLevels = 2;
    multisampled.sampleCount = 4;
    CHECK(SelectMipGenerationPath(device, multisampled).Error() == ErrorCode::InvalidArgument);
    RecordingDevice bare(Allocator_());
    CHECK(SelectMipGenerationPath(
              bare, MipDesc(TextureDimension::Texture2D, PixelFormat::RGBA8Unorm, both))
              .Error() == ErrorCode::Unsupported);

    // 쓸 수 없는 경로를 강제하면 Unsupported(기록 없이), mip 하나는 InvalidArgument다.
    shader::ShaderTypeRegistry registry(Allocator_());
    REQUIRE(registry.Register(EngineShaderTypes()));
    REQUIRE(registry.Freeze());
    RegistryShaderSet shaders(Allocator_(), device, registry);
    REQUIRE(shaders.ready);
    PipelineCache cache(Allocator_(), device);
    const graphics::TextureDesc rasterOnly = MipDesc(
        TextureDimension::TextureCube, PixelFormat::RGBA16Float, TextureUsage::RenderTarget);
    auto texture = device.CreateTexture(rasterOnly, {});
    REQUIRE(texture);
    const usize callsBefore = device.Calls().Size();
    CHECK(GenerateMips(device, shaders.set, cache, texture.Value(), rasterOnly,
                       MipGenerationPath::Compute)
              .Error() == ErrorCode::Unsupported);
    CHECK(GenerateMips(device, shaders.set, cache, texture.Value(), single).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(device.Calls().Size() == callsBefore);
    // 기록 장치 집합의 바인딩 layout은 비어 있어 원본 텍스처 slot이 없다: 기록을 시작하기 전에
    // NotFound.
    CHECK(GenerateMips(device, shaders.set, cache, texture.Value(), rasterOnly).Error() ==
          ErrorCode::NotFound);
    CHECK(CountOps(device, RecordingDevice::Op::BeginOffscreen) == 0);
    device.Retire(texture.Value());
    cache.Clear();
    shaders.set.Retire();
}

// clear 도우미의 기록 열과 오류다.
TEST(RenderBase_ClearResourcesRecording) {
    using graphics::TextureUsage;
    RecordingDevice device(Allocator_());
    device.SetPixelFormatCaps(
        graphics::PixelFormatCaps::Sampled | graphics::PixelFormatCaps::RenderTarget |
        graphics::PixelFormatCaps::StorageWrite | graphics::PixelFormatCaps::DepthStencil |
        graphics::PixelFormatCaps::TexelBufferWrite);
    shader::ShaderTypeRegistry registry(Allocator_());
    REQUIRE(registry.Register(EngineShaderTypes()));
    REQUIRE(registry.Freeze());
    RegistryShaderSet shaders(Allocator_(), device, registry);
    REQUIRE(shaders.ready);
    PipelineCache cache(Allocator_(), device);

    graphics::TextureDesc storage;
    storage.width = 20;
    storage.height = 9;
    storage.format = PixelFormat::RGBA16Float;
    storage.usage = TextureUsage::Storage;
    auto texture = device.CreateTexture(storage, {});
    REQUIRE(texture);
    graphics::TextureViewDesc writable;
    writable.access = graphics::ViewAccess::ReadWrite;
    writable.mipCount = 1;
    auto view = device.CreateTextureView(texture.Value(), writable);
    auto readView = device.CreateTextureView(texture.Value(), graphics::TextureViewDesc{});
    REQUIRE(view && readView);

    auto frame = device.BeginPass(graphics::PassDesc{});
    REQUIRE(frame);
    graphics::CommandContext& commands = *frame.Value();
    WritableTextureRegion region;
    region.x = 2;
    region.width = 17;
    region.height = 9;
    REQUIRE(ClearWritableTexture(device, commands, shaders.set, cache, view.Value(),
                                 ClearValue::Float(1, 2, 3, 4), region));
    const RecordingDevice::Call* dispatch = LastOp(device, RecordingDevice::Op::Dispatch);
    // 17 × 9 texel을 8 × 8 그룹으로: (3, 2, 1).
    CHECK(dispatch != nullptr && dispatch->value == RecordingDevice::DispatchValue(3, 2, 1));
    CHECK(CountOps(device, RecordingDevice::Op::SetWritableTexture) == 1);
    const RecordingDevice::Call* constants = LastOp(device, RecordingDevice::Op::SetDrawConstants);
    CHECK(constants != nullptr && (constants->value >> 32) == sizeof(ClearWritableConstants));
    // 오류: 빈 범위, Read view, 2D인데 층 범위.
    WritableTextureRegion empty;
    CHECK(ClearWritableTexture(device, commands, shaders.set, cache, view.Value(),
                               ClearValue::Float(0, 0, 0, 0), empty)
              .Error() == ErrorCode::InvalidArgument);
    CHECK(ClearWritableTexture(device, commands, shaders.set, cache, readView.Value(),
                               ClearValue::Float(0, 0, 0, 0), region)
              .Error() == ErrorCode::InvalidArgument);
    WritableTextureRegion layered = region;
    layered.depth = 2;
    CHECK(ClearWritableTexture(device, commands, shaders.set, cache, view.Value(),
                               ClearValue::Float(0, 0, 0, 0), layered)
              .Error() == ErrorCode::InvalidArgument);

    // typed 버퍼: 원소 70,000개 → 64 스레드 그룹 1,094개(감싸지 않는다), 값 계열이 다르면
    // InvalidArgument.
    graphics::BufferDesc bufferDesc;
    bufferDesc.size = 70'000u * 16u;
    bufferDesc.usage = graphics::BufferUsage::ShaderWrite | graphics::BufferUsage::Raw;
    // 기록 장치는 초기 byte가 크기와 같아야 한다.
    Array<uint8> zeros(Allocator_());
    REQUIRE(zeros.TryResize(bufferDesc.size));
    auto buffer = device.CreateBuffer(bufferDesc, zeros.View());
    REQUIRE(buffer);
    graphics::BufferViewDesc typed;
    typed.kind = graphics::BufferViewKind::Typed;
    typed.access = graphics::ViewAccess::ReadWrite;
    typed.format = PixelFormat::RGBA32Float;
    auto bufferView = device.CreateBufferView(buffer.Value(), typed);
    REQUIRE(bufferView);
    REQUIRE(ClearWritableBuffer(device, commands, shaders.set, cache, bufferView.Value(),
                                PixelFormat::RGBA32Float, ClearValue::Float(0, 0, 0, 0), 0,
                                70'000u));
    dispatch = LastOp(device, RecordingDevice::Op::Dispatch);
    CHECK(dispatch != nullptr && dispatch->value == RecordingDevice::DispatchValue(1094, 1, 1));
    CHECK(ClearWritableBuffer(device, commands, shaders.set, cache, bufferView.Value(),
                              PixelFormat::R32Uint, ClearValue::Uint(0, 0, 0, 0), 0, 10)
              .Error() == ErrorCode::InvalidArgument);
    CHECK(ClearWritableBuffer(device, commands, shaders.set, cache, bufferView.Value(),
                              PixelFormat::RGBA32Float, ClearValue::Float(0, 0, 0, 0), 0, 0)
              .Error() == ErrorCode::InvalidArgument);

    // 간접 인자: 정렬 위반은 InvalidArgument, count 0은 기록 없음, 그 밖은 dispatch 하나.
    graphics::BufferDesc argumentDesc;
    argumentDesc.size = 64;
    argumentDesc.usage = graphics::BufferUsage::Indirect | graphics::BufferUsage::ShaderWrite |
                         graphics::BufferUsage::Raw;
    const uint8 argumentZeros[64] = {};
    auto arguments =
        device.CreateBuffer(argumentDesc, Span<const uint8>(argumentZeros, sizeof(argumentZeros)));
    REQUIRE(arguments);
    graphics::BufferViewDesc raw;
    raw.kind = graphics::BufferViewKind::Raw;
    raw.access = graphics::ViewAccess::ReadWrite;
    auto rawView = device.CreateBufferView(arguments.Value(), raw);
    REQUIRE(rawView);
    const graphics::DispatchIndirectArguments values{1, 2, 3};
    CHECK(InitializeDispatchArguments(device, commands, shaders.set, cache, rawView.Value(), 2, 1,
                                      values)
              .Error() == ErrorCode::InvalidArgument);
    CHECK(InitializeDispatchArguments(device, commands, shaders.set, cache, bufferView.Value(), 0,
                                      1, values)
              .Error() == ErrorCode::InvalidArgument);
    const usize dispatches = CountOps(device, RecordingDevice::Op::Dispatch);
    CHECK(InitializeDispatchArguments(device, commands, shaders.set, cache, rawView.Value(), 4, 0,
                                      values));
    CHECK(CountOps(device, RecordingDevice::Op::Dispatch) == dispatches);
    CHECK(InitializeDispatchArguments(device, commands, shaders.set, cache, rawView.Value(), 4, 3,
                                      values));
    CHECK(CountOps(device, RecordingDevice::Op::Dispatch) == dispatches + 1);
    REQUIRE(device.EndFrame());

    // 부분 clear: 오류 표와 기록 열(viewport·scissor를 사각형으로, Draw(3), 되돌림).
    graphics::TextureDesc colorDesc;
    colorDesc.width = colorDesc.height = 32;
    colorDesc.format = PixelFormat::RGBA8Unorm;
    colorDesc.usage = TextureUsage::RenderTarget;
    auto color = device.CreateTexture(colorDesc, {});
    REQUIRE(color);
    auto pass = device.BeginOffscreen(color.Value(), {});
    REQUIRE(pass);
    ClearRegionDesc desc;
    desc.rect = {4, 4, 8, 8};
    desc.targetWidth = desc.targetHeight = 32;
    desc.colorFormat = PixelFormat::RGBA8Unorm;
    desc.clearColor = true;
    desc.color = ClearValue::Float(1, 0, 0, 1);
    const usize drawsBefore = CountOps(device, RecordingDevice::Op::Draw);
    REQUIRE(ClearRegion(*pass.Value(), shaders.set, cache, desc));
    CHECK(CountOps(device, RecordingDevice::Op::Draw) == drawsBefore + 1);
    ClearRegionDesc nothing = desc;
    nothing.clearColor = false;
    CHECK(ClearRegion(*pass.Value(), shaders.set, cache, nothing).Error() ==
          ErrorCode::InvalidArgument);
    ClearRegionDesc outside = desc;
    outside.rect = {30, 0, 4, 4};
    CHECK(ClearRegion(*pass.Value(), shaders.set, cache, outside).Error() ==
          ErrorCode::InvalidArgument);
    ClearRegionDesc emptyRect = desc;
    emptyRect.rect.width = 0;
    CHECK(ClearRegion(*pass.Value(), shaders.set, cache, emptyRect).Error() ==
          ErrorCode::InvalidArgument);
    ClearRegionDesc stencilWithoutPlane = desc;
    stencilWithoutPlane.depthFormat = PixelFormat::D32Float;
    stencilWithoutPlane.clearStencil = true;
    CHECK(ClearRegion(*pass.Value(), shaders.set, cache, stencilWithoutPlane).Error() ==
          ErrorCode::InvalidArgument);
    ClearRegionDesc depthAsColor = desc;
    depthAsColor.colorFormat = PixelFormat::D32Float;
    CHECK(ClearRegion(*pass.Value(), shaders.set, cache, depthAsColor).Error() ==
          ErrorCode::InvalidArgument);
    ClearRegionDesc depthWithoutFormat = desc;
    depthWithoutFormat.clearDepth = true;
    CHECK(ClearRegion(*pass.Value(), shaders.set, cache, depthWithoutFormat).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(CountOps(device, RecordingDevice::Op::Draw) == drawsBefore + 1);
    REQUIRE(device.EndFrame());

    device.Retire(view.Value());
    device.Retire(readView.Value());
    device.Retire(texture.Value());
    device.Retire(bufferView.Value());
    device.Retire(buffer.Value());
    device.Retire(rawView.Value());
    device.Retire(arguments.Value());
    device.Retire(color.Value());
    cache.Clear();
    shaders.set.Retire();
}

// 기본 texel 부호화다(UE FClearValueBinding 값의 형식 부호화와 같은 뜻 — 정확한 byte).
TEST(RenderBase_EncodeDefaultTexel) {
    uint8 texel[MaxDefaultTexelBytes] = {};
    const Span<uint8> out(texel, sizeof(texel));
    auto encoded = EncodeDefaultTexel(PixelFormat::RGBA8Unorm, {0.2f, 0.4f, 0.6f, 0.8f}, out);
    CHECK(encoded && encoded.Value() == 4 && texel[0] == 51 && texel[1] == 102 && texel[2] == 153 &&
          texel[3] == 204);
    encoded = EncodeDefaultTexel(PixelFormat::BGRA8Unorm, {1.0f, 0.0f, 0.2f, 1.0f}, out);
    CHECK(encoded && texel[0] == 51 && texel[1] == 0 && texel[2] == 255 && texel[3] == 255);
    // sRGB: 선형 0.5 → 188(Core LinearColor::ToSRGB), 알파는 선형 0.5 → 128.
    encoded = EncodeDefaultTexel(PixelFormat::RGBA8UnormSrgb, {0.5f, 0.0f, 1.0f, 0.5f}, out);
    CHECK(encoded && texel[0] == 188 && texel[1] == 0 && texel[2] == 255 && texel[3] == 128);
    encoded = EncodeDefaultTexel(PixelFormat::RGBA8Snorm, {-1.0f, -2.0f, 0.5f, 1.0f}, out);
    CHECK(encoded && static_cast<int8>(texel[0]) == -127 && static_cast<int8>(texel[1]) == -127 &&
          texel[2] == 64 && texel[3] == 127);
    encoded = EncodeDefaultTexel(PixelFormat::RGBA16Float, {1.0f, -2.0f, 65504.0f, 0.5f}, out);
    uint16 halves[4] = {};
    memcpy(halves, texel, sizeof(halves));
    CHECK(encoded && encoded.Value() == 8 && halves[0] == Float16::FromFloat(1.0f).bits &&
          halves[1] == Float16::FromFloat(-2.0f).bits && halves[2] == 0x7bffu &&
          halves[3] == Float16::FromFloat(0.5f).bits);
    encoded = EncodeDefaultTexel(PixelFormat::RGBA32Uint, {-5.0f, 7.9f, NAN, 1.0e12f}, out);
    uint32 words[4] = {};
    memcpy(words, texel, sizeof(words));
    CHECK(encoded && encoded.Value() == 16 && words[0] == 0 && words[1] == 7 && words[2] == 0 &&
          words[3] == 4294967040u);
    encoded = EncodeDefaultTexel(PixelFormat::R32Sint, {-3.0f, 0, 0, 0}, out);
    int32 signedValue = 0;
    memcpy(&signedValue, texel, sizeof(signedValue));
    CHECK(encoded && encoded.Value() == 4 && signedValue == -3);
    encoded = EncodeDefaultTexel(PixelFormat::R16Unorm, {0.5f, 0, 0, 0}, out);
    uint16 unorm = 0;
    memcpy(&unorm, texel, sizeof(unorm));
    CHECK(encoded && encoded.Value() == 2 && unorm == 32768);
    for (const PixelFormat format :
         {PixelFormat::BC1Unorm, PixelFormat::D32Float, PixelFormat::RGB10A2Unorm,
          PixelFormat::RG11B10Ufloat, PixelFormat::Unknown}) {
        CHECK(EncodeDefaultTexel(format, {}, out).Error() == ErrorCode::Unsupported);
    }

    CHECK(EncodeDefaultTexel(PixelFormat::RGBA8Unorm, {}, Span<uint8>(texel, 4)).Error() ==
          ErrorCode::InvalidArgument);
}

// 캐시: 키 적중·다른 값·cube 배열 능력·장치 실패·상한·Retire.
TEST(RenderBase_DefaultTextureCache) {
    RecordingDevice device(Allocator_());
    DefaultTextureCache cache;
    const graphics::ClearColor black = {0.0f, 0.0f, 0.0f, 1.0f};
    auto first = cache.Find(device, TextureDimension::Texture2D, PixelFormat::RGBA8Unorm, black);
    auto again = cache.Find(device, TextureDimension::Texture2D, PixelFormat::RGBA8Unorm, black);
    REQUIRE(first && again);
    CHECK(first.Value() == again.Value() && cache.Size() == 1);
    CHECK(CountOps(device, RecordingDevice::Op::CreateTexture) == 1);
    auto cube = cache.Find(device, TextureDimension::TextureCube, PixelFormat::RGBA8Unorm, black);
    auto volume = cache.Find(device, TextureDimension::Texture3D, PixelFormat::RGBA8Unorm, black);
    auto other = cache.Find(device, TextureDimension::Texture2D, PixelFormat::RGBA8Unorm,
                            {0.0f, 0.0f, 0.0f, 0.0f});
    REQUIRE(cube && volume && other);
    CHECK(cube.Value() != first.Value() && volume.Value() != first.Value() &&
          other.Value() != first.Value() && cache.Size() == 4);
    // −0과 +0은 다른 bit라 다른 키다(값은 bit 비교).
    auto negativeZero = cache.Find(device, TextureDimension::Texture2D, PixelFormat::RGBA8Unorm,
                                   {-0.0f, 0.0f, 0.0f, 1.0f});
    CHECK(negativeZero && negativeZero.Value() != first.Value());
    device.MutableCaps().features.textureCubeArray = false;
    CHECK(cache.Find(device, TextureDimension::TextureCubeArray, PixelFormat::RGBA8Unorm, black)
              .Error() == ErrorCode::Unsupported);
    CHECK(cache.Find(device, TextureDimension::Texture2D, PixelFormat::BC1Unorm, black).Error() ==
          ErrorCode::Unsupported);
    device.SetCreateTextureError(ErrorCode::OutOfMemory);
    const usize sizeBefore = cache.Size();
    CHECK(cache.Find(device, TextureDimension::Texture2D, PixelFormat::R32Float, black).Error() ==
          ErrorCode::OutOfMemory);
    CHECK(cache.Size() == sizeBefore);
    device.SetCreateTextureError(ErrorCode::None);
    // 상한: 값만 다른 항목으로 채운다.
    usize created = cache.Size();
    for (uint32 index = 0; created < DefaultTextureCache::MaxDefaultTextures; ++index) {
        const float value = static_cast<float>(index + 1) / 256.0f;
        created += cache.Find(device, TextureDimension::Texture2D, PixelFormat::R32Float,
                              {value, 0.0f, 0.0f, 1.0f})
                       ? 1u
                       : 0u;
    }

    CHECK(cache.Size() == DefaultTextureCache::MaxDefaultTextures);
    CHECK(cache
              .Find(device, TextureDimension::Texture2D, PixelFormat::R32Float,
                    {-1.0f, 0.0f, 0.0f, 1.0f})
              .Error() == ErrorCode::CapacityExceeded);
    // 이미 있는 키는 상한에서도 적중한다.
    CHECK(cache.Find(device, TextureDimension::Texture2D, PixelFormat::RGBA8Unorm, black));
    const usize retiredBefore = CountOps(device, RecordingDevice::Op::RetireTexture);
    cache.Retire(device);
    CHECK(cache.Size() == 0 && CountOps(device, RecordingDevice::Op::RetireTexture) ==
                                   retiredBefore + DefaultTextureCache::MaxDefaultTextures);
    auto recreated =
        cache.Find(device, TextureDimension::Texture2D, PixelFormat::RGBA8Unorm, black);
    CHECK(recreated && cache.Size() == 1);
    cache.Retire(device);
}

} // namespace utility_shader_tests_file
