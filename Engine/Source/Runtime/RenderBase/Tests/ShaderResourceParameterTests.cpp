#include "RecordingDevice.h"

#include <Core/Memory/SystemAllocator.h>
#include <RenderBase/ParameterBatch.h>
#include <RenderBase/ShaderResourceMap.h>
#include <ShaderCore/ShaderBindingLayout.h>
#include <ShaderCore/ShaderFormatTypes.h>
#include <ShaderParameterTestFixtures/ShaderResourceFixtures.shaderparams.h>
#include <TestSupport/Test.h>
#include <stdio.h>
#include <string.h>

// 생성 파라미터 구조체 표의 자원 지도와 묶음 시험이다(Docs/Design/ShaderSystem 16.2절 SSD64·SSD66
// 1): 바인딩 layout v5의 (구조체 identity, 멤버 오프셋)으로 찾기(이름이 달라도 맞음), 짝
// 샘플러·공유 샘플러·쓰지 않는 멤버(자리 없음)·멤버가 아닌 쓰기 자원(지도 밖), 음성(생성 표
// 아님·멤버 아닌 자원· 다른 구조체·같은 오프셋의 다른 종류·이름 표 밖 버퍼 view),
// ParameterBatch::SetParameters의 장치 멤버 값·짝 샘플러 override·그래프 멤버 건너뜀과 Apply가 낸
// 장치 호출.
using namespace rocks;
using namespace rocks::render;
using namespace rocks::shader;
using rocks::render::testing::RecordingDevice;
using rocks::shader_test_fixtures::FixtureMaterial;

namespace shader_resource_parameter_tests_file {

SystemAllocator& Allocator_() noexcept {
    static SystemAllocator allocator;
    return allocator;
}

constexpr const ShaderParameterLayout& MaterialLayout = ShaderParameterLayoutOf<FixtureMaterial>();

const ShaderParameterResource& Member(StringView name) noexcept {
    for (const ShaderParameterResource& resource : MaterialLayout.resources) {
        if (resource.name == name) {
            return resource;
        }
    }

    return MaterialLayout.resources[0];
}

// 멤버의 바인딩 자원(워커가 낸 v5 항목과 같은 모양)이다. name은 셰이더 철자이고 지도는 보지 않는다.
ShaderBindingResource Bound(ShaderResourceKind kind, uint16 slot, const char* name,
                            const ShaderParameterResource& member) noexcept {
    ShaderBindingResource resource;
    resource.kind = kind;
    resource.slot = slot;
    resource.name = StringView(name);
    if (kind != ShaderResourceKind::Sampler) {
        resource.shape = member.shape;
        resource.component = member.component;
        resource.structureStride = member.structureStride;
    }

    resource.structId = MaterialLayout.structId;
    resource.memberOffset = member.offset;
    return resource;
}

// scene_depth의 slot이다. Vulkan binding은 종류를 이어 매겨 텍스처 slot이 이름 표 상한과 같거나 클
// 수 있다(이름 표 slot은 버퍼 view·쓰기 자원만이다).
constexpr uint16 DepthSlot = static_cast<uint16>(graphics::MaxPipelineNamedSlots);

// PS: cbuffer(지도 밖), albedo(셰이더 철자를 일부러 다르게)·짝, codes, lights(typed), scene_depth와
// 짝, SharedWrapSampler(공유), RocksDebugBuffer(멤버 아닌 쓰기 자원 — 지도 밖). shadow·coverage·
// scene_color는 셰이더가 쓰지 않는다.
struct Stages {
    Stages() noexcept : pixel(Allocator_()) {
        pixel.stage = ShaderStagePixel;
        ShaderBindingResource constants;
        constants.kind = ShaderResourceKind::ConstantBuffer;
        constants.name = "FixtureMaterial";
        constants.constantBufferBytes = MaterialLayout.totalBytes;
        (void)pixel.resources.TryPushBack(constants);
        (void)pixel.resources.TryPushBack(
            Bound(ShaderResourceKind::SampledTexture, 0, "albedoRenamed", Member("albedo")));
        (void)pixel.resources.TryPushBack(
            Bound(ShaderResourceKind::SampledTexture, 1, "codes", Member("codes")));
        (void)pixel.resources.TryPushBack(
            Bound(ShaderResourceKind::SampledTexture, 2, "lights", Member("lights")));
        (void)pixel.resources.TryPushBack(Bound(ShaderResourceKind::SampledTexture, DepthSlot,
                                                "scene_depth", Member("scene_depth")));
        ShaderBindingResource debug;
        debug.kind = ShaderResourceKind::StorageBufferReadWrite;
        debug.slot = 0;
        debug.name = "RocksDebugBuffer";
        debug.shape = ShaderResourceShape::RawBuffer;
        (void)pixel.resources.TryPushBack(debug);
        (void)pixel.resources.TryPushBack(
            Bound(ShaderResourceKind::Sampler, 0, "albedoSampler", Member("albedo")));
        (void)pixel.resources.TryPushBack(
            Bound(ShaderResourceKind::Sampler, 1, "scene_depthSampler", Member("scene_depth")));
        ShaderBindingResource shared;
        shared.kind = ShaderResourceKind::Sampler;
        shared.slot = 2;
        shared.name = "SharedWrapSampler";
        shared.sharedSampler = true;
        (void)pixel.resources.TryPushBack(shared);
        names.bufferViews[0] = "lights";
    }

    ShaderBindingResource& Find(StringView name) noexcept {
        for (ShaderBindingResource& resource : pixel.resources) {
            if (resource.name == name) {
                return resource;
            }
        }

        return pixel.resources[0];
    }

    ShaderBindingLayout pixel;
    graphics::PipelineBindingNames names;
};

usize EntryOf(const ShaderResourceMap& map, StringView name) noexcept {
    auto found = map.Find(name);
    return found ? found.Value() : map.Table().Size();
}

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

TEST(RenderBase_ShaderResourceMapFromGeneratedTable) {
    Stages stages;
    const ShaderBindingLayout* layouts[] = {nullptr, &stages.pixel};
    const char* reason = "";
    auto map =
        ShaderResourceMap::Build(Allocator_(), MaterialLayout, layouts, stages.names, &reason);
    if (!map) {
        fprintf(stderr, "%s\n", reason);
    }

    REQUIRE(map);
    const ShaderResourceMap& value = map.Value();
    CHECK(value.Layout() == &MaterialLayout);
    // 자원 7 + 짝 샘플러 4 + 공유 샘플러 1.
    REQUIRE(value.Table().Size() == 12);
    const usize albedo = EntryOf(value, "albedo");
    const usize pair = EntryOf(value, "albedoSampler");
    REQUIRE(albedo < value.Table().Size() && pair < value.Table().Size());
    CHECK(value.Table()[albedo].kind == ResourceParameterKind::Texture &&
          value.Table()[albedo].memberKind == ShaderParameterResourceKind::DeviceTexture &&
          value.Table()[albedo].memberOffset == Member("albedo").offset);
    CHECK(value.Table()[pair].kind == ResourceParameterKind::Sampler &&
          value.Table()[pair].memberOffset == Member("albedo").offset);
    // 셰이더 철자가 달라도(albedoRenamed) identity·멤버 오프셋으로 찾는다.
    REQUIRE(value.PlacementsOf(albedo).Size() == 1);
    CHECK(value.PlacementsOf(albedo)[0].stage == graphics::ShaderStage::Pixel &&
          value.PlacementsOf(albedo)[0].slot == 0);
    CHECK(value.PlacementsOf(pair)[0].slot == 0);
    CHECK(value.PlacementsOf(EntryOf(value, "codes"))[0].component == ShaderComponentType::Uint);
    // 버퍼 view는 이름 표 slot이다.
    const usize lights = EntryOf(value, "lights");
    CHECK(value.Table()[lights].kind == ResourceParameterKind::BufferView &&
          value.PlacementsOf(lights)[0].slot == 0);
    // 그래프 멤버(포함 구조체에서 펼친 이름)와 짝.
    const usize depth = EntryOf(value, "scene_depth");
    CHECK(value.Table()[depth].memberKind == ShaderParameterResourceKind::GraphTexture &&
          value.PlacementsOf(depth).Size() == 1 && value.PlacementsOf(depth)[0].slot == DepthSlot &&
          value.PlacementsOf(EntryOf(value, "scene_depthSampler"))[0].slot == 1);
    // 셰이더가 쓰지 않는 멤버는 자리 없는 항목이다(오류 아님).
    CHECK(value.PlacementsOf(EntryOf(value, "shadow")).IsEmpty() &&
          value.PlacementsOf(EntryOf(value, "shadowSampler")).IsEmpty() &&
          value.PlacementsOf(EntryOf(value, "coverage")).IsEmpty() &&
          value.PlacementsOf(EntryOf(value, "scene_color")).IsEmpty());
    // 공유 샘플러는 멤버 없는 항목이다.
    const usize shared = EntryOf(value, "SharedWrapSampler");
    REQUIRE(shared < value.Table().Size());
    CHECK(!value.Table()[shared].HasMember() && value.PlacementsOf(shared)[0].sharedSampler &&
          value.PlacementsOf(shared)[0].slot == 2);
    CHECK(EntryOf(value, "RocksDebugBuffer") == value.Table().Size());
    CHECK(StringView(ResourceParameterKindName(ResourceParameterKind::WritableBuffer)) ==
          "WritableBuffer");

    // 손으로 쓴 표(identity 없음)는 생성 표 경로가 아니다.
    const ShaderParameterLayout handWritten(MaterialLayout.name, false, MaterialLayout.totalBytes,
                                            MaterialLayout.members);
    CHECK(ShaderResourceMap::Build(Allocator_(), handWritten, layouts, stages.names, &reason)
              .Error() == ErrorCode::InvalidArgument);
    // 멤버가 아닌 텍스처, 다른 구조체의 멤버.
    {
        Stages other;
        other.Find("codes").structId = 0;
        other.Find("codes").memberOffset = NoShaderParameterMember;
        const ShaderBindingLayout* list[] = {&other.pixel};
        CHECK(!ShaderResourceMap::Build(Allocator_(), MaterialLayout, list, other.names, &reason));
        CHECK(strstr(reason, "not a member") != nullptr);
        other.Find("codes").structId = MaterialLayout.structId + 1;
        other.Find("codes").memberOffset = Member("codes").offset;
        CHECK(!ShaderResourceMap::Build(Allocator_(), MaterialLayout, list, other.names, &reason));
        CHECK(strstr(reason, "not a member") != nullptr);
    }

    // 같은 멤버 오프셋의 다른 종류(텍스처 멤버 자리에 구조화 버퍼).
    {
        Stages other;
        ShaderBindingResource& codes = other.Find("codes");
        codes.kind = ShaderResourceKind::StorageBufferRead;
        codes.shape = ShaderResourceShape::StructuredBuffer;
        codes.component = ShaderComponentType::None;
        codes.structureStride = 16;
        codes.slot = 9;
        const ShaderBindingLayout* list[] = {&other.pixel};
        CHECK(!ShaderResourceMap::Build(Allocator_(), MaterialLayout, list, other.names, &reason));
        CHECK(strstr(reason, "differ in kind") != nullptr);
    }

    // 버퍼 view가 이름 표에 없음.
    {
        Stages other;
        other.names.bufferViews[0] = StringView();
        const ShaderBindingLayout* list[] = {&other.pixel};
        CHECK(!ShaderResourceMap::Build(Allocator_(), MaterialLayout, list, other.names, &reason));
        CHECK(strstr(reason, "binding names") != nullptr);
    }
}

TEST(RenderBase_ParameterBatchSetsGeneratedParameters) {
    RecordingDevice device(Allocator_());
    Stages stages;
    const ShaderBindingLayout* layouts[] = {&stages.pixel};
    auto map = ShaderResourceMap::Build(Allocator_(), MaterialLayout, layouts, stages.names);
    REQUIRE(map);
    graphics::TextureDesc albedoDesc;
    albedoDesc.width = 4;
    albedoDesc.height = 4;
    albedoDesc.format = graphics::PixelFormat::RGBA8Unorm;
    graphics::TextureDesc codesDesc = albedoDesc;
    codesDesc.format = graphics::PixelFormat::R8Uint;
    // codes 멤버는 Texture2DArray<uint4>다.
    codesDesc.dimension = graphics::TextureDimension::Texture2DArray;
    codesDesc.arraySize = 2;
    const graphics::TextureHandle albedo = device.CreateTexture(albedoDesc, {}).Value();
    const graphics::TextureHandle codes = device.CreateTexture(codesDesc, {}).Value();
    const uint8 bufferBytes[64] = {};
    graphics::BufferDesc bufferDesc;
    bufferDesc.size = sizeof(bufferBytes);
    bufferDesc.usage = graphics::BufferUsage::ShaderRead;
    auto buffer =
        device.CreateBuffer(bufferDesc, Span<const uint8>(bufferBytes, sizeof(bufferBytes)));
    REQUIRE(buffer);
    graphics::BufferViewDesc viewDesc;
    viewDesc.kind = graphics::BufferViewKind::Typed;
    viewDesc.format = graphics::PixelFormat::RGBA32Float;
    auto lightsView = device.CreateBufferView(buffer.Value(), viewDesc);
    REQUIRE(lightsView);
    const graphics::BufferViewHandle lights = lightsView.Value();

    ParameterBatch batch(Allocator_(), map.Value());
    FixtureMaterial values;
    // Initialize 전.
    CHECK(batch.SetParameters(values).Error() == ErrorCode::InvalidArgument);
    REQUIRE(batch.Initialize());
    values.albedo.texture = albedo;
    values.codes.texture = codes;
    values.lights.view = lights;
    graphics::SamplerDesc point;
    point.filter = graphics::Filter::Point;
    values.albedo.sampler = SamplerOverride::Of(point);
    // 그래프 멤버(scene_depth)는 null 그대로다 — 묶음의 값이 아니다.
    REQUIRE(batch.SetParameters(values));
    graphics::SamplerDesc wrap;
    REQUIRE(batch.SetSampler("SharedWrapSampler", wrap));
    const char* reason = "";
    if (!batch.Validate(device, &reason)) {
        fprintf(stderr, "%s\n", reason);
        REQUIRE(false);
    }

    graphics::PassDesc pass;
    auto recording = device.BeginPass(pass);
    REQUIRE(recording);
    graphics::CommandContext& commands = *recording.Value();
    REQUIRE(batch.Apply(commands, &reason));
    using Op = RecordingDevice::Op;
    const auto pixel = graphics::ShaderStage::Pixel;
    // 장치 텍스처 둘만 묶는다(그래프 멤버 scene_depth와 그 짝, 쓰지 않는 멤버는 건너뛴다). 짝
    // 샘플러는 override(Point), 공유 샘플러는 준 값(Trilinear)이다.
    CHECK(CountOps(device, Op::SetTexture) == 2);
    CHECK(Recorded(device, Op::SetTexture, RecordingDevice::ResourceValue(pixel, 0, 1)));
    CHECK(Recorded(device, Op::SetTexture, RecordingDevice::ResourceValue(pixel, 1, 1)));
    CHECK(CountOps(device, Op::SetSampler) == 2);
    CHECK(Recorded(device, Op::SetSampler,
                   RecordingDevice::ResourceValue(
                       pixel, 0, 1u + static_cast<uint32>(graphics::Filter::Point))));
    CHECK(Recorded(device, Op::SetSampler,
                   RecordingDevice::ResourceValue(
                       pixel, 2, 1u + static_cast<uint32>(graphics::Filter::Trilinear))));
    CHECK(CountOps(device, Op::SetBufferView) == 1);
    CHECK(Recorded(device, Op::SetBufferView, (0u << 1) | 1u));
    CHECK(device.NullTextureBindings() == 0);

    // override를 지우면 짝 샘플러 값도 지워진다(텍스처 desc로 되돌림). 다시 검증해야 묶는다.
    values.albedo.sampler = SamplerOverride{};
    REQUIRE(batch.SetParameters(values));
    CHECK(!batch.IsValidated());
    REQUIRE(batch.Validate(device));
    REQUIRE(batch.Apply(commands));
    CHECK(Recorded(device, Op::SetSampler, RecordingDevice::ResourceValue(pixel, 0, 0)));
    REQUIRE(device.EndFrame());

    // 장치 멤버 null은 Validate가 막는다.
    values.codes.texture = graphics::TextureHandle{};
    REQUIRE(batch.SetParameters(values));
    CHECK(!batch.Validate(device));
    // 잘못된 샘플러 desc, 다른 layout, null 값.
    values.codes.texture = codes;
    values.albedo.sampler = SamplerOverride::Of(point);
    values.albedo.sampler.desc.maxAnisotropy = 0;
    CHECK(batch.SetParameters(values).Error() == ErrorCode::InvalidArgument);
    const shader::ShaderParameterLayout& other =
        ShaderParameterLayoutOf<shader_test_fixtures::FixtureComposite>();
    CHECK(batch.SetParameters(&values, other).Error() == ErrorCode::InvalidArgument);
    CHECK(batch.SetParameters(nullptr, MaterialLayout).Error() == ErrorCode::InvalidArgument);

    // 손으로 쓴 표 지도의 묶음은 구조체를 받지 않는다.
    constexpr ResourceParameter table[] = {{"codes", ResourceParameterKind::Texture}};
    ShaderBindingLayout plain(Allocator_());
    plain.stage = ShaderStagePixel;
    ShaderBindingResource texture;
    texture.kind = ShaderResourceKind::SampledTexture;
    texture.name = "codes";
    texture.shape = ShaderResourceShape::Texture2D;
    texture.component = ShaderComponentType::Uint;
    (void)plain.resources.TryPushBack(texture);
    const ShaderBindingLayout* plainList[] = {&plain};
    auto plainMap = ShaderResourceMap::Build(Allocator_(), table, plainList, stages.names);
    REQUIRE(plainMap);
    CHECK(plainMap.Value().Layout() == nullptr);
    ParameterBatch plainBatch(Allocator_(), plainMap.Value());
    REQUIRE(plainBatch.Initialize());
    values.albedo.sampler = SamplerOverride{};
    CHECK(plainBatch.SetParameters(values).Error() == ErrorCode::InvalidArgument);
}

} // namespace shader_resource_parameter_tests_file
