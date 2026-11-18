#include "TextureViewSelfTest.h"

#include <Core/Containers/Array.h>
#include <Platform/Memory.h>
#include <stdio.h>

namespace sandbox {

namespace {

using namespace rocks;
using namespace rocks::graphics;

// HZB 시험 텍스처의 한 변과 mip 수다(8, 4, 2, 1).
constexpr uint32 PyramidExtent = 8;
constexpr uint32 PyramidMips = 4;
// 줄이기 compute의 그룹 한 변이다(셰이더 numthreads와 같다).
constexpr uint32 ReduceGroup = 4;
// PS가 읽은 값을 색으로 낼 때 나누는 수다(mip 0의 최댓값 63 + 1).
constexpr float ValueScale = 64.0f;
// 셰이더로 읽은 값과 기대값의 허용 차이(8-bit 단위)다.
constexpr int ColorTolerance = 2;

uint32 failures = 0;
const char* backendName = "";

void Check(bool condition, const char* what) noexcept {
    if (!condition) {
        ++failures;
        printf("  [FAIL] %s texture views: %s\n", backendName, what);
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

// 그리기별 상수 하나(float4)다. x는 view 기준 mip, y는 출력 한 변이다.
struct LevelConstants {
    float values[4] = {};
};

#define TEXTURE_VIEW_GLSL_HEAD "#version 320 es\nprecision highp float;\nprecision highp int;\n"

// mip 0을 x + y × 8로 채운다.
constexpr char FillHlsl[] = R"(
RWTexture2D<float> target : register(u0);
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) { target[id.xy] = id.x + id.y * 8.0; }
)";

constexpr char FillGlsl[] = TEXTURE_VIEW_GLSL_HEAD R"(
layout(local_size_x = 8, local_size_y = 8) in;
layout(binding = 0, r32f) uniform highp writeonly image2D target;
void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    imageStore(target, p, vec4(float(p.x + p.y * 8)));
}
)";

// 읽기 view(t0)의 기준 mip에서 2×2 최댓값을 쓰기 view(u0)에 쓴다.
constexpr char ReduceHlsl[] = R"(
cbuffer DrawConstants : register(b0) { float4 level; };
Texture2D<float> source : register(t0);
RWTexture2D<float> target : register(u0);
[numthreads(4, 4, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= (uint)level.y || id.y >= (uint)level.y) { return; }
    int lod = (int)level.x;
    int2 p = int2(id.xy) * 2;
    float a = max(source.Load(int3(p, lod)), source.Load(int3(p + int2(1, 0), lod)));
    float b = max(source.Load(int3(p + int2(0, 1), lod)), source.Load(int3(p + int2(1, 1), lod)));
    target[id.xy] = max(a, b);
}
)";

constexpr char ReduceGlsl[] = TEXTURE_VIEW_GLSL_HEAD R"(
layout(local_size_x = 4, local_size_y = 4) in;
uniform highp vec4 cu_h[1];
layout(binding = 0) uniform highp sampler2D source;
layout(binding = 0, r32f) uniform highp writeonly image2D target;
void main() {
    ivec2 id = ivec2(gl_GlobalInvocationID.xy);
    int size = int(cu_h[0].y);
    if (id.x >= size || id.y >= size) { return; }
    int lod = int(cu_h[0].x);
    ivec2 p = id * 2;
    float a = max(texelFetch(source, p, lod).r, texelFetch(source, p + ivec2(1, 0), lod).r);
    float b = max(texelFetch(source, p + ivec2(0, 1), lod).r, texelFetch(source, p + ivec2(1, 1), lod).r);
    imageStore(target, id, vec4(max(a, b)));
}
)";

#define TEXTURE_VIEW_FULL_SCREEN_HLSL                                                              \
    "float4 VSMain(uint id : SV_VertexID) : SV_Position {\n"                                       \
    "    float2 uv = float2((id << 1) & 2, id & 2);\n"                                             \
    "    return float4(uv * 2.0 - 1.0, 0.5, 1.0);\n"                                               \
    "}\n"

constexpr char FullScreenGlsl[] = TEXTURE_VIEW_GLSL_HEAD R"(
void main() {
    vec2 uv = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(uv * 2.0 - 1.0, 0.5, 1.0);
}
)";

// 읽기 view의 기준 mip texel (0, 0)을 64로 나눠 빨강으로 낸다.
constexpr char LevelHlsl[] =
    "cbuffer DrawConstants : register(b0) { float4 level; };\n"
    "Texture2D<float> source : register(t0);\n" TEXTURE_VIEW_FULL_SCREEN_HLSL
    "float4 PSMain() : SV_Target {\n"
    "    float v = source.Load(int3(0, 0, (int)level.x));\n"
    "    return float4(v / 64.0, 0.0, 0.0, 1.0);\n"
    "}\n";

constexpr char LevelGlsl[] = TEXTURE_VIEW_GLSL_HEAD R"(
uniform highp vec4 pu_h[1];
layout(binding = 0) uniform highp sampler2D ps0;
layout(location = 0) out vec4 out_var_SV_Target0;
void main() {
    float v = texelFetch(ps0, ivec2(0, 0), int(pu_h[0].x)).r;
    out_var_SV_Target0 = vec4(v / 64.0, 0.0, 0.0, 1.0);
}
)";

// 2D 배열 view의 층 0 texel (0, 0)을 낸다.
constexpr char LayerHlsl[] =
    "Texture2DArray<float4> source : register(t0);\n" TEXTURE_VIEW_FULL_SCREEN_HLSL
    "float4 PSMain() : SV_Target { return source.Load(int4(0, 0, 0, 0)); }\n";

constexpr char LayerGlsl[] = TEXTURE_VIEW_GLSL_HEAD R"(
layout(binding = 0) uniform highp sampler2DArray ps0;
layout(location = 0) out vec4 out_var_SV_Target0;
void main() { out_var_SV_Target0 = texelFetch(ps0, ivec3(0, 0, 0), 0); }
)";

#undef TEXTURE_VIEW_GLSL_HEAD
#undef TEXTURE_VIEW_FULL_SCREEN_HLSL

// mip level의 CPU 기댓값이다: mip 0은 x + y × 8, 위 mip은 아래 mip 2×2의 최댓값(= 오른쪽 아래
// texel).
float Expected(uint32 level, uint32 x, uint32 y) noexcept {
    const uint32 span = 1u << level;
    const uint32 right = x * span + span - 1;
    const uint32 bottom = y * span + span - 1;
    return static_cast<float>(right + bottom * PyramidExtent);
}

// 1×1 RGBA8 렌더 타깃에 full-screen 삼각형 하나를 그리고 그 픽셀을 읽는다.
struct Canvas {
    Device& device;
    TextureHandle target;
    uint8 pixel[4] = {};

    explicit Canvas(Device& owner) noexcept : device(owner) {
    }

    bool Initialize() noexcept {
        TextureDesc desc;
        desc.width = 1;
        desc.height = 1;
        desc.format = PixelFormat::RGBA8Unorm;
        desc.usage = TextureUsage::RenderTarget;
        auto created = device.CreateTexture(desc, {});
        if (created) {
            target = created.Value();
        }

        return created.IsOk();
    }

    Result<PipelineHandle> Pipeline(StringView hlsl, StringView glsl, uint32 constants) noexcept {
        PipelineDesc desc;
        desc.vertex = {hlsl, Text(FullScreenGlsl)};
        desc.pixel = {hlsl, glsl};
        desc.colorFormats[0] = PixelFormat::RGBA8Unorm;
        desc.drawConstantBytes = constants;
        return device.CreatePipeline(desc);
    }

    // texture를 묶고 그려 pixel에 읽는다. constants가 있으면 그리기별 상수로 올린다.
    bool Draw(PipelineHandle pipeline, TextureHandle texture,
              const LevelConstants* constants) noexcept {
        auto frame = device.BeginOffscreen(target, {});
        if (!frame) {
            return false;
        }

        CommandContext& commands = *frame.Value();
        commands.SetPipeline(pipeline);
        commands.SetTexture(texture);
        bool set = true;
        if (constants != nullptr) {
            set = commands.SetDrawConstants(DrawConstantBytes(*constants)).IsOk();
        }

        commands.Draw(3);
        return device.EndFrame().IsOk() && set &&
               device.ReadPixels(target, Span<uint8>(pixel, sizeof(pixel))).IsOk();
    }

    void Release() noexcept {
        if (!target.IsNull()) {
            device.Retire(target);
        }
    }
};

TextureViewDesc MipView(uint32 mip, ViewAccess access) noexcept {
    TextureViewDesc desc;
    desc.firstMip = mip;
    desc.mipCount = 1;
    desc.access = access;
    return desc;
}

// mip 0을 채우고 mip k → k + 1로 줄인 뒤 모든 mip을 읽어 대조한다. pyramid는 만든 Storage
// 텍스처다.
void RunPyramid(Device& device, Allocator& allocator, TextureHandle pyramid) noexcept {
    ComputePipelineDesc fillDesc;
    fillDesc.shader = {Text(FillHlsl), Text(FillGlsl)};
    ComputePipelineDesc reduceDesc;
    reduceDesc.shader = {Text(ReduceHlsl), Text(ReduceGlsl)};
    reduceDesc.drawConstantBytes = sizeof(LevelConstants);
    auto fill = device.CreateComputePipeline(fillDesc);
    auto reduce = device.CreateComputePipeline(reduceDesc);
    Check(fill && reduce, "fill and reduce compute pipelines");
    if (!fill || !reduce) {
        PrintLastError(device);
        return;
    }

    TextureHandle reads[PyramidMips] = {};
    TextureHandle writes[PyramidMips] = {};
    bool views = true;
    for (uint32 mip = 0; mip < PyramidMips; ++mip) {
        auto read = device.CreateTextureView(pyramid, MipView(mip, ViewAccess::Read));
        auto write = device.CreateTextureView(pyramid, MipView(mip, ViewAccess::ReadWrite));
        views = views && read && write;
        if (read) {
            reads[mip] = read.Value();
        }

        if (write) {
            writes[mip] = write.Value();
        }
    }

    Check(views, "read and write views of every mip");
    if (!views) {
        PrintLastError(device);
    }

    const DeviceFeatures& features = device.Caps().features;
    const uint64 ignoredBefore = device.IgnoredDraws();
    auto frame = device.BeginPass(PassDesc{});
    if (frame && views) {
        CommandContext& commands = *frame.Value();
        // 쓰기 텍스처를 묶지 않은 dispatch는 무시된다.
        commands.SetPipeline(fill.Value());
        commands.Dispatch(1, 1);
        commands.SetWritableTexture(0, writes[0]);
        commands.Dispatch(1, 1);
        commands.SetPipeline(reduce.Value());
        for (uint32 mip = 0; mip + 1 < PyramidMips; ++mip) {
            const uint32 size = PyramidExtent >> (mip + 1);
            LevelConstants level;
            level.values[0] =
                static_cast<float>(TextureViewBaseMip(features, MipView(mip, ViewAccess::Read)));
            level.values[1] = static_cast<float>(size);
            commands.SetTexture(reads[mip]);
            commands.SetWritableTexture(0, writes[mip + 1]);
            Check(commands.SetDrawConstants(DrawConstantBytes(level)).IsOk(),
                  "reduce draw constants are accepted");
            commands.Dispatch((size + ReduceGroup - 1) / ReduceGroup,
                              (size + ReduceGroup - 1) / ReduceGroup);
        }
    }

    Check(frame && device.EndFrame(), "pyramid pass submits");
    Check(device.IgnoredDraws() - ignoredBefore == 1,
          "a dispatch without its writable texture is ignored");
    for (uint32 mip = 0; mip < PyramidMips; ++mip) {
        const uint32 size = PyramidExtent >> mip;
        Array<uint8> bytes(allocator);
        const bool read = bytes.TryResize(static_cast<usize>(size) * size * sizeof(float)).IsOk() &&
                          device.ReadTexture(pyramid, mip, 0, bytes.View()).IsOk();
        bool same = read;
        for (uint32 y = 0; same && y < size; ++y) {
            for (uint32 x = 0; same && x < size; ++x) {
                float value = 0.0f;
                platform::MemoryCopy(&value, bytes.Data() + (y * size + x) * sizeof(float),
                                     sizeof(float));
                same = value == Expected(mip, x, y);
                if (!same) {
                    printf("    mip %u (%u, %u) = %f, expected %f\n", mip, x, y,
                           static_cast<double>(value), static_cast<double>(Expected(mip, x, y)));
                }
            }
        }

        char what[96];
        snprintf(what, sizeof(what), "mip %u of the reduced pyramid matches the CPU reference",
                 mip);
        Check(same, what);
    }

    // PS가 mip 범위 view(mip 2부터)를 view 기준 mip으로 읽는다. mip 2 (0, 0)은 27이다.
    Canvas canvas(device);
    auto levelPipeline =
        canvas.Initialize()
            ? canvas.Pipeline(Text(LevelHlsl), Text(LevelGlsl), sizeof(LevelConstants))
            : Result<PipelineHandle>::Failure(ErrorCode::InvalidArgument);
    TextureViewDesc upper;
    upper.firstMip = 2;
    auto upperView = device.CreateTextureView(pyramid, upper);
    Check(levelPipeline && upperView, "mip range view and level pipeline");
    if (levelPipeline && upperView) {
        LevelConstants level;
        level.values[0] = static_cast<float>(TextureViewBaseMip(features, upper));
        const int expected = static_cast<int>(Expected(2, 0, 0) / ValueScale * 255.0f + 0.5f);
        Check(canvas.Draw(levelPipeline.Value(), upperView.Value(), &level) &&
                  Near(canvas.pixel[0], expected),
              "a pixel shader reads the first mip of a mip range view");
        if (!Near(canvas.pixel[0], expected)) {
            printf("    red %u, expected %d\n", canvas.pixel[0], expected);
        }
    } else {
        PrintLastError(device);
    }

    if (upperView) {
        device.Retire(upperView.Value());
    }

    if (levelPipeline) {
        device.Retire(levelPipeline.Value());
    }

    canvas.Release();
    for (uint32 mip = 0; mip < PyramidMips; ++mip) {
        if (!reads[mip].IsNull()) {
            device.Retire(reads[mip]);
        }

        if (!writes[mip].IsNull()) {
            device.Retire(writes[mip]);
        }
    }

    device.Retire(fill.Value());
    device.Retire(reduce.Value());
}

// 2D 배열 slice 범위 view: 범위를 지원하면 층 1(초록)을 view의 층 0으로 읽는다.
void RunLayerView(Device& device) noexcept {
    TextureDesc desc;
    desc.dimension = TextureDimension::Texture2DArray;
    desc.width = 1;
    desc.height = 1;
    desc.arraySize = 2;
    desc.format = PixelFormat::RGBA8Unorm;
    const uint8 layers[8] = {255, 0, 0, 255, 0, 255, 0, 255};
    auto texture = device.CreateTexture(desc, Span<const uint8>(layers, sizeof(layers)));
    Check(texture.IsOk(), "two layer array texture");
    if (!texture) {
        PrintLastError(device);
        return;
    }

    TextureViewDesc second;
    second.firstSlice = 1;
    second.sliceCount = 1;
    auto view = device.CreateTextureView(texture.Value(), second);
    if (!device.Caps().features.textureViewRanges) {
        Check(view.Error() == ErrorCode::Unsupported,
              "a partial slice view is Unsupported without texture view ranges");
    } else {
        Canvas canvas(device);
        auto pipeline = canvas.Initialize()
                            ? canvas.Pipeline(Text(LayerHlsl), Text(LayerGlsl), 0)
                            : Result<PipelineHandle>::Failure(ErrorCode::InvalidArgument);
        Check(view && pipeline && canvas.Draw(pipeline.Value(), view.Value(), nullptr) &&
                  Near(canvas.pixel[0], 0) && Near(canvas.pixel[1], 255),
              "a slice range view reads its first layer");
        if (pipeline) {
            device.Retire(pipeline.Value());
        }

        canvas.Release();
    }

    if (view) {
        device.Retire(view.Value());
    }

    device.Retire(texture.Value());
}

} // namespace

uint32 RunTextureViewChecks(const char* name, Device& device, Allocator& allocator) noexcept {
    failures = 0;
    backendName = name;
    const uint64 validationBefore = device.ValidationErrorCount();
    TextureDesc desc;
    desc.width = PyramidExtent;
    desc.height = PyramidExtent;
    desc.format = PixelFormat::R32Float;
    desc.usage = TextureUsage::Storage;
    desc.mipLevels = PyramidMips;
    desc.sampler.filter = Filter::Point;
    const bool capable =
        HasPixelFormatCaps(device.GetPixelFormatCaps(desc.format), RequiredPixelFormatCaps(desc));
    const bool compute = device.Caps().features.computeShaders;
    auto pyramid = device.CreateTexture(desc, {});
    if (!capable) {
        Check(pyramid.Error() == ErrorCode::Unsupported,
              "Storage without Sampled, StorageWrite and Mipmaps is Unsupported");
    } else {
        Check(pyramid.IsOk(), "Storage R32Float mip chain");
        if (!pyramid) {
            PrintLastError(device);
        }
    }

    if (pyramid) {
        // 규칙 위반: 쓰기 view의 mip 둘, 범위 밖.
        TextureViewDesc twoMips;
        twoMips.access = ViewAccess::ReadWrite;
        twoMips.mipCount = 2;
        TextureViewDesc outside;
        outside.firstMip = PyramidMips;
        Check(device.CreateTextureView(pyramid.Value(), twoMips).Error() ==
                      ErrorCode::InvalidArgument &&
                  device.CreateTextureView(pyramid.Value(), outside).Error() ==
                      ErrorCode::InvalidArgument,
              "writable views over two mips and views outside the texture are rejected");
        if (compute) {
            RunPyramid(device, allocator, pyramid.Value());
        }

        device.Retire(pyramid.Value());
    }

    // Sampled 텍스처의 쓰기 view는 규칙 위반이다.
    TextureDesc sampledDesc;
    sampledDesc.width = 1;
    sampledDesc.height = 1;
    sampledDesc.format = PixelFormat::RGBA8Unorm;
    const uint8 white[4] = {255, 255, 255, 255};
    auto sampled = device.CreateTexture(sampledDesc, Span<const uint8>(white, sizeof(white)));
    if (sampled) {
        TextureViewDesc write;
        write.access = ViewAccess::ReadWrite;
        Check(device.CreateTextureView(sampled.Value(), write).Error() ==
                  ErrorCode::InvalidArgument,
              "a writable view of a Sampled texture is rejected");
        device.Retire(sampled.Value());
    }

    RunLayerView(device);
    auto idle = device.WaitIdle();
    Check(idle.IsOk(), "wait idle");
    Check(device.ValidationErrorCount() == validationBefore, "no validation errors");
    printf("  %s texture views: storage pyramid %s, ranges %s, %u failures\n", name,
           capable && compute ? "yes" : "no",
           device.Caps().features.textureViewRanges ? "yes" : "no (whole texture)", failures);
    return failures;
}

} // namespace sandbox
