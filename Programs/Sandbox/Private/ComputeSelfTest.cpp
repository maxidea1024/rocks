#include "ComputeSelfTest.h"
#include "SandboxShaders.h"

#include <Core/Containers/Array.h>
#include <Core/Hash.h>
#include <Core/Memory/FrameArena.h>
#include <Platform/Memory.h>
#include <RenderBase/ComputeDispatch.h>
#include <RenderBase/PipelineCache.h>
#include <RenderGraph/RenderGraph.h>
#include <stdio.h>

namespace sandbox {

namespace {

using namespace rocks;
using namespace rocks::graphics;
using rocks::render::ComputeGroupCount;
using rocks::render::ComputePipelineRequest;
using rocks::render::GraphAsyncCompute;
using rocks::render::GraphBuffer;
using rocks::render::GraphBufferView;
using rocks::render::GraphComputeDispatch;
using rocks::render::GraphPassAccesses;
using rocks::render::GraphPassContext;
using rocks::render::GraphPassFlags;
using rocks::render::GraphPassKind;
using rocks::render::GraphResourcePool;
using rocks::render::GraphTexture;
using rocks::render::GraphTextureView;
using rocks::render::PipelineCache;
using rocks::render::ReleaseRetiredResource;
using rocks::render::RenderGraph;
using rocks::render::RenderGraphDesc;
using rocks::render::RenderHandle;
using rocks::render::RenderHandleTable;

TraceScopeName SequenceName("SelfTest.ComputeSequence");
TraceScopeName ArgumentsName("SelfTest.ComputeArguments");
TraceScopeName PatternName("SelfTest.ComputePattern");
TraceScopeName RejectedName("SelfTest.ComputeRejected");

// 수열 원소 수·곱·더하기, 무늬 그룹 한 변(텍스처 한 변 = 한 변 × 8)이다. 셰이더는
// GraphCompute.rsf다(SequenceCS 64 스레드, PatternCS 8 × 8).
constexpr uint32 SequenceCount = 200;
constexpr uint32 SequenceMultiplier = 3;
constexpr uint32 SequenceAddend = 7;
constexpr uint32 SequenceGroupSize = 64;
constexpr uint32 PatternGroupSide = 2;
constexpr uint32 PatternGroupSize = 8;
constexpr uint32 PatternExtent = PatternGroupSide * PatternGroupSize;
// 무늬의 행 표지 bit 위치다(셰이더 id.y << 20).
constexpr uint32 PatternRowShift = 20;
// 간접 인자 버퍼 크기와 인자 자리(ArgumentsCS가 byte 16에 쓴다), 앞 16 byte 표지다.
constexpr uint64 ArgumentBytes = 32;
constexpr uint64 ArgumentOffset = 16;
constexpr uint32 ArgumentMarkers[4] = {0xA0u, 0xA1u, 0xA2u, 0xA3u};

uint32 failures = 0;
const char* backendName = "";

void Check(bool condition, const char* what) noexcept {
    if (!condition) {
        ++failures;
        printf("  [FAIL] %s compute: %s\n", backendName, what);
    }
}

// SequenceCS·ArgumentsCS·PatternCS의 그리기별 상수(float4)다.
struct SequenceConstants {
    float values[4] = {};
};

SequenceConstants MakeConstants() noexcept {
    SequenceConstants constants;
    constants.values[0] = static_cast<float>(SequenceCount);
    constants.values[1] = static_cast<float>(SequenceMultiplier);
    constants.values[2] = static_cast<float>(SequenceAddend);
    constants.values[3] = static_cast<float>(PatternGroupSide);
    return constants;
}

uint32 ExpectedValue(uint32 index) noexcept {
    return index * index * SequenceMultiplier + SequenceAddend;
}

uint32 ExpectedPattern(uint32 x, uint32 y) noexcept {
    return ExpectedValue((x + y * PatternExtent) % SequenceCount) + (y << PatternRowShift);
}

uint64 HashWords(const Array<uint32>& words) noexcept {
    uint64 hash = 0;
    for (uint32 word : words) {
        hash = HashCombine(hash, word);
    }

    return hash;
}

// 한 번의 실행 결과다.
struct ComputeRun {
    Array<uint32> values;
    Array<uint32> arguments;
    Array<uint32> pattern;
    uint32 asyncPasses = 0;
    bool ok = false;

    explicit ComputeRun(Allocator& allocator) noexcept
        : values(allocator), arguments(allocator), pattern(allocator) {
    }
};

struct ComputeBench {
    Device* device = nullptr;
    Allocator* allocator = nullptr;
    RenderHandleTable* handles = nullptr;
    GraphResourcePool* pool = nullptr;
    FrameArena* arena = nullptr;
    PipelineCache* cache = nullptr;
    render::ShaderSet* set = nullptr;
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

    ComputePipelineRequest Request(const shaders::ShaderTypeDesc& type) const noexcept {
        ComputePipelineRequest request;
        request.set = set;
        auto entry = set->FindEntry(type.nameHash);
        request.computeEntry = entry ? entry.Value() : render::NoShaderSetEntry;
        request.state.drawConstantBytes = sizeof(SequenceConstants);
        return request;
    }
};

// 수열 파이프라인 요청이다(쓰기 slot 0 = Values).
ComputePipelineRequest SequenceRequest(const ComputeBench& bench) noexcept {
    ComputePipelineRequest request = bench.Request(shaders::GraphSequenceCS);
    request.state.bindings.writableBuffers[0] = "Values";
    return request;
}

// handle의 장치 버퍼를 words로 읽는다.
bool ReadWords(ComputeBench& bench, RenderHandle handle, usize count,
               Array<uint32>& words) noexcept {
    auto buffer = bench.handles->Buffer(handle);
    if (!buffer || !words.TryResize(count)) {
        return false;
    }

    Span<uint8> bytes(reinterpret_cast<uint8*>(words.Data()), count * sizeof(uint32));
    return bench.device->ReadBuffer(buffer.Value(), 0, bytes).IsOk();
}

void PrintGraphError(const RenderGraph& graph) noexcept {
    const StringView error = graph.LastError();
    printf("    graph error: %.*s\n", static_cast<int>(error.ByteCount()), error.Data());
}

// 수열 → 인자 → 무늬 그래프를 한 번 돈다. async면 compute pass를 AsyncCompute로 넣고 그래프가
// compute queue를 늘 쓰게 한다(장치에 queue가 없으면 graphics pipe로 낮춘다).
void RunGraph(ComputeBench& bench, bool async, ComputeRun& run) noexcept {
    RenderGraphDesc desc = bench.Desc();
    desc.asyncCompute = async ? GraphAsyncCompute::Always : GraphAsyncCompute::Disabled;
    const GraphPassKind kind = async ? GraphPassKind::AsyncCompute : GraphPassKind::Compute;
    RenderHandle valuesHandle;
    RenderHandle argumentsHandle;
    RenderHandle patternHandle;
    bool ok = false;
    {
        RenderGraph graph(desc);
        BufferDesc valuesDesc;
        valuesDesc.size = SequenceCount * sizeof(uint32);
        valuesDesc.usage =
            BufferUsage::ShaderRead | BufferUsage::ShaderWrite | BufferUsage::Structured;
        valuesDesc.stride = sizeof(uint32);
        BufferDesc argumentsDesc;
        argumentsDesc.size = ArgumentBytes;
        argumentsDesc.usage = BufferUsage::Indirect | BufferUsage::ShaderWrite | BufferUsage::Raw;
        TextureDesc patternDesc;
        patternDesc.width = PatternExtent;
        patternDesc.height = PatternExtent;
        patternDesc.format = PixelFormat::R32Uint;
        patternDesc.usage = TextureUsage::Storage;
        patternDesc.sampler.filter = Filter::Point;
        auto values = graph.CreateBuffer(valuesDesc, "ComputeValues");
        auto arguments = graph.CreateBuffer(argumentsDesc, "ComputeArguments");
        auto pattern = graph.CreateTexture(patternDesc, "ComputePattern");
        ok = values && arguments && pattern;
        BufferViewDesc structured;
        structured.kind = BufferViewKind::Structured;
        BufferViewDesc raw;
        raw.kind = BufferViewKind::Raw;
        raw.access = ViewAccess::ReadWrite;
        TextureViewDesc patternView;
        patternView.access = ViewAccess::ReadWrite;
        patternView.mipCount = 1;
        auto valuesRead = ok ? graph.CreateBufferView(values.Value(), structured)
                             : Result<GraphBufferView>::Failure(ErrorCode::InvalidArgument);
        structured.access = ViewAccess::ReadWrite;
        auto valuesWrite = ok ? graph.CreateBufferView(values.Value(), structured)
                              : Result<GraphBufferView>::Failure(ErrorCode::InvalidArgument);
        auto argumentsWrite = ok ? graph.CreateBufferView(arguments.Value(), raw)
                                 : Result<GraphBufferView>::Failure(ErrorCode::InvalidArgument);
        auto patternWrite = ok ? graph.CreateTextureView(pattern.Value(), patternView)
                               : Result<GraphTextureView>::Failure(ErrorCode::InvalidArgument);
        ok = valuesRead && valuesWrite && argumentsWrite && patternWrite;
        const SequenceConstants constants = MakeConstants();

        // 수열: 직접 dispatch(그룹 4, 마지막 그룹은 일부 스레드만 쓴다).
        if (ok) {
            const ComputePipelineRequest request = SequenceRequest(bench);
            GraphPassAccesses accesses(graph);
            accesses.BufferView(valuesWrite.Value(), ResourceAccess::UavCompute);
            GraphComputeDispatch dispatch;
            dispatch.groups = render::GroupCountFor(SequenceCount, SequenceGroupSize);
            const GraphBufferView view = valuesWrite.Value();
            ok = graph
                     .AddComputePass(
                         SequenceName, kind, GraphPassFlags::None, accesses, *bench.cache, request,
                         dispatch,
                         [view, constants](GraphPassContext& pass) noexcept {
                             CommandContext& commands = pass.Commands();
                             Check(commands.SetDrawConstants(DrawConstantBytes(constants)).IsOk(),
                                   "sequence constants are accepted");
                             commands.SetWritableBufferView(0, pass.View(view));
                         })
                     .IsOk();
        }

        // 인자: compute가 raw 버퍼에 간접 인자를 쓴다.
        if (ok) {
            ComputePipelineRequest request = bench.Request(shaders::GraphArgumentsCS);
            request.state.bindings.writableBuffers[0] = "Arguments";
            GraphPassAccesses accesses(graph);
            accesses.BufferView(argumentsWrite.Value(), ResourceAccess::UavCompute);
            GraphComputeDispatch dispatch;
            const GraphBufferView view = argumentsWrite.Value();
            ok = graph
                     .AddComputePass(
                         ArgumentsName, kind, GraphPassFlags::None, accesses, *bench.cache, request,
                         dispatch,
                         [view, constants](GraphPassContext& pass) noexcept {
                             CommandContext& commands = pass.Commands();
                             Check(commands.SetDrawConstants(DrawConstantBytes(constants)).IsOk(),
                                   "argument constants are accepted");
                             commands.SetWritableBufferView(0, pass.View(view));
                         })
                     .IsOk();
        }

        // 무늬: 앞 pass가 쓴 인자로 간접 dispatch하고 수열을 SRV view로 읽는다(인자 버퍼의
        // IndirectArgs 접근은 도우미가 더한다).
        if (ok) {
            ComputePipelineRequest request = bench.Request(shaders::GraphPatternCS);
            request.state.bindings.bufferViews[0] = "ReadValues";
            request.state.bindings.writableTextures[0] = "Pattern";
            GraphPassAccesses accesses(graph);
            accesses.BufferView(valuesRead.Value(), ResourceAccess::SrvCompute)
                .TextureView(patternWrite.Value(), ResourceAccess::UavCompute);
            GraphComputeDispatch dispatch;
            dispatch.indirectArguments = arguments.Value();
            dispatch.indirectOffset = ArgumentOffset;
            const GraphBufferView read = valuesRead.Value();
            const GraphTextureView write = patternWrite.Value();
            ok = graph
                     .AddComputePass(
                         PatternName, kind, GraphPassFlags::None, accesses, *bench.cache, request,
                         dispatch,
                         [read, write, constants](GraphPassContext& pass) noexcept {
                             CommandContext& commands = pass.Commands();
                             Check(commands.SetDrawConstants(DrawConstantBytes(constants)).IsOk(),
                                   "pattern constants are accepted");
                             commands.SetBufferView(0, pass.View(read));
                             commands.SetWritableTexture(0, pass.View(write));
                         })
                     .IsOk();
        }

        auto exportedValues = ok ? graph.ExportBuffer(values.Value(), ResourceAccess::CopySource)
                                 : Result<RenderHandle>::Failure(ErrorCode::InvalidArgument);
        auto exportedArguments =
            ok ? graph.ExportBuffer(arguments.Value(), ResourceAccess::CopySource)
               : Result<RenderHandle>::Failure(ErrorCode::InvalidArgument);
        auto exportedPattern = ok ? graph.ExportTexture(pattern.Value(), ResourceAccess::CopySource)
                                  : Result<RenderHandle>::Failure(ErrorCode::InvalidArgument);
        ok = exportedValues && exportedArguments && exportedPattern && graph.Execute().IsOk();
        if (!ok) {
            PrintGraphError(graph);
        }

        valuesHandle = exportedValues ? exportedValues.Value() : RenderHandle{};
        argumentsHandle = exportedArguments ? exportedArguments.Value() : RenderHandle{};
        patternHandle = exportedPattern ? exportedPattern.Value() : RenderHandle{};
        run.asyncPasses = graph.Stats().asyncComputePasses;
    }

    Device& device = *bench.device;
    ok = ok && device.WaitIdle().IsOk();
    ok = ok && ReadWords(bench, valuesHandle, SequenceCount, run.values) &&
         ReadWords(bench, argumentsHandle, ArgumentBytes / sizeof(uint32), run.arguments);
    auto texture = ok ? bench.handles->Texture(patternHandle)
                      : Result<TextureHandle>::Failure(ErrorCode::InvalidArgument);
    ok = texture && run.pattern.TryResize(static_cast<usize>(PatternExtent) * PatternExtent);
    ok = ok && device
                   .ReadTexture(texture.Value(), 0, 0,
                                Span<uint8>(reinterpret_cast<uint8*>(run.pattern.Data()),
                                            run.pattern.Size() * sizeof(uint32)))
                   .IsOk();
    run.ok = ok;

    // export handle을 놓는다(풀 반납, 결정 R15).
    const RenderHandle exported[] = {valuesHandle, argumentsHandle, patternHandle};
    for (RenderHandle handle : exported) {
        if (handle.IsNull()) {
            continue;
        }

        auto retired = bench.handles->BeginRetire(handle);
        if (retired) {
            ReleaseRetiredResource(device, handle, retired.Value());
        }
    }
}

uint32 CountMismatches(const ComputeRun& run) noexcept {
    uint32 mismatches = 0;
    for (uint32 index = 0; index < SequenceCount; ++index) {
        mismatches += run.values[index] != ExpectedValue(index) ? 1u : 0u;
    }

    for (uint32 y = 0; y < PatternExtent; ++y) {
        for (uint32 x = 0; x < PatternExtent; ++x) {
            const uint32 actual = run.pattern[static_cast<usize>(y) * PatternExtent + x];
            const uint32 expected = ExpectedPattern(x, y);
            if (actual != expected && mismatches < 4) {
                printf("    pattern (%u, %u) = %u, expected %u\n", x, y, actual, expected);
            }

            mismatches += actual != expected ? 1u : 0u;
        }
    }

    return mismatches;
}

// 그래프의 마지막 오류 문구에 text가 있는지다.
bool Mentions(const RenderGraph& graph, const char* text) noexcept {
    const StringView error = graph.LastError();
    const StringView expected(text);
    for (usize at = 0; at + expected.ByteCount() <= error.ByteCount(); ++at) {
        if (platform::MemoryEquals(error.Data() + at, expected.Data(), expected.ByteCount())) {
            return true;
        }
    }

    return false;
}

// 도우미의 거부: 장치 한도 밖 그룹 수와 Raster 종류다. 요청은 수열 pass와 같아 캐시가 적중하므로
// 거부는 dispatch 검사의 것이다(그래프 오류 문구로 확인한다).
void RunRejections(ComputeBench& bench) noexcept {
    const uint32 limit = bench.device->Caps().limits.computeGroupCount[0];
    {
        RenderGraph graph(bench.Desc());
        GraphPassAccesses accesses(graph);
        GraphComputeDispatch dispatch;
        dispatch.groups = ComputeGroupCount{limit + 1, 1, 1};
        auto added = graph.AddComputePass(
            RejectedName, GraphPassKind::Compute, GraphPassFlags::HasSideEffects, accesses,
            *bench.cache, SequenceRequest(bench), dispatch, [](GraphPassContext&) noexcept {});
        Check(limit != 0 && added.Error() == ErrorCode::InvalidArgument &&
                  Mentions(graph, "over the device limit") &&
                  graph.Execute().Error() == ErrorCode::InvalidArgument,
              "a dispatch over the device group limit is rejected");
    }

    {
        RenderGraph graph(bench.Desc());
        GraphPassAccesses accesses(graph);
        auto added = graph.AddComputePass(RejectedName, GraphPassKind::Raster,
                                          GraphPassFlags::HasSideEffects, accesses, *bench.cache,
                                          SequenceRequest(bench), GraphComputeDispatch{},
                                          [](GraphPassContext&) noexcept {});
        Check(added.Error() == ErrorCode::InvalidArgument && Mentions(graph, "must be a Compute") &&
                  graph.Execute().Error() == ErrorCode::InvalidArgument,
              "a raster compute pass is rejected");
    }
}

} // namespace

uint32 RunComputeChecks(const char* name, Device& device, Allocator& allocator) noexcept {
    failures = 0;
    backendName = name;
    render::ShaderSet* set = shaders::ActiveSet();
    if (!device.Caps().features.computeShaders || set == nullptr ||
        !shaders::Has(shaders::GraphPatternCS)) {
        printf("  [NOT RUN] %s compute: no compute shaders or shader set\n", name);
        return 0;
    }

    const uint64 validationBefore = device.ValidationErrorCount();
    const uint64 ignoredBefore = device.IgnoredDraws();
    RenderHandleTable handles(allocator);
    GraphResourcePool pool(allocator, device);
    FrameArena arena(allocator);
    PipelineCache cache(allocator, device);
    ComputeBench bench;
    bench.device = &device;
    bench.allocator = &allocator;
    bench.handles = &handles;
    bench.pool = &pool;
    bench.arena = &arena;
    bench.cache = &cache;
    bench.set = set;

    ComputeRun graphicsRun(allocator);
    ComputeRun asyncRun(allocator);
    RunGraph(bench, false, graphicsRun);
    Check(graphicsRun.ok, "the compute graph runs on the graphics pipe");
    Check(cache.Misses() == 3 && cache.Hits() == 0 && cache.Size() == 3,
          "the first graph creates three compute pipelines");
    auto cleared = arena.Reset();
    (void)cleared;
    RunGraph(bench, true, asyncRun);
    Check(asyncRun.ok, "the compute graph runs with async compute");
    Check(cache.Misses() == 3 && cache.Hits() == 3, "the second graph hits the pipeline cache");
    RunRejections(bench);
    Check(cache.Misses() == 3 && cache.Hits() == 5,
          "rejected compute passes reuse the cached pipeline");

    const bool ran = graphicsRun.ok && asyncRun.ok;
    Check(ran && CountMismatches(graphicsRun) == 0,
          "direct and indirect dispatch results match the CPU reference");
    bool argumentsMatch = ran && graphicsRun.arguments.Size() == ArgumentBytes / sizeof(uint32);
    for (uint32 index = 0; argumentsMatch && index < 4; ++index) {
        argumentsMatch = graphicsRun.arguments[index] == ArgumentMarkers[index];
    }

    const usize argumentWord = ArgumentOffset / sizeof(uint32);
    argumentsMatch = argumentsMatch && graphicsRun.arguments[argumentWord] == PatternGroupSide &&
                     graphicsRun.arguments[argumentWord + 1] == PatternGroupSide &&
                     graphicsRun.arguments[argumentWord + 2] == 1;
    Check(argumentsMatch, "the compute-written indirect arguments are exact");
    Check(ran &&
              platform::MemoryEquals(graphicsRun.values.Data(), asyncRun.values.Data(),
                                     graphicsRun.values.Size() * sizeof(uint32)) &&
              platform::MemoryEquals(graphicsRun.pattern.Data(), asyncRun.pattern.Data(),
                                     graphicsRun.pattern.Size() * sizeof(uint32)) &&
              platform::MemoryEquals(graphicsRun.arguments.Data(), asyncRun.arguments.Data(),
                                     graphicsRun.arguments.Size() * sizeof(uint32)),
          "graphics-pipe and async compute runs give the same bytes");
    const bool queue = device.Caps().features.asyncCompute;
    Check(graphicsRun.asyncPasses == 0 && asyncRun.asyncPasses == (queue ? 3u : 0u),
          "async compute passes run on the compute queue only when the device has one");

    cache.Clear();
    pool.Clear();
    auto idle = device.WaitIdle();
    Check(idle.IsOk(), "wait idle");
    const uint64 validationErrors = device.ValidationErrorCount() - validationBefore;
    const uint64 ignored = device.IgnoredDraws() - ignoredBefore;
    Check(validationErrors == 0, "validation errors are zero");
    Check(ignored == 0, "no dispatch is ignored");
    if (ran) {
        printf("  %s compute: values hash %016llx, pattern hash %016llx (same on every backend), "
               "async passes %u, cache misses %llu hits %llu\n",
               name, static_cast<unsigned long long>(HashWords(graphicsRun.values)),
               static_cast<unsigned long long>(HashWords(graphicsRun.pattern)),
               asyncRun.asyncPasses, static_cast<unsigned long long>(cache.Misses()),
               static_cast<unsigned long long>(cache.Hits()));
    }

    return failures;
}

} // namespace sandbox
