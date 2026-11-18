#include "RenderGraphSelfTest.h"

#include <Core/Containers/Array.h>
#include <Core/Memory/FrameArena.h>
#include <Platform/Memory.h>
#include <RenderGraph/RenderGraph.h>
#include <stdio.h>

namespace sandbox {

namespace {

using namespace rocks;
using namespace rocks::graphics;
using rocks::render::GraphBuffer;
using rocks::render::GraphLoadAction;
using rocks::render::GraphPassAccesses;
using rocks::render::GraphPassContext;
using rocks::render::GraphPassFlags;
using rocks::render::GraphPassKind;
using rocks::render::GraphResourcePool;
using rocks::render::GraphTexture;
using rocks::render::ReleaseRetiredResource;
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
TraceScopeName CopyName("SelfTest.Copy");
TraceScopeName ClearName("SelfTest.Clear");

// 업로드·복사 시험 버퍼 크기다(4 byte 배수).
constexpr uint32 CopyBytes = 256;

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
bool RunGraph(GraphBench& bench, RenderHandle output, RenderHandle history, RenderHandle* exported,
              ResourceAccess outputFinal = ResourceAccess::SrvGraphics) noexcept {
    RenderGraph graph(bench.Desc());
    rocks::render::GraphImportOptions outputOptions;
    outputOptions.finalAccess = outputFinal;
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

// 그래프 오류를 찍는다.
void PrintGraphError(const RenderGraph& graph) noexcept {
    printf("    graph error: %.*s\n", static_cast<int>(graph.LastError().ByteCount()),
           graph.LastError().Data());
}

// 같은 그래프 안 재사용(RG2): A(빨강) → B(A를 읽음) → C(파랑으로 clear, A가 놓은 객체를 이어 씀) →
// output(B를 합성) · second(C를 합성). B가 빨강이면 C가 A의 객체를 이어 쓴 뒤에도 앞 pass의 결과가
// 맞고, C가 파랑이면 이어 쓴 객체의 이전 내용이 새지 않았다.
bool RunReuseGraph(GraphBench& bench, RenderHandle output, RenderHandle second) noexcept {
    RenderGraph graph(bench.Desc());
    auto first = graph.ImportTexture(output, "Output");
    auto other = graph.ImportTexture(second, "Second");
    auto a = graph.CreateTexture(TargetDesc(), "A");
    auto b = graph.CreateTexture(TargetDesc(), "B");
    auto c = graph.CreateTexture(TargetDesc(), "C");
    if (!first || !other || !a || !b || !c) {
        PrintGraphError(graph);
        return false;
    }

    GraphPassAccesses a0(graph);
    a0.RenderTarget(a.Value(), GraphLoadAction::Clear, {0, 0, 0, 1});
    auto p0 =
        graph.AddPass(SceneName, GraphPassKind::Raster, GraphPassFlags::None, a0,
                      FullScreen(bench.renderer, bench.redTexture, {}, LinearColor(1, 1, 1, 1)));
    GraphPassAccesses a1(graph);
    a1.Texture(a.Value(), ResourceAccess::SrvGraphics)
        .RenderTarget(b.Value(), GraphLoadAction::Clear, {0, 1, 0, 1});
    auto p1 = graph.AddPass(CompositeName, GraphPassKind::Raster, GraphPassFlags::None, a1,
                            FullScreen(bench.renderer, {}, a.Value(), LinearColor(1, 1, 1, 1)));
    GraphPassAccesses a2(graph);
    a2.RenderTarget(c.Value(), GraphLoadAction::Clear, {0, 0, 1, 1});
    auto p2 = graph.AddPass(ClearName, GraphPassKind::Raster, GraphPassFlags::None, a2,
                            [](GraphPassContext& pass) noexcept { (void)pass; });
    GraphPassAccesses a3(graph);
    a3.Texture(b.Value(), ResourceAccess::SrvGraphics)
        .RenderTarget(first.Value(), GraphLoadAction::Clear, {0, 0, 0, 1});
    auto p3 = graph.AddPass(CompositeName, GraphPassKind::Raster, GraphPassFlags::None, a3,
                            FullScreen(bench.renderer, {}, b.Value(), LinearColor(1, 1, 1, 1)));
    GraphPassAccesses a4(graph);
    a4.Texture(c.Value(), ResourceAccess::SrvGraphics)
        .RenderTarget(other.Value(), GraphLoadAction::Clear, {0, 0, 0, 1});
    auto p4 = graph.AddPass(CompositeName, GraphPassKind::Raster, GraphPassFlags::None, a4,
                            FullScreen(bench.renderer, {}, c.Value(), LinearColor(1, 1, 1, 1)));
    if (!p0 || !p1 || !p2 || !p3 || !p4 || !graph.Execute()) {
        PrintGraphError(graph);
        return false;
    }

    Check(graph.Lifetime(c.Value()).reusedWithinGraph, "C takes the object A released");
    Check(graph.Stats().reusedWithinGraph == 1, "one in-graph reuse");
    return true;
}

// 그래프 안 업로드와 버퍼 재사용: 업로드한 source를 복사 pass가 middle로, 다시 last로 복사한다.
// last는 source가 놓은 객체를 이어 쓴다(버퍼 첫 전이 before Discard를 장치에 내린다). middle·last를
// export해 바이트를 읽는다. 기록을 여는 clear pass가 앞에 있다(열린 기록 없는 Copy pass는 GX4 전).
bool RunUploadCopyGraph(GraphBench& bench, const uint8* data, RenderHandle* middleOut,
                        RenderHandle* lastOut) noexcept {
    RenderGraph graph(bench.Desc());
    auto target = graph.CreateTexture(TargetDesc(), "Opener");
    BufferDesc desc;
    desc.size = CopyBytes;
    auto source = graph.CreateBuffer(desc, "Source");
    auto middle = graph.CreateBuffer(desc, "Middle");
    auto last = graph.CreateBuffer(desc, "Last");
    if (!target || !source || !middle || !last ||
        !graph.UploadBuffer(source.Value(), Span<const uint8>(data, CopyBytes))) {
        PrintGraphError(graph);
        return false;
    }

    GraphPassAccesses opener(graph);
    opener.RenderTarget(target.Value(), GraphLoadAction::Clear, {0, 0, 0, 1});
    auto p0 = graph.AddPass(ClearName, GraphPassKind::Raster, GraphPassFlags::HasSideEffects,
                            opener, [](GraphPassContext& pass) noexcept { (void)pass; });
    auto copy = [](GraphBuffer to, GraphBuffer from) noexcept {
        return [to, from](GraphPassContext& pass) noexcept {
            auto copied =
                pass.Commands().CopyBuffer(pass.Buffer(to), 0, pass.Buffer(from), 0, CopyBytes);
            Check(copied.IsOk(), "graph copy pass copies the buffer");
        };
    };
    GraphPassAccesses c1(graph);
    c1.Buffer(source.Value(), ResourceAccess::CopySource)
        .Buffer(middle.Value(), ResourceAccess::CopyDestination);
    auto p1 = graph.AddPass(CopyName, GraphPassKind::Copy, GraphPassFlags::None, c1,
                            copy(middle.Value(), source.Value()));
    GraphPassAccesses c2(graph);
    c2.Buffer(middle.Value(), ResourceAccess::CopySource)
        .Buffer(last.Value(), ResourceAccess::CopyDestination);
    auto p2 = graph.AddPass(CopyName, GraphPassKind::Copy, GraphPassFlags::None, c2,
                            copy(last.Value(), middle.Value()));
    auto middleHandle = graph.ExportBuffer(middle.Value());
    auto lastHandle = graph.ExportBuffer(last.Value());
    if (!p0 || !p1 || !p2 || !middleHandle || !lastHandle || !graph.Execute()) {
        PrintGraphError(graph);
        return false;
    }

    Check(graph.Stats().uploads == 1, "the upload runs once");
    Check(graph.Lifetime(last.Value()).reusedWithinGraph, "Last takes the object Source released");
    *middleOut = middleHandle.Value();
    *lastOut = lastHandle.Value();
    return true;
}

// export handle을 Retire한다(렌더 lane의 Retire 경로와 같다: 풀 반납, 결정 R15).
void RetireExport(RenderHandleTable& handles, Device& device, RenderHandle handle) noexcept {
    auto retired = handles.BeginRetire(handle);
    Check(retired && !retired.Value().deferred, "export handle retires without leases");
    if (retired) {
        ReleaseRetiredResource(device, handle, retired.Value());
    }
}

// export한 버퍼 바이트가 data와 같은지다.
bool BufferMatches(Device& device, RenderHandleTable& handles, RenderHandle handle,
                   const uint8* data) noexcept {
    auto buffer = handles.Buffer(handle);
    uint8 read[CopyBytes] = {};
    if (!buffer || !device.ReadBuffer(buffer.Value(), 0, Span<uint8>(read)).IsOk()) {
        return false;
    }

    for (uint32 index = 0; index < CopyBytes; ++index) {
        if (read[index] != data[index]) {
            printf("    byte %u is %u, expected %u\n", index, read[index], data[index]);
            return false;
        }
    }

    return true;
}

// Sampled 그래프 텍스처(1×1)에 color를 올려 output에 합성한다(두 번째부터는 풀이 mip 갱신으로
// 올린다).
// 분리 전이(RG2 B부) 그래프다. 같은 장치 기록 안에서 생산 pass와 소비 Copy pass 사이에 다른 pass가
// 있어 텍스처(복사 대상 → 원본, 이미지 layout 전이)와 버퍼(복사 대상 → 원본)가 begin·end로 나뉜다:
//   p0 Raster A ← 빨강 / p1 Raster opener(새 기록) / p2 Copy C ← A / p3 Copy M ← 업로드 S /
//   p4 Copy D ← C (C 분리) / p5 Copy L ← M (M 분리) / p6 Raster output ← D 합성
// split이 거짓이면 같은 그래프를 분리 없이 낸다. L은 export해 바이트를 읽는다.
bool RunSplitGraph(GraphBench& bench, RenderHandle output, const uint8* data, bool split,
                   RenderHandle* lastOut, uint32* splitCount) noexcept {
    RenderGraphDesc desc = bench.Desc();
    desc.splitTransitions = split;
    RenderGraph graph(desc);
    auto target = graph.ImportTexture(output, "Output");
    auto a = graph.CreateTexture(TargetDesc(), "A");
    auto opener = graph.CreateTexture(TargetDesc(), "Opener");
    auto c = graph.CreateTexture(TargetDesc(), "C");
    auto d = graph.CreateTexture(TargetDesc(), "D");
    BufferDesc bufferDesc;
    bufferDesc.size = CopyBytes;
    auto source = graph.CreateBuffer(bufferDesc, "Source");
    auto middle = graph.CreateBuffer(bufferDesc, "Middle");
    auto last = graph.CreateBuffer(bufferDesc, "Last");
    if (!target || !a || !opener || !c || !d || !source || !middle || !last ||
        !graph.UploadBuffer(source.Value(), Span<const uint8>(data, CopyBytes))) {
        PrintGraphError(graph);
        return false;
    }

    auto copyTexture = [](GraphTexture to, GraphTexture from) noexcept {
        return [to, from](GraphPassContext& pass) noexcept {
            auto copied = pass.Commands().CopyTexture(pass.Texture(to), pass.Texture(from));
            Check(copied.IsOk(), "graph copy pass copies the texture");
        };
    };
    auto copyBuffer = [](GraphBuffer to, GraphBuffer from) noexcept {
        return [to, from](GraphPassContext& pass) noexcept {
            auto copied =
                pass.Commands().CopyBuffer(pass.Buffer(to), 0, pass.Buffer(from), 0, CopyBytes);
            Check(copied.IsOk(), "graph copy pass copies the buffer");
        };
    };
    GraphPassAccesses p0(graph);
    p0.RenderTarget(a.Value(), GraphLoadAction::Clear, {0, 0, 0, 1});
    GraphPassAccesses p1(graph);
    p1.RenderTarget(opener.Value(), GraphLoadAction::Clear, {0, 0, 0, 1});
    GraphPassAccesses p2(graph);
    p2.Texture(a.Value(), ResourceAccess::CopySource)
        .Texture(c.Value(), ResourceAccess::CopyDestination);
    GraphPassAccesses p3(graph);
    p3.Buffer(source.Value(), ResourceAccess::CopySource)
        .Buffer(middle.Value(), ResourceAccess::CopyDestination);
    GraphPassAccesses p4(graph);
    p4.Texture(c.Value(), ResourceAccess::CopySource)
        .Texture(d.Value(), ResourceAccess::CopyDestination);
    GraphPassAccesses p5(graph);
    p5.Buffer(middle.Value(), ResourceAccess::CopySource)
        .Buffer(last.Value(), ResourceAccess::CopyDestination);
    GraphPassAccesses p6(graph);
    p6.Texture(d.Value(), ResourceAccess::SrvGraphics)
        .RenderTarget(target.Value(), GraphLoadAction::Clear, {0, 1, 0, 1});
    auto added0 =
        graph.AddPass(SceneName, GraphPassKind::Raster, GraphPassFlags::None, p0,
                      FullScreen(bench.renderer, bench.redTexture, {}, LinearColor(1, 1, 1, 1)));
    auto added1 = graph.AddPass(ClearName, GraphPassKind::Raster, GraphPassFlags::HasSideEffects,
                                p1, [](GraphPassContext& pass) noexcept { (void)pass; });
    auto added2 = graph.AddPass(CopyName, GraphPassKind::Copy, GraphPassFlags::None, p2,
                                copyTexture(c.Value(), a.Value()));
    auto added3 = graph.AddPass(CopyName, GraphPassKind::Copy, GraphPassFlags::None, p3,
                                copyBuffer(middle.Value(), source.Value()));
    auto added4 = graph.AddPass(CopyName, GraphPassKind::Copy, GraphPassFlags::None, p4,
                                copyTexture(d.Value(), c.Value()));
    auto added5 = graph.AddPass(CopyName, GraphPassKind::Copy, GraphPassFlags::None, p5,
                                copyBuffer(last.Value(), middle.Value()));
    auto added6 = graph.AddPass(CompositeName, GraphPassKind::Raster, GraphPassFlags::None, p6,
                                FullScreen(bench.renderer, {}, d.Value(), LinearColor(1, 1, 1, 1)));
    auto lastHandle = graph.ExportBuffer(last.Value());
    if (!added0 || !added1 || !added2 || !added3 || !added4 || !added5 || !added6 || !lastHandle ||
        !graph.Execute()) {
        PrintGraphError(graph);
        return false;
    }

    *lastOut = lastHandle.Value();
    *splitCount = graph.Stats().splitTransitions;
    return true;
}

bool RunSampledUploadGraph(GraphBench& bench, RenderHandle output, const uint8* color) noexcept {
    RenderGraph graph(bench.Desc());
    TextureDesc desc;
    desc.width = 1;
    desc.height = 1;
    desc.usage = TextureUsage::Sampled;
    desc.sampler.filter = Filter::Nearest;
    auto lut = graph.CreateTexture(desc, "Uploaded");
    auto target = graph.ImportTexture(output, "Output");
    if (!lut || !target || !graph.UploadTexture(lut.Value(), 0, Span<const uint8>(color, 4))) {
        PrintGraphError(graph);
        return false;
    }

    GraphPassAccesses accesses(graph);
    accesses.Texture(lut.Value(), ResourceAccess::SrvGraphics)
        .RenderTarget(target.Value(), GraphLoadAction::Clear, {0, 0, 0, 1});
    auto pass = graph.AddPass(CompositeName, GraphPassKind::Raster, GraphPassFlags::None, accesses,
                              FullScreen(bench.renderer, {}, lut.Value(), LinearColor(1, 1, 1, 1)));
    if (!pass || !graph.Execute()) {
        PrintGraphError(graph);
        return false;
    }

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
        // 두 번째는 출력을 복사 원본 상태로 끝낸다(readback이 같은 상태 barrier를 내지 않는지).
        ok = RunGraph(bench, output.Value(), {}, nullptr,
                      run == 0 ? ResourceAccess::SrvGraphics : ResourceAccess::CopySource);
        Check(ok && device.ReadPixels(outputTexture.Value(), pixels.View()).IsOk() &&
                  CenterIs(pixels, 255, 0, 0),
              "graph with a pooled scene texture composites red");
    }

    // 결정 R15: history handle의 Retire는 장치 해제가 아니라 풀 반납이다. GPU 완료를 기다린 뒤
    // (fence gate를 결정적으로 통과) 다음 그래프의 장면 텍스처가 그 객체를 장치 생성 없이 다시
    // 쓴다.
    if (ok) {
        const uint64 createdBefore = pool.Stats().created;
        RetireExport(handles, device, history);
        Check(pool.Stats().returning == 1, "retired history waits for its fence in the pool");
        Check(device.WaitIdle().IsOk(), "wait idle before reusing the history");
        auto cleared = arena.Reset();
        (void)cleared;
        ok = RunGraph(bench, output.Value(), {}, nullptr);
        Check(ok && pool.Stats().returned == 1 && pool.Stats().created == createdBefore,
              "the returned history is reused without a device creation");
    }

    // RG2: 같은 그래프 안 재사용(픽셀).
    auto secondTexture = device.CreateTexture(TargetDesc(), {});
    auto second = handles.Reserve(RenderResourceKind::Texture);
    if (ok && secondTexture && second &&
        handles.BindTexture(second.Value(), secondTexture.Value()).IsOk()) {
        auto cleared = arena.Reset();
        (void)cleared;
        ok = RunReuseGraph(bench, output.Value(), second.Value());
        Check(ok && device.ReadPixels(outputTexture.Value(), pixels.View()).IsOk() &&
                  CenterIs(pixels, 255, 0, 0),
              "in-graph reuse keeps the earlier result red");
        Check(ok && device.ReadPixels(secondTexture.Value(), pixels.View()).IsOk() &&
                  CenterIs(pixels, 0, 0, 255),
              "the reused texture shows only its own blue clear");
    } else {
        Check(false, "second output creation");
    }

    // RG2: 그래프 안 업로드 → 복사 pass 두 번(버퍼 재사용·Discard 전이) → export 바이트. 두 번째
    // 그래프는 반납된 export 버퍼를 다시 쓴다(GPU 완료를 기다린 뒤).
    for (uint32 run = 0; ok && run < 2; ++run) {
        uint8 data[CopyBytes];
        for (uint32 index = 0; index < CopyBytes; ++index) {
            data[index] = static_cast<uint8>(index * 7 + run * 13 + 1);
        }

        auto cleared = arena.Reset();
        (void)cleared;
        const uint64 buffersBefore = pool.Stats().created;
        RenderHandle middle;
        RenderHandle last;
        ok = RunUploadCopyGraph(bench, data, &middle, &last);
        Check(ok && BufferMatches(device, handles, middle, data),
              "uploaded bytes reach the first copy");
        Check(ok && BufferMatches(device, handles, last, data),
              "uploaded bytes reach the reused buffer");
        if (run == 1) {
            Check(pool.Stats().created == buffersBefore,
                  "returned export buffers are reused without device creations");
        }

        if (ok) {
            RetireExport(handles, device, middle);
            RetireExport(handles, device, last);
            Check(device.WaitIdle().IsOk(), "wait idle after the copy graph");
        }
    }

    // RG2: Sampled 그래프 텍스처 업로드(처음은 생성 초기 데이터, 두 번째는 풀의 mip 갱신).
    const uint8 colors[2][4] = {{0, 255, 0, 255}, {0, 0, 255, 255}};
    for (uint32 run = 0; ok && run < 2; ++run) {
        auto cleared = arena.Reset();
        (void)cleared;
        ok = RunSampledUploadGraph(bench, output.Value(), colors[run]);
        Check(ok && device.ReadPixels(outputTexture.Value(), pixels.View()).IsOk() &&
                  CenterIs(pixels, colors[run][0], colors[run][1], colors[run][2]),
              run == 0 ? "uploaded sampled texture composites green"
                       : "re-uploaded pooled sampled texture composites blue");
    }

    // RG2 B부: 분리 전이를 켜고 끈 그래프의 픽셀·바이트가 같다(분리 켬은 텍스처·버퍼 전이 둘을
    // 나눈다, Vulkan은 synchronization2 event, 그 밖은 end에서 한 번에 낮춘다).
    uint8 splitData[CopyBytes];
    for (uint32 index = 0; index < CopyBytes; ++index) {
        splitData[index] = static_cast<uint8>(255 - index);
    }

    uint8 splitPixels[2][4] = {};
    for (uint32 run = 0; ok && run < 2; ++run) {
        const bool split = run == 0;
        auto cleared = arena.Reset();
        (void)cleared;
        RenderHandle last;
        uint32 splitCount = 0;
        ok = RunSplitGraph(bench, output.Value(), splitData, split, &last, &splitCount);
        Check(ok && (split ? splitCount == 2 : splitCount == 0),
              split ? "split graph splits the texture and buffer transitions"
                    : "unsplit graph splits nothing");
        Check(ok && device.ReadPixels(outputTexture.Value(), pixels.View()).IsOk() &&
                  CenterIs(pixels, 255, 0, 0),
              split ? "split transitions composite red" : "unsplit transitions composite red");
        if (ok) {
            const usize center =
                (static_cast<usize>(TargetSize / 2) * TargetSize + TargetSize / 2) * 4;
            for (uint32 channel = 0; channel < 4; ++channel) {
                splitPixels[run][channel] = pixels[center + channel];
            }
        }

        Check(ok && BufferMatches(device, handles, last, splitData),
              split ? "split buffer transition keeps the copied bytes"
                    : "unsplit buffer transition keeps the copied bytes");
        if (ok) {
            RetireExport(handles, device, last);
            Check(device.WaitIdle().IsOk(), "wait idle after the split graph");
        }
    }

    Check(!ok || platform::MemoryEquals(splitPixels[0], splitPixels[1], 4),
          "split and unsplit transitions give the same pixels");
    printf("  %s render graph split transitions: native %s\n", name,
           device.Caps().features.splitTransitions ? "yes" : "no (lowered at end)");

    const rocks::render::GraphPoolStats stats = pool.Stats();
    Check(stats.reused >= 1, "pool reuses graph textures across graphs");
    const uint64 validationErrors = device.ValidationErrorCount() - validationBefore;
    Check(validationErrors == 0, "validation errors (incl. transition mismatches) are zero");
    printf("  %s render graph: pool created %llu, reused %llu, validation errors %llu\n", name,
           static_cast<unsigned long long>(stats.created),
           static_cast<unsigned long long>(stats.reused),
           static_cast<unsigned long long>(validationErrors));

    if (second) {
        auto retired = handles.BeginRetire(second.Value());
        if (retired) {
            ReleaseRetiredResource(device, second.Value(), retired.Value());
        }
    }

    device.Retire(redTexture.Value());
    device.Retire(outputTexture.Value());
    pool.Clear();
    auto idle = device.WaitIdle();
    Check(idle.IsOk(), "wait idle");
    return failures;
}

} // namespace sandbox
