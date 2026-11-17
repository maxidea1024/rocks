#include "VertexSelfTest.h"

#include <Core/Containers/Array.h>
#include <Core/Math/VertexPacking.h>
#include <Platform/Memory.h>
#include <stdio.h>

namespace sandbox {

namespace {

using namespace rocks;
using namespace rocks::graphics;

// offscreen 대상 크기다(선형 RGBA8, sRGB 인코딩 없음).
constexpr uint32 TargetSize = 8;
// 기대 색과 읽은 색의 허용 차이(8-bit 단위)다.
constexpr int ColorTolerance = 2;

// 정점 셰이더 입력 계열별 원문이다. 위치(ATTRIBUTE0)와 값(ATTRIBUTE1)을 읽어 값의 rgb를 색으로
// 낸다. 정수 계열은 값을 255로 나눠 낸다.
constexpr char FloatHlsl[] = R"(
struct VSInput { float2 position : ATTRIBUTE0; float4 value : ATTRIBUTE1; };
struct VSOutput { float4 position : SV_Position; float4 color : COLOR0; };
VSOutput VSMain(VSInput input) {
    VSOutput output;
    output.position = float4(input.position, 0.5, 1.0);
    output.color = float4(input.value.rgb, 1.0);
    return output;
}
float4 PSMain(VSOutput input) : SV_Target { return input.color; }
)";

constexpr char IntHlsl[] = R"(
struct VSInput { float2 position : ATTRIBUTE0; int4 value : ATTRIBUTE1; };
struct VSOutput { float4 position : SV_Position; float4 color : COLOR0; };
VSOutput VSMain(VSInput input) {
    VSOutput output;
    output.position = float4(input.position, 0.5, 1.0);
    output.color = float4(float3(input.value.rgb) / 255.0, 1.0);
    return output;
}
float4 PSMain(VSOutput input) : SV_Target { return input.color; }
)";

constexpr char UintHlsl[] = R"(
struct VSInput { float2 position : ATTRIBUTE0; uint4 value : ATTRIBUTE1; };
struct VSOutput { float4 position : SV_Position; float4 color : COLOR0; };
VSOutput VSMain(VSInput input) {
    VSOutput output;
    output.position = float4(input.position, 0.5, 1.0);
    output.color = float4(float3(input.value.rgb) / 255.0, 1.0);
    return output;
}
float4 PSMain(VSOutput input) : SV_Target { return input.color; }
)";

constexpr char FloatVertexGlsl[] = R"(#version 450 core
layout(location = 0) in vec2 aPosition;
layout(location = 1) in vec4 aValue;
layout(location = 0) out vec4 vColor;
void main() {
    gl_Position = vec4(aPosition, 0.5, 1.0);
    vColor = vec4(aValue.rgb, 1.0);
}
)";

constexpr char IntVertexGlsl[] = R"(#version 450 core
layout(location = 0) in vec2 aPosition;
layout(location = 1) in ivec4 aValue;
layout(location = 0) out vec4 vColor;
void main() {
    gl_Position = vec4(aPosition, 0.5, 1.0);
    vColor = vec4(vec3(aValue.rgb) / 255.0, 1.0);
}
)";

constexpr char UintVertexGlsl[] = R"(#version 450 core
layout(location = 0) in vec2 aPosition;
layout(location = 1) in uvec4 aValue;
layout(location = 0) out vec4 vColor;
void main() {
    gl_Position = vec4(aPosition, 0.5, 1.0);
    vColor = vec4(vec3(aValue.rgb) / 255.0, 1.0);
}
)";

constexpr char ColorPixelGlsl[] = R"(#version 450 core
layout(location = 0) in vec4 vColor;
layout(location = 0) out vec4 oColor;
void main() {
    oColor = vColor;
}
)";

// 인스턴스 stream: 위치(정점), 인스턴스별 offset·색이다.
constexpr char InstanceHlsl[] = R"(
struct VSInput { float2 position : ATTRIBUTE0; float2 offset : ATTRIBUTE1; float4 color : ATTRIBUTE2; };
struct VSOutput { float4 position : SV_Position; float4 color : COLOR0; };
VSOutput VSMain(VSInput input) {
    VSOutput output;
    output.position = float4(input.position + input.offset, 0.5, 1.0);
    output.color = float4(input.color.rgb, 1.0);
    return output;
}
float4 PSMain(VSOutput input) : SV_Target { return input.color; }
)";

constexpr char InstanceVertexGlsl[] = R"(#version 450 core
layout(location = 0) in vec2 aPosition;
layout(location = 1) in vec2 aOffset;
layout(location = 2) in vec4 aColor;
layout(location = 0) out vec4 vColor;
void main() {
    gl_Position = vec4(aPosition + aOffset, 0.5, 1.0);
    vColor = vec4(aColor.rgb, 1.0);
}
)";

// 정점 버퍼 없이 정점 번호로 화면을 덮는 삼각형을 만든다.
constexpr char NoInputHlsl[] = R"(
struct VSOutput { float4 position : SV_Position; float4 color : COLOR0; };
VSOutput VSMain(uint id : SV_VertexID) {
    VSOutput output;
    float2 uv = float2((id << 1) & 2, id & 2);
    output.position = float4(uv * 2.0 - 1.0, 0.5, 1.0);
    output.color = float4(0.25, 0.5, 0.75, 1.0);
    return output;
}
float4 PSMain(VSOutput input) : SV_Target { return input.color; }
)";

constexpr char NoInputVertexGlsl[] = R"(#version 450 core
layout(location = 0) out vec4 vColor;
void main() {
    vec2 uv = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(uv * 2.0 - 1.0, 0.5, 1.0);
    vColor = vec4(0.25, 0.5, 0.75, 1.0);
}
)";

StringView Text(const char* text, usize bytes) noexcept {
    return StringView(text, bytes - 1);
}

template <usize Count>
StringView Text(const char (&text)[Count]) noexcept {
    return Text(text, Count);
}

uint32 failures = 0;
const char* backendName = "";

void Check(bool condition, const char* what) noexcept {
    if (!condition) {
        ++failures;
        printf("  [FAIL] %s vertex: %s\n", backendName, what);
    }
}

void CheckFormat(bool condition, VertexFormat format, const char* what) noexcept {
    if (!condition) {
        ++failures;
        printf("  [FAIL] %s vertex %s: %s\n", backendName, VertexFormatName(format), what);
    }
}

struct Expected {
    uint8 r = 0;
    uint8 g = 0;
    uint8 b = 0;
};

uint8 ToByte(float value) noexcept {
    return math::PackUnorm8(value);
}

// 형식 하나의 시험 값(최대 16 byte)과 기대 색이다. 값 rgb = (0.25, 0.5, 0.75) 또는 정수
// (64, 128, 192)를 형식이 담을 수 있는 성분만큼 넣는다. 없는 성분의 기대 색은 0이다.
struct FormatCase {
    uint8 bytes[16] = {};
    Expected expected;
};

FormatCase BuildCase(VertexFormat format) noexcept {
    const float values[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    const uint32 integers[4] = {64, 128, 192, 255};
    const uint32 components = VertexFormatComponents(format);
    FormatCase result;
    float decoded[3] = {};
    auto put = [&result](usize offset, const void* data, usize size) noexcept {
        platform::MemoryCopy(result.bytes + offset, data, size);
    };
    for (uint32 index = 0; index < 4; ++index) {
        switch (format) {
        case VertexFormat::Float1:
        case VertexFormat::Float2:
        case VertexFormat::Float3:
        case VertexFormat::Float4:
            if (index < components) {
                put(index * 4u, &values[index], 4);
                if (index < 3) {
                    decoded[index] = values[index];
                }
            }
            break;
        case VertexFormat::Byte4Snorm: {
            const int8 packed = math::PackSnorm8(values[index]);
            put(index, &packed, 1);
            if (index < 3) {
                decoded[index] = math::UnpackSnorm8(packed);
            }
            break;
        }
        case VertexFormat::UByte4: {
            const uint8 packed = static_cast<uint8>(integers[index]);
            put(index, &packed, 1);
            if (index < 3) {
                decoded[index] = static_cast<float>(packed) / 255.0f;
            }
            break;
        }
        case VertexFormat::UByte4Unorm: {
            const uint8 packed = math::PackUnorm8(values[index]);
            put(index, &packed, 1);
            if (index < 3) {
                decoded[index] = math::UnpackUnorm8(packed);
            }
            break;
        }
        case VertexFormat::Bgra8Unorm: {
            // 메모리는 B·G·R·A 순이고 셰이더는 r·g·b·a로 받는다.
            const uint8 packed = math::PackUnorm8(values[index]);
            const uint32 position = index < 3 ? 2 - index : 3;
            put(position, &packed, 1);
            if (index < 3) {
                decoded[index] = math::UnpackUnorm8(packed);
            }
            break;
        }
        case VertexFormat::Short2:
        case VertexFormat::Short4:
            if (index < components) {
                const int16 packed = static_cast<int16>(integers[index]);
                put(index * 2u, &packed, 2);
                if (index < 3) {
                    decoded[index] = static_cast<float>(packed) / 255.0f;
                }
            }
            break;
        case VertexFormat::Short2Snorm:
        case VertexFormat::Short4Snorm:
            if (index < components) {
                const int16 packed = math::PackSnorm16(values[index]);
                put(index * 2u, &packed, 2);
                if (index < 3) {
                    decoded[index] = math::UnpackSnorm16(packed);
                }
            }
            break;
        case VertexFormat::Half2:
        case VertexFormat::Half4:
            if (index < components) {
                const Float16 packed = Float16::FromFloat(values[index]);
                put(index * 2u, &packed.bits, 2);
                if (index < 3) {
                    decoded[index] = packed.ToFloat();
                }
            }
            break;
        case VertexFormat::UShort2:
        case VertexFormat::UShort4:
            if (index < components) {
                const uint16 packed = static_cast<uint16>(integers[index]);
                put(index * 2u, &packed, 2);
                if (index < 3) {
                    decoded[index] = static_cast<float>(packed) / 255.0f;
                }
            }
            break;
        case VertexFormat::UShort2Unorm:
        case VertexFormat::UShort4Unorm:
            if (index < components) {
                const uint16 packed = math::PackUnorm16(values[index]);
                put(index * 2u, &packed, 2);
                if (index < 3) {
                    decoded[index] = math::UnpackUnorm16(packed);
                }
            }
            break;
        case VertexFormat::Rgb10A2Unorm:
            if (index == 0) {
                const uint32 packed =
                    math::PackRgb10A2Unorm(values[0], values[1], values[2], values[3]);
                put(0, &packed, 4);
                for (uint32 channel = 0; channel < 3; ++channel) {
                    decoded[channel] = math::UnpackRgb10A2Unorm(packed, channel);
                }
            }
            break;
        case VertexFormat::UInt1:
            if (index == 0) {
                put(0, &integers[0], 4);
                decoded[0] = static_cast<float>(integers[0]) / 255.0f;
            }
            break;
        }
    }

    result.expected = {ToByte(decoded[0]), ToByte(decoded[1]), ToByte(decoded[2])};
    return result;
}

bool Near(uint8 actual, uint8 expected) noexcept {
    const int difference = static_cast<int>(actual) - static_cast<int>(expected);
    return difference <= ColorTolerance && difference >= -ColorTolerance;
}

bool PixelIs(const Array<uint8>& pixels, uint32 x, uint32 y, Expected expected) noexcept {
    const usize offset = (static_cast<usize>(y) * TargetSize + x) * 4;
    const bool same = Near(pixels[offset], expected.r) && Near(pixels[offset + 1], expected.g) &&
                      Near(pixels[offset + 2], expected.b);
    if (!same) {
        printf("    pixel (%u, %u) = (%u, %u, %u), expected (%u, %u, %u)\n", x, y, pixels[offset],
               pixels[offset + 1], pixels[offset + 2], expected.r, expected.g, expected.b);
    }

    return same;
}

// 테스트 장치 자원과 그리기 도우미다.
struct Bench {
    Device* device = nullptr;
    TextureHandle target;
    Array<uint8> pixels;

    explicit Bench(Allocator& allocator) noexcept : pixels(allocator) {
    }

    Result<BufferHandle> Buffer(const void* data, usize size,
                                BufferUsage usage = BufferUsage::Vertex) noexcept {
        BufferDesc desc;
        desc.size = size;
        desc.usage = usage;
        return device->CreateBuffer(desc, Span<const uint8>(static_cast<const uint8*>(data), size));
    }

    Result<PipelineHandle> Pipeline(StringView hlsl, StringView vertexGlsl,
                                    const VertexLayout& layout) noexcept {
        PipelineDesc desc;
        desc.vertex = {hlsl, vertexGlsl};
        desc.pixel = {hlsl, Text(ColorPixelGlsl)};
        desc.layout = layout;
        desc.colorFormat = PixelFormat::RGBA8Unorm;
        return device->CreatePipeline(desc);
    }

    // draw가 기록을 채운 프레임 하나를 그리고 읽는다.
    template <class Draw>
    bool Render(Draw&& draw) noexcept {
        auto frame = device->BeginOffscreen(target, {});
        if (!frame) {
            return false;
        }

        draw(*frame.Value());
        return device->EndFrame().IsOk() &&
               pixels.TryResize(static_cast<usize>(TargetSize) * TargetSize * 4).IsOk() &&
               device->ReadPixels(target, pixels.View()).IsOk();
    }
};

// 화면을 덮는 사각형(삼각형 둘)의 위치다.
constexpr float FullScreen[6][2] = {{-1, -1}, {-1, 1}, {1, 1}, {-1, -1}, {1, 1}, {1, -1}};

void RunFormat(Bench& bench, VertexFormat format, bool openGl) noexcept {
    const VertexShaderType shaderType = VertexFormatShaderType(format);
    StringView hlsl = Text(FloatHlsl);
    StringView glsl = Text(FloatVertexGlsl);
    if (shaderType == VertexShaderType::Int) {
        hlsl = Text(IntHlsl);
        glsl = Text(IntVertexGlsl);
    } else if (shaderType == VertexShaderType::Uint) {
        hlsl = Text(UintHlsl);
        glsl = Text(UintVertexGlsl);
    }

    const VertexAttribute attributes[] = {{0, VertexFormat::Float2, 0, 0}, {1, format, 0, 8}};
    const VertexStream streams[] = {{24, VertexStepRate::Vertex}};
    auto pipeline = bench.Pipeline(hlsl, glsl, {attributes, streams});
    if (openGl && format == VertexFormat::Bgra8Unorm) {
        // ES 공통 API에 BGRA 정점 배열이 없어 미지원이다(음성 대조).
        CheckFormat(pipeline.Error() == ErrorCode::Unsupported, format,
                    "OpenGL rejects the format as unsupported");
        return;
    }

    CheckFormat(pipeline.IsOk(), format, "pipeline accepts the format");
    if (!pipeline) {
        printf("    %.*s\n", static_cast<int>(bench.device->LastError().ByteCount()),
               bench.device->LastError().Data());
        return;
    }

    const FormatCase sample = BuildCase(format);
    uint8 vertices[6][24] = {};
    for (uint32 vertex = 0; vertex < 6; ++vertex) {
        platform::MemoryCopy(vertices[vertex], FullScreen[vertex], 8);
        platform::MemoryCopy(vertices[vertex] + 8, sample.bytes, 16);
    }

    auto buffer = bench.Buffer(vertices, sizeof(vertices));
    const bool drawn = buffer && bench.Render([&](CommandContext& commands) noexcept {
        commands.SetPipeline(pipeline.Value());
        const VertexBufferBinding binding{buffer.Value(), 0};
        commands.SetVertexBuffers(0, Span<const VertexBufferBinding>(&binding, 1));
        commands.Draw(6);
    });
    CheckFormat(drawn && PixelIs(bench.pixels, 4, 4, sample.expected), format,
                "drawn color matches the packed value");
    if (buffer) {
        bench.device->Retire(buffer.Value());
    }

    bench.device->Retire(pipeline.Value());
}

// 정점 버퍼 내용 갱신(GX8): UpdateBuffer와 staging에서 올린 색이 다음 그리기에 보인다.
void RunBufferUpdates(Bench& bench) noexcept {
    const VertexAttribute attributes[] = {{0, VertexFormat::Float2, 0, 0},
                                          {1, VertexFormat::UByte4Unorm, 1, 0}};
    const VertexStream streams[] = {{8, VertexStepRate::Vertex}, {4, VertexStepRate::Vertex}};
    auto pipeline = bench.Pipeline(Text(FloatHlsl), Text(FloatVertexGlsl), {attributes, streams});
    uint8 colors[6][4] = {};
    auto positions = bench.Buffer(FullScreen, sizeof(FullScreen));
    auto colorBuffer = bench.Buffer(colors, sizeof(colors));
    auto draw = [&]() noexcept {
        return bench.Render([&](CommandContext& commands) noexcept {
            commands.SetPipeline(pipeline.Value());
            const VertexBufferBinding bindings[] = {{positions.Value(), 0},
                                                    {colorBuffer.Value(), 0}};
            commands.SetVertexBuffers(0, bindings);
            commands.Draw(6);
        });
    };

    bool ready = pipeline && positions && colorBuffer;
    for (uint32 vertex = 0; vertex < 6; ++vertex) {
        colors[vertex][0] = 200;
        colors[vertex][1] = 40;
        colors[vertex][2] = 16;
        colors[vertex][3] = 255;
    }

    const Span<const uint8> updated(&colors[0][0], sizeof(colors));
    Check(ready && bench.device->UpdateBuffer(colorBuffer.Value(), 0, updated).IsOk() && draw() &&
              PixelIs(bench.pixels, 4, 4, {200, 40, 16}),
          "vertex buffer content update is visible to the next draw");
    if (ready) {
        Check(bench.device->UpdateBuffer(colorBuffer.Value(), 4, updated).Error() ==
                  ErrorCode::InvalidArgument,
              "buffer update past the end is rejected");
    }

    auto staging = ready ? bench.device->AcquireStaging(sizeof(colors))
                         : Result<StagingBuffer>::Failure(ErrorCode::InvalidArgument);
    if (staging) {
        for (uint32 vertex = 0; vertex < 6; ++vertex) {
            const uint8 color[4] = {24, 160, 96, 255};
            platform::MemoryCopy(staging.Value().data + vertex * 4, color, 4);
        }

        const bool uploaded =
            bench.device
                ->UploadBufferFromStaging(colorBuffer.Value(), 0, staging.Value().handle, 0,
                                          sizeof(colors))
                .IsOk();
        bench.device->ReleaseStaging(staging.Value().handle);
        Check(uploaded && draw() && PixelIs(bench.pixels, 4, 4, {24, 160, 96}),
              "vertex buffer uploaded from staging");
    } else {
        Check(false, "staging for a buffer upload");
    }

    if (positions) {
        bench.device->Retire(positions.Value());
    }

    if (colorBuffer) {
        bench.device->Retire(colorBuffer.Value());
    }

    if (pipeline) {
        bench.device->Retire(pipeline.Value());
    }
}

void RunStreams(Bench& bench) noexcept {
    // 다중 stream: 위치 stream 0, 색 stream 1(binding offset 4 byte).
    const VertexAttribute attributes[] = {{0, VertexFormat::Float2, 0, 0},
                                          {1, VertexFormat::UByte4Unorm, 1, 0}};
    const VertexStream streams[] = {{8, VertexStepRate::Vertex}, {4, VertexStepRate::Vertex}};
    auto pipeline = bench.Pipeline(Text(FloatHlsl), Text(FloatVertexGlsl), {attributes, streams});
    uint8 colors[7][4] = {};
    for (uint32 vertex = 1; vertex < 7; ++vertex) {
        colors[vertex][0] = 64;
        colors[vertex][1] = 128;
        colors[vertex][2] = 192;
        colors[vertex][3] = 255;
    }

    auto positions = bench.Buffer(FullScreen, sizeof(FullScreen));
    auto colorBuffer = bench.Buffer(colors, sizeof(colors));
    bool drawn = pipeline && positions && colorBuffer;
    drawn = drawn && bench.Render([&](CommandContext& commands) noexcept {
        commands.SetPipeline(pipeline.Value());
        const VertexBufferBinding bindings[] = {{positions.Value(), 0}, {colorBuffer.Value(), 4}};
        commands.SetVertexBuffers(0, bindings);
        commands.Draw(6);
    });
    Check(drawn && PixelIs(bench.pixels, 4, 4, {64, 128, 192}),
          "two vertex streams with a binding offset");

    // stream 1을 묶지 않은 그리기는 무시하고 진단 계수를 올린다.
    const uint64 ignored = bench.device->IgnoredDraws();
    bool rendered = drawn && bench.Render([&](CommandContext& commands) noexcept {
        commands.SetPipeline(pipeline.Value());
        const VertexBufferBinding binding{positions.Value(), 0};
        commands.SetVertexBuffers(0, Span<const VertexBufferBinding>(&binding, 1));
        commands.Draw(6);
    });
    Check(rendered && bench.device->IgnoredDraws() == ignored + 1 &&
              PixelIs(bench.pixels, 4, 4, {0, 0, 0}),
          "draw with an unbound stream is ignored and counted");

    if (pipeline) {
        bench.device->Retire(pipeline.Value());
    }

    if (positions) {
        bench.device->Retire(positions.Value());
    }

    if (colorBuffer) {
        bench.device->Retire(colorBuffer.Value());
    }
}

void RunInstances(Bench& bench) noexcept {
    // 왼쪽 절반 사각형을 인스턴스 둘로 그린다: 인스턴스 0은 그대로 빨강, 1은 오른쪽으로 옮겨 초록.
    const VertexAttribute attributes[] = {{0, VertexFormat::Float2, 0, 0},
                                          {1, VertexFormat::Float2, 1, 0},
                                          {2, VertexFormat::UByte4Unorm, 1, 8}};
    const VertexStream streams[] = {{8, VertexStepRate::Vertex}, {12, VertexStepRate::Instance}};
    auto pipeline =
        bench.Pipeline(Text(InstanceHlsl), Text(InstanceVertexGlsl), {attributes, streams});
    const float half[6][2] = {{-1, -1}, {-1, 1}, {0, 1}, {-1, -1}, {0, 1}, {0, -1}};
    struct Instance {
        float offset[2];
        uint8 color[4];
    };
    const Instance instances[2] = {{{0, 0}, {255, 0, 0, 255}}, {{1, 0}, {0, 255, 0, 255}}};
    auto positions = bench.Buffer(half, sizeof(half));
    auto perInstance = bench.Buffer(instances, sizeof(instances));
    bool drawn = pipeline && positions && perInstance;
    drawn = drawn && bench.Render([&](CommandContext& commands) noexcept {
        commands.SetPipeline(pipeline.Value());
        const VertexBufferBinding bindings[] = {{positions.Value(), 0}, {perInstance.Value(), 0}};
        commands.SetVertexBuffers(0, bindings);
        commands.Draw(6, 2);
    });
    Check(drawn && PixelIs(bench.pixels, 1, 4, {255, 0, 0}) &&
              PixelIs(bench.pixels, 6, 4, {0, 255, 0}),
          "instance stream advances once per instance");
    if (pipeline) {
        bench.device->Retire(pipeline.Value());
    }

    if (positions) {
        bench.device->Retire(positions.Value());
    }

    if (perInstance) {
        bench.device->Retire(perInstance.Value());
    }
}

void RunIndexed(Bench& bench, IndexFormat format) noexcept {
    // 네 귀퉁이 정점과 index 여섯으로 화면을 덮는다. UInt32는 앞에 쓰지 않는 index 셋을 두고 첫
    // index 3부터 그린다.
    const VertexAttribute attributes[] = {{0, VertexFormat::Float2, 0, 0},
                                          {1, VertexFormat::UByte4Unorm, 0, 8}};
    const VertexStream streams[] = {{12, VertexStepRate::Vertex}};
    auto pipeline = bench.Pipeline(Text(FloatHlsl), Text(FloatVertexGlsl), {attributes, streams});
    const uint8 shade = format == IndexFormat::UInt16 ? 96 : 160;
    struct Corner {
        float position[2];
        uint8 color[4];
    };
    const Corner corners[4] = {{{-1, -1}, {shade, 32, 200, 255}},
                               {{-1, 1}, {shade, 32, 200, 255}},
                               {{1, 1}, {shade, 32, 200, 255}},
                               {{1, -1}, {shade, 32, 200, 255}}};
    const uint16 indices16[6] = {0, 1, 2, 0, 2, 3};
    const uint32 indices32[9] = {7, 7, 7, 0, 1, 2, 0, 2, 3};
    auto vertices = bench.Buffer(corners, sizeof(corners));
    auto indices = format == IndexFormat::UInt16
                       ? bench.Buffer(indices16, sizeof(indices16), BufferUsage::Index)
                       : bench.Buffer(indices32, sizeof(indices32), BufferUsage::Index);
    bool drawn = pipeline && vertices && indices;
    drawn = drawn && bench.Render([&](CommandContext& commands) noexcept {
        commands.SetPipeline(pipeline.Value());
        const VertexBufferBinding binding{vertices.Value(), 0};
        commands.SetVertexBuffers(0, Span<const VertexBufferBinding>(&binding, 1));
        commands.SetIndexBuffer(indices.Value(), format);
        commands.DrawIndexed(6, 1, format == IndexFormat::UInt16 ? 0u : 3u);
    });
    const Expected expected{shade, 32, 200};
    Check(drawn && PixelIs(bench.pixels, 0, 0, expected) && PixelIs(bench.pixels, 7, 7, expected) &&
              PixelIs(bench.pixels, 0, 7, expected) && PixelIs(bench.pixels, 7, 0, expected),
          format == IndexFormat::UInt16 ? "indexed draw with UInt16 indices"
                                        : "indexed draw with UInt32 indices and a first index");
    if (pipeline) {
        bench.device->Retire(pipeline.Value());
    }

    if (vertices) {
        bench.device->Retire(vertices.Value());
    }

    if (indices) {
        bench.device->Retire(indices.Value());
    }
}

void RunNoInput(Bench& bench) noexcept {
    auto pipeline = bench.Pipeline(Text(NoInputHlsl), Text(NoInputVertexGlsl), {});
    bool drawn = pipeline && bench.Render([&](CommandContext& commands) noexcept {
        commands.SetPipeline(pipeline.Value());
        commands.Draw(3);
    });
    const Expected expected{ToByte(0.25f), ToByte(0.5f), ToByte(0.75f)};
    Check(drawn && PixelIs(bench.pixels, 0, 0, expected) && PixelIs(bench.pixels, 7, 7, expected),
          "draw without vertex buffers uses the vertex index");
    if (pipeline) {
        bench.device->Retire(pipeline.Value());
    }
}

void RunNegative(Bench& bench) noexcept {
    // 셰이더가 읽는 location 1이 배치에 없다.
    {
        const VertexAttribute attributes[] = {{0, VertexFormat::Float2, 0, 0}};
        const VertexStream streams[] = {{8, VertexStepRate::Vertex}};
        auto pipeline =
            bench.Pipeline(Text(FloatHlsl), Text(FloatVertexGlsl), {attributes, streams});
        Check(pipeline.Error() == ErrorCode::InvalidArgument &&
                  bench.device->LastError().ByteCount() != 0,
              "missing shader input location is rejected with a diagnostic");
    }

    // int 입력에 정규화 형식을 넣는다.
    {
        const VertexAttribute attributes[] = {{0, VertexFormat::Float2, 0, 0},
                                              {1, VertexFormat::UByte4Unorm, 0, 8}};
        const VertexStream streams[] = {{12, VertexStepRate::Vertex}};
        auto pipeline = bench.Pipeline(Text(IntHlsl), Text(IntVertexGlsl), {attributes, streams});
        Check(pipeline.Error() == ErrorCode::InvalidArgument,
              "normalized format feeding an int input is rejected");
    }

    // 정수 형식을 float 입력에 넣는다.
    {
        const VertexAttribute attributes[] = {{0, VertexFormat::Float2, 0, 0},
                                              {1, VertexFormat::UShort4, 0, 8}};
        const VertexStream streams[] = {{16, VertexStepRate::Vertex}};
        auto pipeline =
            bench.Pipeline(Text(FloatHlsl), Text(FloatVertexGlsl), {attributes, streams});
        Check(pipeline.Error() == ErrorCode::InvalidArgument,
              "integer format feeding a float input is rejected");
    }

    // 배치 자체 위반(location 중복)은 셰이더 전에 거부한다.
    {
        const VertexAttribute attributes[] = {{0, VertexFormat::Float2, 0, 0},
                                              {0, VertexFormat::Float4, 0, 8}};
        const VertexStream streams[] = {{24, VertexStepRate::Vertex}};
        auto pipeline =
            bench.Pipeline(Text(FloatHlsl), Text(FloatVertexGlsl), {attributes, streams});
        Check(pipeline.Error() == ErrorCode::InvalidArgument,
              "invalid layout is rejected at pipeline creation");
    }
}

} // namespace

uint32 RunVertexChecks(const char* name, Device& device, Allocator& allocator) noexcept {
    failures = 0;
    backendName = name;
    const bool openGl = device.Info().backend == Backend::OpenGL;
    const uint64 validationBefore = device.ValidationErrorCount();
    Bench bench(allocator);
    bench.device = &device;
    TextureDesc targetDesc;
    targetDesc.width = TargetSize;
    targetDesc.height = TargetSize;
    targetDesc.format = PixelFormat::RGBA8Unorm;
    targetDesc.usage = TextureUsage::RenderTarget;
    auto target = device.CreateTexture(targetDesc, {});
    Check(target.IsOk(), "render target creation");
    if (!target) {
        return failures;
    }

    bench.target = target.Value();
    uint32 formatsChecked = 0;
    for (uint32 number = 1; number <= VertexFormatCount; ++number) {
        auto format = VertexFormatFromNumber(number);
        if (format) {
            const uint32 before = failures;
            RunFormat(bench, format.Value(), openGl);
            formatsChecked += failures == before ? 1u : 0u;
        }
    }

    RunStreams(bench);
    RunBufferUpdates(bench);
    RunInstances(bench);
    RunIndexed(bench, IndexFormat::UInt16);
    RunIndexed(bench, IndexFormat::UInt32);
    RunNoInput(bench);
    RunNegative(bench);
    device.Retire(bench.target);
    Check(device.WaitIdle().IsOk() && device.PendingRetireCount() == 0,
          "vertex check resources are reclaimed");
    Check(device.ValidationErrorCount() == validationBefore, "no validation errors");
    printf("  %s vertex layout: %u/%u formats, %u failure%s\n", name, formatsChecked,
           VertexFormatCount, failures, failures == 1 ? "" : "s");
    return failures;
}

} // namespace sandbox
