#include "RenderGraphInternal.h"

#include "GraphCommandList.h"

#include <Task/ParallelFor.h>
#include <TraceCapture/TraceMark.h>

// 그래프 실행이다: 그래프 시작 업로드, 첫 사용 pass 직전 할당(같은 그래프 안 재사용 자리 또는 풀),
// Raster pass마다 장치 기록 열기, prologue 즉시 전이, pass 기록 함수와 CPU·GPU 구간, async compute
// 기록과 fork·join 대기, 그래프 끝 마무리(Docs/Design/RenderBase.ko.md 7.1절 실행·업로드 행, 8절
// RG2·RG4, UE RC/Private/RenderGraphBuilder.cpp:2098–2167, 2691–2837, 3424–3528).
namespace rocks::render::detail {

using graphics::ResourceAccess;

Result<void> AllocateResource(FrameArena& arena, GraphState& state, uint32 index) noexcept {
    ResourceRecord& resource = state.resources[index];
    if (resource.allocated) {
        return Result<void>::Success();
    }

    // Immediate 모드(계획 없음)는 자원마다 새 자리다.
    if (resource.physical == GraphIndexNone) {
        PhysicalResource physical;
        physical.owner = index;
        physical.held = true;
        auto pushed = state.physicals.TryPushBack(arena, physical);
        if (!pushed) {
            return Result<void>::Failure(
                RecordGraphError(state, pushed.Error(), false, "'{}' could not be allocated ({})",
                                 resource.name, ErrorCodeName(pushed.Error())));
        }

        resource.physical = static_cast<uint32>(state.physicals.Size() - 1);
    }

    PhysicalResource& physical = state.physicals[resource.physical];
    if (physical.acquired) {
        // 같은 그래프의 앞 자원이 놓은 객체를 이어 쓴다(풀에 가지 않는다).
        resource.device = physical.device;
        resource.poolEntry = physical.entry;
        resource.allocated = true;
        ++state.stats.reusedWithinGraph;
        return Result<void>::Success();
    }

    Span<const uint8> pixels;
    if (resource.kind == GraphResourceKind::Texture &&
        resource.textureDesc.usage == graphics::TextureUsage::Sampled) {
        // Immediate 모드는 컴파일 검사가 없으므로 여기서 mip 사슬이 다 올라왔는지 본다.
        for (uint32 mip = 0; mip < resource.mipLevels; ++mip) {
            if (!state.tracks[resource.firstTrack + mip].uploaded) {
                return Result<void>::Failure(RecordGraphError(
                    state, ErrorCode::InvalidArgument, true,
                    "sampled graph texture '{}' has no upload for mip {}", resource.name, mip));
            }
        }

        pixels =
            Span<const uint8>(resource.uploadPixels,
                              static_cast<usize>(graphics::TextureDataBytes(resource.textureDesc)));
    }

    auto acquired = resource.kind == GraphResourceKind::Texture
                        ? state.pool->AcquireTexture(resource.textureDesc, pixels)
                        : state.pool->AcquireBuffer(resource.bufferDesc);
    if (!acquired) {
        return Result<void>::Failure(
            RecordGraphError(state, acquired.Error(), false, "'{}' could not be allocated ({})",
                             resource.name, ErrorCodeName(acquired.Error())));
    }

    physical.acquired = true;
    physical.entry = acquired.Value().entry;
    physical.device = acquired.Value().device;
    resource.device = physical.device;
    resource.poolEntry = physical.entry;
    resource.allocated = true;
    if (acquired.Value().reused) {
        ++state.stats.reusedResources;
    } else {
        ++state.stats.createdResources;
    }

    // 텍스처 업로드는 풀이 생성 초기 데이터나 mip별 갱신으로 이미 올렸다.
    if (!pixels.IsEmpty()) {
        state.stats.uploads += resource.mipLevels;
    }

    // 버퍼 업로드를 선언 순서대로 올린다(장치 업로드 경로: 다음 제출 앞에서 실행된다).
    if (resource.kind == GraphResourceKind::Buffer && resource.uploadTarget) {
        for (const BufferUpload& upload : state.bufferUploads) {
            if (upload.resource != index) {
                continue;
            }

            auto updated =
                state.device->UpdateBuffer(graphics::BufferHandle{resource.device}, upload.offset,
                                           Span<const uint8>(upload.data, upload.size));
            if (!updated) {
                return Result<void>::Failure(
                    RecordGraphError(state, updated.Error(), false, "upload into '{}' failed ({})",
                                     resource.name, ErrorCodeName(updated.Error())));
            }

            ++state.stats.uploads;
        }
    }

    return Result<void>::Success();
}

Result<void> ExecuteUploads(FrameArena& arena, GraphState& state) noexcept {
    for (uint32 index = 0; index < state.resources.Size(); ++index) {
        const ResourceRecord& resource = state.resources[index];
        // 남은 pass가 쓰지 않고 export도 아니면 올리지 않는다(할당도 하지 않는다).
        const bool live = resource.references != 0 || resource.exported;
        if (resource.imported || !resource.uploadTarget || !live) {
            continue;
        }

        auto allocated = AllocateResource(arena, state, index);
        if (!allocated) {
            return allocated;
        }
    }

    return Result<void>::Success();
}

namespace {

// 열린 장치 기록을 제출한다.
// 그래프 구간 marker를 이 기록에 다시 push한다(기록을 열 때, marker는 기록 안에서 짝이 맞아야
// 한다).
void RestoreScopeMarkers(GraphState& state) noexcept {
    state.scopeMarkers = 0;
    if (state.trace == nullptr || !state.trace->MarkersEnabled() || state.commands == nullptr) {
        return;
    }

    for (const OpenScopeEntry& entry : state.openScopes) {
        state.commands->PushMarker(StringView(state.scopes[entry.scope].name->Name()));
        ++state.scopeMarkers;
    }
}

Result<void> CloseRecording(GraphState& state) noexcept {
    if (state.commands == nullptr) {
        return Result<void>::Success();
    }

    // 열린 구간의 marker는 이 기록 안에서 닫는다(다음 기록이 다시 push한다).
    for (; state.scopeMarkers > 0; --state.scopeMarkers) {
        state.commands->PopMarker();
    }

    const bool compute = state.recordingQueue == graphics::QueueKind::AsyncCompute;
    state.commands = nullptr;
    state.recordingPass = GraphIndexNone;
    state.recordingQueue = graphics::QueueKind::Graphics;
    auto submitted = state.device->EndFrame();
    if (!submitted) {
        return Result<void>::Failure(RecordGraphError(state, submitted.Error(), false,
                                                      "graph recording submission failed ({})",
                                                      ErrorCodeName(submitted.Error())));
    }

    // compute 기록이면 그 run의 async pass들에 제출 지점을 적는다(join이 기다린다).
    if (compute) {
        state.lastComputePoint = submitted.Value();
        for (uint32 pass = state.computeRunStart; pass < state.passes.Size(); ++pass) {
            PassRecord& record = state.passes[pass];
            if (record.culled) {
                continue;
            }

            if (!record.async) {
                break;
            }

            record.submitted = submitted.Value();
        }

        state.computeRunStart = GraphIndexNone;
    }

    return Result<void>::Success();
}

// 그래프 GPU 프레임을 graphics 기록에서 연다(compute 기록에는 GPU 구간을 두지 않는다, RG4).
void BeginTraceFrame(GraphState& state) noexcept {
    if (state.trace != nullptr && !state.traceFrameOpen && state.commands != nullptr &&
        state.recordingQueue == graphics::QueueKind::Graphics) {
        state.trace->BeginFrame(*state.device, *state.commands, state.frame);
        state.traceFrameOpen = true;
    }
}

// attachment 없는 graphics 기록을 연다(Compute·Copy pass, prologue·그래프 끝 join 기록).
Result<void> OpenGraphicsRecording(GraphState& state, const char* purpose) noexcept {
    auto begun = state.device->BeginPass(graphics::PassDesc{});
    if (!begun) {
        return Result<void>::Failure(RecordGraphError(state, begun.Error(), false,
                                                      "{} could not begin a recording ({})",
                                                      purpose, ErrorCodeName(begun.Error())));
    }

    state.commands = begun.Value();
    state.recordingQueue = graphics::QueueKind::Graphics;
    ++state.stats.recordings;
    BeginTraceFrame(state);
    RestoreScopeMarkers(state);
    return Result<void>::Success();
}

graphics::ResourceTransition ToDevice(const GraphState& state,
                                      const GraphTransitionRecord& record) noexcept {
    graphics::ResourceTransition transition;
    const ResourceRecord& resource = state.resources[record.resource];
    if (record.kind == GraphResourceKind::Texture) {
        transition.texture = graphics::TextureHandle{resource.device};
        transition.range.firstMip = record.firstMip;
        transition.range.mipCount = record.mipCount;
        transition.range.firstSlice = record.firstSlice;
        transition.range.sliceCount = record.sliceCount;
    } else {
        transition.buffer = graphics::BufferHandle{resource.device};
    }

    transition.before = record.before;
    transition.after = record.after;
    return transition;
}

// 분리 전이 묶음(beginPass, endPass)의 장치 전이 목록을 state.lowered에 만든다(begin과 end가 같은
// 목록을 넘기도록 늘 같은 순서로 만든다). 임시 공간 할당이 실패하면 false다.
bool BuildSplitList(FrameArena& arena, GraphState& state, uint32 beginPass,
                    uint32 endPass) noexcept {
    state.lowered.Clear();
    for (const GraphTransitionRecord& record : state.transitions) {
        if (record.beginPass != beginPass || record.pass != endPass) {
            continue;
        }

        if (!state.lowered.TryPushBack(arena, ToDevice(state, record))) {
            return false;
        }
    }

    return true;
}

// pass의 epilogue: 그 pass가 마지막 사용자인 분리 전이를 end pass별 묶음으로 begin한다.
void IssueSplitBegins(FrameArena& arena, GraphState& state, uint32 pass) noexcept {
    if (!state.splitEnabled || state.commands == nullptr) {
        return;
    }

    for (const GraphTransitionRecord& record : state.transitions) {
        if (record.beginPass != pass) {
            continue;
        }

        bool known = false;
        for (const SplitGroup& group : state.splitGroups) {
            known = known || (group.beginPass == pass && group.endPass == record.pass);
        }

        if (known) {
            continue;
        }

        SplitGroup group;
        group.beginPass = pass;
        group.endPass = record.pass;
        // 목록을 만들지 못하면 짝 없이 둔다(end가 null 짝으로 Transition처럼 낮춘다).
        if (BuildSplitList(arena, state, pass, record.pass)) {
            group.split = state.commands->BeginTransitions(state.lowered.View());
        }

        auto pushed = state.splitGroups.TryPushBack(arena, group);
        (void)pushed;
        state.lowered.Clear();
    }
}

// pass의 prologue에서 그 pass가 end하는 분리 전이 묶음을 끝낸다.
void IssueSplitEnds(FrameArena& arena, GraphState& state, uint32 pass) noexcept {
    if (!state.splitEnabled || state.commands == nullptr) {
        return;
    }

    for (const SplitGroup& group : state.splitGroups) {
        if (group.endPass != pass) {
            continue;
        }

        if (!BuildSplitList(arena, state, group.beginPass, pass)) {
            continue;
        }

        state.commands->EndTransitions(group.split, state.lowered.View());
        state.stats.transitions += static_cast<uint32>(state.lowered.Size());
        state.stats.splitTransitions += static_cast<uint32>(state.lowered.Size());
    }

    state.lowered.Clear();
}

// 전이 기록 가운데 pass 번호가 pass인 것을 커서부터 장치에 낸다(attachment는 장치 Begin이 이미
// 냈으므로 세기만 한다). 분리 전이는 먼저 end하고(IssueSplitEnds) 나머지를 한 번에 낸다.
void IssueTransitions(FrameArena& arena, GraphState& state, uint32 pass) noexcept {
    IssueSplitEnds(arena, state, pass);
    state.lowered.Clear();
    while (state.nextTransition < state.transitions.Size() &&
           state.transitions[state.nextTransition].pass == pass) {
        const GraphTransitionRecord& record = state.transitions[state.nextTransition];
        ++state.nextTransition;
        if (record.attachment) {
            ++state.stats.attachmentTransitions;
            continue;
        }

        if (record.beginPass != GraphIndexNone && state.splitEnabled) {
            continue;
        }

        // async compute fork 전이는 fork 자리의 graphics 기록이 낸다(IssueForkTransitions).
        if (record.forkPass != GraphIndexNone) {
            continue;
        }

        auto pushed = state.lowered.TryPushBack(arena, ToDevice(state, record));
        if (!pushed) {
            // 임시 공간이 없으면 지금까지 모은 것을 내고 하나씩 낸다.
            if (state.commands != nullptr && !state.lowered.IsEmpty()) {
                state.commands->Transition(state.lowered.View());
                state.stats.transitions += static_cast<uint32>(state.lowered.Size());
                state.lowered.Clear();
            }

            const graphics::ResourceTransition single = ToDevice(state, record);
            if (state.commands != nullptr) {
                state.commands->Transition(Span<const graphics::ResourceTransition>(&single, 1));
                ++state.stats.transitions;
            }
        }
    }

    if (state.commands != nullptr && !state.lowered.IsEmpty()) {
        state.commands->Transition(state.lowered.View());
        state.stats.transitions += static_cast<uint32>(state.lowered.Size());
    }

    state.lowered.Clear();
}

// fork 자리(graphics pass forkPass의 epilogue 또는 GraphProloguePass)의 graphics → async compute
// 전이를 지금 graphics 기록에 낸다(RG4: compute 기록은 graphics 상태 barrier를 낼 수 없다).
void IssueForkTransitions(FrameArena& arena, GraphState& state, uint32 forkPass) noexcept {
    state.lowered.Clear();
    for (const GraphTransitionRecord& record : state.transitions) {
        if (record.forkPass != forkPass) {
            continue;
        }

        const graphics::ResourceTransition transition = ToDevice(state, record);
        if (!state.lowered.TryPushBack(arena, transition) && state.commands != nullptr) {
            state.commands->Transition(Span<const graphics::ResourceTransition>(&transition, 1));
            ++state.stats.transitions;
        }
    }

    if (state.commands != nullptr && !state.lowered.IsEmpty()) {
        state.commands->Transition(state.lowered.View());
        state.stats.transitions += static_cast<uint32>(state.lowered.Size());
    }

    state.lowered.Clear();
}

// fork pass forkPass를 기다리는 run의 async pass 자원을 미리 할당한다(fork 전이가 장치 handle을
// 쓴다).
Result<void> PrepareForkedPasses(FrameArena& arena, GraphState& state, uint32 forkPass) noexcept {
    for (uint32 pass = 0; pass < state.passes.Size(); ++pass) {
        const PassRecord& record = state.passes[pass];
        if (record.async && record.runFork == forkPass) {
            auto prepared = PreparePass(arena, state, pass);
            if (!prepared) {
                return prepared;
            }
        }
    }

    return Result<void>::Success();
}

// graphics queue가 compute 지점 point를 기다리게 한다(join).
Result<void> WaitComputeOnGraphics(GraphState& state, graphics::GpuFencePoint point) noexcept {
    if (point.IsNull() || point.value <= state.joinedComputeValue) {
        return Result<void>::Success();
    }

    auto waited = state.device->QueueWait(graphics::QueueKind::Graphics, point);
    if (!waited) {
        return Result<void>::Failure(RecordGraphError(state, waited.Error(), false,
                                                      "graph join wait failed ({})",
                                                      ErrorCodeName(waited.Error())));
    }

    state.joinedComputeValue = point.value;
    ++state.stats.queueWaits;
    return Result<void>::Success();
}

// run의 첫 async pass에서 compute 기록을 연다: 먼저 run의 fork 지점을 compute queue가 기다린다.
Result<void> OpenComputeRecording(GraphState& state, uint32 pass) noexcept {
    const PassRecord& record = state.passes[pass];
    const graphics::GpuFencePoint fork = record.runFork == GraphIndexNone
                                             ? state.prologuePoint
                                             : state.passes[record.runFork].submitted;
    if (!fork.IsNull() && fork.value > state.forkWaitedValue) {
        auto waited = state.device->QueueWait(graphics::QueueKind::AsyncCompute, fork);
        if (!waited) {
            return Result<void>::Failure(RecordGraphError(state, waited.Error(), false,
                                                          "graph fork wait failed ({})",
                                                          ErrorCodeName(waited.Error())));
        }

        state.forkWaitedValue = fork.value;
        ++state.stats.queueWaits;
    }

    graphics::PassDesc desc;
    desc.queue = graphics::QueueKind::AsyncCompute;
    auto begun = state.device->BeginPass(desc);
    if (!begun) {
        const char* passName = record.name->Name() != nullptr ? record.name->Name() : "";
        return Result<void>::Failure(RecordGraphError(
            state, begun.Error(), false, "async compute pass '{}' could not begin a recording ({})",
            passName, ErrorCodeName(begun.Error())));
    }

    state.commands = begun.Value();
    state.recordingQueue = graphics::QueueKind::AsyncCompute;
    state.computeRunStart = pass;
    ++state.stats.recordings;
    ++state.stats.computeRecordings;
    RestoreScopeMarkers(state);
    return Result<void>::Success();
}

} // namespace

} // namespace rocks::render::detail

namespace rocks::render {

using namespace detail;

// RenderGraph의 friend가 아닌 곳에서 GraphPassContext를 만들 수 없으므로 실행 helper는
// RenderGraph의 내부 접근을 통해 문맥을 만든다(GraphPassContext의 생성자는 RenderGraph만 부른다).
class GraphExecutionAccess {
public:
    static void Run(RenderGraph& graph, GraphPassExecutor& executor,
                    graphics::CommandContext& commands, uint32 pass) noexcept;
    // 병렬 기록: 기록 함수를 기록 목록에 기록한다(worker, CPU 구간만).
    static void Record(RenderGraph& graph, GraphPassExecutor& executor, GraphCommandList& list,
                       uint32 pass) noexcept;
    // 기록 목록을 장치 기록에 재생한다(소유 스레드, Run과 같은 GPU 구간 위치).
    static Result<void> Replay(RenderGraph& graph, const GraphCommandList& list,
                               graphics::CommandContext& commands, uint32 pass) noexcept;
};

} // namespace rocks::render

namespace rocks::render::detail {

namespace {

// ancestor가 scope 사슬(자신 포함)에 있는지다.
bool IsWithinScope(const GraphState& state, uint32 scope, uint32 ancestor) noexcept {
    for (uint32 current = scope; current != GraphIndexNone;
         current = state.scopes[current].parent) {
        if (current == ancestor) {
            return true;
        }
    }

    return false;
}

bool IsScopeOpen(const GraphState& state, uint32 scope) noexcept {
    for (const OpenScopeEntry& entry : state.openScopes) {
        if (entry.scope == scope) {
            return true;
        }
    }

    return false;
}

// scope와 아직 열지 않은 바깥 구간을 바깥부터 연다.
void EnterScopeChain(FrameArena& arena, GraphState& state, uint32 scope) noexcept {
    if (scope == GraphIndexNone || IsScopeOpen(state, scope)) {
        return;
    }

    const ScopeRecord& record = state.scopes[scope];
    EnterScopeChain(arena, state, record.parent);
    OpenScopeEntry entry;
    entry.scope = scope;
    if (TraceCpuChannel.IsEnabled()) {
        entry.cpuSerial = rocks::detail::BeginCpuScope(*record.name);
    }

    if (state.trace != nullptr && state.commands != nullptr) {
        if (state.trace->MarkersEnabled()) {
            state.commands->PushMarker(StringView(record.name->Name()));
            ++state.scopeMarkers;
        }

        // GPU 구간은 graphics queue 하나의 장부다(compute 기록에서 연 구간은 CPU 구간만, RG4).
        if (state.recordingQueue == graphics::QueueKind::Graphics) {
            entry.gpuSpan = state.trace->BeginSpan(*state.commands, *record.name);
            entry.gpuOpened = true;
        }
    }

    // 번호가 있는 구간은 연 직후 trace 표지로 번호를 남긴다(7.5절).
    if (record.hasIndex) {
        ROCKS_TRACE_MARK("Graph scope {} {}", StringView(record.name->Name()), record.index);
    }

    auto pushed = state.openScopes.TryPushBack(arena, entry);
    (void)pushed;
}

} // namespace

void LeaveScopes(GraphState& state, uint32 scope) noexcept {
    while (!state.openScopes.IsEmpty()) {
        const OpenScopeEntry entry = state.openScopes.Back();
        if (IsWithinScope(state, scope, entry.scope)) {
            break;
        }

        // 끝 timestamp는 graphics 기록에만 쓴다(compute 기록이면 그 구간을 무효로 닫는다).
        if (state.trace != nullptr && entry.gpuOpened) {
            if (state.commands != nullptr &&
                state.recordingQueue == graphics::QueueKind::Graphics) {
                state.trace->EndSpan(*state.commands, entry.gpuSpan);
            } else {
                state.trace->DropSpan(entry.gpuSpan);
            }
        }

        if (state.commands != nullptr && state.scopeMarkers > 0) {
            state.commands->PopMarker();
            --state.scopeMarkers;
        }

        if (entry.cpuSerial != 0) {
            rocks::detail::EndCpuScope(entry.cpuSerial);
        }

        state.openScopes.PopBack();
    }
}

void EnterScopes(FrameArena& arena, GraphState& state, uint32 scope) noexcept {
    EnterScopeChain(arena, state, scope);
}

namespace {

// pass가 지금 장치 기록을 연 Raster pass의 attachment subresource에 접근하는지다.
bool UsesOpenAttachment(const GraphState& state, uint32 pass) noexcept {
    return state.recordingPass != GraphIndexNone &&
           UsesAttachmentsOf(state, pass, state.recordingPass);
}

} // namespace

bool UsesAttachmentsOf(const GraphState& state, uint32 pass, uint32 rasterPass) noexcept {
    const PassRecord& owner = state.passes[rasterPass];
    const PassRecord& record = state.passes[pass];
    for (uint32 index = record.firstUse; index < record.firstUse + record.useCount; ++index) {
        const SubresourceUse& use = state.uses[index];
        // 장치가 막는 것은 패스 attachment의 복사다(CopyTexture). SRV·UAV 접근은 backend가 render
        // pass를 나눠 처리한다.
        if (!graphics::HasAnyAccess(use.access,
                                    ResourceAccess::CopySource | ResourceAccess::CopyDestination)) {
            continue;
        }

        for (uint32 other = owner.firstUse; other < owner.firstUse + owner.useCount; ++other) {
            const SubresourceUse& attached = state.uses[other];
            if (attached.attachment && attached.resource == use.resource &&
                attached.subresource == use.subresource) {
                return true;
            }
        }
    }

    return false;
}

Result<void> PreparePass(FrameArena& arena, GraphState& state, uint32 pass) noexcept {
    PassRecord& record = state.passes[pass];
    const char* passName = record.name->Name() != nullptr ? record.name->Name() : "";
    if (record.prepared) {
        return Result<void>::Success();
    }

    record.prepared = true;
    // 첫 사용 pass 직전에 받는다(두 모드가 같은 지점이다. Deferred의 업로드 대상은 그래프 시작에
    // 이미 받았다).
    for (uint32 index = record.firstUse; index < record.firstUse + record.useCount; ++index) {
        const uint32 resource = state.uses[index].resource;
        auto allocated = AllocateResource(arena, state, resource);
        if (!allocated) {
            return allocated;
        }

        if (state.mode == GraphMode::Immediate) {
            ResourceRecord& entry = state.resources[resource];
            ++entry.references;
            if (entry.firstPass == GraphIndexNone) {
                entry.firstPass = pass;
            }

            entry.lastPass = pass;
            entry.released = !entry.imported && !entry.exported;
        }
    }

    // 이 pass가 처음 쓰는 view의 장치 view를 만든다(자원 할당 뒤, 그래프 끝에 Retire한다).
    for (uint32 index = record.firstView; index < record.firstView + record.viewCount; ++index) {
        ViewRecord& view = state.views[state.passViews[index]];
        if (!view.device.IsNull()) {
            continue;
        }

        const ResourceRecord& resource = state.resources[view.texture];
        auto created =
            state.device->CreateTextureView(graphics::TextureHandle{resource.device}, view.desc);
        if (!created) {
            return Result<void>::Failure(RecordGraphError(
                state, created.Error(), false, "pass '{}' could not create a view of '{}' ({})",
                passName, resource.name, ErrorCodeName(created.Error())));
        }

        view.device = created.Value().slot;
    }

    return Result<void>::Success();
}

Result<void> RecordPassesInParallel(RenderGraph& graph, FrameArena& arena,
                                    GraphState& state) noexcept {
    ArenaArray<uint32> order;
    for (uint32 pass = 0; pass < state.passes.Size(); ++pass) {
        if (!state.passes[pass].culled) {
            auto pushed = order.TryPushBack(arena, pass);
            if (!pushed) {
                return pushed;
            }
        }
    }

    if (order.Size() < 2) {
        return Result<void>::Success();
    }

    // 소유 스레드가 선언 순서로 자원 할당·장치 view 생성을 모두 마친다(기록 함수가 보는 장치
    // handle이 정해진다).
    for (const uint32 pass : order) {
        auto prepared = PreparePass(arena, state, pass);
        if (!prepared) {
            return prepared;
        }
    }

    // pass마다 기록 목록 하나다. ParallelFor가 돌아오면 모든 기록이 끝났다(명시 join).
    auto recorded = ParallelFor(*state.scheduler, *state.scratch, order.Size(),
                                [&](usize begin, usize end) noexcept -> ErrorCode {
                                    for (usize index = begin; index < end; ++index) {
                                        const uint32 pass = order[index];
                                        PassRecord& record = state.passes[pass];
                                        auto list = arena.TryNew<GraphCommandList>(arena);
                                        if (!list) {
                                            return list.Error();
                                        }

                                        GraphExecutionAccess::Record(graph, *record.executor,
                                                                     *list.Value(), pass);
                                        if (list.Value()->RecordError() != ErrorCode::None) {
                                            return list.Value()->RecordError();
                                        }

                                        record.recorded = list.Value();
                                    }

                                    return ErrorCode::None;
                                });
    if (!recorded) {
        return Result<void>::Failure(RecordGraphError(state, recorded.Error(), false,
                                                      "parallel pass recording failed ({})",
                                                      ErrorCodeName(recorded.Error())));
    }

    state.stats.parallelPasses = static_cast<uint32>(order.Size());
    return Result<void>::Success();
}

Result<void> ExecutePass(RenderGraph& graph, FrameArena& arena, GraphState& state,
                         uint32 pass) noexcept {
    PassRecord& record = state.passes[pass];
    const char* passName = record.name->Name() != nullptr ? record.name->Name() : "";
    auto prepared = PreparePass(arena, state, pass);
    if (!prepared) {
        return prepared;
    }

    // 즉시 실행 모드는 이 pass의 전이를 지금 계산한다(앞을 내다보지 않으므로 합치지 않는다).
    if (state.mode == GraphMode::Immediate) {
        const usize firstChain = state.chains.Size();
        auto tracked = TrackPassTransitions(arena, state, pass, false);
        if (!tracked) {
            return tracked;
        }

        auto collected = CollectTransitions(arena, state, firstChain);
        if (!collected) {
            return collected;
        }
    }

    // async compute(RG4): compute 기록에 이어 쓰고, 열린 graphics 기록이 있으면 먼저 닫는다. run의
    // 첫 pass가 fork 지점을 기다리고 compute 기록을 연다.
    if (record.async) {
        if (state.commands != nullptr && state.recordingQueue == graphics::QueueKind::Graphics) {
            LeaveScopes(state, record.scope);
            auto closed = CloseRecording(state);
            if (!closed) {
                return closed;
            }
        }

        if (state.commands == nullptr) {
            auto opened = OpenComputeRecording(state, pass);
            if (!opened) {
                return opened;
            }
        }
    } else {
        // graphics pipe로 돌아오면 compute 기록을 제출한다.
        if (state.commands != nullptr &&
            state.recordingQueue == graphics::QueueKind::AsyncCompute) {
            LeaveScopes(state, record.scope);
            auto closed = CloseRecording(state);
            if (!closed) {
                return closed;
            }
        }

        // join: 새 graphics 기록 앞에서, 이 pass를 join으로 둔 async pass들의 compute 제출을
        // 기다린다.
        if (record.joins) {
            if (state.commands != nullptr) {
                LeaveScopes(state, record.scope);
                auto closed = CloseRecording(state);
                if (!closed) {
                    return closed;
                }
            }

            graphics::GpuFencePoint point;
            for (const PassRecord& async : state.passes) {
                if (async.async && async.joinPass == pass && async.submitted.value > point.value) {
                    point = async.submitted;
                }
            }

            auto joined = WaitComputeOnGraphics(state, point);
            if (!joined) {
                return joined;
            }
        }
    }

    // Raster pass는 자기 렌더 타깃으로 장치 기록을 연다. 그 밖은 열린 기록에 이어 쓴다. 이 pass의
    // 구간 사슬 밖 구간은 지금 기록에서 닫는다(GPU 구간 끝이 앞 기록에 남는다).
    if (record.async) {
        // 위에서 compute 기록을 열었다.
    } else if (record.kind == GraphPassKind::Raster) {
        LeaveScopes(state, record.scope);
        auto closed = CloseRecording(state);
        if (!closed) {
            return closed;
        }

        Result<graphics::CommandContext*> begun =
            Result<graphics::CommandContext*>::Failure(ErrorCode::InvalidArgument);
        if (record.target.kind == GraphTargetKind::Texture) {
            // attachment는 장치 BeginPass로 붙인다(색 MRT·깊이·load·mip·slice, 전이는 장치가 낸다).
            const auto loadOf = [](GraphLoadAction load) noexcept {
                return load == GraphLoadAction::Load       ? graphics::LoadAction::Load
                       : load == GraphLoadAction::DontCare ? graphics::LoadAction::DontCare
                                                           : graphics::LoadAction::Clear;
            };
            graphics::PassDesc desc;
            desc.colorCount = record.target.colorCount;
            for (uint32 index = 0; index < record.target.colorCount; ++index) {
                const GraphColorTarget& color = record.target.colors[index];
                graphics::ColorAttachment& attachment = desc.colors[index];
                attachment.texture =
                    graphics::TextureHandle{state.resources[color.texture.index].device};
                attachment.load = loadOf(color.load);
                attachment.clear = color.clear;
                attachment.mipLevel = color.mipLevel;
                attachment.slice = color.slice;
                if (!color.resolveTarget.IsNull()) {
                    attachment.resolveTarget =
                        graphics::TextureHandle{state.resources[color.resolveTarget.index].device};
                    attachment.resolveMipLevel = color.resolveMipLevel;
                    attachment.resolveSlice = color.resolveSlice;
                }
            }

            if (record.target.hasDepth) {
                const GraphDepthTarget& depth = record.target.depth;
                desc.depth.texture =
                    graphics::TextureHandle{state.resources[depth.texture.index].device};
                desc.depth.depthLoad = loadOf(depth.depthLoad);
                desc.depth.clearDepth = depth.clearDepth;
                desc.depth.stencilLoad = loadOf(depth.stencilLoad);
                desc.depth.clearStencil = depth.clearStencil;
                desc.depth.readOnly = depth.readOnly;
                desc.depth.mipLevel = depth.mipLevel;
                desc.depth.slice = depth.slice;
            }

            begun = state.device->BeginPass(desc);
        } else {
            begun = state.device->BeginFrame(record.target.swapChain, record.target.clear);
        }

        if (!begun) {
            return Result<void>::Failure(RecordGraphError(
                state, begun.Error(), false, "pass '{}' could not begin its render target ({})",
                passName, ErrorCodeName(begun.Error())));
        }

        state.commands = begun.Value();
        state.recordingQueue = graphics::QueueKind::Graphics;
        state.recordingPass = pass;
        ++state.stats.recordings;
        BeginTraceFrame(state);
        RestoreScopeMarkers(state);
    } else if (state.commands != nullptr && UsesOpenAttachment(state, pass)) {
        // Compute·Copy pass가 열린 기록의 attachment subresource에 접근하면 그 기록을 닫는다(장치는
        // 패스 attachment의 복사·UAV를 막는다). 아래에서 attachment 없는 기록을 연다.
        LeaveScopes(state, record.scope);
        auto closed = CloseRecording(state);
        if (!closed) {
            return closed;
        }
    }

    if (!record.async && record.kind != GraphPassKind::Raster && state.commands == nullptr) {
        // 렌더 타깃이 없는 Compute·Copy pass는 attachment 없는 기록을 연다.
        auto begun = state.device->BeginPass(graphics::PassDesc{});
        if (!begun) {
            return Result<void>::Failure(RecordGraphError(
                state, begun.Error(), false, "{} pass '{}' could not begin a recording ({})",
                record.kind == GraphPassKind::Copy ? "copy" : "compute", passName,
                ErrorCodeName(begun.Error())));
        }

        state.commands = begun.Value();
        state.recordingQueue = graphics::QueueKind::Graphics;
        ++state.stats.recordings;
        BeginTraceFrame(state);
        RestoreScopeMarkers(state);
    }

    // 그래프 구간(GraphScope)을 이 pass의 사슬에 맞춘다(전이도 구간 안이다).
    LeaveScopes(state, record.scope);
    EnterScopes(arena, state, record.scope);
    IssueTransitions(arena, state, pass);
    if (record.recorded != nullptr) {
        // 병렬 기록: 기록 함수 위치에 기록 목록을 재생한다(장치 호출 열이 직렬과 같다).
        auto replayed =
            GraphExecutionAccess::Replay(graph, *record.recorded, *state.commands, pass);
        if (!replayed) {
            return Result<void>::Failure(RecordGraphError(
                state, replayed.Error(), false, "pass '{}' recorded commands failed on replay ({})",
                passName, ErrorCodeName(replayed.Error())));
        }
    } else {
        GraphExecutionAccess::Run(graph, *record.executor, *state.commands, pass);
    }

    IssueSplitBegins(arena, state, pass);
    record.executed = true;
    ++state.stats.executedPasses;
    if (record.async) {
        ++state.stats.asyncComputePasses;
    }

    // fork pass: 기다리는 run의 fork 전이를 이 graphics 기록에 내고 기록을 닫아 지점을 남긴다(다음
    // pass는 새 기록을 연다). 구간은 열린 채로 두고 marker만 이 기록에서 닫는다.
    if (record.forks) {
        auto forked = PrepareForkedPasses(arena, state, pass);
        if (!forked) {
            return forked;
        }

        IssueForkTransitions(arena, state, pass);
        auto closed = CloseRecording(state);
        if (!closed) {
            return closed;
        }

        record.submitted = state.device->LastSubmittedFence(graphics::QueueKind::Graphics);
    }

    return Result<void>::Success();
}

Result<void> ExecutePrologue(FrameArena& arena, GraphState& state) noexcept {
    // 그래프 업로드를 먼저 제출한다(prologue fork가 그 뒤를 기다린다. 업로드 대상을 async pass가
    // 읽을 수 있다).
    auto uploads = state.device->SubmitUploads();
    if (!uploads) {
        return Result<void>::Failure(RecordGraphError(state, uploads.Error(), false,
                                                      "graph upload submission failed ({})",
                                                      ErrorCodeName(uploads.Error())));
    }

    bool prologue = false;
    for (const GraphTransitionRecord& record : state.transitions) {
        prologue = prologue || record.forkPass == GraphProloguePass;
    }

    // prologue fork 전이(처음 쓰는 상태가 graphics 상태인 자원 등)는 그 전이만 담은 graphics 기록
    // 하나에서 낸다(UE prologue pass).
    if (prologue) {
        auto prepared = PrepareForkedPasses(arena, state, GraphIndexNone);
        if (!prepared) {
            return prepared;
        }

        auto opened = OpenGraphicsRecording(state, "graph prologue");
        if (!opened) {
            return opened;
        }

        IssueForkTransitions(arena, state, GraphProloguePass);
        auto closed = CloseRecording(state);
        if (!closed) {
            return closed;
        }
    }

    state.prologuePoint = state.device->LastSubmittedFence(graphics::QueueKind::Graphics);
    return Result<void>::Success();
}

Result<void> FinishGraph(FrameArena& arena, GraphState& state, bool success) noexcept {
    Result<void> outcome = Result<void>::Success();

    // 그래프 끝 join(RG4, UE epilogue): 열린 compute 기록을 제출하고, graphics queue가 아직
    // 기다리지 않은 compute 제출이 있으면 그것을 기다리는 graphics 기록을 연다. 그래서 그래프의
    // 마지막 제출은 늘 graphics이고 마지막 전이·export 상태도 그 기록에서 낸다.
    if (state.commands != nullptr && state.recordingQueue == graphics::QueueKind::AsyncCompute) {
        auto closed = CloseRecording(state);
        if (!closed) {
            outcome = Move(closed);
        }
    }

    if (state.lastComputePoint.value > state.joinedComputeValue) {
        auto closed = CloseRecording(state);
        auto joined = closed ? WaitComputeOnGraphics(state, state.lastComputePoint) : Move(closed);
        if (joined) {
            joined = OpenGraphicsRecording(state, "graph epilogue");
        }

        if (outcome && !joined) {
            outcome = Move(joined);
        }
    }

    // 그래프 끝 상태(export·import 마지막 상태) 전이를 마지막 기록에 낸다.
    if (success && state.mode == GraphMode::Immediate) {
        const usize firstChain = state.chains.Size();
        auto finals = TrackFinalTransitions(arena, state);
        if (finals) {
            finals = CollectTransitions(arena, state, firstChain);
        }

        if (!finals) {
            outcome = Move(finals);
        }
    }

    if (success && outcome) {
        IssueTransitions(arena, state, GraphIndexNone);
    }

    // 열린 그래프 구간을 모두 닫는다(마지막 기록 안에서, 프레임 끝 timestamp 앞).
    LeaveScopes(state, GraphIndexNone);
    if (state.traceFrameOpen && state.commands != nullptr) {
        state.trace->EndFrame(*state.commands);
    }

    state.traceFrameOpen = false;
    auto closed = CloseRecording(state);
    if (outcome && !closed) {
        outcome = Move(closed);
    }

    // 장치 view를 놓는다(장치 Retire는 마지막 사용 fence 뒤 해제다).
    for (ViewRecord& view : state.views) {
        if (!view.device.IsNull()) {
            state.device->Retire(graphics::TextureHandle{view.device});
            view.device = {};
        }
    }

    const bool completed = success && outcome.IsOk();
    const graphics::GpuFencePoint lastUse = state.device->LastSubmittedFence();
    for (ResourceRecord& resource : state.resources) {
        if (resource.imported) {
            if (resource.lease.IsValid()) {
                state.handles->ReleaseLease(resource.lease);
                resource.lease = {};
            }

            continue;
        }

        if (resource.exported) {
            // 풀 자원을 export handle에 잇는다(결정 R15: 자원은 풀 소속 그대로이고 handle의
            // Retire는 fence 뒤 풀 반납이다). 실패한 그래프의 export handle은 Failed다.
            bool bound = false;
            if (completed && resource.allocated) {
                PhysicalResource& physical = state.physicals[resource.physical];
                auto exported = state.pool->Export(physical.entry, *state.handles, resource.handle);
                bound = exported.IsOk();
                physical.exported = bound;
            }

            if (!bound) {
                auto failed = state.handles->MarkFailed(
                    resource.handle,
                    state.failure != ErrorCode::None ? state.failure : ErrorCode::Cancelled);
                (void)failed;
            }
        }

        resource.allocated = false;
    }

    // 풀에서 받은 자리를 그래프 끝에 한 번씩 돌려준다(같은 그래프 안에서 여러 자원이 쓴 자리도
    // 하나다). export한 자리는 handle이 가진다.
    for (PhysicalResource& physical : state.physicals) {
        if (physical.acquired && !physical.exported) {
            state.pool->Release(physical.entry, lastUse, state.frame);
        }

        physical.acquired = false;
    }

    state.pool->Evict(state.frame);
    return outcome;
}

} // namespace rocks::render::detail

namespace rocks::render {

void GraphExecutionAccess::Run(RenderGraph& graph, GraphPassExecutor& executor,
                               graphics::CommandContext& commands, uint32 pass) noexcept {
    GraphState& state = *graph.state_;
    TraceScopeName& name = *state.passes[pass].name;
    GraphPassContext context(graph, commands, pass);
    CpuScope cpu(name);
    if (state.trace != nullptr && state.recordingQueue == graphics::QueueKind::Graphics) {
        GpuScope gpu(*state.trace, commands, name);
        executor.Execute(context);
        return;
    }

    // compute 기록은 GPU 구간 없이 marker만 둔다(RG4).
    const bool marker = state.trace != nullptr && state.trace->MarkersEnabled();
    if (marker) {
        commands.PushMarker(StringView(name.Name()));
    }

    executor.Execute(context);
    if (marker) {
        commands.PopMarker();
    }
}

void GraphExecutionAccess::Record(RenderGraph& graph, GraphPassExecutor& executor,
                                  GraphCommandList& list, uint32 pass) noexcept {
    GraphState& state = *graph.state_;
    GraphPassContext context(graph, list, pass);
    CpuScope cpu(*state.passes[pass].name);
    executor.Execute(context);
}

Result<void> GraphExecutionAccess::Replay(RenderGraph& graph, const GraphCommandList& list,
                                          graphics::CommandContext& commands,
                                          uint32 pass) noexcept {
    GraphState& state = *graph.state_;
    TraceScopeName& name = *state.passes[pass].name;
    if (state.trace != nullptr && state.recordingQueue == graphics::QueueKind::Graphics) {
        GpuScope gpu(*state.trace, commands, name);
        return list.Replay(commands);
    }

    const bool marker = state.trace != nullptr && state.trace->MarkersEnabled();
    if (marker) {
        commands.PushMarker(StringView(name.Name()));
    }

    auto replayed = list.Replay(commands);
    if (marker) {
        commands.PopMarker();
    }

    return replayed;
}

} // namespace rocks::render
