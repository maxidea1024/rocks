#include <Core/Text/NamePool.h>
#include <World/Actor.generated.h>
#include <World/Actor.h>
#include <World/Component.generated.h>
#include <World/Component.h>
#include <World/World.h>

#include "WorldTags.h"

// 태그 값 도우미(Actor.cpp·Component.cpp가 쓴다)와 World 태그 찾기다(Docs/Design/EngineRuntime
// 31.4·31.5절).
namespace rocks {

namespace world_tags {

bool Contains(Span<const Name> tags, Name tag) noexcept {
    if (!tag.IsValid() || tag.IsNone()) {
        return false;
    }

    for (const Name value : tags) {
        if (value == tag) {
            return true;
        }
    }

    return false;
}

// tag가 registry의 이름 pool에서 발급한 None 아닌 토큰인지 본다.
Result<void> CheckToken(ObjectRegistry& registry, Name tag) noexcept {
    if (!tag.IsValid() || tag.IsNone()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    NamePool* pool = registry.Names();
    if (pool == nullptr) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    // 다른 pool·옛 토큰은 Acquire가 거부한다(StaleHandle). lease는 바로 놓는다(entry는 pool
    // 종료까지 산다).
    auto lease = pool->Acquire(tag);
    if (!lease) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<void>::Success();
}

Result<void> Add(Array<Name>& tags, ObjectRegistry& registry, Name tag) noexcept {
    if (auto checked = CheckToken(registry, tag); !checked) {
        return checked;
    }

    if (Contains(tags.View(), tag)) {
        return Result<void>::Success();
    }

    return tags.TryPushBack(tag);
}

Result<void> Remove(Array<Name>& tags, Name tag) noexcept {
    if (!tag.IsValid() || tag.IsNone()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 안정 제거(순서를 지킨다 — UE TArray::Remove).
    usize kept = 0;
    for (usize index = 0; index < tags.Size(); ++index) {
        if (!(tags[index] == tag)) {
            tags[kept++] = tags[index];
        }
    }

    while (tags.Size() > kept) {
        (void)tags.TryPopBack();
    }

    return Result<void>::Success();
}

Result<void> Set(Array<Name>& tags, ObjectRegistry& registry, Span<const Name> values) noexcept {
    for (const Name value : values) {
        if (auto checked = CheckToken(registry, value); !checked) {
            return checked;
        }
    }

    Array<Name> staged(tags.GetAllocator());
    if (auto reserved = staged.TryReserve(values.Size()); !reserved) {
        return reserved;
    }

    for (const Name value : values) {
        // 칸을 먼저 확보했으므로 실패하지 않는다.
        (void)staged.TryPushBack(value);
    }

    tags = Move(staged);
    return Result<void>::Success();
}

} // namespace world_tags

// --- World 찾기 ---

Result<void> World::FindActorsWithTag(rocks::Name tag, Array<ObjectHandle>& out) noexcept {
    out.Clear();
    if (!tag.IsValid() || tag.IsNone()) {
        return Result<void>::Success();
    }

    ObjectAccessScope scope(Access());
    for (const ActorRecord& record : actors_) {
        if (record.state == ActorState::PendingDestroy) {
            continue;
        }

        auto borrowed = objects_.Resolve<Actor>(Access(), record.handle);
        if (!borrowed) {
            continue;
        }

        auto actor = borrowed.Value().Get();
        if (!actor || !actor.Value()->HasTag(tag)) {
            continue;
        }

        if (auto pushed = out.TryPushBack(record.handle); !pushed) {
            out.Clear();
            return pushed;
        }
    }

    return Result<void>::Success();
}

Result<void> World::FindComponentsByTag(ObjectHandle actor, TypeId type, rocks::Name tag,
                                        Array<ObjectHandle>& out) noexcept {
    out.Clear();
    if (FindActorRecord(actor) == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (!tag.IsValid() || tag.IsNone()) {
        return Result<void>::Success();
    }

    ObjectAccessScope scope(Access());
    for (usize index = 0;; ++index) {
        const ComponentSlot slot = ComponentSlotAt(actor, index);
        if (index >= slot.count) {
            break;
        }

        if (slot.component.IsNull()) {
            continue;
        }

        if (!ComponentMatchesTag(slot.component, type, tag)) {
            continue;
        }

        if (auto pushed = out.TryPushBack(slot.component); !pushed) {
            out.Clear();
            return pushed;
        }
    }

    return Result<void>::Success();
}

bool World::ComponentMatchesTag(ObjectHandle component, TypeId type, rocks::Name tag) noexcept {
    // 한 번 빌려 수명 기록(state_ — World가 friend)과 태그를 함께 본다. 타입은 거를 때만 묻는다.
    auto borrowed = objects_.Resolve<Component>(Access(), component);
    if (!borrowed) {
        return false;
    }

    auto value = borrowed.Value().Get();
    if (!value || value.Value()->state_ == ComponentState::PendingDestroy ||
        !value.Value()->HasTag(tag)) {
        return false;
    }

    if (type == TypeId{}) {
        return true;
    }

    auto lease = objects_.AcquireType(Access(), component);
    return lease && lease.Value().IsA(type);
}

ObjectHandle World::FindComponentByTag(ObjectHandle actor, TypeId type, rocks::Name tag) noexcept {
    if (FindActorRecord(actor) == nullptr || !tag.IsValid() || tag.IsNone()) {
        return ObjectHandle{};
    }

    ObjectAccessScope scope(Access());
    for (usize index = 0;; ++index) {
        const ComponentSlot slot = ComponentSlotAt(actor, index);
        if (index >= slot.count) {
            return ObjectHandle{};
        }

        if (slot.component.IsNull()) {
            continue;
        }

        if (ComponentMatchesTag(slot.component, type, tag)) {
            return slot.component;
        }
    }
}

} // namespace rocks
