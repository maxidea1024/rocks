#include "RenderGraphSelfTest.h"
#include "SandboxShaders.h"

#include "TextureViewSelfTest.h"

#include <Core/Containers/Array.h>
#include <Core/Memory/FrameArena.h>
#include <Platform/Memory.h>
#include <RenderGraph/RenderGraph.h>
#include <Task/WorkerScheduler.h>
#include <stdio.h>

namespace sandbox {

namespace {

using namespace rocks;
using namespace rocks::graphics;
using rocks::render::GraphAsyncCompute;
using rocks::render::GraphBuffer;
using rocks::render::GraphLoadAction;
using rocks::render::GraphPassAccesses;
using rocks::render::GraphPassContext;
using rocks::render::GraphPassFlags;
using rocks::render::GraphPassKind;
using rocks::render::GraphResourcePool;
using rocks::render::GraphTexture;
using rocks::render::GraphTextureView;
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
TraceScopeName HzbFillName("SelfTest.HzbFill");
TraceScopeName TintName("SelfTest.Tint");
TraceScopeName ParallelCopyName("SelfTest.ParallelCopy");

// 병렬 기록 시험(RG3)의 Raster pass 수와 worker 수다.
constexpr uint32 ParallelPasses = 6;
constexpr uint32 ParallelWorkers = 3;

// 그리기별 상수 색으로 화면을 채운다(병렬 기록 시험, 샘플 렌더러를 worker에서 함께 쓰지 않으려고
// 따로 둔다).
// 셰이더는 FullScreen.rsf·Tint.rsf다.
struct TintConstants {
    float values[4] = {};
};
TraceScopeName MsaaName("SelfTest.Msaa");
TraceScopeName ResolveScopeName("SelfTest.ResolveScope");
TraceScopeName HzbReduceName("SelfTest.HzbReduce");
TraceScopeName AsyncSceneName("SelfTest.AsyncScene");
TraceScopeName AsyncProbeName("SelfTest.AsyncProbe");
TraceScopeName TransientName("SelfTest.Transient");

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
    desc.sampler.filter = Filter::Point;
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
    // RG2 풀 재사용 시험이라 transient aliasing(RG5)을 끈다(aliasing은 RunTransientGraph).
    RenderGraphDesc desc = bench.Desc();
    desc.transientAliasing = false;
    RenderGraph graph(desc);
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
    desc.sampler.filter = Filter::Point;
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

// RG1 남은 것 B부: HZB 축소를 그래프 texture view로 낸다. mip 0을 쓰기 view로 채우고, mip k 읽기
// view와 mip k + 1 쓰기 view를 가진 Compute pass로 줄인 뒤 export해 모든 mip을 읽는다. 셰이더는
// view 기준 mip(GraphPassContext::ViewBaseMip)을 상수로 받는다(OpenGL ES는 텍스처 전체가 묶인다).
bool RunHzbGraph(GraphBench& bench, Allocator& allocator) noexcept {
    Device& device = *bench.device;
    auto fill = hzb::CreateFillPipeline(device);
    auto reduce = hzb::CreateReducePipeline(device);
    Check(fill && reduce, "HZB compute pipelines");
    bool ok = fill && reduce;
    RenderHandle exported;
    if (ok) {
        RenderGraph graph(bench.Desc());
        auto pyramid = graph.CreateTexture(hzb::PyramidDesc(), "Hzb");
        GraphTextureView reads[hzb::Mips];
        GraphTextureView writes[hzb::Mips];
        ok = pyramid.IsOk();
        for (uint32 mip = 0; ok && mip < hzb::Mips; ++mip) {
            TextureViewDesc view;
            view.firstMip = mip;
            view.mipCount = 1;
            auto read = graph.CreateTextureView(pyramid.Value(), view);
            view.access = ViewAccess::ReadWrite;
            auto write = graph.CreateTextureView(pyramid.Value(), view);
            ok = read && write;
            if (ok) {
                reads[mip] = read.Value();
                writes[mip] = write.Value();
            }
        }

        const PipelineHandle fillPipeline = fill.Value();
        const PipelineHandle reducePipeline = reduce.Value();
        if (ok) {
            GraphPassAccesses accesses(graph);
            accesses.TextureView(writes[0], ResourceAccess::UavCompute);
            const GraphTextureView first = writes[0];
            ok = graph
                     .AddPass(HzbFillName, GraphPassKind::Compute, GraphPassFlags::None, accesses,
                              [fillPipeline, first](GraphPassContext& pass) noexcept {
                                  pass.Commands().SetPipeline(fillPipeline);
                                  pass.Commands().SetWritableTexture(0, pass.View(first));
                                  pass.Commands().Dispatch(1, 1);
                              })
                     .IsOk();
        }

        for (uint32 mip = 0; ok && mip + 1 < hzb::Mips; ++mip) {
            GraphPassAccesses accesses(graph);
            accesses.TextureView(reads[mip], ResourceAccess::SrvCompute)
                .TextureView(writes[mip + 1], ResourceAccess::UavCompute);
            const GraphTextureView read = reads[mip];
            const GraphTextureView write = writes[mip + 1];
            const uint32 size = hzb::Extent >> (mip + 1);
            ok = graph
                     .AddPass(HzbReduceName, GraphPassKind::Compute, GraphPassFlags::None, accesses,
                              [reducePipeline, read, write, size](GraphPassContext& pass) noexcept {
                                  hzb::LevelConstants level;
                                  level.values[0] = static_cast<float>(pass.ViewBaseMip(read));
                                  level.values[1] = static_cast<float>(size);
                                  CommandContext& commands = pass.Commands();
                                  commands.SetPipeline(reducePipeline);
                                  commands.SetTexture(
                                      ShaderStage::Compute,
                                      shaders::TextureSlot(shaders::TextureViewReduceCS, "Source"),
                                      pass.View(read));
                                  commands.SetWritableTexture(0, pass.View(write));
                                  Check(commands.SetDrawConstants(DrawConstantBytes(level)).IsOk(),
                                        "HZB reduce draw constants are accepted");
                                  const uint32 groups =
                                      (size + hzb::ReduceGroup - 1) / hzb::ReduceGroup;
                                  commands.Dispatch(groups, groups);
                              })
                     .IsOk();
        }

        auto handle = ok ? graph.ExportTexture(pyramid.Value())
                         : Result<RenderHandle>::Failure(ErrorCode::InvalidArgument);
        ok = handle && graph.Execute().IsOk();
        if (handle) {
            exported = handle.Value();
        }

        if (!ok) {
            PrintGraphError(graph);
        }
    }

    Check(ok, "HZB graph executes");
    auto texture = ok ? bench.handles->Texture(exported)
                      : Result<TextureHandle>::Failure(ErrorCode::InvalidArgument);
    Check(texture && device.WaitIdle().IsOk() &&
              hzb::CountMismatchedMips(device, allocator, texture.Value()) == 0,
          "every mip of the graph-reduced HZB matches the CPU reference");
    if (!exported.IsNull()) {
        RetireExport(*bench.handles, device, exported);
    }

    if (fill) {
        device.Retire(fill.Value());
    }

    if (reduce) {
        device.Retire(reduce.Value());
    }

    return ok;
}

// RG4 async compute 시험 결과다.
struct AsyncComputeRun {
    uint32 asyncPasses = 0;
    uint32 computeRecordings = 0;
    uint32 queueWaits = 0;
    uint32 mismatchedMips = 0;
    // queue별 timestamp로 잰 구간(ms)이다(통계만, 단언하지 않는다).
    bool measured = false;
    double graphicsMs = 0.0;
    double computeMs = 0.0;
    double overlapMs = 0.0;
};

// pass 기록 함수가 쓴 timestamp 번호다(장치 query 번호, queue마다 따로).
struct AsyncStamps {
    uint64 sceneBegin = 0;
    uint64 sceneEnd = 0;
    uint64 compositeBegin = 0;
    uint64 compositeEnd = 0;
    uint64 computeBegin = 0;
    uint64 computeEnd = 0;
    uint32 written = 0;
};

// 겹침 계측이 쓰는 timestamp 수(장면·합성 시작·끝, compute 시작·끝)와 결과 버퍼 크기다.
constexpr uint32 AsyncStampCount = 6;
constexpr usize AsyncTimestampCapacity = 256;
constexpr double NanosecondsPerMillisecond = 1000000.0;

// query 번호의 tick을 그 queue의 calibration으로 CPU ns로 옮긴다. 찾지 못하면 false다.
bool StampNanoseconds(Span<const TimestampSample> samples, QueueKind queue, uint64 query,
                      const ClockCalibration& calibration, double& nanoseconds) noexcept {
    for (const TimestampSample& sample : samples) {
        if (sample.queue == queue && sample.query == query && !sample.disjoint &&
            calibration.gpuFrequency != 0) {
            const double ticks =
                static_cast<double>(sample.ticks) - static_cast<double>(calibration.gpuTicks);
            nanoseconds = static_cast<double>(calibration.cpuNanoseconds) +
                          ticks * 1e9 / static_cast<double>(calibration.gpuFrequency);
            return true;
        }
    }

    return false;
}

void WriteStamp(CommandContext& commands, uint64& slot, AsyncStamps& stamps) noexcept {
    auto written = commands.WriteTimestamp();
    if (written) {
        slot = written.Value();
        ++stamps.written;
    }
}

// RG4: graphics pass(장면 → 합성)와 async compute HZB(fill·reduce)가 섞인 그래프다. HZB는 graphics
// 생산자가 없어 그래프 시작에서 fork하고, probe pass가 pyramid를 읽어 join한다. setting이
// Disabled면 같은 그래프를 graphics pipe로 실행한다. probe 픽셀과 HZB mip을 돌려준다.
bool RunAsyncComputeGraph(GraphBench& bench, Allocator& allocator, RenderHandle output,
                          PipelineHandle fillPipeline, PipelineHandle reducePipeline,
                          GraphAsyncCompute setting, Array<uint8>& probePixels,
                          AsyncComputeRun& run) noexcept {
    Device& device = *bench.device;
    RenderGraphDesc desc = bench.Desc();
    desc.asyncCompute = setting;
    AsyncStamps stamps;
    RenderHandle pyramidHandle;
    RenderHandle probeHandle;
    bool ok = false;
    {
        RenderGraph graph(desc);
        auto scene = graph.CreateTexture(TargetDesc(), "AsyncScene");
        auto probe = graph.CreateTexture(TargetDesc(), "AsyncProbe");
        auto pyramid = graph.CreateTexture(hzb::PyramidDesc(), "AsyncHzb");
        auto target = graph.ImportTexture(output, "Output");
        GraphTextureView reads[hzb::Mips];
        GraphTextureView writes[hzb::Mips];
        ok = scene && probe && pyramid && target;
        for (uint32 mip = 0; ok && mip < hzb::Mips; ++mip) {
            TextureViewDesc view;
            view.firstMip = mip;
            view.mipCount = 1;
            auto read = graph.CreateTextureView(pyramid.Value(), view);
            view.access = ViewAccess::ReadWrite;
            auto write = graph.CreateTextureView(pyramid.Value(), view);
            ok = read && write;
            if (ok) {
                reads[mip] = read.Value();
                writes[mip] = write.Value();
            }
        }

        AsyncStamps* stamp = &stamps;
        sample::Renderer* renderer = bench.renderer;
        const TextureHandle red = bench.redTexture;
        if (ok) {
            GraphPassAccesses accesses(graph);
            accesses.RenderTarget(scene.Value(), GraphLoadAction::Clear, {0, 0, 0, 1});
            ok = graph
                     .AddPass(AsyncSceneName, GraphPassKind::Raster, GraphPassFlags::None, accesses,
                              [renderer, red, stamp](GraphPassContext& pass) noexcept {
                                  WriteStamp(pass.Commands(), stamp->sceneBegin, *stamp);
                                  FullScreen(renderer, red, {}, LinearColor(1, 1, 1, 1))(pass);
                                  WriteStamp(pass.Commands(), stamp->sceneEnd, *stamp);
                              })
                     .IsOk();
        }

        if (ok) {
            GraphPassAccesses accesses(graph);
            accesses.TextureView(writes[0], ResourceAccess::UavCompute);
            const GraphTextureView first = writes[0];
            ok = graph
                     .AddPass(HzbFillName, GraphPassKind::AsyncCompute, GraphPassFlags::None,
                              accesses,
                              [fillPipeline, first, stamp](GraphPassContext& pass) noexcept {
                                  WriteStamp(pass.Commands(), stamp->computeBegin, *stamp);
                                  pass.Commands().SetPipeline(fillPipeline);
                                  pass.Commands().SetWritableTexture(0, pass.View(first));
                                  pass.Commands().Dispatch(1, 1);
                              })
                     .IsOk();
        }

        for (uint32 mip = 0; ok && mip + 1 < hzb::Mips; ++mip) {
            GraphPassAccesses accesses(graph);
            accesses.TextureView(reads[mip], ResourceAccess::SrvCompute)
                .TextureView(writes[mip + 1], ResourceAccess::UavCompute);
            const GraphTextureView read = reads[mip];
            const GraphTextureView write = writes[mip + 1];
            const uint32 size = hzb::Extent >> (mip + 1);
            const bool last = mip + 2 == hzb::Mips;
            ok = graph
                     .AddPass(
                         HzbReduceName, GraphPassKind::AsyncCompute, GraphPassFlags::None, accesses,
                         [reducePipeline, read, write, size, last,
                          stamp](GraphPassContext& pass) noexcept {
                             hzb::LevelConstants level;
                             level.values[0] = static_cast<float>(pass.ViewBaseMip(read));
                             level.values[1] = static_cast<float>(size);
                             CommandContext& commands = pass.Commands();
                             commands.SetPipeline(reducePipeline);
                             commands.SetTexture(
                                 ShaderStage::Compute,
                                 shaders::TextureSlot(shaders::TextureViewReduceCS, "Source"),
                                 pass.View(read));
                             commands.SetWritableTexture(0, pass.View(write));
                             Check(commands.SetDrawConstants(DrawConstantBytes(level)).IsOk(),
                                   "async HZB reduce draw constants are accepted");
                             const uint32 groups = (size + hzb::ReduceGroup - 1) / hzb::ReduceGroup;
                             commands.Dispatch(groups, groups);
                             if (last) {
                                 WriteStamp(commands, stamp->computeEnd, *stamp);
                             }
                         })
                     .IsOk();
        }

        // 합성은 compute와 겹치는 graphics pass다(HZB에 기대지 않는다).
        if (ok) {
            GraphPassAccesses accesses(graph);
            accesses.Texture(scene.Value(), ResourceAccess::SrvGraphics)
                .RenderTarget(target.Value());
            const GraphTexture source = scene.Value();
            ok = graph
                     .AddPass(CompositeName, GraphPassKind::Raster, GraphPassFlags::None, accesses,
                              [renderer, source, stamp](GraphPassContext& pass) noexcept {
                                  WriteStamp(pass.Commands(), stamp->compositeBegin, *stamp);
                                  FullScreen(renderer, {}, source, LinearColor(1, 1, 1, 1))(pass);
                                  WriteStamp(pass.Commands(), stamp->compositeEnd, *stamp);
                              })
                     .IsOk();
        }

        // probe가 pyramid를 graphics에서 읽는다(join: compute 제출을 기다린 뒤 전이).
        if (ok) {
            GraphPassAccesses accesses(graph);
            accesses.Texture(pyramid.Value(), ResourceAccess::SrvGraphics)
                .RenderTarget(probe.Value(), GraphLoadAction::Clear, {0, 0, 0, 1});
            ok = graph
                     .AddPass(AsyncProbeName, GraphPassKind::Raster, GraphPassFlags::None, accesses,
                              FullScreen(renderer, {}, pyramid.Value(), LinearColor(1, 1, 1, 1)))
                     .IsOk();
        }

        auto exportedProbe = ok ? graph.ExportTexture(probe.Value())
                                : Result<RenderHandle>::Failure(ErrorCode::InvalidArgument);
        auto exportedPyramid = ok ? graph.ExportTexture(pyramid.Value())
                                  : Result<RenderHandle>::Failure(ErrorCode::InvalidArgument);
        ok = exportedProbe && exportedPyramid && graph.Execute().IsOk();
        if (exportedProbe) {
            probeHandle = exportedProbe.Value();
        }

        if (exportedPyramid) {
            pyramidHandle = exportedPyramid.Value();
        }

        if (!ok) {
            PrintGraphError(graph);
        }

        const rocks::render::GraphStats stats = graph.Stats();
        run.asyncPasses = stats.asyncComputePasses;
        run.computeRecordings = stats.computeRecordings;
        run.queueWaits = stats.queueWaits;
    }

    ok = ok && device.WaitIdle().IsOk();
    auto probeTexture = ok ? bench.handles->Texture(probeHandle)
                           : Result<TextureHandle>::Failure(ErrorCode::InvalidArgument);
    auto pyramidTexture = ok ? bench.handles->Texture(pyramidHandle)
                             : Result<TextureHandle>::Failure(ErrorCode::InvalidArgument);
    ok = probeTexture && pyramidTexture &&
         device.ReadPixels(probeTexture.Value(), probePixels.View()).IsOk();
    run.mismatchedMips =
        ok ? hzb::CountMismatchedMips(device, allocator, pyramidTexture.Value()) : hzb::Mips;

    // 겹침 계측(통계만): queue별 timestamp를 그 queue의 calibration으로 CPU 시간축에 놓고, compute
    // 구간이 graphics pass 구간(장면·합성)과 겹친 시간을 더한다. graphics pipe 실행은 compute가 두
    // pass 사이에 직렬이라 0에 가깝다.
    const QueueKind computeQueue =
        run.asyncPasses != 0 ? QueueKind::AsyncCompute : QueueKind::Graphics;
    TimestampSample samples[AsyncTimestampCapacity];
    const usize count = ok && stamps.written == AsyncStampCount
                            ? device.CollectTimestamps(Span<TimestampSample>(samples))
                            : 0;
    auto graphicsClock = device.CalibrateClocks(QueueKind::Graphics);
    auto computeClock = device.CalibrateClocks(computeQueue);
    const uint64 graphicsQueries[] = {stamps.sceneBegin, stamps.sceneEnd, stamps.compositeBegin,
                                      stamps.compositeEnd};
    double graphics[4] = {};
    double computeBegin = 0.0;
    double computeEnd = 0.0;
    const Span<const TimestampSample> collected(samples, count);
    bool measured = count != 0 && graphicsClock && computeClock &&
                    StampNanoseconds(collected, computeQueue, stamps.computeBegin,
                                     computeClock.Value(), computeBegin) &&
                    StampNanoseconds(collected, computeQueue, stamps.computeEnd,
                                     computeClock.Value(), computeEnd);
    for (uint32 index = 0; measured && index < 4; ++index) {
        measured = StampNanoseconds(collected, QueueKind::Graphics, graphicsQueries[index],
                                    graphicsClock.Value(), graphics[index]);
    }

    run.measured = measured;
    if (measured) {
        run.computeMs = (computeEnd - computeBegin) / NanosecondsPerMillisecond;
        for (uint32 pair = 0; pair < 2; ++pair) {
            const double begin = graphics[pair * 2];
            const double end = graphics[pair * 2 + 1];
            const double start = begin > computeBegin ? begin : computeBegin;
            const double stop = end < computeEnd ? end : computeEnd;
            run.graphicsMs += (end - begin) / NanosecondsPerMillisecond;
            run.overlapMs += stop > start ? (stop - start) / NanosecondsPerMillisecond : 0.0;
        }
    }

    if (!probeHandle.IsNull()) {
        RetireExport(*bench.handles, device, probeHandle);
    }

    if (!pyramidHandle.IsNull()) {
        RetireExport(*bench.handles, device, pyramidHandle);
    }

    return ok;
}

// RG5: 크기가 다른 렌더 타깃 사슬(32 → 16 → 32 → 출력)이다. 앞 결과를 읽어 다음에 그리므로
// 첫 텍스처가 놓인 뒤 셋째 텍스처가 그 heap 구간을 이어 받는다(aliasing). aliasing이 꺼져 있으면
// 풀(RG2)이다. 출력은 빨강이어야 한다.
bool RunTransientGraph(GraphBench& bench, RenderHandle output, bool aliasing,
                       rocks::render::GraphStats& stats) noexcept {
    RenderGraphDesc desc = bench.Desc();
    desc.transientAliasing = aliasing;
    RenderGraph graph(desc);
    TextureDesc small = TargetDesc();
    small.width = TargetSize / 2;
    small.height = TargetSize / 2;
    auto first = graph.CreateTexture(TargetDesc(), "TransientFirst");
    auto second = graph.CreateTexture(small, "TransientSecond");
    auto third = graph.CreateTexture(TargetDesc(), "TransientThird");
    auto target = graph.ImportTexture(output, "Output");
    bool ok = first && second && third && target;
    sample::Renderer* renderer = bench.renderer;
    if (ok) {
        GraphPassAccesses accesses(graph);
        accesses.RenderTarget(first.Value(), GraphLoadAction::Clear, {0, 0, 0, 1});
        ok = graph
                 .AddPass(TransientName, GraphPassKind::Raster, GraphPassFlags::None, accesses,
                          FullScreen(renderer, bench.redTexture, {}, LinearColor(1, 1, 1, 1)))
                 .IsOk();
    }

    const GraphTexture chain[] = {
        first ? first.Value() : GraphTexture{}, second ? second.Value() : GraphTexture{},
        third ? third.Value() : GraphTexture{}, target ? target.Value() : GraphTexture{}};
    for (uint32 step = 0; ok && step + 1 < 4; ++step) {
        GraphPassAccesses accesses(graph);
        accesses.Texture(chain[step], ResourceAccess::SrvGraphics)
            .RenderTarget(chain[step + 1], GraphLoadAction::DontCare);
        ok = graph
                 .AddPass(TransientName, GraphPassKind::Raster, GraphPassFlags::None, accesses,
                          FullScreen(renderer, {}, chain[step], LinearColor(1, 1, 1, 1)))
                 .IsOk();
    }

    ok = ok && graph.Execute().IsOk();
    if (!ok) {
        PrintGraphError(graph);
    }

    stats = graph.Stats();
    return ok;
}

// RG1 남은 것 설계 2: 그래프 MSAA pass가 마젠타로 clear한 4x 텍스처를 1표본 그래프 텍스처로
// resolve하고, 다음 pass가 그 대상을 SRV로 읽어 output에 합성한다. 두 pass는 그래프 구간 안이고
// GpuTrace marker를 켠다(기록마다 marker 짝이 맞아야 검증 계층이 조용하다).
bool RunResolveGraph(GraphBench& bench, Allocator& allocator, RenderHandle output) noexcept {
    rocks::render::GpuTraceSettings settings;
    settings.markers = true;
    rocks::render::GpuTrace trace(allocator, settings);
    RenderGraphDesc desc = bench.Desc();
    desc.trace = &trace;
    RenderGraph graph(desc);
    TextureDesc msaaDesc = TargetDesc();
    msaaDesc.sampleCount = 4;
    auto target = graph.ImportTexture(output, "Output");
    auto msaa = graph.CreateTexture(msaaDesc, "Msaa");
    auto resolved = graph.CreateTexture(TargetDesc(), "Resolved");
    if (!target || !msaa || !resolved) {
        PrintGraphError(graph);
        return false;
    }

    bool added = false;
    {
        rocks::render::GraphScope scope(graph, ResolveScopeName, 1);
        rocks::render::GraphColorTarget color;
        color.texture = msaa.Value();
        color.clear = {1.0f, 0.0f, 1.0f, 1.0f};
        color.resolveTarget = resolved.Value();
        GraphPassAccesses msaaAccesses(graph);
        msaaAccesses.ColorTarget(color);
        // clear만 한다(샘플 렌더러 파이프라인은 1표본이다).
        auto msaaPass =
            graph.AddPass(MsaaName, GraphPassKind::Raster, GraphPassFlags::None, msaaAccesses,
                          [](GraphPassContext& pass) noexcept { (void)pass; });
        GraphPassAccesses compositeAccesses(graph);
        compositeAccesses.Texture(resolved.Value(), ResourceAccess::SrvGraphics)
            .RenderTarget(target.Value(), GraphLoadAction::Clear, {0, 1, 0, 1});
        auto compositePass = graph.AddPass(
            CompositeName, GraphPassKind::Raster, GraphPassFlags::None, compositeAccesses,
            FullScreen(bench.renderer, {}, resolved.Value(), LinearColor(1, 1, 1, 1)));
        added = msaaPass && compositePass;
    }

    if (!added || !graph.Execute()) {
        PrintGraphError(graph);
        return false;
    }

    return true;
}

// RG3 병렬 기록: Raster pass 여섯이 앞 결과를 읽으며(의존 사슬) 각자 색을 그리고, 마지막 Copy
// pass가 그 결과를 output에 복사한다. scheduler가 있으면 기록 함수를 worker에서 기록하고 소유
// 스레드가 재생한다. pixels에 output을 읽는다.
bool RunParallelGraph(GraphBench& bench, PipelineHandle tint, RenderHandle output,
                      TextureHandle outputTexture, WorkerScheduler* scheduler, Allocator& allocator,
                      Array<uint8>& pixels, uint32& recorded) noexcept {
    RenderGraphDesc desc = bench.Desc();
    desc.scheduler = scheduler;
    desc.scratch = &allocator;
    RenderGraph graph(desc);
    rocks::render::GraphImportOptions outputOptions;
    outputOptions.finalAccess = ResourceAccess::SrvGraphics;
    auto target = graph.ImportTexture(output, "Output", outputOptions);
    if (!target) {
        return false;
    }

    GraphTexture previous;
    bool added = true;
    for (uint32 index = 0; index < ParallelPasses && added; ++index) {
        auto texture = graph.CreateTexture(TargetDesc(), "Tinted");
        if (!texture) {
            return false;
        }

        GraphPassAccesses accesses(graph);
        if (!previous.IsNull()) {
            accesses.Texture(previous, ResourceAccess::SrvGraphics);
        }

        accesses.RenderTarget(texture.Value(), GraphLoadAction::Clear, {0, 0, 0, 1});
        TintConstants constants;
        constants.values[0] = static_cast<float>(index) / static_cast<float>(ParallelPasses);
        constants.values[1] = 1.0f - constants.values[0];
        constants.values[2] = index % 2 == 0 ? 1.0f : 0.0f;
        constants.values[3] = 1.0f;
        added =
            graph
                .AddPass(TintName, GraphPassKind::Raster, GraphPassFlags::None, accesses,
                         [tint, constants](GraphPassContext& pass) noexcept {
                             pass.Commands().SetPipeline(tint);
                             (void)pass.Commands().SetDrawConstants(DrawConstantBytes(constants));
                             pass.Commands().Draw(3);
                         })
                .IsOk();
        previous = texture.Value();
    }

    GraphPassAccesses copy(graph);
    copy.Texture(previous, ResourceAccess::CopySource)
        .Texture(target.Value(), ResourceAccess::CopyDestination);
    const GraphTexture from = previous;
    const GraphTexture to = target.Value();
    added =
        added &&
        graph
            .AddPass(ParallelCopyName, GraphPassKind::Copy, GraphPassFlags::HasSideEffects, copy,
                     [from, to](GraphPassContext& pass) noexcept {
                         (void)pass.Commands().CopyTexture(pass.Texture(to), pass.Texture(from));
                     })
            .IsOk();
    if (!added || !graph.Execute()) {
        PrintGraphError(graph);
        return false;
    }

    recorded = graph.Stats().parallelPasses;
    return bench.device->ReadPixels(outputTexture, pixels.View()).IsOk();
}

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
    redDesc.sampler.filter = Filter::Point;
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

    // RG1 남은 것 설계 2: 그래프 resolve와 그래프 구간(4x MSAA를 지원하는 장치).
    if (ok && (device.GetPixelFormatSampleCounts(TargetDesc().format) & SampleCountBit(4)) != 0) {
        auto cleared = arena.Reset();
        (void)cleared;
        ok = RunResolveGraph(bench, allocator, output.Value());
        Check(ok && device.ReadPixels(outputTexture.Value(), pixels.View()).IsOk() &&
                  CenterIs(pixels, 255, 0, 255),
              "a graph MSAA pass resolves into a texture the next pass composites (magenta)");
    }

    // RG3 병렬 기록: 같은 그래프를 직렬·병렬로 실행한 출력 픽셀이 같다.
    if (ok) {
        PipelineDesc tintDesc;
        tintDesc.colorFormats[0] = TargetDesc().format;
        tintDesc.drawConstantBytes = sizeof(TintConstants);
        auto tint = shaders::CreateGraphicsPipeline(device, shaders::FullScreenVS, shaders::TintPS,
                                                    tintDesc);
        WorkerScheduler scheduler(allocator);
        WorkerSchedulerConfig config;
        config.workerCount = ParallelWorkers;
        const bool started = scheduler.Start(config).IsOk();
        Array<uint8> serialPixels(allocator);
        Array<uint8> parallelPixels(allocator);
        uint32 serialRecorded = 0;
        uint32 parallelRecorded = 0;
        const bool sized = serialPixels.TryResize(pixels.Size()).IsOk() &&
                           parallelPixels.TryResize(pixels.Size()).IsOk();
        Check(tint && started && sized, "tint pipeline and worker scheduler");
        if (tint && started && sized) {
            auto cleared = arena.Reset();
            (void)cleared;
            const bool serialOk =
                RunParallelGraph(bench, tint.Value(), output.Value(), outputTexture.Value(),
                                 nullptr, allocator, serialPixels, serialRecorded);
            cleared = arena.Reset();
            (void)cleared;
            const bool parallelOk =
                RunParallelGraph(bench, tint.Value(), output.Value(), outputTexture.Value(),
                                 &scheduler, allocator, parallelPixels, parallelRecorded);
            Check(serialOk && parallelOk && serialRecorded == 0 &&
                      parallelRecorded == ParallelPasses + 1,
                  "the same graph runs serially and with parallel recording");
            Check(serialOk && parallelOk &&
                      platform::MemoryEquals(serialPixels.Data(), parallelPixels.Data(),
                                             serialPixels.Size()),
                  "serial and parallel recording give the same pixels");
            const usize center =
                (static_cast<usize>(TargetSize / 2) * TargetSize + TargetSize / 2) * 4;
            // 마지막 pass(번호 5)의 색: R 5/6, G 1/6, B 0(대상 형식이 sRGB면 R·G가 인코딩으로 더
            // 밝다).
            const bool lastColor = parallelOk &&
                                   parallelPixels[center] > parallelPixels[center + 1] &&
                                   parallelPixels[center] >= 200 && parallelPixels[center + 2] == 0;
            Check(lastColor, "parallel recording draws the last pass color");
            if (!lastColor) {
                printf("    center (%u, %u, %u)\n", parallelPixels[center],
                       parallelPixels[center + 1], parallelPixels[center + 2]);
            }
        }

        if (started) {
            (void)scheduler.Shutdown();
        }

        if (tint) {
            device.Retire(tint.Value());
        }
    }

    // RG1 남은 것 B부: 그래프 texture view로 낸 HZB(Storage 형식 능력과 compute가 있는 장치).
    const TextureDesc hzbDesc = hzb::PyramidDesc();
    if (ok && device.Caps().features.computeShaders &&
        HasPixelFormatCaps(device.GetPixelFormatCaps(hzbDesc.format),
                           RequiredPixelFormatCaps(hzbDesc))) {
        auto cleared = arena.Reset();
        (void)cleared;
        ok = RunHzbGraph(bench, allocator);
        printf("  %s render graph HZB: view base mip %s\n", name,
               device.Caps().features.textureViewRanges ? "0 (range views)"
                                                        : "first mip (whole texture)");
    }
    // RG5: transient aliasing 켬·끔의 출력이 같고(빨강), 켜면 heap에서 실제로 쓴 최대 크기가 자원
    // 크기 합보다 작다(첫 텍스처의 구간을 셋째가 이어 받는다). transient heap이 없는 장치는 둘 다
    // 풀이다.
    if (ok) {
        Array<uint8> aliased(allocator);
        Array<uint8> pooled(allocator);
        rocks::render::GraphStats aliasedStats;
        rocks::render::GraphStats pooledStats;
        const bool sized =
            aliased.TryResize(pixels.Size()).IsOk() && pooled.TryResize(pixels.Size()).IsOk();
        auto cleared = arena.Reset();
        (void)cleared;
        const bool pooledOk = sized &&
                              RunTransientGraph(bench, output.Value(), false, pooledStats) &&
                              device.ReadPixels(outputTexture.Value(), pooled.View()).IsOk();
        cleared = arena.Reset();
        (void)cleared;
        const bool aliasedOk = sized &&
                               RunTransientGraph(bench, output.Value(), true, aliasedStats) &&
                               device.ReadPixels(outputTexture.Value(), aliased.View()).IsOk();
        const bool heaps = device.Caps().features.transientHeaps;
        Check(pooledOk && aliasedOk, "the transient chain runs with and without aliasing");
        Check(pooledOk && aliasedOk &&
                  platform::MemoryEquals(pooled.Data(), aliased.Data(), pooled.Size()) &&
                  CenterIs(aliased, 255, 0, 0),
              "aliasing on and off give the same (red) output");
        Check(pooledStats.transientResources == 0 &&
                  aliasedStats.transientResources == (heaps ? 3u : 0u),
              "transient resources use heaps only when the device has them");
        if (heaps) {
            Check(aliasedStats.aliasedResources == 1 &&
                      aliasedStats.transientPeakBytes < aliasedStats.transientBytes,
                  "aliasing lowers the transient peak");
        }

        printf("  %s render graph transient aliasing: heaps %s, resources %u, aliased %u, bytes "
               "%llu, peak %llu, heaps used %u\n",
               name, heaps ? "yes" : "no", aliasedStats.transientResources,
               aliasedStats.aliasedResources,
               static_cast<unsigned long long>(aliasedStats.transientBytes),
               static_cast<unsigned long long>(aliasedStats.transientPeakBytes),
               aliasedStats.transientHeaps);
    }

    // RG4: async compute와 graphics pipe 실행의 결과(합성 출력·probe 픽셀·HZB mip)가 같다. compute
    // queue가 있는 장치는 Always가 compute queue를 쓰고, 겹침은 queue별 timestamp로 재어 출력만
    // 한다(시간에 기대는 단언은 하지 않는다).
    if (ok && device.Caps().features.computeShaders &&
        HasPixelFormatCaps(device.GetPixelFormatCaps(hzbDesc.format),
                           RequiredPixelFormatCaps(hzbDesc))) {
        auto fill = hzb::CreateFillPipeline(device);
        auto reduce = hzb::CreateReducePipeline(device);
        Array<uint8> graphicsProbe(allocator);
        Array<uint8> asyncProbe(allocator);
        Array<uint8> graphicsOutput(allocator);
        Array<uint8> asyncOutput(allocator);
        const bool ready = fill && reduce && graphicsProbe.TryResize(pixels.Size()).IsOk() &&
                           asyncProbe.TryResize(pixels.Size()).IsOk() &&
                           graphicsOutput.TryResize(pixels.Size()).IsOk() &&
                           asyncOutput.TryResize(pixels.Size()).IsOk();
        Check(ready, "async compute HZB pipelines and readback buffers");
        AsyncComputeRun graphicsRun;
        AsyncComputeRun asyncRun;
        bool ran = false;
        if (ready) {
            auto cleared = arena.Reset();
            (void)cleared;
            const bool graphicsOk =
                RunAsyncComputeGraph(bench, allocator, output.Value(), fill.Value(), reduce.Value(),
                                     GraphAsyncCompute::Disabled, graphicsProbe, graphicsRun);
            const bool graphicsRead =
                device.ReadPixels(outputTexture.Value(), graphicsOutput.View()).IsOk();
            cleared = arena.Reset();
            (void)cleared;
            const bool asyncOk =
                RunAsyncComputeGraph(bench, allocator, output.Value(), fill.Value(), reduce.Value(),
                                     GraphAsyncCompute::Always, asyncProbe, asyncRun);
            const bool asyncRead =
                device.ReadPixels(outputTexture.Value(), asyncOutput.View()).IsOk();
            ran = graphicsOk && asyncOk && graphicsRead && asyncRead;
        }

        const bool queue = device.Caps().features.asyncCompute;
        Check(ran, "the async compute graph runs on the graphics pipe and with async compute");
        Check(ran && graphicsRun.mismatchedMips == 0 && asyncRun.mismatchedMips == 0,
              "async and graphics-pipe HZB mips match the CPU reference");
        Check(ran && platform::MemoryEquals(graphicsProbe.Data(), asyncProbe.Data(),
                                            graphicsProbe.Size()),
              "async and graphics-pipe runs give the same probe pixels");
        Check(ran &&
                  platform::MemoryEquals(graphicsOutput.Data(), asyncOutput.Data(),
                                         graphicsOutput.Size()) &&
                  CenterIs(asyncOutput, 255, 0, 0),
              "async and graphics-pipe runs give the same composite (red)");
        Check(graphicsRun.asyncPasses == 0 && asyncRun.asyncPasses == (queue ? hzb::Mips : 0),
              "async compute passes run on the compute queue only when the device has one");
        if (queue) {
            Check(asyncRun.computeRecordings == 1 && asyncRun.queueWaits == 2,
                  "one compute recording with a fork and a join wait");
        }

        printf("  %s render graph async compute: queue %s (efficient %s), async passes %u, "
               "compute recordings %u, queue waits %u\n",
               name, queue ? "yes" : "no",
               device.Caps().features.efficientAsyncCompute ? "yes" : "no", asyncRun.asyncPasses,
               asyncRun.computeRecordings, asyncRun.queueWaits);
        if (asyncRun.measured) {
            printf("  %s render graph async compute overlap: graphics %.3f ms, compute %.3f ms, "
                   "overlap %.3f ms (graphics pipe run: overlap %.3f ms)\n",
                   name, asyncRun.graphicsMs, asyncRun.computeMs, asyncRun.overlapMs,
                   graphicsRun.overlapMs);
        } else {
            printf("  %s render graph async compute overlap: not measured (no timestamps)\n", name);
        }

        if (fill) {
            device.Retire(fill.Value());
        }

        if (reduce) {
            device.Retire(reduce.Value());
        }
    }

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
