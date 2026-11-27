#pragma once

#include <AudioAssets/SoundConcurrencySettings.h>

namespace rocks {

// 재생 스냅숏의 동시 발음 항목이다(3.41.2절). key는 실행 중 유일 id(에셋 그룹이면 ConcurrencyGroup
// 객체, 재정의면 사운드 객체 — UE handle의 UObject::GetUniqueID)이고 0이 아니어야 한다.
struct SoundConcurrencyEntry {
    uint64 key = 0;
    // 사운드 자체 재정의 설정이다(그룹 모드가 Sound·OwnerPerSound가 된다).
    bool isOverride = false;
    SoundConcurrencySettings settings;
};

// 항목 목록을 본다: 수 ≤ MaxConcurrencyGroups(넘으면 CapacityExceeded), 키가 있고, 설정이 범위
// 안이고, 같은 (키, 재정의) 항목이 없다(어기면 InvalidArgument).
ROCKS_AUDIOASSETS_API Result<void>
ValidateSoundConcurrencyEntries(const SoundConcurrencyEntry* entries, usize count) noexcept;

} // namespace rocks
