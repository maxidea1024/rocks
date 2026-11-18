#include "RenderGraphInternal.h"

// 그래프 실행이다: 그래프 시작 업로드, 첫 사용 pass 직전 할당(같은 그래프 안 재사용 자리 또는 풀),
// Raster pass마다 장치 기록 열기, prologue 즉시 전이, pass 기록 함수와 CPU·GPU 구간, 그래프 끝
// 마무리(Docs/Design/RenderBase.ko.md 7.1절 실행·업로드 행, 8절 RG2, UE
// RC/Private/RenderGraphBuilder.cpp:2104–2167, 2691–2837, 3424–3528).
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
Result<void> CloseRecording(GraphState& state) noexcept {
    if (state.commands == nullptr) {
        return Result<void>::Success();
    }

    state.commands = nullptr;
    auto submitted = state.device->EndFrame();
    if (!submitted) {
        return Result<void>::Failure(RecordGraphError(state, submitted.Error(), false,
                                                      "graph recording submission failed ({})",
                                                      ErrorCodeName(submitted.Error())));
    }

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
};

} // namespace rocks::render

namespace rocks::render::detail {

Result<void> ExecutePass(RenderGraph& graph, FrameArena& arena, GraphState& state,
                         uint32 pass) noexcept {
    PassRecord& record = state.passes[pass];
    const char* passName = record.name->Name() != nullptr ? record.name->Name() : "";

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

    // Raster pass는 자기 렌더 타깃으로 장치 기록을 연다. 그 밖은 열린 기록에 이어 쓴다.
    if (record.kind == GraphPassKind::Raster) {
        auto closed = CloseRecording(state);
        if (!closed) {
            return closed;
        }

        Result<graphics::CommandContext*> begun =
            Result<graphics::CommandContext*>::Failure(ErrorCode::InvalidArgument);
        if (record.target.kind == GraphTargetKind::Texture) {
            const ResourceRecord& target = state.resources[record.target.texture.index];
            begun = state.device->BeginOffscreen(graphics::TextureHandle{target.device},
                                                 record.target.clear);
        } else {
            begun = state.device->BeginFrame(record.target.swapChain, record.target.clear);
        }

        if (!begun) {
            return Result<void>::Failure(RecordGraphError(
                state, begun.Error(), false, "pass '{}' could not begin its render target ({})",
                passName, ErrorCodeName(begun.Error())));
        }

        state.commands = begun.Value();
        ++state.stats.recordings;
        if (state.trace != nullptr && !state.traceFrameOpen) {
            state.trace->BeginFrame(*state.device, *state.commands, state.frame);
            state.traceFrameOpen = true;
        }
    } else if (state.commands == nullptr) {
        return Result<void>::Failure(RecordGraphError(
            state, ErrorCode::Unsupported, false,
            "{} pass '{}' has no open recording; recordings without a render target arrive "
            "with GX4",
            record.kind == GraphPassKind::Copy ? "copy" : "compute", passName));
    }

    IssueTransitions(arena, state, pass);
    GraphExecutionAccess::Run(graph, *record.executor, *state.commands, pass);
    IssueSplitBegins(arena, state, pass);
    record.executed = true;
    ++state.stats.executedPasses;
    return Result<void>::Success();
}

Result<void> FinishGraph(FrameArena& arena, GraphState& state, bool success) noexcept {
    Result<void> outcome = Result<void>::Success();

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

    if (state.traceFrameOpen && state.commands != nullptr) {
        state.trace->EndFrame(*state.commands);
    }

    state.traceFrameOpen = false;
    auto closed = CloseRecording(state);
    if (outcome && !closed) {
        outcome = Move(closed);
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
    if (state.trace != nullptr) {
        GpuScope gpu(*state.trace, commands, name);
        executor.Execute(context);
        return;
    }

    executor.Execute(context);
}

} // namespace rocks::render
