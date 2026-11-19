#include "SliceAttachmentSelfTest.h"

#include <Core/Containers/Array.h>
#include <stdio.h>

namespace sandbox {

namespace {

using namespace rocks;
using namespace rocks::graphics;

constexpr uint32 PixelBytes = 4;
// 셰이더로 읽은 값과 기대값의 허용 차이(8-bit 단위)다.
constexpr int ColorTolerance = 2;

uint32 failures = 0;
const char* backendName = "";

void Check(bool condition, const char* what) noexcept {
    if (!condition) {
        ++failures;
        printf("  [FAIL] %s slice attachments: %s\n", backendName, what);
    }
}

void PrintLastError(Device& device) noexcept {
    printf("    %.*s\n", static_cast<int>(device.LastError().ByteCount()),
           device.LastError().Data());
}

template <usize Count>
StringView Text(const char (&text)[Count]) noexcept {
    return StringView(text, Count - 1);
}

bool Near(uint8 actual, int expected) noexcept {
    const int delta = static_cast<int>(actual) - expected;
    return delta <= ColorTolerance && delta >= -ColorTolerance;
}

struct Rgba {
    uint8 r = 0;
    uint8 g = 0;
    uint8 b = 0;
    uint8 a = 0;
};

// 그리기별 상수 하나(float4)다. x는 읽기 view의 기준 mip이다.
struct LevelConstants {
    float values[4] = {};
};

#define SLICE_GLSL_HEAD "#version 320 es\nprecision highp float;\nprecision highp int;\n"
#define SLICE_FULL_SCREEN_HLSL                                                                     \
    "float4 VSMain(uint id : SV_VertexID) : SV_Position {\n"                                       \
    "    float2 uv = float2((id << 1) & 2, id & 2);\n"                                             \
    "    return float4(uv * 2.0 - 1.0, 0.5, 1.0);\n"                                               \
    "}\n"

constexpr char FullScreenGlsl[] = SLICE_GLSL_HEAD R"(
void main() {
    vec2 uv = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(uv * 2.0 - 1.0, 0.5, 1.0);
}
)";

// 흰색으로 채운다(깊이 0.5).
constexpr char WhiteHlsl[] =
    SLICE_FULL_SCREEN_HLSL "float4 PSMain() : SV_Target { return float4(1.0, 1.0, 1.0, 1.0); }\n";

constexpr char WhitePixelGlsl[] = SLICE_GLSL_HEAD R"(
layout(location = 0) out vec4 out_var_SV_Target0;
void main() { out_var_SV_Target0 = vec4(1.0); }
)";

// 읽기 view의 기준 mip texel (0, 0)의 절반을 낸다.
constexpr char HalfHlsl[] = "cbuffer DrawConstants : register(b0) { float4 level; };\n"
                            "Texture2D<float4> source : register(t0);\n" SLICE_FULL_SCREEN_HLSL
                            "float4 PSMain() : SV_Target {\n"
                            "    return source.Load(int3(0, 0, (int)level.x)) * 0.5;\n"
                            "}\n";

constexpr char HalfPixelGlsl[] = SLICE_GLSL_HEAD R"(
uniform highp vec4 pu_h[1];
layout(binding = 0) uniform highp sampler2D ps0;
layout(location = 0) out vec4 out_var_SV_Target0;
void main() { out_var_SV_Target0 = texelFetch(ps0, ivec2(0, 0), int(pu_h[0].x)) * 0.5; }
)";

// MSAA 2D 배열 층 1의 표본 0을 낸다(텍스처 GX2 나머지 C부).
constexpr char MsaaLayerHlsl[] =
    "Texture2DMSArray<float4> source : register(t0);\n" SLICE_FULL_SCREEN_HLSL
    "float4 PSMain() : SV_Target { return source.Load(int3(0, 0, 1), 0); }\n";

constexpr char MsaaLayerPixelGlsl[] = SLICE_GLSL_HEAD R"(
layout(binding = 0) uniform highp sampler2DMSArray ps0;
layout(location = 0) out vec4 out_var_SV_Target0;
void main() { out_var_SV_Target0 = texelFetch(ps0, ivec3(0, 0, 1), 0); }
)";

#undef SLICE_GLSL_HEAD
#undef SLICE_FULL_SCREEN_HLSL

Result<PipelineHandle> MakePipeline(Device& device, StringView hlsl, StringView pixelGlsl,
                                    uint32 constants, PixelFormat depth) noexcept {
    PipelineDesc desc;
    desc.vertex = {hlsl, Text(FullScreenGlsl)};
    desc.pixel = {hlsl, pixelGlsl};
    desc.colorFormats[0] = PixelFormat::RGBA8Unorm;
    desc.drawConstantBytes = constants;
    desc.depthFormat = depth;
    if (depth != PixelFormat::Unknown) {
        // reverse-Z 기본 비교 GreaterEqual이다. 깊이 0.5는 0으로 clear한 층에서만 통과한다.
        desc.depthStencil.depthTest = true;
    }

    return device.CreatePipeline(desc);
}

TextureDesc ColorDesc(TextureDimension dimension, uint32 extent, uint32 layers, uint32 depth,
                      uint32 mipLevels) noexcept {
    TextureDesc desc;
    desc.dimension = dimension;
    desc.width = extent;
    desc.height = extent;
    desc.arraySize = layers;
    desc.depth = depth;
    desc.mipLevels = mipLevels;
    desc.format = PixelFormat::RGBA8Unorm;
    desc.usage = TextureUsage::RenderTarget;
    desc.sampler.filter = Filter::Point;
    return desc;
}

// 색 attachment (mip, slice) 하나를 clear하고, white가 있으면 흰색을 그린다.
bool ClearSlice(Device& device, TextureHandle texture, uint32 mip, uint32 slice,
                const ClearColor& clear, PipelineHandle white) noexcept {
    PassDesc pass;
    pass.colorCount = 1;
    pass.colors[0].texture = texture;
    pass.colors[0].clear = clear;
    pass.colors[0].mipLevel = mip;
    pass.colors[0].slice = slice;
    auto frame = device.BeginPass(pass);
    if (!frame) {
        PrintLastError(device);
        return false;
    }

    if (!white.IsNull()) {
        frame.Value()->SetPipeline(white);
        frame.Value()->Draw(3);
    }

    return device.EndFrame().IsOk();
}

// mip의 slice(또는 3D 깊이 면) 하나의 첫 픽셀이 expected인지다.
bool SliceIs(Device& device, Allocator& allocator, TextureHandle texture, const TextureDesc& desc,
             uint32 mip, uint32 slice, Rgba expected) noexcept {
    Array<uint8> bytes(allocator);
    const usize mipBytes = static_cast<usize>(TextureMipBytes(desc, mip));
    const usize image =
        static_cast<usize>(MipExtent(desc.width, mip)) * MipExtent(desc.height, mip) * PixelBytes;
    if (!bytes.TryResize(mipBytes) || !device.ReadTexture(texture, mip, 0, bytes.View())) {
        PrintLastError(device);
        return false;
    }

    const uint8* pixel = bytes.Data() + image * slice;
    const bool same = Near(pixel[0], expected.r) && Near(pixel[1], expected.g) &&
                      Near(pixel[2], expected.b) && Near(pixel[3], expected.a);
    if (!same) {
        printf("    mip %u slice %u = (%u, %u, %u, %u), expected (%u, %u, %u, %u)\n", mip, slice,
               pixel[0], pixel[1], pixel[2], pixel[3], expected.r, expected.g, expected.b,
               expected.a);
    }

    return same;
}

// mip 0을 빨강으로 clear하고, mip 0 view를 읽으며 mip 1에 절반 값을 그린다.
void RunMipChain(Device& device, Allocator& allocator, PipelineHandle half) noexcept {
    const TextureDesc desc = ColorDesc(TextureDimension::Texture2D, 4, 1, 1, 3);
    auto chain = device.CreateTexture(desc, {});
    TextureViewDesc mip0;
    mip0.mipCount = 1;
    auto view = chain ? device.CreateTextureView(chain.Value(), mip0)
                      : Result<TextureHandle>::Failure(ErrorCode::InvalidArgument);
    Check(chain && view, "mip chain render target and its mip 0 view");
    if (!chain || !view) {
        PrintLastError(device);
        return;
    }

    bool drawn = ClearSlice(device, chain.Value(), 0, 0, {1.0f, 0.0f, 0.0f, 1.0f}, {});
    PassDesc pass;
    pass.colorCount = 1;
    pass.colors[0].texture = chain.Value();
    pass.colors[0].mipLevel = 1;
    auto frame = device.BeginPass(pass);
    if (frame) {
        LevelConstants level;
        level.values[0] = static_cast<float>(TextureViewBaseMip(device.Caps().features, mip0));
        frame.Value()->SetPipeline(half);
        frame.Value()->SetTexture(view.Value());
        drawn = drawn && frame.Value()->SetDrawConstants(DrawConstantBytes(level)).IsOk();
        frame.Value()->Draw(3);
    }

    drawn = drawn && frame && device.EndFrame().IsOk();
    Check(drawn && SliceIs(device, allocator, chain.Value(), desc, 0, 0, {255, 0, 0, 255}) &&
              SliceIs(device, allocator, chain.Value(), desc, 1, 0, {128, 0, 0, 128}),
          "rendering mip 1 while sampling mip 0 of the same texture");
    // 범위 밖 mip과 mip 크기가 다른 attachment는 거부한다.
    PassDesc outside = pass;
    outside.colors[0].mipLevel = 3;
    Check(device.BeginPass(outside).Error() == ErrorCode::InvalidArgument,
          "an attachment mip outside the texture is rejected");
    PassDesc mixed;
    mixed.colorCount = 2;
    mixed.colors[0].texture = chain.Value();
    mixed.colors[1].texture = chain.Value();
    mixed.colors[1].mipLevel = 1;
    Check(device.BeginPass(mixed).Error() == ErrorCode::InvalidArgument,
          "attachments of different mip sizes are rejected");
    device.Retire(view.Value());
    device.Retire(chain.Value());
}

// 2D 배열 층·cube 면·3D 깊이 면마다 다른 값을 쓴다.
void RunSlices(Device& device, Allocator& allocator, PipelineHandle white) noexcept {
    const TextureDesc arrayDesc = ColorDesc(TextureDimension::Texture2DArray, 2, 3, 1, 1);
    auto layers = device.CreateTexture(arrayDesc, {});
    Check(layers.IsOk(), "2D array render target");
    if (layers) {
        const bool cleared =
            ClearSlice(device, layers.Value(), 0, 0, {1.0f, 0.0f, 0.0f, 1.0f}, {}) &&
            ClearSlice(device, layers.Value(), 0, 1, {0.0f, 1.0f, 0.0f, 1.0f}, {}) &&
            ClearSlice(device, layers.Value(), 0, 2, {0.0f, 0.0f, 1.0f, 1.0f}, white);
        Check(cleared &&
                  SliceIs(device, allocator, layers.Value(), arrayDesc, 0, 0, {255, 0, 0, 255}) &&
                  SliceIs(device, allocator, layers.Value(), arrayDesc, 0, 1, {0, 255, 0, 255}) &&
                  SliceIs(device, allocator, layers.Value(), arrayDesc, 0, 2, {255, 255, 255, 255}),
              "2D array layer attachments clear and draw only their layer");
        PassDesc outside;
        outside.colorCount = 1;
        outside.colors[0].texture = layers.Value();
        outside.colors[0].slice = 3;
        Check(device.BeginPass(outside).Error() == ErrorCode::InvalidArgument,
              "an attachment slice outside the texture is rejected");
        device.Retire(layers.Value());
    }

    const TextureDesc cubeDesc = ColorDesc(TextureDimension::TextureCube, 2, 1, 1, 1);
    auto cube = device.CreateTexture(cubeDesc, {});
    Check(cube.IsOk(), "cube render target");
    if (cube) {
        bool cleared = true;
        for (uint32 face = 0; face < CubeFaceCount; ++face) {
            const float green = face == 3 ? 1.0f : 0.0f;
            cleared =
                cleared && ClearSlice(device, cube.Value(), 0, face, {0.0f, green, 0.0f, 1.0f}, {});
        }

        Check(cleared &&
                  SliceIs(device, allocator, cube.Value(), cubeDesc, 0, 3, {0, 255, 0, 255}) &&
                  SliceIs(device, allocator, cube.Value(), cubeDesc, 0, 2, {0, 0, 0, 255}),
              "cube face attachments write only their face");
        device.Retire(cube.Value());
    }

    const TextureDesc volumeDesc = ColorDesc(TextureDimension::Texture3D, 2, 1, 4, 1);
    auto volume = device.CreateTexture(volumeDesc, {});
    const bool volumeCapable = HasPixelFormatCaps(device.GetPixelFormatCaps(volumeDesc.format),
                                                  RequiredPixelFormatCaps(volumeDesc));
    Check(volumeCapable ? volume.IsOk() : volume.Error() == ErrorCode::Unsupported,
          "3D render target follows the Texture3D and RenderTarget caps");
    if (volume) {
        bool cleared = true;
        for (uint32 z = 0; z < volumeDesc.depth; ++z) {
            const float value = static_cast<float>(z) * 0.25f;
            cleared =
                cleared && ClearSlice(device, volume.Value(), 0, z, {value, 0.0f, 0.0f, 1.0f}, {});
        }

        Check(cleared &&
                  SliceIs(device, allocator, volume.Value(), volumeDesc, 0, 1, {64, 0, 0, 255}) &&
                  SliceIs(device, allocator, volume.Value(), volumeDesc, 0, 3, {191, 0, 0, 255}),
              "3D depth slice attachments write only their slice");
        device.Retire(volume.Value());
    }
}

// 깊이 배열 층마다 다른 깊이로 clear하고 깊이 시험으로 고른 층을 확인한다.
// 깊이 텍스처의 near 층(1로 clear)과 far 층(0으로 clear)이 따로인지 깊이 시험으로 본다. 다중 plane
// 깊이(D24S8)의 2D 배열·cube도 같다(텍스처 GX2 나머지 C부).
void RunDepthLayers(Device& device, PipelineHandle depthWhite, PixelFormat format,
                    TextureDimension dimension, uint32 nearLayer, uint32 farLayer,
                    const char* what) noexcept {
    TextureDesc depthDesc;
    depthDesc.dimension = dimension;
    depthDesc.width = 2;
    depthDesc.height = 2;
    depthDesc.arraySize = dimension == TextureDimension::Texture2DArray ? 2u : 1u;
    depthDesc.format = format;
    depthDesc.usage = TextureUsage::DepthStencil;
    if (!HasPixelFormatCaps(device.GetPixelFormatCaps(format),
                            RequiredPixelFormatCaps(depthDesc))) {
        return;
    }

    auto depth = device.CreateTexture(depthDesc, {});
    auto color = device.CreateTexture(ColorDesc(TextureDimension::Texture2D, 2, 1, 1, 1), {});
    Check(depth && color, "depth array and color target");
    if (!depth || !color) {
        PrintLastError(device);
        return;
    }

    // near 층은 1(가까움), far 층은 0(먼 평면)으로 clear한다.
    const auto clearLayer = [&](uint32 layer, float value) noexcept {
        PassDesc pass;
        pass.depth.texture = depth.Value();
        pass.depth.slice = layer;
        pass.depth.clearDepth = value;
        auto frame = device.BeginPass(pass);
        return frame.IsOk() && device.EndFrame().IsOk();
    };
    // 그 층을 Load로 붙여 깊이 0.5를 GreaterEqual로 그린다.
    const auto drawOver = [&](uint32 layer) noexcept {
        PassDesc pass;
        pass.colorCount = 1;
        pass.colors[0].texture = color.Value();
        pass.depth.texture = depth.Value();
        pass.depth.slice = layer;
        pass.depth.depthLoad = LoadAction::Load;
        auto frame = device.BeginPass(pass);
        if (!frame) {
            return false;
        }

        frame.Value()->SetPipeline(depthWhite);
        frame.Value()->Draw(3);
        uint8 pixels[2 * 2 * PixelBytes] = {};
        return device.EndFrame().IsOk() &&
               device.ReadPixels(color.Value(), Span<uint8>(pixels, sizeof(pixels))).IsOk() &&
               pixels[0] == 255;
    };
    const bool cleared = clearLayer(nearLayer, 1.0f) && clearLayer(farLayer, 0.0f);
    Check(cleared && !drawOver(nearLayer) && drawOver(farLayer), what);
    device.Retire(depth.Value());
    device.Retire(color.Value());
}

// 4x MSAA를 2D 배열 층 1에 resolve한다.
void RunResolveSlice(Device& device, Allocator& allocator) noexcept {
    TextureDesc msaaDesc = ColorDesc(TextureDimension::Texture2D, 2, 1, 1, 1);
    msaaDesc.sampleCount = 4;
    if ((device.GetPixelFormatSampleCounts(msaaDesc.format) & SampleCountBit(4)) == 0) {
        return;
    }

    const TextureDesc arrayDesc = ColorDesc(TextureDimension::Texture2DArray, 2, 2, 1, 1);
    auto msaa = device.CreateTexture(msaaDesc, {});
    auto layers = device.CreateTexture(arrayDesc, {});
    Check(msaa && layers, "MSAA source and array resolve target");
    if (msaa && layers) {
        const bool cleared = ClearSlice(device, layers.Value(), 0, 0, {1.0f, 0.0f, 0.0f, 1.0f}, {});
        PassDesc pass;
        pass.colorCount = 1;
        pass.colors[0].texture = msaa.Value();
        pass.colors[0].clear = {0.0f, 1.0f, 0.0f, 1.0f};
        pass.colors[0].store = StoreAction::DontCare;
        pass.colors[0].resolveTarget = layers.Value();
        pass.colors[0].resolveSlice = 1;
        auto frame = device.BeginPass(pass);
        Check(cleared && frame && device.EndFrame() &&
                  SliceIs(device, allocator, layers.Value(), arrayDesc, 0, 1, {0, 255, 0, 255}) &&
                  SliceIs(device, allocator, layers.Value(), arrayDesc, 0, 0, {255, 0, 0, 255}),
              "MSAA resolves into the chosen array layer only");
        PassDesc outside = pass;
        outside.colors[0].resolveSlice = 2;
        Check(device.BeginPass(outside).Error() == ErrorCode::InvalidArgument,
              "a resolve slice outside the target is rejected");
    }

    if (msaa) {
        device.Retire(msaa.Value());
    }

    if (layers) {
        device.Retire(layers.Value());
    }
}

// 4x MSAA 2D 배열: 층 0은 빨강, 층 1은 초록으로 clear하고 층 1을 1표본 배열 층 1에 resolve한다
// (층 0 그대로). PS가 Texture2DMSArray 층 1의 표본 0을 읽는다(텍스처 GX2 나머지 C부).
void RunMsaaArray(Device& device, Allocator& allocator) noexcept {
    TextureDesc msaaDesc = ColorDesc(TextureDimension::Texture2DArray, 2, 2, 1, 1);
    msaaDesc.sampleCount = 4;
    if ((device.GetPixelFormatSampleCounts(msaaDesc.format) & SampleCountBit(4)) == 0) {
        return;
    }

    const TextureDesc arrayDesc = ColorDesc(TextureDimension::Texture2DArray, 2, 2, 1, 1);
    auto msaa = device.CreateTexture(msaaDesc, {});
    auto layers = device.CreateTexture(arrayDesc, {});
    auto color = device.CreateTexture(ColorDesc(TextureDimension::Texture2D, 2, 1, 1, 1), {});
    auto read = MakePipeline(device, Text(MsaaLayerHlsl), Text(MsaaLayerPixelGlsl), 0,
                             PixelFormat::Unknown);
    Check(msaa && layers && color && read, "MSAA 2D array, resolve target and reader");
    if (msaa && layers && color && read) {
        const bool cleared =
            ClearSlice(device, layers.Value(), 0, 0, {0.0f, 0.0f, 1.0f, 1.0f}, {}) &&
            ClearSlice(device, layers.Value(), 0, 1, {0.0f, 0.0f, 1.0f, 1.0f}, {}) &&
            ClearSlice(device, msaa.Value(), 0, 0, {1.0f, 0.0f, 0.0f, 1.0f}, {});
        PassDesc pass;
        pass.colorCount = 1;
        pass.colors[0].texture = msaa.Value();
        pass.colors[0].slice = 1;
        pass.colors[0].clear = {0.0f, 1.0f, 0.0f, 1.0f};
        pass.colors[0].resolveTarget = layers.Value();
        pass.colors[0].resolveSlice = 1;
        auto frame = device.BeginPass(pass);
        Check(cleared && frame && device.EndFrame() &&
                  SliceIs(device, allocator, layers.Value(), arrayDesc, 0, 1, {0, 255, 0, 255}) &&
                  SliceIs(device, allocator, layers.Value(), arrayDesc, 0, 0, {0, 0, 255, 255}),
              "MSAA array layer 1 resolves into array layer 1 only");
        PassDesc draw;
        draw.colorCount = 1;
        draw.colors[0].texture = color.Value();
        auto drawn = device.BeginPass(draw);
        if (drawn) {
            drawn.Value()->SetPipeline(read.Value());
            drawn.Value()->SetTexture(msaa.Value());
            drawn.Value()->Draw(3);
        }

        uint8 pixels[2 * 2 * PixelBytes] = {};
        Check(drawn && device.EndFrame() &&
                  device.ReadPixels(color.Value(), Span<uint8>(pixels, sizeof(pixels))).IsOk() &&
                  pixels[0] == 0 && pixels[1] == 255 && pixels[2] == 0,
              "a pixel shader reads sample 0 of MSAA array layer 1");
    } else {
        PrintLastError(device);
    }

    if (read) {
        device.Retire(read.Value());
    }

    if (msaa) {
        device.Retire(msaa.Value());
    }

    if (layers) {
        device.Retire(layers.Value());
    }

    if (color) {
        device.Retire(color.Value());
    }

    // MSAA cube는 없다.
    TextureDesc msaaCube = ColorDesc(TextureDimension::TextureCube, 2, 1, 1, 1);
    msaaCube.sampleCount = 4;
    Check(device.CreateTexture(msaaCube, {}).Error() == ErrorCode::InvalidArgument,
          "an MSAA cube is rejected");
}

} // namespace

uint32 RunSliceAttachmentChecks(const char* name, Device& device, Allocator& allocator) noexcept {
    failures = 0;
    backendName = name;
    const uint64 validationBefore = device.ValidationErrorCount();
    auto white =
        MakePipeline(device, Text(WhiteHlsl), Text(WhitePixelGlsl), 0, PixelFormat::Unknown);
    auto half = MakePipeline(device, Text(HalfHlsl), Text(HalfPixelGlsl), sizeof(LevelConstants),
                             PixelFormat::Unknown);
    auto depthWhite =
        MakePipeline(device, Text(WhiteHlsl), Text(WhitePixelGlsl), 0, PixelFormat::D32Float);
    // 다중 plane 깊이(D24S8) 파이프라인은 그 형식이 깊이 attachment일 때만 만든다.
    const bool stencilDepth = HasPixelFormatCaps(
        device.GetPixelFormatCaps(PixelFormat::D24UnormS8Uint), PixelFormatCaps::DepthStencil);
    auto stencilWhite = stencilDepth ? MakePipeline(device, Text(WhiteHlsl), Text(WhitePixelGlsl),
                                                    0, PixelFormat::D24UnormS8Uint)
                                     : Result<PipelineHandle>::Failure(ErrorCode::Unsupported);
    Check(white && half && depthWhite && (!stencilDepth || stencilWhite),
          "slice attachment pipelines");
    if (white && half && depthWhite) {
        RunMipChain(device, allocator, half.Value());
        RunSlices(device, allocator, white.Value());
        RunDepthLayers(device, depthWhite.Value(), PixelFormat::D32Float,
                       TextureDimension::Texture2DArray, 1, 0,
                       "depth array layer attachments keep separate depth per layer");
        if (stencilWhite) {
            RunDepthLayers(device, stencilWhite.Value(), PixelFormat::D24UnormS8Uint,
                           TextureDimension::Texture2DArray, 1, 0,
                           "D24S8 array layer attachments keep separate depth per layer");
            RunDepthLayers(device, stencilWhite.Value(), PixelFormat::D24UnormS8Uint,
                           TextureDimension::TextureCube, 3, 0,
                           "D24S8 cube face attachments keep separate depth per face");
        }

        RunResolveSlice(device, allocator);
        RunMsaaArray(device, allocator);
    } else {
        PrintLastError(device);
    }

    // 3D 깊이 attachment는 없다.
    TextureDesc volumeDepth;
    volumeDepth.dimension = TextureDimension::Texture3D;
    volumeDepth.width = 2;
    volumeDepth.height = 2;
    volumeDepth.depth = 2;
    volumeDepth.format = PixelFormat::D32Float;
    volumeDepth.usage = TextureUsage::DepthStencil;
    // 장치는 형식 능력(3D 깊이 형식은 Texture3D가 없다)을 규칙보다 먼저 볼 수 있다.
    const ErrorCode volumeDepthError = device.CreateTexture(volumeDepth, {}).Error();
    Check(volumeDepthError == ErrorCode::InvalidArgument ||
              volumeDepthError == ErrorCode::Unsupported,
          "a 3D depth stencil texture is rejected");
    Result<PipelineHandle>* pipelines[] = {&white, &half, &depthWhite, &stencilWhite};
    for (Result<PipelineHandle>* pipeline : pipelines) {
        if (*pipeline) {
            device.Retire(pipeline->Value());
        }
    }

    auto idle = device.WaitIdle();
    Check(idle.IsOk(), "wait idle");
    Check(device.ValidationErrorCount() == validationBefore, "no validation errors");
    printf("  %s slice attachments: D24S8 array and cube %s, MSAA array %s, %u failures\n", name,
           stencilWhite ? "yes" : "no",
           (device.GetPixelFormatSampleCounts(PixelFormat::RGBA8Unorm) & SampleCountBit(4)) != 0
               ? "yes"
               : "no",
           failures);
    return failures;
}

} // namespace sandbox
