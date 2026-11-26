#pragma once

#include <Core/Math/Vector4.h>
#include <UiCore/Styles/ButtonStyle.h>
#include <UiCore/Styles/TextStyle.h>
#include <UiCore/VectorImageKey.h>

namespace rocks::ui {

// 직접 그린 창 테두리 스타일이다(UE FWindowStyle — Docs/Design/UiToolkit 9.1절 UW7b). 창 버튼은
// 버튼 스타일(올림 바탕)과 아이콘(전경색으로 tint)으로 그리고, 테두리는 바탕·2 px 틀·그 안쪽
// outlineInset 자리의 1 px 외곽선을 겹친다. 내용은 borderPadding 안에 둔다(최대화 중에는 틀·여백
// 없음).
struct WindowStyle {
    ButtonStyle controlButton{};
    ButtonStyle closeButton{};
    VectorImageKey minimizeIcon{};
    VectorImageKey maximizeIcon{};
    VectorImageKey restoreIcon{};
    VectorImageKey closeIcon{};
    Vec2 buttonSize{};
    TextStyle title{};
    Vec4 titlePadding{};
    VectorImageKey appIcon{};
    Vec2 appIconSize{};
    Vec4 appIconPadding{};
    StyleBrush background{};
    StyleBrush border{};
    StyleBrush outline{};
    float outlineInset = 0.0f;
    Vec4 borderPadding{};
    // 제목 막대 아래 내용(도킹 영역 등)을 창 틀에서 띄우는 여백이다(왼쪽·위·오른쪽·아래). 최대화해도
    // 남는다 — 내용 가장자리가 창 외곽선에 붙어 선이 두 겹으로 보이지 않게.
    Vec4 contentPadding{};
    bool operator==(const WindowStyle&) const noexcept = default;
};

} // namespace rocks::ui
