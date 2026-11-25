#include "RenderUtilitySelfTest.h"
#include "SandboxShaders.h"

#include <Core/Containers/Array.h>
#include <Core/Memory/FrameArena.h>
#include <RenderBase/EngineShaderTypes.h>
#include <RenderBase/FullscreenPass.h>
#include <RenderBase/PipelineCache.h>
#include <RenderBase/RenderHandle.h>
#include <RenderGraph/GraphResourcePool.h>
#include <RenderGraph/RenderGraph.h>
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
    commands.SetTexture(source);
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
    again.SetTexture(TextureHandle{});
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
        auto pass =
            graph.AddFullscreenPass(CopyName, GraphPassFlags::None, accesses, cache, request,
                                    [read](GraphPassContext& context) noexcept {
                                        context.Commands().SetTexture(context.Texture(read));
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

    request.Value().state.bindings.texture = render::CopyTextureSourceName;
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
