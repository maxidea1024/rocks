#include "WorldInternal.h"

#include <Core/Containers/Array.h>
#include <Core/Containers/HashMap.h>
#include <Core/Containers/InsertOutcome.h>
#include <Platform/Fatal.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Level.generated.h>
#include <World/SceneComponent.generated.h>
#include <World/World.h>

// 레벨 편입이다(Docs/Design/EngineRuntime 29절): 로드한 Level 그래프를 장부에 넣고 수명 단계로
// 진행한다. 순서: 검증(바꾸지 않음) → 용량 → 장부·타입 규칙(실패하면 되돌림) → 정리 → 부착
// 재구성 → 수명.
namespace rocks {

namespace {

// 편입할 Component 하나다.
struct AdoptedComponent {
    ObjectHandle handle;
    ObjectHandle actor;
    bool scene = false;
    bool staticMobility = false;
    // 저장된 부착 부모다(검증 뒤 — 끊으면 null).
    ObjectHandle parent;
};

// 편입할 Actor 하나다. components의 [firstComponent, firstComponent + componentCount)가 이 Actor
// 것이다(검증이 Actor마다 연속으로 넣는다).
struct AdoptedActor {
    ObjectHandle handle;
    usize firstComponent = 0;
    usize componentCount = 0;
    // components 배열에 null이 있었다(순서를 지키며 뺀다).
    bool hasNull = false;
    // rootComponent가 components 밖이다(비운다).
    bool clearRoot = false;
};

} // namespace

Result<LevelAdoption> World::AdoptLevel(RootRef<Object>&& loaded) noexcept {
    RootRef<Object> root = Move(loaded);
    if (!CanMutate()) {
        return Result<LevelAdoption>::Failure(
            state_ == WorldState::Created ? ErrorCode::InvalidArgument : ErrorCode::ShuttingDown);
    }

    if (hookDepth_ != 0) {
        return Result<LevelAdoption>::Failure(ErrorCode::Busy);
    }

    const ObjectHandle level = root.Handle();
    ObjectAccessScope scope(Access());
    auto unadopted =
        With<Level>(level, [](Level& value) noexcept { return value.world_ == nullptr; });
    if (!unadopted) {
        return Result<LevelAdoption>::Failure(unadopted.Error());
    }

    if (!unadopted.Value() || FindLevelRecord(level) != nullptr) {
        return Result<LevelAdoption>::Failure(ErrorCode::AlreadyExists);
    }

    LevelAdoption report;
    report.level = level;

    // --- 1. 검증(바꾸지 않는다 — 29.2절 2) ---
    Array<AdoptedActor> actors(allocator_);
    Array<AdoptedComponent> components(allocator_);
    // slot → actors·components 색인이다(중복·부착 부모 조회).
    HashMap<uint32, uint32> actorSlots(allocator_);
    HashMap<uint32, uint32> componentIndex(allocator_);
    usize levelNulls = 0;
    {
        Array<ObjectHandle> listed(allocator_);
        auto copied = With<Level>(
            level, [&](Level& value) noexcept { return listed.TryAppend(value.Actors()); });
        if (!copied) {
            return Result<LevelAdoption>::Failure(copied.Error());
        }

        for (const ObjectHandle actor : listed) {
            if (actor.IsNull()) {
                ++levelNulls;
                continue;
            }

            // 한 Actor가 배열에 두 번 있으면 장부가 둘로 갈라진다(29.2절 2).
            auto seen = actorSlots.Insert(actor.slot, static_cast<uint32>(actors.Size()));
            if (!seen) {
                return Result<LevelAdoption>::Failure(seen.Error());
            }

            if (seen.Value() != InsertOutcome::Inserted) {
                return Result<LevelAdoption>::Failure(ErrorCode::InvalidArgument);
            }

            AdoptedActor entry;
            entry.handle = actor;
            entry.firstComponent = components.Size();
            Array<ObjectHandle> owned(allocator_);
            ObjectHandle rootComponent;
            auto read = With<Actor>(actor, [&](Actor& value) noexcept -> Result<void> {
                if (value.world_ != nullptr) {
                    return Result<void>::Failure(ErrorCode::InvalidArgument);
                }

                rootComponent = value.RootComponent();
                return owned.TryAppend(value.Components());
            });
            if (!read) {
                return Result<LevelAdoption>::Failure(read.Error() == ErrorCode::OutOfMemory
                                                          ? ErrorCode::OutOfMemory
                                                          : ErrorCode::InvalidArgument);
            }

            entry.clearRoot = !rootComponent.IsNull();
            for (const ObjectHandle component : owned) {
                if (component.IsNull()) {
                    entry.hasNull = true;
                    ++report.droppedEntries;
                    continue;
                }

                if (component == rootComponent) {
                    entry.clearRoot = false;
                }

                AdoptedComponent adopted;
                adopted.handle = component;
                adopted.actor = actor;
                auto checked = With<Component>(
                    component, [](Component& value) noexcept { return value.world_ == nullptr; });
                if (!checked || !checked.Value()) {
                    return Result<LevelAdoption>::Failure(ErrorCode::InvalidArgument);
                }

                auto scene = With<SceneComponent>(component, [&](SceneComponent& value) noexcept {
                    adopted.parent = value.attachParent_.Handle();
                    adopted.staticMobility = value.mobility_ == ComponentMobility::Static;
                    return value.relative_.IsValid();
                });
                if (scene) {
                    adopted.scene = true;
                    if (!scene.Value()) {
                        return Result<LevelAdoption>::Failure(ErrorCode::InvalidArgument);
                    }
                }

                auto inserted =
                    componentIndex.Insert(component.slot, static_cast<uint32>(components.Size()));
                if (!inserted) {
                    return Result<LevelAdoption>::Failure(inserted.Error());
                }

                // 한 Component가 두 번 나온다(두 Actor 또는 한 Actor에 두 번).
                if (inserted.Value() != InsertOutcome::Inserted) {
                    return Result<LevelAdoption>::Failure(ErrorCode::InvalidArgument);
                }

                if (auto pushed = components.TryPushBack(adopted); !pushed) {
                    return Result<LevelAdoption>::Failure(pushed.Error());
                }
            }

            entry.componentCount = components.Size() - entry.firstComponent;
            if (entry.clearRoot) {
                ++report.droppedEntries;
            }

            if (auto pushed = actors.TryPushBack(entry); !pushed) {
                return Result<LevelAdoption>::Failure(pushed.Error());
            }
        }
    }

    report.droppedEntries += static_cast<uint32>(levelNulls);

    // 부착 부모가 이 Level의 SceneComponent가 아니거나 Static 자식이 Static이 아닌 부모에 붙었으면
    // 끊는다(29.2절 2 — ERD59, UE AttachToComponent의 Mobility 규칙). 순환은 거부한다.
    for (AdoptedComponent& component : components) {
        if (component.parent.IsNull()) {
            continue;
        }

        const uint32* parentIndex = componentIndex.Find(component.parent.slot);
        const bool foreign = !component.scene || parentIndex == nullptr ||
                             components[*parentIndex].handle != component.parent ||
                             !components[*parentIndex].scene;
        const bool mobilityViolation =
            !foreign && component.staticMobility && !components[*parentIndex].staticMobility;
        if (foreign || mobilityViolation) {
            component.parent = ObjectHandle{};
            ++report.droppedEntries;
        }
    }

    for (const AdoptedComponent& component : components) {
        ObjectHandle current = component.parent;
        for (uint32 depth = 0; !current.IsNull(); ++depth) {
            if (current == component.handle || depth >= SceneComponent::MaxHierarchyDepth) {
                return Result<LevelAdoption>::Failure(ErrorCode::InvalidArgument);
            }

            const uint32* next = componentIndex.Find(current.slot);
            current = next == nullptr ? ObjectHandle{} : components[*next].parent;
        }
    }

    // --- 2. 용량(뒤 단계가 할당 실패로 멈추지 않게 — 29.2절 4) ---
    const usize actorTotal = actors_.Size() + actors.Size();
    const usize componentTotal = componentCount_ + components.Size();
    if (!levels_.TryEnsureCapacity(levels_.Size() + 1) ||
        !pendingStart_.TryEnsureCapacity(actorTotal) ||
        !destroyActors_.TryEnsureCapacity(actorTotal) ||
        !destroyComponents_.TryEnsureCapacity(componentTotal) ||
        !pendingComponentStart_.TryEnsureCapacity(componentTotal)) {
        return Result<LevelAdoption>::Failure(ErrorCode::OutOfMemory);
    }

    // 부모마다 자식 수를 세어 자식 목록 용량을 먼저 늘린다(부착 재구성이 실패하지 않게).
    HashMap<uint32, uint32> childCounts(allocator_);
    for (const AdoptedComponent& component : components) {
        if (component.parent.IsNull()) {
            continue;
        }

        if (uint32* count = childCounts.Find(component.parent.slot)) {
            ++*count;
        } else if (auto inserted = childCounts.Insert(component.parent.slot, 1u); !inserted) {
            return Result<LevelAdoption>::Failure(inserted.Error());
        }
    }

    for (const AdoptedComponent& component : components) {
        const uint32* count = childCounts.Find(component.handle.slot);
        if (count == nullptr) {
            continue;
        }

        auto reserved = With<SceneComponent>(component.handle, [&](SceneComponent& value) noexcept {
            return value.children_.TryEnsureCapacity(value.children_.Size() + *count);
        });
        if (!reserved) {
            return Result<LevelAdoption>::Failure(reserved.Error());
        }
    }

    // --- 3. 장부·타입 규칙(29.2절 4 — 실패하면 되돌려 아무것도 바꾸지 않는다) ---
    auto pinned = objects_.TryPin<Level>(Access(), level);
    if (!pinned) {
        return Result<LevelAdoption>::Failure(pinned.Error());
    }

    if (auto indexed = levelSlots_.Insert(level.slot, 0); !indexed) {
        return Result<LevelAdoption>::Failure(indexed.Error());
    }

    // 용량을 위에서 확보했으므로 실패하지 않는다.
    auto levelEntry = levels_.TryEmplaceBack();
    levelEntry.Value()->root = Move(pinned.Value());
    levelEntry.Value()->handle = level;
    root.Reset();
    (void)With<Level>(level, [this](Level& value) noexcept {
        value.world_ = this;
        value.persistent_ = false;
    });

    // 레코드를 되돌린다. Level 배열에 레코드 없는 Actor가 남으면 파괴 경로가 Fatal이므로
    // DestroyLevel을 쓰지 않는다. 정리(4단계)는 아직 하지 않았으므로 그래프는 그대로다.
    Array<SlotHandle> createdSlots(allocator_);
    auto rollback = [&](ErrorCode error) noexcept {
        for (const AdoptedComponent& component : components) {
            if (componentRecords_.Erase(component.handle.slot)) {
                --componentCount_;
            }

            (void)With<Component>(component.handle, [](Component& value) noexcept {
                value.world_ = nullptr;
                value.owner_ = ObjectHandle{};
            });
        }

        for (const AdoptedActor& actor : actors) {
            actorIndex_.Erase(actor.handle.slot);
            (void)With<Actor>(actor.handle, [](Actor& value) noexcept {
                value.world_ = nullptr;
                value.level_ = ObjectHandle{};
            });
        }

        for (const SlotHandle slot : createdSlots) {
            (void)actors_.TryRemove(slot);
        }

        (void)With<Level>(level, [](Level& value) noexcept { value.world_ = nullptr; });
        levelSlots_.Erase(level.slot);
        (void)levels_.TryRemoveAt(levels_.Size() - 1);
        return Result<LevelAdoption>::Failure(error);
    };

    if (!createdSlots.TryEnsureCapacity(actors.Size())) {
        return rollback(ErrorCode::OutOfMemory);
    }

    for (usize position = 0; position < actors.Size(); ++position) {
        const AdoptedActor& actor = actors[position];
        auto slot = actors_.TryEmplace(allocator_);
        if (!slot) {
            return rollback(slot.Error());
        }

        (void)createdSlots.TryPushBack(slot.Value());
        auto indexed = actorIndex_.Insert(actor.handle.slot, slot.Value());
        if (!indexed || indexed.Value() != InsertOutcome::Inserted) {
            return rollback(indexed ? ErrorCode::InvalidArgument : indexed.Error());
        }

        ActorRecord& record = *actors_.Find(slot.Value());
        record.handle = actor.handle;
        record.level = level;
        // 정리는 순서를 지키므로 이 자리가 정리 뒤 Level 배열의 자리다.
        record.levelPosition = static_cast<uint32>(position);
        record.spawnOrder = ++spawnedTotal_;
        if (!record.components.TryEnsureCapacity(actor.componentCount)) {
            return rollback(ErrorCode::OutOfMemory);
        }

        (void)With<Actor>(actor.handle, [this, level](Actor& value) noexcept {
            value.world_ = this;
            value.level_ = level;
        });
        FindLevelRecord(level)->actorCount += 1;

        for (usize index = 0; index < actor.componentCount; ++index) {
            const AdoptedComponent& component = components[actor.firstComponent + index];
            ComponentRecord entry;
            entry.component = component.handle;
            entry.actor = actor.handle;
            auto inserted = componentRecords_.Insert(component.handle.slot, entry);
            if (!inserted || inserted.Value() != InsertOutcome::Inserted) {
                return rollback(inserted ? ErrorCode::InvalidArgument : inserted.Error());
            }

            ++componentCount_;
            // 용량을 위에서 확보했다.
            (void)actors_.Find(slot.Value())->components.TryPushBack(component.handle);
            (void)With<Component>(component.handle, [this, &actor](Component& value) noexcept {
                value.world_ = this;
                value.owner_ = actor.handle;
            });
        }
    }

    // 타입 규칙·EditorOnly(23.2절)는 Actor의 Component가 다 들어간 뒤에 본다.
    for (const AdoptedComponent& component : components) {
        if (auto allowed = CheckComponentRules(component.actor, component.handle); !allowed) {
            return rollback(allowed.Error());
        }
    }

    report.actors = static_cast<uint32>(actors.Size());

    // --- 4. 정리(29.2절 3 — 순서를 지킨다) ---
    // 관찰자 없는 World registry의 참조 API는 할당하지 않아 여기서는 실패하지 않는다. 실패하면
    // 장부가 그래프와 어긋나므로 Fatal로 막는다.
    if (levelNulls != 0) {
        auto compacted = With<Level>(level, [&](Level& value) noexcept -> Result<void> {
            for (usize removed = 0; removed < levelNulls; ++removed) {
                if (auto erased =
                        objects_.RemoveReference(Access(), level, value.actors_, ObjectHandle{});
                    !erased) {
                    return erased;
                }
            }

            return Result<void>::Success();
        });
        if (!compacted) {
            platform::Fatal("Level adoption could not remove a null actor entry");
        }
    }

    for (const AdoptedActor& actor : actors) {
        if (!actor.clearRoot && !actor.hasNull) {
            continue;
        }

        auto cleaned = With<Actor>(actor.handle, [&](Actor& value) noexcept -> Result<void> {
            if (actor.clearRoot) {
                if (auto cleared = objects_.SetReference(Access(), actor.handle,
                                                         value.rootComponent_, ObjectHandle{});
                    !cleared) {
                    return cleared;
                }
            }

            while (actor.hasNull) {
                auto removed = objects_.RemoveReference(Access(), actor.handle, value.components_,
                                                        ObjectHandle{});
                if (!removed) {
                    if (removed.Error() == ErrorCode::NotFound) {
                        break;
                    }

                    return removed;
                }
            }

            return Result<void>::Success();
        });
        if (!cleaned) {
            platform::Fatal("Level adoption could not clean an actor component list");
        }
    }

    // 끊은 부착(그리고 부모 없이 socket만 남은 손상)을 비운다.
    for (const AdoptedComponent& component : components) {
        if (!component.scene || !component.parent.IsNull()) {
            continue;
        }

        (void)With<SceneComponent>(component.handle, [](SceneComponent& value) noexcept {
            if (!value.attachParent_.Handle().IsNull() || !value.attachSocket_.IsEmpty()) {
                value.attachParent_ = WeakObjectRef<SceneComponent>();
                value.attachSocket_.Clear();
            }
        });
    }

    // --- 5. 부착 재구성(29.2절 5 — Level 배열 → components 순) ---
    for (const AdoptedComponent& component : components) {
        if (component.parent.IsNull()) {
            continue;
        }

        (void)With<SceneComponent>(component.handle, [&](SceneComponent& value) noexcept {
            value.parent_ = component.parent;
        });
        // 용량은 위에서 확보했다.
        (void)With<SceneComponent>(component.parent, [&](SceneComponent& value) noexcept {
            return value.AddChild(component.handle);
        });
        UpdateAttachmentTicks(component.handle, ObjectHandle{}, component.parent);
    }

    for (const AdoptedComponent& component : components) {
        if (component.scene) {
            (void)With<SceneComponent>(component.handle,
                                       [](SceneComponent& value) noexcept { value.MarkDirty(); });
        }
    }

    // --- 6. 수명(29.2절 6 — Level 배열 순서) ---
    // 앞 Actor의 hook이 뒤 Actor를 파괴하거나 직접 진행했으면 건너뛴다(실패로 세지 않는다).
    for (const AdoptedActor& actor : actors) {
        const ActorRecord* record = FindActorRecord(actor.handle);
        if (record == nullptr || record->state != ActorState::Constructed) {
            continue;
        }

        if (!FinishSpawning(actor.handle)) {
            ++report.failedActors;
        }
    }

    return Result<LevelAdoption>::Success(report);
}

} // namespace rocks
