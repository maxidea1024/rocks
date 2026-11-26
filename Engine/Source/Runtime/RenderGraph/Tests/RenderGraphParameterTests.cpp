#include "GraphTestSupport.h"

#include <Base/InitializerList.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Move.h>
#include <RenderGraph/GraphParameterCollector.h>
#include <ShaderCore/ShaderBindingLayout.h>
#include <ShaderParameterTestFixtures/ShaderResourceFixtures.shaderparams.h>
#include <TestSupport/Test.h>
#include <stdio.h>

// 렌더 그래프 파라미터 수집기 시험이다(Docs/Design/ShaderSystem 16.2절 SSD65·SSD66 3). 같은
// 그래프를 명시 선언(GraphPassAccesses + 손 묶기)과 파라미터 구조체(AllocateParameters + 구조체
// AddPass + GraphPassContext::SetParameters)로 두 번 만들어 기록 장치의 명령 열이 같은지 본다 —
// Compute(그래프 SRV·UAV·구조화·raw·typed·접근만 선언)·Raster(그래프·장치 SRV·렌더 타깃)와 쓰지
// 않는 멤버. 수집기 음성(Raster의 UAV·Copy의 셰이더 멤버·Compute의 렌더 타깃·셰이더가 읽는 null
// 멤버·다른 구조체 지도· 생성 표 아님·null 값)도 본다.
using namespace rocks;
namespace render_graph_parameter_tests_file {

using namespace rocks::render;
using namespace rocks::render::testing;
using graphics::BufferUsage;
using graphics::BufferViewKind;
using graphics::ResourceAccess;
using graphics::ShaderStage;
using graphics::ViewAccess;
using shader::ShaderBindingLayout;
using shader::ShaderBindingResource;
using shader::ShaderParameterLayout;
using shader::ShaderParameterResource;
using shader::ShaderParameterResourceKind;
using shader::ShaderResourceKind;
using shader::ShaderResourceShape;
using shader_test_fixtures::FixtureComposite;
using shader_test_fixtures::FixtureReduce;

namespace {

TraceScopeName FillName("Parameters.Fill");
TraceScopeName DrawName("Parameters.Draw");
TraceScopeName ReduceName("Parameters.Reduce");
TraceScopeName CompositeName("Parameters.Composite");
TraceScopeName BadName("Parameters.Bad");

constexpr const ShaderParameterLayout& ReduceLayout =
    shader::ShaderParameterLayoutOf<FixtureReduce>();
constexpr const ShaderParameterLayout& CompositeLayout =
    shader::ShaderParameterLayoutOf<FixtureComposite>();

// 버퍼 크기·구조화 원소 byte다.
constexpr uint64 BufferBytes = 256;
constexpr uint32 BoneStride = 64;
constexpr uint32 CounterStride = 4;

// 지도 자리(값 1 slot과 이름 표 slot)다. Compute: source 텍스처 0·짝 0, target storage image 0,
// bones·words 이름 표 bufferViews 0·1, counters·histogram writableBuffers 0·1. Raster PS: source
// 0·짝 0, overlay 1·짝 1, weights bufferViews 0.
constexpr uint32 SourceSlot = 0;
constexpr uint32 OverlaySlot = 1;

// 멤버의 바인딩 자원(워커가 낸 v5 항목 모양)이다.
ShaderBindingResource Bound(const ShaderParameterLayout& layout,
                            const ShaderParameterResource& member, ShaderResourceKind kind,
                            uint16 slot, bool sampler) noexcept {
    ShaderBindingResource resource;
    resource.kind = kind;
    resource.slot = slot;
    resource.name = sampler ? member.samplerName : member.name;
    if (!sampler) {
        resource.shape = member.shape;
        resource.component = member.component;
        resource.structureStride = member.structureStride;
    }

    resource.structId = layout.structId;
    resource.memberOffset = member.offset;
    return resource;
}

const ShaderParameterResource& Member(const ShaderParameterLayout& layout,
                                      StringView name) noexcept {
    for (const ShaderParameterResource& resource : layout.resources) {
        if (resource.name == name) {
            return resource;
        }
    }

    // 없는 이름은 시험 결함이다(fixture 구조체가 바뀌었다).
    platform::Fatal("render graph parameter test member is missing");
}

// Compute 단계 layout이다. skipBones면 셰이더가 bones를 쓰지 않는다.
void FillComputeLayout(ShaderBindingLayout& layout, bool skipBones) noexcept {
    layout.stage = shader::ShaderStageCompute;
    const ShaderParameterLayout& table = ReduceLayout;
    (void)layout.resources.TryPushBack(
        Bound(table, Member(table, "source"), ShaderResourceKind::SampledTexture, 0, false));
    (void)layout.resources.TryPushBack(
        Bound(table, Member(table, "source"), ShaderResourceKind::Sampler, 0, true));
    (void)layout.resources.TryPushBack(
        Bound(table, Member(table, "target"), ShaderResourceKind::StorageImage, 0, false));
    if (!skipBones) {
        (void)layout.resources.TryPushBack(
            Bound(table, Member(table, "bones"), ShaderResourceKind::StorageBufferRead, 1, false));
    }

    (void)layout.resources.TryPushBack(
        Bound(table, Member(table, "words"), ShaderResourceKind::StorageBufferRead, 2, false));
    (void)layout.resources.TryPushBack(Bound(table, Member(table, "counters"),
                                             ShaderResourceKind::StorageBufferReadWrite, 1, false));
    (void)layout.resources.TryPushBack(
        Bound(table, Member(table, "histogram"), ShaderResourceKind::StorageImage, 1, false));
}

void FillPixelLayout(ShaderBindingLayout& layout) noexcept {
    layout.stage = shader::ShaderStagePixel;
    const ShaderParameterLayout& table = CompositeLayout;
    (void)layout.resources.TryPushBack(Bound(
        table, Member(table, "source"), ShaderResourceKind::SampledTexture, SourceSlot, false));
    (void)layout.resources.TryPushBack(
        Bound(table, Member(table, "source"), ShaderResourceKind::Sampler, SourceSlot, true));
    (void)layout.resources.TryPushBack(Bound(
        table, Member(table, "overlay"), ShaderResourceKind::SampledTexture, OverlaySlot, false));
    (void)layout.resources.TryPushBack(
        Bound(table, Member(table, "overlay"), ShaderResourceKind::Sampler, OverlaySlot, true));
    (void)layout.resources.TryPushBack(
        Bound(table, Member(table, "weights"), ShaderResourceKind::SampledTexture, 2, false));
}

graphics::BufferDesc BufferOf(BufferUsage usage, uint32 stride = 0) noexcept {
    graphics::BufferDesc desc;
    desc.size = BufferBytes;
    desc.usage = usage;
    desc.stride = stride;
    return desc;
}

graphics::BufferViewDesc
ViewOf(BufferViewKind kind, ViewAccess access,
       graphics::PixelFormat format = graphics::PixelFormat::Unknown) noexcept {
    graphics::BufferViewDesc desc;
    desc.kind = kind;
    desc.access = access;
    desc.format = format;
    return desc;
}

graphics::TextureViewDesc TextureViewOf(ViewAccess access) noexcept {
    graphics::TextureViewDesc desc;
    desc.access = access;
    desc.mipCount = 1;
    return desc;
}

// 그래프 한 벌의 자원이다.
struct Resources {
    GraphTexture scene;
    GraphTexture history;
    GraphTexture pyramid;
    GraphTexture output;
    GraphTextureView sceneRead;
    GraphTextureView pyramidWrite;
    GraphBuffer arguments;
    GraphBufferView bonesRead;
    GraphBufferView bonesWrite;
    GraphBufferView wordsRead;
    GraphBufferView wordsWrite;
    GraphBufferView countersWrite;
    GraphBufferView histogramWrite;
    GraphBufferView weightsRead;
    GraphBufferView weightsWrite;
    GraphBufferView argumentsWrite;

    bool Make(RenderGraph& graph) noexcept {
        auto sceneTexture = graph.CreateTexture(TargetDesc(), "Scene");
        auto historyTexture = graph.CreateTexture(TargetDesc(), "History");
        graphics::TextureDesc storage = TargetDesc();
        storage.format = graphics::PixelFormat::R32Float;
        storage.usage = graphics::TextureUsage::Storage;
        auto pyramidTexture = graph.CreateTexture(storage, "Pyramid");
        auto outputTexture = graph.CreateTexture(TargetDesc(), "Output");
        auto bones = graph.CreateBuffer(
            BufferOf(BufferUsage::ShaderRead | BufferUsage::ShaderWrite | BufferUsage::Structured,
                     BoneStride),
            "Bones");
        auto words = graph.CreateBuffer(
            BufferOf(BufferUsage::ShaderRead | BufferUsage::ShaderWrite | BufferUsage::Raw),
            "Words");
        auto counters = graph.CreateBuffer(
            BufferOf(BufferUsage::ShaderRead | BufferUsage::ShaderWrite | BufferUsage::Structured,
                     CounterStride),
            "Counters");
        auto histogram = graph.CreateBuffer(
            BufferOf(BufferUsage::ShaderRead | BufferUsage::ShaderWrite), "Histogram");
        auto weights = graph.CreateBuffer(
            BufferOf(BufferUsage::ShaderRead | BufferUsage::ShaderWrite), "Weights");
        auto argumentBuffer = graph.CreateBuffer(
            BufferOf(BufferUsage::Indirect | BufferUsage::ShaderWrite | BufferUsage::Raw),
            "Arguments");
        if (!sceneTexture || !historyTexture || !pyramidTexture || !outputTexture || !bones ||
            !words || !counters || !histogram || !weights || !argumentBuffer) {
            return false;
        }

        scene = sceneTexture.Value();
        history = historyTexture.Value();
        pyramid = pyramidTexture.Value();
        output = outputTexture.Value();
        arguments = argumentBuffer.Value();
        auto read = graph.CreateTextureView(scene, TextureViewOf(ViewAccess::Read));
        auto write = graph.CreateTextureView(pyramid, TextureViewOf(ViewAccess::ReadWrite));
        auto boneRead = graph.CreateBufferView(
            bones.Value(), ViewOf(BufferViewKind::Structured, ViewAccess::Read));
        auto boneWrite = graph.CreateBufferView(
            bones.Value(), ViewOf(BufferViewKind::Structured, ViewAccess::ReadWrite));
        auto wordRead =
            graph.CreateBufferView(words.Value(), ViewOf(BufferViewKind::Raw, ViewAccess::Read));
        auto wordWrite = graph.CreateBufferView(words.Value(),
                                                ViewOf(BufferViewKind::Raw, ViewAccess::ReadWrite));
        auto counterWrite = graph.CreateBufferView(
            counters.Value(), ViewOf(BufferViewKind::Structured, ViewAccess::ReadWrite));
        auto histogramView = graph.CreateBufferView(
            histogram.Value(),
            ViewOf(BufferViewKind::Typed, ViewAccess::ReadWrite, graphics::PixelFormat::R32Uint));
        auto weightRead =
            graph.CreateBufferView(weights.Value(), ViewOf(BufferViewKind::Typed, ViewAccess::Read,
                                                           graphics::PixelFormat::R32Float));
        auto weightWrite = graph.CreateBufferView(
            weights.Value(),
            ViewOf(BufferViewKind::Typed, ViewAccess::ReadWrite, graphics::PixelFormat::R32Float));
        auto argumentWrite =
            graph.CreateBufferView(arguments, ViewOf(BufferViewKind::Raw, ViewAccess::ReadWrite));
        if (!read || !write || !boneRead || !boneWrite || !wordRead || !wordWrite ||
            !counterWrite || !histogramView || !weightRead || !weightWrite || !argumentWrite) {
            return false;
        }

        sceneRead = read.Value();
        pyramidWrite = write.Value();
        bonesRead = boneRead.Value();
        bonesWrite = boneWrite.Value();
        wordsRead = wordRead.Value();
        wordsWrite = wordWrite.Value();
        countersWrite = counterWrite.Value();
        histogramWrite = histogramView.Value();
        weightsRead = weightRead.Value();
        weightsWrite = weightWrite.Value();
        argumentsWrite = argumentWrite.Value();
        return true;
    }
};

// 지도 둘(Compute·Raster)과 그 단계 layout이다.
struct Maps {
    explicit Maps(Allocator& allocator, bool skipBones) noexcept
        : compute(allocator), pixel(allocator) {
        FillComputeLayout(compute, skipBones);
        FillPixelLayout(pixel);
        computeNames.bufferViews[0] = "bones";
        computeNames.bufferViews[1] = "words";
        computeNames.writableTextures[0] = "target";
        computeNames.writableBuffers[0] = "counters";
        computeNames.writableBuffers[1] = "histogram";
        pixelNames.bufferViews[0] = "weights";
        const ShaderBindingLayout* computeStages[] = {&compute};
        const ShaderBindingLayout* pixelStages[] = {nullptr, &pixel};
        auto reduce =
            ShaderResourceMap::Build(allocator, ReduceLayout, computeStages, computeNames, &reason);
        auto composite =
            ShaderResourceMap::Build(allocator, CompositeLayout, pixelStages, pixelNames, &reason);
        if (!reduce || !composite) {
            platform::Fatal("render graph parameter test resource maps could not be built");
        }

        auto reduceMade = UniquePtr<ShaderResourceMap>::TryMake(allocator, Move(reduce.Value()));
        auto compositeMade =
            UniquePtr<ShaderResourceMap>::TryMake(allocator, Move(composite.Value()));
        if (!reduceMade || !compositeMade) {
            platform::Fatal("render graph parameter test resource maps could not be allocated");
        }

        reduceMap = Move(reduceMade.Value());
        compositeMap = Move(compositeMade.Value());
    }

    ShaderBindingLayout compute;
    ShaderBindingLayout pixel;
    graphics::PipelineBindingNames computeNames;
    graphics::PipelineBindingNames pixelNames;
    const char* reason = "";
    UniquePtr<ShaderResourceMap> reduceMap;
    UniquePtr<ShaderResourceMap> compositeMap;
};

// 텍스처 하나의 색 attachment(기본 clear)다.
GraphColorTarget ColorOf(GraphTexture texture) noexcept {
    GraphColorTarget target;
    target.texture = texture;
    return target;
}

graphics::SamplerDesc PointSampler() noexcept {
    graphics::SamplerDesc desc;
    desc.filter = graphics::Filter::Point;
    return desc;
}

constexpr graphics::SubresourceRange HistoryRange = {0, 1, 0, 1};

// 앞 두 pass(버퍼를 쓰는 Compute, 두 색 타깃을 그리는 Raster)는 두 경로에 같은 명시 선언이다.
bool AddProducers(RenderGraph& graph, const Resources& resources,
                  graphics::PipelineHandle pipeline) noexcept {
    GraphPassAccesses fill(graph);
    fill.BufferView(resources.bonesWrite, ResourceAccess::UavCompute)
        .BufferView(resources.wordsWrite, ResourceAccess::UavCompute)
        .BufferView(resources.weightsWrite, ResourceAccess::UavCompute)
        .BufferView(resources.argumentsWrite, ResourceAccess::UavCompute);
    GraphPassAccesses draw(graph);
    draw.ColorTarget(ColorOf(resources.scene)).ColorTarget(ColorOf(resources.history));
    return graph.AddPass(FillName, GraphPassKind::Compute, GraphPassFlags::None, fill,
                         [pipeline](GraphPassContext& pass) noexcept {
                             pass.Commands().SetPipeline(pipeline);
                             pass.Commands().Dispatch(1, 1);
                         }) &&
           graph.AddPass(DrawName, GraphPassKind::Raster, GraphPassFlags::None, draw,
                         [](GraphPassContext& pass) noexcept { pass.Commands().Draw(3); });
}

// 명시 선언 경로: 수집기가 만들 접근과 SetParameters가 낼 묶기를 손으로 쓴다(지도 표 순서·단계
// 순서).
bool AddExplicit(RenderGraph& graph, const Resources& r, graphics::PipelineHandle pipeline,
                 graphics::TextureHandle overlay, bool skipBones) noexcept {
    GraphPassAccesses reduce(graph);
    reduce.TextureView(r.sceneRead, ResourceAccess::SrvCompute)
        .TextureView(r.pyramidWrite, ResourceAccess::UavCompute);
    if (!skipBones) {
        reduce.BufferView(r.bonesRead, ResourceAccess::SrvCompute);
    }

    reduce.BufferView(r.wordsRead, ResourceAccess::SrvCompute)
        .BufferView(r.countersWrite, ResourceAccess::UavCompute)
        .BufferView(r.histogramWrite, ResourceAccess::UavCompute)
        .Texture(r.history, ResourceAccess::SrvCompute, HistoryRange)
        .Buffer(r.arguments, ResourceAccess::IndirectArgs);
    const Resources copy = r;
    auto reduced =
        graph.AddPass(ReduceName, GraphPassKind::Compute, GraphPassFlags::HasSideEffects, reduce,
                      [copy, pipeline, skipBones](GraphPassContext& pass) noexcept {
                          graphics::CommandContext& commands = pass.Commands();
                          const graphics::SamplerDesc point = PointSampler();
                          commands.SetPipeline(pipeline);
                          commands.SetTexture(ShaderStage::Compute, 0, pass.View(copy.sceneRead));
                          (void)commands.SetSampler(ShaderStage::Compute, 0, &point);
                          commands.SetWritableTexture(0, pass.View(copy.pyramidWrite));
                          if (!skipBones) {
                              commands.SetBufferView(0, pass.View(copy.bonesRead));
                          }

                          commands.SetBufferView(1, pass.View(copy.wordsRead));
                          commands.SetWritableBufferView(0, pass.View(copy.countersWrite));
                          commands.SetWritableBufferView(1, pass.View(copy.histogramWrite));
                          commands.Dispatch(1, 1);
                      });
    GraphPassAccesses composite(graph);
    composite.Texture(r.scene, ResourceAccess::SrvGraphics)
        .BufferView(r.weightsRead, ResourceAccess::SrvGraphics)
        .ColorTarget(ColorOf(r.output));
    auto composited = graph.AddPass(
        CompositeName, GraphPassKind::Raster, GraphPassFlags::HasSideEffects, composite,
        [copy, overlay](GraphPassContext& pass) noexcept {
            graphics::CommandContext& commands = pass.Commands();
            commands.SetTexture(ShaderStage::Pixel, SourceSlot, pass.Texture(copy.scene));
            (void)commands.SetSampler(ShaderStage::Pixel, SourceSlot, nullptr);
            commands.SetTexture(ShaderStage::Pixel, OverlaySlot, overlay);
            (void)commands.SetSampler(ShaderStage::Pixel, OverlaySlot, nullptr);
            commands.SetBufferView(0, pass.View(copy.weightsRead));
            commands.Draw(3);
        });
    return reduced && composited;
}

// 파라미터 구조체 경로다.
bool AddCollected(RenderGraph& graph, const Resources& r, graphics::PipelineHandle pipeline,
                  graphics::TextureHandle overlay, const Maps& maps) noexcept {
    FixtureReduce* reduce = graph.AllocateParameters<FixtureReduce>();
    FixtureComposite* composite = graph.AllocateParameters<FixtureComposite>();
    if (reduce == nullptr || composite == nullptr) {
        return false;
    }

    reduce->size = {4, 4, 1, 0};
    reduce->source.view = r.sceneRead;
    reduce->source.sampler = SamplerOverride::Of(PointSampler());
    reduce->target.view = r.pyramidWrite;
    reduce->bones.view = r.bonesRead;
    reduce->words.view = r.wordsRead;
    reduce->counters.view = r.countersWrite;
    reduce->histogram.view = r.histogramWrite;
    reduce->history.texture = r.history;
    reduce->history.access = ResourceAccess::SrvCompute;
    reduce->history.range = HistoryRange;
    reduce->arguments.buffer = r.arguments;
    reduce->arguments.access = ResourceAccess::IndirectArgs;
    const ShaderResourceMap* reduceMap = maps.reduceMap.Get();
    auto reduced =
        graph.AddPass(ReduceName, GraphPassKind::Compute, GraphPassFlags::HasSideEffects, reduce,
                      reduceMap, [reduce, reduceMap, pipeline](GraphPassContext& pass) noexcept {
                          pass.Commands().SetPipeline(pipeline);
                          CHECK(pass.SetParameters(*reduceMap, reduce));
                          pass.Commands().Dispatch(1, 1);
                      });
    composite->source.texture = r.scene;
    composite->overlay.texture = overlay;
    composite->weights.view = r.weightsRead;
    composite->targets.colors[0].texture = r.output;
    composite->targets.colorCount = 1;
    const ShaderResourceMap* compositeMap = maps.compositeMap.Get();
    auto composited = graph.AddPass(CompositeName, GraphPassKind::Raster,
                                    GraphPassFlags::HasSideEffects, composite, compositeMap,
                                    [composite, compositeMap](GraphPassContext& pass) noexcept {
                                        CHECK(pass.SetParameters(*compositeMap, composite));
                                        pass.Commands().Draw(3);
                                    });
    return reduced && composited;
}

// 그래프 하나를 만들어 실행하고 기록 장치의 명령 열을 calls에 둔다.
bool Record(bool collected, bool skipBones, Array<RecordingDevice::Call>& calls,
            GraphStats& stats) noexcept {
    GraphBench bench;
    Maps maps(bench.allocator, skipBones);
    graphics::ComputePipelineDesc computeDesc;
    auto pipeline = bench.device.CreateComputePipeline(computeDesc);
    graphics::TextureDesc overlayDesc = TargetDesc();
    overlayDesc.usage = graphics::TextureUsage::Sampled;
    auto overlay = bench.device.CreateTexture(overlayDesc, {});
    if (!pipeline || !overlay) {
        return false;
    }

    {
        RenderGraph graph(bench.Next());
        Resources resources;
        if (!resources.Make(graph) || !AddProducers(graph, resources, pipeline.Value())) {
            return false;
        }

        const bool added =
            collected ? AddCollected(graph, resources, pipeline.Value(), overlay.Value(), maps)
                      : AddExplicit(graph, resources, pipeline.Value(), overlay.Value(), skipBones);
        if (!added || !graph.Execute()) {
            printf("    %.*s\n", static_cast<int>(graph.LastError().ByteCount()),
                   graph.LastError().Data());
            return false;
        }

        stats = graph.Stats();
    }

    for (const RecordingDevice::Call& call : bench.device.Calls()) {
        if (!calls.TryPushBack(call)) {
            return false;
        }
    }

    return bench.device.ValidationErrorCount() == 0;
}

bool SameCalls(Span<const RecordingDevice::Call> left,
               Span<const RecordingDevice::Call> right) noexcept {
    if (left.Size() != right.Size()) {
        printf("    call counts differ: %zu and %zu\n", left.Size(), right.Size());
        return false;
    }

    for (usize index = 0; index < left.Size(); ++index) {
        if (!(left[index] == right[index])) {
            printf("    call %zu differs: op %u value %llu and op %u value %llu\n", index,
                   static_cast<unsigned>(left[index].op),
                   static_cast<unsigned long long>(left[index].value),
                   static_cast<unsigned>(right[index].op),
                   static_cast<unsigned long long>(right[index].value));
            return false;
        }
    }

    return true;
}

usize CountOp(Span<const RecordingDevice::Call> calls, RecordingDevice::Op op) noexcept {
    usize count = 0;
    for (const RecordingDevice::Call& call : calls) {
        count += call.op == op ? 1u : 0u;
    }

    return count;
}

} // namespace

// 두 경로의 명령 열(전이·view 생성·묶기·그리기·dispatch 전부)이 같다. 쓰지 않는 멤버(bones)는
// 수집기가 의존에서 빼고 SetParameters도 묶지 않는다.
TEST(RenderGraph_ParameterStructCollectorMatchesExplicitAccesses) {
    for (const bool skipBones : {false, true}) {
        SystemAllocator allocator;
        Array<RecordingDevice::Call> explicitCalls(allocator);
        Array<RecordingDevice::Call> collectedCalls(allocator);
        GraphStats explicitStats;
        GraphStats collectedStats;
        REQUIRE(Record(false, skipBones, explicitCalls, explicitStats));
        REQUIRE(Record(true, skipBones, collectedCalls, collectedStats));
        CHECK(SameCalls(explicitCalls.View(), collectedCalls.View()));
        CHECK(explicitStats.validationErrors == 0 && collectedStats.validationErrors == 0);
        CHECK(explicitStats.transitions == collectedStats.transitions);
        CHECK(explicitStats.culledPasses == collectedStats.culledPasses);
        // 묶기가 실제로 났다(Compute 텍스처·샘플러·쓰기 텍스처, Raster 텍스처 둘·샘플러 둘).
        using Op = RecordingDevice::Op;
        CHECK(CountOp(collectedCalls.View(), Op::SetTexture) == 3);
        CHECK(CountOp(collectedCalls.View(), Op::SetSampler) == 3);
        CHECK(CountOp(collectedCalls.View(), Op::SetWritableTexture) == 1);
        CHECK(CountOp(collectedCalls.View(), Op::SetBufferView) == (skipBones ? 2u : 3u));
        CHECK(CountOp(collectedCalls.View(), Op::SetWritableBufferView) == 2);
    }
}

// 수집기 음성: 그래프 검증 오류로 남고 그 뒤 Execute가 실패한다.
TEST(RenderGraph_ParameterStructCollectorRejectsMisuse) {
    GraphBench bench;
    Maps maps(bench.allocator, false);
    const auto expectRejected = [&](const char* expected, auto&& add) noexcept {
        RenderGraph graph(bench.Next());
        Resources resources;
        if (!resources.Make(graph)) {
            return false;
        }

        const ErrorCode added = add(graph, resources);
        const bool message = ContainsText(graph.LastError(), expected);
        if (!message) {
            printf("    last error: %.*s\n", static_cast<int>(graph.LastError().ByteCount()),
                   graph.LastError().Data());
        }

        return added == ErrorCode::InvalidArgument && message &&
               graph.Execute().Error() == ErrorCode::InvalidArgument;
    };
    const auto none = [](GraphPassContext&) noexcept {};
    // Raster pass의 UAV 멤버.
    CHECK(expectRejected("UAV", [&](RenderGraph& graph, const Resources& r) noexcept {
        FixtureReduce* parameters = graph.AllocateParameters<FixtureReduce>();
        parameters->target.view = r.pyramidWrite;
        return graph
            .AddPass(BadName, GraphPassKind::Raster, GraphPassFlags::None, parameters, nullptr,
                     none)
            .Error();
    }));
    // Copy pass의 셰이더 멤버.
    CHECK(expectRejected("copy passes", [&](RenderGraph& graph, const Resources& r) noexcept {
        FixtureComposite* parameters = graph.AllocateParameters<FixtureComposite>();
        parameters->source.texture = r.scene;
        return graph
            .AddPass(BadName, GraphPassKind::Copy, GraphPassFlags::None, parameters, nullptr, none)
            .Error();
    }));
    // Compute pass의 렌더 타깃.
    CHECK(expectRejected("render target", [&](RenderGraph& graph, const Resources& r) noexcept {
        FixtureComposite* parameters = graph.AllocateParameters<FixtureComposite>();
        parameters->targets.colors[0].texture = r.output;
        parameters->targets.colorCount = 1;
        return graph
            .AddPass(BadName, GraphPassKind::Compute, GraphPassFlags::None, parameters, nullptr,
                     none)
            .Error();
    }));
    // 색 타깃이 MaxColorTargets 초과.
    CHECK(expectRejected("more than 8", [&](RenderGraph& graph, const Resources& r) noexcept {
        FixtureComposite* parameters = graph.AllocateParameters<FixtureComposite>();
        parameters->targets.colors[0].texture = r.output;
        parameters->targets.colorCount = graphics::MaxColorTargets + 1;
        return graph
            .AddPass(BadName, GraphPassKind::Raster, GraphPassFlags::None, parameters, nullptr,
                     none)
            .Error();
    }));
    // 셰이더가 읽는(지도에 자리 있는) 멤버가 null.
    CHECK(expectRejected("is null", [&](RenderGraph& graph, const Resources& r) noexcept {
        FixtureReduce* parameters = graph.AllocateParameters<FixtureReduce>();
        parameters->source.view = r.sceneRead;
        return graph
            .AddPass(BadName, GraphPassKind::Compute, GraphPassFlags::None, parameters,
                     maps.reduceMap.Get(), none)
            .Error();
    }));
    // 다른 구조체의 지도.
    CHECK(expectRejected("not built from", [&](RenderGraph& graph, const Resources&) noexcept {
        FixtureReduce* parameters = graph.AllocateParameters<FixtureReduce>();
        return graph
            .AddPass(BadName, GraphPassKind::Compute, GraphPassFlags::None, parameters,
                     maps.compositeMap.Get(), none)
            .Error();
    }));
    // null 값.
    CHECK(expectRejected("value", [&](RenderGraph& graph, const Resources&) noexcept {
        const FixtureReduce* parameters = nullptr;
        return graph
            .AddPass(BadName, GraphPassKind::Compute, GraphPassFlags::None, parameters, nullptr,
                     none)
            .Error();
    }));

    // 지도 없이 넘긴 null 멤버는 의존이 아니다. 생성 표가 아닌 표는 거부한다.
    RenderGraph graph(bench.Next());
    Resources resources;
    REQUIRE(resources.Make(graph));
    FixtureComposite* parameters = graph.AllocateParameters<FixtureComposite>();
    REQUIRE(parameters != nullptr);
    CHECK(parameters->targets.colorCount == 0 && parameters->source.texture.IsNull());
    parameters->weights.view = resources.weightsRead;
    GraphPassAccesses accesses(graph);
    REQUIRE(CollectGraphPassAccesses(accesses, GraphPassKind::Raster, CompositeLayout, parameters,
                                     nullptr));
    REQUIRE(accesses.Accesses().Size() == 1);
    CHECK(accesses.Accesses()[0].access == ResourceAccess::SrvGraphics);
    CHECK(accesses.Target().kind == GraphTargetKind::None);
    const ShaderParameterLayout handWritten(CompositeLayout.name, false, 0,
                                            Span<const shader::ShaderParameterMember>());
    const char* reason = "";
    CHECK(CollectGraphPassAccesses(accesses, GraphPassKind::Raster, handWritten, parameters,
                                   nullptr, &reason)
              .Error() == ErrorCode::InvalidArgument);
    CHECK(ContainsText(StringView(reason), "generated"));
    // pass 문맥의 SetParameters는 생성 표 지도만 받는다.
    constexpr ResourceParameter table[] = {{"weights", ResourceParameterKind::BufferView}};
    ShaderBindingLayout plain(bench.allocator);
    plain.stage = shader::ShaderStagePixel;
    ShaderBindingResource weights;
    weights.kind = ShaderResourceKind::SampledTexture;
    weights.name = "weights";
    weights.shape = ShaderResourceShape::TypedBuffer;
    weights.component = shader::ShaderComponentType::Float;
    (void)plain.resources.TryPushBack(weights);
    const ShaderBindingLayout* stages[] = {&plain};
    auto plainMap = ShaderResourceMap::Build(bench.allocator, table, stages, maps.pixelNames);
    REQUIRE(plainMap);
    GraphPassAccesses draw(graph);
    draw.ColorTarget(ColorOf(resources.output));
    bool rejected = false;
    bool* seen = &rejected;
    const ShaderResourceMap* map = &plainMap.Value();
    REQUIRE(graph.AddPass(CompositeName, GraphPassKind::Raster, GraphPassFlags::HasSideEffects,
                          draw, [map, parameters, seen](GraphPassContext& pass) noexcept {
                              *seen = pass.SetParameters(*map, parameters).Error() ==
                                          ErrorCode::InvalidArgument &&
                                      pass.SetParameters(*map, nullptr).Error() ==
                                          ErrorCode::InvalidArgument;
                          }));
    REQUIRE(graph.Execute());
    CHECK(rejected);
}

} // namespace render_graph_parameter_tests_file
