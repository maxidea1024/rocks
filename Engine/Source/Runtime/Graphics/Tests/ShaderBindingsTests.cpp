#include <Core/Memory/SystemAllocator.h>
#include <Graphics/ShaderBindings.h>
#include <ShaderCore/ShaderBindingLayout.h>
#include <ShaderCore/ShaderFormatTypes.h>
#include <ShaderCore/ShaderGlesBindingHeader.h>
#include <TestSupport/Test.h>
#include <string.h>

// 장치 셰이더 공용 처리 시험이다(Docs/Design/ShaderSystem 16.1절 SH2e2): 코드·layout 검사와 복사,
// 고정 slot 이름 표 해석(PipelineBindingNames → backend 위치), 정점 입력 원소 타입 대조.
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
    names.texture = "Albedo";
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
    CHECK(slots.bindings[1].role == PipelineSlotRole::Texture && slots.bindings[1].location == 0);
    CHECK(slots.bindings[2].role == PipelineSlotRole::BufferView &&
          slots.bindings[2].location == 1);
    CHECK(slots.bindings[3].role == PipelineSlotRole::BufferView && slots.bindings[3].slot == 2 &&
          slots.bindings[3].shape == ShaderResourceShape::StructuredBuffer);
    CHECK(slots.bindings[4].role == PipelineSlotRole::Sampler);
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
    PipelineBindingNames renamed = names;
    renamed.texture = "Other";
    CHECK(!ResolvePipelineSlots(stages, renamed, 32, slots, error));
    // 텍스처 이름에 typed buffer, buffer view 이름에 텍스처.
    PipelineBindingNames swapped = names;
    swapped.texture = "Lights";
    swapped.bufferViews[0] = "Albedo";
    CHECK(!ResolvePipelineSlots(stages, swapped, 32, slots, error));
}

TEST(ShaderBindings_RejectsStageRulesAndMatchesVertexInputs) {
    // VS가 텍스처를 읽음, 상수 버퍼, 그래픽 단계 UAV, 같은 slot을 단계마다 다른 값 계열로 읽음.
    Stage vertex(shader::ShaderStageVertex);
    REQUIRE(vertex.Add(Resource(ShaderResourceKind::SampledTexture, 0, "Albedo",
                                ShaderResourceShape::Texture2D, ShaderComponentType::Float)));
    REQUIRE(vertex.Load(ShaderStage::Vertex));
    PipelineBindingNames names;
    names.texture = "Albedo";
    PipelineSlots slots;
    char error[PipelineSlotErrorBytes] = {};
    const DeviceShaderCode* only[] = {&vertex.code};
    CHECK(!ResolvePipelineSlots(only, names, 0, slots, error));
    CHECK(strstr(error, "vertex shader") != nullptr);

    Stage constants(shader::ShaderStagePixel);
    ShaderBindingResource view = Resource(ShaderResourceKind::ConstantBuffer, 0, "View",
                                          ShaderResourceShape::None, ShaderComponentType::None);
    view.constantBufferBytes = 16;
    REQUIRE(constants.Add(view));
    REQUIRE(constants.Load(ShaderStage::Pixel));
    const DeviceShaderCode* buffer[] = {&constants.code};
    CHECK(!ResolvePipelineSlots(buffer, names, 0, slots, error));
    CHECK(strstr(error, "constant buffer") != nullptr);

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

} // namespace shader_bindings_tests_file
