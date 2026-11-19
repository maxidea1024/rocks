#include "StorageTargetSelfTest.h"

#include <Core/Containers/Array.h>
#include <Platform/Memory.h>
#include <stdio.h>

namespace sandbox {

namespace {

using namespace rocks;
using namespace rocks::graphics;

// 시험 텍스처의 한 변이다(compute 그룹 한 변과 같아 Dispatch(1, 1)이 다 덮는다).
constexpr uint32 Extent = 4;
// compute가 덮는 열 수, PS가 그리기 시작하는 열이다.
constexpr uint32 ComputeColumns = 2;
constexpr uint32 DrawColumn = 3;
// clear·compute·PS가 쓰는 값이다.
constexpr float ClearValue = 0.25f;
constexpr float ComputeValue = 0.5f;
constexpr float DrawValue = 0.75f;
// cube 시험에서 하나만 쓰는 면과 그 값이다(cube 면 2, cube 배열의 두 번째 cube 면 0).
constexpr uint32 CubeFace = 2;
constexpr uint32 CubeArrayFace = 6;
constexpr uint32 CubeArrayCubes = 2;
// layer 거르기를 끄는 값이다(모든 층에 쓴다).
constexpr float AllLayers = -1.0f;

uint32 failures = 0;
const char* backendName = "";

void Check(bool condition, const char* what) noexcept {
    if (!condition) {
        ++failures;
        printf("  [FAIL] %s storage targets: %s\n", backendName, what);
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

// 채우기 compute의 그리기별 상수다. x는 쓸 값, y는 쓸 층(음수면 모든 층)이다.
struct FillConstants {
    float values[4] = {};
};

#define STORAGE_GLSL_HEAD "#version 320 es\nprecision highp float;\nprecision highp int;\n"

// x < 2 열을 0.5로 덮는다(RenderTarget | Storage의 쓰기 view).
constexpr char ColumnsHlsl[] = R"(
RWTexture2D<float> target : register(u0);
[numthreads(4, 4, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) { if (id.x < 2) { target[id.xy] = 0.5; } }
)";

constexpr char ColumnsGlsl[] = STORAGE_GLSL_HEAD R"(
layout(local_size_x = 4, local_size_y = 4) in;
layout(binding = 0, r32f) uniform highp writeonly image2D target;
void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (p.x < 2) { imageStore(target, p, vec4(0.5)); }
}
)";

// 면 2D 배열 쓰기 view의 층(SV_DispatchThreadID.z)에 상수 값을 쓴다. D3D·Vulkan은 cube 계열 쓰기
// view가 2D 배열이라 셰이더 하나로 된다.
constexpr char FillHlsl[] = R"(
cbuffer DrawConstants : register(b0) { float4 fill; };
RWTexture2DArray<float> target : register(u0);
[numthreads(4, 4, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (fill.y >= 0.0 && id.z != (uint)fill.y) { return; }
    target[id] = fill.x;
}
)";

// OpenGL ES는 image 형식이 텍스처 target과 맞아야 한다: layered cube는 imageCube, layered cube
// 배열은 imageCubeArray(z = 면 번호), 면 하나만 고른 view는 image2D다.
#define STORAGE_FILL_GLSL(declaration, coordinate)                                                 \
    STORAGE_GLSL_HEAD "layout(local_size_x = 4, local_size_y = 4) in;\n"                           \
                      "uniform highp vec4 cu_h[1];\n"                                              \
                      "layout(binding = 0, r32f) uniform highp writeonly " declaration             \
                      " target;\n"                                                                 \
                      "void main() {\n"                                                            \
                      "    ivec3 id = ivec3(gl_GlobalInvocationID);\n"                             \
                      "    if (cu_h[0].y >= 0.0 && id.z != int(cu_h[0].y)) { return; }\n"          \
                      "    imageStore(target, " coordinate ", vec4(cu_h[0].x));\n"                 \
                      "}\n"

constexpr char FillCubeGlsl[] = STORAGE_FILL_GLSL("imageCube", "id");
constexpr char FillCubeArrayGlsl[] = STORAGE_FILL_GLSL("imageCubeArray", "id");
constexpr char FillFaceGlsl[] = STORAGE_FILL_GLSL("image2D", "id.xy");

#undef STORAGE_FILL_GLSL

// x ≥ 3 열에만 0.75를 그린다(load attachment 위).
constexpr char DrawHlsl[] = "float4 VSMain(uint id : SV_VertexID) : SV_Position {\n"
                            "    float2 uv = float2((id << 1) & 2, id & 2);\n"
                            "    return float4(uv * 2.0 - 1.0, 0.5, 1.0);\n"
                            "}\n"
                            "float4 PSMain(float4 pos : SV_Position) : SV_Target {\n"
                            "    if (pos.x < 3.0) { discard; }\n"
                            "    return float4(0.75, 0.0, 0.0, 1.0);\n"
                            "}\n";

constexpr char DrawVertexGlsl[] = STORAGE_GLSL_HEAD R"(
void main() {
    vec2 uv = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(uv * 2.0 - 1.0, 0.5, 1.0);
}
)";

constexpr char DrawPixelGlsl[] = STORAGE_GLSL_HEAD R"(
layout(location = 0) out vec4 out_var_SV_Target0;
void main() {
    if (gl_FragCoord.x < 3.0) { discard; }
    out_var_SV_Target0 = vec4(0.75, 0.0, 0.0, 1.0);
}
)";

#undef STORAGE_GLSL_HEAD

TextureDesc StorageDesc(TextureDimension dimension, TextureUsage usage, uint32 arraySize) noexcept {
    TextureDesc desc;
    desc.dimension = dimension;
    desc.width = Extent;
    desc.height = Extent;
    desc.arraySize = arraySize;
    desc.format = PixelFormat::R32Float;
    desc.usage = usage;
    desc.sampler.filter = Filter::Point;
    return desc;
}

// 텍스처 mip 0의 모든 slice를 float로 읽는다.
bool ReadFloats(Device& device, Allocator& allocator, TextureHandle texture, uint32 slices,
                Array<float>& out) noexcept {
    const usize count = static_cast<usize>(Extent) * Extent * slices;
    Array<uint8> bytes(allocator);
    if (!bytes.TryResize(count * sizeof(float)) || !out.TryResize(count) ||
        !device.ReadTexture(texture, 0, 0, bytes.View())) {
        return false;
    }

    platform::MemoryCopy(out.Data(), bytes.Data(), count * sizeof(float));
    return true;
}

TextureViewDesc WriteView(uint32 firstSlice, uint32 sliceCount) noexcept {
    TextureViewDesc desc;
    desc.firstMip = 0;
    desc.mipCount = 1;
    desc.firstSlice = firstSlice;
    desc.sliceCount = sliceCount;
    desc.access = ViewAccess::ReadWrite;
    return desc;
}

// RenderTarget | Storage: clear → compute 쓰기 → load attachment 위 그리기.
void RunRenderTargetStorage(Device& device, Allocator& allocator) noexcept {
    const TextureDesc desc = StorageDesc(TextureDimension::Texture2D,
                                         TextureUsage::RenderTarget | TextureUsage::Storage, 1);
    const bool capable =
        HasPixelFormatCaps(device.GetPixelFormatCaps(desc.format), RequiredPixelFormatCaps(desc));
    auto texture = device.CreateTexture(desc, {});
    if (!capable) {
        Check(texture.Error() == ErrorCode::Unsupported,
              "RenderTarget | Storage without the format caps is Unsupported");
        return;
    }

    Check(texture.IsOk(), "RenderTarget | Storage R32Float texture");
    ComputePipelineDesc columnsDesc;
    columnsDesc.shader = {Text(ColumnsHlsl), Text(ColumnsGlsl)};
    auto columns = device.CreateComputePipeline(columnsDesc);
    PipelineDesc drawDesc;
    drawDesc.vertex = {Text(DrawHlsl), Text(DrawVertexGlsl)};
    drawDesc.pixel = {Text(DrawHlsl), Text(DrawPixelGlsl)};
    drawDesc.colorFormats[0] = PixelFormat::R32Float;
    auto draw = device.CreatePipeline(drawDesc);
    auto write = texture ? device.CreateTextureView(texture.Value(), WriteView(0, 1))
                         : Result<TextureHandle>::Failure(ErrorCode::InvalidArgument);
    Check(columns && draw && write, "pipelines and the writable view of a render target");
    if (!texture || !columns || !draw || !write) {
        PrintLastError(device);
    } else {
        PassDesc clear;
        clear.colorCount = 1;
        clear.colors[0].texture = texture.Value();
        clear.colors[0].clear = {ClearValue, 0.0f, 0.0f, 1.0f};
        Check(device.BeginPass(clear) && device.EndFrame(), "clear the render target");
        auto compute = device.BeginPass(PassDesc{});
        if (compute) {
            compute.Value()->SetPipeline(columns.Value());
            compute.Value()->SetWritableTexture(0, write.Value());
            compute.Value()->Dispatch(1, 1);
        }

        Check(compute && device.EndFrame(), "compute writes the render target");
        PassDesc load = clear;
        load.colors[0].load = LoadAction::Load;
        auto again = device.BeginPass(load);
        if (again) {
            again.Value()->SetPipeline(draw.Value());
            again.Value()->Draw(3);
        }

        Check(again && device.EndFrame(), "draw over the loaded render target");
        Array<float> values(allocator);
        const bool read = ReadFloats(device, allocator, texture.Value(), 1, values);
        bool same = read;
        for (uint32 y = 0; same && y < Extent; ++y) {
            for (uint32 x = 0; same && x < Extent; ++x) {
                const float expected = x < ComputeColumns ? ComputeValue
                                       : x < DrawColumn   ? ClearValue
                                                          : DrawValue;
                same = values[y * Extent + x] == expected;
                if (!same) {
                    printf("    (%u, %u) = %f, expected %f\n", x, y,
                           static_cast<double>(values[y * Extent + x]),
                           static_cast<double>(expected));
                }
            }
        }

        Check(same, "compute and draw results share the render target (0.5 0.5 0.25 0.75)");
    }

    if (write) {
        device.Retire(write.Value());
    }

    if (draw) {
        device.Retire(draw.Value());
    }

    if (columns) {
        device.Retire(columns.Value());
    }

    if (texture) {
        device.Retire(texture.Value());
    }
}

// 쓰기 view로 compute 채우기를 한 번 낸다.
bool Fill(Device& device, PipelineHandle pipeline, TextureHandle view, float value, float layer,
          uint32 layers) noexcept {
    auto compute = device.BeginPass(PassDesc{});
    if (!compute) {
        return false;
    }

    FillConstants constants;
    constants.values[0] = value;
    constants.values[1] = layer;
    compute.Value()->SetPipeline(pipeline);
    compute.Value()->SetWritableTexture(0, view);
    const bool set = compute.Value()->SetDrawConstants(DrawConstantBytes(constants)).IsOk();
    compute.Value()->Dispatch(1, 1, layers);
    return device.EndFrame().IsOk() && set;
}

// slice마다 기대값(면 target만 value, 나머지 0)과 같은지 본다.
bool FacesMatch(const Array<float>& values, uint32 slices, uint32 target, float value) noexcept {
    for (uint32 slice = 0; slice < slices; ++slice) {
        const float expected = slice == target ? value : 0.0f;
        for (uint32 texel = 0; texel < Extent * Extent; ++texel) {
            if (values[slice * Extent * Extent + texel] != expected) {
                printf("    slice %u texel %u = %f, expected %f\n", slice, texel,
                       static_cast<double>(values[slice * Extent * Extent + texel]),
                       static_cast<double>(expected));
                return false;
            }
        }
    }

    return true;
}

ComputePipelineDesc FillDesc(StringView glsl) noexcept {
    ComputePipelineDesc desc;
    desc.shader = {Text(FillHlsl), glsl};
    desc.drawConstantBytes = sizeof(FillConstants);
    return desc;
}

// cube Storage: 모든 면 0 → 면 2만 2. cube 배열: 모든 면 0 → 면 6만 6.
void RunCubeStorage(Device& device, Allocator& allocator) noexcept {
    const TextureDesc cubeDesc =
        StorageDesc(TextureDimension::TextureCube, TextureUsage::Storage, 1);
    const TextureDesc arrayDesc =
        StorageDesc(TextureDimension::TextureCubeArray, TextureUsage::Storage, CubeArrayCubes);
    const bool capable = HasPixelFormatCaps(device.GetPixelFormatCaps(cubeDesc.format),
                                            RequiredPixelFormatCaps(cubeDesc));
    auto cube = device.CreateTexture(cubeDesc, {});
    if (!capable) {
        Check(cube.Error() == ErrorCode::Unsupported,
              "cube Storage without the format caps is Unsupported");
        return;
    }

    Check(cube.IsOk(), "cube Storage R32Float texture");
    auto cubeArray = device.CreateTexture(arrayDesc, {});
    Check(cubeArray.IsOk() || !device.Caps().features.textureCubeArray,
          "cube array Storage R32Float texture");
    auto fillCube = device.CreateComputePipeline(FillDesc(Text(FillCubeGlsl)));
    auto fillArray = device.CreateComputePipeline(FillDesc(Text(FillCubeArrayGlsl)));
    auto fillFace = device.CreateComputePipeline(FillDesc(Text(FillFaceGlsl)));
    Check(fillCube && fillArray && fillFace, "cube fill compute pipelines");
    if (!fillCube || !fillArray || !fillFace) {
        PrintLastError(device);
    }
    if (cube && fillCube && fillArray && fillFace) {
        auto all = device.CreateTextureView(cube.Value(), WriteView(0, CubeFaceCount));
        auto face = device.CreateTextureView(cube.Value(), WriteView(CubeFace, 1));
        Check(all && face, "layered and single face writable views of a cube");
        if (all && face) {
            Check(Fill(device, fillCube.Value(), all.Value(), 0.0f, AllLayers, CubeFaceCount),
                  "fill every cube face");
            Check(Fill(device, fillFace.Value(), face.Value(), static_cast<float>(CubeFace),
                       AllLayers, 1),
                  "fill one cube face through its own view");
            Array<float> values(allocator);
            Check(ReadFloats(device, allocator, cube.Value(), CubeFaceCount, values) &&
                      FacesMatch(values, CubeFaceCount, CubeFace, static_cast<float>(CubeFace)),
                  "only cube face 2 changes");
        } else {
            PrintLastError(device);
        }

        if (all) {
            device.Retire(all.Value());
        }

        if (face) {
            device.Retire(face.Value());
        }
    }

    if (cubeArray && fillArray) {
        const uint32 faces = CubeFaceCount * CubeArrayCubes;
        auto all = device.CreateTextureView(cubeArray.Value(), WriteView(0, faces));
        Check(all.IsOk(), "layered writable view of a cube array");
        if (all) {
            Check(Fill(device, fillArray.Value(), all.Value(), 0.0f, AllLayers, faces) &&
                      Fill(device, fillArray.Value(), all.Value(),
                           static_cast<float>(CubeArrayFace), static_cast<float>(CubeArrayFace),
                           faces),
                  "fill the cube array and then face 6");
            Array<float> values(allocator);
            Check(ReadFloats(device, allocator, cubeArray.Value(), faces, values) &&
                      FacesMatch(values, faces, CubeArrayFace, static_cast<float>(CubeArrayFace)),
                  "only the second cube's face 0 changes");
            device.Retire(all.Value());
        } else {
            PrintLastError(device);
        }
    }

    if (fillCube) {
        device.Retire(fillCube.Value());
    }

    if (fillArray) {
        device.Retire(fillArray.Value());
    }

    if (fillFace) {
        device.Retire(fillFace.Value());
    }

    if (cube) {
        device.Retire(cube.Value());
    }

    if (cubeArray) {
        device.Retire(cubeArray.Value());
    }
}

} // namespace

uint32 RunStorageTargetChecks(const char* name, Device& device, Allocator& allocator) noexcept {
    failures = 0;
    backendName = name;
    const uint64 validationBefore = device.ValidationErrorCount();
    if (!device.Caps().features.computeShaders) {
        printf("  %s storage targets: no compute, skipped\n", name);
        return failures;
    }

    RunRenderTargetStorage(device, allocator);
    RunCubeStorage(device, allocator);
    Check(device.WaitIdle().IsOk(), "wait idle");
    Check(device.ValidationErrorCount() == validationBefore, "no validation errors");
    printf("  %s storage targets: %u failures\n", name, failures);
    return failures;
}

} // namespace sandbox
