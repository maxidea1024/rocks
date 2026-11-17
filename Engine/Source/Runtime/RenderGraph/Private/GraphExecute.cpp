#include "RenderGraphInternal.h"

// 그래프 실행이다: 첫 사용 pass 직전 풀 할당, Raster pass마다 장치 기록 열기, prologue 즉시 전이,
// pass 기록 함수와 CPU·GPU 구간, 그래프 끝 마무리(Docs/Design/RenderBase.ko.md 7.1절 실행 행, UE
// RC/Private/RenderGraphBuilder.cpp:2104–2167, 3424–3528).
namespace rocks::render::detail {

using graphics::ResourceAccess;

namespace {

// 만든 자원의 장치 자원을 풀에서 받는다.
Result<void> AllocateResource(GraphState& state, uint32 index) noexcept {
    ResourceRecord& resource = state.resources[index];
    if (resource.allocated) {
        return Result<void>::Success();
    }

    auto acquired = resource.kind == GraphResourceKind::Texture
                        ? state.pool->AcquireTexture(resource.textureDesc)
                        : state.pool->AcquireBuffer(resource.bufferDesc);
    if (!acquired) {
        return Result<void>::Failure(
            RecordGraphError(state, acquired.Error(), false, "'{}' could not be allocated ({})",
                             resource.name, ErrorCodeName(acquired.Error())));
    }

    resource.device = acquired.Value().device;
    resource.poolEntry = acquired.Value().entry;
    resource.allocated = true;
    if (acquired.Value().reused) {
        ++state.stats.reusedResources;
    } else {
        ++state.stats.createdResources;
    }

    return Result<void>::Success();
}

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
        transition.range.firstSlice = 0;
        transition.range.sliceCount = 1;
    } else {
        transition.buffer = graphics::BufferHandle{resource.device};
    }

    transition.before = record.before;
    transition.after = record.after;
    return transition;
}

// 전이 기록 가운데 pass 번호가 pass인 것을 커서부터 장치에 낸다(attachment는 장치 Begin이 이미
// 냈으므로 세기만 한다).
void IssueTransitions(FrameArena& arena, GraphState& state, uint32 pass) noexcept {
    state.lowered.Clear();
    while (state.nextTransition < state.transitions.Size() &&
           state.transitions[state.nextTransition].pass == pass) {
        const GraphTransitionRecord& record = state.transitions[state.nextTransition];
        ++state.nextTransition;
        if (record.attachment) {
            ++state.stats.attachmentTransitions;
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

    // 첫 사용 pass 직전에 풀에서 받는다(두 모드가 같은 지점이다).
    for (uint32 index = record.firstUse; index < record.firstUse + record.useCount; ++index) {
        const uint32 resource = state.uses[index].resource;
        auto allocated = AllocateResource(state, resource);
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
            // 소유를 export handle로 넘긴다. 실패한 그래프의 export handle은 Failed다.
            bool bound = false;
            if (completed && resource.allocated) {
                auto detached = state.pool->Detach(resource.poolEntry);
                if (detached) {
                    auto attached =
                        resource.kind == GraphResourceKind::Texture
                            ? state.handles->BindTexture(resource.handle,
                                                         graphics::TextureHandle{detached.Value()})
                            : state.handles->BindBuffer(resource.handle,
                                                        graphics::BufferHandle{detached.Value()});
                    bound = attached.IsOk();
                    if (!bound) {
                        // 잇지 못한 장치 자원은 바로 회수한다(장치가 마지막 사용 fence 뒤에
                        // 놓는다).
                        if (resource.kind == GraphResourceKind::Texture) {
                            state.device->Retire(graphics::TextureHandle{detached.Value()});
                        } else {
                            state.device->Retire(graphics::BufferHandle{detached.Value()});
                        }
                    }

                    resource.allocated = false;
                }
            }

            if (!bound) {
                auto failed = state.handles->MarkFailed(
                    resource.handle,
                    state.failure != ErrorCode::None ? state.failure : ErrorCode::Cancelled);
                (void)failed;
            }
        }

        if (resource.allocated) {
            state.pool->Release(resource.poolEntry, lastUse, state.frame);
            resource.allocated = false;
        }
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
