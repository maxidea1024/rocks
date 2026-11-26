#include "RecordingDevice.h"

#include <Base/Compiler.h>
#include <Core/Memory/SystemAllocator.h>
#include <RenderBase/ParameterBatch.h>
#include <RenderBase/ShaderResourceMap.h>
#include <ShaderCore/ShaderBindingLayout.h>
#include <ShaderCore/ShaderFormatTypes.h>
#include <TestSupport/Test.h>
#include <string.h>

// 자원 지도와 묶음 설정 자원 부분 시험이다(Docs/Design/RenderBase 9.13절 5·6항): 손으로 쓴 자원
// 표와 layout 대조(양성·종류 불일치·표 누락·쓰지 않는 항목·이름 표 밖 버퍼 view·이름 중복), 값
// 검증(null·미설정·공유 샘플러·차원·MSAA·값 계열·ReadWrite·stale·버퍼 view 종류·stride·비교
// 샘플러), 검증 전 Apply 거부와 Apply가 낸 장치 호출.
using namespace rocks;
using namespace rocks::render;
using namespace rocks::shader;
using rocks::render::testing::RecordingDevice;

namespace parameter_batch_tests_file {

SystemAllocator& Allocator_() noexcept {
    static SystemAllocator allocator;
    return allocator;
}

ShaderBindingResource Resource(ShaderResourceKind kind, uint16 slot, const char* name,
                               ShaderResourceShape shape = ShaderResourceShape::None,
                               ShaderComponentType component = ShaderComponentType::None,
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

// VS: Heights(유닛 16)·HeightsSampler·Lights. PS: Albedo·Layers(2D 배열)·Codes(uint)·Lights·
// Items(구조화 32 byte)·Material 상수 버퍼(지도 밖)·AlbedoSampler·SharedClampSampler.
struct Stages {
    Stages() noexcept : vertex(Allocator_()), pixel(Allocator_()) {
        vertex.stage = ShaderStageVertex;
        pixel.stage = ShaderStagePixel;
        (void)vertex.resources.TryPushBack(Resource(ShaderResourceKind::SampledTexture, 16,
                                                    "Heights", ShaderResourceShape::Texture2D,
                                                    ShaderComponentType::Float));
        (void)vertex.resources.TryPushBack(Resource(ShaderResourceKind::SampledTexture, 17,
                                                    "Lights", ShaderResourceShape::TypedBuffer,
                                                    ShaderComponentType::Float));
        (void)vertex.resources.TryPushBack(
            Resource(ShaderResourceKind::Sampler, 0, "HeightsSampler"));
        ShaderBindingResource material =
            Resource(ShaderResourceKind::ConstantBuffer, 0, "Material");
        material.constantBufferBytes = 16;
        (void)pixel.resources.TryPushBack(material);
        (void)pixel.resources.TryPushBack(Resource(ShaderResourceKind::SampledTexture, 0, "Albedo",
                                                   ShaderResourceShape::Texture2D,
                                                   ShaderComponentType::Float));
        (void)pixel.resources.TryPushBack(Resource(ShaderResourceKind::SampledTexture, 1, "Layers",
                                                   ShaderResourceShape::Texture2DArray,
                                                   ShaderComponentType::Float));
        (void)pixel.resources.TryPushBack(Resource(ShaderResourceKind::SampledTexture, 2, "Codes",
                                                   ShaderResourceShape::Texture2D,
                                                   ShaderComponentType::Uint));
        (void)pixel.resources.TryPushBack(Resource(ShaderResourceKind::SampledTexture, 3, "Lights",
                                                   ShaderResourceShape::TypedBuffer,
                                                   ShaderComponentType::Float));
        (void)pixel.resources.TryPushBack(Resource(ShaderResourceKind::StorageBufferRead, 4,
                                                   "Items", ShaderResourceShape::StructuredBuffer,
                                                   ShaderComponentType::None, 32));
        (void)pixel.resources.TryPushBack(
            Resource(ShaderResourceKind::Sampler, 0, "AlbedoSampler"));
        ShaderBindingResource shared =
            Resource(ShaderResourceKind::Sampler, 1, "SharedClampSampler");
        shared.sharedSampler = true;
        (void)pixel.resources.TryPushBack(shared);
        names.bufferViews[1] = "Lights";
        names.bufferViews[2] = "Items";
    }

    ShaderBindingLayout vertex;
    ShaderBindingLayout pixel;
    graphics::PipelineBindingNames names;
};

constexpr ResourceParameter Table[] = {{"Heights", ResourceParameterKind::Texture},
                                       {"HeightsSampler", ResourceParameterKind::Sampler},
                                       {"Albedo", ResourceParameterKind::Texture},
                                       {"AlbedoSampler", ResourceParameterKind::Sampler},
                                       {"Layers", ResourceParameterKind::Texture},
                                       {"Codes", ResourceParameterKind::Texture},
                                       {"Lights", ResourceParameterKind::BufferView},
                                       {"Items", ResourceParameterKind::BufferView},
                                       {"SharedClampSampler", ResourceParameterKind::Sampler}};

usize CountOps(const RecordingDevice& device, RecordingDevice::Op op) noexcept {
    usize count = 0;
    for (const RecordingDevice::Call& call : device.Calls()) {
        count += call.op == op ? 1u : 0u;
    }

    return count;
}

bool Recorded(const RecordingDevice& device, RecordingDevice::Op op, uint64 value) noexcept {
    for (const RecordingDevice::Call& call : device.Calls()) {
        if (call.op == op && call.value == value) {
            return true;
        }
    }

    return false;
}

TEST(RenderBase_ShaderResourceMapMatchesLayouts) {
    Stages stages;
    const ShaderBindingLayout* layouts[] = {&stages.vertex, nullptr, &stages.pixel};
    const char* reason = "";
    auto map = ShaderResourceMap::Build(Allocator_(), Table, layouts, stages.names, &reason);
    REQUIRE(map);
    REQUIRE(map.Value().Table().Size() == 9);
    // VS 텍스처는 값 1 slot(유닛 16), 두 단계가 읽는 버퍼 view는 자리 둘(이름 표 slot 1).
    Span<const ResourcePlacement> heights = map.Value().PlacementsOf(0);
    REQUIRE(heights.Size() == 1);
    CHECK(heights[0].stage == graphics::ShaderStage::Vertex && heights[0].slot == 16 &&
          heights[0].shape == ShaderResourceShape::Texture2D);
    Span<const ResourcePlacement> lights = map.Value().PlacementsOf(6);
    REQUIRE(lights.Size() == 2);
    CHECK(lights[0].stage == graphics::ShaderStage::Vertex && lights[0].slot == 1 &&
          lights[1].stage == graphics::ShaderStage::Pixel && lights[1].slot == 1);
    CHECK(map.Value().PlacementsOf(7)[0].slot == 2 &&
          map.Value().PlacementsOf(7)[0].structureStride == 32);
    CHECK(map.Value().PlacementsOf(5)[0].component == ShaderComponentType::Uint);
    CHECK(map.Value().PlacementsOf(8)[0].sharedSampler &&
          !map.Value().PlacementsOf(3)[0].sharedSampler);
    CHECK(map.Value().Find("Layers").Value() == 4);
    CHECK(map.Value().Find("Material").Error() == ErrorCode::NotFound);
    CHECK(map.Value().PlacementsOf(9).IsEmpty());

    // 표 누락(layout과 표 불일치): Codes를 뺀 표.
    constexpr ResourceParameter missing[] = {
        {"Heights", ResourceParameterKind::Texture},
        {"HeightsSampler", ResourceParameterKind::Sampler},
        {"Albedo", ResourceParameterKind::Texture},
        {"AlbedoSampler", ResourceParameterKind::Sampler},
        {"Layers", ResourceParameterKind::Texture},
        {"Lights", ResourceParameterKind::BufferView},
        {"Items", ResourceParameterKind::BufferView},
        {"SharedClampSampler", ResourceParameterKind::Sampler}};
    CHECK(ShaderResourceMap::Build(Allocator_(), missing, layouts, stages.names, &reason).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(strstr(reason, "not in the resource table") != nullptr);

    // 종류 불일치: 텍스처를 샘플러로, 버퍼 view를 텍스처로.
    ResourceParameter swapped[9];
    for (usize index = 0; index < 9; ++index) {
        swapped[index] = Table[index];
    }

    swapped[2].kind = ResourceParameterKind::Sampler;
    CHECK(!ShaderResourceMap::Build(Allocator_(), swapped, layouts, stages.names, &reason));
    CHECK(strstr(reason, "differ in kind") != nullptr);
    swapped[2].kind = ResourceParameterKind::Texture;
    swapped[6].kind = ResourceParameterKind::Texture;
    CHECK(!ShaderResourceMap::Build(Allocator_(), swapped, layouts, stages.names, &reason));
    CHECK(strstr(reason, "differ in kind") != nullptr);
    swapped[6].kind = ResourceParameterKind::BufferView;

    // 쓰지 않는 항목, 이름 중복, 빈 이름.
    ResourceParameter extra[10];
    for (usize index = 0; index < 9; ++index) {
        extra[index] = Table[index];
    }

    extra[9] = {"Ghost", ResourceParameterKind::Texture};
    CHECK(!ShaderResourceMap::Build(Allocator_(), extra, layouts, stages.names, &reason));
    CHECK(strstr(reason, "used by no shader stage") != nullptr);
    extra[9] = {"Albedo", ResourceParameterKind::Texture};
    CHECK(!ShaderResourceMap::Build(Allocator_(), extra, layouts, stages.names, &reason));
    CHECK(strstr(reason, "twice") != nullptr);
    extra[9] = {StringView(), ResourceParameterKind::Texture};
    CHECK(!ShaderResourceMap::Build(Allocator_(), extra, layouts, stages.names, &reason));
    CHECK(strstr(reason, "empty name") != nullptr);

    // 이름 표 밖 버퍼 view.
    graphics::PipelineBindingNames noItems = stages.names;
    noItems.bufferViews[2] = StringView();
    CHECK(!ShaderResourceMap::Build(Allocator_(), Table, layouts, noItems, &reason));
    CHECK(strstr(reason, "pipeline binding names") != nullptr);
}

// 시험 자원이다(기록 장치).
struct Resources {
    explicit Resources(RecordingDevice& owner) noexcept : device(owner) {
        heights =
            Texture(graphics::TextureDimension::Texture2D, 1, graphics::PixelFormat::R32Float);
        albedo =
            Texture(graphics::TextureDimension::Texture2D, 1, graphics::PixelFormat::RGBA8Unorm);
        layers = Texture(graphics::TextureDimension::Texture2DArray, 2,
                         graphics::PixelFormat::RGBA8Unorm);
        codes = Texture(graphics::TextureDimension::Texture2D, 1, graphics::PixelFormat::R8Uint);
        graphics::TextureDesc msaaDesc;
        msaaDesc.width = 4;
        msaaDesc.height = 4;
        msaaDesc.format = graphics::PixelFormat::RGBA8Unorm;
        msaaDesc.usage = graphics::TextureUsage::RenderTarget;
        msaaDesc.sampleCount = 4;
        msaa = device.CreateTexture(msaaDesc, {}).Value();
        graphics::TextureViewDesc writable;
        writable.access = graphics::ViewAccess::ReadWrite;
        writable.mipCount = 1;
        written = device.CreateTextureView(albedo, writable).Value();
        typed = View(graphics::BufferViewKind::Typed, graphics::PixelFormat::RGBA32Float, 0,
                     graphics::ViewAccess::Read);
        structured = View(graphics::BufferViewKind::Structured, graphics::PixelFormat::Unknown, 32,
                          graphics::ViewAccess::Read);
        narrow = View(graphics::BufferViewKind::Structured, graphics::PixelFormat::Unknown, 16,
                      graphics::ViewAccess::Read);
        uints = View(graphics::BufferViewKind::Typed, graphics::PixelFormat::R32Uint, 0,
                     graphics::ViewAccess::Read);
        writes = View(graphics::BufferViewKind::Typed, graphics::PixelFormat::RGBA32Float, 0,
                      graphics::ViewAccess::ReadWrite);
    }

    graphics::TextureHandle Texture(graphics::TextureDimension dimension, uint32 layers,
                                    graphics::PixelFormat format) noexcept {
        graphics::TextureDesc desc;
        desc.dimension = dimension;
        desc.width = 4;
        desc.height = 4;
        desc.arraySize = layers;
        desc.format = format;
        return device.CreateTexture(desc, {}).Value();
    }

    graphics::BufferViewHandle View(graphics::BufferViewKind kind, graphics::PixelFormat format,
                                    uint32 stride, graphics::ViewAccess access) noexcept {
        const uint8 bytes[64] = {};
        graphics::BufferDesc desc;
        desc.size = sizeof(bytes);
        desc.usage =
            graphics::BufferUsage::ShaderRead | graphics::BufferUsage::ShaderWrite |
            (stride != 0 ? graphics::BufferUsage::Structured : graphics::BufferUsage::None);
        desc.stride = stride;
        auto buffer = device.CreateBuffer(desc, Span<const uint8>(bytes, sizeof(bytes)));
        graphics::BufferViewDesc view;
        view.kind = kind;
        view.format = format;
        view.access = access;
        return device.CreateBufferView(buffer.Value(), view).Value();
    }

    RecordingDevice& device;
    graphics::TextureHandle heights;
    graphics::TextureHandle albedo;
    graphics::TextureHandle layers;
    graphics::TextureHandle codes;
    graphics::TextureHandle msaa;
    graphics::TextureHandle written;
    graphics::BufferViewHandle typed;
    graphics::BufferViewHandle structured;
    graphics::BufferViewHandle narrow;
    graphics::BufferViewHandle uints;
    graphics::BufferViewHandle writes;
};

// 모든 값을 맞게 준다(공유 샘플러 포함).
void FillValid(ParameterBatch& batch, const Resources& resources) noexcept {
    (void)batch.SetTexture("Heights", resources.heights);
    (void)batch.SetTexture("Albedo", resources.albedo);
    (void)batch.SetTexture("Layers", resources.layers);
    (void)batch.SetTexture("Codes", resources.codes);
    (void)batch.SetBufferView("Lights", resources.typed);
    (void)batch.SetBufferView("Items", resources.structured);
    graphics::SamplerDesc clamp;
    clamp.addressU = graphics::AddressMode::Clamp;
    clamp.addressV = graphics::AddressMode::Clamp;
    (void)batch.SetSampler("SharedClampSampler", clamp);
}

TEST(RenderBase_ParameterBatchRejectsNullAndKindMismatch) {
    RecordingDevice device(Allocator_());
    Resources resources(device);
    Stages stages;
    const ShaderBindingLayout* layouts[] = {&stages.vertex, &stages.pixel};
    auto map = ShaderResourceMap::Build(Allocator_(), Table, layouts, stages.names);
    REQUIRE(map);
    ParameterBatch batch(Allocator_(), map.Value());
    // Initialize 전, 이름 없음, 다른 종류 항목, 잘못된 샘플러 desc.
    CHECK(batch.SetTexture("Albedo", resources.albedo).Error() == ErrorCode::InvalidArgument);
    REQUIRE(batch.Initialize());
    CHECK(batch.SetTexture("Nothing", resources.albedo).Error() == ErrorCode::NotFound);
    CHECK(batch.SetTexture("AlbedoSampler", resources.albedo).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(batch.SetBufferView("Albedo", resources.typed).Error() == ErrorCode::InvalidArgument);
    graphics::SamplerDesc broken;
    broken.maxAnisotropy = 0;
    CHECK(batch.SetSampler("AlbedoSampler", broken).Error() == ErrorCode::InvalidArgument);
    CHECK(batch.SetSampler("Albedo", graphics::SamplerDesc{}).Error() ==
          ErrorCode::InvalidArgument);

    const char* reason = "";
    usize failed = 0;
    // 값 없음: 첫 항목 Heights에서 멈춘다.
    CHECK(batch.Validate(device, &reason, &failed).Error() == ErrorCode::InvalidArgument);
    CHECK(failed == 0 && strstr(reason, "no value") != nullptr);
    FillValid(batch, resources);
    // 공유 샘플러 값 없음.
    REQUIRE(batch.ResetSampler("SharedClampSampler"));
    CHECK(!batch.Validate(device, &reason, &failed));
    CHECK(failed == 8 && strstr(reason, "shared sampler") != nullptr);
    FillValid(batch, resources);
    REQUIRE(batch.Validate(device, &reason));
    CHECK(batch.IsValidated());

    struct TextureCase {
        const char* name;
        graphics::TextureHandle texture;
        ErrorCode error;
        const char* why;
    };
    const TextureCase textures[] = {
        {"Albedo", {}, ErrorCode::InvalidArgument, "is null"},
        {"Layers", resources.albedo, ErrorCode::InvalidArgument, "dimension or sample count"},
        {"Albedo", resources.layers, ErrorCode::InvalidArgument, "dimension or sample count"},
        {"Albedo", resources.msaa, ErrorCode::InvalidArgument, "dimension or sample count"},
        {"Codes", resources.albedo, ErrorCode::InvalidArgument, "value type"},
        {"Albedo", resources.codes, ErrorCode::InvalidArgument, "value type"},
        {"Albedo", resources.written, ErrorCode::InvalidArgument, "read-write"},
        {"Albedo", graphics::TextureHandle{SlotHandle{12345, 7}}, ErrorCode::StaleHandle,
         "unknown to the device"},
    };
    for (const TextureCase& test : textures) {
        FillValid(batch, resources);
        REQUIRE(batch.SetTexture(test.name, test.texture));
        CHECK(batch.Validate(device, &reason).Error() == test.error);
        CHECK(strstr(reason, test.why) != nullptr);
        CHECK(!batch.IsValidated());
    }

    struct ViewCase {
        const char* name;
        graphics::BufferViewHandle view;
        const char* why;
    };
    const ViewCase views[] = {
        {"Lights", {}, "is null"},
        {"Lights", resources.structured, "kind, value type or structure stride"},
        {"Lights", resources.uints, "kind, value type or structure stride"},
        {"Items", resources.typed, "kind, value type or structure stride"},
        {"Items", resources.narrow, "kind, value type or structure stride"},
        {"Lights", resources.writes, "read-write"},
    };
    for (const ViewCase& test : views) {
        FillValid(batch, resources);
        REQUIRE(batch.SetBufferView(test.name, test.view));
        CHECK(batch.Validate(device, &reason).Error() == ErrorCode::InvalidArgument);
        CHECK(strstr(reason, test.why) != nullptr);
    }

    // 자기 샘플러 값의 비교 여부가 짝 텍스처 desc와 다름, 같으면 받는다.
    FillValid(batch, resources);
    graphics::SamplerDesc compare;
    compare.comparison = true;
    REQUIRE(batch.SetSampler("AlbedoSampler", compare));
    CHECK(!batch.Validate(device, &reason));
    CHECK(strstr(reason, "depth comparison") != nullptr);
    REQUIRE(batch.SetSampler("AlbedoSampler", graphics::SamplerDesc{}));
    CHECK(batch.Validate(device, &reason));
}

TEST(RenderBase_ParameterBatchAppliesValidatedValues) {
    RecordingDevice device(Allocator_());
    Resources resources(device);
    Stages stages;
    const ShaderBindingLayout* layouts[] = {&stages.vertex, &stages.pixel};
    auto map = ShaderResourceMap::Build(Allocator_(), Table, layouts, stages.names);
    REQUIRE(map);
    ParameterBatch batch(Allocator_(), map.Value());
    REQUIRE(batch.Initialize());
    FillValid(batch, resources);
    graphics::PassDesc pass;
    auto recording = device.BeginPass(pass);
    REQUIRE(recording);
    graphics::CommandContext& commands = *recording.Value();
    const char* reason = "";
    if constexpr (!IsReleaseBuild) {
        // 검증하지 않은 값은 묶지 않는다(Release 밖).
        CHECK(batch.Apply(commands, &reason).Error() == ErrorCode::InvalidArgument);
        CHECK(strstr(reason, "Validate") != nullptr);
        CHECK(CountOps(device, RecordingDevice::Op::SetTexture) == 0);
    }

    REQUIRE(batch.Validate(device));
    REQUIRE(batch.Apply(commands, &reason));
    using Op = RecordingDevice::Op;
    const auto vertex = graphics::ShaderStage::Vertex;
    const auto pixel = graphics::ShaderStage::Pixel;
    // 표 순서·단계 순서의 장치 호출: 텍스처 넷(VS 유닛 16), 자기 샘플러 둘은 값 없음(지움), 공유
    // 샘플러는 Clamp(필터 Trilinear), 버퍼 view는 이름 표 slot 1(두 단계)·2.
    CHECK(CountOps(device, Op::SetTexture) == 4 && CountOps(device, Op::SetSampler) == 3 &&
          CountOps(device, Op::SetBufferView) == 3);
    CHECK(Recorded(device, Op::SetTexture, RecordingDevice::ResourceValue(vertex, 16, 1)));
    CHECK(Recorded(device, Op::SetTexture, RecordingDevice::ResourceValue(pixel, 0, 1)));
    CHECK(Recorded(device, Op::SetTexture, RecordingDevice::ResourceValue(pixel, 1, 1)));
    CHECK(Recorded(device, Op::SetTexture, RecordingDevice::ResourceValue(pixel, 2, 1)));
    CHECK(Recorded(device, Op::SetSampler, RecordingDevice::ResourceValue(vertex, 0, 0)));
    CHECK(Recorded(device, Op::SetSampler, RecordingDevice::ResourceValue(pixel, 0, 0)));
    CHECK(Recorded(device, Op::SetSampler,
                   RecordingDevice::ResourceValue(
                       pixel, 1, 1u + static_cast<uint32>(graphics::Filter::Trilinear))));
    CHECK(Recorded(device, Op::SetBufferView, (1u << 1) | 1u));
    CHECK(Recorded(device, Op::SetBufferView, (2u << 1) | 1u));
    CHECK(device.NullTextureBindings() == 0);

    // 값을 바꾸면 다시 검증해야 묶는다. 자기 샘플러 값은 그 slot에 묶인다.
    graphics::SamplerDesc point;
    point.filter = graphics::Filter::Point;
    REQUIRE(batch.SetSampler("AlbedoSampler", point));
    CHECK(!batch.IsValidated());
    if constexpr (!IsReleaseBuild) {
        CHECK(!batch.Apply(commands));
    }

    REQUIRE(batch.Validate(device));
    REQUIRE(batch.Apply(commands));
    CHECK(Recorded(device, Op::SetSampler,
                   RecordingDevice::ResourceValue(
                       pixel, 0, 1u + static_cast<uint32>(graphics::Filter::Point))));
    REQUIRE(device.EndFrame());

    // 기록 밖 Apply는 장치의 SetSampler 거부를 그대로 돌려준다(앞 자리는 이미 묶였을 수 있다).
    CHECK(batch.Apply(commands, &reason).Error() == ErrorCode::InvalidArgument);
    CHECK(strstr(reason, "rejected a sampler") != nullptr);

    // 공유 샘플러 값이 빠지면 Release에서도 Apply가 막는다(장치 질의 없이 아는 값).
    REQUIRE(batch.ResetSampler("SharedClampSampler"));
    CHECK(!batch.Apply(commands));
}

} // namespace parameter_batch_tests_file
