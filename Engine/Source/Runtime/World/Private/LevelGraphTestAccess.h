#pragma once

#include <Core/Math/Transform.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <World/Actor.h>
#include <World/Level.h>
#include <World/SceneComponent.h>
#include <World/World.h>

// 시험이 편입 전(장부 밖) Level 그래프를 손상시키는 모듈 내부 접근점이다(Docs/Design/EngineRuntime
// 29.3절 — null 원소·중복 Actor·root 밖·잘못된 부착·잘못된 Transform). 공개 API로는 이런 그래프를
// 만들 수 없으므로 Tests/ 소스만 include한다(정책 검사 test-access-include).
namespace rocks::detail {

struct LevelGraphTestAccess {
    // Level actors 끝에 actor(null이면 null 원소)를 더한다.
    static Result<void> AppendActor(World& world, ObjectHandle level, ObjectHandle actor) noexcept {
        return world.With<Level>(level, [&](Level& value) noexcept {
            return world.Objects().AddReference(world.Access(), level, value.actors_, actor);
        });
    }

    // Actor components 끝에 component(null이면 null 원소)를 더한다.
    static Result<void> AppendComponent(World& world, ObjectHandle actor,
                                        ObjectHandle component) noexcept {
        return world.With<Actor>(actor, [&](Actor& value) noexcept {
            return world.Objects().AddReference(world.Access(), actor, value.components_,
                                                component);
        });
    }

    // rootComponent를 검사 없이 정한다(components 밖도).
    static Result<void> SetRoot(World& world, ObjectHandle actor, ObjectHandle component) noexcept {
        return world.With<Actor>(actor, [&](Actor& value) noexcept {
            return world.Objects().SetReference(world.Access(), actor, value.rootComponent_,
                                                component);
        });
    }

    // 영속 부착 부모·socket을 검사 없이 정한다.
    static Result<void> SetAttachParent(World& world, ObjectHandle component, ObjectHandle parent,
                                        StringView socket) noexcept {
        return world.With<SceneComponent>(component, [&](SceneComponent& value) noexcept {
            value.attachParent_ = WeakObjectRef<SceneComponent>(parent);
            return value.attachSocket_.TryAssign(socket);
        });
    }

    // 상대 Transform을 검사 없이 정한다.
    static Result<void> SetRelative(World& world, ObjectHandle component,
                                    const Transform& relative) noexcept {
        return world.With<SceneComponent>(
            component, [&](SceneComponent& value) noexcept { value.relative_ = relative; });
    }
};

} // namespace rocks::detail
