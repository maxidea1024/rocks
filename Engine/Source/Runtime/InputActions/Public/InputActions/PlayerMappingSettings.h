#pragma once

#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <Core/Text/String.h>
#include <Localization/Text.h>
#include <Meta/Annotations.h>

namespace rocks::input {

// 리바인딩 표시 정보다(UE UPlayerMappableKeySettings — Docs/Design/InputActions 2.5절). 액션 또는
// 바인딩이 갖는다.
//
// 사용 예:
//   PlayerMappingSettings jump(allocator);
//   (void)jump.name.TryAssign("Jump");
ROCKS_STRUCT()
struct PlayerMappingSettings {
    ROCKS_GENERATED_BODY()

    explicit PlayerMappingSettings(Allocator& allocator) noexcept
        : name(allocator), supportedProfiles(allocator) {
    }

    // 사용자 설정의 저장 키다. 프로젝트 안에서 유일하다.
    ROCKS_PROPERTY() String name;
    ROCKS_PROPERTY() Text displayName;
    ROCKS_PROPERTY() Text displayCategory;
    // 이 매핑을 쓰는 키 프로필 id다. 비면 모든 프로필이다.
    ROCKS_PROPERTY() Array<String> supportedProfiles;
};

} // namespace rocks::input
