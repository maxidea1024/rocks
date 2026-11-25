#include "GraphTestSupport.h"

#include <RenderBase/PipelineCache.h>
#include <RenderBase/ShaderSet.h>
#include <ShaderCore/ShaderBindingLayout.h>
#include <ShaderCore/ShaderLibrary.h>
#include <ShaderCore/ShaderTypeRegistry.h>
#include <TestSupport/Test.h>
#include <stdio.h>

// 그래프 compute pass 도우미와 버퍼 view 시험이다(Docs/Design/RenderBase 4.4절 "GX3·GX4 compute
// 연결 설계" (3)·(4)): 직접·빈·간접 dispatch의 기록 순서와 IndirectArgs 전이, 버퍼 view 생성·묶기·
// Retire, 검증 오류마다 그 뒤 Execute 실패, async compute pass, 캐시 판의 두 그래프 적중.
using namespace rocks;
namespace render_graph_compute_tests_file {

using namespace rocks::render;
using namespace rocks::render::testing;
using graphics::BufferUsage;
using graphics::BufferViewKind;
using graphics::ResourceAccess;
using graphics::ViewAccess;
using Op = RecordingDevice::Op;

namespace {

TraceScopeName ArgumentsName("Compute.Arguments");
TraceScopeName ItemsName("Compute.Items");
TraceScopeName EmptyName("Compute.Empty");
TraceScopeName ReadName("Compute.Read");
TraceScopeName BadName("Compute.Bad");

// 간접 인자를 담는 raw 버퍼 크기와 인자 자리다(인자 하나 12 byte).
constexpr uint64 ArgumentBytes = 32;
constexpr uint64 ArgumentOffset = 12;
// 구조화 버퍼 원소 byte와 원소 수다.
constexpr uint32 ItemStride = 16;
constexpr uint32 ItemCount = 4;

graphics::BufferDesc ArgumentsDesc(uint64 size = ArgumentBytes) noexcept {
    graphics::BufferDesc desc;
    desc.size = size;
    desc.usage = BufferUsage::Indirect | BufferUsage::ShaderWrite | BufferUsage::Raw;
    return desc;
}

graphics::BufferDesc ItemsDesc() noexcept {
    graphics::BufferDesc desc;
    desc.size = ItemStride * ItemCount;
    desc.usage = BufferUsage::ShaderRead | BufferUsage::ShaderWrite | BufferUsage::Structured;
    desc.stride = ItemStride;
    return desc;
}

graphics::BufferViewDesc ViewOf(BufferViewKind kind, ViewAccess access) noexcept {
    graphics::BufferViewDesc desc;
    desc.kind = kind;
    desc.access = access;
    return desc;
}

graphics::PipelineHandle ComputePipeline(RecordingDevice& device) noexcept {
    graphics::ComputePipelineDesc desc;
    auto created = device.CreateComputePipeline(desc);
    if (!created) {
        platform::Fatal("render graph compute test pipeline creation failed");
    }

    return created.Value();
}

// 기록한 op의 value 가운데 하나라도 value인지다.
bool HasCall(const GraphBench& bench, Op op, uint64 value) noexcept {
    for (const RecordingDevice::Call& call : bench.device.Calls()) {
        if (call.op == op && call.value == value) {
            return true;
        }
    }

    return false;
}

// 버퍼 slot index가 after로 가는 전이가 있는지다.
bool HasBufferTransition(const GraphBench& bench, ResourceAccess after) noexcept {
    for (const RecordingDevice::TransitionCall& call : bench.device.TransitionLog()) {
        if (!call.texture && call.after == after) {
            return true;
        }
    }

    return false;
}

// 그래프 하나에서 잘못된 compute pass가 검증 오류이고 그 뒤 Execute가 InvalidArgument인지 본다.
template <class Build>
bool RejectsThenFails(GraphBench& bench, const char* expected, Build&& build) noexcept {
    RenderGraph graph(bench.Next());
    const ErrorCode added = build(graph);
    const bool message = ContainsText(graph.LastError(), expected);
    if (!message) {
        printf("    last error: %.*s\n", static_cast<int>(graph.LastError().ByteCount()),
               graph.LastError().Data());
    }

    return added == ErrorCode::InvalidArgument && message &&
           graph.Execute().Error() == ErrorCode::InvalidArgument;
}

auto NoBind() noexcept {
    return [](GraphPassContext&) noexcept {};
}

} // namespace

// 직접 dispatch가 raw 버퍼에 인자를 쓰고, 간접 dispatch가 그 인자로 구조화 버퍼에 쓴다. 인자
// 버퍼는 도우미가 IndirectArgs로 선언해 UAV → IndirectArgs 전이가 나고, 빈 그룹 수 pass는
// dispatch를 기록하지 않는다. 버퍼 view는 처음 쓰는 pass 직전에 만들고 그래프 끝에 Retire한다.
TEST(RenderGraph_ComputePassDispatchesDirectAndIndirect) {
    GraphBench bench;
    const graphics::PipelineHandle pipeline = ComputePipeline(bench.device);
    graphics::BufferViewHandle seen[3] = {};
    graphics::BufferViewHandle* views = seen;
    {
        RenderGraph graph(bench.Next());
        auto arguments = graph.CreateBuffer(ArgumentsDesc(), "Arguments");
        auto items = graph.CreateBuffer(ItemsDesc(), "Items");
        REQUIRE(arguments && items);
        auto argumentsWrite = graph.CreateBufferView(
            arguments.Value(), ViewOf(BufferViewKind::Raw, ViewAccess::ReadWrite));
        auto itemsWrite = graph.CreateBufferView(
            items.Value(), ViewOf(BufferViewKind::Structured, ViewAccess::ReadWrite));
        auto itemsRead = graph.CreateBufferView(
            items.Value(), ViewOf(BufferViewKind::Structured, ViewAccess::Read));
        REQUIRE(argumentsWrite && itemsWrite && itemsRead);

        GraphPassAccesses writeArguments(graph);
        writeArguments.BufferView(argumentsWrite.Value(), ResourceAccess::UavCompute);
        GraphComputeDispatch direct;
        direct.pipeline = pipeline;
        direct.groups = {1, 2, 3};
        const GraphBufferView argumentsView = argumentsWrite.Value();
        REQUIRE(graph.AddComputePass(ArgumentsName, GraphPassKind::Compute, GraphPassFlags::None,
                                     writeArguments, direct,
                                     [argumentsView, views](GraphPassContext& pass) noexcept {
                                         views[0] = pass.View(argumentsView);
                                         pass.Commands().SetWritableBufferView(0, views[0]);
                                     }));

        GraphPassAccesses writeItems(graph);
        writeItems.BufferView(itemsWrite.Value(), ResourceAccess::UavCompute);
        GraphComputeDispatch indirect;
        indirect.pipeline = pipeline;
        indirect.indirectArguments = arguments.Value();
        indirect.indirectOffset = ArgumentOffset;
        const GraphBufferView itemsView = itemsWrite.Value();
        REQUIRE(graph.AddComputePass(
            ItemsName, GraphPassKind::Compute, GraphPassFlags::None, writeItems, indirect,
            [itemsView, argumentsView, views](GraphPassContext& pass) noexcept {
                views[1] = pass.View(itemsView);
                // 이 pass가 선언하지 않은 view는 null이다.
                CHECK(pass.View(argumentsView).IsNull());
                pass.Commands().SetWritableBufferView(0, views[1]);
            }));
        // 도우미가 인자 버퍼 접근을 더했다(view 하나 + IndirectArgs 하나).
        CHECK(writeItems.Accesses().Size() == 2);

        GraphPassAccesses readItems(graph);
        readItems.BufferView(itemsRead.Value(), ResourceAccess::SrvCompute);
        GraphComputeDispatch empty;
        empty.pipeline = pipeline;
        empty.groups = {0, 1, 1};
        const GraphBufferView readView = itemsRead.Value();
        uint32 emptyBinds = 0;
        uint32* binds = &emptyBinds;
        REQUIRE(graph.AddComputePass(EmptyName, GraphPassKind::Compute,
                                     GraphPassFlags::HasSideEffects, readItems, empty,
                                     [readView, views, binds](GraphPassContext& pass) noexcept {
                                         views[2] = pass.View(readView);
                                         ++*binds;
                                     }));
        REQUIRE(graph.Execute());
        CHECK(emptyBinds == 0);
        CHECK(graph.Stats().validationErrors == 0);
    }

    CHECK(!seen[0].IsNull() && !seen[1].IsNull() && !(seen[0] == seen[1]));
    CHECK(bench.Count(Op::Dispatch) == 1);
    CHECK(HasCall(bench, Op::Dispatch, RecordingDevice::DispatchValue(1, 2, 3)));
    CHECK(bench.Count(Op::DispatchIndirect) == 1);
    CHECK(HasCall(bench, Op::DispatchIndirect, ArgumentOffset));
    // 순서: 파이프라인 → 묶기 → dispatch(각 pass).
    CHECK(bench.IndexOf(Op::SetPipeline, 0) < bench.IndexOf(Op::SetWritableBufferView, 0));
    CHECK(bench.IndexOf(Op::SetWritableBufferView, 0) < bench.IndexOf(Op::Dispatch, 0));
    CHECK(bench.IndexOf(Op::Dispatch, 0) < bench.IndexOf(Op::SetWritableBufferView, 1));
    CHECK(bench.IndexOf(Op::SetWritableBufferView, 1) < bench.IndexOf(Op::DispatchIndirect, 0));
    CHECK(HasCall(bench, Op::SetWritableBufferView, 1) &&
          bench.Count(Op::SetWritableBufferView) == 2);
    CHECK(HasBufferTransition(bench, ResourceAccess::IndirectArgs));
    // 빈 pass의 읽기 view도 만들고(선언했으므로) 셋 모두 그래프 끝에 놓는다.
    CHECK(bench.Count(Op::CreateBufferView) == 3);
    CHECK(HasCall(bench, Op::CreateBufferView,
                  (1ull << 32) | static_cast<uint32>(BufferViewKind::Raw)));
    CHECK(bench.Count(Op::RetireBufferView) == 3 && bench.device.LiveBufferViews() == 0);
    CHECK(bench.device.ValidationErrorCount() == 0 && bench.device.IgnoredDraws() == 0);
    bench.device.Retire(pipeline);
}

// 같은 IndirectArgs 선언을 호출자가 이미 했으면 다시 더하지 않는다. import 인자 버퍼는 offset
// 정렬만 보고 장치가 범위를 본다.
TEST(RenderGraph_ComputePassIndirectArgumentsDeclarations) {
    GraphBench bench;
    const graphics::PipelineHandle pipeline = ComputePipeline(bench.device);
    const uint8 zeros[ArgumentBytes] = {};
    auto external = bench.device.CreateBuffer(ArgumentsDesc(), Span<const uint8>(zeros));
    REQUIRE(external);
    auto handle = bench.handles.Reserve(RenderResourceKind::Buffer);
    REQUIRE(handle && bench.handles.BindBuffer(handle.Value(), external.Value()));
    {
        RenderGraph graph(bench.Next());
        auto arguments = graph.ImportBuffer(handle.Value(), "ExternalArguments");
        REQUIRE(arguments);
        GraphPassAccesses accesses(graph);
        accesses.Buffer(arguments.Value(), ResourceAccess::IndirectArgs);
        GraphComputeDispatch dispatch;
        dispatch.pipeline = pipeline;
        dispatch.indirectArguments = arguments.Value();
        dispatch.indirectOffset = 4;
        REQUIRE(graph.AddComputePass(ReadName, GraphPassKind::Compute,
                                     GraphPassFlags::HasSideEffects, accesses, dispatch, NoBind()));
        CHECK(accesses.Accesses().Size() == 1);
        REQUIRE(graph.Execute());
    }

    CHECK(HasCall(bench, Op::DispatchIndirect, 4));
    CHECK(bench.device.ValidationErrorCount() == 0);
    CHECK(!bench.RetireHandle(handle.Value()));
    bench.device.Retire(pipeline);
}

// 검증 오류마다 AddComputePass가 InvalidArgument이고 그 그래프의 Execute도 실패한다.
TEST(RenderGraph_ComputePassRejectsInvalidDispatches) {
    GraphBench bench;
    bench.device.SetComputeGroupLimit(4, 4, 4);
    const graphics::PipelineHandle pipeline = ComputePipeline(bench.device);
    const uint8 zeros[ArgumentBytes] = {};
    auto external = bench.device.CreateBuffer(ArgumentsDesc(), Span<const uint8>(zeros));
    REQUIRE(external);
    auto handle = bench.handles.Reserve(RenderResourceKind::Buffer);
    REQUIRE(handle && bench.handles.BindBuffer(handle.Value(), external.Value()));

    auto direct = [&](GraphPassKind kind, graphics::PipelineHandle used, ComputeGroupCount groups,
                      const char* expected) noexcept {
        return RejectsThenFails(bench, expected, [&](RenderGraph& graph) noexcept {
            GraphPassAccesses accesses(graph);
            GraphComputeDispatch dispatch;
            dispatch.pipeline = used;
            dispatch.groups = groups;
            return graph
                .AddComputePass(BadName, kind, GraphPassFlags::HasSideEffects, accesses, dispatch,
                                NoBind())
                .Error();
        });
    };

    CHECK(direct(GraphPassKind::Raster, pipeline, {1, 1, 1}, "must be a Compute"));
    CHECK(direct(GraphPassKind::Copy, pipeline, {1, 1, 1}, "must be a Compute"));
    CHECK(direct(GraphPassKind::Compute, {}, {1, 1, 1}, "has no pipeline"));
    CHECK(direct(GraphPassKind::Compute, pipeline, {5, 1, 1}, "over the device limit"));
    CHECK(direct(GraphPassKind::AsyncCompute, pipeline, {1, 1, 5}, "over the device limit"));

    // 간접: 만든 버퍼는 용도·정렬·범위, import 버퍼는 정렬만, 다른 그래프 handle은 거부한다.
    auto indirect = [&](const graphics::BufferDesc& desc, uint64 offset, bool imported,
                        bool foreign, const char* expected) noexcept {
        return RejectsThenFails(bench, expected, [&](RenderGraph& graph) noexcept {
            GraphBuffer arguments;
            if (foreign) {
                RenderGraph other(bench.Next());
                arguments = other.CreateBuffer(desc, "Foreign").Value();
            } else if (imported) {
                arguments = graph.ImportBuffer(handle.Value(), "ExternalArguments").Value();
            } else {
                arguments = graph.CreateBuffer(desc, "Arguments").Value();
            }

            GraphPassAccesses accesses(graph);
            GraphComputeDispatch dispatch;
            dispatch.pipeline = pipeline;
            dispatch.indirectArguments = arguments;
            dispatch.indirectOffset = offset;
            return graph
                .AddComputePass(BadName, GraphPassKind::Compute, GraphPassFlags::HasSideEffects,
                                accesses, dispatch, NoBind())
                .Error();
        });
    };

    graphics::BufferDesc noIndirect = ArgumentsDesc();
    noIndirect.usage = BufferUsage::ShaderWrite | BufferUsage::Raw;
    CHECK(indirect(noIndirect, 0, false, false, "invalid indirect arguments"));
    CHECK(indirect(ArgumentsDesc(), 2, false, false, "invalid indirect arguments"));
    CHECK(indirect(ArgumentsDesc(16), 8, false, false, "invalid indirect arguments"));
    CHECK(indirect(ArgumentsDesc(), 2, true, false, "invalid indirect arguments"));
    CHECK(indirect(ArgumentsDesc(), 0, false, true, "from another graph"));

    // 버퍼 view 규칙: 구조화 view는 구조화 버퍼만, 다른 그래프 버퍼 거부, 쓰기 view는 UAV 접근만.
    {
        RenderGraph graph(bench.Next());
        auto arguments = graph.CreateBuffer(ArgumentsDesc(), "Arguments");
        REQUIRE(arguments);
        CHECK(graph
                  .CreateBufferView(arguments.Value(),
                                    ViewOf(BufferViewKind::Structured, ViewAccess::ReadWrite))
                  .Error() == ErrorCode::InvalidArgument);
        CHECK(ContainsText(graph.LastError(), "buffer view of 'Arguments' is invalid"));
        CHECK(graph.Execute().Error() == ErrorCode::InvalidArgument);
    }

    {
        RenderGraph graph(bench.Next());
        RenderGraph other(bench.Next());
        auto foreign = other.CreateBuffer(ItemsDesc(), "Foreign");
        REQUIRE(foreign);
        CHECK(graph
                  .CreateBufferView(foreign.Value(),
                                    ViewOf(BufferViewKind::Structured, ViewAccess::Read))
                  .Error() == ErrorCode::InvalidArgument);
        CHECK(ContainsText(graph.LastError(), "from another graph"));
    }

    CHECK(RejectsThenFails(bench, "through a read-write view", [&](RenderGraph& graph) noexcept {
        auto items = graph.CreateBuffer(ItemsDesc(), "Items");
        auto write = graph.CreateBufferView(
            items.Value(), ViewOf(BufferViewKind::Structured, ViewAccess::ReadWrite));
        GraphPassAccesses accesses(graph);
        accesses.BufferView(write.Value(), ResourceAccess::SrvCompute);
        GraphComputeDispatch dispatch;
        dispatch.pipeline = pipeline;
        return graph
            .AddComputePass(BadName, GraphPassKind::Compute, GraphPassFlags::HasSideEffects,
                            accesses, dispatch, NoBind())
            .Error();
    }));

    // 검증 실패한 pass는 아무것도 기록하지 않았다.
    CHECK(bench.Count(Op::Dispatch) == 0 && bench.Count(Op::DispatchIndirect) == 0);
    CHECK(!bench.RetireHandle(handle.Value()));
    bench.device.Retire(pipeline);
}

namespace {

ROCKS_SHADER_TYPE_NAME(GraphComputeName, "GraphComputeTest");

struct GraphComputeShader {
    static constexpr shader::ShaderTypeDesc Type = shader::MakeShaderTypeDesc(
        GraphComputeName, "/Engine/Private/GraphComputeTest.rsf", "MainCS",
        shader::ShaderStageCompute, shader::ShaderTypeOwner::Engine);
};

constexpr uint8 GraphComputeBytes[] = {4, 3, 2, 1};

// CS 하나짜리 쿠킹 라이브러리를 연 셰이더 집합이다.
struct ComputeSetBench {
    explicit ComputeSetBench(GraphBench& graph) noexcept
        : registry(graph.allocator), set(Desc(graph)) {
        const shader::ShaderTypeDesc* const types[] = {&GraphComputeShader::Type};
        if (!registry.Register(types) || !registry.Freeze()) {
            return;
        }

        Array<uint8> layout(graph.allocator);
        shader::ShaderBindingLayout binding(graph.allocator);
        binding.stage = shader::ShaderStageCompute;
        if (!shader::EncodeShaderBindingLayout(binding, layout)) {
            return;
        }

        const shader::ShaderLibraryValue values[] = {
            {shader::ShaderLibraryValueCode,
             Span<const uint8>(GraphComputeBytes, sizeof(GraphComputeBytes))},
            {shader::ShaderLibraryValueBindingLayout, layout.View()}};
        shader::ShaderLibraryBuildEntry entry;
        entry.key =
            shader::ShaderLibraryKey{0, GraphComputeName.hash, shader::NoVertexInputType, 0};
        entry.typeName = GraphComputeName.name;
        entry.stage = shader::ShaderStageCompute;
        entry.layoutSignature =
            shader::ShaderParameterLayoutSignature(Span<const shader::ShaderParameterLayout>());
        entry.values = values;
        shader::ShaderLibraryBuildDesc desc;
        desc.kind = shader::ShaderLibraryKind::Engine;
        if (!shader::SetShaderFormatFactsId(desc.facts, "D3D12_SM6")) {
            return;
        }

        desc.facts.backend = shader::ShaderBackend::D3D12;
        desc.typeSetSha256 = registry.TypeSetDigest();
        desc.entries = Span<const shader::ShaderLibraryBuildEntry>(&entry, 1);
        Array<uint8> bytes(graph.allocator);
        ready =
            shader::BuildShaderLibrary(desc, graph.allocator, bytes) && set.OpenCooked(Move(bytes));
    }

    ShaderSetDesc Desc(GraphBench& graph) noexcept {
        ShaderSetDesc desc;
        desc.allocator = &graph.allocator;
        desc.device = &graph.device;
        desc.registry = &registry;
        desc.formatId = "D3D12_SM6";
        return desc;
    }

    shader::ShaderTypeRegistry registry;
    ShaderSet set;
    bool ready = false;
};

} // namespace

// 캐시 판은 선언 때 파이프라인을 정한다(두 그래프에서 미적중 1·적중 1). AsyncCompute pass는 compute
// queue 기록으로 실행된다. 캐시 실패는 그래프 오류로 남아 Execute가 그 오류를 돌려준다.
TEST(RenderGraph_ComputePassFromCacheOnAsyncCompute) {
    GraphBench bench;
    bench.device.SetAsyncCompute(true);
    ComputeSetBench shaders(bench);
    REQUIRE(shaders.ready);
    auto entry = TypedShader<GraphComputeShader>::Find(shaders.set);
    REQUIRE(entry);
    PipelineCache cache(bench.allocator, bench.device);
    ComputePipelineRequest request;
    request.set = &shaders.set;
    request.computeEntry = entry.Value().Entry();
    request.state.bindings.writableBuffers[0] = "Items";
    for (uint32 run = 0; run < 2; ++run) {
        RenderGraphDesc desc = bench.Next();
        desc.asyncCompute = GraphAsyncCompute::Always;
        RenderGraph graph(desc);
        auto items = graph.CreateBuffer(ItemsDesc(), "Items");
        REQUIRE(items);
        auto write = graph.CreateBufferView(
            items.Value(), ViewOf(BufferViewKind::Structured, ViewAccess::ReadWrite));
        REQUIRE(write);
        GraphPassAccesses accesses(graph);
        accesses.BufferView(write.Value(), ResourceAccess::UavCompute);
        GraphComputeDispatch dispatch;
        dispatch.groups = {ItemCount, 1, 1};
        const GraphBufferView view = write.Value();
        auto pass = graph.AddComputePass(
            ItemsName, GraphPassKind::AsyncCompute, GraphPassFlags::None, accesses, cache, request,
            dispatch, [view](GraphPassContext& context) noexcept {
                context.Commands().SetWritableBufferView(0, context.View(view));
            });
        REQUIRE(pass);
        auto exported = graph.ExportBuffer(items.Value(), ResourceAccess::SrvGraphics);
        REQUIRE(exported);
        REQUIRE(graph.Execute());
        CHECK(graph.PassPipe(pass.Value()) == GraphPassPipe::AsyncCompute);
        CHECK(graph.Stats().asyncComputePasses == 1);
        bench.device.CompleteAll();
        (void)bench.RetireHandle(exported.Value());
    }

    CHECK(cache.Misses() == 1 && cache.Hits() == 1 && cache.Size() == 1);
    CHECK(bench.Count(Op::BeginCompute) == 2 && bench.Count(Op::Dispatch) == 2);
    CHECK(HasCall(bench, Op::Dispatch, RecordingDevice::DispatchValue(ItemCount, 1, 1)));
    CHECK(bench.device.ValidationErrorCount() == 0 && bench.device.IgnoredDraws() == 0);

    // 캐시 실패(집합 없음)는 그 오류이고 그래프의 Execute도 그 오류다.
    {
        RenderGraph graph(bench.Next());
        GraphPassAccesses accesses(graph);
        ComputePipelineRequest broken = request;
        broken.set = nullptr;
        auto pass =
            graph.AddComputePass(BadName, GraphPassKind::Compute, GraphPassFlags::HasSideEffects,
                                 accesses, cache, broken, GraphComputeDispatch{}, NoBind());
        CHECK(pass.Error() == ErrorCode::InvalidArgument);
        CHECK(ContainsText(graph.LastError(), "could not get its pipeline"));
        CHECK(graph.Execute().Error() == ErrorCode::InvalidArgument);
    }

    cache.Clear();
    bench.device.CompleteAll();
    bench.device.ReclaimCompleted();
}

} // namespace render_graph_compute_tests_file
