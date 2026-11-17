#include "RenderGraphSelfTest.h"

#include <Core/Containers/Array.h>
#include <Core/Memory/FrameArena.h>
#include <RenderGraph/RenderGraph.h>
#include <stdio.h>

namespace sandbox {

namespace {

using namespace rocks;
using namespace rocks::graphics;
using rocks::render::GraphLoadAction;
using rocks::render::GraphPassAccesses;
using rocks::render::GraphPassContext;
using rocks::render::GraphPassFlags;
using rocks::render::GraphPassKind;
using rocks::render::GraphResourcePool;
using rocks::render::GraphTexture;
using rocks::render::RenderGraph;
using rocks::render::RenderGraphDesc;
using rocks::render::RenderHandle;
using rocks::render::RenderHandleTable;
using rocks::render::RenderResourceKind;

// 대상 한 변이다.
constexpr uint32 TargetSize = 32;
// 채널 허용 오차다(RenderBase 8절 공통 기준 ±8).
constexpr int ChannelTolerance = 8;

TraceScopeName SceneName("SelfTest.Scene");
TraceScopeName UnusedName("SelfTest.Unused");
TraceScopeName CompositeName("SelfTest.Composite");

uint32 failures = 0;
const char* backendName = "";

void Check(bool condition, const char* what) noexcept {
    if (!condition) {
        ++failures;
        printf("  [FAIL] %s render graph: %s\n", backendName, what);
    }
}

// 로컬 Y(오른쪽)·Z(위)를 clip X·Y로 보내 단위 사각형이 화면을 가득 채우는 변환이다(UE 좌표).
Mat4 FillClip() noexcept {
    Mat4 matrix = Mat4::Zero();
    matrix.m[4] = 2.0f;
    matrix.m[9] = 2.0f;
    matrix.m[14] = 0.5f;
    matrix.m[15] = 1.0f;
    return matrix;
}

bool CenterIs(const Array<uint8>& pixels, uint8 r, uint8 g, uint8 b) noexcept {
    const usize offset = (static_cast<usize>(TargetSize / 2) * TargetSize + TargetSize / 2) * 4;
    auto near = [](uint8 actual, uint8 expected) noexcept {
        const int difference = static_cast<int>(actual) - static_cast<int>(expected);
        return difference <= ChannelTolerance && difference >= -ChannelTolerance;
    };

    const bool same =
        near(pixels[offset], r) && near(pixels[offset + 1], g) && near(pixels[offset + 2], b);
    if (!same) {
        printf("    center (%u, %u, %u), expected (%u, %u, %u)\n", pixels[offset],
               pixels[offset + 1], pixels[offset + 2], r, g, b);
    }

    return same;
}

struct GraphBench {
    Device* device = nullptr;
    sample::Renderer* renderer = nullptr;
    RenderHandleTable* handles = nullptr;
    GraphResourcePool* pool = nullptr;
    FrameArena* arena = nullptr;
    TextureHandle redTexture;
    uint64 frame = 0;

    RenderGraphDesc Desc() noexcept {
        RenderGraphDesc desc;
        desc.device = device;
        desc.handles = handles;
        desc.pool = pool;
        desc.arena = arena;
        desc.frame = ++frame;
        return desc;
    }
};

TextureDesc TargetDesc() noexcept {
    TextureDesc desc;
    desc.width = TargetSize;
    desc.height = TargetSize;
    desc.usage = TextureUsage::RenderTarget;
    desc.sampler.filter = Filter::Nearest;
    return desc;
}

// texture를 화면 가득 그리는 pass 함수다(null이면 그래프 자원 source를 읽는다).
auto FullScreen(sample::Renderer* renderer, TextureHandle texture, GraphTexture source,
                LinearColor color) noexcept {
    return [renderer, texture, source, color](GraphPassContext& pass) noexcept {
        sample::RenderView view;
        view.viewProjection = FillClip();
        sample::RenderItem item;
        item.texture = source.IsNull() ? texture : pass.Texture(source);
        item.color = color;
        renderer->Draw(pass.Commands(), view, Span<const sample::RenderItem>(&item, 1));
    };
}

// 장면(빨강) → 쓰이지 않는 파랑 pass → 합성 pass로 output을 채운다. exportScene이면 장면을
// history로 내보낸다. history가 있으면 장면 대신 그것을 합성한다.
bool RunGraph(GraphBench& bench, RenderHandle output, RenderHandle history,
              RenderHandle* exported) noexcept {
    RenderGraph graph(bench.Desc());
    rocks::render::GraphImportOptions outputOptions;
    outputOptions.finalAccess = ResourceAccess::SrvGraphics;
    auto target = graph.ImportTexture(output, "Output", outputOptions);
    auto scene = graph.CreateTexture(TargetDesc(), "Scene");
    auto unused = graph.CreateTexture(TargetDesc(), "Unused");
    if (!target || !scene || !unused) {
        return false;
    }

    GraphTexture source = scene.Value();
    if (!history.IsNull()) {
        rocks::render::GraphImportOptions historyOptions;
        historyOptions.initialAccess = ResourceAccess::SrvGraphics;
        auto previous = graph.ImportTexture(history, "History", historyOptions);
        if (!previous) {
            return false;
        }

        source = previous.Value();
    }

    GraphPassAccesses sceneAccesses(graph);
    sceneAccesses.RenderTarget(scene.Value(), GraphLoadAction::Clear, {0, 0, 0, 1});
    auto scenePass =
        graph.AddPass(SceneName, GraphPassKind::Raster, GraphPassFlags::None, sceneAccesses,
                      FullScreen(bench.renderer, bench.redTexture, {}, LinearColor(1, 1, 1, 1)));
    // 결과를 아무도 읽지 않으므로 제거된다. 실행되면 output이 파랗게 보이게 장면을 덮어쓴다.
    GraphPassAccesses unusedAccesses(graph);
    unusedAccesses.Texture(scene.Value(), ResourceAccess::SrvGraphics)
        .RenderTarget(unused.Value(), GraphLoadAction::Clear, {0, 0, 1, 1});
    auto unusedPass =
        graph.AddPass(UnusedName, GraphPassKind::Raster, GraphPassFlags::None, unusedAccesses,
                      FullScreen(bench.renderer, {}, scene.Value(), LinearColor(0, 0, 1, 1)));
    GraphPassAccesses compositeAccesses(graph);
    compositeAccesses.Texture(source, ResourceAccess::SrvGraphics)
        .RenderTarget(target.Value(), GraphLoadAction::Clear, {0, 1, 0, 1});
    auto compositePass =
        graph.AddPass(CompositeName, GraphPassKind::Raster, GraphPassFlags::None, compositeAccesses,
                      FullScreen(bench.renderer, {}, source, LinearColor(1, 1, 1, 1)));
    if (!scenePass || !unusedPass || !compositePass) {
        printf("    graph error: %.*s\n", static_cast<int>(graph.LastError().ByteCount()),
               graph.LastError().Data());
        return false;
    }

    if (exported != nullptr) {
        auto handle = graph.ExportTexture(scene.Value());
        if (!handle) {
            return false;
        }

        *exported = handle.Value();
    }

    auto executed = graph.Execute();
    if (!executed) {
        printf("    graph error: %.*s\n", static_cast<int>(graph.LastError().ByteCount()),
               graph.LastError().Data());
        return false;
    }

    Check(graph.IsCulled(unusedPass.Value()), "unused pass is culled");
    const bool historyCulled = !history.IsNull() && exported == nullptr;
    Check(graph.IsCulled(scenePass.Value()) == historyCulled,
          "scene pass runs unless history replaces it");
    Check(graph.Stats().transitions >= 1, "graph lowers explicit transitions");
    return true;
}

} // namespace

uint32 RunRenderGraphChecks(const char* name, Device& device, sample::Renderer& renderer,
                            Allocator& allocator) noexcept {
    failures = 0;
    backendName = name;
    const uint64 validationBefore = device.ValidationErrorCount();
    RenderHandleTable handles(allocator);
    GraphResourcePool pool(allocator, device);
    FrameArena arena(allocator);
    GraphBench bench;
    bench.device = &device;
    bench.renderer = &renderer;
    bench.handles = &handles;
    bench.pool = &pool;
    bench.arena = &arena;

    const uint8 red[4] = {255, 0, 0, 255};
    TextureDesc redDesc;
    redDesc.width = 1;
    redDesc.height = 1;
    redDesc.sampler.filter = Filter::Nearest;
    auto redTexture = device.CreateTexture(redDesc, Span<const uint8>(red, sizeof(red)));
    auto outputTexture = device.CreateTexture(TargetDesc(), {});
    auto output = handles.Reserve(RenderResourceKind::Texture);
    if (!redTexture || !outputTexture || !output ||
        !handles.BindTexture(output.Value(), outputTexture.Value())) {
        Check(false, "resource creation");
        return failures;
    }

    bench.redTexture = redTexture.Value();
    Array<uint8> pixels(allocator);
    bool ok = pixels.TryResize(static_cast<usize>(TargetSize) * TargetSize * 4).IsOk();

    // 1: 장면 → 합성(장면은 history로 내보낸다).
    RenderHandle history;
    ok = ok && RunGraph(bench, output.Value(), {}, &history);
    Check(ok && device.ReadPixels(outputTexture.Value(), pixels.View()).IsOk() &&
              CenterIs(pixels, 255, 0, 0),
          "composite of the scene texture is red (blue unused pass culled)");
    // 2: 다음 그래프가 history를 import해 합성한다(장면 pass는 쓰이지 않아 제거된다).
    if (ok) {
        auto cleared = arena.Reset();
        (void)cleared;
        ok = RunGraph(bench, output.Value(), history, nullptr);
        Check(ok && device.ReadPixels(outputTexture.Value(), pixels.View()).IsOk() &&
                  CenterIs(pixels, 255, 0, 0),
              "imported history composites the previous scene");
    }

    // 3·4: export하지 않는 그래프 둘. 앞 그래프가 풀에 돌려준 장면 텍스처를 뒤 그래프가 다시
    // 쓴다(첫 그래프의 장면은 history로 넘어가 풀 밖에 있다).
    for (uint32 run = 0; ok && run < 2; ++run) {
        auto cleared = arena.Reset();
        (void)cleared;
        ok = RunGraph(bench, output.Value(), {}, nullptr);
        Check(ok && device.ReadPixels(outputTexture.Value(), pixels.View()).IsOk() &&
                  CenterIs(pixels, 255, 0, 0),
              "graph with a pooled scene texture composites red");
    }

    const rocks::render::GraphPoolStats stats = pool.Stats();
    Check(stats.reused >= 1, "pool reuses graph textures across graphs");
    const uint64 validationErrors = device.ValidationErrorCount() - validationBefore;
    Check(validationErrors == 0, "validation errors (incl. transition mismatches) are zero");
    printf("  %s render graph: pool created %llu, reused %llu, validation errors %llu\n", name,
           static_cast<unsigned long long>(stats.created),
           static_cast<unsigned long long>(stats.reused),
           static_cast<unsigned long long>(validationErrors));

    auto historyTexture = handles.Texture(history);
    if (historyTexture) {
        device.Retire(historyTexture.Value());
    }

    device.Retire(redTexture.Value());
    device.Retire(outputTexture.Value());
    pool.Clear();
    auto idle = device.WaitIdle();
    Check(idle.IsOk(), "wait idle");
    return failures;
}

} // namespace sandbox
