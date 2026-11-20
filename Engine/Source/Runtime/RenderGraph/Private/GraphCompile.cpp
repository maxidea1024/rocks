#include "RenderGraphInternal.h"

// 그래프 컴파일이다: 미사용 제거(culling), 참조 수 수명, async compute fork·join과 수명 연장, 같은
// 그래프 안 재사용 계획, subresource 상태 추적과 전이 계산(Docs/Design/RenderBase.ko.md 7.1절
// culling·컴파일·barrier·수명 행, 8절 RG2·RG4, UE RC/Private/RenderGraphBuilder.cpp:1160–1325,
// 1562–1667, 3128–3175, 3632–3646, 3783–3928, RC/Private/RenderGraphResources.cpp:69–152).
namespace rocks::render::detail {

using graphics::ResourceAccess;

bool SamePipe(const GraphState& state, uint32 left, uint32 right) noexcept {
    if (left == GraphIndexNone || right == GraphIndexNone) {
        return true;
    }

    return state.passes[left].async == state.passes[right].async;
}

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

    // 실행과 같은 규칙으로 기록 경계를 정한다: Raster pass는 새 기록을 열고, 앞 Raster pass의
    // attachment에 접근하는 Compute·Copy pass는 그 기록을 닫고 attachment 없는 기록을 연다. async
    // compute(RG4): run의 첫 async pass는 compute 기록을 열고, pipe가 graphics로 돌아온 pass·fork
    // pass 다음 pass·join pass는 새 graphics 기록을 연다.
    uint32 recording = GraphIndexNone;
    uint32 previous = GraphIndexNone;
    for (uint32 pass = 0; pass < state.passes.Size(); ++pass) {
        PassRecord& record = state.passes[pass];
        if (record.culled) {
            continue;
        }

        const bool pipeChanged = previous != GraphIndexNone && !SamePipe(state, previous, pass);
        const bool afterFork = previous != GraphIndexNone && state.passes[previous].forks;
        previous = pass;
        record.opensRecording = false;
        if (record.async) {
            record.opensRecording = record.startsRun;
            recording = GraphIndexNone;
        } else if (record.kind == GraphPassKind::Raster) {
            record.opensRecording = true;
            recording = pass;
        } else if (pipeChanged || afterFork || record.joins) {
            record.opensRecording = true;
            recording = GraphIndexNone;
        } else if (recording != GraphIndexNone && UsesAttachmentsOf(state, pass, recording)) {
            record.opensRecording = true;
            recording = GraphIndexNone;
        }

        // 새 장치 기록이 열리면 앞 기록의 attachment였던 자리도 이제 받을 수 있다.
        if (record.opensRecording) {
            for (PhysicalResource& physical : state.physicals) {
                physical.waitsForRecording = false;
            }
        }

        const uint32 endUse = record.firstUse + record.useCount;
        const auto acquire = [&](uint32 index) noexcept -> Result<void> {
            ResourceRecord& resource = state.resources[index];
            // transient 자원은 장치 객체 자리가 아니라 heap 구간을 쓴다(RG5).
            if (resource.imported || resource.transient || resource.physical != GraphIndexNone) {
                return Result<void>::Success();
            }

            uint32 found = GraphIndexNone;
            for (uint32 slot = 0; slot < state.physicals.Size(); ++slot) {
                const PhysicalResource& physical = state.physicals[slot];
                if (!physical.held && !physical.waitsForRecording &&
                    SameResourceKey(state.resources[physical.owner], resource)) {
                    found = slot;
                    break;
                }
            }

            if (found == GraphIndexNone) {
                return NewPhysical(arena, state, index);
            }

            state.physicals[found].held = true;
            resource.physical = found;
            resource.reusedWithinGraph = true;
            for (uint32 subresource = 0; subresource < resource.subresources; ++subresource) {
                state.tracks[resource.firstTrack + subresource].access = ResourceAccess::Discard;
            }

            return Result<void>::Success();
        };
        for (uint32 use = record.firstUse; use < endUse; ++use) {
            auto acquired = acquire(state.uses[use].resource);
            if (!acquired) {
                return acquired;
            }
        }

        // async compute로 넓힌 수명(fork pass에서 시작하지만 그 pass는 쓰지 않는 자원)이다.
        if (state.hasAsyncPasses) {
            for (uint32 index = 0; index < state.resources.Size(); ++index) {
                if (state.resources[index].firstPass == pass) {
                    auto acquired = acquire(index);
                    if (!acquired) {
                        return acquired;
                    }
                }
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
            // 렌더 타깃이었던 객체는 그 장치 기록(attachment로 묶인 채)이 닫힌 뒤에 넘긴다(같은
            // 기록의 복사·compute가 attachment 객체를 쓰지 않게, 장치 CopyTexture 계약).
            state.physicals[resource.physical].waitsForRecording = resource.renderTarget;
        }

        // async compute로 넓힌 수명(join pass에서 끝나지만 그 pass는 쓰지 않는 자원)이다.
        if (state.hasAsyncPasses) {
            for (const ResourceRecord& resource : state.resources) {
                if (resource.imported || resource.exported || resource.lastPass != pass ||
                    resource.physical == GraphIndexNone) {
                    continue;
                }

                state.physicals[resource.physical].held = false;
                state.physicals[resource.physical].waitsForRecording = resource.renderTarget;
            }
        }
    }

    return Result<void>::Success();
}

// async compute 계획이다(RG4, UE RC/Private/RenderGraphBuilder.cpp:1160–1196, 1231–1235,
// 1562–1667과 같은 식).
// - pipe: 그래프가 compute queue를 쓰면 남은 AsyncCompute pass가 async다.
// - 다른 pipe 간선: subresource마다 마지막으로 접근한 pass를 따라, 다른 pipe의 앞 접근이 있으면
//   의존이다(읽기 → 읽기도 넣는다. 다른 pipe의 읽기 상태를 합치지 않으므로 그 사이에 전이 자리가
//   필요하다). async pass는 다른 pipe의 가장 늦은 생산자를, graphics 소비자는 그 async pass의 첫
//   소비자가 된다.
// - fork = max(다른 pipe 생산자, 앞 async pass의 fork, 그래프 시작), join = min(첫 소비자, 뒤 async
//   pass의 join, 그래프 끝). 그래서 둘 다 선언 순서로 늘기만 한다.
// - run: graphics pass 없이 이어진 async pass 묶음이 compute 기록 하나이고, run 전체가 마지막
// pass의
//   fork(가장 늦은 fork)를 기다린다.
// - 수명 연장: async pass가 쓰는 자원은 run의 fork pass(그래프 시작이면 첫 실행 pass)부터 join
//   pass(그래프 끝이면 마지막 실행 pass)까지 놓지 않는다.
Result<void> PlanAsyncCompute(FrameArena& arena, GraphState& state) noexcept {
    if (!state.asyncEnabled) {
        return Result<void>::Success();
    }

    uint32 firstLive = GraphIndexNone;
    uint32 lastLive = GraphIndexNone;
    for (uint32 pass = 0; pass < state.passes.Size(); ++pass) {
        PassRecord& record = state.passes[pass];
        record.async = !record.culled && record.kind == GraphPassKind::AsyncCompute;
        state.hasAsyncPasses = state.hasAsyncPasses || record.async;
        if (!record.culled) {
            firstLive = firstLive == GraphIndexNone ? pass : firstLive;
            lastLive = pass;
        }
    }

    if (!state.hasAsyncPasses) {
        return Result<void>::Success();
    }

    ArenaArray<uint32> lastAccess;
    ArenaArray<uint32> crossProducer;
    ArenaArray<uint32> firstConsumer;
    auto assigned = lastAccess.TryAssign(arena, state.tracks.Size(), GraphIndexNone);
    if (assigned) {
        assigned = crossProducer.TryAssign(arena, state.passes.Size(), GraphIndexNone);
    }

    if (assigned) {
        assigned = firstConsumer.TryAssign(arena, state.passes.Size(), GraphIndexNone);
    }

    if (!assigned) {
        return assigned;
    }

    for (uint32 pass = 0; pass < state.passes.Size(); ++pass) {
        const PassRecord& record = state.passes[pass];
        if (record.culled) {
            continue;
        }

        for (uint32 use = record.firstUse; use < record.firstUse + record.useCount; ++use) {
            const SubresourceUse& entry = state.uses[use];
            const uint32 track = state.resources[entry.resource].firstTrack + entry.subresource;
            const uint32 previous = lastAccess[track];
            lastAccess[track] = pass;
            if (previous == GraphIndexNone || previous == pass || SamePipe(state, previous, pass)) {
                continue;
            }

            if (record.async) {
                if (crossProducer[pass] == GraphIndexNone || previous > crossProducer[pass]) {
                    crossProducer[pass] = previous;
                }
            } else if (firstConsumer[previous] == GraphIndexNone) {
                // pass는 선언 순서로 늘므로 처음 정한 것이 첫 소비자다.
                firstConsumer[previous] = pass;
            }
        }
    }

    // fork(앞 → 뒤): 그래프 시작은 GraphIndexNone이다.
    uint32 fork = GraphIndexNone;
    for (uint32 pass = 0; pass < state.passes.Size(); ++pass) {
        PassRecord& record = state.passes[pass];
        if (!record.async) {
            continue;
        }

        const uint32 producer = crossProducer[pass];
        if (producer != GraphIndexNone && (fork == GraphIndexNone || producer > fork)) {
            fork = producer;
        }

        record.runFork = fork;
    }

    // join(뒤 → 앞): 그래프 끝은 GraphIndexNone(가장 큰 값)이다.
    uint32 join = GraphIndexNone;
    for (uint32 pass = static_cast<uint32>(state.passes.Size()); pass > 0; --pass) {
        PassRecord& record = state.passes[pass - 1];
        if (!record.async) {
            continue;
        }

        join = firstConsumer[pass - 1] < join ? firstConsumer[pass - 1] : join;
        record.joinPass = join;
    }

    // run: 이어진 async pass 묶음과 그 fork(마지막 pass의 fork)다.
    uint32 runStart = GraphIndexNone;
    uint32 runLast = GraphIndexNone;
    const auto closeRun = [&]() noexcept {
        if (runStart == GraphIndexNone) {
            return;
        }

        const uint32 runFork = state.passes[runLast].runFork;
        for (uint32 member = runStart; member <= runLast; ++member) {
            if (state.passes[member].async) {
                state.passes[member].runFork = runFork;
            }
        }

        state.passes[runStart].startsRun = true;
        if (runFork != GraphIndexNone) {
            state.passes[runFork].forks = true;
        }

        runStart = GraphIndexNone;
    };
    for (uint32 pass = 0; pass < state.passes.Size(); ++pass) {
        const PassRecord& record = state.passes[pass];
        if (record.culled) {
            continue;
        }

        if (!record.async) {
            closeRun();
            continue;
        }

        runStart = runStart == GraphIndexNone ? pass : runStart;
        runLast = pass;
    }

    closeRun();
    for (uint32 pass = 0; pass < state.passes.Size(); ++pass) {
        const PassRecord& record = state.passes[pass];
        if (!record.async) {
            continue;
        }

        if (record.joinPass != GraphIndexNone) {
            state.passes[record.joinPass].joins = true;
        }

        // 수명 연장이다.
        const uint32 start = record.runFork == GraphIndexNone ? firstLive : record.runFork;
        const uint32 end = record.joinPass == GraphIndexNone ? lastLive : record.joinPass;
        for (uint32 use = record.firstUse; use < record.firstUse + record.useCount; ++use) {
            ResourceRecord& resource = state.resources[state.uses[use].resource];
            resource.firstPass = start < resource.firstPass ? start : resource.firstPass;
            resource.lastPass = resource.lastPass == GraphIndexNone || end > resource.lastPass
                                    ? end
                                    : resource.lastPass;
        }
    }

    return Result<void>::Success();
}

// transient aliasing 계획이다(RG5, RenderBase 8절 "GX12·RG5 transient aliasing 설계", UE
// RHICore/Private/RHICoreTransientResourceAllocator.cpp:132–296과 같은 first fit).
// - 대상: 그래프가 만든 RenderTarget·DepthStencil·Storage 텍스처와 쓰기 용도 버퍼(import·export·
//   업로드 대상·Sampled 제외, asyncComputeAliasing이 꺼져 있으면 async pass가 쓰는 자원 제외).
//   장치가 placement를 주지 않으면 풀로 둔다.
// - 남은 pass를 선언 순서로 훑어 pass p에서 수명이 시작하는 자원을 할당하고, p에서 끝나는 자원을
//   p 뒤에 놓는다. 그래서 할당은 앞 pass들이 놓은 구간만 받는다(같은 queue 순서, async 자원은 RG4로
//   넓힌 fork·join 수명이라 graphics 순서 안이다).
// - heap 종류마다 heap 목록이 있고 빈 구간 목록(offset 순)을 처음부터 훑어 붙어 있는 구간을 이어
//   붙인다. 앞 자원이 놓은 구간을 받으면 aliasing이다. 맞는 자리가 없으면 그 종류의 heap을 하나
//   더 둔다(크기는 풀 설정의 transientHeapBytes와 자원 크기 가운데 큰 것).
// - 앞 자원이 놓은 구간을 이웃과 합치지 않는다(UE와 같다, 그래프 끝에 목록을 버린다).
Result<void> PlanTransientResources(FrameArena& arena, GraphState& state) noexcept {
    if (!state.transientEnabled) {
        return Result<void>::Success();
    }

    // async pass가 쓰는 자원 표시다.
    ArenaArray<uint8> usedByAsync;
    auto assigned = usedByAsync.TryAssign(arena, state.resources.Size(), 0);
    if (!assigned) {
        return assigned;
    }

    for (const PassRecord& record : state.passes) {
        // attachment resolve 대상은 BeginPass가 쓰는데 획득 자리(attachment acquire)가 없으므로
        // 풀로 둔다.
        for (uint32 color = 0; color < record.target.colorCount; ++color) {
            const GraphTexture resolve = record.target.colors[color].resolveTarget;
            if (!resolve.IsNull() && resolve.index < usedByAsync.Size()) {
                usedByAsync[resolve.index] = 2;
            }
        }

        if (!record.async) {
            continue;
        }

        for (uint32 use = record.firstUse; use < record.firstUse + record.useCount; ++use) {
            uint8& mark = usedByAsync[state.uses[use].resource];
            mark = mark == 2 ? mark : 1;
        }
    }

    ArenaArray<graphics::TransientPlacement> placements;
    assigned = placements.TryAssign(arena, state.resources.Size(), graphics::TransientPlacement{});
    if (!assigned) {
        return assigned;
    }

    for (uint32 index = 0; index < state.resources.Size(); ++index) {
        ResourceRecord& resource = state.resources[index];
        const bool live = resource.references != 0 && resource.firstPass != GraphIndexNone;
        // usedByAsync: 1은 async pass, 2는 resolve 대상(늘 제외)이다.
        bool eligible = live && !resource.imported && !resource.exported &&
                        !resource.uploadTarget && usedByAsync[index] != 2 &&
                        (state.asyncComputeAliasing || usedByAsync[index] == 0);
        if (eligible && resource.kind == GraphResourceKind::Texture) {
            eligible = resource.textureDesc.usage != graphics::TextureUsage::Sampled;
        } else if (eligible) {
            eligible = graphics::HasAnyBufferUsage(resource.bufferDesc.usage,
                                                   graphics::BufferUsage::ShaderWrite);
        }

        if (!eligible) {
            continue;
        }

        auto placement = resource.kind == GraphResourceKind::Texture
                             ? state.device->GetTexturePlacement(resource.textureDesc)
                             : state.device->GetBufferPlacement(resource.bufferDesc);
        if (!placement || placement.Value().bytes == 0 || placement.Value().alignment == 0) {
            continue;
        }

        placements[index] = placement.Value();
        resource.transient = true;
        resource.transientBytes = placement.Value().bytes;
    }

    const uint64 heapBytes = state.pool->Config().transientHeapBytes;
    const auto allocate = [&](uint32 index) noexcept -> Result<void> {
        ResourceRecord& resource = state.resources[index];
        const graphics::TransientPlacement& placement = placements[index];
        for (uint32 heap = 0; heap <= state.transientHeaps.Size(); ++heap) {
            if (heap == state.transientHeaps.Size()) {
                // 맞는 자리가 없다: 이 종류의 heap을 하나 더 둔다(빈 구간 하나).
                TransientHeapPlan plan;
                plan.kind = placement.kind;
                plan.capacity = placement.bytes > heapBytes ? placement.bytes : heapBytes;
                auto pushed = state.transientHeaps.TryPushBack(arena, plan);
                if (!pushed) {
                    return pushed;
                }

                TransientRange whole;
                whole.heap = heap;
                whole.size = plan.capacity;
                pushed = state.transientRanges.TryPushBack(arena, whole);
                if (!pushed) {
                    return pushed;
                }
            }

            if (state.transientHeaps[heap].kind != placement.kind) {
                continue;
            }

            // 이 heap의 빈 구간을 offset 순으로 훑어 붙어 있는 구간을 이어 붙인다.
            usize runFirst = GraphIndexNone;
            uint64 runBegin = 0;
            uint64 allocBegin = 0;
            uint64 allocEnd = 0;
            uint64 next = 0;
            for (usize range = 0; range < state.transientRanges.Size(); ++range) {
                const TransientRange& candidate = state.transientRanges[range];
                if (candidate.heap != heap) {
                    continue;
                }

                const uint64 candidateEnd = candidate.offset + candidate.size;
                if (runFirst == GraphIndexNone || candidate.offset != next) {
                    const uint64 aligned = (candidate.offset + placement.alignment - 1) /
                                           placement.alignment * placement.alignment;
                    if (aligned >= candidateEnd) {
                        runFirst = GraphIndexNone;
                        continue;
                    }

                    runFirst = range;
                    runBegin = candidate.offset;
                    allocBegin = aligned;
                    allocEnd = aligned + placement.bytes;
                }

                next = candidateEnd;
                if (allocEnd > candidateEnd) {
                    continue;
                }

                // 자리를 찾았다: [runFirst, range] 구간을 쓰고 마지막 구간의 남은 끝을 남긴다.
                bool aliased = false;
                usize removed = 0;
                for (usize used = runFirst; used <= range; ++used) {
                    TransientRange& taken = state.transientRanges[used - removed];
                    if (taken.heap != heap) {
                        continue;
                    }

                    aliased = aliased || taken.resource != GraphIndexNone;
                    if (used == range && candidateEnd > allocEnd) {
                        taken.offset = allocEnd;
                        taken.size = candidateEnd - allocEnd;
                        taken.resource = GraphIndexNone;
                        continue;
                    }

                    for (usize shift = used - removed; shift + 1 < state.transientRanges.Size();
                         ++shift) {
                        state.transientRanges[shift] = state.transientRanges[shift + 1];
                    }

                    state.transientRanges.PopBack();
                    ++removed;
                }

                resource.transientHeap = heap;
                resource.transientOffset = allocBegin;
                resource.regionBegin = runBegin;
                resource.regionEnd = allocEnd;
                TransientHeapPlan& plan = state.transientHeaps[heap];
                plan.highWater = allocEnd > plan.highWater ? allocEnd : plan.highWater;
                ++state.stats.transientResources;
                state.stats.transientBytes += placement.bytes;
                state.stats.aliasedResources += aliased ? 1u : 0u;
                return Result<void>::Success();
            }
        }

        return Result<void>::Failure(ErrorCode::OutOfMemory);
    };
    const auto release = [&](uint32 index) noexcept -> Result<void> {
        const ResourceRecord& resource = state.resources[index];
        TransientRange freed;
        freed.heap = resource.transientHeap;
        freed.offset = resource.regionBegin;
        freed.size = resource.regionEnd - resource.regionBegin;
        freed.resource = index;
        // offset 순 자리에 끼운다(같은 heap 안에서).
        usize at = state.transientRanges.Size();
        for (usize range = 0; range < state.transientRanges.Size(); ++range) {
            const TransientRange& existing = state.transientRanges[range];
            if (existing.heap == freed.heap && existing.offset > freed.offset) {
                at = range;
                break;
            }
        }

        auto pushed = state.transientRanges.TryPushBack(arena, freed);
        if (!pushed) {
            return pushed;
        }

        for (usize shift = state.transientRanges.Size() - 1; shift > at; --shift) {
            state.transientRanges[shift] = state.transientRanges[shift - 1];
        }

        state.transientRanges[at] = freed;
        return Result<void>::Success();
    };

    for (uint32 pass = 0; pass < state.passes.Size(); ++pass) {
        if (state.passes[pass].culled) {
            continue;
        }

        for (uint32 index = 0; index < state.resources.Size(); ++index) {
            if (state.resources[index].transient && state.resources[index].firstPass == pass) {
                auto allocated = allocate(index);
                if (!allocated) {
                    return allocated;
                }
            }
        }

        for (uint32 index = 0; index < state.resources.Size(); ++index) {
            if (state.resources[index].transient && state.resources[index].lastPass == pass) {
                auto released = release(index);
                if (!released) {
                    return released;
                }
            }
        }
    }

    for (const TransientHeapPlan& plan : state.transientHeaps) {
        state.stats.transientPeakBytes += plan.highWater;
    }

    state.stats.transientHeaps = static_cast<uint32>(state.transientHeaps.Size());
    // transient 자원의 subresource는 첫 전이가 Discard(획득)다.
    for (const ResourceRecord& resource : state.resources) {
        if (!resource.transient) {
            continue;
        }

        for (uint32 subresource = 0; subresource < resource.subresources; ++subresource) {
            state.tracks[resource.firstTrack + subresource].access = ResourceAccess::Discard;
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
        const uint32 previousUser = track.lastUser;
        track.lastUser = pass;
        if (allowMerge && track.chain != GraphIndexNone && SamePipe(state, previousUser, pass) &&
            CanMergeAcrossPasses(track.access, track.flags, use.access, use.flags)) {
            track.access |= use.access;
            state.chains[track.chain].after = track.access;
            continue;
        }

        ChainEntry entry;
        entry.previousUser = previousUser;
        entry.pass = pass;
        entry.resource = use.resource;
        entry.subresource = use.subresource;
        entry.before = track.access;
        entry.after = use.access;
        entry.beforeFlags = track.flags;
        entry.afterFlags = use.flags;
        // attachment subresource의 전이는 장치 BeginPass가 낸다.
        entry.attachment = use.attachment;
        // transient 자원의 첫 전이는 aliasing 획득이다(RG5).
        entry.acquire = resource.transient && previousUser == GraphIndexNone;
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

namespace {

// 분리 전이의 begin pass다(나누지 않으면 None). 앞 구간을 마지막으로 쓴 pass(previous)의
// epilogue에서 begin하고 pass의 prologue에서 end한다. 나누는 조건: 그래프 안 전이(attachment·그래프
// 끝 아님), 앞 사용자가 있음(첫 사용은 앞 상태가 장치 추적 상태라 begin 자리가 없다), 그 사이에
// 실행되는 pass가 하나 이상 있음(바로 다음이면 나눌 이득이 없다), begin과 end가 같은 장치 기록
// 안(Raster pass는 새 기록을 열므로 (previous, pass] 안에 Raster pass가 없어야 한다). UE도 같은
// pipe에서 생산 pass epilogue begin, 소비 pass prologue
// end다(RC/Private/RenderGraphBuilder.cpp:3783–3928).
uint32 SplitBeginPass(const GraphState& state, const ChainEntry& entry) noexcept {
    // 다른 pipe 사이 전이는 나누지 않는다(begin·end가 다른 queue의 기록이다, RG4).
    if (!state.splitEnabled || entry.attachment || entry.pass == GraphIndexNone ||
        entry.previousUser == GraphIndexNone || entry.previousUser >= entry.pass ||
        !SamePipe(state, entry.previousUser, entry.pass)) {
        return GraphIndexNone;
    }

    bool between = false;
    for (uint32 pass = entry.previousUser + 1; pass <= entry.pass; ++pass) {
        const PassRecord& record = state.passes[pass];
        if (record.culled) {
            continue;
        }

        if (record.opensRecording) {
            return GraphIndexNone;
        }

        between = between || pass != entry.pass;
    }

    return between ? entry.previousUser : GraphIndexNone;
}

// record와 entry가 slice·mip 범위 말고는 같은 전이인지다.
bool SameTransitionGroup(const GraphTransitionRecord& record, const ChainEntry& entry,
                         GraphResourceKind kind, uint32 beginPass, uint32 forkPass) noexcept {
    return record.pass == entry.pass && record.kind == kind && record.resource == entry.resource &&
           record.before == entry.before && record.after == entry.after &&
           record.attachment == entry.attachment && record.beginPass == beginPass &&
           record.forkPass == forkPass && record.acquire == entry.acquire;
}

// graphics 쪽에서 내야 하는 async compute 전이의 fork 자리다(RG4): async pass의 전이 가운데 앞
// 사용자가 graphics pass이거나 없는(그래프 처음 사용) 것이다. compute 기록은 graphics 상태
// barrier를 낼 수 없으므로 run이 기다릴 fork pass의 epilogue(없으면 그래프 시작 기록)에서 낸다. 그
// 밖은 GraphIndexNone이다.
uint32 ForkPassOf(const GraphState& state, const ChainEntry& entry) noexcept {
    if (entry.pass == GraphIndexNone || !state.passes[entry.pass].async ||
        (entry.previousUser != GraphIndexNone && state.passes[entry.previousUser].async)) {
        return GraphIndexNone;
    }

    const uint32 fork = state.passes[entry.pass].runFork;
    return fork == GraphIndexNone ? GraphProloguePass : fork;
}

// 마지막 두 record가 같은 mip 범위의 이웃 slice 범위이고 나머지가 같으면 하나로 합친다. 새
// record를 넣기 전과 수집 끝에 불러 slice마다 모은 mip 범위를 slice 범위로 묶는다.
void MergeSliceRuns(GraphState& state) noexcept {
    const usize size = state.transitions.Size();
    if (size < 2 || size - 2 < state.nextTransition) {
        return;
    }

    GraphTransitionRecord& previous = state.transitions[size - 2];
    const GraphTransitionRecord& last = state.transitions[size - 1];
    if (previous.pass == last.pass && previous.kind == last.kind &&
        previous.resource == last.resource && previous.before == last.before &&
        previous.after == last.after && previous.attachment == last.attachment &&
        previous.beginPass == last.beginPass && previous.forkPass == last.forkPass &&
        previous.acquire == last.acquire && previous.firstMip == last.firstMip &&
        previous.mipCount == last.mipCount &&
        previous.firstSlice + previous.sliceCount == last.firstSlice) {
        previous.sliceCount += last.sliceCount;
        state.transitions.PopBack();
    }
}

} // namespace

Result<void> CollectTransitions(FrameArena& arena, GraphState& state, usize first) noexcept {
    for (usize index = first; index < state.chains.Size(); ++index) {
        const ChainEntry& entry = state.chains[index];
        if (!IsTransitionRequired(entry.before, entry.beforeFlags, entry.after, entry.afterFlags)) {
            continue;
        }

        const GraphResourceKind kind = state.resources[entry.resource].kind;
        const uint32 beginPass = SplitBeginPass(state, entry);
        const uint32 forkPass = ForkPassOf(state, entry);
        const ResourceRecord& resource = state.resources[entry.resource];
        const uint32 mip = entry.subresource % resource.mipLevels;
        const uint32 slice = entry.subresource / resource.mipLevels;
        // 같은 pass·자원·전후 상태·begin 자리의 같은 slice 바로 다음 mip이면 앞 범위를 늘린다.
        if (!state.transitions.IsEmpty()) {
            GraphTransitionRecord& last = state.transitions.Back();
            const usize lastIndex = state.transitions.Size() - 1;
            if (lastIndex >= state.nextTransition &&
                SameTransitionGroup(last, entry, kind, beginPass, forkPass) &&
                last.sliceCount == 1 && last.firstSlice == slice &&
                last.firstMip + last.mipCount == mip) {
                ++last.mipCount;
                continue;
            }
        }

        // 새 record를 넣기 전에 앞에서 다 모은 slice의 mip 범위를 그 앞 slice 범위와 합친다.
        GraphTransitionRecord record;
        record.pass = entry.pass;
        record.kind = kind;
        record.resource = entry.resource;
        record.firstMip = mip;
        record.mipCount = 1;
        record.firstSlice = slice;
        record.sliceCount = 1;
        record.before = entry.before;
        record.after = entry.after;
        record.attachment = entry.attachment;
        record.beginPass = beginPass;
        record.forkPass = forkPass;
        record.acquire = entry.acquire;
        MergeSliceRuns(state);
        auto pushed = state.transitions.TryPushBack(arena, record);
        if (!pushed) {
            return pushed;
        }
    }

    MergeSliceRuns(state);
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

    auto async = PlanAsyncCompute(arena, state);
    if (!async) {
        return async;
    }

    auto transient = PlanTransientResources(arena, state);
    if (!transient) {
        return transient;
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
