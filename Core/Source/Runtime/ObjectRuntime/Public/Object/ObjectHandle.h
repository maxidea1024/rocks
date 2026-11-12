#pragma once

#include <Base/Types.h>

namespace rocks {

// 소유권이 없는 값 식별자다. 저장소를 살려 두거나 역참조 권한을 부여하지 않는다.
// registry는 재생성된 레지스트리, generation은 재사용된 슬롯을 구분한다.
// IsNull은 비어 있는 값만 판정한다. 현재 생존 여부는 Registry에서 검증한다.
struct ObjectHandle {
    uint64 registry = 0;
    uint32 slot = 0;
    uint32 generation = 0;

    bool IsNull() const noexcept {
        return registry == 0 || generation == 0;
    }

    bool operator==(const ObjectHandle&) const noexcept = default;
};

} // namespace rocks
