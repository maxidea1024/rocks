#include <Core/Memory/SystemAllocator.h>
#include <Graphics/ShaderBindings.h>
#include <ShaderCore/ShaderBindingLayout.h>
#include <ShaderCore/ShaderFormatTypes.h>
#include <ShaderCore/ShaderGlesBindingHeader.h>
#include <TestSupport/Test.h>
#include <stdio.h>
#include <string.h>

// 장치 셰이더 공용 처리 시험이다(Docs/Design/ShaderSystem 16.1절 SH2e2): 코드·layout 검사와 복사,
// 고정 slot 이름 표 해석(PipelineBindingNames → backend 위치), 텍스처·샘플러의 값 1 slot 해석과
// 짝(Docs/Design/RenderBase 9.13절), 정점 입력 원소 타입 대조.
using namespace rocks;
using namespace rocks::graphics;

namespace shader_bindings_tests_file {

SystemAllocator& Allocator_() noexcept {
    static SystemAllocator allocator;
    return allocator;
}

constexpr uint8 Code[] = {0x03, 0x02, 0x23, 0x07, 0, 0, 1, 0};

using shader::ShaderBindingLayout;
using shader::ShaderBindingResource;
using shader::ShaderComponentType;
using shader::ShaderResourceKind;
using shader::ShaderResourceShape;

ShaderBindingResource Resource(ShaderResourceKind kind, uint16 slot, const char* name,
                               ShaderResourceShape shape, ShaderComponentType component,
                               uint32 stride = 0) noexcept {
    ShaderBindingResource resource;
    resource.kind = kind;
    resource.slot = slot;
    resource.name = StringView(name);
    resource.shape = shape;
    resource.component = component;
    resource.structureStride = stride;
    return resource;
}

// 단계 layout을 만들어 장치 셰이더로 읽는다.
struct Stage {
    explicit Stage(uint32 stageBit) noexcept
        : layout(Allocator_()), bytes(Allocator_()), code(Allocator_()) {
        layout.stage = stageBit;
    }

    bool Add(const ShaderBindingResource& resource) noexcept {
        return static_cast<bool>(layout.resources.TryPushBack(resource));
    }

    bool Load(ShaderStage stage) noexcept {
        bytes.Clear();
        if (!shader::EncodeShaderBindingLayout(layout, bytes)) {
            return false;
        }

        ShaderCodeDesc desc;
        desc.kind = ShaderCodeKind::Dxil;
        desc.stage = stage;
        desc.code = Span<const uint8>(Code, sizeof(Code));
        desc.bindingLayout = bytes.View();
        const char* reason = "";
        return static_cast<bool>(code.Load(desc, ShaderCodeKind::Dxil, &reason));
    }

    ShaderBindingLayout layout;
    Array<uint8> bytes;
    DeviceShaderCode code;
};

TEST(ShaderBindings_DeviceShaderCodeChecksKindStageAndHeader) {
    Stage vertex(shader::ShaderStageVertex);
    REQUIRE(vertex.Load(ShaderStage::Vertex));
    CHECK(vertex.code.Stage() == ShaderStage::Vertex && vertex.code.Code().Size() == sizeof(Code));
    DeviceShaderCode code(Allocator_());
    ShaderCodeDesc desc;
    desc.kind = ShaderCodeKind::Dxil;
    desc.stage = ShaderStage::Vertex;
    desc.code = Span<const uint8>(Code, sizeof(Code));
    desc.bindingLayout = vertex.bytes.View();
    const char* reason = "";
    // 다른 backend 코드 종류, 빈 코드, 단계 불일치, GLES 머리 유무 위반, 손상 layout.
    CHECK(code.Load(desc, ShaderCodeKind::Spirv, &reason).Error() == ErrorCode::InvalidArgument);
    ShaderCodeDesc empty = desc;
    empty.code = {};
    CHECK(!code.Load(empty, ShaderCodeKind::Dxil, &reason));
    ShaderCodeDesc pixel = desc;
    pixel.stage = ShaderStage::Pixel;
    CHECK(!DeviceShaderCode(Allocator_()).Load(pixel, ShaderCodeKind::Dxil, &reason));
    CHECK(strstr(reason, "stage") != nullptr);
    ShaderCodeDesc essl = desc;
    essl.kind = ShaderCodeKind::Essl;
    CHECK(!DeviceShaderCode(Allocator_()).Load(essl, ShaderCodeKind::Essl, &reason));
    CHECK(strstr(reason, "GLES binding header") != nullptr);
    ShaderCodeDesc headed = desc;
    headed.glesBindingHeader = Span<const uint8>(Code, sizeof(Code));
    CHECK(!DeviceShaderCode(Allocator_()).Load(headed, ShaderCodeKind::Dxil, &reason));
    ShaderCodeDesc odd = desc;
    odd.kind = ShaderCodeKind::Spirv;
    odd.code = Span<const uint8>(Code, 6);
    CHECK(!DeviceShaderCode(Allocator_()).Load(odd, ShaderCodeKind::Spirv, &reason));
    ShaderCodeDesc broken = desc;
    broken.bindingLayout = Span<const uint8>(vertex.bytes.Data(), vertex.bytes.Size() - 1);
    CHECK(!DeviceShaderCode(Allocator_()).Load(broken, ShaderCodeKind::Dxil, &reason));

    // 표: 만들기·찾기·Retire 뒤 stale.
    DeviceShaderTable table(Allocator_());
    auto handle = table.Create(desc, ShaderCodeKind::Dxil, &reason);
    REQUIRE(handle);
    CHECK(table.Find(handle.Value()) != nullptr && table.Size() == 1);
    table.Retire(handle.Value());
    CHECK(table.Find(handle.Value()) == nullptr && table.Size() == 0);
}

TEST(ShaderBindings_ResolveNamesToBackendSlots) {
    Stage vertex(shader::ShaderStageVertex);
    vertex.layout.rootConstantBytes = 16;
    REQUIRE(vertex.Add(Resource(ShaderResourceKind::SampledTexture, 2, "Lights",
                                ShaderResourceShape::TypedBuffer, ShaderComponentType::Float)));
    REQUIRE(vertex.Load(ShaderStage::Vertex));
    Stage pixel(shader::ShaderStagePixel);
    pixel.layout.rootConstantBytes = 32;
    REQUIRE(pixel.Add(Resource(ShaderResourceKind::SampledTexture, 0, "Albedo",
                               ShaderResourceShape::Texture2D, ShaderComponentType::Float)));
    REQUIRE(pixel.Add(Resource(ShaderResourceKind::SampledTexture, 1, "Lights",
                               ShaderResourceShape::TypedBuffer, ShaderComponentType::Float)));
    REQUIRE(
        pixel.Add(Resource(ShaderResourceKind::StorageBufferRead, 3, "Items",
                           ShaderResourceShape::StructuredBuffer, ShaderComponentType::None, 32)));
    REQUIRE(pixel.Add(Resource(ShaderResourceKind::Sampler, 0, "AlbedoSampler",
                               ShaderResourceShape::None, ShaderComponentType::None)));
    REQUIRE(pixel.Load(ShaderStage::Pixel));
    const DeviceShaderCode* stages[] = {&vertex.code, &pixel.code};
    PipelineBindingNames names;
    names.bufferViews[0] = "Lights";
    names.bufferViews[2] = "Items";
    PipelineSlots slots;
    char error[PipelineSlotErrorBytes] = {};
    REQUIRE(ResolvePipelineSlots(stages, names, 32, slots, error));
    REQUIRE(slots.count == 5);
    // VS typed slot 0 = register 2, PS typed slot 0 = register 1(같은 slot을 단계마다 다른 위치로).
    CHECK(slots.bindings[0].stage == ShaderStage::Vertex &&
          slots.bindings[0].role == PipelineSlotRole::BufferView && slots.bindings[0].slot == 0 &&
          slots.bindings[0].location == 2);
    // 텍스처는 이름 표 없이 값 1 slot이고, 자기 샘플러와 서로의 위치를 pair로 잇는다.
    CHECK(slots.bindings[1].role == PipelineSlotRole::Texture && slots.bindings[1].location == 0 &&
          slots.bindings[1].pair == 0);
    CHECK(slots.bindings[2].role == PipelineSlotRole::BufferView &&
          slots.bindings[2].location == 1);
    CHECK(slots.bindings[3].role == PipelineSlotRole::BufferView && slots.bindings[3].slot == 2 &&
          slots.bindings[3].shape == ShaderResourceShape::StructuredBuffer);
    CHECK(slots.bindings[4].role == PipelineSlotRole::Sampler && slots.bindings[4].pair == 0);
    CHECK(slots.bufferSlots.usedMask == 0b101 &&
          slots.bufferSlots.kinds[0] == BufferViewKind::Typed &&
          slots.bufferSlots.typedShaderTypes[0] == PixelShaderType::Float &&
          slots.bufferSlots.kinds[2] == BufferViewKind::Structured &&
          slots.bufferSlots.structureStrides[2] == 32);
    CHECK(slots.rootConstantBytes[0] == 16 && slots.rootConstantBytes[1] == 32);

    // 음성 대조: 루트 상수가 선언보다 큼, 표에 없는 이름, 짝이 아닌 샘플러.
    CHECK(ResolvePipelineSlots(stages, names, 16, slots, error).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(strstr(error, "drawConstantBytes 16") != nullptr);
    PipelineBindingNames missing = names;
    missing.bufferViews[2] = StringView();
    CHECK(!ResolvePipelineSlots(stages, missing, 32, slots, error));
    CHECK(strstr(error, "Items") != nullptr);
    // buffer view 이름이 텍스처를 가리키면 typed buffer Lights가 표에 없다.
    PipelineBindingNames swapped = names;
    swapped.bufferViews[0] = "Albedo";
    CHECK(!ResolvePipelineSlots(stages, swapped, 32, slots, error));
    CHECK(strstr(error, "Lights") != nullptr);
}

TEST(ShaderBindings_RejectsStageRulesAndMatchesVertexInputs) {
    // VS 텍스처(이제 받는다), 상수 버퍼(이름 표 없이 받는다), 그래픽 단계 UAV, 같은 slot을 단계마다
    // 다른 값 계열로 읽음.
    Stage vertex(shader::ShaderStageVertex);
    REQUIRE(vertex.Add(Resource(ShaderResourceKind::SampledTexture, 0, "Albedo",
                                ShaderResourceShape::Texture2D, ShaderComponentType::Float)));
    REQUIRE(vertex.Load(ShaderStage::Vertex));
    PipelineBindingNames names;
    PipelineSlots slots;
    char error[PipelineSlotErrorBytes] = {};
    const DeviceShaderCode* only[] = {&vertex.code};
    REQUIRE(ResolvePipelineSlots(only, names, 0, slots, error));
    CHECK(slots.count == 1 && slots.bindings[0].stage == ShaderStage::Vertex &&
          slots.bindings[0].role == PipelineSlotRole::Texture &&
          slots.bindings[0].pair == NoPairedSlot);

    Stage constants(shader::ShaderStagePixel);
    ShaderBindingResource view = Resource(ShaderResourceKind::ConstantBuffer, 0, "View",
                                          ShaderResourceShape::None, ShaderComponentType::None);
    view.constantBufferBytes = 16;
    REQUIRE(constants.Add(view));
    REQUIRE(constants.Load(ShaderStage::Pixel));
    const DeviceShaderCode* buffer[] = {&constants.code};
    // 상수 버퍼는 이름 표 없이 값 1 slot으로 받는다(RenderBase 9.11절 GX6).
    REQUIRE(ResolvePipelineSlots(buffer, names, 0, slots, error));
    CHECK(slots.constantSlots.usedMask[ConstantBufferStageIndex(ShaderStage::Pixel)] == 1u &&
          slots.constantSlots.bytes[ConstantBufferStageIndex(ShaderStage::Pixel)][0] == 16);

    Stage uav(shader::ShaderStagePixel);
    REQUIRE(uav.Add(Resource(ShaderResourceKind::StorageBufferReadWrite, 0, "Output",
                             ShaderResourceShape::RawBuffer, ShaderComponentType::None)));
    REQUIRE(uav.Load(ShaderStage::Pixel));
    PipelineBindingNames writes;
    writes.writableBuffers[0] = "Output";
    const DeviceShaderCode* writer[] = {&uav.code};
    CHECK(!ResolvePipelineSlots(writer, writes, 0, slots, error));
    CHECK(strstr(error, "compute only") != nullptr);

    Stage floats(shader::ShaderStageVertex);
    REQUIRE(floats.Add(Resource(ShaderResourceKind::SampledTexture, 1, "Codes",
                                ShaderResourceShape::TypedBuffer, ShaderComponentType::Float)));
    REQUIRE(floats.Load(ShaderStage::Vertex));
    Stage uints(shader::ShaderStagePixel);
    REQUIRE(uints.Add(Resource(ShaderResourceKind::SampledTexture, 1, "Codes",
                               ShaderResourceShape::TypedBuffer, ShaderComponentType::Uint)));
    REQUIRE(uints.Load(ShaderStage::Pixel));
    PipelineBindingNames codes;
    codes.bufferViews[1] = "Codes";
    const DeviceShaderCode* mixed[] = {&floats.code, &uints.code};
    CHECK(!ResolvePipelineSlots(mixed, codes, 0, slots, error));

    // compute 쓰기 slot 하나에 버퍼와 텍스처를 함께 두지 못한다.
    Stage compute(shader::ShaderStageCompute);
    REQUIRE(compute.Add(Resource(ShaderResourceKind::StorageBufferReadWrite, 0, "Items",
                                 ShaderResourceShape::RawBuffer, ShaderComponentType::None)));
    REQUIRE(compute.Add(Resource(ShaderResourceKind::StorageImage, 1, "Target",
                                 ShaderResourceShape::Texture2D, ShaderComponentType::Float)));
    REQUIRE(compute.Load(ShaderStage::Compute));
    PipelineBindingNames shared;
    shared.writableBuffers[0] = "Items";
    shared.writableTextures[0] = "Target";
    const DeviceShaderCode* cs[] = {&compute.code};
    CHECK(!ResolvePipelineSlots(cs, shared, 0, slots, error));
    shared.writableTextures[0] = StringView();
    shared.writableTextures[1] = "Target";
    REQUIRE(ResolvePipelineSlots(cs, shared, 0, slots, error));
    CHECK(slots.writeTextures == 0b10 && slots.writeSlots.usedMask == 0b1 &&
          slots.writeSlots.kinds[0] == BufferViewKind::Raw);

    // 정점 입력: float·int·uint 원소 타입과 형식 계열 대조, 없는 location.
    ShaderBindingLayout inputs(Allocator_());
    inputs.stage = shader::ShaderStageVertex;
    inputs.attributeMask = 0b111;
    inputs.attributeIntMask = 0b010;
    inputs.attributeUintMask = 0b100;
    const VertexAttribute fitting[] = {{0, VertexFormat::Float3, 0, 0},
                                       {1, VertexFormat::Short4, 0, 12},
                                       {2, VertexFormat::UShort4, 0, 20}};
    const VertexStream streams[] = {{28, VertexStepRate::Vertex}};
    CHECK(MatchShaderVertexInputs(inputs, {fitting, streams}, error));
    const VertexAttribute wrong[] = {{0, VertexFormat::Float3, 0, 0},
                                     {1, VertexFormat::UByte4Unorm, 0, 12},
                                     {2, VertexFormat::UShort4, 0, 16}};
    CHECK(MatchShaderVertexInputs(inputs, {wrong, streams}, error).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(strstr(error, "location 1") != nullptr);
    CHECK(!MatchShaderVertexInputs(inputs, {Span<const VertexAttribute>(fitting, 2), streams},
                                   error));
    CHECK(strstr(error, "location 2") != nullptr);
}

// 텍스처·샘플러는 값 1 slot으로 받는다(RenderBase 9.13절 1항): 여러 텍스처·자기·공유 샘플러 짝,
// 공유 출처 텍스처의 pair, 찾기 함수와 모양 대조, 상한 거부.
TEST(ShaderBindings_ResolvesTextureAndSamplerSlots) {
    Stage pixel(shader::ShaderStagePixel);
    REQUIRE(pixel.Add(Resource(ShaderResourceKind::SampledTexture, 0, "Albedo",
                               ShaderResourceShape::Texture2D, ShaderComponentType::Float)));
    REQUIRE(pixel.Add(Resource(ShaderResourceKind::SampledTexture, 1, "Layers",
                               ShaderResourceShape::Texture2DArray, ShaderComponentType::Float)));
    ShaderBindingResource detail =
        Resource(ShaderResourceKind::SampledTexture, 3, "Detail", ShaderResourceShape::Texture2D,
                 ShaderComponentType::Float);
    detail.samplerSource = shader::ShaderSamplerSource::SharedClamp;
    REQUIRE(pixel.Add(detail));
    REQUIRE(pixel.Add(Resource(ShaderResourceKind::SampledTexture, 4, "Codes",
                               ShaderResourceShape::Texture2D, ShaderComponentType::Uint)));
    REQUIRE(pixel.Add(Resource(ShaderResourceKind::Sampler, 0, "AlbedoSampler",
                               ShaderResourceShape::None, ShaderComponentType::None)));
    REQUIRE(pixel.Add(Resource(ShaderResourceKind::Sampler, 2, "LayersSampler",
                               ShaderResourceShape::None, ShaderComponentType::None)));
    ShaderBindingResource shared = Resource(ShaderResourceKind::Sampler, 5, "SharedClampSampler",
                                            ShaderResourceShape::None, ShaderComponentType::None);
    shared.sharedSampler = true;
    REQUIRE(pixel.Add(shared));
    REQUIRE(pixel.Load(ShaderStage::Pixel));
    Stage vertex(shader::ShaderStageVertex);
    REQUIRE(vertex.Add(Resource(ShaderResourceKind::SampledTexture, 16, "Heights",
                                ShaderResourceShape::Texture2D, ShaderComponentType::Float)));
    REQUIRE(vertex.Add(Resource(ShaderResourceKind::Sampler, 0, "HeightsSampler",
                                ShaderResourceShape::None, ShaderComponentType::None)));
    REQUIRE(vertex.Load(ShaderStage::Vertex));
    const DeviceShaderCode* stages[] = {&vertex.code, &pixel.code};
    PipelineSlots slots;
    char error[PipelineSlotErrorBytes] = {};
    REQUIRE(ResolvePipelineSlots(stages, {}, 0, slots, error));
    REQUIRE(slots.count == 9);
    // VS 텍스처(GLES 형식이면 유닛 16부터)와 그 샘플러.
    CHECK(slots.bindings[0].stage == ShaderStage::Vertex &&
          slots.bindings[0].role == PipelineSlotRole::Texture && slots.bindings[0].location == 16 &&
          slots.bindings[0].pair == 0);
    CHECK(slots.bindings[1].role == PipelineSlotRole::Sampler && slots.bindings[1].pair == 16);
    // PS: 자기 샘플러 짝 둘, 공유 출처 텍스처, 샘플러 없는 정수 텍스처.
    CHECK(slots.bindings[2].location == 0 && slots.bindings[2].pair == 0);
    CHECK(slots.bindings[3].location == 1 && slots.bindings[3].pair == 2 &&
          slots.bindings[3].shape == ShaderResourceShape::Texture2DArray);
    CHECK(slots.bindings[4].location == 3 && slots.bindings[4].pair == 5);
    CHECK(slots.bindings[5].location == 4 && slots.bindings[5].pair == NoPairedSlot);
    CHECK(slots.bindings[6].role == PipelineSlotRole::Sampler && slots.bindings[6].pair == 0);
    CHECK(slots.bindings[7].role == PipelineSlotRole::Sampler && slots.bindings[7].pair == 1);
    CHECK(slots.bindings[8].role == PipelineSlotRole::SharedSampler &&
          slots.bindings[8].location == 5 && slots.bindings[8].pair == NoPairedSlot);

    // 찾기: 텍스처 모양만 텍스처 slot이다(typed buffer는 버퍼 view).
    CHECK(FindTextureSlot(pixel.code.Layout(), "Layers").Value() == 1);
    CHECK(FindTextureSlot(pixel.code.Layout(), "AlbedoSampler").Error() == ErrorCode::NotFound);
    CHECK(FindSamplerSlot(pixel.code.Layout(), "SharedClampSampler").Value() == 5);
    CHECK(FindSamplerSlot(pixel.code.Layout(), "Albedo").Error() == ErrorCode::NotFound);
    Stage typed(shader::ShaderStagePixel);
    REQUIRE(typed.Add(Resource(ShaderResourceKind::SampledTexture, 0, "Lights",
                               ShaderResourceShape::TypedBuffer, ShaderComponentType::Float)));
    REQUIRE(typed.Load(ShaderStage::Pixel));
    CHECK(FindTextureSlot(typed.code.Layout(), "Lights").Error() == ErrorCode::NotFound);

    // 모양 대조(자원 묶음 검증).
    CHECK(TextureShapeAccepts(ShaderResourceShape::Texture2D, TextureDimension::Texture2D, 1));
    CHECK(!TextureShapeAccepts(ShaderResourceShape::Texture2D, TextureDimension::Texture2D, 4));
    CHECK(TextureShapeAccepts(ShaderResourceShape::Texture2DMultisample,
                              TextureDimension::Texture2D, 4));
    CHECK(
        !TextureShapeAccepts(ShaderResourceShape::Texture2DArray, TextureDimension::Texture2D, 1));
    CHECK(TextureShapeAccepts(ShaderResourceShape::Texture2DMultisampleArray,
                              TextureDimension::Texture2DArray, 2));
    CHECK(TextureShapeAccepts(ShaderResourceShape::TextureCube, TextureDimension::TextureCube, 1));
    CHECK(!TextureShapeAccepts(ShaderResourceShape::TextureCube, TextureDimension::Texture2DArray,
                               1));
    CHECK(TextureShapeAccepts(ShaderResourceShape::TextureCubeArray,
                              TextureDimension::TextureCubeArray, 1));
    CHECK(TextureShapeAccepts(ShaderResourceShape::Texture3D, TextureDimension::Texture3D, 1));
    CHECK(!TextureShapeAccepts(ShaderResourceShape::Texture1D, TextureDimension::Texture2D, 1));
    CHECK(!TextureShapeAccepts(ShaderResourceShape::TypedBuffer, TextureDimension::Texture2D, 1));
    CHECK(PixelShaderTypeOf(ShaderComponentType::Uint) == PixelShaderType::Uint &&
          PixelShaderTypeOf(ShaderComponentType::None) == PixelShaderType::None);

    // 음성: 짝 텍스처가 없는 샘플러, 다른 단계의 텍스처와 짝인 샘플러.
    Stage orphan(shader::ShaderStagePixel);
    REQUIRE(orphan.Add(Resource(ShaderResourceKind::Sampler, 0, "AlbedoSampler",
                                ShaderResourceShape::None, ShaderComponentType::None)));
    REQUIRE(orphan.Load(ShaderStage::Pixel));
    const DeviceShaderCode* lonely[] = {&orphan.code};
    CHECK(ResolvePipelineSlots(lonely, {}, 0, slots, error).Error() == ErrorCode::InvalidArgument);
    CHECK(strstr(error, "AlbedoSampler") != nullptr);
    Stage cross(shader::ShaderStagePixel);
    REQUIRE(cross.Add(Resource(ShaderResourceKind::Sampler, 0, "HeightsSampler",
                               ShaderResourceShape::None, ShaderComponentType::None)));
    REQUIRE(cross.Load(ShaderStage::Pixel));
    const DeviceShaderCode* crossed[] = {&vertex.code, &cross.code};
    CHECK(!ResolvePipelineSlots(crossed, {}, 0, slots, error));
    CHECK(strstr(error, "pixel shader resource HeightsSampler") != nullptr);

    // 음성: slot 번호가 MaxResourceSlots 밖(바로 아래는 받는다).
    Stage edge(shader::ShaderStagePixel);
    REQUIRE(edge.Add(Resource(ShaderResourceKind::SampledTexture, MaxResourceSlots - 1, "Edge",
                              ShaderResourceShape::Texture2D, ShaderComponentType::Float)));
    REQUIRE(edge.Load(ShaderStage::Pixel));
    const DeviceShaderCode* inside[] = {&edge.code};
    CHECK(ResolvePipelineSlots(inside, {}, 0, slots, error));
    Stage beyond(shader::ShaderStagePixel);
    REQUIRE(beyond.Add(Resource(ShaderResourceKind::SampledTexture, MaxResourceSlots, "Beyond",
                                ShaderResourceShape::Texture2D, ShaderComponentType::Float)));
    REQUIRE(beyond.Load(ShaderStage::Pixel));
    const DeviceShaderCode* outside[] = {&beyond.code};
    CHECK(!ResolvePipelineSlots(outside, {}, 0, slots, error));
    CHECK(strstr(error, "MaxResourceSlots") != nullptr);
    Stage far(shader::ShaderStagePixel);
    REQUIRE(far.Add(Resource(ShaderResourceKind::SampledTexture, 0, "Near",
                             ShaderResourceShape::Texture2D, ShaderComponentType::Float)));
    REQUIRE(far.Add(Resource(ShaderResourceKind::Sampler, MaxResourceSlots, "NearSampler",
                             ShaderResourceShape::None, ShaderComponentType::None)));
    REQUIRE(far.Load(ShaderStage::Pixel));
    const DeviceShaderCode* farSampler[] = {&far.code};
    CHECK(!ResolvePipelineSlots(farSampler, {}, 0, slots, error));

    // 음성: 단계 SRV 텍스처 MaxStageTextures + 1, 샘플러 MaxStageSamplers + 1(경계는 받는다).
    static char textureNames[MaxStageTextures + 1][16];
    static char samplerNames[MaxStageSamplers + 1][24];
    Stage many(shader::ShaderStagePixel);
    Stage manyFit(shader::ShaderStagePixel);
    for (uint32 index = 0; index <= MaxStageTextures; ++index) {
        snprintf(textureNames[index], sizeof(textureNames[index]), "T%u", index);
        const ShaderBindingResource texture = Resource(
            ShaderResourceKind::SampledTexture, static_cast<uint16>(index), textureNames[index],
            ShaderResourceShape::Texture2D, ShaderComponentType::Float);
        REQUIRE(many.Add(texture));
        if (index < MaxStageTextures) {
            REQUIRE(manyFit.Add(texture));
        }
    }

    REQUIRE(many.Load(ShaderStage::Pixel) && manyFit.Load(ShaderStage::Pixel));
    const DeviceShaderCode* tooMany[] = {&many.code};
    CHECK(!ResolvePipelineSlots(tooMany, {}, 0, slots, error));
    CHECK(strstr(error, "MaxStageTextures") != nullptr);
    const DeviceShaderCode* fitting[] = {&manyFit.code};
    CHECK(ResolvePipelineSlots(fitting, {}, 0, slots, error));
    CHECK(slots.count == MaxStageTextures);
    Stage samplers(shader::ShaderStagePixel);
    for (uint32 index = 0; index <= MaxStageSamplers; ++index) {
        REQUIRE(samplers.Add(Resource(ShaderResourceKind::SampledTexture,
                                      static_cast<uint16>(index), textureNames[index],
                                      ShaderResourceShape::Texture2D, ShaderComponentType::Float)));
    }

    for (uint32 index = 0; index <= MaxStageSamplers; ++index) {
        snprintf(samplerNames[index], sizeof(samplerNames[index]), "T%uSampler", index);
        REQUIRE(samplers.Add(Resource(ShaderResourceKind::Sampler, static_cast<uint16>(index),
                                      samplerNames[index], ShaderResourceShape::None,
                                      ShaderComponentType::None)));
    }

    REQUIRE(samplers.Load(ShaderStage::Pixel));
    const DeviceShaderCode* tooManySamplers[] = {&samplers.code};
    CHECK(!ResolvePipelineSlots(tooManySamplers, {}, 0, slots, error));
    CHECK(strstr(error, "MaxStageSamplers") != nullptr);
}

} // namespace shader_bindings_tests_file
