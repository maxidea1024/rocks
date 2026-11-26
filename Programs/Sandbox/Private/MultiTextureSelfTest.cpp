#include "MultiTextureSelfTest.h"
#include "SandboxShaders.h"

#include <Graphics/ShaderBindings.h>
#include <RenderBase/ParameterBatch.h>
#include <RenderBase/ShaderResourceMap.h>
#include <stdio.h>

// 다중 텍스처 slot self-test다(Docs/Design/RenderBase 9.13절). 셰이더는 MultiTexture.rsf이고,
// slot은 장치 형식의 값 1에서 자원 지도가 이름으로 찾는다(형식마다 다르다 — GLES VS 유닛은 16부터,
// Vulkan binding은 종류를 이어 매긴다). 대상의 열 x가 자원 하나를 보여 준다. 텍스처는 점 필터이고
// 색은 UNORM8·정수라 정확하다(허용 오차 ±2).
namespace sandbox {

namespace {

using namespace rocks;
using namespace rocks::graphics;

// 대상 열 수다(MultiPS의 열 0–4).
constexpr uint32 TargetWidth = 5;
constexpr int ColorTolerance = 2;
// RGBA8 texel 하나의 byte 수다.
constexpr uint32 TexelBytes = 4;

uint32 failures = 0;
const char* backendName = "";

void Check(bool condition, const char* what) noexcept {
    if (!condition) {
        ++failures;
        printf("  [FAIL] %s multi-texture: %s\n", backendName, what);
    }
}

// MultiTexture.rsf의 자원 표다(손으로 쓴 표 — P7 전).
constexpr render::ResourceParameter MultiTextureResources[] = {
    {"Heights", render::ResourceParameterKind::Texture},
    {"Albedo", render::ResourceParameterKind::Texture},
    {"AlbedoSampler", render::ResourceParameterKind::Sampler},
    {"Layers", render::ResourceParameterKind::Texture},
    {"LayersSampler", render::ResourceParameterKind::Sampler},
    {"Detail", render::ResourceParameterKind::Texture},
    {"SharedClampSampler", render::ResourceParameterKind::Sampler},
    {"Codes", render::ResourceParameterKind::Texture}};

struct Rgba {
    int r = 0;
    int g = 0;
    int b = 0;
    int a = 0;
};

// 열마다 기대 색이다: Albedo(Wrap이면 빨강, Clamp면 초록), Layers 층 1 노랑, Detail 공유 Clamp
// 자홍, Codes 정수 (10, 20, 30, 255), VS Heights (64, 128, 192, 255).
constexpr Rgba WrappedAlbedo = {255, 0, 0, 255};
constexpr Rgba ClampedAlbedo = {0, 255, 0, 255};
constexpr Rgba LayerOne = {255, 255, 0, 255};
constexpr Rgba ClampedDetail = {255, 0, 255, 255};
constexpr Rgba CodeColor = {10, 20, 30, 255};
constexpr Rgba HeightColor = {64, 128, 192, 255};

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

Result<TextureHandle> MakeTexture(Device& device, TextureDimension dimension, uint32 width,
                                  uint32 layers, PixelFormat format, AddressMode address,
                                  Span<const uint8> texels) noexcept {
    TextureDesc desc;
    desc.dimension = dimension;
    desc.width = width;
    desc.height = 1;
    desc.arraySize = layers;
    desc.format = format;
    desc.sampler.filter = Filter::Point;
    desc.sampler.addressU = address;
    desc.sampler.addressV = address;
    return device.CreateTexture(desc, texels);
}

struct Bench {
    Device* device = nullptr;
    TextureHandle target;
    uint8 pixels[TargetWidth * TexelBytes] = {};

    // record로 그리고 대상을 읽는다(검은 투명 clear).
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
        return PixelIs(pixels, 0, albedo) && PixelIs(pixels, 1, LayerOne) &&
               PixelIs(pixels, 2, ClampedDetail) && PixelIs(pixels, 3, CodeColor) &&
               PixelIs(pixels, 4, HeightColor);
    }
};

void RunDraws(Device& device, Allocator& allocator, Bench& bench) noexcept {
    const uint8 heights[] = {64, 128, 192, 255};
    const uint8 albedo[] = {255, 0, 0, 255, 0, 255, 0, 255};
    const uint8 layers[] = {0, 0, 255, 255, 255, 255, 0, 255};
    const uint8 detail[] = {0, 255, 255, 255, 255, 0, 255, 255};
    const uint8 codes[] = {10, 20, 30, 255};
    if (!HasPixelFormatCaps(device.GetPixelFormatCaps(PixelFormat::RGBA8Uint),
                            PixelFormatCaps::Sampled)) {
        printf("  [NOT RUN] %s multi-texture: RGBA8Uint is not sampled on this device\n",
               backendName);
        return;
    }

    // Detail의 desc 샘플러는 Wrap이다 — 공유 샘플러(Clamp)가 쓰이면 오른쪽 texel(자홍)이다.
    auto heightsTexture = MakeTexture(device, TextureDimension::Texture2D, 1, 1,
                                      PixelFormat::RGBA8Unorm, AddressMode::Clamp, heights);
    auto albedoTexture = MakeTexture(device, TextureDimension::Texture2D, 2, 1,
                                     PixelFormat::RGBA8Unorm, AddressMode::Wrap, albedo);
    auto layersTexture = MakeTexture(device, TextureDimension::Texture2DArray, 1, 2,
                                     PixelFormat::RGBA8Unorm, AddressMode::Clamp, layers);
    auto detailTexture = MakeTexture(device, TextureDimension::Texture2D, 2, 1,
                                     PixelFormat::RGBA8Unorm, AddressMode::Wrap, detail);
    auto codesTexture = MakeTexture(device, TextureDimension::Texture2D, 1, 1,
                                    PixelFormat::RGBA8Uint, AddressMode::Clamp, codes);
    PipelineDesc desc;
    desc.colorFormats[0] = PixelFormat::RGBA8Unorm;
    auto first = shaders::CreateGraphicsPipeline(device, shaders::MultiTextureVS,
                                                 shaders::MultiTexturePS, desc);
    auto second = shaders::CreateGraphicsPipeline(device, shaders::MultiTextureVS,
                                                  shaders::MultiTexturePS, desc);
    Check(heightsTexture && albedoTexture && layersTexture && detailTexture && codesTexture,
          "textures are created");
    Check(first && second, "pipelines are created");
    if (!first || !second) {
        printf("    %.*s\n", static_cast<int>(device.LastError().ByteCount()),
               device.LastError().Data());
    }

    const shader::ShaderBindingLayout* stages[] = {shaders::Layout(shaders::MultiTextureVS),
                                                   shaders::Layout(shaders::MultiTexturePS)};
    const char* reason = "";
    auto map = render::ShaderResourceMap::Build(allocator, MultiTextureResources, stages,
                                                desc.bindings, &reason);
    Check(map.IsOk(), "the resource map matches the shader layouts");
    if (!map) {
        printf("    %s\n", reason);
    }

    const bool ready = heightsTexture && albedoTexture && layersTexture && detailTexture &&
                       codesTexture && first && second && map;
    if (ready) {
        const uint64 nullBefore = device.NullTextureBindings();
        const uint64 ignoredBefore = device.IgnoredDraws();
        render::ParameterBatch batch(allocator, map.Value());
        SamplerDesc clamp;
        clamp.filter = Filter::Point;
        clamp.addressU = AddressMode::Clamp;
        clamp.addressV = AddressMode::Clamp;
        const bool filled = batch.Initialize() &&
                            batch.SetTexture("Heights", heightsTexture.Value()) &&
                            batch.SetTexture("Albedo", albedoTexture.Value()) &&
                            batch.SetTexture("Layers", layersTexture.Value()) &&
                            batch.SetTexture("Detail", detailTexture.Value()) &&
                            batch.SetTexture("Codes", codesTexture.Value()) &&
                            batch.SetSampler("SharedClampSampler", clamp);
        auto validated = filled ? batch.Validate(device, &reason)
                                : Result<void>::Failure(ErrorCode::InvalidArgument);
        Check(filled && validated, "the parameter batch validates");
        if (!validated) {
            printf("    %s\n", reason);
        }

        // 1) 자원 묶음 하나로 다섯 텍스처를 묶는다. 자기 샘플러 값이 없으니 Albedo는 desc Wrap이다.
        Check(validated && bench.Draw([&](CommandContext& commands) noexcept {
            commands.SetPipeline(first.Value());
            Check(batch.Apply(commands).IsOk(), "the parameter batch applies");
            commands.Draw(3);
        }) && bench.Shows(WrappedAlbedo),
              "five textures in one pass match (texture desc sampler, shared clamp sampler)");
        if (!bench.Shows(WrappedAlbedo)) {
            PrintPixels(bench.pixels);
        }

        // 2) 자기 샘플러 값(Clamp)을 주고, 묶은 뒤 다른 파이프라인으로 바꿔도 묶음이 남는다.
        Check(batch.SetSampler("AlbedoSampler", clamp) && batch.Validate(device),
              "the sampler value validates");
        Check(bench.Draw([&](CommandContext& commands) noexcept {
            commands.SetPipeline(first.Value());
            Check(batch.Apply(commands).IsOk(), "the parameter batch applies again");
            commands.SetPipeline(second.Value());
            commands.Draw(3);
        }) && bench.Shows(ClampedAlbedo),
              "a sampler value replaces the texture desc sampler and bindings survive "
              "SetPipeline");
        if (!bench.Shows(ClampedAlbedo)) {
            PrintPixels(bench.pixels);
        }

        // 3) 공유 샘플러 값을 지우면 그리기를 무시한다(대상은 clear 그대로).
        auto shared =
            shaders::Layout(shaders::MultiTexturePS) != nullptr
                ? FindSamplerSlot(*shaders::Layout(shaders::MultiTexturePS), "SharedClampSampler")
                : Result<uint32>::Failure(ErrorCode::NotFound);
        Check(shared.IsOk(), "the shared sampler slot is found");
        const uint64 ignoredBeforeShared = device.IgnoredDraws();
        Check(shared && bench.Draw([&](CommandContext& commands) noexcept {
            commands.SetPipeline(first.Value());
            (void)batch.Apply(commands);
            Check(commands.SetSampler(ShaderStage::Pixel, shared.Value(), nullptr).IsOk(),
                  "a sampler value can be cleared");
            commands.Draw(3);
        }) && PixelIs(bench.pixels, 0, Rgba{}) &&
                  PixelIs(bench.pixels, 4, Rgba{}),
              "a draw without a shared sampler value leaves the target cleared");
        Check(device.IgnoredDraws() == ignoredBeforeShared + 1,
              "the draw without a shared sampler value is counted as ignored");
        Check(device.IgnoredDraws() == ignoredBefore + 1, "only that draw is ignored");
        Check(device.NullTextureBindings() == nullBefore,
              "the parameter batch path binds no null texture");
    }

    const Result<TextureHandle>* textures[] = {&heightsTexture, &albedoTexture, &layersTexture,
                                               &detailTexture, &codesTexture};
    for (const Result<TextureHandle>* texture : textures) {
        if (*texture) {
            device.Retire(texture->Value());
        }
    }

    if (first) {
        device.Retire(first.Value());
    }

    if (second) {
        device.Retire(second.Value());
    }
}

} // namespace

uint32 RunMultiTextureChecks(const char* name, Device& device, Allocator& allocator) noexcept {
    failures = 0;
    backendName = name;
    if (!shaders::Has(shaders::MultiTexturePS) || !shaders::Has(shaders::MultiTextureVS)) {
        printf("  [NOT RUN] %s multi-texture: no shader set\n", name);
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
          "multi-texture check resources are reclaimed");
    Check(device.ValidationErrorCount() == validationBefore, "no validation errors");
    printf("  %s multi-texture slots: %u failure%s\n", name, failures, failures == 1 ? "" : "s");
    return failures;
}

} // namespace sandbox
