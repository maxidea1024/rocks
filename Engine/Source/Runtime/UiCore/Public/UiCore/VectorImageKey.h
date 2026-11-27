#pragma once

#include <Core/Hash.h>
#include <Core/Text/StringView.h>

namespace rocks::ui {

// 벡터 이미지(SVG 아이콘)를 가리키는 키다(Docs/Design/UiToolkit 17.2절). 아이콘 루트 기준 상대
// 경로에서 확장자를 뺀 것의 해시이고 구분자는 '/'다. 대소문자를 구별한다. constexpr이라 정적 스타일
// 값에 바로 쓴다. 해시가 겹치는 두 경로는 등록 때 AlreadyExists로 막는다.
//
// 사용 예:
//   constexpr VectorImageKey Check = VectorImageKey::FromPath("CoreWidgets/CheckBox/Check");
//   Brush brush = Brush::Vector(Check, {16, 16});
struct VectorImageKey {
    // 0은 "키 없음"이다.
    uint64 hash = 0;

    static constexpr VectorImageKey FromPath(StringView path) noexcept {
        const uint64 hashed = HashStringBytes(path);
        // 0은 빈 키라 실제 경로가 0으로 해시되면 1로 옮긴다.
        return {hashed == 0 ? 1 : hashed};
    }

    constexpr bool IsNull() const noexcept {
        return hash == 0;
    }

    friend constexpr bool operator==(const VectorImageKey&,
                                     const VectorImageKey&) noexcept = default;
};

} // namespace rocks::ui
