#include "MsaaSelfTest.h"

#include <stdio.h>

namespace sandbox {

namespace {

using namespace rocks;
using namespace rocks::graphics;

// 대상은 4×1이다. 사각형은 x = 1.5(NDC -0.25)에서 끝나 픽셀 0은 덮이고 1은 반쯤, 2·3은 비어 있다.
constexpr uint32 TargetWidth = 4;
constexpr uint32 TargetHeight = 1;
constexpr uint32 Samples = 4;
constexpr uint32 PixelBytes = 4;

uint32 failures = 0;
const char* backendName = "";

void Check(bool condition, const char* what) noexcept {
    if (!condition) {
        ++failures;
        printf("  [FAIL] %s msaa: %s\n", backendName, what);
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

#define MSAA_GLSL_HEAD "#version 320 es\nprecision highp float;\nprecision highp int;\n"

// 정점 6개로 x ∈ [-1, -0.25], y ∈ [-1, 1] 사각형을 그리고 흰색을 낸다.
constexpr char CoverHlsl[] = R"(
float4 VSMain(uint id : SV_VertexID) : SV_Position {
    const float2 corners[6] = {float2(-1, -1), float2(-1, 1), float2(-0.25, -1),
                               float2(-0.25, -1), float2(-1, 1), float2(-0.25, 1)};
    return float4(corners[id], 0.5, 1.0);
}
float4 PSMain() : SV_Target { return float4(1.0, 1.0, 1.0, 1.0); }
)";

constexpr char CoverVertexGlsl[] = MSAA_GLSL_HEAD R"(
void main() {
    const vec2 corners[6] = vec2[6](vec2(-1, -1), vec2(-1, 1), vec2(-0.25, -1),
                                    vec2(-0.25, -1), vec2(-1, 1), vec2(-0.25, 1));
    gl_Position = vec4(corners[gl_VertexID], 0.5, 1.0);
}
)";

constexpr char CoverPixelGlsl[] = MSAA_GLSL_HEAD R"(
layout(location = 0) out vec4 out_var_SV_Target0;
void main() { out_var_SV_Target0 = vec4(1.0); }
)";

// 화면을 덮고 MSAA 텍스처의 같은 픽셀 표본 0을 낸다.
constexpr char LoadHlsl[] = R"(
Texture2DMS<float4> msaa : register(t0);
float4 VSMain(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * 2.0 - 1.0, 0.5, 1.0);
}
float4 PSMain(float4 position : SV_Position) : SV_Target {
    return msaa.Load(int2(position.xy), 0);
}
)";

constexpr char LoadVertexGlsl[] = MSAA_GLSL_HEAD R"(
void main() {
    vec2 uv = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(uv * 2.0 - 1.0, 0.5, 1.0);
}
)";

constexpr char LoadPixelGlsl[] = MSAA_GLSL_HEAD R"(
layout(binding = 0) uniform highp sampler2DMS ps0;
layout(location = 0) out vec4 out_var_SV_Target0;
void main() { out_var_SV_Target0 = texelFetch(ps0, ivec2(gl_FragCoord.xy), 0); }
)";

#undef MSAA_GLSL_HEAD

TextureDesc TargetDesc(uint32 samples, TextureUsage usage, PixelFormat format) noexcept {
    TextureDesc desc;
    desc.width = TargetWidth;
    desc.height = TargetHeight;
    desc.format = format;
    desc.usage = usage;
    desc.sampleCount = samples;
    return desc;
}

Result<PipelineHandle> MakePipeline(Device& device, StringView hlsl, StringView vertexGlsl,
                                    StringView pixelGlsl, uint32 samples) noexcept {
    PipelineDesc desc;
    desc.vertex = {hlsl, vertexGlsl};
    desc.pixel = {hlsl, pixelGlsl};
    desc.colorFormats[0] = PixelFormat::RGBA8Unorm;
    desc.sampleCount = samples;
    return device.CreatePipeline(desc);
}

// 4x MSAA에 그리고 resolve한 픽셀과, PS가 표본을 읽은 픽셀을 대조한다.
void RunResolve(Device& device, TextureHandle msaa) noexcept {
    auto resolved = device.CreateTexture(
        TargetDesc(1, TextureUsage::RenderTarget, PixelFormat::RGBA8Unorm), {});
    auto loaded = device.CreateTexture(
        TargetDesc(1, TextureUsage::RenderTarget, PixelFormat::RGBA8Unorm), {});
    auto cover =
        MakePipeline(device, Text(CoverHlsl), Text(CoverVertexGlsl), Text(CoverPixelGlsl), Samples);
    auto load = MakePipeline(device, Text(LoadHlsl), Text(LoadVertexGlsl), Text(LoadPixelGlsl), 1);
    Check(resolved && loaded && cover && load, "resolve targets and MSAA pipelines");
    if (!resolved || !loaded || !cover || !load) {
        PrintLastError(device);
        return;
    }

    // MSAA 내용은 resolve만 쓰므로 store는 DontCare다.
    PassDesc pass;
    pass.colorCount = 1;
    pass.colors[0].texture = msaa;
    pass.colors[0].store = StoreAction::DontCare;
    pass.colors[0].resolveTarget = resolved.Value();
    auto frame = device.BeginPass(pass);
    if (frame) {
        frame.Value()->SetPipeline(cover.Value());
        frame.Value()->Draw(6);
    }

    uint8 pixels[TargetWidth * TargetHeight * PixelBytes] = {};
    Check(frame && device.EndFrame() &&
              device.ReadPixels(resolved.Value(), Span<uint8>(pixels, sizeof(pixels))).IsOk(),
          "MSAA pass resolves and the resolve target reads back");
    const uint8 covered = pixels[0];
    const uint8 edge = pixels[1 * PixelBytes];
    const uint8 empty = pixels[2 * PixelBytes];
    const uint8 far = pixels[3 * PixelBytes];
    Check(covered == 255 && empty == 0 && far == 0,
          "resolve keeps fully covered and empty pixels exact");
    Check(edge > 0 && edge < 255, "resolve averages the samples of an edge pixel");
    printf("    %s msaa edge pixel resolves to %u of 255\n", backendName, edge);

    // PS가 resolve하지 않은 MSAA 표본(표본 0)을 읽는다. store DontCare라 내용이 없을 수 있으므로
    // 다시 그린다(store Store).
    pass.colors[0].store = StoreAction::Store;
    pass.colors[0].resolveTarget = {};
    frame = device.BeginPass(pass);
    if (frame) {
        frame.Value()->SetPipeline(cover.Value());
        frame.Value()->Draw(6);
    }

    Check(frame && device.EndFrame(), "MSAA pass without resolve submits");
    PassDesc readPass;
    readPass.colorCount = 1;
    readPass.colors[0].texture = loaded.Value();
    frame = device.BeginPass(readPass);
    if (frame) {
        frame.Value()->SetPipeline(load.Value());
        frame.Value()->SetTexture(msaa);
        frame.Value()->Draw(3);
    }

    Check(frame && device.EndFrame() &&
              device.ReadPixels(loaded.Value(), Span<uint8>(pixels, sizeof(pixels))).IsOk() &&
              pixels[0] == 255 && pixels[3 * PixelBytes] == 0,
          "a pixel shader loads MSAA samples (Texture2DMS, sampler2DMS)");

    // 규칙 위반: 크기가 다른 resolve 대상, 1표본 attachment의 resolve, 표본 수가 다른 attachment.
    TextureDesc wideDesc = TargetDesc(1, TextureUsage::RenderTarget, PixelFormat::RGBA8Unorm);
    wideDesc.width = TargetWidth * 2;
    auto wide = device.CreateTexture(wideDesc, {});
    if (wide) {
        PassDesc mismatch = pass;
        mismatch.colors[0].resolveTarget = wide.Value();
        Check(device.BeginPass(mismatch).Error() == ErrorCode::InvalidArgument,
              "a resolve target of another size is rejected");
        device.Retire(wide.Value());
    }

    PassDesc single = readPass;
    single.colors[0].resolveTarget = resolved.Value();
    Check(device.BeginPass(single).Error() == ErrorCode::InvalidArgument,
          "resolving a single-sample attachment is rejected");
    PassDesc mixed;
    mixed.colorCount = 2;
    mixed.colors[0].texture = msaa;
    mixed.colors[1].texture = loaded.Value();
    Check(device.BeginPass(mixed).Error() == ErrorCode::InvalidArgument,
          "attachments with different sample counts are rejected");
    Check(device.ReadPixels(msaa, Span<uint8>(pixels, sizeof(pixels))).Error() ==
              ErrorCode::InvalidArgument,
          "reading an MSAA texture back is rejected (resolve first)");
    device.Retire(cover.Value());
    device.Retire(load.Value());
    device.Retire(resolved.Value());
    device.Retire(loaded.Value());
}

} // namespace

uint32 RunMsaaChecks(const char* name, Device& device, Allocator& allocator) noexcept {
    (void)allocator;
    failures = 0;
    backendName = name;
    const uint64 validationBefore = device.ValidationErrorCount();
    // 장치 무관 규칙: 표본 수 3, MSAA Sampled.
    Check(
        device.CreateTexture(TargetDesc(3, TextureUsage::RenderTarget, PixelFormat::RGBA8Unorm), {})
                .Error() == ErrorCode::InvalidArgument,
        "a sample count that is not a power of two is rejected");
    const uint8 texels[TargetWidth * PixelBytes] = {};
    Check(device.CreateTexture(TargetDesc(Samples, TextureUsage::Sampled, PixelFormat::RGBA8Unorm),
                               Span<const uint8>(texels, sizeof(texels)))
                  .Error() == ErrorCode::InvalidArgument,
          "an MSAA Sampled texture is rejected");
    const bool supported =
        (device.GetPixelFormatSampleCounts(PixelFormat::RGBA8Unorm) & SampleCountBit(Samples)) != 0;
    auto msaa = device.CreateTexture(
        TargetDesc(Samples, TextureUsage::RenderTarget, PixelFormat::RGBA8Unorm), {});
    if (!supported) {
        Check(msaa.Error() == ErrorCode::Unsupported,
              "4x MSAA without device support is Unsupported");
    } else {
        Check(msaa.IsOk(), "4x MSAA render target");
        if (!msaa) {
            PrintLastError(device);
        }
    }

    if (msaa) {
        RunResolve(device, msaa.Value());
        device.Retire(msaa.Value());
    }

    auto idle = device.WaitIdle();
    Check(idle.IsOk(), "wait idle");
    Check(device.ValidationErrorCount() == validationBefore, "no validation errors");
    printf("  %s msaa: 4x %s, %u failures\n", name, supported ? "yes" : "no", failures);
    return failures;
}

} // namespace sandbox
