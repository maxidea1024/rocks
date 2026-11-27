#include "EngineShaderComputeProbes.h"
#include "SandboxShaders.h"

#include <Core/Math/Float16.h>
#include <Platform/Memory.h>
#include <RenderBase/ComputeDispatch.h>
#include <RenderBase/EngineShaderTypes.h>
#include <RenderBase/FullscreenPass.h>
#include <RenderBase/ShaderSet.h>

// 엔진 셰이더 SL1 probe 장치 연결과 CPU 기준이다(Docs/Design/EngineShaders 14.2·14.4절·14.6절 2항).
namespace sandbox::engine_shaders {

namespace {

using namespace rocks;
using namespace rocks::graphics;

// ComputeDispatchProbe의 사례 수·그룹 크기, 그룹 연산 probe의 그룹 수, 감싼 dispatch의 그룹
// 수(65,535를 넘어 두 행이 된다)와 간접 인자 offset이다.
constexpr uint32 DispatchCases = 4096;
constexpr uint32 DispatchGroupThreads = 64;
constexpr uint32 GroupOperationGroups = 64;
constexpr uint32 GroupOperationRecords = 3;
constexpr uint32 WrappedGroups = 65540;
constexpr uint32 ArgumentBytes = 32;
constexpr uint32 ArgumentOffset = 16;
constexpr uint32 NoWave = 0xffffffffu;
constexpr uint32 SignBit = 0x80000000u;
// 그룹 연산 float 입력 모양(ComputeProbes.rsf와 같다)이다.
constexpr uint32 FloatModes = 4;
constexpr uint32 NegativeZeroPeriod = 7;
constexpr uint32 NegativeZeroPhase = 3;
// 유틸리티 probe 원본 크기·배치와 LUT 한 변이다.
constexpr uint32 SampleSide = 16;
constexpr uint32 UtilityWidth = 64;
constexpr uint32 UtilityHeight = 64;
constexpr uint32 LutSide = 32;
// 정수 원본 값 범위(float로 정확 — 2^20)다.
constexpr uint32 IntegerRange = 1u << 20;
// Catmull–Rom·LUT 표본의 허용 오차(bilinear 가중 정밀도 — D3D 최소 8 bit 소수, half 원본)다.
constexpr double CatmullRomAbsolute = 1.0e-2;
constexpr double LutAbsolute = 2.0e-3;

// --- 공용 장치 보조 ---

struct StorageBuffer {
    BufferHandle buffer;
    BufferViewHandle view;
};

void Release(Device& device, StorageBuffer& storage) noexcept {
    if (!storage.view.IsNull()) {
        device.Retire(storage.view);
    }

    if (!storage.buffer.IsNull()) {
        device.Retire(storage.buffer);
    }

    storage = {};
}

// 0으로 시작하는 쓰기 버퍼와 그 쓰기 view(구조화 또는 raw)를 만든다.
bool CreateStorage(Device& device, Allocator& allocator, uint32 bytes, uint32 stride, bool indirect,
                   StorageBuffer& out) noexcept {
    Array<uint8> zeros(allocator);
    if (!zeros.TryResize(bytes)) {
        return false;
    }

    platform::MemorySet(zeros.Data(), 0, bytes);
    BufferDesc desc;
    desc.size = bytes;
    desc.usage = indirect
                     ? BufferUsage::Indirect | BufferUsage::ShaderWrite | BufferUsage::Raw
                     : BufferUsage::ShaderRead | BufferUsage::ShaderWrite | BufferUsage::Structured;
    desc.stride = indirect ? 0 : stride;
    auto buffer = device.CreateBuffer(desc, zeros.View());
    if (!buffer) {
        return false;
    }

    BufferViewDesc view;
    view.kind = indirect ? BufferViewKind::Raw : BufferViewKind::Structured;
    view.access = ViewAccess::ReadWrite;
    auto created = device.CreateBufferView(buffer.Value(), view);
    if (!created) {
        device.Retire(buffer.Value());
        return false;
    }

    out.buffer = buffer.Value();
    out.view = created.Value();
    return true;
}

// 활성 집합의 (타입, 퍼뮤테이션) compute 파이프라인이다(쓰기 버퍼 slot 0 = writable).
Result<PipelineHandle> ComputePipeline(Device& device, const shaders::ShaderTypeDesc& type,
                                       uint32 permutation, StringView writable) noexcept {
    render::ShaderSet* set = shaders::ActiveSet();
    if (set == nullptr) {
        return Result<PipelineHandle>::Failure(ErrorCode::NotReady);
    }

    auto entry = set->FindEntry(type.nameHash, shader::NoVertexInputType, permutation);
    if (!entry) {
        return Result<PipelineHandle>::Failure(entry.Error());
    }

    auto shader = set->DeviceShader(entry.Value());
    if (!shader) {
        return Result<PipelineHandle>::Failure(shader.Error());
    }

    ComputePipelineDesc desc;
    desc.computeShader = shader.Value();
    desc.drawConstantBytes = sizeof(ProbeConstants);
    desc.bindings.writableBuffers[0] = writable;
    return device.CreateComputePipeline(desc);
}

Span<const uint8> ConstantBytes(const ProbeConstants& constants) noexcept {
    return Span<const uint8>(reinterpret_cast<const uint8*>(&constants), sizeof(constants));
}

bool ReadWords(Device& device, BufferHandle buffer, Array<uint32>& words, usize count) noexcept {
    if (!words.TryResize(count)) {
        return false;
    }

    return device
        .ReadBuffer(buffer, 0, Span<uint8>(reinterpret_cast<uint8*>(words.Data()), count * 4u))
        .IsOk();
}

void PrintDeviceError(Device& device) noexcept {
    printf("    device error: %.*s\n", static_cast<int>(device.LastError().ByteCount()),
           device.LastError().Data());
}

// --- ComputeDispatch 함수(CPU 기준) ---

uint32 DivideRoundUp(uint32 value, uint32 divisor) noexcept {
    return value / divisor + (value % divisor != 0 ? 1u : 0u);
}

uint32 MortonCompact(uint32 x) noexcept {
    uint32 result = 0;
    for (uint32 bit = 0; bit < 16; ++bit) {
        result |= ((x >> (2 * bit)) & 1u) << bit;
    }

    return result;
}

constexpr uint32 SpecialThreads[8] = {0u, 1u, 63u, 64u, 65u, 4194240u, 4194241u, 0xffffffffu};

void DispatchArgumentsReference(uint32 threads, uint32 groupSize, uint32 out[3]) noexcept {
    const uint32 groups = DivideRoundUp(threads, groupSize);
    const render::ComputeGroupCount wrapped = render::WrapGroupCount(groups);
    // 셰이더는 0 그룹을 (0, 1, 1)로 낸다(WrapGroupCount와 같다).
    out[0] = wrapped.x;
    out[1] = wrapped.y;
    out[2] = wrapped.z;
}

void DispatchReference(uint32 function, uint32 selector, uint32 c, uint32 out[4]) noexcept {
    if (function == ProbeDivideRoundUp) {
        const uint32 a = c < 4 ? (c < 2 ? 0xffffffffu : 0u) : ProbeBits(c, 0, 0);
        const uint32 b = c < 4 ? ((c & 1u) != 0 ? 0xffffffffu : 1u)
                               : (ProbeBits(c, 1, 0) >> (ProbeBits(c, 2, 0) & 31u)) | 1u;
        out[0] = DivideRoundUp(a, b);
        out[1] = DivideRoundUp(a >> 1, b);
        out[2] = DivideRoundUp(a >> 3, (b >> 1) | 1u);
        out[3] = DivideRoundUp(a, b | 2u) + DivideRoundUp(b, 5u) + DivideRoundUp(a ^ b, 9u);
        return;
    }

    if (function == ProbeGroupCoordinates) {
        if (selector == 0) {
            const uint32 linear = ProbeBits(c, 0, 0);
            const uint32 perRow = ProbeBits(c, 1, 0) % 1000u + 1u;
            const uint32 tile = linear & 0xffffu;
            out[0] = linear % perRow;
            out[1] = linear / perRow;
            out[2] = tile % perRow * 8u;
            out[3] = tile / perRow * 8u;
            return;
        }

        const uint32 index = c % 4096u;
        out[0] = MortonCompact(index);
        out[1] = MortonCompact(index >> 1);
        out[2] = c % 8u + 8u * ((c / 8u) % 8u + 8u * ((c / 64u) % 16u));
        out[3] = ProbeBits(c, 0, 0) % 65535u +
                 ProbeBits(c, 1, 0) % 65535u * render::MaxWrappedGroupsPerAxis;
        return;
    }

    const uint32 threads =
        c < 8 ? SpecialThreads[c] : ProbeBits(c, 0, 0) >> (ProbeBits(c, 2, 0) % 32u);
    const uint32 groupSize = c < 8 ? 64u : ProbeBits(c, 1, 0) % 1024u + 1u;
    DispatchArgumentsReference(threads, groupSize, out);
    const uint32 group =
        ProbeBits(c, 3, 0) % 65535u + ProbeBits(c, 4, 0) % 65u * render::MaxWrappedGroupsPerAxis;
    out[3] = group * 64u + ProbeBits(c, 5, 0) % 64u;
}

void RunDispatchFunctionProbes(ProbeContext& context, Device& device) noexcept {
    struct Case {
        uint32 function;
        uint32 selector;
        const char* label;
    };

    static constexpr Case Cases[] = {
        {ProbeDivideRoundUp, 0, "ComputeDispatch.DivideAndRoundUp"},
        {ProbeGroupCoordinates, 0, "ComputeDispatch.UnflattenAndTiles"},
        {ProbeGroupCoordinates, 1, "ComputeDispatch.MortonLinearUnwrap"},
        {ProbeDispatchArguments, 0, "ComputeDispatch.DispatchArguments"}};
    auto pipeline = ComputePipeline(device, shaders::ComputeDispatchProbe, 0, "ProbeRecords");
    if (!pipeline) {
        ReportProbe(context, "ComputeDispatch", false, "probe pipeline unavailable");
        PrintDeviceError(device);
        return;
    }

    for (const Case& probe : Cases) {
        StorageBuffer records;
        Array<uint32> words(context.allocator);
        bool ok =
            CreateStorage(device, context.allocator, DispatchCases * 16u, 16u, false, records);
        auto frame = ok ? device.BeginPass(PassDesc{})
                        : Result<CommandContext*>::Failure(ErrorCode::OutOfMemory);
        if (frame) {
            ProbeConstants constants;
            constants.control.x = probe.function | (probe.selector << 16);
            constants.control.y = DispatchCases;
            CommandContext& commands = *frame.Value();
            commands.SetPipeline(pipeline.Value());
            commands.SetWritableBufferView(0, records.view);
            ok = commands.SetDrawConstants(ConstantBytes(constants)).IsOk();
            commands.Dispatch(render::GroupsFor(DispatchCases, DispatchGroupThreads));
            ok = device.EndFrame().IsOk() && ok &&
                 ReadWords(device, records.buffer, words, DispatchCases * 4u);
        } else {
            ok = false;
        }

        Release(device, records);
        if (!ok) {
            ReportProbe(context, probe.label, false, "dispatch or readback failed");
            PrintDeviceError(device);
            continue;
        }

        uint32 mismatches = 0;
        uint32 first = 0;
        for (uint32 c = 0; c < DispatchCases; ++c) {
            uint32 expected[4] = {};
            DispatchReference(probe.function, probe.selector, c, expected);
            for (uint32 k = 0; k < 4; ++k) {
                if (words[c * 4u + k] != expected[k]) {
                    first = mismatches == 0 ? c * 4u + k : first;
                    ++mismatches;
                }
            }
        }

        context.recorder.RecordUint(probe.label, words.View());
        char detail[160];
        snprintf(detail, sizeof(detail), "%u cases, %u mismatching words%s (first word %u)",
                 DispatchCases, mismatches, mismatches != 0 ? " NOT BIT EXACT" : "", first);
        ReportProbe(context, probe.label, mismatches == 0, detail);
    }

    device.Retire(pipeline.Value());
}

// 감싼 직접 dispatch와 셰이더가 쓴 간접 인자로 낸 dispatch에서 모든 선형 그룹이 정확히 한 번인지
// 본다.
void RunWrappedDispatchProbes(ProbeContext& context, Device& device) noexcept {
    auto counter = ComputePipeline(device, shaders::WrappedDispatchProbe, 0, "GroupCounts");
    auto writer =
        ComputePipeline(device, shaders::WriteDispatchArgumentsProbe, 0, "ProbeArguments");
    if (!counter || !writer) {
        ReportProbe(context, "ComputeDispatch.WrappedDispatch", false,
                    "probe pipelines unavailable");
        PrintDeviceError(device);
        if (counter) {
            device.Retire(counter.Value());
        }

        if (writer) {
            device.Retire(writer.Value());
        }

        return;
    }

    StorageBuffer direct;
    StorageBuffer indirect;
    StorageBuffer arguments;
    bool ok = CreateStorage(device, context.allocator, WrappedGroups * 4u, 4u, false, direct) &&
              CreateStorage(device, context.allocator, WrappedGroups * 4u, 4u, false, indirect) &&
              CreateStorage(device, context.allocator, ArgumentBytes, 0, true, arguments);
    Array<uint32> directCounts(context.allocator);
    Array<uint32> indirectCounts(context.allocator);
    Array<uint32> argumentWords(context.allocator);
    const render::ComputeGroupCount groups = render::WrapGroupCount(WrappedGroups);
    ok = ok && render::ValidateGroupCount(device.Caps().limits, groups).IsOk();
    auto frame = ok ? device.BeginPass(PassDesc{})
                    : Result<CommandContext*>::Failure(ErrorCode::InvalidArgument);
    if (frame) {
        ProbeConstants constants;
        constants.control.y = WrappedGroups;
        constants.control.z = ArgumentOffset;
        CommandContext& commands = *frame.Value();
        commands.SetPipeline(counter.Value());
        commands.SetWritableBufferView(0, direct.view);
        ok = commands.SetDrawConstants(ConstantBytes(constants)).IsOk();
        commands.Dispatch(groups.x, groups.y, groups.z);
        commands.SetPipeline(writer.Value());
        commands.SetWritableBufferView(0, arguments.view);
        ok = commands.SetDrawConstants(ConstantBytes(constants)).IsOk() && ok;
        commands.Dispatch(1);
        commands.SetPipeline(counter.Value());
        commands.SetWritableBufferView(0, indirect.view);
        ok = commands.SetDrawConstants(ConstantBytes(constants)).IsOk() && ok;
        commands.DispatchIndirect(arguments.buffer, ArgumentOffset);
        ok = device.EndFrame().IsOk() && ok &&
             ReadWords(device, direct.buffer, directCounts, WrappedGroups) &&
             ReadWords(device, indirect.buffer, indirectCounts, WrappedGroups) &&
             ReadWords(device, arguments.buffer, argumentWords, ArgumentBytes / 4u);
    } else {
        ok = false;
    }

    Release(device, direct);
    Release(device, indirect);
    Release(device, arguments);
    device.Retire(counter.Value());
    device.Retire(writer.Value());
    if (!ok) {
        ReportProbe(context, "ComputeDispatch.WrappedDispatch", false,
                    "dispatch or readback failed");
        PrintDeviceError(device);
        return;
    }

    uint32 directWrong = 0;
    uint32 indirectWrong = 0;
    for (uint32 group = 0; group < WrappedGroups; ++group) {
        directWrong += directCounts[group] != 1u ? 1u : 0u;
        indirectWrong += indirectCounts[group] != 1u ? 1u : 0u;
    }

    const uint32* written = argumentWords.Data() + ArgumentOffset / 4u;
    const bool argumentsMatch =
        written[0] == groups.x && written[1] == groups.y && written[2] == groups.z;
    char detail[192];
    snprintf(detail, sizeof(detail),
             "%u groups as (%u, %u, %u): direct %u wrong counts, shader-written arguments (%u, %u, "
             "%u)%s, indirect %u wrong counts",
             WrappedGroups, groups.x, groups.y, groups.z, directWrong, written[0], written[1],
             written[2], argumentsMatch ? "" : " DIFFER", indirectWrong);
    ReportProbe(context, "ComputeDispatch.WrappedDispatch",
                directWrong == 0 && indirectWrong == 0 && argumentsMatch, detail);
}

// --- WaveOperations(CPU 기준과 두 경로 대조) ---

uint32 OrderedBits(float value) noexcept {
    const uint32 bits = FloatToBits(value);
    return (bits & SignBit) != 0 ? ~bits : (bits | SignBit);
}

float GroupFloatInput(uint32 group, uint32 c) noexcept {
    const uint32 mode = group % FloatModes;
    float f = ProbeSigned(c, 1, 4, 0);
    f = mode == 1 ? fabsf(f) : f;
    f = mode == 2 ? 0.0f : f;
    return mode != 0 && c % NegativeZeroPeriod == NegativeZeroPhase ? BitsToFloat(SignBit) : f;
}

// 한 경로(퍼뮤테이션)의 결과다.
struct GroupOperationRun {
    explicit GroupOperationRun(Allocator& allocator) noexcept : words(allocator) {
    }

    Array<uint32> words;
    bool ok = false;
};

void RunGroupOperationPath(ProbeContext& context, Device& device, uint32 sizeIndex,
                           bool forceGroupshared, GroupOperationRun& run) noexcept {
    const uint32 threads = shaders::GroupOperationProbeSizes[sizeIndex];
    const uint32 cases = threads * GroupOperationGroups;
    auto pipeline = ComputePipeline(
        device, shaders::GroupOperationsProbe,
        shaders::GroupOperationProbePermutation(sizeIndex, forceGroupshared), "ProbeRecords");
    if (!pipeline) {
        PrintDeviceError(device);
        return;
    }

    StorageBuffer records;
    bool ok = CreateStorage(device, context.allocator, cases * GroupOperationRecords * 16u, 16u,
                            false, records);
    auto frame = ok ? device.BeginPass(PassDesc{})
                    : Result<CommandContext*>::Failure(ErrorCode::OutOfMemory);
    if (frame) {
        // −0 입력의 bit다(셰이더 리터럴은 FXC가 +0으로 접는다).
        ProbeConstants constants;
        constants.control.z = SignBit;
        CommandContext& commands = *frame.Value();
        commands.SetPipeline(pipeline.Value());
        commands.SetWritableBufferView(0, records.view);
        ok = commands.SetDrawConstants(ConstantBytes(constants)).IsOk();
        commands.Dispatch(GroupOperationGroups);
        ok = device.EndFrame().IsOk() && ok &&
             ReadWords(device, records.buffer, run.words, cases * GroupOperationRecords * 4u);
    } else {
        ok = false;
    }

    Release(device, records);
    device.Retire(pipeline.Value());
    run.ok = ok;
    if (!ok) {
        PrintDeviceError(device);
    }
}

// CPU 기준과 대조한다. 정수 결과(float 최소·최대·broadcast 포함)는 bit 일치, float 합은 순서 무관
// 상한 (N − 1)·2^-24·Σ|f| 안이다. 웨이브 배치 정보가 있으면 연속 구간 가정을 확인한다.
bool CheckGroupOperationRun(ProbeContext& context, uint32 sizeIndex, const char* label,
                            const GroupOperationRun& run, uint32& laneCount) noexcept {
    const uint32 threads = shaders::GroupOperationProbeSizes[sizeIndex];
    uint32 integerMismatches = 0;
    // 함수(word)별 틀린 수다(합·최소·최대·prefix·float 최소·최대·broadcast).
    uint32 wordMismatches[7] = {};
    uint32 sumFailures = 0;
    uint32 layoutFailures = 0;
    double worstSumRatio = 0.0;
    laneCount = 0;
    for (uint32 group = 0; group < GroupOperationGroups; ++group) {
        const uint32 base = group * threads;
        uint32 sum = 0;
        uint32 minimum = 0xffffffffu;
        uint32 maximum = 0;
        double floatSum = 0.0;
        double floatMagnitude = 0.0;
        uint32 floatMinimum = 0xffffffffu;
        uint32 floatMaximum = 0;
        for (uint32 i = 0; i < threads; ++i) {
            const uint32 value = ProbeBits(base + i, 0, 0);
            const float f = GroupFloatInput(group, base + i);
            sum += value;
            minimum = value < minimum ? value : minimum;
            maximum = value > maximum ? value : maximum;
            floatSum += f;
            floatMagnitude += fabs(static_cast<double>(f));
            floatMinimum = OrderedBits(f) < floatMinimum ? OrderedBits(f) : floatMinimum;
            floatMaximum = OrderedBits(f) > floatMaximum ? OrderedBits(f) : floatMaximum;
        }

        const uint32 firstValue = ProbeBits(base, 0, 0);
        const float firstFloat = GroupFloatInput(group, base);
        uint32 prefix = 0;
        for (uint32 i = 0; i < threads; ++i) {
            const uint32 c = base + i;
            const uint32* record = run.words.Data() + static_cast<usize>(c) * 12u;
            const uint32 expected[7] = {
                sum,
                minimum,
                maximum,
                prefix,
                (floatMinimum & SignBit) != 0 ? floatMinimum & ~SignBit : ~floatMinimum,
                (floatMaximum & SignBit) != 0 ? floatMaximum & ~SignBit : ~floatMaximum,
                firstValue};
            const uint32 actual[7] = {record[0], record[1], record[2], record[3],
                                      record[5], record[6], record[7]};
            for (uint32 k = 0; k < 7; ++k) {
                if (actual[k] != expected[k] && integerMismatches == 0) {
                    printf("    first integer mismatch: group %u thread %u word %u gpu 0x%08x ref "
                           "0x%08x\n",
                           group, i, k, actual[k], expected[k]);
                }

                integerMismatches += actual[k] != expected[k] ? 1u : 0u;
                wordMismatches[k] += actual[k] != expected[k] ? 1u : 0u;
            }

            integerMismatches += record[8] != FloatToBits(firstFloat) ? 1u : 0u;
            const double bound = (threads - 1.0) * ldexp(1.0, -24) * floatMagnitude + 1.0e-30;
            const double error = fabs(static_cast<double>(BitsToFloat(record[4])) - floatSum);
            worstSumRatio = fmax(worstSumRatio, error / bound);
            sumFailures += error > bound ? 1u : 0u;
            // 웨이브 배치: groupIndex = 웨이브 첫 lane의 groupIndex + lane, 웨이브 첫 lane은 웨이브
            // 크기 배수.
            if (record[9] != NoWave) {
                const uint32 lane = record[9] & 0xffffu;
                const uint32 count = record[9] >> 16;
                laneCount = count;
                const bool contiguous = count != 0 && record[10] + lane == record[11] &&
                                        record[10] % count == 0 && record[11] == i;
                layoutFailures += contiguous ? 0u : 1u;
            }

            prefix += ProbeBits(c, 0, 0);
        }
    }

    if (integerMismatches != 0) {
        printf("    mismatches by function: sum %u, min %u, max %u, prefix %u, float min %u, float "
               "max %u, broadcast %u\n",
               wordMismatches[0], wordMismatches[1], wordMismatches[2], wordMismatches[3],
               wordMismatches[4], wordMismatches[5], wordMismatches[6]);
    }

    char detail[224];
    snprintf(detail, sizeof(detail),
             "%u groups of %u: integer mismatches %u, float sum max %.3f of the order bound%s, "
             "wave lanes %u%s",
             GroupOperationGroups, threads, integerMismatches, worstSumRatio,
             sumFailures != 0 ? " OUT OF BOUND" : "", laneCount,
             layoutFailures != 0 ? " WAVES ARE NOT CONTIGUOUS GROUP INDEX RANGES" : "");
    const bool passed = integerMismatches == 0 && sumFailures == 0 && layoutFailures == 0;
    ReportProbe(context, label, passed, detail);
    return passed;
}

// 정수 결과(float 합·웨이브 배치를 뺀 워드)만 모은다(장치 사이 bit 대조용).
void CollectIntegerWords(const GroupOperationRun& run, Array<uint32>& out) noexcept {
    const usize records = run.words.Size() / 12u;
    if (!out.TryResize(records * 8u)) {
        return;
    }

    for (usize c = 0; c < records; ++c) {
        const uint32* record = run.words.Data() + c * 12u;
        const uint32 kept[8] = {record[0], record[1], record[2], record[3],
                                record[5], record[6], record[7], record[8]};
        platform::MemoryCopy(out.Data() + c * 8u, kept, sizeof(kept));
    }
}

void RunGroupOperationProbes(ProbeContext& context, Device& device) noexcept {
    static const char* const labels[shaders::GroupOperationProbeSizeCount] = {
        "WaveOperations.Group64", "WaveOperations.Group256", "WaveOperations.Group100"};
    static const char* const forcedLabels[shaders::GroupOperationProbeSizeCount] = {
        "WaveOperations.Group64.Groupshared", "WaveOperations.Group256.Groupshared",
        "WaveOperations.Group100.Groupshared"};
    for (uint32 size = 0; size < shaders::GroupOperationProbeSizeCount; ++size) {
        GroupOperationRun run(context.allocator);
        RunGroupOperationPath(context, device, size, false, run);
        if (!run.ok) {
            ReportProbe(context, labels[size], false, "dispatch or readback failed");
            continue;
        }

        uint32 lanes = 0;
        CheckGroupOperationRun(context, size, labels[size], run, lanes);
        Array<uint32> integers(context.allocator);
        CollectIntegerWords(run, integers);
        context.recorder.RecordUint(labels[size], integers.View());
        if (run.words.Size() >= 12u && run.words[9] == NoWave) {
            // 웨이브 경로가 없는 형식은 기본 퍼뮤테이션이 이미 groupshared 경로다.
            continue;
        }

        // 웨이브 경로 형식: 강제 groupshared 경로도 돌려 두 경로가 같은지 본다(결정 SLD5).
        GroupOperationRun forced(context.allocator);
        RunGroupOperationPath(context, device, size, true, forced);
        if (!forced.ok) {
            ReportProbe(context, forcedLabels[size], false, "dispatch or readback failed");
            continue;
        }

        uint32 forcedLanes = 0;
        CheckGroupOperationRun(context, size, forcedLabels[size], forced, forcedLanes);
        Array<uint32> forcedIntegers(context.allocator);
        CollectIntegerWords(forced, forcedIntegers);
        uint32 differing = 0;
        for (usize index = 0; index < integers.Size() && index < forcedIntegers.Size(); ++index) {
            differing += integers[index] != forcedIntegers[index] ? 1u : 0u;
        }

        double sumDifference = 0.0;
        for (usize c = 0; c * 12u + 4u < run.words.Size(); ++c) {
            sumDifference =
                fmax(sumDifference, fabs(static_cast<double>(BitsToFloat(run.words[c * 12u + 4u])) -
                                         BitsToFloat(forced.words[c * 12u + 4u])));
        }

        char detail[160];
        snprintf(detail, sizeof(detail),
                 "wave (%u lanes) vs groupshared: %u differing integer words, float sum max "
                 "difference %.3g",
                 lanes, differing, sumDifference);
        ReportProbe(context, "WaveOperations.PathsAgree",
                    differing == 0 && integers.Size() == forcedIntegers.Size(), detail);
    }
}

// --- 유틸리티 PS probe(텍스처) ---

// 유틸리티 원본 텍스처와 그 CPU 값이다.
struct UtilitySources {
    explicit UtilitySources(Allocator& allocator) noexcept
        : sample(allocator), uints(allocator), ints(allocator) {
    }

    TextureHandle sampleTexture;
    TextureHandle uintTexture;
    TextureHandle intTexture;
    TextureHandle unwrappedLut;
    TextureHandle volumeLut;
    // SampleSource의 half 복호 값(texel마다 넷), UintSource 값(넷), IntSource 값(하나)이다.
    Array<float> sample;
    Array<uint32> uints;
    Array<int32> ints;
};

SamplerDesc LinearClamp() noexcept {
    SamplerDesc desc;
    desc.filter = Filter::Bilinear;
    desc.addressU = AddressMode::Clamp;
    desc.addressV = AddressMode::Clamp;
    desc.addressW = AddressMode::Clamp;
    return desc;
}

bool CreateUtilitySources(Device& device, Allocator& allocator, UtilitySources& sources) noexcept {
    constexpr uint32 texels = SampleSide * SampleSide;
    Array<uint16> halves(allocator);
    if (!sources.sample.TryResize(texels * 4u) || !halves.TryResize(texels * 4u) ||
        !sources.uints.TryResize(texels * 4u) || !sources.ints.TryResize(texels)) {
        return false;
    }

    for (uint32 index = 0; index < texels * 4u; ++index) {
        const Float16 half = Float16::FromFloat(ProbeUnit(index, 40, 0));
        halves[index] = half.bits;
        sources.sample[index] = half.ToFloat();
        sources.uints[index] = ProbeBits(index, 41, 0) % IntegerRange;
    }

    for (uint32 index = 0; index < texels; ++index) {
        sources.ints[index] = static_cast<int32>(ProbeBits(index, 42, 0) % (2u * IntegerRange)) -
                              static_cast<int32>(IntegerRange);
    }

    TextureDesc desc;
    desc.width = SampleSide;
    desc.height = SampleSide;
    desc.format = PixelFormat::RGBA16Float;
    desc.sampler = LinearClamp();
    auto sample = device.CreateTexture(
        desc, Span<const uint8>(reinterpret_cast<const uint8*>(halves.Data()), texels * 8u));
    desc.format = PixelFormat::RGBA32Uint;
    desc.sampler.filter = Filter::Point;
    auto uints = device.CreateTexture(
        desc,
        Span<const uint8>(reinterpret_cast<const uint8*>(sources.uints.Data()), texels * 16u));
    desc.format = PixelFormat::R32Sint;
    auto ints = device.CreateTexture(
        desc, Span<const uint8>(reinterpret_cast<const uint8*>(sources.ints.Data()), texels * 4u));
    // 항등 LUT: 2D 펼침(x = b·32 + r, y = g)과 32³ 볼륨, half(r/31, g/31, b/31, 1).
    Array<uint16> lut(allocator);
    if (!lut.TryResize(static_cast<usize>(LutSide) * LutSide * LutSide * 4u)) {
        return false;
    }

    const uint16 one = Float16::FromFloat(1.0f).bits;
    for (uint32 b = 0; b < LutSide; ++b) {
        for (uint32 g = 0; g < LutSide; ++g) {
            for (uint32 r = 0; r < LutSide; ++r) {
                // 볼륨 배치(z = b, y = g, x = r)와 2D 펼친 배치(y = g, x = b·32 + r)의 texel
                // 번호다.
                const usize volume = (static_cast<usize>(b) * LutSide + g) * LutSide + r;
                uint16* texel = lut.Data() + volume * 4u;
                texel[0] = Float16::FromFloat(r / (LutSide - 1.0f)).bits;
                texel[1] = Float16::FromFloat(g / (LutSide - 1.0f)).bits;
                texel[2] = Float16::FromFloat(b / (LutSide - 1.0f)).bits;
                texel[3] = one;
            }
        }
    }

    Array<uint16> unwrapped(allocator);
    if (!unwrapped.TryResize(lut.Size())) {
        return false;
    }

    for (uint32 b = 0; b < LutSide; ++b) {
        for (uint32 g = 0; g < LutSide; ++g) {
            for (uint32 r = 0; r < LutSide; ++r) {
                const usize volume = (static_cast<usize>(b) * LutSide + g) * LutSide + r;
                const usize flat = static_cast<usize>(g) * LutSide * LutSide + b * LutSide + r;
                platform::MemoryCopy(unwrapped.Data() + flat * 4u, lut.Data() + volume * 4u, 8u);
            }
        }
    }

    TextureDesc flat;
    flat.width = LutSide * LutSide;
    flat.height = LutSide;
    flat.format = PixelFormat::RGBA16Float;
    flat.sampler = LinearClamp();
    auto unwrappedLut = device.CreateTexture(
        flat,
        Span<const uint8>(reinterpret_cast<const uint8*>(unwrapped.Data()), unwrapped.Size() * 2u));
    TextureDesc volumeDesc = flat;
    volumeDesc.dimension = TextureDimension::Texture3D;
    volumeDesc.width = LutSide;
    volumeDesc.height = LutSide;
    volumeDesc.depth = LutSide;
    auto volumeLut = device.CreateTexture(
        volumeDesc, Span<const uint8>(reinterpret_cast<const uint8*>(lut.Data()), lut.Size() * 2u));
    sources.sampleTexture = sample ? sample.Value() : TextureHandle{};
    sources.uintTexture = uints ? uints.Value() : TextureHandle{};
    sources.intTexture = ints ? ints.Value() : TextureHandle{};
    sources.unwrappedLut = unwrappedLut ? unwrappedLut.Value() : TextureHandle{};
    sources.volumeLut = volumeLut ? volumeLut.Value() : TextureHandle{};
    return sample && uints && ints && unwrappedLut && volumeLut;
}

void ReleaseUtilitySources(Device& device, UtilitySources& sources) noexcept {
    const TextureHandle textures[5] = {sources.sampleTexture, sources.uintTexture,
                                       sources.intTexture, sources.unwrappedLut, sources.volumeLut};
    for (const TextureHandle texture : textures) {
        if (!texture.IsNull()) {
            device.Retire(texture);
        }
    }
}

// 유틸리티 probe 실행기다(사례 하나가 RGBA32Float 픽셀 하나 — 배치가 작아 한 번에 그린다).
class UtilityProbeRunner final : public ProbeDevice {
public:
    UtilityProbeRunner(Device& device, const UtilitySources& sources,
                       PipelineHandle pipeline) noexcept
        : device_(device), sources_(sources), pipeline_(pipeline) {
    }

    bool RunFloat(const ProbeRequest& request, Array<float>& out) noexcept override {
        const uint32 cases = CaseCount(request);
        if (!out.TryResize(static_cast<usize>(cases) * 4u)) {
            return false;
        }

        TextureDesc desc;
        desc.width = request.width;
        desc.height = request.height;
        desc.format = PixelFormat::RGBA32Float;
        desc.usage = TextureUsage::RenderTarget;
        auto target = device_.CreateTexture(desc, {});
        if (!target) {
            return false;
        }

        ProbeConstants constants;
        constants.control.x = request.function | (request.selector << 16);
        constants.control.y = request.width;
        constants.parameters = {request.parameters[0], request.parameters[1], 0.0f, 0.0f};
        auto frame = device_.BeginOffscreen(target.Value(), {});
        bool ok = frame.IsOk();
        if (frame) {
            CommandContext& commands = *frame.Value();
            commands.SetPipeline(pipeline_);
            const struct {
                const char* name;
                TextureHandle texture;
            } bindings[] = {{"SampleSource", sources_.sampleTexture},
                            {"UintSource", sources_.uintTexture},
                            {"IntSource", sources_.intTexture},
                            {"UnwrappedLut", sources_.unwrappedLut},
                            {"VolumeLut", sources_.volumeLut}};
            for (const auto& binding : bindings) {
                commands.SetTexture(ShaderStage::Pixel,
                                    shaders::TextureSlot(shaders::UtilityProbe, binding.name),
                                    binding.texture);
            }

            ok = commands.SetDrawConstants(ConstantBytes(constants)).IsOk();
            render::DrawFullscreenTriangle(commands);
            ok = device_.EndFrame().IsOk() && ok &&
                 device_
                     .ReadTexture(target.Value(), 0, 0,
                                  Span<uint8>(reinterpret_cast<uint8*>(out.Data()),
                                              static_cast<usize>(cases) * 16u))
                     .IsOk();
        }

        device_.Retire(target.Value());
        return ok;
    }

    bool RunUint(const ProbeRequest&, Array<uint32>&) noexcept override {
        return false;
    }

private:
    Device& device_;
    const UtilitySources& sources_;
    PipelineHandle pipeline_;
};

// Catmull–Rom(장력 ½) 가중이다.
void CatmullRom(double f, double w[4]) noexcept {
    const double f2 = f * f;
    const double f3 = f2 * f;
    w[0] = -0.5 * f + f2 - 0.5 * f3;
    w[1] = 1.0 - 2.5 * f2 + 1.5 * f3;
    w[2] = 0.5 * f + 2.0 * f2 - 1.5 * f3;
    w[3] = -0.5 * f2 + 0.5 * f3;
}

int32 ClampTexel(int32 x) noexcept {
    return x < 0 ? 0 : (x >= static_cast<int32>(SampleSide) ? SampleSide - 1 : x);
}

double UtilityUv(uint32 c, uint32 axis) noexcept {
    return static_cast<double>(ProbeUnit(c, axis, 0)) * 2.0 - 0.5;
}

void RunUtilityPixelProbes(ProbeContext& context, const UtilitySources& sources) noexcept {
    ProbeRequest catmull = MakeProbeRequest(ProbeCatmullRom, 0, UtilityWidth, UtilityHeight);
    catmull.parameters[0] = SampleSide;
    catmull.parameters[1] = SampleSide;
    CheckFloatProbe(context, "TextureSamplingHelpers.CatmullRom5Tap", catmull,
                    UniformTolerance(CatmullRomAbsolute, 0.0),
                    [&sources](uint32 c, const float*, ProbeExpectation& e) {
                        const double px = UtilityUv(c, 0) * SampleSide - 0.5;
                        const double py = UtilityUv(c, 1) * SampleSide - 0.5;
                        const double bx = floor(px);
                        const double by = floor(py);
                        double wx[4] = {};
                        double wy[4] = {};
                        CatmullRom(px - bx, wx);
                        CatmullRom(py - by, wy);
                        double sum[4] = {};
                        double total = 0.0;
                        for (int32 j = 0; j < 4; ++j) {
                            for (int32 i = 0; i < 4; ++i) {
                                const bool corner = (i == 0 || i == 3) && (j == 0 || j == 3);
                                if (corner) {
                                    continue;
                                }

                                const double weight = wx[i] * wy[j];
                                const int32 x = ClampTexel(static_cast<int32>(bx) + i - 1);
                                const int32 y = ClampTexel(static_cast<int32>(by) + j - 1);
                                const float* texel =
                                    sources.sample.Data() + (y * SampleSide + x) * 4;
                                for (uint32 k = 0; k < 4; ++k) {
                                    sum[k] += weight * texel[k];
                                }

                                total += weight;
                            }
                        }

                        for (uint32 k = 0; k < 4; ++k) {
                            e.value[k] = sum[k] / total;
                        }
                    });

    for (uint32 kind = 0; kind < 2; ++kind) {
        ProbeRequest request = MakeProbeRequest(kind == 0 ? ProbeBilinearUint : ProbeBilinearInt, 0,
                                                UtilityWidth, UtilityHeight);
        request.parameters[0] = SampleSide;
        request.parameters[1] = SampleSide;
        CheckFloatProbe(
            context,
            kind == 0 ? "TextureSamplingHelpers.LoadBilinearUint"
                      : "TextureSamplingHelpers.LoadBilinearInt",
            request, UniformTolerance(Arithmetic(8) * 2.0 * IntegerRange, Arithmetic(8)),
            [&sources, kind](uint32 c, const float*, ProbeExpectation& e) {
                const double px = UtilityUv(c, 0) * SampleSide - 0.5;
                const double py = UtilityUv(c, 1) * SampleSide - 0.5;
                const double bx = floor(px);
                const double by = floor(py);
                const double fx = px - bx;
                const double fy = py - by;
                const int32 x0 = ClampTexel(static_cast<int32>(bx));
                const int32 x1 = ClampTexel(static_cast<int32>(bx) + 1);
                const int32 y0 = ClampTexel(static_cast<int32>(by));
                const int32 y1 = ClampTexel(static_cast<int32>(by) + 1);
                for (uint32 k = 0; k < 4; ++k) {
                    const auto texel = [&](int32 x, int32 y) noexcept -> double {
                        if (kind == 0) {
                            return sources.uints[(y * SampleSide + x) * 4 + k];
                        }

                        // R32Sint은 (v, 0, 0, 1)로 읽힌다.
                        return k == 0 ? sources.ints[y * SampleSide + x] : (k == 3 ? 1.0 : 0.0);
                    };
                    const double top = texel(x0, y0) + (texel(x1, y0) - texel(x0, y0)) * fx;
                    const double bottom = texel(x0, y1) + (texel(x1, y1) - texel(x0, y1)) * fx;
                    e.value[k] = top + (bottom - top) * fy;
                }
            });
    }

    ProbeRequest center = MakeProbeRequest(ProbeTexelCenter, 0, UtilityWidth, UtilityHeight);
    center.parameters[0] = SampleSide;
    center.parameters[1] = SampleSide;
    CheckFloatProbe(context, "TextureSamplingHelpers.SnapUvToTexelCenter", center,
                    UniformTolerance(0.0, 0.0), [](uint32 c, const float*, ProbeExpectation& e) {
                        for (uint32 axis = 0; axis < 2; ++axis) {
                            const double uv = UtilityUv(c, axis);
                            e.value[axis] = (floor(uv * SampleSide) + 0.5) / SampleSide;
                            e.value[axis + 2] = uv;
                        }
                    });

    static const char* const lutLabels[2] = {"LutCoordinates.SampleUnwrappedLut2D",
                                             "LutCoordinates.SampleLut3D"};
    for (uint32 kind = 0; kind < 2; ++kind) {
        CheckFloatProbe(context, lutLabels[kind],
                        MakeProbeRequest(kind == 0 ? ProbeUnwrappedLut : ProbeVolumeLut, 0,
                                         UtilityWidth, UtilityHeight),
                        UniformTolerance(LutAbsolute, 0.0),
                        [](uint32 c, const float*, ProbeExpectation& e) {
                            for (uint32 channel = 0; channel < 3; ++channel) {
                                e.value[channel] = ProbeUnit(c, channel, 0);
                            }

                            e.value[3] = 1.0;
                        });
    }
}

} // namespace

void RunComputeProbes(ProbeContext& context, Device& device) noexcept {
    RunDispatchFunctionProbes(context, device);
    RunWrappedDispatchProbes(context, device);
    RunGroupOperationProbes(context, device);
}

void RunUtilityProbes(ProbeContext& context, Device& device) noexcept {
    render::ShaderSet* set = shaders::ActiveSet();
    auto vertexEntry = set != nullptr ? set->FindEntry(render::FullscreenTriangleVertexType.hash)
                                      : Result<uint32>::Failure(ErrorCode::NotReady);
    auto vertex = vertexEntry ? set->DeviceShader(vertexEntry.Value())
                              : Result<ShaderHandle>::Failure(vertexEntry.Error());
    auto pixel = shaders::Get(shaders::UtilityProbe);
    UtilitySources sources(context.allocator);
    const bool created = CreateUtilitySources(device, context.allocator, sources);
    Result<PipelineHandle> pipeline = Result<PipelineHandle>::Failure(ErrorCode::NotReady);
    if (vertex && pixel && created) {
        PipelineDesc desc;
        desc.vertexShader = vertex.Value();
        desc.pixelShader = pixel.Value();
        desc.colorFormats[0] = PixelFormat::RGBA32Float;
        desc.drawConstantBytes = sizeof(ProbeConstants);
        pipeline = device.CreatePipeline(desc);
    }

    if (!pipeline) {
        ReportProbe(context, "TextureSamplingHelpers", false,
                    "utility probe pipeline or source textures unavailable");
        PrintDeviceError(device);
        ReleaseUtilitySources(device, sources);
        return;
    }

    UtilityProbeRunner runner(device, sources, pipeline.Value());
    ProbeContext utility{runner, context.recorder, context.allocator, context.deviceName};
    RunUtilityPixelProbes(utility, sources);
    context.failures += utility.failures;
    context.probes += utility.probes;
    device.Retire(pipeline.Value());
    ReleaseUtilitySources(device, sources);
}

} // namespace sandbox::engine_shaders
