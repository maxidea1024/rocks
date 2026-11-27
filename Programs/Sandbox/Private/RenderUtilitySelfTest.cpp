#include "RenderUtilitySelfTest.h"
#include "SandboxShaders.h"

#include <Core/Containers/Array.h>
#include <Core/Memory/FrameArena.h>
#include <RenderBase/BuiltinResources.h>
#include <RenderBase/EngineShaderTypes.h>
#include <RenderBase/FullscreenPass.h>
#include <RenderBase/NoiseTextures.h>
#include <RenderBase/PipelineCache.h>
#include <RenderBase/PipelineStatePresets.h>
#include <RenderBase/RenderHandle.h>
#include <RenderGraph/GraphResourcePool.h>
#include <RenderGraph/RenderGraph.h>
#include <ShaderCore/ShaderBindingLayout.h>
#include <stdio.h>

// RenderBase 그리기 유틸리티 self-test다(Docs/Design/RenderBase 11.4절). 셰이더는 엔진 셰이더
// (Engine/Shaders/Private/Utility)이고 Sandbox 공급자가 Sandbox 셰이더와 함께 만든다.
namespace sandbox {

namespace {

using namespace rocks;
using namespace rocks::graphics;
using rocks::render::GraphLoadAction;
using rocks::render::GraphPassAccesses;
using rocks::render::GraphPassContext;
using rocks::render::GraphPassFlags;
using rocks::render::GraphResourcePool;
using rocks::render::RenderGraph;
using rocks::render::RenderGraphDesc;
using rocks::render::RenderHandleTable;
using rocks::render::RenderResourceKind;

// 대상·원본 크기다(4 × 4 — point 표본이 texel 중심을 정확히 읽는다).
constexpr uint32 TargetSize = 4;
constexpr uint32 BytesPerPixel = 4;
constexpr usize TargetBytes = static_cast<usize>(TargetSize) * TargetSize * BytesPerPixel;

TraceScopeName CopyName("FullscreenCopy");

uint32 failures = 0;
// 전체화면 복사 PS(CopyTexturePixel)의 텍스처 slot이다(RunRenderUtilityChecks가 값 1에서 정한다).
uint32 copySourceSlot = MaxResourceSlots;
const char* backendName = "";

void Check(bool condition, const char* what) noexcept {
    if (!condition) {
        ++failures;
        printf("  [FAIL] %s render utility: %s\n", backendName, what);
    }
}

// 원본 픽셀 (x, y)의 값이다(채널마다 다른 값 — 위치가 뒤바뀌면 드러난다).
uint8 SourceChannel(uint32 x, uint32 y, uint32 channel) noexcept {
    return static_cast<uint8>(16 * x + 64 * y + 3 * channel + 1);
}

void FillSource(Span<uint8> pixels) noexcept {
    for (uint32 y = 0; y < TargetSize; ++y) {
        for (uint32 x = 0; x < TargetSize; ++x) {
            for (uint32 channel = 0; channel < BytesPerPixel; ++channel) {
                pixels[(static_cast<usize>(y) * TargetSize + x) * BytesPerPixel + channel] =
                    SourceChannel(x, y, channel);
            }
        }
    }
}

// 읽은 대상이 원본과 byte 단위로 같은지다(point 표본·UNORM8이라 정확하다).
bool MatchesSource(Span<const uint8> pixels, const char* label) noexcept {
    for (uint32 y = 0; y < TargetSize; ++y) {
        for (uint32 x = 0; x < TargetSize; ++x) {
            for (uint32 channel = 0; channel < BytesPerPixel; ++channel) {
                const uint8 actual =
                    pixels[(static_cast<usize>(y) * TargetSize + x) * BytesPerPixel + channel];
                if (actual != SourceChannel(x, y, channel)) {
                    printf("    %s pixel (%u, %u) channel %u is %u, expected %u\n", label, x, y,
                           channel, actual, SourceChannel(x, y, channel));
                    return false;
                }
            }
        }
    }

    return true;
}

TextureDesc SourceDesc() noexcept {
    TextureDesc desc;
    desc.width = TargetSize;
    desc.height = TargetSize;
    desc.format = PixelFormat::RGBA8Unorm;
    desc.sampler.filter = Filter::Point;
    return desc;
}

TextureDesc TargetDesc() noexcept {
    TextureDesc desc = SourceDesc();
    desc.usage = TextureUsage::RenderTarget;
    return desc;
}

// 장치 직접 경로: 파이프라인 캐시 + DrawFullscreenTriangle.
void RunDirectCopy(Device& device, PipelineHandle pipeline, TextureHandle source,
                   TextureHandle target, Array<uint8>& pixels) noexcept {
    const uint64 nullBefore = device.NullTextureBindings();
    auto frame = device.BeginOffscreen(target, {});
    Check(frame.IsOk(), "offscreen recording begins");
    if (!frame) {
        return;
    }

    CommandContext& commands = *frame.Value();
    commands.SetPipeline(pipeline);
    commands.SetTexture(ShaderStage::Pixel, copySourceSlot, source);
    render::DrawFullscreenTriangle(commands);
    Check(device.EndFrame().IsOk() && device.ReadPixels(target, pixels.View()).IsOk(),
          "fullscreen copy frame");
    Check(MatchesSource(pixels.View(), "direct copy"),
          "the fullscreen triangle copies every texel to the same pixel");
    Check(device.NullTextureBindings() == nullBefore,
          "the RenderBase fullscreen path binds no null texture");

    // 음성: null 텍스처 묶기는 흰 텍스처로 대신 그리고 센다.
    frame = device.BeginOffscreen(target, {});
    Check(frame.IsOk(), "null binding recording begins");
    if (!frame) {
        return;
    }

    CommandContext& again = *frame.Value();
    again.SetPipeline(pipeline);
    again.SetTexture(ShaderStage::Pixel, copySourceSlot, TextureHandle{});
    render::DrawFullscreenTriangle(again);
    Check(device.EndFrame().IsOk() && device.ReadPixels(target, pixels.View()).IsOk(),
          "null binding frame");
    Check(device.NullTextureBindings() == nullBefore + 1, "a null texture binding is counted");
    Check(pixels[0] == 255 && pixels[1] == 255 && pixels[2] == 255,
          "a null texture binding reads the device white texture");
}

// 렌더 그래프 경로: AddFullscreenPass(캐시 판)로 import한 원본을 import한 대상에 복사한다.
void RunGraphCopy(Device& device, render::PipelineCache& cache,
                  const render::GraphicsPipelineRequest& request, TextureHandle source,
                  TextureHandle target, Array<uint8>& pixels, Allocator& allocator) noexcept {
    RenderHandleTable handles(allocator);
    GraphResourcePool pool(allocator, device);
    FrameArena arena(allocator);
    auto sourceHandle = handles.Reserve(RenderResourceKind::Texture);
    auto targetHandle = handles.Reserve(RenderResourceKind::Texture);
    Check(sourceHandle.IsOk() && targetHandle.IsOk() &&
              handles.BindTexture(sourceHandle.Value(), source).IsOk() &&
              handles.BindTexture(targetHandle.Value(), target).IsOk(),
          "graph render handles are bound");
    if (!sourceHandle || !targetHandle) {
        return;
    }

    const uint64 nullBefore = device.NullTextureBindings();
    {
        RenderGraphDesc desc;
        desc.device = &device;
        desc.handles = &handles;
        desc.pool = &pool;
        desc.arena = &arena;
        desc.frame = 1;
        RenderGraph graph(desc);
        auto input = graph.ImportTexture(sourceHandle.Value(), "Source");
        auto output = graph.ImportTexture(targetHandle.Value(), "Output");
        Check(input.IsOk() && output.IsOk(), "graph imports the source and the target");
        if (!input || !output) {
            return;
        }

        GraphPassAccesses accesses(graph);
        accesses.Texture(input.Value(), ResourceAccess::SrvGraphics)
            .RenderTarget(output.Value(), GraphLoadAction::Clear, {0, 0, 0, 1});
        const render::GraphTexture read = input.Value();
        auto pass = graph.AddFullscreenPass(CopyName, GraphPassFlags::None, accesses, cache,
                                            request, [read](GraphPassContext& context) noexcept {
                                                context.Commands().SetTexture(
                                                    ShaderStage::Pixel, copySourceSlot,
                                                    context.Texture(read));
                                            });
        auto executed = pass ? graph.Execute() : Result<void>::Failure(pass.Error());
        if (!executed) {
            printf("    graph error: %.*s\n", static_cast<int>(graph.LastError().ByteCount()),
                   graph.LastError().Data());
        }

        Check(executed.IsOk(), "the fullscreen graph pass executes");
    }

    Check(device.WaitIdle().IsOk() && device.ReadPixels(target, pixels.View()).IsOk(),
          "graph copy readback");
    Check(MatchesSource(pixels.View(), "graph copy"),
          "AddFullscreenPass copies every texel to the same pixel");
    Check(device.NullTextureBindings() == nullBefore,
          "the RenderGraph fullscreen path binds no null texture");
    (void)handles.BeginRetire(sourceHandle.Value());
    (void)handles.BeginRetire(targetHandle.Value());
    (void)arena.Reset();
}

// --- 내장 자원 팩(11.4절 5–7항) ---

// 팩 텍스처 하나의 mip 0 plane 0 byte가 기대 texel의 반복인지다.
bool TextureHolds(Device& device, TextureHandle texture, Span<const uint8> texel, uint32 slices,
                  Array<uint8>& scratch, const char* label) noexcept {
    const usize bytes = texel.Size() * slices;
    if (!scratch.TryResize(bytes)) {
        return false;
    }

    auto read = device.ReadTexture(texture, 0, 0, scratch.View());
    if (read.Error() == ErrorCode::Unsupported) {
        printf("  [NOT RUN] %s render utility: %s readback (unsupported)\n", backendName, label);
        return true;
    }

    if (!read) {
        printf("    %s readback failed\n", label);
        return false;
    }

    for (usize index = 0; index < bytes; ++index) {
        if (scratch[index] != texel[index % texel.Size()]) {
            printf("    %s byte %u is %u, expected %u\n", label, static_cast<uint32>(index),
                   scratch[index], texel[index % texel.Size()]);
            return false;
        }
    }

    return true;
}

bool BufferHolds(Device& device, BufferHandle buffer, Span<const uint8> expected,
                 Array<uint8>& scratch, const char* label) noexcept {
    if (!scratch.TryResize(expected.Size()) ||
        !device.ReadBuffer(buffer, 0, scratch.View()).IsOk()) {
        printf("    %s readback failed\n", label);
        return false;
    }

    for (usize index = 0; index < expected.Size(); ++index) {
        if (scratch[index] != expected[index]) {
            printf("    %s byte %u differs\n", label, static_cast<uint32>(index));
            return false;
        }
    }

    return true;
}

template <class T, usize N>
Span<const uint8> BytesOf(const T (&values)[N]) noexcept {
    return Span<const uint8>(reinterpret_cast<const uint8*>(values), sizeof(values));
}

// 팩의 텍스처·버퍼가 11.4절 5항의 값인지 읽어 본다(BR02 기준 값).
void CheckPackValues(Device& device, const render::BuiltinResources& pack,
                     Allocator& allocator) noexcept {
    using render::BuiltinBuffer;
    using render::BuiltinTexture;
    Array<uint8> scratch(allocator);
    constexpr uint32 CubeFaces = 6;
    const uint8 white[] = {255, 255, 255, 255};
    const uint8 black[] = {0, 0, 0, 255};
    const uint8 clear[] = {0, 0, 0, 0};
    const uint8 normal[] = {128, 128, 255, 255};
    const uint8 zero32[] = {0, 0, 0, 0};
    Check(TextureHolds(device, pack.Texture(BuiltinTexture::White2D), white, 1, scratch, "white"),
          "the builtin white texture is (1, 1, 1, 1)");
    Check(TextureHolds(device, pack.Texture(BuiltinTexture::Black2D), black, 1, scratch, "black"),
          "the builtin black texture is (0, 0, 0, 1)");
    Check(TextureHolds(device, pack.Texture(BuiltinTexture::Transparent2D), clear, 1, scratch,
                       "transparent"),
          "the builtin transparent texture is (0, 0, 0, 0)");
    Check(TextureHolds(device, pack.Texture(BuiltinTexture::FlatNormal2D), normal, 1, scratch,
                       "flat normal"),
          "the builtin flat normal texture is (128, 128, 255, 255)");
    Check(TextureHolds(device, pack.Texture(BuiltinTexture::White2DArray), white, 1, scratch,
                       "white array"),
          "the builtin white array texture is white");
    Check(
        TextureHolds(device, pack.Texture(BuiltinTexture::White3D), white, 1, scratch, "white 3D"),
        "the builtin white 3D texture is white");
    Check(TextureHolds(device, pack.Texture(BuiltinTexture::BlackCube), black, CubeFaces, scratch,
                       "black cube"),
          "every builtin black cube face is (0, 0, 0, 1)");
    if (!pack.Texture(BuiltinTexture::BlackCubeArray).IsNull()) {
        Check(TextureHolds(device, pack.Texture(BuiltinTexture::BlackCubeArray), clear, CubeFaces,
                           scratch, "black cube array"),
              "every builtin black cube array face is (0, 0, 0, 0)");
    }

    Check(TextureHolds(device, pack.Texture(BuiltinTexture::ZeroUint2D), zero32, 1, scratch,
                       "zero uint"),
          "the builtin uint texture is 0");
    Check(TextureHolds(device, pack.Texture(BuiltinTexture::FarDepth2D), zero32, 1, scratch,
                       "far depth"),
          "the builtin depth texture is 0 (the reversed-Z far plane)");
    // noise 시스템 텍스처(11.5절): GPU가 가진 byte가 CPU 표(UE 5.8과 같은 데이터)와 같다.
    Check(TextureHolds(device, pack.Texture(BuiltinTexture::PerlinGradient2D),
                       render::PerlinGradient2DTexels(), 1, scratch, "noise gradient 2D"),
          "the builtin noise gradient 2D texture matches the CPU table byte for byte");
    Check(TextureHolds(device, pack.Texture(BuiltinTexture::PerlinGradient3D),
                       render::PerlinGradient3DTexels(), 1, scratch, "noise gradient 3D"),
          "the builtin noise gradient 3D texture matches the CPU table byte for byte");
    const float zero[4] = {0, 0, 0, 0};
    const float one[4] = {1, 1, 1, 1};
    const uint32 zeroUint[1] = {0};
    const float identity[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    Check(BufferHolds(device, pack.Buffer(BuiltinBuffer::ZeroFloat4), BytesOf(zero), scratch,
                      "zero float4"),
          "the builtin zero buffer is (0, 0, 0, 0)");
    Check(BufferHolds(device, pack.Buffer(BuiltinBuffer::OneFloat4), BytesOf(one), scratch,
                      "one float4"),
          "the builtin one buffer is (1, 1, 1, 1)");
    Check(BufferHolds(device, pack.Buffer(BuiltinBuffer::ZeroUint), BytesOf(zeroUint), scratch,
                      "zero uint"),
          "the builtin uint buffer is 0");
    Check(BufferHolds(device, pack.Buffer(BuiltinBuffer::IdentityAffine), BytesOf(identity),
                      scratch, "identity affine"),
          "the builtin identity buffer holds the three affine columns");
}

// 8-bit 값을 [0, 1]로 바꾼다.
float Unit8(uint8 value) noexcept {
    return static_cast<float>(value) / 255.0f;
}

uint8 ToUnit8(float value) noexcept {
    const float clamped = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
    return static_cast<uint8>(clamped * 255.0f + 0.5f);
}

// 블렌드 프리셋의 CPU 기대값이다(소스 s, 대상 d, 채널마다).
struct BlendCase {
    const char* name;
    graphics::ColorTargetBlend blend;
};

uint8 Expected(const graphics::ColorTargetBlend& blend, const uint8 (&source)[4],
               const uint8 (&target)[4], uint32 channel) noexcept {
    const float s = Unit8(source[channel]);
    const float d = Unit8(target[channel]);
    const float sa = Unit8(source[3]);
    if (!blend.enable) {
        return source[channel];
    }

    const auto factor = [&](graphics::BlendFactor value) noexcept {
        switch (value) {
        case graphics::BlendFactor::Zero:
            return 0.0f;
        case graphics::BlendFactor::One:
            return 1.0f;
        case graphics::BlendFactor::SourceAlpha:
            return sa;
        case graphics::BlendFactor::InverseSourceAlpha:
            return 1.0f - sa;
        default:
            return 0.0f;
        }
    };
    const bool alpha = channel == 3;
    const float sourceFactor = factor(alpha ? blend.sourceAlpha : blend.sourceColor);
    const float destinationFactor = factor(alpha ? blend.destinationAlpha : blend.destinationColor);
    return ToUnit8(s * sourceFactor + d * destinationFactor);
}

constexpr int BlendTolerance = 2;

// 블렌드 프리셋: 1×1 원본을 전체화면 복사로 clear한 대상 위에 섞는다.
void CheckBlendPresets(Device& device, render::PipelineCache& cache,
                       const render::GraphicsPipelineRequest& copy, TextureHandle target,
                       Array<uint8>& pixels) noexcept {
    const uint8 source[4] = {204, 102, 51, 128};
    const uint8 background[4] = {51, 51, 102, 255};
    TextureDesc sourceDesc = SourceDesc();
    sourceDesc.width = 1;
    sourceDesc.height = 1;
    auto texture = device.CreateTexture(sourceDesc, Span<const uint8>(source, sizeof(source)));
    Check(texture.IsOk(), "the blend source texture is created");
    if (!texture) {
        return;
    }

    const BlendCase cases[] = {{"opaque", render::OpaqueBlend()},
                               {"alpha", render::AlphaBlend()},
                               {"premultiplied", render::PremultipliedAlphaBlend()},
                               {"additive", render::AdditiveBlend()}};
    for (const BlendCase& entry : cases) {
        render::GraphicsPipelineRequest request = copy;
        render::SetBlend(request.state, entry.blend);
        auto pipeline = cache.FindOrCreate(request);
        ClearColor clearColor{Unit8(background[0]), Unit8(background[1]), Unit8(background[2]),
                              Unit8(background[3])};
        auto frame = pipeline ? device.BeginOffscreen(target, clearColor)
                              : Result<CommandContext*>::Failure(pipeline.Error());
        Check(frame.IsOk(), "a blend preset frame begins");
        if (!frame) {
            continue;
        }

        CommandContext& commands = *frame.Value();
        commands.SetPipeline(pipeline.Value());
        commands.SetTexture(ShaderStage::Pixel, copySourceSlot, texture.Value());
        render::DrawFullscreenTriangle(commands);
        Check(device.EndFrame().IsOk() && device.ReadPixels(target, pixels.View()).IsOk(),
              "blend preset readback");
        bool same = true;
        for (uint32 channel = 0; channel < 4; ++channel) {
            const int expected = Expected(entry.blend, source, background, channel);
            const int actual = pixels[channel];
            if (actual - expected > BlendTolerance || expected - actual > BlendTolerance) {
                printf("    %s blend channel %u is %d, expected %d\n", entry.name, channel, actual,
                       expected);
                same = false;
            }
        }

        Check(same, "blend presets match the CPU blend equation");
    }

    device.Retire(texture.Value());
}

// fallback 메시 셰이더 그리기의 local → clip이다(행 벡터 p·M, 행 우선): 위에서 내려다본 직교
// 투영 — clip.x = y/50(+Y 오른쪽), clip.y = x/50(+X 위), clip.z = 0.5 + z/200(reversed-Z — +Z가
// 가까움). 기본 메시 100 cm 면이 대상 전체를 덮는다.
render::BuiltinMeshConstants TopDown(float r, float g, float b) noexcept {
    render::BuiltinMeshConstants constants;
    constants.localToClip[0][1] = 1.0f / 50.0f;
    constants.localToClip[1][0] = 1.0f / 50.0f;
    constants.localToClip[2][2] = 1.0f / 200.0f;
    constants.localToClip[3][2] = 0.5f;
    constants.localToClip[3][3] = 1.0f;
    constants.color[0] = r;
    constants.color[1] = g;
    constants.color[2] = b;
    constants.color[3] = 1.0f;
    return constants;
}

struct MeshBench {
    Device* device = nullptr;
    render::PipelineCache* cache = nullptr;
    const render::BuiltinResources* pack = nullptr;
    render::ShaderSet* set = nullptr;
    TextureHandle color;
    TextureHandle depth;
    Array<uint8>* pixels = nullptr;

    Result<PipelineHandle> Pipeline(graphics::RasterState raster,
                                    graphics::DepthStencilState depthState) noexcept {
        render::GraphicsPipelineRequest request;
        request.set = set;
        request.vertexEntry = pack->ShaderEntries().meshVertex;
        request.pixelEntry = pack->ShaderEntries().meshPixel;
        request.state.layout = render::BuiltinVertexLayout();
        request.state.colorFormats[0] = PixelFormat::RGBA8Unorm;
        request.state.depthFormat = PixelFormat::D32Float;
        request.state.raster = raster;
        request.state.depthStencil = depthState;
        request.state.drawConstantBytes = sizeof(render::BuiltinMeshConstants);
        return cache->FindOrCreate(request);
    }

    void Draw(CommandContext& commands, PipelineHandle pipeline, render::BuiltinMesh mesh,
              const render::BuiltinMeshConstants& constants) noexcept {
        const render::BuiltinMeshBuffers& buffers = pack->Mesh(mesh);
        commands.SetPipeline(pipeline);
        (void)commands.SetDrawConstants(
            Span<const uint8>(reinterpret_cast<const uint8*>(&constants), sizeof(constants)));
        auto white = pack->FallbackTexture(shader::ShaderResourceShape::Texture2D,
                                           shader::ShaderComponentType::Float);
        commands.SetTexture(
            ShaderStage::Pixel,
            shaders::TextureSlot(set->BindingLayout(pack->ShaderEntries().meshPixel),
                                 render::BuiltinMeshAlbedoName),
            white ? white.Value() : TextureHandle{});
        const VertexBufferBinding vertices[] = {{buffers.vertices, 0}};
        commands.SetVertexBuffers(0, vertices);
        commands.SetIndexBuffer(buffers.indices, buffers.indexFormat);
        commands.DrawIndexed(buffers.indexCount);
    }

    // 장면 하나를 그리고 가운데 픽셀이 기대 색인지다.
    template <class Record>
    bool Frame(Record&& record, uint8 r, uint8 g, uint8 b, const char* label) noexcept {
        PassDesc pass;
        pass.colorCount = 1;
        pass.colors[0].texture = color;
        pass.colors[0].clear = ClearColor{0, 0, 0, 1};
        pass.depth.texture = depth;
        pass.depth.clearDepth = 0.0f;
        auto frame = device->BeginPass(pass);
        if (!frame) {
            printf("    %s pass failed\n", label);
            return false;
        }

        record(*frame.Value());
        if (!device->EndFrame() || !device->ReadPixels(color, pixels->View())) {
            printf("    %s readback failed\n", label);
            return false;
        }

        bool same = true;
        for (uint32 corner = 0; corner < TargetSize * TargetSize; ++corner) {
            const uint8* pixel = pixels->Data() + corner * BytesPerPixel;
            same = same && pixel[0] == r && pixel[1] == g && pixel[2] == b;
        }

        if (!same) {
            const uint8* pixel = pixels->Data();
            printf("    %s first pixel is (%u, %u, %u), expected (%u, %u, %u)\n", label, pixel[0],
                   pixel[1], pixel[2], r, g, b);
        }

        return same;
    }
};

// 기본 메시를 fallback 셰이더(BuiltinVertexInput 정점 입력 축)로 그려 컬·깊이 프리셋을 본다.
void CheckBuiltinMeshes(Device& device, render::PipelineCache& cache, render::ShaderSet& set,
                        const render::BuiltinResources& pack, TextureHandle color,
                        Array<uint8>& pixels) noexcept {
    TextureDesc depthDesc = SourceDesc();
    depthDesc.format = PixelFormat::D32Float;
    depthDesc.usage = TextureUsage::DepthStencil;
    auto depth = device.CreateTexture(depthDesc, {});
    Check(depth.IsOk(), "the mesh depth target is created");
    if (!depth) {
        return;
    }

    MeshBench bench;
    bench.device = &device;
    bench.cache = &cache;
    bench.pack = &pack;
    bench.set = &set;
    bench.color = color;
    bench.depth = depth.Value();
    bench.pixels = &pixels;
    auto backPlane = bench.Pipeline(render::BackFaceCull(), render::DepthDisabled());
    auto frontPlane = bench.Pipeline(render::FrontFaceCull(), render::DepthDisabled());
    auto backCube = bench.Pipeline(render::BackFaceCull(), render::DepthReadWrite());
    auto frontCube = bench.Pipeline(render::FrontFaceCull(), render::DepthReadWrite());
    auto depthRead = bench.Pipeline(render::BackFaceCull(), render::DepthRead());
    Check(backPlane.IsOk() && frontPlane.IsOk() && backCube.IsOk() && frontCube.IsOk() &&
              depthRead.IsOk(),
          "fallback mesh pipelines are created");
    if (!backPlane || !frontPlane || !backCube || !frontCube || !depthRead) {
        device.Retire(depth.Value());
        return;
    }

    const uint64 nullBefore = device.NullTextureBindings();
    const render::BuiltinMeshConstants red = TopDown(1, 0, 0);
    const render::BuiltinMeshConstants green = TopDown(0, 1, 0);
    // 1) 위에서 본 plane은 앞면이다: 뒤면 컬이면 그려지고 앞면 컬이면 버려진다.
    Check(bench.Frame(
              [&](CommandContext& commands) noexcept {
                  bench.Draw(commands, backPlane.Value(), render::BuiltinMesh::Plane, red);
              },
              255, 0, 0, "plane back-face cull"),
          "the builtin plane faces +Z (front face from above)");
    Check(bench.Frame(
              [&](CommandContext& commands) noexcept {
                  bench.Draw(commands, frontPlane.Value(), render::BuiltinMesh::Plane, red);
              },
              0, 0, 0, "plane front-face cull"),
          "front-face culling discards the builtin plane seen from above");
    // 2) cube 윗면(깊이 0.75)을 쓴 뒤 plane(0.5)의 깊이 읽기는 실패한다.
    Check(bench.Frame(
              [&](CommandContext& commands) noexcept {
                  bench.Draw(commands, backCube.Value(), render::BuiltinMesh::Cube, red);
                  bench.Draw(commands, depthRead.Value(), render::BuiltinMesh::Plane, green);
              },
              255, 0, 0, "cube top then plane depth read"),
          "depth read keeps the nearer cube top face (reversed-Z GreaterEqual)");
    // 3) 앞면 컬 cube는 아랫면(0.25)만 쓰므로 plane의 깊이 읽기가 통과한다.
    Check(bench.Frame(
              [&](CommandContext& commands) noexcept {
                  bench.Draw(commands, frontCube.Value(), render::BuiltinMesh::Cube, red);
                  bench.Draw(commands, depthRead.Value(), render::BuiltinMesh::Plane, green);
              },
              0, 255, 0, "cube bottom then plane depth read"),
          "front-face culling leaves the cube bottom face and depth read passes in front of it");
    Check(device.NullTextureBindings() == nullBefore,
          "fallback mesh draws bind no null texture (the pack white texture is bound)");
    device.Retire(depth.Value());
}

} // namespace

uint32 RunRenderUtilityChecks(const char* name, Device& device, Allocator& allocator) noexcept {
    failures = 0;
    backendName = name;
    render::ShaderSet* set = shaders::ActiveSet();
    if (set == nullptr) {
        printf("  [NOT RUN] %s render utility: no shader set\n", name);
        return 0;
    }

    auto copy = set->FindEntry(render::CopyTexturePixelType.hash);
    Check(copy.IsOk(), "the engine shader set has CopyTexturePixel");
    if (!copy) {
        return failures;
    }

    auto request = render::FullscreenPipelineRequest(*set, copy.Value(), PixelFormat::RGBA8Unorm);
    Check(request.IsOk(), "a fullscreen pipeline request is made");
    if (!request) {
        return failures;
    }

    copySourceSlot =
        shaders::TextureSlot(set->BindingLayout(copy.Value()), render::CopyTextureSourceName);
    render::PipelineCache cache(allocator, device);
    auto pipeline = cache.FindOrCreate(request.Value());
    Check(pipeline.IsOk(), "the fullscreen copy pipeline is created");
    Array<uint8> pixels(allocator);
    Array<uint8> initial(allocator);
    Check(pixels.TryResize(TargetBytes).IsOk() && initial.TryResize(TargetBytes).IsOk(),
          "pixel buffers");
    if (!pipeline || pixels.Size() != TargetBytes || initial.Size() != TargetBytes) {
        return failures;
    }

    FillSource(initial.View());
    auto source = device.CreateTexture(SourceDesc(), initial.View());
    auto target = device.CreateTexture(TargetDesc(), {});
    Check(source.IsOk() && target.IsOk(), "fullscreen copy textures are created");
    if (source && target) {
        const uint64 validationBefore = device.ValidationErrorCount();
        RunDirectCopy(device, pipeline.Value(), source.Value(), target.Value(), pixels);
        RunGraphCopy(device, cache, request.Value(), source.Value(), target.Value(), pixels,
                     allocator);
        // 팩은 엔진 렌더 자원이 집합과 함께 만든 것이다(RenderBase 9.12절 2항).
        const render::BuiltinResources* pack = shaders::ActiveBuiltins();
        Check(pack != nullptr && pack->IsReady(),
              "the engine render resources opened the builtin resource pack");
        if (pack != nullptr && pack->IsReady()) {
            CheckPackValues(device, *pack, allocator);
            CheckBlendPresets(device, cache, request.Value(), target.Value(), pixels);
            CheckBuiltinMeshes(device, cache, *set, *pack, target.Value(), pixels);
        }

        Check(device.ValidationErrorCount() == validationBefore,
              "fullscreen draws raise no validation errors");
    }

    cache.Clear();
    if (source) {
        device.Retire(source.Value());
    }

    if (target) {
        device.Retire(target.Value());
    }

    (void)device.WaitIdle();
    return failures;
}

} // namespace sandbox
