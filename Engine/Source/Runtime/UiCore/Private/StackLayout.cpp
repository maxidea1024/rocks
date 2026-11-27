#include <UiCore/StackLayout.h>

namespace rocks::ui {

namespace {

// Fill 슬롯 최소·최대 크기 제한을 다시 나누는 최대 반복 수다(그 안에 수렴한다).
constexpr int StackResolvePasses = 5;

double ClampSize(const StackSlot& slot, double size) noexcept {
    return math::Clamp(size, static_cast<double>(slot.minSize), static_cast<double>(slot.maxSize));
}

double ContentWeight(const StackSlot& slot, bool growing) noexcept {
    return growing ? slot.weight : slot.shrinkWeight * ClampSize(slot, slot.desired);
}

double ContentSize(const StackSlot& slot, bool growing, double adjustment) noexcept {
    return ClampSize(slot,
                     ClampSize(slot, slot.desired) + adjustment * ContentWeight(slot, growing));
}

// 모든 슬롯이 같은 방향으로 변하므로 가중치당 누적 변화량 하나로 현재 크기를 복원한다.
// 한계에 닿은 슬롯은 다음 분배에서 제외한다.
double AdjustContent(Span<const StackSlot> slots, double space, bool growing) noexcept {
    double adjustment = 0;
    for (int pass = 0; pass < StackResolvePasses; ++pass) {
        double used = 0;
        double weights = 0;
        for (const StackSlot& slot : slots) {
            if (slot.rule == StackSizeRule::FillContent) {
                const double size = ContentSize(slot, growing, adjustment);
                used += size;
                if (growing ? size < slot.maxSize : size > slot.minSize) {
                    weights += ContentWeight(slot, growing);
                }
            }
        }

        const double delta = space - used;
        if (weights == 0 || (growing ? delta <= 0 : delta >= 0)) {
            break;
        }

        const double next = adjustment + delta / weights;
        if (next == adjustment) {
            break;
        }

        adjustment = next;
    }

    return adjustment;
}

} // namespace

Result<void> ArrangeStack(Span<const StackSlot> slots, Span<StackPlacement> output, float available,
                          bool allowShrink) noexcept {
    if (slots.Size() != output.Size() || !math::IsFinite(available) || available < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    double reserved = 0;
    double weights = 0;
    double contentBasis = 0;
    double stretchBasis = 0;
    bool hasContent = false;
    for (usize i = 0; i < slots.Size(); ++i) {
        const StackSlot& slot = slots[i];
        if ((slot.rule != StackSizeRule::Auto && slot.rule != StackSizeRule::Fill &&
             slot.rule != StackSizeRule::FillContent) ||
            !math::IsFinite(slot.shrinkWeight) || slot.shrinkWeight < 0 ||
            !math::IsFinite(slot.desired) || !math::IsFinite(slot.weight) ||
            !math::IsFinite(slot.minSize) || !math::IsFinite(slot.maxSize) ||
            !math::IsFinite(slot.before) || !math::IsFinite(slot.after) || slot.desired < 0 ||
            !math::IsFinite(slot.crossBefore) || !math::IsFinite(slot.crossAfter) ||
            slot.weight < 0 || slot.minSize < 0 || slot.maxSize < slot.minSize || slot.before < 0 ||
            slot.after < 0 || slot.crossBefore < 0 || slot.crossAfter < 0 ||
            slot.mainAlignment > StackAlignment::End ||
            (slot.alignment != StackAlignment::Fill && slot.alignment != StackAlignment::Start &&
             slot.alignment != StackAlignment::Center && slot.alignment != StackAlignment::End)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        reserved += static_cast<double>(slot.before) + slot.after;
        if (slot.rule == StackSizeRule::Auto) {
            const float size = math::Clamp(slot.desired, slot.minSize, slot.maxSize);
            reserved += size;
        } else {
            weights += slot.weight;
            stretchBasis += ClampSize(slot, slot.desired);
            if (slot.rule == StackSizeRule::FillContent) {
                hasContent = true;
                contentBasis += ClampSize(slot, slot.desired);
            }
        }
    }

    const double remaining = math::Max(allowShrink ? 0.0 : stretchBasis, available - reserved);
    auto fillSize = [&](const StackSlot& slot) noexcept {
        return ClampSize(slot, weights > 0 ? remaining * slot.weight / weights : 0);
    };

    double contentSpace = remaining;
    if (hasContent) {
        for (const StackSlot& slot : slots) {
            if (slot.rule == StackSizeRule::Fill) {
                contentSpace -= fillSize(slot);
            }
        }
    }

    const bool growing = contentSpace > contentBasis;
    const double adjustment = hasContent && contentSpace != contentBasis
                                  ? AdjustContent(slots, contentSpace, growing)
                                  : 0;
    // 먼저 모든 위치가 float 범위에 드는지 확인해 실패 시 결과를 보존한다.
    for (int pass = 0; pass < 2; ++pass) {
        double position = 0;
        for (usize i = 0; i < slots.Size(); ++i) {
            const StackSlot& slot = slots[i];
            double size = ClampSize(slot, slot.desired);
            if (slot.rule == StackSizeRule::Fill) {
                size = fillSize(slot);
            } else if (slot.rule == StackSizeRule::FillContent) {
                size = ContentSize(slot, growing, adjustment);
            }

            position += slot.before;
            if (position > static_cast<double>(NumericLimits<float>::Max()) ||
                !math::IsFinite(position)) {
                return Result<void>::Failure(ErrorCode::Overflow);
            }

            if (pass == 1) {
                output[i] = {static_cast<float>(position), static_cast<float>(size)};
            }

            position += size + slot.after;
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::ui
