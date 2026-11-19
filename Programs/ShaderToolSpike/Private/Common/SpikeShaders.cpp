#include "SpikeShaders.h"

// SpvImageFormat 값은 SPIRV-Reflect가 동봉한 spirv.h(spirv_reflect.h가 include)에서 온다.
#include <spirv_reflect.h>

namespace rocks::spike {

namespace {

// Programs/Sandbox/Private/SampleRenderer.cpp ShaderHlsl과 같은 행렬 규약의 원문이다(SH0 입력).
// 행 벡터 mul(v, transform)이며 컴파일 옵션(-Zpr, FXC /Zpr)이 행렬을 행 우선으로 담는다
// (ShaderSystem 7.2절 6항).
constexpr char RendererHlsl[] = R"(
cbuffer DrawConstants : register(b0) {
    float4x4 transform;
    float4 color;
};

struct VSInput {
    float3 position : TEXCOORD0;
    float2 uv : TEXCOORD1;
    float4 color : TEXCOORD2;
};

struct VSOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

Texture2D albedo : register(t0);
SamplerState linearClamp : register(s0);

VSOutput VSMain(VSInput input) {
    VSOutput output;
    output.position = mul(float4(input.position, 1.0), transform);
    output.uv = input.uv;
    output.color = input.color * color;
    return output;
}

float4 PSMain(VSOutput input) : SV_Target {
    return albedo.Sample(linearClamp, input.uv) * input.color;
}
)";

constexpr char TranscendentalHlsl[] = R"(
struct Targets {
    float4 first : SV_Target0;
    float4 second : SV_Target1;
};

Targets PSMain() {
    Targets targets;
    targets.first = float4(sin(0.7f), cos(2.2f), tan(0.4f), atan2(0.3f, 1.7f));
    targets.second = float4(exp(1.3f), log(3.1f), pow(2.0f, 0.37f), sqrt(2.0f));
    return targets;
}
)";

// 철자는 FloatLiteralCases와 같은 순서·값이다.
constexpr char FloatLiteralHlsl[] = R"(
struct Targets {
    float4 first : SV_Target0;
    float4 second : SV_Target1;
};

Targets PSMain() {
    Targets targets;
    targets.first = float4(1.40129846e-45f, 1.17549421e-38f, 1.17549435e-38f, 3.40282347e+38f);
    targets.second = float4(0.1f, 0.333333343f, 1.00000012f, -1.0e-40f);
    return targets;
}
)";

// 측정 S13: typed buffer SRV 셋(float4·int4·uint)과 짝 규칙(7.2절 9항)을 지킨 텍스처 하나를 읽는
// PS, typed buffer SRV를 읽어 typed buffer UAV 둘(rgba8 지정 float4·uint)에 쓰는 CS다. register는
// 적지 않는다(7.2절 1항).
constexpr char TypedBufferHlsl[] = R"(
Buffer<float4> floatValues;
Buffer<int4> intValues;
Buffer<uint> uintValues;
Texture2D albedo;
SamplerState albedoSampler;

float4 PSMain(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0 {
    const uint index = (uint)position.x;
    return floatValues.Load(index) + float4(intValues[index]) + uintValues[index] +
           albedo.Sample(albedoSampler, uv);
}

[[vk::image_format("rgba8")]] RWBuffer<float4> colorOutput;
RWBuffer<uint> countOutput;

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    colorOutput[id.x] = floatValues[id.x] + float4(intValues.Load(id.x));
    countOutput[id.x] = countOutput[id.x] + uintValues[id.x];
}
)";

// 측정 S14: VS가 layer(SV_RenderTargetArrayIndex)와 viewport(SV_ViewportArrayIndex)를 함께 낸다
// (7.2절 7항 vertexViewportLayerOutput은 둘을 함께 연다). point 광원 6면처럼 instance마다 고른다.
constexpr char LayerViewportHlsl[] = R"(
struct VSOutput {
    float4 position : SV_Position;
    uint layer : SV_RenderTargetArrayIndex;
    uint viewport : SV_ViewportArrayIndex;
};

VSOutput VSMain(float3 position : TEXCOORD0, uint instance : SV_InstanceID) {
    VSOutput output;
    output.position = float4(position, 1.0);
    output.layer = instance % 6;
    output.viewport = instance / 6;
    return output;
}
)";

constexpr char ImageFormatPrefix[] = "\n";
constexpr char ImageFormatBody[] = R"( RWTexture2D<)";
constexpr char ImageFormatSuffix[] = R"(> Output;

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    Output[id.xy] = 1;
}
)";

} // namespace

const ShaderInput RendererVertex{"Renderer.vs", RendererHlsl, "VSMain", ShaderStage::Vertex};
const ShaderInput RendererPixel{"Renderer.ps", RendererHlsl, "PSMain", ShaderStage::Pixel};
const ShaderInput TranscendentalPixel{"Transcendental.ps", TranscendentalHlsl, "PSMain",
                                      ShaderStage::Pixel};
const ShaderInput FloatLiteralPixel{"FloatLiteral.ps", FloatLiteralHlsl, "PSMain",
                                    ShaderStage::Pixel};

const ShaderInput TypedBufferPixel{"TypedBuffer.ps", TypedBufferHlsl, "PSMain", ShaderStage::Pixel};
const ShaderInput TypedBufferCompute{"TypedBuffer.cs", TypedBufferHlsl, "CSMain",
                                     ShaderStage::Compute};
const ShaderInput LayerViewportVertex{"LayerViewport.vs", LayerViewportHlsl, "VSMain",
                                      ShaderStage::Vertex};

const ImageFormatCase ImageFormatCases[] = {
    // 지정이 없으면 DXC가 원소 타입으로 추론한다(float4 → Rgba32f, 7.2절 1항).
    {"ImageFormat.none", nullptr, "float4", SpvImageFormatRgba32f},
    {"ImageFormat.rgba8", "rgba8", "float4", SpvImageFormatRgba8},
    {"ImageFormat.rgba8snorm", "rgba8snorm", "float4", SpvImageFormatRgba8Snorm},
    {"ImageFormat.rgba16f", "rgba16f", "float4", SpvImageFormatRgba16f},
    {"ImageFormat.r11g11b10f", "r11g11b10f", "float3", SpvImageFormatR11fG11fB10f},
    {"ImageFormat.r32f", "r32f", "float", SpvImageFormatR32f},
    {"ImageFormat.r32ui", "r32ui", "uint", SpvImageFormatR32ui},
    // unknown은 shaderStorageImageWriteWithoutFormat이 필요한 형식 없는 image다.
    {"ImageFormat.unknown", "unknown", "float4", SpvImageFormatUnknown},
};

const usize ImageFormatCaseCount = sizeof(ImageFormatCases) / sizeof(ImageFormatCases[0]);

bool BuildImageFormatSource(const ImageFormatCase& test, StringBuilderBase& source) noexcept {
    source.Clear();
    source.Append(StringView(ImageFormatPrefix));
    if (test.attribute != nullptr) {
        source.Append(StringView("[[vk::image_format(\""))
            .Append(StringView(test.attribute))
            .Append(StringView("\")]]"));
    }

    source.Append(StringView(ImageFormatBody))
        .Append(StringView(test.element))
        .Append(StringView(ImageFormatSuffix));
    return !source.HasFailed();
}

// 비트 패턴은 IEEE 754 binary32 최근접 짝수 반올림 값이다(HLSL 리터럴은 clang APFloat가 같은
// 규칙으로 읽는다).
const FloatLiteralCase FloatLiteralCases[] = {
    {"1.40129846e-45f", 0x00000001u, "smallest subnormal"},
    {"1.17549421e-38f", 0x007fffffu, "largest subnormal"},
    {"1.17549435e-38f", 0x00800000u, "smallest normal"},
    {"3.40282347e+38f", 0x7f7fffffu, "largest finite"},
    {"0.1f", 0x3dcccccdu, "0.1 (inexact)"},
    {"0.333333343f", 0x3eaaaaabu, "1/3 (9 digits)"},
    {"1.00000012f", 0x3f800001u, "1 + ulp (9 digits)"},
    {"-1.0e-40f", 0x800116c2u, "negative subnormal"},
};

const usize FloatLiteralCaseCount = sizeof(FloatLiteralCases) / sizeof(FloatLiteralCases[0]);

} // namespace rocks::spike
