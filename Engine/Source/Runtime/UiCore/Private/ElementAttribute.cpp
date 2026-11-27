#include <UiCore/ElementAttribute.h>

namespace rocks::ui {

AttributeBinding::AttributeBinding(Element& owner) noexcept : owner_(owner) {
}

AttributeBinding::~AttributeBinding() noexcept {
    SetRegistered(false);
}

Result<void> AttributeBinding::SetPrerequisite(AttributeBinding* prerequisite) noexcept {
    if (prerequisite && &prerequisite->owner_ != &owner_) {
        return Result<void>::Failure(ErrorCode::WrongContext);
    }

    for (auto* p = prerequisite; p; p = p->prerequisite_) {
        if (p == this) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    prerequisite_ = prerequisite;
    return Result<void>::Success();
}

void AttributeBinding::SetRegistered(bool registered) noexcept {
    if (registered_ == registered) {
        return;
    }

    auto** link = &owner_.bindings_;
    if (registered) {
        // 등록 시점과 무관하게 멤버 선언 위치 순으로 평가한다.
        while (*link && reinterpret_cast<uptr>(*link) < reinterpret_cast<uptr>(this)) {
            link = &(*link)->next_;
        }

        next_ = *link;
        *link = this;
    } else {
        while (*link != this) {
            link = &(*link)->next_;
        }

        *link = next_;
    }

    for (auto* p = &owner_; p; p = p->Parent()) {
        if (registered) {
            ++p->subtreeBindings_;
        } else {
            --p->subtreeBindings_;
        }
    }

    registered_ = registered;
}

void AttributeBinding::Evaluate(uint64 pass) noexcept {
    if (!registered_ || evaluated_ == pass) {
        return;
    }

    evaluated_ = pass;
    if (prerequisite_) {
        prerequisite_->Evaluate(pass);
    }

    EvaluateValue();
}

void AttributeBinding::Changed(InvalidateReason reason) noexcept {
    switch (reason) {
    case InvalidateReason::Paint:
        owner_.InvalidatePaint();
        break;
    case InvalidateReason::Layout:
        owner_.InvalidateLayout();
        break;
    case InvalidateReason::Arrangement:
        owner_.InvalidateArrangement();
        break;
    }
}

void Element::UpdateBoundAttributes() noexcept {
    if (!subtreeBindings_) {
        return;
    }

    ++bindingPass_;
    // 보이기 묶음을 먼저 평가한다 — Collapsed여도 다시 보일 수 있게(UE
    // UpdateOnlyVisibilityAttributes, SWidget.cpp 1846–1849행, Docs/Design/UiToolkit 20.6절).
    EvaluateVisibilityBinding(bindingPass_);
    if (visibility_ == Visibility::Collapsed) {
        return;
    }

    // Hidden은 공간을 차지하므로 나머지 묶음도 평가한다(UE 1852–1859행). 보이기 묶음은 이번 회차에
    // 이미 평가되어 건너뛴다.
    for (auto* binding = bindings_; binding;) {
        auto* next = binding->next_;
        binding->Evaluate(bindingPass_);
        binding = next;
    }

    for (const auto& child : ActiveChildren()) {
        if (child->subtreeBindings_) {
            child->UpdateBoundAttributes();
        }
    }
}

} // namespace rocks::ui
