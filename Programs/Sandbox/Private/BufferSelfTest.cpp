#include "BufferSelfTest.h"

#include <Core/Containers/Array.h>
#include <Core/Diagnostics/Log.h>
#include <Graphics/GraphicsLog.h>
#include <Platform/Memory.h>
#include <stdio.h>

namespace sandbox {

namespace {

using namespace rocks;
using namespace rocks::graphics;

// offscreen 대상 크기다(선형 RGBA8).
constexpr uint32 TargetSize = 8;
constexpr int ColorTolerance = 2;
// typed view 능력 순회에 쓰는 버퍼 크기다(원소 1·2·4·8·12·16 byte의 공배수).
constexpr uint64 FormatProbeBytes = 768;

uint32 failures = 0;
const char* backendName = "";

void Check(bool condition, const char* what) noexcept {
    if (!condition) {
        ++failures;
        printf("  [FAIL] %s buffer: %s\n", backendName, what);
    }
}

StringView Text(const char* text, usize bytes) noexcept {
    return StringView(text, bytes - 1);
}

template <usize Count>
StringView Text(const char (&text)[Count]) noexcept {
    return Text(text, Count);
}

// --- 셰이더(PipelineDesc 고정 배치: buffer view slot n = HLSL t(n + 1)) ---

// 정점 번호로 화면을 덮는 삼각형을 만드는 공통 VS 머리다.
#define BUFFER_TEST_VS_OUTPUT                                                                      \
    "struct VSOutput { float4 position : SV_Position; float4 color : COLOR0; };\n"                 \
    "float4 FullScreen(uint id) {\n"                                                               \
    "    float2 uv = float2((id << 1) & 2, id & 2);\n"                                             \
    "    return float4(uv * 2.0 - 1.0, 0.5, 1.0);\n"                                               \
    "}\n"

// slot 0 typed float SRV를 PS가 읽는다.
constexpr char TypedPixelHlsl[] = BUFFER_TEST_VS_OUTPUT R"(
Buffer<float4> lights : register(t1);
VSOutput VSMain(uint id : SV_VertexID) {
    VSOutput output;
    output.position = FullScreen(id);
    output.color = float4(0.0, 0.0, 0.0, 1.0);
    return output;
}
float4 PSMain(VSOutput input) : SV_Target { return lights.Load(0); }
)";

// slot 1 typed uint SRV를 VS가 읽는다.
constexpr char TypedVertexHlsl[] = BUFFER_TEST_VS_OUTPUT R"(
Buffer<uint> codes : register(t2);
VSOutput VSMain(uint id : SV_VertexID) {
    VSOutput output;
    output.position = FullScreen(id);
    uint v = codes.Load(0);
    output.color = float4(v & 255, (v >> 8) & 255, (v >> 16) & 255, 255) / 255.0;
    return output;
}
float4 PSMain(VSOutput input) : SV_Target { return input.color; }
)";

// slot 2 구조화 SRV를 PS가 읽는다.
constexpr char StructuredPixelHlsl[] = BUFFER_TEST_VS_OUTPUT R"(
struct Item { float4 color; float4 unused; };
StructuredBuffer<Item> items : register(t3);
VSOutput VSMain(uint id : SV_VertexID) {
    VSOutput output;
    output.position = FullScreen(id);
    output.color = float4(0.0, 0.0, 0.0, 1.0);
    return output;
}
float4 PSMain(VSOutput input) : SV_Target { return items[1].color; }
)";

// slot 3 raw SRV를 VS가 읽는다.
constexpr char RawVertexHlsl[] = BUFFER_TEST_VS_OUTPUT R"(
ByteAddressBuffer words : register(t4);
VSOutput VSMain(uint id : SV_VertexID) {
    VSOutput output;
    output.position = FullScreen(id);
    uint v = words.Load(4);
    output.color = float4(v & 255, (v >> 8) & 255, (v >> 16) & 255, 255) / 255.0;
    return output;
}
float4 PSMain(VSOutput input) : SV_Target { return input.color; }
)";

// 배치 밖 register(t5)를 읽는다(음성 대조).
constexpr char OutsideHlsl[] = BUFFER_TEST_VS_OUTPUT R"(
Buffer<float4> outside : register(t5);
VSOutput VSMain(uint id : SV_VertexID) {
    VSOutput output;
    output.position = FullScreen(id);
    output.color = float4(0.0, 0.0, 0.0, 1.0);
    return output;
}
float4 PSMain(VSOutput input) : SV_Target { return outside.Load(0); }
)";

// PS가 UAV에 쓴다(그래픽 단계 UAV 거부, 음성 대조).
constexpr char PixelUavHlsl[] = BUFFER_TEST_VS_OUTPUT R"(
RWBuffer<float4> output : register(u1);
VSOutput VSMain(uint id : SV_VertexID) {
    VSOutput result;
    result.position = FullScreen(id);
    result.color = float4(0.0, 0.0, 0.0, 1.0);
    return result;
}
float4 PSMain(VSOutput input) : SV_Target {
    output[0] = input.color;
    return input.color;
}
)";

#undef BUFFER_TEST_VS_OUTPUT

#define BUFFER_TEST_GLSL_HEAD "#version 320 es\nprecision highp float;\nprecision highp int;\n"

constexpr char BlackVertexGlsl[] = BUFFER_TEST_GLSL_HEAD R"(
layout(location = 0) out vec4 out_var_COLOR0;
void main() {
    vec2 uv = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(uv * 2.0 - 1.0, 0.5, 1.0);
    out_var_COLOR0 = vec4(0.0, 0.0, 0.0, 1.0);
}
)";

constexpr char ColorPixelGlsl[] = BUFFER_TEST_GLSL_HEAD R"(
layout(location = 0) in vec4 in_var_COLOR0;
layout(location = 0) out vec4 out_var_SV_Target0;
void main() { out_var_SV_Target0 = in_var_COLOR0; }
)";

// PS typed slot 0은 텍스처 유닛 1이다.
constexpr char TypedPixelGlsl[] = BUFFER_TEST_GLSL_HEAD R"(
layout(binding = 1) uniform highp samplerBuffer lights;
layout(location = 0) in vec4 in_var_COLOR0;
layout(location = 0) out vec4 out_var_SV_Target0;
void main() { out_var_SV_Target0 = texelFetch(lights, 0); }
)";

// VS typed slot 1은 텍스처 유닛 17 + 1이다.
constexpr char TypedVertexGlsl[] = BUFFER_TEST_GLSL_HEAD R"(
layout(binding = 18) uniform highp usamplerBuffer codes;
layout(location = 0) out vec4 out_var_COLOR0;
void main() {
    vec2 uv = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(uv * 2.0 - 1.0, 0.5, 1.0);
    uint v = texelFetch(codes, 0).x;
    out_var_COLOR0 = vec4(float(v & 255u), float((v >> 8u) & 255u), float((v >> 16u) & 255u),
                          255.0) / 255.0;
}
)";

// 구조화 slot 2는 storage block binding 2다.
constexpr char StructuredPixelGlsl[] = BUFFER_TEST_GLSL_HEAD R"(
struct Item { vec4 color; vec4 unused; };
layout(std430, binding = 2) readonly buffer type_StructuredBuffer_Item { Item data[]; } items;
layout(location = 0) in vec4 in_var_COLOR0;
layout(location = 0) out vec4 out_var_SV_Target0;
void main() { out_var_SV_Target0 = items.data[1].color; }
)";

// raw slot 3은 storage block binding 3이다(이름 접두사로 raw view를 고른다).
constexpr char RawVertexGlsl[] = BUFFER_TEST_GLSL_HEAD R"(
layout(std430, binding = 3) readonly buffer type_ByteAddressBuffer { uint data[]; } words;
layout(location = 0) out vec4 out_var_COLOR0;
void main() {
    vec2 uv = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(uv * 2.0 - 1.0, 0.5, 1.0);
    uint v = words.data[1];
    out_var_COLOR0 = vec4(float(v & 255u), float((v >> 8u) & 255u), float((v >> 16u) & 255u),
                          255.0) / 255.0;
}
)";

constexpr char OutsidePixelGlsl[] = BUFFER_TEST_GLSL_HEAD R"(
layout(binding = 6) uniform highp samplerBuffer outside;
layout(location = 0) in vec4 in_var_COLOR0;
layout(location = 0) out vec4 out_var_SV_Target0;
void main() { out_var_SV_Target0 = texelFetch(outside, 0); }
)";

#undef BUFFER_TEST_GLSL_HEAD

struct Expected {
    uint8 r = 0;
    uint8 g = 0;
    uint8 b = 0;
};

bool Near(uint8 actual, uint8 expected) noexcept {
    const int difference = static_cast<int>(actual) - static_cast<int>(expected);
    return difference <= ColorTolerance && difference >= -ColorTolerance;
}

struct Bench {
    Device* device = nullptr;
    TextureHandle target;
    Array<uint8> pixels;

    explicit Bench(Allocator& allocator) noexcept : pixels(allocator) {
    }

    Result<BufferHandle> Buffer(const void* data, usize size, BufferUsage usage,
                                uint32 stride = 0) noexcept {
        BufferDesc desc;
        desc.size = size;
        desc.usage = usage;
        desc.stride = stride;
        return device->CreateBuffer(desc, Span<const uint8>(static_cast<const uint8*>(data), size));
    }

    Result<BufferViewHandle> View(BufferHandle buffer, BufferViewKind kind,
                                  PixelFormat format = PixelFormat::Unknown, uint64 offset = 0,
                                  uint64 count = 0, ViewAccess access = ViewAccess::Read) noexcept {
        BufferViewDesc desc;
        desc.kind = kind;
        desc.format = format;
        desc.offset = offset;
        desc.elementCount = count;
        desc.access = access;
        return device->CreateBufferView(buffer, desc);
    }

    Result<PipelineHandle> Pipeline(StringView hlsl, StringView vertexGlsl,
                                    StringView pixelGlsl) noexcept {
        PipelineDesc desc;
        desc.vertex = {hlsl, vertexGlsl};
        desc.pixel = {hlsl, pixelGlsl};
        desc.colorFormat = PixelFormat::RGBA8Unorm;
        return device->CreatePipeline(desc);
    }

    // view를 slot에 묶고 화면을 덮는 삼각형 하나를 그린 뒤 대상을 읽는다.
    bool Draw(PipelineHandle pipeline, uint32 slot, BufferViewHandle view) noexcept {
        auto frame = device->BeginOffscreen(target, {});
        if (!frame) {
            return false;
        }

        frame.Value()->SetPipeline(pipeline);
        frame.Value()->SetBufferView(slot, view);
        frame.Value()->Draw(3);
        return device->EndFrame().IsOk() &&
               pixels.TryResize(static_cast<usize>(TargetSize) * TargetSize * 4).IsOk() &&
               device->ReadPixels(target, pixels.View()).IsOk();
    }

    bool CenterIs(Expected expected) const noexcept {
        const usize offset = (static_cast<usize>(TargetSize / 2) * TargetSize + TargetSize / 2) * 4;
        const bool same = Near(pixels[offset], expected.r) &&
                          Near(pixels[offset + 1], expected.g) &&
                          Near(pixels[offset + 2], expected.b);
        if (!same) {
            printf("    center (%u, %u, %u), expected (%u, %u, %u)\n", pixels[offset],
                   pixels[offset + 1], pixels[offset + 2], expected.r, expected.g, expected.b);
        }

        return same;
    }
};

void PrintLastError(Device& device) noexcept {
    printf("    %.*s\n", static_cast<int>(device.LastError().ByteCount()),
           device.LastError().Data());
}

// seed로 정해지는 byte 무늬다.
void Pattern(Array<uint8>& out, usize size, uint8 seed) noexcept {
    if (!out.TryResize(size)) {
        return;
    }

    for (usize index = 0; index < size; ++index) {
        out[index] = static_cast<uint8>(seed + index * 7u);
    }
}

bool ReadMatches(Device& device, BufferHandle buffer, uint64 offset, Span<const uint8> expected,
                 Allocator& allocator) noexcept {
    Array<uint8> read(allocator);
    if (!read.TryResize(expected.Size()) || !device.ReadBuffer(buffer, offset, read.View())) {
        return false;
    }

    return platform::MemoryCompare(read.Data(), expected.Data(), expected.Size()) == 0;
}

// 용도 하나로 생성·ReadBuffer·부분 읽기·갱신·staging 업로드 바이트를 대조한다.
void RunUsage(Device& device, Allocator& allocator, const char* label, BufferUsage usage,
              uint32 stride) noexcept {
    constexpr usize Size = 256;
    const bool constant = usage == BufferUsage::Constant;
    Array<uint8> data(allocator);
    Pattern(data, Size, 11);
    BufferDesc desc;
    desc.size = Size;
    desc.usage = usage;
    desc.stride = stride;
    auto buffer = device.CreateBuffer(desc, data.View());
    char what[160] = {};
    snprintf(what, sizeof(what), "%s buffer creation and readback match", label);
    Check(buffer && data.Size() == Size &&
              ReadMatches(device, buffer.Value(), 0, data.View(), allocator),
          what);
    if (!buffer) {
        PrintLastError(device);
        return;
    }

    snprintf(what, sizeof(what), "%s buffer partial readback matches", label);
    Check(
        ReadMatches(device, buffer.Value(), 16, Span<const uint8>(data.Data() + 16, 32), allocator),
        what);
    // 상수 버퍼는 부분 갱신을 하지 않는다(D3D11.0 상수 버퍼 부분 갱신 제약). 전체를 갈아 쓴다.
    Array<uint8> update(allocator);
    Pattern(update, constant ? Size : 64, 97);
    const uint64 updateOffset = constant ? 0 : 32;
    bool updated = update.Size() != 0 &&
                   device.UpdateBuffer(buffer.Value(), updateOffset, update.View()).IsOk();
    if (updated) {
        platform::MemoryCopy(data.Data() + updateOffset, update.Data(), update.Size());
    }

    snprintf(what, sizeof(what), "%s buffer update is visible to readback", label);
    Check(updated && ReadMatches(device, buffer.Value(), 0, data.View(), allocator), what);
    if (!constant) {
        auto staging = device.AcquireStaging(64);
        bool uploaded = staging.IsOk();
        if (staging) {
            for (usize index = 0; index < 64; ++index) {
                staging.Value().data[index] = static_cast<uint8>(200 - index);
            }

            uploaded =
                device.UploadBufferFromStaging(buffer.Value(), 128, staging.Value().handle, 0, 64)
                    .IsOk();
            if (uploaded) {
                platform::MemoryCopy(data.Data() + 128, staging.Value().data, 64);
            }

            device.ReleaseStaging(staging.Value().handle);
        }

        snprintf(what, sizeof(what), "%s buffer staging copy is visible to readback", label);
        Check(uploaded && ReadMatches(device, buffer.Value(), 0, data.View(), allocator), what);
    }

    device.Retire(buffer.Value());
}

void RunUsages(Device& device, Allocator& allocator) noexcept {
    RunUsage(device, allocator, "vertex", BufferUsage::Vertex, 0);
    RunUsage(device, allocator, "index", BufferUsage::Index, 0);
    RunUsage(device, allocator, "vertex+index", BufferUsage::Vertex | BufferUsage::Index, 0);
    RunUsage(device, allocator, "constant", BufferUsage::Constant, 0);
    RunUsage(device, allocator, "indirect", BufferUsage::Indirect | BufferUsage::ShaderWrite, 0);
    RunUsage(device, allocator, "typed", BufferUsage::ShaderRead | BufferUsage::ShaderWrite, 0);
    RunUsage(device, allocator, "structured", BufferUsage::ShaderRead | BufferUsage::Structured,
             16);
    RunUsage(device, allocator, "raw",
             BufferUsage::ShaderRead | BufferUsage::ShaderWrite | BufferUsage::Raw, 0);

    // 생성 음성 대조: 설정 규칙 위반과 initial 크기 불일치는 InvalidArgument다.
    const uint8 bytes[64] = {};
    const auto create = [&](BufferUsage usage, uint32 stride, usize initial) noexcept {
        BufferDesc desc;
        desc.size = 64;
        desc.usage = usage;
        desc.stride = stride;
        return device.CreateBuffer(desc, Span<const uint8>(bytes, initial)).Error();
    };
    Check(create(BufferUsage::Constant | BufferUsage::Vertex, 0, 64) ==
                  ErrorCode::InvalidArgument &&
              create(BufferUsage::ShaderRead | BufferUsage::Structured, 0, 64) ==
                  ErrorCode::InvalidArgument &&
              create(BufferUsage::Raw, 0, 64) == ErrorCode::InvalidArgument &&
              create(BufferUsage::Vertex, 0, 32) == ErrorCode::InvalidArgument,
          "invalid buffer descriptions are rejected");

    // ReadBuffer 음성 대조.
    auto buffer = device.CreateBuffer({64, BufferUsage::Vertex, 0}, Span<const uint8>(bytes, 64));
    uint8 out[32] = {};
    Check(buffer &&
              device.ReadBuffer(buffer.Value(), 48, Span<uint8>(out, 32)).Error() ==
                  ErrorCode::InvalidArgument &&
              device.ReadBuffer(buffer.Value(), 65, Span<uint8>(out, 1)).Error() ==
                  ErrorCode::InvalidArgument &&
              device.ReadBuffer(buffer.Value(), 0, Span<uint8>()).Error() ==
                  ErrorCode::InvalidArgument,
          "buffer readback rejects out of range requests");
    if (buffer) {
        device.Retire(buffer.Value());
        Check(device.ReadBuffer(buffer.Value(), 0, Span<uint8>(out, 4)).Error() ==
                  ErrorCode::StaleHandle,
              "buffer readback of a retired buffer is stale");
    }
}

// 형식마다 typed view 생성 ⇔ TexelBufferRead(Read)·TexelBufferWrite(ReadWrite) 능력이다.
void RunTexelFormats(Device& device, Allocator& allocator) noexcept {
    Array<uint8> zeros(allocator);
    if (!zeros.TryResize(FormatProbeBytes)) {
        Check(false, "probe buffer memory");
        return;
    }

    platform::MemorySet(zeros.Data(), 0, zeros.Size());
    auto readOnly =
        device.CreateBuffer({FormatProbeBytes, BufferUsage::ShaderRead, 0}, zeros.View());
    auto readWrite = device.CreateBuffer(
        {FormatProbeBytes, BufferUsage::ShaderRead | BufferUsage::ShaderWrite, 0}, zeros.View());
    Check(readOnly && readWrite, "typed probe buffers");
    if (!readOnly || !readWrite) {
        return;
    }

    uint32 readable = 0;
    uint32 writable = 0;
    uint32 probed = 0;
    for (uint32 number = 1; number <= MaxPixelFormatNumber; ++number) {
        auto format = PixelFormatFromNumber(number);
        if (!format) {
            continue;
        }

        const PixelFormatInfo& info = GetPixelFormatInfo(format.Value());
        if (info.kind != PixelFormatKind::Color || info.planeCount != 1 || info.blockBytes == 0) {
            continue;
        }

        ++probed;
        const PixelFormatCaps caps = device.GetPixelFormatCaps(format.Value());
        for (uint32 pass = 0; pass < 2; ++pass) {
            const bool write = pass == 1;
            BufferViewDesc desc;
            desc.kind = BufferViewKind::Typed;
            desc.format = format.Value();
            desc.access = write ? ViewAccess::ReadWrite : ViewAccess::Read;
            auto view = device.CreateBufferView(write ? readWrite.Value() : readOnly.Value(), desc);
            const bool capable = HasPixelFormatCaps(caps, write ? PixelFormatCaps::TexelBufferWrite
                                                                : PixelFormatCaps::TexelBufferRead);
            const bool consistent = capable ? view.IsOk() : view.Error() == ErrorCode::Unsupported;
            if (!consistent) {
                ++failures;
                printf("  [FAIL] %s buffer: %s typed %s view %s but the format caps say %s\n",
                       backendName, PixelFormatName(format.Value()), write ? "UAV" : "SRV",
                       view ? "was created" : ErrorCodeName(view.Error()),
                       capable ? "supported" : "unsupported");
                if (!view) {
                    PrintLastError(device);
                }
            }

            if (view) {
                (write ? writable : readable) += 1;
                device.Retire(view.Value());
            }
        }
    }

    printf("  %s texel buffer: %u/%u formats readable, %u writable\n", backendName, readable,
           probed, writable);
    Log::Info(LogGraphics,
              "{Backend} texel buffer views: {Readable} of {Probed} formats readable, "
              "{Writable} writable",
              backendName, readable, probed, writable);
    device.Retire(readOnly.Value());
    device.Retire(readWrite.Value());
}

void RunViewNegatives(Device& device, Bench& bench) noexcept {
    const DeviceLimits& limits = device.Caps().limits;
    const uint8 bytes[256] = {};
    auto typed = bench.Buffer(bytes, 256, BufferUsage::ShaderRead);
    auto structured =
        bench.Buffer(bytes, 256, BufferUsage::ShaderRead | BufferUsage::Structured, 16);
    auto raw = bench.Buffer(bytes, 256, BufferUsage::ShaderRead | BufferUsage::Raw);
    Check(typed && structured && raw, "view probe buffers");
    if (!typed || !structured || !raw) {
        return;
    }

    const auto error = [](Result<BufferViewHandle> view) noexcept { return view.Error(); };
    Check(error(bench.View(structured.Value(), BufferViewKind::Typed, PixelFormat::R32Uint)) ==
                  ErrorCode::InvalidArgument &&
              error(bench.View(typed.Value(), BufferViewKind::Structured)) ==
                  ErrorCode::InvalidArgument &&
              error(bench.View(typed.Value(), BufferViewKind::Raw)) == ErrorCode::InvalidArgument,
          "view kind must match the buffer usage");
    Check(error(bench.View(typed.Value(), BufferViewKind::Typed, PixelFormat::R32Uint, 0, 0,
                           ViewAccess::ReadWrite)) == ErrorCode::InvalidArgument,
          "write view needs ShaderWrite usage");
    Check(error(bench.View(typed.Value(), BufferViewKind::Typed, PixelFormat::BC1Unorm)) ==
                  ErrorCode::InvalidArgument &&
              error(bench.View(typed.Value(), BufferViewKind::Typed, PixelFormat::Unknown)) ==
                  ErrorCode::InvalidArgument,
          "typed view needs an uncompressed color format");
    Check(error(bench.View(typed.Value(), BufferViewKind::Typed, PixelFormat::R32Uint, 256, 1)) ==
                  ErrorCode::InvalidArgument &&
              error(bench.View(typed.Value(), BufferViewKind::Typed, PixelFormat::R32Uint, 16,
                               0)) == ErrorCode::InvalidArgument &&
              error(bench.View(typed.Value(), BufferViewKind::Typed, PixelFormat::R32Uint, 0,
                               65)) == ErrorCode::InvalidArgument &&
              error(bench.View(raw.Value(), BufferViewKind::Raw, PixelFormat::Unknown, 8, 1)) ==
                  ErrorCode::InvalidArgument,
          "view range must stay inside the buffer and keep element and raw alignment");
    // 장치 정렬: 원소 정렬(4)만 맞고 장치 정렬이 더 크면 InvalidArgument, 아니면 만들어진다.
    auto misaligned = bench.View(typed.Value(), BufferViewKind::Typed, PixelFormat::R32Uint, 4, 1);
    if (limits.texelBufferOffsetAlignment > 4) {
        Check(misaligned.Error() == ErrorCode::InvalidArgument,
              "typed view offset must follow the device alignment");
    } else {
        Check(misaligned.IsOk(), "typed view offset on an element boundary");
        if (misaligned) {
            device.Retire(misaligned.Value());
        }
    }

    device.Retire(typed.Value());
    Check(error(bench.View(typed.Value(), BufferViewKind::Typed, PixelFormat::R32Uint)) ==
              ErrorCode::StaleHandle,
          "view of a retired buffer is stale");
    device.Retire(structured.Value());
    device.Retire(raw.Value());
}

struct DrawCase {
    const char* label;
    StringView hlsl;
    StringView vertexGlsl;
    StringView pixelGlsl;
    uint32 slot;
    BufferViewKind kind;
    PixelFormat format;
    // 셰이더가 읽는 단계다(OpenGL ES storage block 한도 판정).
    bool vertexStage;
};

void RunDraws(Device& device, Bench& bench) noexcept {
    const DeviceLimits& limits = device.Caps().limits;
    const bool openGl = device.Info().backend == Backend::OpenGL;
    // 원소 1(오프셋 view의 첫 원소)이 (0.25, 0.5, 0.75)다.
    const float lights[8] = {1.0f, 0.0f, 0.0f, 1.0f, 0.25f, 0.5f, 0.75f, 1.0f};
    const uint32 codes[4] = {0x00C08040u, 0, 0, 0};
    const float items[16] = {1, 0, 0, 1, 0, 0, 0, 0, 0.5f, 0.25f, 1.0f, 1.0f, 0, 0, 0, 0};
    const uint32 words[4] = {0, 0x0040C080u, 0, 0};
    auto lightBuffer = bench.Buffer(lights, sizeof(lights), BufferUsage::ShaderRead);
    auto codeBuffer = bench.Buffer(codes, sizeof(codes), BufferUsage::ShaderRead);
    auto itemBuffer =
        bench.Buffer(items, sizeof(items), BufferUsage::ShaderRead | BufferUsage::Structured, 32);
    auto wordBuffer =
        bench.Buffer(words, sizeof(words), BufferUsage::ShaderRead | BufferUsage::Raw);
    Check(lightBuffer && codeBuffer && itemBuffer && wordBuffer, "draw buffers");
    if (!lightBuffer || !codeBuffer || !itemBuffer || !wordBuffer) {
        return;
    }

    // typed view는 장치 정렬을 맞추려 오프셋 원소를 장치 정렬이 허락할 때만 쓴다.
    const bool offsetView = limits.texelBufferOffsetAlignment <= 16;
    auto lightView = bench.View(lightBuffer.Value(), BufferViewKind::Typed,
                                PixelFormat::RGBA32Float, offsetView ? 16 : 0, offsetView ? 1 : 0);
    auto codeView = bench.View(codeBuffer.Value(), BufferViewKind::Typed, PixelFormat::R32Uint);
    auto itemView = bench.View(itemBuffer.Value(), BufferViewKind::Structured);
    auto wordView = bench.View(wordBuffer.Value(), BufferViewKind::Raw);
    Check(lightView && codeView && itemView && wordView, "draw buffer views");
    if (!lightView || !codeView || !itemView || !wordView) {
        PrintLastError(device);
        return;
    }

    const Expected lightColor = offsetView ? Expected{64, 128, 191} : Expected{255, 0, 0};
    const DrawCase cases[] = {
        {"pixel shader reads a typed float view", Text(TypedPixelHlsl), Text(BlackVertexGlsl),
         Text(TypedPixelGlsl), 0, BufferViewKind::Typed, PixelFormat::RGBA32Float, false},
        {"vertex shader reads a typed uint view", Text(TypedVertexHlsl), Text(TypedVertexGlsl),
         Text(ColorPixelGlsl), 1, BufferViewKind::Typed, PixelFormat::R32Uint, true},
        {"pixel shader reads a structured view", Text(StructuredPixelHlsl), Text(BlackVertexGlsl),
         Text(StructuredPixelGlsl), 2, BufferViewKind::Structured, PixelFormat::Unknown, false},
        {"vertex shader reads a raw view", Text(RawVertexHlsl), Text(RawVertexGlsl),
         Text(ColorPixelGlsl), 3, BufferViewKind::Raw, PixelFormat::Unknown, true}};
    const BufferViewHandle views[] = {lightView.Value(), codeView.Value(), itemView.Value(),
                                      wordView.Value()};
    const Expected colors[] = {lightColor, {64, 128, 192}, {128, 64, 255}, {128, 192, 64}};
    PipelineHandle typedPixel;
    for (uint32 index = 0; index < 4; ++index) {
        const DrawCase& draw = cases[index];
        const StageResourceLimits& stage = draw.vertexStage ? limits.vertex : limits.pixel;
        if (openGl && draw.kind != BufferViewKind::Typed && stage.storageBuffers == 0) {
            // ES 3.2의 VS·PS storage block 최소 보장은 0이다(장치 한도로 공개, 대체하지 않는다).
            printf("  [NOT RUN] %s buffer: %s (no %s storage blocks)\n", backendName, draw.label,
                   draw.vertexStage ? "vertex" : "pixel");
            continue;
        }

        auto pipeline = bench.Pipeline(draw.hlsl, draw.vertexGlsl, draw.pixelGlsl);
        if (!pipeline) {
            Check(false, draw.label);
            PrintLastError(device);
            continue;
        }

        Check(bench.Draw(pipeline.Value(), draw.slot, views[index]) &&
                  bench.CenterIs(colors[index]),
              draw.label);
        if (index == 0) {
            typedPixel = pipeline.Value();
        } else {
            device.Retire(pipeline.Value());
        }
    }

    if (!typedPixel.IsNull()) {
        // 버퍼를 view보다 먼저 놓아도 view가 버퍼를 붙든다.
        auto early = bench.Buffer(lights, sizeof(lights), BufferUsage::ShaderRead);
        auto earlyView =
            early ? bench.View(early.Value(), BufferViewKind::Typed, PixelFormat::RGBA32Float)
                  : Result<BufferViewHandle>::Failure(early.Error());
        if (early) {
            device.Retire(early.Value());
        }

        Check(earlyView && bench.Draw(typedPixel, 0, earlyView.Value()) &&
                  bench.CenterIs({255, 0, 0}),
              "view keeps a retired buffer alive for drawing");
        if (earlyView) {
            device.Retire(earlyView.Value());
        }

        // slot 배치 불일치는 그 그리기를 무시한다: 빈 slot, 값 계열이 다른 typed view, 다른 종류,
        // 쓰기 view(그래픽 단계 UAV 없음 → slot이 빈다).
        auto writable = bench.Buffer(lights, sizeof(lights),
                                     BufferUsage::ShaderRead | BufferUsage::ShaderWrite);
        auto uav = writable ? bench.View(writable.Value(), BufferViewKind::Typed,
                                         PixelFormat::RGBA32Float, 0, 0, ViewAccess::ReadWrite)
                            : Result<BufferViewHandle>::Failure(writable.Error());
        const uint64 ignoredBefore = device.IgnoredDraws();
        bool drawn = bench.Draw(typedPixel, 0, BufferViewHandle{});
        drawn = bench.Draw(typedPixel, 0, codeView.Value()) && drawn;
        drawn = bench.Draw(typedPixel, 0, itemView.Value()) && drawn;
        uint64 expectedIgnored = 3;
        if (uav) {
            drawn = bench.Draw(typedPixel, 0, uav.Value()) && drawn;
            expectedIgnored = 4;
        }

        Check(uav.IsOk() || !HasPixelFormatCaps(device.GetPixelFormatCaps(PixelFormat::RGBA32Float),
                                                PixelFormatCaps::TexelBufferWrite),
              "typed UAV view creation");
        Check(drawn && device.IgnoredDraws() - ignoredBefore == expectedIgnored,
              "draws whose buffer view slots do not match the pipeline are ignored");
        if (uav) {
            device.Retire(uav.Value());
        }

        if (writable) {
            device.Retire(writable.Value());
        }

        device.Retire(typedPixel);
    }

    // 배치 밖 register와 그래픽 단계 UAV는 파이프라인 생성에서 거부한다.
    auto outside = bench.Pipeline(Text(OutsideHlsl), Text(BlackVertexGlsl), Text(OutsidePixelGlsl));
    Check(outside.Error() == ErrorCode::InvalidArgument && device.LastError().ByteCount() != 0,
          "shader resources outside the fixed layout are rejected");
    if (!openGl) {
        auto pixelUav =
            bench.Pipeline(Text(PixelUavHlsl), Text(BlackVertexGlsl), Text(ColorPixelGlsl));
        Check(pixelUav.Error() == ErrorCode::InvalidArgument,
              "pixel shader UAVs are rejected (UAVs are compute only)");
    }

    device.Retire(lightView.Value());
    device.Retire(codeView.Value());
    device.Retire(itemView.Value());
    device.Retire(wordView.Value());
    device.Retire(lightBuffer.Value());
    device.Retire(codeBuffer.Value());
    device.Retire(itemBuffer.Value());
    device.Retire(wordBuffer.Value());
}

// 표본 수 집합을 "1/2/4/8" 꼴로 쓴다.
void SampleCountText(uint32 counts, char (&out)[32]) noexcept {
    usize length = 0;
    out[0] = '\0';
    for (uint32 samples = 1; samples <= MaxSampleCount; samples *= 2) {
        if ((counts & SampleCountBit(samples)) == 0) {
            continue;
        }

        const int written =
            snprintf(out + length, sizeof(out) - length, "%s%u", length == 0 ? "" : "/", samples);
        length += written > 0 ? static_cast<usize>(written) : 0u;
        if (length >= sizeof(out)) {
            return;
        }
    }

    if (length == 0) {
        snprintf(out, sizeof(out), "none");
    }
}

} // namespace

uint32 RunCapsChecks(const char* name, Device& device) noexcept {
    failures = 0;
    backendName = name;
    const DeviceCaps& caps = device.Caps();
    const DeviceLimits& limits = caps.limits;
    const DeviceFeatures& features = caps.features;
    Log::Info(
        LogGraphics,
        "{Backend} caps: color attachments {Mrt}, sampled textures VS/PS/CS "
        "{Vertex}/{Pixel}/{Compute}, samplers {Samplers}, constant buffers {Constant}, "
        "storage buffers VS/PS/CS {StorageVertex}/{StoragePixel}/{StorageCompute}, storage "
        "images VS/PS/CS {ImageVertex}/{ImagePixel}/{ImageCompute}",
        name, limits.colorAttachments, limits.vertex.sampledTextures, limits.pixel.sampledTextures,
        limits.compute.sampledTextures, limits.pixel.samplers, limits.pixel.constantBuffers,
        limits.vertex.storageBuffers, limits.pixel.storageBuffers, limits.compute.storageBuffers,
        limits.vertex.storageImages, limits.pixel.storageImages, limits.compute.storageImages);
    Log::Info(LogGraphics,
              "{Backend} caps: pipeline sampled textures {Textures}, samplers {Samplers}, storage "
              "buffers {Buffers}, storage images {Images}, per stage resources {PerStage}, UAVs "
              "{Uavs}, descriptor sets {Sets}, uniform vectors VS/PS/CS {Vertex}/{Pixel}/{Compute}",
              name, limits.pipelineSampledTextures, limits.pipelineSamplers,
              limits.pipelineStorageBuffers, limits.pipelineStorageImages, limits.perStageResources,
              limits.pipelineUavs, limits.boundDescriptorSets, limits.vertex.uniformVectors,
              limits.pixel.uniformVectors, limits.compute.uniformVectors);
    Log::Info(LogGraphics,
              "{Backend} caps: constant buffer {ConstantBytes} bytes, root constants {RootBytes} "
              "bytes, offset alignment constant/storage/texel {ConstantAlign}/{StorageAlign}/"
              "{TexelAlign}, storage view {StorageBytes} bytes, texel elements {TexelElements}, "
              "buffer {BufferBytes} bytes, vertex attributes {Attributes}, streams {Streams}, "
              "inter-stage vectors {Varyings}",
              name, limits.constantBufferBytes, limits.rootConstantBytes,
              limits.constantBufferOffsetAlignment, limits.storageBufferOffsetAlignment,
              limits.texelBufferOffsetAlignment, limits.storageBufferBytes,
              limits.texelBufferElements, limits.bufferBytes, limits.vertexAttributes,
              limits.vertexStreams, limits.interStageVectors);
    Log::Info(LogGraphics,
              "{Backend} caps: compute groups {CountX}x{CountY}x{CountZ}, group size "
              "{SizeX}x{SizeY}x{SizeZ}, invocations {Invocations}, shared memory {Shared} bytes, "
              "texture 2D {Size2D}, 3D {Size3D}, cube {SizeCube}, layers {Layers}, anisotropy "
              "{Anisotropy}",
              name, limits.computeGroupCount[0], limits.computeGroupCount[1],
              limits.computeGroupCount[2], limits.computeGroupSize[0], limits.computeGroupSize[1],
              limits.computeGroupSize[2], limits.computeGroupInvocations,
              limits.computeSharedMemoryBytes, limits.textureSize2D, limits.textureSize3D,
              limits.textureSizeCube, limits.textureArrayLayers, limits.samplerAnisotropy);
    Log::Info(LogGraphics,
              "{Backend} features: compute {Compute}, depth clamp {DepthClamp}, VS layer "
              "{Layer}, atomic64 {Atomic64}, indirect first instance {FirstInstance}, multi draw "
              "indirect {MultiDraw}, cube array {CubeArray}, sRGB pair view {SrgbView}, VS stores "
              "{VertexStores}, PS stores {PixelStores}, HDR output {Hdr}, binding tier {Tier}, "
              "shader model {ShaderModel}, API version {Api}",
              name, features.computeShaders, features.depthClamp, features.vertexShaderLayerOutput,
              features.atomic64, features.indirectFirstInstance, features.multiDrawIndirect,
              features.textureCubeArray, features.srgbPairView, features.vertexStores,
              features.pixelStores, features.hdrOutput,
              static_cast<uint32>(features.resourceBindingTier),
              static_cast<uint32>(features.shaderModel), device.Info().apiVersion);

    // 대표 형식의 MSAA 표본 수와 typed UAV load·texel buffer 능력이다(Renderer RQ4).
    const PixelFormat msaaFormats[] = {PixelFormat::RGBA8Unorm,    PixelFormat::RGBA16Float,
                                       PixelFormat::RG11B10Ufloat, PixelFormat::R32Uint,
                                       PixelFormat::D32Float,      PixelFormat::D24UnormS8Uint};
    char counts[6][32] = {};
    for (usize index = 0; index < 6; ++index) {
        SampleCountText(device.GetPixelFormatSampleCounts(msaaFormats[index]), counts[index]);
    }

    Log::Info(LogGraphics,
              "{Backend} MSAA samples: RGBA8Unorm {Rgba8}, RGBA16Float {Rgba16f}, RG11B10Ufloat "
              "{Rg11b10}, R32Uint {R32ui}, D32Float {D32}, D24UnormS8Uint {D24s8}",
              name, counts[0], counts[1], counts[2], counts[3], counts[4], counts[5]);
    const auto has = [&](PixelFormat format, PixelFormatCaps required) noexcept {
        return HasPixelFormatCaps(device.GetPixelFormatCaps(format), required);
    };
    Log::Info(LogGraphics,
              "{Backend} format caps: typed UAV load R32Float {R32f}, RG16Float {Rg16f}, "
              "RGBA16Float {Rgba16f}; texel buffer R32Uint read {R32uiRead} write {R32uiWrite}, "
              "RGBA32Float read {Rgba32fRead} write {Rgba32fWrite}",
              name, has(PixelFormat::R32Float, PixelFormatCaps::StorageRead),
              has(PixelFormat::RG16Float, PixelFormatCaps::StorageRead),
              has(PixelFormat::RGBA16Float, PixelFormatCaps::StorageRead),
              has(PixelFormat::R32Uint, PixelFormatCaps::TexelBufferRead),
              has(PixelFormat::R32Uint, PixelFormatCaps::TexelBufferWrite),
              has(PixelFormat::RGBA32Float, PixelFormatCaps::TexelBufferRead),
              has(PixelFormat::RGBA32Float, PixelFormatCaps::TexelBufferWrite));

    // 사양 최소 보장(ES 3.2·Vulkan 1.1·D3D FL 11.0의 공통 하한) 이상인지 본다.
    Check(limits.colorAttachments >= 4 && limits.pixel.sampledTextures >= 16 &&
              limits.pixel.samplers >= 16 && limits.vertex.sampledTextures >= 16 &&
              limits.texelBufferElements >= 65536 && limits.computeGroupInvocations >= 128 &&
              limits.textureSize2D >= 2048 && limits.textureArrayLayers >= 256 &&
              limits.bufferBytes >= (uint64{128} << 20) && limits.rootConstantBytes >= 128 &&
              limits.vertexAttributes >= 16 && limits.computeGroupCount[0] >= 65535 &&
              features.computeShaders,
          "device limits meet the API minimum guarantees");
    // 형식마다 Multisample ⇔ 2표본 이상, 표본 수 집합 ⇔ 렌더 타깃·깊이 능력.
    uint32 multisampled = 0;
    bool consistent = true;
    for (uint32 number = 1; number <= MaxPixelFormatNumber; ++number) {
        auto format = PixelFormatFromNumber(number);
        if (!format) {
            continue;
        }

        const PixelFormatCaps formatCaps = device.GetPixelFormatCaps(format.Value());
        const uint32 sampleCounts = device.GetPixelFormatSampleCounts(format.Value());
        const bool target = HasPixelFormatCaps(formatCaps, PixelFormatCaps::RenderTarget) ||
                            HasPixelFormatCaps(formatCaps, PixelFormatCaps::DepthStencil);
        const bool multisample = HasPixelFormatCaps(formatCaps, PixelFormatCaps::Multisample);
        if ((sampleCounts != 0) != target || multisample != (sampleCounts > SampleCountBit(1)) ||
            (target && (sampleCounts & SampleCountBit(1)) == 0)) {
            consistent = false;
            printf("    %s: caps 0x%X, sample counts 0x%X\n", PixelFormatName(format.Value()),
                   static_cast<uint32>(formatCaps), sampleCounts);
        }

        multisampled += multisample ? 1u : 0u;
    }

    Check(consistent, "Multisample caps match the queried sample counts");
    printf("  %s caps: MRT %u, PS sampled %u, PS samplers %u, pipeline sampled %u, per stage "
           "resources %u, MSAA formats %u (RGBA8Unorm %s, D32Float %s)\n",
           name, limits.colorAttachments, limits.pixel.sampledTextures, limits.pixel.samplers,
           limits.pipelineSampledTextures, limits.perStageResources, multisampled, counts[0],
           counts[4]);
    return failures;
}

uint32 RunBufferChecks(const char* name, Device& device, Allocator& allocator) noexcept {
    failures = 0;
    backendName = name;
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
    RunUsages(device, allocator);
    RunTexelFormats(device, allocator);
    RunViewNegatives(device, bench);
    RunDraws(device, bench);
    device.Retire(bench.target);
    Check(device.WaitIdle().IsOk() && device.PendingRetireCount() == 0,
          "buffer check resources are reclaimed");
    Check(device.ValidationErrorCount() == validationBefore, "no validation errors");
    printf("  %s buffers and views: %u failure%s\n", name, failures, failures == 1 ? "" : "s");
    return failures;
}

} // namespace sandbox
