#include "ShaderResourceSelfTest.h"
#include "SandboxShaders.h"

#include <Graphics/ShaderBindings.h>
#include <RenderBase/ParameterBatch.h>
#include <RenderBase/ShaderResourceMap.h>
#include <stdio.h>

// 생성 셰이더 파라미터 구조체 자원 멤버 self-test다(Docs/Design/ShaderSystem 16.2절 SSD66 4).
// 셰이더는 GeneratedResources.rsf이고 구조체는 SandboxShaderParameters.h의
// GeneratedResourceParameters다. 대상의 열 x가 자원 하나를 보여 준다(점 필터·UNORM8·정수라
// 정확하다 — 허용 오차 ±2).
namespace sandbox {

namespace {

using namespace rocks;
using namespace rocks::graphics;
using shaders::GeneratedResourceParameters;

// 대상 열 수다(GeneratedResourcesPS의 열 0–2).
constexpr uint32 TargetWidth = 3;
constexpr int ColorTolerance = 2;
// RGBA8 texel 하나의 byte 수다.
constexpr uint32 TexelBytes = 4;
// albedo 녹색을 반으로 줄이는 상수(cbuffer 값이 C++ 앞부분 그대로 올라가는지 본다)다.
constexpr float GreenScale = 0.5f;

uint32 failures = 0;
const char* backendName = "";

void Check(bool condition, const char* what) noexcept {
    if (!condition) {
        ++failures;
        printf("  [FAIL] %s shader resources: %s\n", backendName, what);
    }
}

struct Rgba {
    int r = 0;
    int g = 0;
    int b = 0;
    int a = 0;
};

// 열마다 기대 색이다: albedo(desc Wrap이면 왼쪽 빨강, override Clamp면 오른쪽 초록 × 0.5), codes
// 정수 (10, 20, 30, 255), lights (0.25, 0.5, 0.75, 1).
constexpr Rgba WrappedAlbedo = {255, 0, 0, 255};
constexpr Rgba ClampedAlbedo = {0, 128, 0, 255};
constexpr Rgba CodeColor = {10, 20, 30, 255};
constexpr Rgba LightColor = {64, 128, 191, 255};

bool Near(int actual, int expected) noexcept {
    return actual - expected <= ColorTolerance && expected - actual <= ColorTolerance;
}

bool PixelIs(const uint8* pixels, uint32 column, Rgba expected) noexcept {
    const uint8* pixel = pixels + column * TexelBytes;
    return Near(pixel[0], expected.r) && Near(pixel[1], expected.g) && Near(pixel[2], expected.b) &&
           Near(pixel[3], expected.a);
}

void PrintPixels(const uint8* pixels) noexcept {
    for (uint32 column = 0; column < TargetWidth; ++column) {
        const uint8* pixel = pixels + column * TexelBytes;
        printf("    column %u: %u %u %u %u\n", column, pixel[0], pixel[1], pixel[2], pixel[3]);
    }
}

Result<TextureHandle> MakeTexture(Device& device, uint32 width, PixelFormat format,
                                  Span<const uint8> texels) noexcept {
    TextureDesc desc;
    desc.width = width;
    desc.height = 1;
    desc.format = format;
    desc.sampler.filter = Filter::Point;
    desc.sampler.addressU = AddressMode::Wrap;
    desc.sampler.addressV = AddressMode::Wrap;
    return device.CreateTexture(desc, texels);
}

// 값 1의 자원 항목이 생성 표의 자원 멤버(identity·멤버 오프셋)를 가리키는지 본다.
bool CarriesMembers(const shader::ShaderBindingLayout& layout,
                    const shader::ShaderParameterLayout& table) noexcept {
    usize matched = 0;
    for (const shader::ShaderBindingResource& resource : layout.resources) {
        if (resource.kind == shader::ShaderResourceKind::ConstantBuffer) {
            continue;
        }

        for (const shader::ShaderParameterResource& member : table.resources) {
            const StringView name = resource.kind == shader::ShaderResourceKind::Sampler
                                        ? member.samplerName
                                        : member.name;
            if (!name.IsEmpty() && name == resource.name) {
                matched +=
                    resource.structId == table.structId && resource.memberOffset == member.offset
                        ? 1
                        : 0;
            }
        }
    }

    // albedo·albedoSampler·codes·lights
    constexpr usize Expected = 4;
    return matched == Expected;
}

struct Bench {
    Device* device = nullptr;
    TextureHandle target;
    uint8 pixels[TargetWidth * TexelBytes] = {};

    template <class Record>
    bool Draw(Record&& record) noexcept {
        auto frame = device->BeginOffscreen(target, ClearColor{0.0f, 0.0f, 0.0f, 0.0f});
        if (!frame) {
            return false;
        }

        record(*frame.Value());
        return device->EndFrame().IsOk() &&
               device->ReadPixels(target, Span<uint8>(pixels, sizeof(pixels))).IsOk();
    }

    bool Shows(Rgba albedo) const noexcept {
        return PixelIs(pixels, 0, albedo) && PixelIs(pixels, 1, CodeColor) &&
               PixelIs(pixels, 2, LightColor);
    }
};

void RunDraws(Device& device, Allocator& allocator, Bench& bench) noexcept {
    if (!HasPixelFormatCaps(device.GetPixelFormatCaps(PixelFormat::RGBA8Uint),
                            PixelFormatCaps::Sampled)) {
        printf("  [NOT RUN] %s shader resources: RGBA8Uint is not sampled on this device\n",
               backendName);
        return;
    }

    const shader::ShaderParameterLayout& table =
        shader::ShaderParameterLayoutOf<GeneratedResourceParameters>();
    const shader::ShaderBindingLayout* pixel = shaders::Layout(shaders::GeneratedResourcesPS);
    Check(pixel != nullptr && CarriesMembers(*pixel, table),
          "the binding layout carries the generated struct identity and member offsets");
    auto constants = pixel != nullptr ? FindConstantBufferSlot(*pixel, table.name)
                                      : Result<uint32>::Failure(ErrorCode::NotFound);
    Check(constants.IsOk(), "the generated cbuffer is in the pixel binding layout");

    const uint8 albedo[] = {255, 0, 0, 255, 0, 255, 0, 255};
    const uint8 codes[] = {10, 20, 30, 255};
    const float light[] = {0.25f, 0.5f, 0.75f, 1.0f};
    auto albedoTexture = MakeTexture(device, 2, PixelFormat::RGBA8Unorm, albedo);
    auto codesTexture = MakeTexture(device, 1, PixelFormat::RGBA8Uint, codes);
    BufferDesc bufferDesc;
    bufferDesc.size = sizeof(light);
    bufferDesc.usage = BufferUsage::ShaderRead;
    auto lightBuffer = device.CreateBuffer(
        bufferDesc, Span<const uint8>(reinterpret_cast<const uint8*>(light), sizeof(light)));
    Result<BufferViewHandle> lightView = Result<BufferViewHandle>::Failure(ErrorCode::NotFound);
    if (lightBuffer) {
        BufferViewDesc viewDesc;
        viewDesc.kind = BufferViewKind::Typed;
        viewDesc.format = PixelFormat::RGBA32Float;
        lightView = device.CreateBufferView(lightBuffer.Value(), viewDesc);
    }

    PipelineDesc desc;
    desc.colorFormats[0] = PixelFormat::RGBA8Unorm;
    desc.bindings.bufferViews[0] = "lights";
    auto pipeline = shaders::CreateGraphicsPipeline(device, shaders::GeneratedResourcesVS,
                                                    shaders::GeneratedResourcesPS, desc);
    Check(albedoTexture && codesTexture && lightBuffer && lightView, "resources are created");
    Check(pipeline.IsOk(), "the pipeline is created");
    const shader::ShaderBindingLayout* stages[] = {shaders::Layout(shaders::GeneratedResourcesVS),
                                                   pixel};
    const char* reason = "";
    auto map = render::ShaderResourceMap::Build(allocator, table, stages, desc.bindings, &reason);
    Check(map.IsOk(), "the resource map is built from the generated table");
    if (!map) {
        printf("    %s\n", reason);
    }

    const bool ready = albedoTexture && codesTexture && lightView && pipeline && map && constants;
    if (ready) {
        GeneratedResourceParameters values;
        values.scale = {1.0f, GreenScale, 1.0f, 1.0f};
        values.albedo.texture = albedoTexture.Value();
        values.codes.texture = codesTexture.Value();
        values.lights.view = lightView.Value();
        render::ParameterBatch batch(allocator, map.Value());
        const bool filled = batch.Initialize() && batch.SetParameters(values);
        auto validated = filled ? batch.Validate(device, &reason)
                                : Result<void>::Failure(ErrorCode::InvalidArgument);
        Check(filled && validated, "the parameter batch reads the struct and validates");
        if (!validated) {
            printf("    %s\n", reason);
        }

        const Span<const uint8> constantBytes(reinterpret_cast<const uint8*>(&values),
                                              table.totalBytes);
        const uint64 nullBefore = device.NullTextureBindings();
        // 1) 짝 샘플러 override 없음: albedo는 텍스처 desc 샘플러(Wrap)다.
        Check(validated && bench.Draw([&](CommandContext& commands) noexcept {
            commands.SetPipeline(pipeline.Value());
            Check(commands.SetConstantData(ShaderStage::Pixel, constants.Value(), constantBytes)
                      .IsOk(),
                  "the constant part is bound as the front C++ bytes");
            Check(batch.Apply(commands).IsOk(), "the parameter batch applies");
            commands.Draw(3);
        }) && bench.Shows(WrappedAlbedo),
              "struct members draw (texture desc sampler, integer texture, typed buffer)");
        if (!bench.Shows(WrappedAlbedo)) {
            PrintPixels(bench.pixels);
        }

        // 2) 짝 샘플러 override(Clamp)가 텍스처 desc 샘플러를 바꾼다.
        SamplerDesc clamp;
        clamp.filter = Filter::Point;
        clamp.addressU = AddressMode::Clamp;
        clamp.addressV = AddressMode::Clamp;
        values.albedo.sampler = render::SamplerOverride::Of(clamp);
        Check(batch.SetParameters(values) && batch.Validate(device),
              "the sampler override validates");
        Check(bench.Draw([&](CommandContext& commands) noexcept {
            commands.SetPipeline(pipeline.Value());
            (void)commands.SetConstantData(ShaderStage::Pixel, constants.Value(), constantBytes);
            Check(batch.Apply(commands).IsOk(), "the parameter batch applies again");
            commands.Draw(3);
        }) && bench.Shows(ClampedAlbedo),
              "the sampler override replaces the texture desc sampler");
        if (!bench.Shows(ClampedAlbedo)) {
            PrintPixels(bench.pixels);
        }

        Check(device.NullTextureBindings() == nullBefore,
              "the generated struct path binds no null texture");
    }

    if (lightView) {
        device.Retire(lightView.Value());
    }

    if (lightBuffer) {
        device.Retire(lightBuffer.Value());
    }

    if (albedoTexture) {
        device.Retire(albedoTexture.Value());
    }

    if (codesTexture) {
        device.Retire(codesTexture.Value());
    }

    if (pipeline) {
        device.Retire(pipeline.Value());
    }
}

} // namespace

uint32 RunShaderResourceChecks(const char* name, Device& device, Allocator& allocator) noexcept {
    failures = 0;
    backendName = name;
    if (!shaders::Has(shaders::GeneratedResourcesPS) ||
        !shaders::Has(shaders::GeneratedResourcesVS)) {
        printf("  [NOT RUN] %s shader resources: no shader set\n", name);
        return 0;
    }

    const uint64 validationBefore = device.ValidationErrorCount();
    Bench bench;
    bench.device = &device;
    TextureDesc targetDesc;
    targetDesc.width = TargetWidth;
    targetDesc.height = 1;
    targetDesc.format = PixelFormat::RGBA8Unorm;
    targetDesc.usage = TextureUsage::RenderTarget;
    auto target = device.CreateTexture(targetDesc, {});
    Check(target.IsOk(), "render target creation");
    if (!target) {
        return failures;
    }

    bench.target = target.Value();
    RunDraws(device, allocator, bench);
    device.Retire(bench.target);
    Check(device.WaitIdle().IsOk() && device.PendingRetireCount() == 0,
          "shader resource check resources are reclaimed");
    Check(device.ValidationErrorCount() == validationBefore, "no validation errors");
    printf("  %s shader resource members: %u failure%s\n", name, failures,
           failures == 1 ? "" : "s");
    return failures;
}

} // namespace sandbox
