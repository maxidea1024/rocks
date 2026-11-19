#pragma once

#include <Core/Error.h>
#include <Object/ObjectRegistryOptions.h>

// World 구현 파일들이 함께 쓰는 내부 helper다(공개 API 아님). 시험은
// <World/Private/WorldInternal.h>로 판정 함수를 직접 본다.
namespace rocks::detail {

// 진행 중인 증분 cycle을 끝까지 진행하는 Step 상한과 그 Step의 예산이다(종료 — GC 설계 17.3절).
// 예산 없는 Step 하나가 단계 하나를 끝내므로 몇 번이면 Idle이다.
inline constexpr uint32 FullCollectionFinishSteps = 64;
inline constexpr GcBudget UnboundedFinishBudget{GcBudget::DefaultMaxObjects, 0};

// Level 배열에서 Actor를 떼지 못했을 때의 Fatal 메시지다. 떼기는 World 정리 안에서만 하므로 실패는
// 일어날 수 없다 — 공개 API로 닿는 하나는 Level을 PinForEncode로 고정한 채 Tick해 그 프레임의
// Actor 정리가 고정된 배열을 고치려는 경우다(Busy). 그때는 계약(고정도 Tick 전에 반환)을 밝힌다.
inline const char* LevelDetachFailureMessage(ErrorCode error) noexcept {
    if (error == ErrorCode::Busy) {
        return "A Level was pinned for encoding across World::Tick; return PinForEncode pins "
               "before Tick or Shutdown (World ownership contract)";
    }

    return "World could not detach an actor from its Level";
}

} // namespace rocks::detail
