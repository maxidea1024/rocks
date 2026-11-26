#include <Gameplay/GameplayWorldSubsystem.h>
#include <Gameplay/PlayerController.generated.h>
#include <Gameplay/PlayerController.h>
#include <InputActions/InputAssetAccess.h>
#include <World/World.h>

// PlayerController의 에셋 컨텍스트와 World 전체 다시 모으기다(Docs/Design/InputActions 5.4절 —
// IA6b).
namespace rocks {

Result<void> PlayerController::AddInputContext(AssetManager& assets, AssetLease&& context,
                                               int32 priority,
                                               const input::ContextOptions& options) noexcept {
    if (inputAssetsClosed_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    ReleaseRemovedInputAssets();
    input::InputContextBundle bundle(inputAssets_.GetAllocator());
    if (auto collected = input::CollectInputContext(assets, context, bundle); !collected) {
        return collected;
    }

    const AssetId asset = context.Asset();
    const Guid contextId = bundle.id;
    const usize held = FindInputAsset(asset);
    if (held < inputAssets_.Size()) {
        // 같은 에셋: AddContext의 같은 id 규칙을 따른다. 쥔 lease는 받은 새 lease로 바꾼다(옛
        // lease가 회수됐어도 다시 넣으면 살아난다 — 같은 에셋이라 상주는 그대로다).
        if (inputAssets_[held].context != contextId) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        if (auto added = actionInput_.AddContext(Move(bundle), priority, options); !added) {
            return added;
        }

        inputAssets_[held].lease = Move(context);
        return Result<void>::Success();
    }

    // 한 컨텍스트 id는 한 원천이다(다른 에셋·직접 넣은 bundle).
    if (actionInput_.HasContext(contextId)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (auto reserved = inputAssets_.TryReserve(inputAssets_.Size() + 1); !reserved) {
        return reserved;
    }

    if (auto added = actionInput_.AddContext(Move(bundle), priority, options); !added) {
        return added;
    }

    AppliedInputAsset entry{asset, contextId, actionInput_.ContextSerial(contextId), Move(context)};
    // 칸을 먼저 확보했으므로 실패하지 않는다.
    (void)inputAssets_.TryPushBack(Move(entry));
    return Result<void>::Success();
}

Result<void> PlayerController::RemoveInputContext(AssetId asset,
                                                  const input::ContextOptions& options) noexcept {
    ReleaseRemovedInputAssets();
    const usize held = FindInputAsset(asset);
    if (held >= inputAssets_.Size()) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    const Guid contextId = inputAssets_[held].context;
    if (auto removed = actionInput_.RemoveContext(contextId, options); !removed) {
        return removed;
    }

    if (!IsInputAssetLive(inputAssets_[held])) {
        (void)inputAssets_.TryRemoveAt(held);
    }

    return Result<void>::Success();
}

Result<void> PlayerController::RefreshInputContext(AssetManager& assets, AssetId asset,
                                                   bool forceImmediately) noexcept {
    ReleaseRemovedInputAssets();
    const usize held = FindInputAsset(asset);
    if (held >= inputAssets_.Size()) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    input::InputContextBundle bundle(inputAssets_.GetAllocator());
    if (auto collected = input::CollectInputContext(assets, inputAssets_[held].lease, bundle);
        !collected) {
        return collected;
    }

    if (bundle.id != inputAssets_[held].context) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return actionInput_.UpdateContext(Move(bundle), forceImmediately);
}

bool PlayerController::HasInputContext(AssetId asset) const noexcept {
    const usize held = FindInputAsset(asset);
    return held < inputAssets_.Size() && IsInputAssetLive(inputAssets_[held]);
}

bool PlayerController::IsInputAssetLive(const AppliedInputAsset& entry) const noexcept {
    return actionInput_.ContextSerial(entry.context) == entry.serial;
}

usize PlayerController::FindInputAsset(AssetId asset) const noexcept {
    for (usize index = 0; index < inputAssets_.Size(); ++index) {
        if (inputAssets_[index].asset == asset) {
            return index;
        }
    }

    return inputAssets_.Size();
}

void PlayerController::ReleaseRemovedInputAssets() noexcept {
    // 뒤에서부터 지워 index가 밀리지 않게 한다.
    for (usize index = inputAssets_.Size(); index > 0; --index) {
        if (!IsInputAssetLive(inputAssets_[index - 1])) {
            (void)inputAssets_.TryRemoveAt(index - 1);
        }
    }
}

void PlayerController::ReleaseInputAssets() noexcept {
    for (const AppliedInputAsset& entry : inputAssets_) {
        // 등록 횟수를 세는 컨텍스트는 한 번에 한 횟수씩 빠진다. 끝난 칸(다른 원천의 같은 id)은
        // 건드리지 않는다.
        while (IsInputAssetLive(entry)) {
            if (!actionInput_.RemoveContext(entry.context)) {
                break;
            }
        }
    }

    inputAssets_.Clear();
    inputAssetsClosed_ = true;
}

Result<void> GameplayWorldSubsystem::RefreshInputContext(World& world, AssetManager& assets,
                                                         AssetId asset,
                                                         bool forceImmediately) noexcept {
    // 모두 시도하고 첫 실패를 돌려준다. 목록은 index로 다시 읽는다(Refresh가 hook을 부르지 않지만
    // 등록 목록 규칙을 UpdateCameras와 같게 둔다).
    Result<void> first = Result<void>::Success();
    for (usize index = 0; index < controllers_.Size(); ++index) {
        const ObjectHandle controller = controllers_[index];
        auto state = world.GetActorState(controller);
        if (!state || state.Value() == ActorState::PendingDestroy) {
            continue;
        }

        // With는 Result를 펼치므로 함수 결과는 ErrorCode로 돌려받는다.
        auto refreshed = world.With<PlayerController>(
            controller, [&](PlayerController& value) noexcept -> ErrorCode {
                if (!value.HasInputContext(asset)) {
                    return ErrorCode::None;
                }

                auto done = value.RefreshInputContext(assets, asset, forceImmediately);
                return done ? ErrorCode::None : done.Error();
            });
        // PlayerController가 아닌 Controller(AI)는 TypeMismatch로 건너뛴다. 그 밖의 With 오류는
        // 실패다.
        if (!refreshed) {
            if (refreshed.Error() != ErrorCode::TypeMismatch && first) {
                first = Result<void>::Failure(refreshed.Error());
            }

            continue;
        }

        if (refreshed.Value() != ErrorCode::None && first) {
            first = Result<void>::Failure(refreshed.Value());
        }
    }

    return first;
}

} // namespace rocks
