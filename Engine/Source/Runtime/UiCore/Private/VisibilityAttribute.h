#pragma once

#include <UiCore/ElementAttribute.h>

namespace rocks::ui {

// Element 보이기 묶음이다(Docs/Design/UiToolkit 20.6절 — UE Visibility 속성).
// Element::BindVisibility가 만들어 semantics에 두고, UpdateBoundAttributes가 다른 묶음보다 먼저
// 평가한다.
class VisibilityAttribute final : public AttributeBinding {
public:
    VisibilityAttribute(Element& owner, Delegate<Visibility()>&& getter) noexcept
        : AttributeBinding(owner), getter_(Move(getter)) {
        SetRegistered(true);
    }

    // 묶음 목록에서 뺀다(평가 중 교체 — 객체는 갱신이 끝난 뒤 해제된다).
    void Retire() noexcept {
        SetRegistered(false);
    }

private:
    void EvaluateValue() noexcept override {
        Owner().ApplyVisibility(getter_.Execute());
    }

    Delegate<Visibility()> getter_;
};

} // namespace rocks::ui
