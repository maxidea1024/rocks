#pragma once

#include <Base/NumericLimits.h>
#include <Core/Containers/Span.h>
#include <Core/Math/MathUtility.h>
#include <Core/Result.h>

namespace rocks::ui {

enum class StackSizeRule : uint8 {
    Auto,
    Fill,
    FillContent
};

enum class StackAlignment : uint8 {
    Fill,
    Start,
    Center,
    End
};

// 한 축의 슬롯 설정이다. 여백은 크기 밖에 배치된다.
struct StackSlot {
    StackSizeRule rule = StackSizeRule::Auto;
    float desired = 0;
    float weight = 1;
    float minSize = 0;
    float maxSize = NumericLimits<float>::Max();
    float before = 0;
    float after = 0;
    float crossBefore = 0;
    float crossAfter = 0;
    StackAlignment alignment = StackAlignment::Fill;
    // FillContent를 줄일 때는 이 계수에 자식의 기준 크기를 곱한다.
    float shrinkWeight = 1;
    // 주축의 배정 크기 안에 내용을 어떻게 놓을지 지정한다. Fill이면 배정 크기를 그대로 쓴다.
    StackAlignment mainAlignment = StackAlignment::Fill;
    bool operator==(const StackSlot&) const noexcept = default;
};

struct StackPlacement {
    float offset = 0;
    float size = 0;
};

// Auto와 여백을 먼저 배정하고 남은 공간을 Fill 계수로 나눈다. Fill의 최소·최대 제한으로
// 생긴 차이는 재분배하지 않는다. 입력이 잘못되면 결과 배열을 변경하지 않는다.
ROCKS_UICORE_API Result<void> ArrangeStack(Span<const StackSlot> slots, Span<StackPlacement> output,
                                           float available, bool allowShrink = true) noexcept;

} // namespace rocks::ui
