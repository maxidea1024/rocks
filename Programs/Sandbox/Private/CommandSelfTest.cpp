#include "CommandSelfTest.h"

#include <Core/Containers/Array.h>
#include <Platform/Memory.h>
#include <stdio.h>

namespace sandbox {

namespace {

using namespace rocks;
using namespace rocks::graphics;

// 대상 크기다(8 × 8, 가운데 행·열 4).
constexpr uint32 TargetSize = 8;
constexpr uint32 PixelBytes = 4;
constexpr int ColorTolerance = 2;

uint32 failures = 0;
const char* backendName = "";

void Check(bool condition, const char* what) noexcept {
    if (!condition) {
        ++failures;
        printf("  [FAIL] %s commands: %s\n", backendName, what);
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

// --- 셰이더 ---
//
// 그리기별 상수는 float4 다섯 개(80 byte, FillConstants)로 읽는다. constants[0].x는 깊이 z,
// constants[0].y는 정점 모양(0 화면 덮는 삼각형, 1 화면 덮는 띠 사각형, 2 가운데 행 가로선, 3
// 가운데 점), constants[4]는 색이다.

constexpr char FillHlsl[] = R"(
cbuffer DrawConstants : register(b0) { float4 constants[5]; };
struct VSOutput {
    float4 position : SV_Position;
#ifdef __spirv__
    [[vk::builtin("PointSize")]] float pointSize : PSIZE;
#endif
};
VSOutput VSMain(uint id : SV_VertexID) {
    VSOutput output;
    float mode = constants[0].y;
    float2 p;
    if (mode < 0.5) {
        p = float2((id << 1) & 2, id & 2) * 2.0 - 1.0;
    } else if (mode < 1.5) {
        p = float2((id >> 1) & 1, id & 1) * 2.0 - 1.0;
    } else if (mode < 2.5) {
        p = float2(id == 0 ? -1.0 : 1.0, -0.125);
    } else {
        p = float2(0.125, -0.125);
    }
    output.position = float4(p, constants[0].x, 1.0);
#ifdef __spirv__
    output.pointSize = 1.0;
#endif
    return output;
}
float4 PSMain(VSOutput input) : SV_Target { return constants[4]; }
)";

#define COMMAND_TEST_GLSL_HEAD "#version 320 es\nprecision highp float;\nprecision highp int;\n"

constexpr char FillVertexGlsl[] = COMMAND_TEST_GLSL_HEAD R"(
uniform highp vec4 vu_h[5];
void main() {
    int id = gl_VertexID;
    float mode = vu_h[0].y;
    vec2 p;
    if (mode < 0.5) {
        p = vec2((id << 1) & 2, id & 2) * 2.0 - 1.0;
    } else if (mode < 1.5) {
        p = vec2((id >> 1) & 1, id & 1) * 2.0 - 1.0;
    } else if (mode < 2.5) {
        p = vec2(id == 0 ? -1.0 : 1.0, -0.125);
    } else {
        p = vec2(0.125, -0.125);
    }
    gl_Position = vec4(p, vu_h[0].x, 1.0);
    gl_PointSize = 1.0;
}
)";

constexpr char FillPixelGlsl[] = COMMAND_TEST_GLSL_HEAD R"(
uniform highp vec4 pu_h[5];
layout(location = 0) out vec4 out_var_SV_Target0;
void main() { out_var_SV_Target0 = pu_h[4]; }
)";

// compute가 구조화 UAV(u0)와 typed UAV(u1)에 쓴다.
constexpr char WriterHlsl[] = R"(
cbuffer DrawConstants : register(b0) { float4 constants[5]; };
RWStructuredBuffer<float4> items : register(u0);
RWBuffer<uint> codes : register(u1);
[numthreads(4, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    items[id.x] = float4(constants[4].xy, 0.0, 1.0);
    codes[id.x] = 0x40u + id.x * 16u;
}
)";

// 쓰기 구조화 slot 0은 storage binding 4, 쓰기 typed slot 1은 image unit 1이다.
constexpr char WriterGlsl[] = COMMAND_TEST_GLSL_HEAD R"(
layout(local_size_x = 4) in;
uniform highp vec4 cu_h[5];
layout(std430, binding = 4) buffer type_RWStructuredBuffer_v4float { vec4 data[]; } items;
layout(binding = 1, r32ui) uniform highp writeonly uimageBuffer codes;
void main() {
    uint i = gl_GlobalInvocationID.x;
    items.data[i] = vec4(cu_h[4].xy, 0.0, 1.0);
    imageStore(codes, int(i), uvec4(0x40u + i * 16u));
}
)";

// PS가 구조화 slot 0(t1)을, VS가 typed slot 1(t2)을 읽는다.
constexpr char ReaderHlsl[] = R"(
StructuredBuffer<float4> items : register(t1);
Buffer<uint> codes : register(t2);
struct VSOutput { float4 position : SV_Position; float4 color : COLOR0; };
VSOutput VSMain(uint id : SV_VertexID) {
    VSOutput output;
    output.position = float4(float2((id << 1) & 2, id & 2) * 2.0 - 1.0, 0.5, 1.0);
    output.color = float4(0.0, 0.0, codes.Load(2) / 255.0, 1.0);
    return output;
}
float4 PSMain(VSOutput input) : SV_Target { return float4(items[1].xy, input.color.z, 1.0); }
)";

constexpr char ReaderVertexGlsl[] = COMMAND_TEST_GLSL_HEAD R"(
layout(binding = 18) uniform highp usamplerBuffer codes;
layout(location = 0) out vec4 out_var_COLOR0;
void main() {
    int id = gl_VertexID;
    gl_Position = vec4(vec2((id << 1) & 2, id & 2) * 2.0 - 1.0, 0.5, 1.0);
    out_var_COLOR0 = vec4(0.0, 0.0, float(texelFetch(codes, 2).x) / 255.0, 1.0);
}
)";

constexpr char ReaderPixelGlsl[] = COMMAND_TEST_GLSL_HEAD R"(
layout(std430, binding = 0) readonly buffer type_StructuredBuffer_v4float { vec4 data[]; } items;
layout(location = 0) in vec4 in_var_COLOR0;
layout(location = 0) out vec4 out_var_SV_Target0;
void main() { out_var_SV_Target0 = vec4(items.data[1].xy, in_var_COLOR0.z, 1.0); }
)";

// compute가 간접 인자 세 가지(그리기 0, index 그리기 16, dispatch 36)를 raw UAV에 쓴다.
constexpr char ArgumentsHlsl[] = R"(
RWByteAddressBuffer args : register(u0);
[numthreads(1, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    args.Store4(0, uint4(3, 1, 0, 0));
    args.Store4(16, uint4(3, 1, 0, 0));
    args.Store(32, 0);
    args.Store3(36, uint3(2, 1, 1));
}
)";

constexpr char ArgumentsGlsl[] = COMMAND_TEST_GLSL_HEAD R"(
layout(local_size_x = 1) in;
layout(std430, binding = 4) buffer type_RWByteAddressBuffer { uint data[]; } args;
void main() {
    args.data[0] = 3u; args.data[1] = 1u; args.data[2] = 0u; args.data[3] = 0u;
    args.data[4] = 3u; args.data[5] = 1u; args.data[6] = 0u; args.data[7] = 0u; args.data[8] = 0u;
    args.data[9] = 2u; args.data[10] = 1u; args.data[11] = 1u;
}
)";

// 그룹마다 표시 하나를 raw UAV에 쓴다(간접 dispatch의 그룹 수 확인).
constexpr char MarkerHlsl[] = R"(
RWByteAddressBuffer marks : register(u0);
[numthreads(1, 1, 1)]
void CSMain(uint3 group : SV_GroupID) { marks.Store(group.x * 4, 0xC0DE0000u + group.x); }
)";

constexpr char MarkerGlsl[] = COMMAND_TEST_GLSL_HEAD R"(
layout(local_size_x = 1) in;
layout(std430, binding = 4) buffer type_RWByteAddressBuffer { uint data[]; } marks;
void main() { marks.data[gl_WorkGroupID.x] = 0xC0DE0000u + gl_WorkGroupID.x; }
)";

// 배치 밖 UAV(u5)를 쓴다(음성 대조).
constexpr char OutsideHlsl[] = R"(
RWByteAddressBuffer outside : register(u5);
[numthreads(1, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) { outside.Store(0, 1); }
)";

constexpr char OutsideGlsl[] = COMMAND_TEST_GLSL_HEAD R"(
layout(local_size_x = 1) in;
layout(std430, binding = 9) buffer type_RWByteAddressBuffer { uint data[]; } outside;
void main() { outside.data[0] = 1u; }
)";

// 8개 색 타깃에 쓴다(6은 R32Uint, 7은 RGBA16Float).
constexpr char MrtHlsl[] = R"(
struct VSOutput { float4 position : SV_Position; };
VSOutput VSMain(uint id : SV_VertexID) {
    VSOutput output;
    output.position = float4(float2((id << 1) & 2, id & 2) * 2.0 - 1.0, 0.5, 1.0);
    return output;
}
struct PSOutput {
    float4 c0 : SV_Target0;
    float4 c1 : SV_Target1;
    float4 c2 : SV_Target2;
    float4 c3 : SV_Target3;
    float4 c4 : SV_Target4;
    float4 c5 : SV_Target5;
    uint c6 : SV_Target6;
    float4 c7 : SV_Target7;
};
PSOutput PSMain(VSOutput input) {
    PSOutput output;
    output.c0 = float4(0.0 / 8.0, 0.5, 0.25, 1.0);
    output.c1 = float4(1.0 / 8.0, 0.5, 0.25, 1.0);
    output.c2 = float4(2.0 / 8.0, 0.5, 0.25, 1.0);
    output.c3 = float4(3.0 / 8.0, 0.5, 0.25, 1.0);
    output.c4 = float4(4.0 / 8.0, 0.5, 0.25, 1.0);
    output.c5 = float4(5.0 / 8.0, 0.5, 0.25, 1.0);
    output.c6 = 1234u;
    output.c7 = float4(7.0 / 8.0, 0.5, 0.25, 1.0);
    return output;
}
)";

constexpr char MrtVertexGlsl[] = COMMAND_TEST_GLSL_HEAD R"(
void main() {
    int id = gl_VertexID;
    gl_Position = vec4(vec2((id << 1) & 2, id & 2) * 2.0 - 1.0, 0.5, 1.0);
}
)";

constexpr char MrtPixelGlsl[] = COMMAND_TEST_GLSL_HEAD R"(
layout(location = 0) out vec4 out_var_SV_Target0;
layout(location = 1) out vec4 out_var_SV_Target1;
layout(location = 2) out vec4 out_var_SV_Target2;
layout(location = 3) out vec4 out_var_SV_Target3;
layout(location = 4) out vec4 out_var_SV_Target4;
layout(location = 5) out vec4 out_var_SV_Target5;
layout(location = 6) out highp uint out_var_SV_Target6;
layout(location = 7) out vec4 out_var_SV_Target7;
void main() {
    out_var_SV_Target0 = vec4(0.0 / 8.0, 0.5, 0.25, 1.0);
    out_var_SV_Target1 = vec4(1.0 / 8.0, 0.5, 0.25, 1.0);
    out_var_SV_Target2 = vec4(2.0 / 8.0, 0.5, 0.25, 1.0);
    out_var_SV_Target3 = vec4(3.0 / 8.0, 0.5, 0.25, 1.0);
    out_var_SV_Target4 = vec4(4.0 / 8.0, 0.5, 0.25, 1.0);
    out_var_SV_Target5 = vec4(5.0 / 8.0, 0.5, 0.25, 1.0);
    out_var_SV_Target6 = 1234u;
    out_var_SV_Target7 = vec4(7.0 / 8.0, 0.5, 0.25, 1.0);
}
)";

// 그리기별 상수 16 byte(float4 하나, TintConstants)를 PS만 읽어 화면을 그 색으로 채운다.
constexpr char TintHlsl[] = R"(
cbuffer DrawConstants : register(b0) { float4 tint; };
float4 VSMain(uint id : SV_VertexID) : SV_Position {
    return float4(float2((id << 1) & 2, id & 2) * 2.0 - 1.0, 0.5, 1.0);
}
float4 PSMain() : SV_Target { return tint; }
)";

constexpr char TintVertexGlsl[] = COMMAND_TEST_GLSL_HEAD R"(
void main() {
    int id = gl_VertexID;
    gl_Position = vec4(vec2((id << 1) & 2, id & 2) * 2.0 - 1.0, 0.5, 1.0);
}
)";

constexpr char TintPixelGlsl[] = COMMAND_TEST_GLSL_HEAD R"(
uniform highp vec4 pu_h[1];
layout(location = 0) out vec4 out_var_SV_Target0;
void main() { out_var_SV_Target0 = pu_h[0]; }
)";

#undef COMMAND_TEST_GLSL_HEAD

struct Rgba {
    uint8 r = 0;
    uint8 g = 0;
    uint8 b = 0;
    uint8 a = 0;
};

bool Near(uint8 actual, uint8 expected) noexcept {
    const int difference = static_cast<int>(actual) - static_cast<int>(expected);
    return difference <= ColorTolerance && difference >= -ColorTolerance;
}

// FillHlsl·WriterHlsl의 그리기별 상수다(float4 constants[5]).
struct FillConstants {
    float shape[16] = {};
    float color[4] = {};
};

// TintHlsl의 그리기별 상수다(float4 tint).
struct TintConstants {
    float color[4] = {};
};

FillConstants Constants(float z, float mode, float r, float g, float b, float a) noexcept {
    FillConstants constants;
    constants.shape[0] = z;
    constants.shape[1] = mode;
    constants.color[0] = r;
    constants.color[1] = g;
    constants.color[2] = b;
    constants.color[3] = a;
    return constants;
}

// Fill·Writer 셰이더의 그리기별 상수를 올린다. 거부되면 실패로 센다.
void SetFill(CommandContext& commands, const FillConstants& constants) noexcept {
    Check(commands.SetDrawConstants(DrawConstantBytes(constants)).IsOk(),
          "fill draw constants are accepted");
}

// 화면 덮는 삼각형·띠·선·점 모드다(FillHlsl).
constexpr float FullScreen = 0.0f;
constexpr float StripQuad = 1.0f;
constexpr float MiddleLine = 2.0f;
constexpr float MiddlePoint = 3.0f;

struct Bench {
    Device& device;
    Allocator& allocator;
    TextureHandle target;
    Array<uint8> pixels;

    Bench(Device& owner, Allocator& memory) noexcept
        : device(owner), allocator(memory), pixels(memory) {
    }

    Result<TextureHandle> Texture(PixelFormat format, TextureUsage usage,
                                  Span<const uint8> data = {}) noexcept {
        TextureDesc desc;
        desc.width = TargetSize;
        desc.height = TargetSize;
        desc.format = format;
        desc.usage = usage;
        return device.CreateTexture(desc, data);
    }

    Result<BufferHandle> Buffer(uint64 size, BufferUsage usage, uint32 stride,
                                uint8 fill) noexcept {
        Array<uint8> data(allocator);
        if (!data.TryResize(static_cast<usize>(size))) {
            return Result<BufferHandle>::Failure(ErrorCode::OutOfMemory);
        }

        platform::MemorySet(data.Data(), fill, data.Size());
        BufferDesc desc;
        desc.size = size;
        desc.usage = usage;
        desc.stride = stride;
        return device.CreateBuffer(desc, data.View());
    }

    Result<BufferViewHandle> View(BufferHandle buffer, BufferViewKind kind, ViewAccess access,
                                  PixelFormat format = PixelFormat::Unknown,
                                  uint64 count = 0) noexcept {
        BufferViewDesc desc;
        desc.kind = kind;
        desc.access = access;
        desc.format = format;
        desc.elementCount = count;
        return device.CreateBufferView(buffer, desc);
    }

    Result<PipelineHandle> Pipeline(PipelineDesc desc, StringView hlsl, StringView vertexGlsl,
                                    StringView pixelGlsl) noexcept {
        desc.vertex = {hlsl, vertexGlsl};
        desc.pixel = {hlsl, pixelGlsl};
        // 대상은 선형 RGBA8이다(기본 RGBA8UnormSrgb 대신).
        if (desc.colorTargetCount == 1 && desc.colorFormats[0] == PixelFormat::RGBA8UnormSrgb) {
            desc.colorFormats[0] = PixelFormat::RGBA8Unorm;
        }

        return device.CreatePipeline(desc);
    }

    Result<PipelineHandle> Fill(PipelineDesc desc = {}) noexcept {
        desc.drawConstantBytes = sizeof(FillConstants);
        return Pipeline(desc, Text(FillHlsl), Text(FillVertexGlsl), Text(FillPixelGlsl));
    }

    Result<PipelineHandle> Compute(StringView hlsl, StringView glsl,
                                   uint32 drawConstantBytes = 0) noexcept {
        ComputePipelineDesc desc;
        desc.shader = {hlsl, glsl};
        desc.drawConstantBytes = drawConstantBytes;
        return device.CreateComputePipeline(desc);
    }

    bool Read(TextureHandle texture) noexcept {
        return pixels.TryResize(static_cast<usize>(TargetSize) * TargetSize * PixelBytes).IsOk() &&
               device.ReadPixels(texture, pixels.View()).IsOk();
    }

    bool PixelIs(uint32 x, uint32 y, Rgba expected) const noexcept {
        const usize offset = (static_cast<usize>(y) * TargetSize + x) * PixelBytes;
        const bool same = Near(pixels[offset], expected.r) &&
                          Near(pixels[offset + 1], expected.g) &&
                          Near(pixels[offset + 2], expected.b);
        if (!same) {
            printf("    pixel (%u, %u) = (%u, %u, %u), expected (%u, %u, %u)\n", x, y,
                   pixels[offset], pixels[offset + 1], pixels[offset + 2], expected.r, expected.g,
                   expected.b);
        }

        return same;
    }
};

bool ReadWords(Device& device, Allocator& allocator, BufferHandle buffer, uint32* words,
               usize count) noexcept {
    Array<uint8> bytes(allocator);
    if (!bytes.TryResize(count * sizeof(uint32)) || !device.ReadBuffer(buffer, 0, bytes.View())) {
        return false;
    }

    platform::MemoryCopy(words, bytes.Data(), bytes.Size());
    return true;
}

// compute가 구조화·typed UAV에 쓰고 같은 패스의 VS·PS가 SRV로 읽어 그린다(GX5 UAV → SRV 전이).
void RunComputeWrites(Bench& bench) noexcept {
    Device& device = bench.device;
    auto writer = bench.Compute(Text(WriterHlsl), Text(WriterGlsl), sizeof(FillConstants));
    auto reader =
        bench.Pipeline({}, Text(ReaderHlsl), Text(ReaderVertexGlsl), Text(ReaderPixelGlsl));
    auto items = bench.Buffer(
        64, BufferUsage::ShaderRead | BufferUsage::ShaderWrite | BufferUsage::Structured, 16, 0);
    auto codes = bench.Buffer(16, BufferUsage::ShaderRead | BufferUsage::ShaderWrite, 0, 0);
    Check(writer && reader && items && codes, "compute and reader pipelines and buffers");
    if (!writer || !reader || !items || !codes) {
        PrintLastError(device);
        return;
    }

    auto itemsWrite = bench.View(items.Value(), BufferViewKind::Structured, ViewAccess::ReadWrite);
    auto itemsRead = bench.View(items.Value(), BufferViewKind::Structured, ViewAccess::Read);
    auto codesWrite = bench.View(codes.Value(), BufferViewKind::Typed, ViewAccess::ReadWrite,
                                 PixelFormat::R32Uint);
    auto codesRead =
        bench.View(codes.Value(), BufferViewKind::Typed, ViewAccess::Read, PixelFormat::R32Uint);
    Check(itemsWrite && itemsRead && codesWrite && codesRead, "compute buffer views");
    if (itemsWrite && itemsRead && codesWrite && codesRead) {
        auto frame = device.BeginOffscreen(bench.target, {});
        if (frame) {
            CommandContext& commands = *frame.Value();
            commands.SetPipeline(writer.Value());
            SetFill(commands, Constants(0.0f, FullScreen, 0.25f, 0.75f, 0.0f, 1.0f));
            commands.SetWritableBufferView(0, itemsWrite.Value());
            commands.SetWritableBufferView(1, codesWrite.Value());
            commands.Dispatch(1);
            commands.SetPipeline(reader.Value());
            commands.SetBufferView(0, itemsRead.Value());
            commands.SetBufferView(1, codesRead.Value());
            commands.Draw(3);
        }

        Check(frame && device.EndFrame() && bench.Read(bench.target) &&
                  bench.PixelIs(4, 4, {64, 191, 96, 255}),
              "compute writes to structured and typed UAVs are read by VS and PS");
        float itemValues[16] = {};
        uint32 codeValues[4] = {};
        Array<uint8> bytes(bench.allocator);
        const bool itemsRead64 =
            bytes.TryResize(64).IsOk() && device.ReadBuffer(items.Value(), 0, bytes.View()).IsOk();
        if (itemsRead64) {
            platform::MemoryCopy(itemValues, bytes.Data(), 64);
        }

        bool itemsMatch = itemsRead64;
        for (uint32 index = 0; index < 4 && itemsMatch; ++index) {
            itemsMatch = itemValues[index * 4] == 0.25f && itemValues[index * 4 + 1] == 0.75f &&
                         itemValues[index * 4 + 2] == 0.0f && itemValues[index * 4 + 3] == 1.0f;
        }

        Check(itemsMatch && ReadWords(device, bench.allocator, codes.Value(), codeValues, 4) &&
                  codeValues[0] == 0x40 && codeValues[1] == 0x50 && codeValues[2] == 0x60 &&
                  codeValues[3] == 0x70,
              "compute UAV bytes read back");
    }

    Result<BufferViewHandle>* views[] = {&itemsWrite, &itemsRead, &codesWrite, &codesRead};
    for (Result<BufferViewHandle>* view : views) {
        if (*view) {
            device.Retire(view->Value());
        }
    }

    device.Retire(items.Value());
    device.Retire(codes.Value());
    device.Retire(writer.Value());
    device.Retire(reader.Value());
}

// compute가 쓴 간접 인자로 간접 그리기·index 간접 그리기·간접 dispatch를 한다(GX5 UAV → 간접 인자).
void RunIndirect(Bench& bench) noexcept {
    Device& device = bench.device;
    auto writer = bench.Compute(Text(ArgumentsHlsl), Text(ArgumentsGlsl));
    auto marker = bench.Compute(Text(MarkerHlsl), Text(MarkerGlsl));
    auto fill = bench.Fill();
    auto args =
        bench.Buffer(48, BufferUsage::Indirect | BufferUsage::ShaderWrite | BufferUsage::Raw, 0, 0);
    auto marks = bench.Buffer(16, BufferUsage::ShaderWrite | BufferUsage::Raw, 0, 0);
    auto plain = bench.Buffer(48, BufferUsage::Vertex, 0, 0);
    const uint16 indices[4] = {0, 1, 2, 0};
    BufferDesc indexDesc;
    indexDesc.size = sizeof(indices);
    indexDesc.usage = BufferUsage::Index;
    auto index = device.CreateBuffer(
        indexDesc, Span<const uint8>(reinterpret_cast<const uint8*>(indices), sizeof(indices)));
    Check(writer && marker && fill && args && marks && plain && index,
          "indirect pipelines and buffers");
    if (!writer || !marker || !fill || !args || !marks || !plain || !index) {
        PrintLastError(device);
        return;
    }

    auto argsWrite = bench.View(args.Value(), BufferViewKind::Raw, ViewAccess::ReadWrite);
    auto marksWrite = bench.View(marks.Value(), BufferViewKind::Raw, ViewAccess::ReadWrite);
    Check(argsWrite && marksWrite, "indirect argument views");
    if (argsWrite && marksWrite) {
        // 간접 그리기: 같은 패스에서 compute가 인자를 쓰고 바로 쓴다. Indirect 용도가 없는 버퍼의
        // 간접 그리기는 무시된다.
        const uint64 ignoredBefore = device.IgnoredDraws();
        auto frame = device.BeginOffscreen(bench.target, {});
        if (frame) {
            CommandContext& commands = *frame.Value();
            commands.SetPipeline(writer.Value());
            commands.SetWritableBufferView(0, argsWrite.Value());
            commands.Dispatch(1);
            commands.SetPipeline(fill.Value());
            SetFill(commands, Constants(0.5f, FullScreen, 1.0f, 0.0f, 0.0f, 1.0f));
            commands.DrawIndirect(args.Value(), 0);
            commands.DrawIndirect(plain.Value(), 0);
            commands.DrawIndirect(args.Value(), 40);
        }

        Check(frame && device.EndFrame() && bench.Read(bench.target) &&
                  bench.PixelIs(4, 4, {255, 0, 0, 255}),
              "indirect draw uses compute-written arguments");
        Check(device.IgnoredDraws() - ignoredBefore == 2,
              "indirect draws without the Indirect usage or past the buffer end are ignored");
        frame = device.BeginOffscreen(bench.target, {});
        if (frame) {
            CommandContext& commands = *frame.Value();
            commands.SetPipeline(fill.Value());
            commands.SetIndexBuffer(index.Value(), IndexFormat::UInt16);
            SetFill(commands, Constants(0.5f, FullScreen, 0.0f, 1.0f, 0.0f, 1.0f));
            commands.DrawIndexedIndirect(args.Value(), 16);
        }

        Check(frame && device.EndFrame() && bench.Read(bench.target) &&
                  bench.PixelIs(4, 4, {0, 255, 0, 255}),
              "indexed indirect draw uses compute-written arguments");
        // 간접 dispatch는 attachment 없는 패스(compute 전용)에서 한다.
        frame = device.BeginPass(PassDesc{});
        if (frame) {
            CommandContext& commands = *frame.Value();
            commands.SetPipeline(marker.Value());
            commands.SetWritableBufferView(0, marksWrite.Value());
            commands.DispatchIndirect(args.Value(), 36);
        }

        uint32 words[4] = {};
        Check(frame && device.EndFrame() &&
                  ReadWords(device, bench.allocator, marks.Value(), words, 4) &&
                  words[0] == 0xC0DE0000u && words[1] == 0xC0DE0001u && words[2] == 0 &&
                  words[3] == 0,
              "indirect dispatch runs the compute-written group count");
    }

    if (argsWrite) {
        device.Retire(argsWrite.Value());
    }

    if (marksWrite) {
        device.Retire(marksWrite.Value());
    }

    device.Retire(args.Value());
    device.Retire(marks.Value());
    device.Retire(plain.Value());
    device.Retire(index.Value());
    device.Retire(writer.Value());
    device.Retire(marker.Value());
    device.Retire(fill.Value());
}

// MRT 8: 타깃별 clear 값, 블렌드, 쓰기 mask, 정수·half 형식 타깃을 읽어 대조한다.
void RunMultipleTargets(Bench& bench) noexcept {
    Device& device = bench.device;
    PassDesc tooMany;
    tooMany.colorCount = MaxColorTargets + 1;
    Check(device.BeginPass(tooMany).Error() == ErrorCode::InvalidArgument,
          "a pass with more than MaxColorTargets attachments is rejected");
    if (device.Caps().limits.colorAttachments < MaxColorTargets) {
        printf("  [NOT RUN] %s commands: MRT 8 (device has %u color attachments)\n", backendName,
               device.Caps().limits.colorAttachments);
        return;
    }

    constexpr PixelFormat Formats[MaxColorTargets] = {
        PixelFormat::RGBA8Unorm, PixelFormat::RGBA8Unorm, PixelFormat::RGBA8Unorm,
        PixelFormat::RGBA8Unorm, PixelFormat::RGBA8Unorm, PixelFormat::RGBA8Unorm,
        PixelFormat::R32Uint,    PixelFormat::RGBA16Float};
    TextureHandle targets[MaxColorTargets];
    bool created = true;
    for (uint32 index = 0; index < MaxColorTargets; ++index) {
        auto texture = bench.Texture(Formats[index], TextureUsage::RenderTarget);
        created = created && texture.IsOk();
        targets[index] = texture ? texture.Value() : TextureHandle{};
    }

    PipelineDesc desc;
    desc.colorTargetCount = MaxColorTargets;
    for (uint32 index = 0; index < MaxColorTargets; ++index) {
        desc.colorFormats[index] = Formats[index];
    }

    // 타깃 1은 쓰지 않고(clear 값 유지), 2는 더하기 블렌드, 3은 R·A만 쓴다.
    desc.blend[1].writeMask = ColorWriteMask::None;
    desc.blend[2].enable = true;
    desc.blend[2].sourceColor = BlendFactor::One;
    desc.blend[2].destinationColor = BlendFactor::One;
    desc.blend[2].sourceAlpha = BlendFactor::One;
    desc.blend[2].destinationAlpha = BlendFactor::One;
    desc.blend[3].writeMask = ColorWriteMask::Red | ColorWriteMask::Alpha;
    auto pipeline = bench.Pipeline(desc, Text(MrtHlsl), Text(MrtVertexGlsl), Text(MrtPixelGlsl));
    Check(created && pipeline.IsOk(), "MRT 8 targets and pipeline");
    if (!created || !pipeline) {
        PrintLastError(device);
    }

    if (created && pipeline) {
        PassDesc pass;
        pass.colorCount = MaxColorTargets;
        for (uint32 index = 0; index < MaxColorTargets; ++index) {
            pass.colors[index].texture = targets[index];
            pass.colors[index].clear = {0.1f * static_cast<float>(index), 0.2f, 0.3f, 1.0f};
        }

        pass.colors[6].clear = {7.0f, 0.0f, 0.0f, 0.0f};
        auto frame = device.BeginPass(pass);
        if (frame) {
            frame.Value()->SetPipeline(pipeline.Value());
            frame.Value()->Draw(3);
        }

        Check(frame && device.EndFrame(), "MRT 8 pass records and submits");
        const Rgba expected[6] = {{0, 128, 64, 255}, {26, 51, 77, 255},   {115, 179, 140, 255},
                                  {96, 51, 77, 255}, {128, 128, 64, 255}, {160, 128, 64, 255}};
        Array<uint8> read(bench.allocator);
        const usize center = static_cast<usize>(4) * TargetSize + 4;
        for (uint32 index = 0; index < MaxColorTargets; ++index) {
            const uint32 bytes = index == 7 ? 8u : 4u;
            const bool readable =
                read.TryResize(static_cast<usize>(TargetSize) * TargetSize * bytes).IsOk() &&
                device.ReadTexture(targets[index], 0, 0, read.View()).IsOk();
            bool same = false;
            if (readable && index < 6) {
                const uint8* pixel = read.Data() + center * 4;
                same = Near(pixel[0], expected[index].r) && Near(pixel[1], expected[index].g) &&
                       Near(pixel[2], expected[index].b) && Near(pixel[3], expected[index].a);
                if (!same) {
                    printf("    target %u = (%u, %u, %u, %u)\n", index, pixel[0], pixel[1],
                           pixel[2], pixel[3]);
                }
            } else if (readable && index == 6) {
                uint32 value = 0;
                platform::MemoryCopy(&value, read.Data() + center * 4, 4);
                same = value == 1234u;
            } else if (readable) {
                // half 0.875 = 0x3B00, 0.5 = 0x3800, 0.25 = 0x3400, 1.0 = 0x3C00
                uint16 halves[4] = {};
                platform::MemoryCopy(halves, read.Data() + center * 8, 8);
                same = halves[0] == 0x3B00 && halves[1] == 0x3800 && halves[2] == 0x3400 &&
                       halves[3] == 0x3C00;
            }

            char what[96] = {};
            snprintf(what, sizeof(what), "MRT target %u holds its clear, blend and mask result",
                     index);
            Check(same, what);
        }
    }

    if (pipeline) {
        device.Retire(pipeline.Value());
    }

    for (TextureHandle texture : targets) {
        if (!texture.IsNull()) {
            device.Retire(texture);
        }
    }
}

// 깊이 비교·쓰기, 스텐실 연산·기준 값, 알파 블렌드, scissor 조합과 읽기 전용 깊이 attachment다.
void RunDepthStencil(Bench& bench) noexcept {
    Device& device = bench.device;
    PixelFormat depthFormat = PixelFormat::D32FloatS8Uint;
    if (!HasPixelFormatCaps(device.GetPixelFormatCaps(depthFormat),
                            PixelFormatCaps::DepthStencil)) {
        depthFormat = PixelFormat::D24UnormS8Uint;
    }

    auto depth = bench.Texture(depthFormat, TextureUsage::DepthStencil);
    PipelineDesc base;
    base.depthFormat = depthFormat;
    // a: 깊이 GreaterEqual·쓰기, 스텐실 Always → Replace(기준 1)
    PipelineDesc first = base;
    first.depthStencil.depthTest = true;
    first.depthStencil.depthWrite = true;
    first.depthStencil.stencilTest = true;
    first.depthStencil.front.pass = StencilOperation::Replace;
    first.depthStencil.back.pass = StencilOperation::Replace;
    // b: 깊이 GreaterEqual·쓰기, 스텐실 Equal(기준 0)·쓰지 않음
    PipelineDesc second = base;
    second.depthStencil.depthTest = true;
    second.depthStencil.depthWrite = true;
    second.depthStencil.stencilTest = true;
    second.depthStencil.front.compare = CompareFunction::Equal;
    second.depthStencil.back.compare = CompareFunction::Equal;
    // c: 알파 블렌드, 깊이 Equal·쓰지 않음
    PipelineDesc third = base;
    third.blend[0] = AlphaBlend();
    third.depthStencil.depthTest = true;
    third.depthStencil.depthCompare = CompareFunction::Equal;
    // 읽기 전용 패스용: 시험만(ro), 쓰기(rw, 무시돼야 한다)
    PipelineDesc readOnly = base;
    readOnly.depthStencil.depthTest = true;
    PipelineDesc writing = readOnly;
    writing.depthStencil.depthWrite = true;
    auto pipelineA = bench.Fill(first);
    auto pipelineB = bench.Fill(second);
    auto pipelineC = bench.Fill(third);
    auto pipelineRo = bench.Fill(readOnly);
    auto pipelineRw = bench.Fill(writing);
    Check(depth && pipelineA && pipelineB && pipelineC && pipelineRo && pipelineRw,
          "depth target and depth stencil pipelines");
    if (!depth || !pipelineA || !pipelineB || !pipelineC || !pipelineRo || !pipelineRw) {
        PrintLastError(device);
    }

    if (depth && pipelineA && pipelineB && pipelineC && pipelineRo && pipelineRw) {
        PassDesc pass;
        pass.colorCount = 1;
        pass.colors[0].texture = bench.target;
        pass.depth.texture = depth.Value();
        auto frame = device.BeginPass(pass);
        if (frame) {
            CommandContext& commands = *frame.Value();
            commands.SetPipeline(pipelineA.Value());
            commands.SetStencilReference(1);
            commands.SetScissor({0, 0, TargetSize / 2, TargetSize});
            SetFill(commands, Constants(0.5f, FullScreen, 1.0f, 0.0f, 0.0f, 1.0f));
            commands.Draw(3);
            commands.SetScissor({0, 0, TargetSize, TargetSize});
            commands.SetPipeline(pipelineB.Value());
            commands.SetStencilReference(0);
            SetFill(commands, Constants(0.25f, FullScreen, 0.0f, 1.0f, 0.0f, 1.0f));
            commands.Draw(3);
            commands.SetPipeline(pipelineC.Value());
            SetFill(commands, Constants(0.25f, FullScreen, 0.0f, 0.0f, 1.0f, 0.5f));
            commands.Draw(3);
        }

        // 왼쪽: a만 통과(빨강). 오른쪽: b가 스텐실 0·깊이 0.25 ≥ 0으로 통과(초록), c가 깊이 Equal로
        // 통과해 파랑 반을 섞는다.
        Check(frame && device.EndFrame() && bench.Read(bench.target) &&
                  bench.PixelIs(1, 4, {255, 0, 0, 255}) && bench.PixelIs(6, 4, {0, 128, 128, 255}),
              "depth compare, depth write, stencil replace and equal, scissor and alpha blend");
        pass.colors[0].load = LoadAction::Load;
        pass.depth.depthLoad = LoadAction::Load;
        pass.depth.stencilLoad = LoadAction::Load;
        pass.depth.readOnly = true;
        const uint64 ignoredBefore = device.IgnoredDraws();
        frame = device.BeginPass(pass);
        if (frame) {
            CommandContext& commands = *frame.Value();
            commands.SetPipeline(pipelineRw.Value());
            SetFill(commands, Constants(0.25f, FullScreen, 0.0f, 0.0f, 1.0f, 1.0f));
            commands.Draw(3);
            commands.SetPipeline(pipelineRo.Value());
            SetFill(commands, Constants(0.25f, FullScreen, 1.0f, 1.0f, 0.0f, 1.0f));
            commands.Draw(3);
        }

        Check(frame && device.EndFrame() && bench.Read(bench.target) &&
                  bench.PixelIs(1, 4, {255, 0, 0, 255}) && bench.PixelIs(6, 4, {255, 255, 0, 255}),
              "read-only depth attachment keeps depth and tests against it");
        Check(device.IgnoredDraws() - ignoredBefore == 1,
              "draws that write depth into a read-only depth attachment are ignored");
    }

    Result<PipelineHandle>* pipelines[] = {&pipelineA, &pipelineB, &pipelineC, &pipelineRo,
                                           &pipelineRw};
    for (Result<PipelineHandle>* pipeline : pipelines) {
        if (*pipeline) {
            device.Retire(pipeline->Value());
        }
    }

    if (depth) {
        device.Retire(depth.Value());
    }
}

// 기본도형 종류(띠·선·점)와 viewport다.
void RunTopologyAndViewport(Bench& bench) noexcept {
    Device& device = bench.device;
    PipelineDesc strip;
    strip.topology = PrimitiveTopology::TriangleStrip;
    PipelineDesc lines;
    lines.topology = PrimitiveTopology::LineList;
    PipelineDesc points;
    points.topology = PrimitiveTopology::PointList;
    auto stripPipeline = bench.Fill(strip);
    auto linePipeline = bench.Fill(lines);
    auto pointPipeline = bench.Fill(points);
    auto fill = bench.Fill();
    Check(stripPipeline && linePipeline && pointPipeline && fill, "topology pipelines");
    if (!stripPipeline || !linePipeline || !pointPipeline || !fill) {
        PrintLastError(device);
    }

    if (stripPipeline && linePipeline && pointPipeline && fill) {
        const auto draw = [&](PipelineHandle pipeline, float mode, uint32 count) noexcept {
            auto frame = device.BeginOffscreen(bench.target, {});
            if (frame) {
                frame.Value()->SetPipeline(pipeline);
                SetFill(*frame.Value(), Constants(0.5f, mode, 1.0f, 1.0f, 1.0f, 1.0f));
                frame.Value()->Draw(count);
            }

            return frame && device.EndFrame() && bench.Read(bench.target);
        };
        constexpr Rgba White = {255, 255, 255, 255};
        constexpr Rgba Black = {0, 0, 0, 255};
        // 띠는 정점 5개로 그린다. 다섯째 정점은 퇴화가 아니라 오른쪽 아래 반을 한 번 더 덮는
        // 삼각형이며, llvmpipe(mesa 26.2.7) linear 경로를 피하려고 둔다. 화면 정렬 사각형만 있는
        // bin에서 llvmpipe가 셰이더를 AoS u8로 컴파일하면 load_ubo의 정수 offset이 상수
        // 변환(float_to_ubyte)으로 0이 되어 PS가 push constant 첫 vec4를 읽는다
        // (LP_PERF=no_rast_linear로 사라짐, 2026-11-18 확인, mesa 버그).
        Check(draw(stripPipeline.Value(), StripQuad, 5) && bench.PixelIs(0, 0, White) &&
                  bench.PixelIs(7, 7, White),
              "triangle strip covers the target");
        Check(draw(linePipeline.Value(), MiddleLine, 2) && bench.PixelIs(4, 4, White) &&
                  bench.PixelIs(4, 1, Black),
              "line list draws the middle row");
        Check(draw(pointPipeline.Value(), MiddlePoint, 1) && bench.PixelIs(4, 4, White) &&
                  bench.PixelIs(3, 3, Black),
              "point list draws the middle pixel");
        auto frame = device.BeginOffscreen(bench.target, {});
        if (frame) {
            frame.Value()->SetPipeline(fill.Value());
            frame.Value()->SetViewport({0.0f, 0.0f, TargetSize / 2.0f, TargetSize});
            SetFill(*frame.Value(), Constants(0.5f, FullScreen, 1.0f, 1.0f, 1.0f, 1.0f));
            frame.Value()->Draw(3);
        }

        Check(frame && device.EndFrame() && bench.Read(bench.target) &&
                  bench.PixelIs(1, 4, White) && bench.PixelIs(6, 4, Black),
              "viewport limits the draw to the left half");
    }

    Result<PipelineHandle>* pipelines[] = {&stripPipeline, &linePipeline, &pointPipeline, &fill};
    for (Result<PipelineHandle>* pipeline : pipelines) {
        if (*pipeline) {
            device.Retire(pipeline->Value());
        }
    }
}

// 버퍼·텍스처 복사와 UAV clear를 attachment 없는 패스에서 하고 바이트를 대조한다.
void RunCopies(Bench& bench) noexcept {
    Device& device = bench.device;
    Allocator& allocator = bench.allocator;
    Array<uint8> pattern(allocator);
    Check(pattern.TryResize(256).IsOk(), "copy pattern memory");
    for (usize index = 0; index < pattern.Size(); ++index) {
        pattern[index] = static_cast<uint8>(index * 7 + 3);
    }

    BufferDesc sourceDesc;
    sourceDesc.size = 256;
    sourceDesc.usage = BufferUsage::Vertex;
    auto source = device.CreateBuffer(sourceDesc, pattern.View());
    auto destination = bench.Buffer(256, BufferUsage::Vertex, 0, 0);
    auto raw = bench.Buffer(
        64, BufferUsage::ShaderRead | BufferUsage::ShaderWrite | BufferUsage::Raw, 0, 0x11);
    auto typed = bench.Buffer(64, BufferUsage::ShaderRead | BufferUsage::ShaderWrite, 0, 0x22);
    auto structured = bench.Buffer(
        64, BufferUsage::ShaderRead | BufferUsage::ShaderWrite | BufferUsage::Structured, 16, 0x33);
    auto floats = bench.Buffer(64, BufferUsage::ShaderRead | BufferUsage::ShaderWrite, 0, 0);
    Array<uint8> image(allocator);
    Check(image.TryResize(static_cast<usize>(TargetSize) * TargetSize * 4).IsOk(),
          "copy image memory");
    for (usize index = 0; index < image.Size(); ++index) {
        image[index] = static_cast<uint8>(index * 13 + 5);
    }

    auto textureSource =
        bench.Texture(PixelFormat::RGBA8Unorm, TextureUsage::Sampled, image.View());
    platform::MemorySet(image.Data(), 0, image.Size());
    auto textureDestination =
        bench.Texture(PixelFormat::RGBA8Unorm, TextureUsage::Sampled, image.View());
    TextureDesc smallDesc;
    smallDesc.width = TargetSize / 2;
    smallDesc.height = TargetSize / 2;
    smallDesc.format = PixelFormat::RGBA8Unorm;
    smallDesc.usage = TextureUsage::RenderTarget;
    auto small = device.CreateTexture(smallDesc, {});
    auto history = bench.Texture(PixelFormat::RGBA8Unorm, TextureUsage::RenderTarget);
    Check(source && destination && raw && typed && structured && floats && textureSource &&
              textureDestination && small && history,
          "copy buffers and textures");
    if (!source || !destination || !raw || !typed || !structured || !floats || !textureSource ||
        !textureDestination || !small || !history) {
        return;
    }

    auto rawView = bench.View(raw.Value(), BufferViewKind::Raw, ViewAccess::ReadWrite,
                              PixelFormat::Unknown, 8);
    auto typedView = bench.View(typed.Value(), BufferViewKind::Typed, ViewAccess::ReadWrite,
                                PixelFormat::R32Uint, 4);
    auto structuredView = bench.View(structured.Value(), BufferViewKind::Structured,
                                     ViewAccess::ReadWrite, PixelFormat::Unknown, 2);
    auto readView =
        bench.View(typed.Value(), BufferViewKind::Typed, ViewAccess::Read, PixelFormat::R32Uint, 4);
    auto floatView = bench.View(floats.Value(), BufferViewKind::Typed, ViewAccess::ReadWrite,
                                PixelFormat::RGBA32Float);
    const bool floatWritable = HasPixelFormatCaps(
        device.GetPixelFormatCaps(PixelFormat::RGBA32Float), PixelFormatCaps::TexelBufferWrite);
    Check(rawView && typedView && structuredView && readView && (floatView || !floatWritable),
          "clear views");

    // history 대상: 화면을 그린 RenderTarget을 다른 RenderTarget에 복사한다.
    auto fill = bench.Fill();
    auto frame = device.BeginOffscreen(bench.target, {});
    if (frame && fill) {
        frame.Value()->SetPipeline(fill.Value());
        SetFill(*frame.Value(), Constants(0.5f, FullScreen, 0.0f, 0.0f, 1.0f, 1.0f));
        frame.Value()->Draw(3);
    }

    Check(frame && device.EndFrame(), "history source frame");
    const uint64 ignoredBefore = device.IgnoredDraws();
    frame = device.BeginPass(PassDesc{});
    Check(frame.IsOk(), "a pass without attachments begins");
    if (frame) {
        CommandContext& commands = *frame.Value();
        Check(commands.CopyBuffer(destination.Value(), 64, source.Value(), 32, 128).IsOk(),
              "buffer copy records");
        Check(commands.CopyBuffer(source.Value(), 0, source.Value(), 128, 64).Error() ==
                      ErrorCode::InvalidArgument &&
                  commands.CopyBuffer(destination.Value(), 200, source.Value(), 0, 64).Error() ==
                      ErrorCode::InvalidArgument &&
                  commands.CopyBuffer(destination.Value(), 2, source.Value(), 0, 64).Error() ==
                      ErrorCode::InvalidArgument,
              "buffer copies within one buffer, out of range or unaligned are rejected");
        if (rawView && typedView && structuredView && readView) {
            Check(commands.ClearWritableBufferView(rawView.Value(), 0xDEADBEEFu).IsOk() &&
                      commands.ClearWritableBufferView(typedView.Value(), 7).IsOk(),
                  "UAV clears of raw and R32Uint typed views record");
            Check(commands.ClearWritableBufferView(readView.Value(), 1).Error() ==
                      ErrorCode::InvalidArgument,
                  "clearing a read view is rejected");
            Check(commands.ClearWritableBufferView(structuredView.Value(), 1).Error() ==
                      ErrorCode::Unsupported,
                  "clearing a structured view is unsupported (D3D12 does not clear them)");
        }

        if (floatView) {
            Check(commands.ClearWritableBufferView(floatView.Value(), 1).Error() ==
                      ErrorCode::Unsupported,
                  "clearing a non 32-bit integer typed view is unsupported");
        }

        Check(commands.CopyTexture(textureDestination.Value(), textureSource.Value()).IsOk() &&
                  commands.CopyTexture(history.Value(), bench.target).IsOk(),
              "texture copies record");
        Check(commands.CopyTexture(textureSource.Value(), textureSource.Value()).Error() ==
                      ErrorCode::InvalidArgument &&
                  commands.CopyTexture(small.Value(), history.Value()).Error() ==
                      ErrorCode::InvalidArgument,
              "texture copies onto itself or of another size are rejected");
        // attachment 없는 패스의 그리기는 무시된다.
        if (fill) {
            commands.SetPipeline(fill.Value());
            commands.Draw(3);
        }
    }

    Check(frame && device.EndFrame(), "copy pass submits");
    Check(device.IgnoredDraws() - ignoredBefore == 1,
          "a draw in a pass without attachments is ignored");
    Array<uint8> read(allocator);
    bool copied =
        read.TryResize(256).IsOk() && device.ReadBuffer(destination.Value(), 0, read.View()).IsOk();
    for (usize index = 0; copied && index < 256; ++index) {
        const uint8 expected = index >= 64 && index < 192 ? pattern[index - 32] : 0;
        copied = read[index] == expected;
    }

    Check(copied, "buffer copy bytes match");
    const auto words = [&](BufferHandle buffer, uint32 value, usize cleared, uint8 rest) noexcept {
        Array<uint8> bytes(allocator);
        if (!bytes.TryResize(64) || !device.ReadBuffer(buffer, 0, bytes.View())) {
            return false;
        }

        for (usize index = 0; index < 64; ++index) {
            const uint8 expected =
                index < cleared ? static_cast<uint8>(value >> ((index % 4) * 8)) : rest;
            if (bytes[index] != expected) {
                printf("    byte %zu = 0x%02X, expected 0x%02X\n", index, bytes[index], expected);
                return false;
            }
        }

        return true;
    };
    Check(words(raw.Value(), 0xDEADBEEFu, 32, 0x11), "raw UAV clear fills only the view range");
    Check(words(typed.Value(), 7, 16, 0x22), "typed UAV clear fills only the view range");
    Check(words(structured.Value(), 0x33333333u, 0, 0x33),
          "a rejected structured UAV clear leaves the buffer unchanged");
    Array<uint8> pixels(allocator);
    bool textureCopied = pixels.TryResize(image.Size()).IsOk() &&
                         device.ReadTexture(textureDestination.Value(), 0, 0, pixels.View()).IsOk();
    for (usize index = 0; textureCopied && index < pixels.Size(); ++index) {
        textureCopied = pixels[index] == static_cast<uint8>(index * 13 + 5);
    }

    Check(textureCopied, "texture copy bytes match");
    Check(bench.Read(history.Value()) && bench.PixelIs(4, 4, {0, 0, 255, 255}),
          "render target copy keeps the drawn frame (history)");
    Result<BufferViewHandle>* views[] = {&rawView, &typedView, &structuredView, &readView,
                                         &floatView};
    for (Result<BufferViewHandle>* view : views) {
        if (*view) {
            device.Retire(view->Value());
        }
    }

    const BufferHandle buffers[] = {source.Value(), destination.Value(), raw.Value(),
                                    typed.Value(),  structured.Value(),  floats.Value()};
    for (BufferHandle buffer : buffers) {
        device.Retire(buffer);
    }

    const TextureHandle textures[] = {textureSource.Value(), textureDestination.Value(),
                                      small.Value(), history.Value()};
    for (TextureHandle texture : textures) {
        device.Retire(texture);
    }

    if (fill) {
        device.Retire(fill.Value());
    }
}

// 그리기별 상수(RenderBase 결정 R7, SX2): 크기 규칙·셰이더 대조는 생성 때, 크기 불일치는
// SetDrawConstants에서 거부한다. 값은 기록마다 0으로 시작하고 SetPipeline으로 바뀌지 않으며, 크기가
// 다른 파이프라인은 같은 값의 앞부분을 읽는다.
void RunDrawConstants(Bench& bench) noexcept {
    Device& device = bench.device;
    const auto tintPipeline = [&](uint32 bytes) noexcept {
        PipelineDesc desc;
        desc.drawConstantBytes = bytes;
        return bench.Pipeline(desc, Text(TintHlsl), Text(TintVertexGlsl), Text(TintPixelGlsl));
    };
    auto tooLarge = tintPipeline(MaxDrawConstantBytes + DrawConstantAlignment);
    auto unaligned = tintPipeline(DrawConstantAlignment + 8);
    auto computeTooLarge = bench.Compute(Text(WriterHlsl), Text(WriterGlsl),
                                         MaxDrawConstantBytes + DrawConstantAlignment);
    Check(tooLarge.Error() == ErrorCode::InvalidArgument &&
              unaligned.Error() == ErrorCode::InvalidArgument &&
              computeTooLarge.Error() == ErrorCode::InvalidArgument,
          "drawConstantBytes above 128 or not a multiple of 16 is rejected");
    // 셰이더가 읽는 블록이 선언보다 크면 거부한다(80 byte를 읽는 Fill에 64·0, 16 byte Tint에 0).
    PipelineDesc fillSmall;
    fillSmall.drawConstantBytes = 64;
    auto fillShort =
        bench.Pipeline(fillSmall, Text(FillHlsl), Text(FillVertexGlsl), Text(FillPixelGlsl));
    auto fillNone = bench.Pipeline({}, Text(FillHlsl), Text(FillVertexGlsl), Text(FillPixelGlsl));
    auto tintNone = tintPipeline(0);
    auto writerShort = bench.Compute(Text(WriterHlsl), Text(WriterGlsl), DrawConstantAlignment);
    Check(fillShort.Error() == ErrorCode::InvalidArgument &&
              fillNone.Error() == ErrorCode::InvalidArgument &&
              tintNone.Error() == ErrorCode::InvalidArgument &&
              writerShort.Error() == ErrorCode::InvalidArgument &&
              device.LastError().ByteCount() != 0,
          "pipelines declaring fewer draw constant bytes than the shader reads are rejected");
    // 셰이더보다 큰 선언은 받는다.
    auto tintWide = tintPipeline(MaxDrawConstantBytes);
    auto tint = tintPipeline(sizeof(TintConstants));
    auto fill = bench.Fill();
    // 상수를 읽지 않는 0 byte 파이프라인이다.
    auto none = bench.Pipeline({}, Text(ReaderHlsl), Text(ReaderVertexGlsl), Text(ReaderPixelGlsl));
    Check(tintWide && tint && fill && none,
          "draw constant pipelines of 0, 16, 80 and 128 bytes are created");
    if (!tintWide || !tint || !fill || !none) {
        PrintLastError(device);
        return;
    }

    const TintConstants red = {{1.0f, 0.0f, 0.0f, 1.0f}};
    const TintConstants blue = {{0.0f, 0.0f, 1.0f, 1.0f}};
    // 기록 시작 값은 0이다: 상수를 올리지 않은 Tint가 (0, 0, 0, 0)을 그린다(흰색 clear 위).
    auto frame = device.BeginOffscreen(bench.target, ClearColor{1.0f, 1.0f, 1.0f, 1.0f});
    if (frame) {
        CommandContext& commands = *frame.Value();
        Check(commands.SetDrawConstants(DrawConstantBytes(red)).Error() ==
                  ErrorCode::InvalidArgument,
              "SetDrawConstants without a bound pipeline is rejected");
        commands.SetPipeline(tint.Value());
        commands.Draw(3);
    }

    Check(frame && device.EndFrame() && bench.Read(bench.target) &&
              bench.PixelIs(4, 4, {0, 0, 0, 0}),
          "draw constants start as zero in each recording");
    // 크기가 다른 파이프라인을 한 패스에서 번갈아 그린다(세로 띠: x 0–1 Tint 빨강, 2–4 Fill
    // 초록, 5–7 Tint를 다시 묶고 올리지 않음 → Fill이 바꾼 앞 16 byte (0.5, 0, 0, 0)을 읽는다).
    frame = device.BeginOffscreen(bench.target, {});
    if (frame) {
        CommandContext& commands = *frame.Value();
        commands.SetPipeline(none.Value());
        Check(commands.SetDrawConstants({}).IsOk() &&
                  commands.SetDrawConstants(DrawConstantBytes(red)).Error() ==
                      ErrorCode::InvalidArgument,
              "a 0 byte pipeline takes only empty draw constants");
        commands.SetPipeline(tint.Value());
        Check(commands.SetDrawConstants(DrawConstantBytes(red)).IsOk() &&
                  commands.SetDrawConstants({}).Error() == ErrorCode::InvalidArgument &&
                  commands.SetDrawConstants(
                              DrawConstantBytes(Constants(0.5f, FullScreen, 0, 0, 1, 1)))
                          .Error() == ErrorCode::InvalidArgument,
              "draw constants of another size than the bound pipeline are rejected");
        commands.SetScissor({0, 0, 2, TargetSize});
        commands.Draw(3);
        commands.SetPipeline(fill.Value());
        Check(commands.SetDrawConstants(DrawConstantBytes(blue)).Error() ==
                  ErrorCode::InvalidArgument,
              "16 byte draw constants are rejected by an 80 byte pipeline");
        SetFill(commands, Constants(0.5f, FullScreen, 0.0f, 1.0f, 0.0f, 1.0f));
        commands.SetScissor({2, 0, 3, TargetSize});
        commands.Draw(3);
        commands.SetPipeline(tint.Value());
        commands.SetScissor({5, 0, 3, TargetSize});
        commands.Draw(3);
    }

    Check(frame && device.EndFrame() && bench.Read(bench.target) &&
              bench.PixelIs(1, 4, {255, 0, 0, 255}) && bench.PixelIs(3, 4, {0, 255, 0, 255}) &&
              bench.PixelIs(6, 4, {128, 0, 0, 0}),
          "16 and 80 byte pipelines in one pass read the shared draw constants");
    // 128 byte 선언 파이프라인은 128 byte를 받고, 셰이더는 그 앞 16 byte를 읽는다.
    frame = device.BeginOffscreen(bench.target, {});
    if (frame) {
        CommandContext& commands = *frame.Value();
        commands.SetPipeline(tintWide.Value());
        struct WideConstants {
            float values[MaxDrawConstantBytes / sizeof(float)] = {};
        } wide;
        wide.values[2] = 1.0f;
        wide.values[3] = 1.0f;
        Check(commands.SetDrawConstants(DrawConstantBytes(wide)).IsOk(),
              "128 byte draw constants are accepted");
        commands.Draw(3);
    }

    Check(frame && device.EndFrame() && bench.Read(bench.target) &&
              bench.PixelIs(4, 4, {0, 0, 255, 255}),
          "a 128 byte pipeline reads the front of its draw constants");
    device.Retire(tintWide.Value());
    device.Retire(tint.Value());
    device.Retire(fill.Value());
    device.Retire(none.Value());
}

// 능력·규칙 위반은 다른 것으로 바꾸지 않고 명시 실패한다.
void RunRejections(Bench& bench) noexcept {
    Device& device = bench.device;
    const DeviceFeatures& features = device.Caps().features;
    auto outside = bench.Compute(Text(OutsideHlsl), Text(OutsideGlsl));
    Check(outside.Error() == ErrorCode::InvalidArgument,
          "compute resources outside the fixed layout are rejected");
    PipelineDesc tooMany;
    tooMany.colorTargetCount = MaxColorTargets + 1;
    PipelineDesc depthWithoutTarget;
    depthWithoutTarget.depthStencil.depthTest = true;
    PipelineDesc stencilWithoutPlane;
    stencilWithoutPlane.depthFormat = PixelFormat::D32Float;
    stencilWithoutPlane.depthStencil.stencilTest = true;
    Check(bench.Fill(tooMany).Error() == ErrorCode::InvalidArgument &&
              bench.Fill(depthWithoutTarget).Error() == ErrorCode::InvalidArgument &&
              bench.Fill(stencilWithoutPlane).Error() == ErrorCode::InvalidArgument,
          "pipeline state rule violations are InvalidArgument");
    // 깊이 bias 상한·깊이 clamp는 능력이 없으면 Unsupported, 있으면 만든다.
    PipelineDesc biasClamp;
    biasClamp.raster.depthBias = 4;
    biasClamp.raster.depthBiasClamp = 0.5f;
    PipelineDesc depthClamp;
    depthClamp.raster.depthClamp = true;
    auto clamped = bench.Fill(biasClamp);
    auto unclipped = bench.Fill(depthClamp);
    Check(features.depthBiasClamp ? clamped.IsOk() : clamped.Error() == ErrorCode::Unsupported,
          "depth bias clamp follows DeviceFeatures::depthBiasClamp");
    Check(features.depthClamp ? unclipped.IsOk() : unclipped.Error() == ErrorCode::Unsupported,
          "depth clamp follows DeviceFeatures::depthClamp");
    PipelineDesc alphaToCoverage;
    alphaToCoverage.alphaToCoverage = true;
    auto coverage = bench.Fill(alphaToCoverage);
    Check(features.alphaToCoverage ? coverage.IsOk() : coverage.Error() == ErrorCode::Unsupported,
          "alpha to coverage follows DeviceFeatures::alphaToCoverage");
    Result<PipelineHandle>* pipelines[] = {&clamped, &unclipped, &coverage};
    for (Result<PipelineHandle>* pipeline : pipelines) {
        if (*pipeline) {
            device.Retire(pipeline->Value());
        }
    }

    // 패스 attachment: 크기가 다른 attachment, 색 자리의 깊이 텍스처는 InvalidArgument다.
    TextureDesc smallDesc;
    smallDesc.width = TargetSize / 2;
    smallDesc.height = TargetSize / 2;
    smallDesc.format = PixelFormat::RGBA8Unorm;
    smallDesc.usage = TextureUsage::RenderTarget;
    auto small = device.CreateTexture(smallDesc, {});
    auto depth = bench.Texture(PixelFormat::D32Float, TextureUsage::DepthStencil);
    if (small && depth) {
        PassDesc mismatched;
        mismatched.colorCount = 2;
        mismatched.colors[0].texture = bench.target;
        mismatched.colors[1].texture = small.Value();
        PassDesc depthAsColor;
        depthAsColor.colorCount = 1;
        depthAsColor.colors[0].texture = depth.Value();
        PassDesc twice;
        twice.colorCount = 2;
        twice.colors[0].texture = bench.target;
        twice.colors[1].texture = bench.target;
        Check(device.BeginPass(mismatched).Error() == ErrorCode::InvalidArgument &&
                  device.BeginPass(depthAsColor).Error() == ErrorCode::InvalidArgument &&
                  device.BeginPass(twice).Error() == ErrorCode::InvalidArgument,
              "pass attachments of different sizes, wrong usage or listed twice are rejected");
    }

    if (small) {
        device.Retire(small.Value());
    }

    if (depth) {
        device.Retire(depth.Value());
    }

    // 파이프라인 종류가 맞지 않는 그리기·dispatch와 그룹 수 한도 밖 dispatch는 무시된다.
    auto fill = bench.Fill();
    auto marker = bench.Compute(Text(MarkerHlsl), Text(MarkerGlsl));
    if (fill && marker) {
        const uint64 ignoredBefore = device.IgnoredDraws();
        auto frame = device.BeginOffscreen(bench.target, {});
        if (frame) {
            CommandContext& commands = *frame.Value();
            commands.SetPipeline(fill.Value());
            commands.Dispatch(1);
            commands.SetPipeline(marker.Value());
            commands.Draw(3);
            const uint32 groups = device.Caps().limits.computeGroupCount[0];
            if (groups != NumericLimits<uint32>::Max()) {
                commands.Dispatch(groups + 1);
            } else {
                commands.Dispatch(1);
            }
        }

        Check(frame && device.EndFrame() && device.IgnoredDraws() - ignoredBefore == 3,
              "dispatches of graphics pipelines, draws of compute pipelines and oversized or "
              "unbound dispatches are ignored");
    }

    if (fill) {
        device.Retire(fill.Value());
    }

    if (marker) {
        device.Retire(marker.Value());
    }
}

} // namespace

uint32 RunCommandChecks(const char* name, Device& device, Allocator& allocator) noexcept {
    failures = 0;
    backendName = name;
    const uint64 validationBefore = device.ValidationErrorCount();
    Bench bench(device, allocator);
    auto target = bench.Texture(PixelFormat::RGBA8Unorm, TextureUsage::RenderTarget);
    Check(target.IsOk(), "render target");
    if (!target) {
        return failures;
    }

    bench.target = target.Value();
    RunComputeWrites(bench);
    RunIndirect(bench);
    RunMultipleTargets(bench);
    RunDepthStencil(bench);
    RunTopologyAndViewport(bench);
    RunCopies(bench);
    RunDrawConstants(bench);
    RunRejections(bench);
    device.Retire(bench.target);
    auto idle = device.WaitIdle();
    Check(idle.IsOk(), "wait idle");
    Check(device.ValidationErrorCount() == validationBefore, "no validation errors");
    printf("  %s commands: %u failures\n", name, failures);
    return failures;
}

} // namespace sandbox
