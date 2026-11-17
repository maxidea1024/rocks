#include "RenderGraphInternal.h"

// 그래프 컴파일이다: 미사용 제거(culling), 참조 수 수명, subresource 상태 추적과 전이 계산
// (Docs/Design/RenderBase.ko.md 7.1절 culling·컴파일·barrier·수명 행, UE
// RC/Private/RenderGraphBuilder.cpp:1199–1325, 3783–3928,
// RC/Private/RenderGraphResources.cpp:69–152).
namespace rocks::render::detail {

using graphics::ResourceAccess;

Result<void> UncullPass(FrameArena& arena, GraphState& state, uint32 pass) noexcept {
    if (!state.passes[pass].culled) {
        return Result<void>::Success();
    }

    state.passes[pass].culled = false;
    state.stack.Clear();
    auto pushed = state.stack.TryPushBack(arena, pass);
    if (!pushed) {
        return pushed;
    }

    // 생산자 간선을 거꾸로 따라 살린다(이미 산 pass에서 멈춘다).
    while (!state.stack.IsEmpty()) {
        const uint32 current = state.stack.Back();
        state.stack.PopBack();
        for (uint32 link = state.passes[current].firstProducer; link != GraphIndexNone;
             link = state.producers[link].next) {
            const uint32 producer = state.producers[link].pass;
            if (!state.passes[producer].culled) {
                continue;
            }

            state.passes[producer].culled = false;
            auto stacked = state.stack.TryPushBack(arena, producer);
            if (!stacked) {
                return stacked;
            }
        }
    }

    return Result<void>::Success();
}

Result<void> TrackPassTransitions(FrameArena& arena, GraphState& state, uint32 pass,
                                  bool allowMerge) noexcept {
    const PassRecord& record = state.passes[pass];
    for (uint32 index = record.firstUse; index < record.firstUse + record.useCount; ++index) {
        const SubresourceUse& use = state.uses[index];
        const ResourceRecord& resource = state.resources[use.resource];
        SubresourceTrack& track = state.tracks[resource.firstTrack + use.subresource];

        // 앞 pass들과 한 상태로 합칠 수 있으면 그 구간을 넓힌다. 그러면 구간의 첫 pass에서 넓힌
        // 상태로 한 번만 바꾸고 이 pass에는 전이가 없다(읽기 → 읽기 전이 0).
        if (allowMerge && track.chain != GraphIndexNone &&
            CanMergeAcrossPasses(track.access, track.flags, use.access, use.flags)) {
            track.access |= use.access;
            state.chains[track.chain].after = track.access;
            continue;
        }

        ChainEntry entry;
        entry.pass = pass;
        entry.resource = use.resource;
        entry.subresource = use.subresource;
        entry.before = track.access;
        entry.after = use.access;
        entry.beforeFlags = track.flags;
        entry.afterFlags = use.flags;
        // 렌더 타깃 mip 0의 전이는 장치 BeginOffscreen이 낸다(attachment).
        entry.attachment = record.kind == GraphPassKind::Raster &&
                           record.target.kind == GraphTargetKind::Texture &&
                           record.target.texture.index == use.resource && use.subresource == 0;
        auto pushed = state.chains.TryPushBack(arena, entry);
        if (!pushed) {
            return pushed;
        }

        track.chain = static_cast<uint32>(state.chains.Size() - 1);
        track.access = use.access;
        track.flags = use.flags;
    }

    return Result<void>::Success();
}

Result<void> TrackFinalTransitions(FrameArena& arena, GraphState& state) noexcept {
    for (uint32 index = 0; index < state.resources.Size(); ++index) {
        const ResourceRecord& resource = state.resources[index];
        if (resource.finalAccess == ResourceAccess::Unknown) {
            continue;
        }

        for (uint32 subresource = 0; subresource < resource.subresources; ++subresource) {
            SubresourceTrack& track = state.tracks[resource.firstTrack + subresource];
            // 실행한 pass가 쓰지 않은 subresource는 처음 상태 그대로 둔다(장치 기록이 없을 수도
            // 있다).
            if (track.chain == GraphIndexNone) {
                continue;
            }

            ChainEntry entry;
            entry.pass = GraphIndexNone;
            entry.resource = index;
            entry.subresource = subresource;
            entry.before = track.access;
            entry.after = resource.finalAccess;
            entry.beforeFlags = track.flags;
            auto pushed = state.chains.TryPushBack(arena, entry);
            if (!pushed) {
                return pushed;
            }

            track.chain = static_cast<uint32>(state.chains.Size() - 1);
            track.access = resource.finalAccess;
            track.flags = GraphAccessFlags::None;
        }
    }

    return Result<void>::Success();
}

Result<void> CollectTransitions(FrameArena& arena, GraphState& state, usize first) noexcept {
    for (usize index = first; index < state.chains.Size(); ++index) {
        const ChainEntry& entry = state.chains[index];
        if (!IsTransitionRequired(entry.before, entry.beforeFlags, entry.after, entry.afterFlags)) {
            continue;
        }

        const GraphResourceKind kind = state.resources[entry.resource].kind;
        // 같은 pass·자원·전후 상태의 바로 다음 mip이면 앞 범위를 늘린다.
        if (!state.transitions.IsEmpty()) {
            GraphTransitionRecord& last = state.transitions.Back();
            const usize lastIndex = state.transitions.Size() - 1;
            if (lastIndex >= state.nextTransition && last.pass == entry.pass && last.kind == kind &&
                last.resource == entry.resource && last.before == entry.before &&
                last.after == entry.after && last.attachment == entry.attachment &&
                last.firstMip + last.mipCount == entry.subresource) {
                ++last.mipCount;
                continue;
            }
        }

        GraphTransitionRecord record;
        record.pass = entry.pass;
        record.kind = kind;
        record.resource = entry.resource;
        record.firstMip = entry.subresource;
        record.mipCount = 1;
        record.before = entry.before;
        record.after = entry.after;
        record.attachment = entry.attachment;
        auto pushed = state.transitions.TryPushBack(arena, record);
        if (!pushed) {
            return pushed;
        }
    }

    return Result<void>::Success();
}

Result<void> CompileGraph(FrameArena& arena, GraphState& state) noexcept {
    // export 자원의 마지막 생산자를 살린다(외부에서 결과를 읽는다).
    for (uint32 index = 0; index < state.resources.Size(); ++index) {
        const ResourceRecord& resource = state.resources[index];
        if (!resource.exported) {
            continue;
        }

        if (!resource.produced) {
            return Result<void>::Failure(
                RecordGraphError(state, ErrorCode::InvalidArgument, true,
                                 "'{}' is exported before any pass produced it", resource.name));
        }

        for (uint32 subresource = 0; subresource < resource.subresources; ++subresource) {
            const uint32 producer = state.tracks[resource.firstTrack + subresource].lastProducer;
            if (producer != GraphIndexNone) {
                auto uncull = UncullPass(arena, state, producer);
                if (!uncull) {
                    return uncull;
                }
            }
        }
    }

    // 수명: 남은 pass의 참조 수, 첫·마지막 사용 pass다.
    for (uint32 pass = 0; pass < state.passes.Size(); ++pass) {
        const PassRecord& record = state.passes[pass];
        if (record.culled) {
            ++state.stats.culledPasses;
            continue;
        }

        for (uint32 index = record.firstUse; index < record.firstUse + record.useCount; ++index) {
            ResourceRecord& resource = state.resources[state.uses[index].resource];
            ++resource.references;
            if (resource.firstPass == GraphIndexNone) {
                resource.firstPass = pass;
            }

            resource.lastPass = pass;
        }
    }

    for (ResourceRecord& resource : state.resources) {
        resource.released = !resource.imported && !resource.exported && resource.references != 0;
    }

    // 전이: 남은 pass를 선언 순서로 훑어 subresource 상태를 추적하고 합칠 수 있는 접근을 합친다.
    for (uint32 pass = 0; pass < state.passes.Size(); ++pass) {
        if (state.passes[pass].culled) {
            continue;
        }

        auto tracked = TrackPassTransitions(arena, state, pass, true);
        if (!tracked) {
            return tracked;
        }
    }

    auto finals = TrackFinalTransitions(arena, state);
    if (!finals) {
        return finals;
    }

    auto collected = CollectTransitions(arena, state, 0);
    if (!collected) {
        return collected;
    }

    state.compiled = true;
    return Result<void>::Success();
}

} // namespace rocks::render::detail
