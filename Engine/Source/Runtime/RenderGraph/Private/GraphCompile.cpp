#include "RenderGraphInternal.h"

// 그래프 컴파일이다: 미사용 제거(culling), 참조 수 수명, 같은 그래프 안 재사용 계획, subresource
// 상태 추적과 전이 계산(Docs/Design/RenderBase.ko.md 7.1절 culling·컴파일·barrier·수명 행, 8절 RG2,
// UE RC/Private/RenderGraphBuilder.cpp:1199–1325, 3128–3175, 3632–3646, 3783–3928,
// RC/Private/RenderGraphResources.cpp:69–152).
namespace rocks::render::detail {

using graphics::ResourceAccess;

bool SameResourceKey(const ResourceRecord& left, const ResourceRecord& right) noexcept {
    if (left.kind != right.kind) {
        return false;
    }

    return left.kind == GraphResourceKind::Texture
               ? GraphResourcePool::SameTextureKey(left.textureDesc, right.textureDesc)
               : GraphResourcePool::SameBufferKey(left.bufferDesc, right.bufferDesc);
}

namespace {

// 장치 객체 자리를 새로 만들어 자원에 준다.
Result<void> NewPhysical(FrameArena& arena, GraphState& state, uint32 resource) noexcept {
    PhysicalResource physical;
    physical.owner = resource;
    physical.held = true;
    auto pushed = state.physicals.TryPushBack(arena, physical);
    if (!pushed) {
        return pushed;
    }

    state.resources[resource].physical = static_cast<uint32>(state.physicals.Size() - 1);
    return Result<void>::Success();
}

// 같은 그래프 안 재사용 계획이다(RG2: 수명이 겹치지 않는 같은 desc 자원이 장치 객체 하나를 쓴다).
// culling 뒤 남은 pass를 선언(=실행) 순서로 훑으며, pass p에서 처음 쓰는 자원은 p 앞에서 놓인 같은
// 키의 자리를 받고(없으면 새 자리), p에서 마지막으로 쓰는 자원은 p 뒤에 자리를 놓는다. 그래서 같은
// pass에서 쓰는 두 자원은 자리를 나누지 않는다. UE도 마지막 사용 pass에서 풀 자원을 놓고 뒤 pass의
// 할당이 같은 desc를 다시 받는다(RC/Private/RenderGraphBuilder.cpp:3128–3175, 3632–3646).
//
// 예외: import 자원은 자리가 없다. export 자원은 그래프 끝까지 놓지 않는다(소유가 넘어간다). 업로드
// 대상은 그래프 시작에 자리를 받아(업로드가 첫 pass 앞에서 실행된다) 앞 자원의 자리를 받지 않는다.
//
// 이어 받은 자원의 subresource 처음 상태는 Discard다: 첫 전이가 "이전 내용 무효"를 밝힌다(backend는
// Unknown처럼 장치가 추적한 상태에서 바꾼다). UE 풀 재사용은 앞 자원의 마지막 상태를 이어받고
// Discard는 transient aliasing에만 쓴다(RenderGraphBuilder.cpp:4060–4080).
Result<void> PlanResourceReuse(FrameArena& arena, GraphState& state) noexcept {
    for (uint32 index = 0; index < state.resources.Size(); ++index) {
        const ResourceRecord& resource = state.resources[index];
        const bool live = resource.references != 0 || resource.exported;
        if (!resource.imported && resource.uploadTarget && live) {
            auto created = NewPhysical(arena, state, index);
            if (!created) {
                return created;
            }
        }
    }

    for (uint32 pass = 0; pass < state.passes.Size(); ++pass) {
        const PassRecord& record = state.passes[pass];
        if (record.culled) {
            continue;
        }

        const uint32 endUse = record.firstUse + record.useCount;
        for (uint32 use = record.firstUse; use < endUse; ++use) {
            const uint32 index = state.uses[use].resource;
            ResourceRecord& resource = state.resources[index];
            if (resource.imported || resource.physical != GraphIndexNone) {
                continue;
            }

            uint32 found = GraphIndexNone;
            for (uint32 slot = 0; slot < state.physicals.Size(); ++slot) {
                const PhysicalResource& physical = state.physicals[slot];
                if (!physical.held && SameResourceKey(state.resources[physical.owner], resource)) {
                    found = slot;
                    break;
                }
            }

            if (found == GraphIndexNone) {
                auto created = NewPhysical(arena, state, index);
                if (!created) {
                    return created;
                }

                continue;
            }

            state.physicals[found].held = true;
            resource.physical = found;
            resource.reusedWithinGraph = true;
            for (uint32 subresource = 0; subresource < resource.subresources; ++subresource) {
                state.tracks[resource.firstTrack + subresource].access = ResourceAccess::Discard;
            }
        }

        // 이 pass가 마지막 사용인 자원의 자리를 놓는다(뒤 pass부터 받을 수 있다).
        for (uint32 use = record.firstUse; use < endUse; ++use) {
            const ResourceRecord& resource = state.resources[state.uses[use].resource];
            if (resource.imported || resource.exported || resource.lastPass != pass ||
                resource.physical == GraphIndexNone) {
                continue;
            }

            state.physicals[resource.physical].held = false;
        }
    }

    return Result<void>::Success();
}

} // namespace

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
        // Sampled 그래프 텍스처는 장치 생성에 mip 사슬 전체가 필요하다(내용은 업로드뿐이다).
        const bool live = resource.references != 0 || resource.exported;
        if (live && !resource.imported && resource.kind == GraphResourceKind::Texture &&
            resource.textureDesc.usage == graphics::TextureUsage::Sampled) {
            for (uint32 mip = 0; mip < resource.subresources; ++mip) {
                if (!state.tracks[resource.firstTrack + mip].uploaded) {
                    return Result<void>::Failure(RecordGraphError(
                        state, ErrorCode::InvalidArgument, true,
                        "sampled graph texture '{}' has no upload for mip {}", resource.name, mip));
                }
            }
        }
    }

    auto planned = PlanResourceReuse(arena, state);
    if (!planned) {
        return planned;
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
