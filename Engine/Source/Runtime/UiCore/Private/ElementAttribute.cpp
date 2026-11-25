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
    if (!subtreeBindings_ || visibility_ == Visibility::Hidden ||
        visibility_ == Visibility::Collapsed) {
        return;
    }

    ++bindingPass_;
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
