#pragma once

#include <Core/Result.h>
#include <InputActions/Haptics/HapticEffect.h>
#include <InputActions/InputContextBundle.h>

namespace rocks {
class AssetLease;
class AssetManager;
} // namespace rocks

// 입력 에셋을 lease로 읽는 도우미다(Docs/Design/InputActions 2.8절 — IA6a). 모두 AssetManager의
// 소유 스레드(Game)에서 부른다.
namespace rocks::input {

// lease의 대표 에셋(InputContext)과 그 actions를 모아 bundle을 만든다. AssetManager::With 범위에서
// 에셋 객체 Registry로 2.7절 CollectInputContext를 부른다.
//
// 사용 예:
//   InputContextBundle bundle(allocator);
//   if (auto collected = CollectInputContext(assets, contextLease, bundle); collected) {
//       (void)player.AddContext(Move(bundle), 0);
//   }
//
// 오류: 회수·무효 lease는 StaleHandle, 다른 스레드는 WrongThread, 초기화 전 AssetManager는
// InvalidArgument(With 오류 그대로), 대표 에셋이
// InputContext가 아니면 TypeMismatch, 그 밖은 2.7절 모으기와 같다(null actions 원소
// InvalidArgument, 커브 slot 범위 밖·null 원소 InvalidArgument, 풀리지 않는 커브 에셋은 Resolve
// 오류, 할당 실패 OutOfMemory). 실패하면 bundle은 그대로다.
ROCKS_INPUTACTIONS_API Result<void> CollectInputContext(AssetManager& assets,
                                                        const AssetLease& context,
                                                        InputContextBundle& bundle) noexcept;

// lease의 대표 에셋(HapticEffectAsset)의 effect를 out에 깊이 복사하고 out.id를 실효 id(2.8절)로
// 쓴다. 사본을 다 만든 뒤에 out을 바꾼다(HapticEffect::TryCopyFrom). 사본의 allocator는 out의
// 것이다. lease는 쥐지 않는다(사본은 lease와 수명이 따로다). 게임 스레드 전용이다.
//
// 사용 예:
//   HapticEffect hit(allocator);
//   if (CopyHapticEffect(assets, hitLease, hit)) {
//       (void)controller.Haptics().PlayHapticEffect(hit, {});
//   }
//
// 채널의 커브 에셋 slot은 HapticEffectAsset::curveAssets로 평탄화한다(2.9절).
//
// 오류: lease 오류는 CollectInputContext와 같고, 대표 에셋이 HapticEffectAsset이 아니면
// TypeMismatch, 커브 slot 범위 밖·null 원소 InvalidArgument, 풀리지 않는 커브 에셋은 Resolve 오류,
// 할당 실패는 OutOfMemory다. 실패하면 out은 그대로다.
ROCKS_INPUTACTIONS_API Result<void> CopyHapticEffect(AssetManager& assets, const AssetLease& effect,
                                                     HapticEffect& out) noexcept;

} // namespace rocks::input
