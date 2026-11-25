#pragma once

#include <Core/Result.h>
#include <World/Actor.h>
#include <World/World.h>

// 엔진 Actor(광원·Trigger·환경)가 프레임워크 PreAwake에서 root 컴포넌트를 준비하는 공용
// 규칙이다(Docs/Design/EngineRuntime 24.2.5절).
namespace rocks::actor_root_setup {

// T 타입(파생 포함) root가 이미 있으면(레벨 로드) 그대로 쓰고, 다른 타입 root면
// InvalidArgument다(옛 root를 떼어 둔 채 바꾸지 않는다). 없으면 T를 만들어 configure(T&) →
// Result<void>를 부르고 root로 둔다. 실패하면 그 오류다(만든 컴포넌트는 Spawn 실패 정리가 지운다).
template <typename T, typename Configure>
Result<void> PrepareRoot(Actor& actor, Configure&& configure) noexcept {
    World& world = actor.GetWorld();
    if (const ObjectHandle root = actor.RootComponent(); !root.IsNull()) {
        if (world.With<T>(root, [](T&) noexcept { return true; })) {
            return Result<void>::Success();
        }

        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto created = world.AddComponent<T>(actor.Handle());
    if (!created) {
        return Result<void>::Failure(created.Error());
    }

    auto configured = world.With<T>(created.Value(), configure);
    if (!configured) {
        return configured;
    }

    return actor.SetRootComponent(created.Value());
}

} // namespace rocks::actor_root_setup
