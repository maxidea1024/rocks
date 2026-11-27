#pragma once

#include <UiWidget/UserWidgetAnimation.h>

namespace rocks::ui {

// 화면이 외부 핸들과 별도로 소유하는 재생 목록이다. 완료한 인스턴스는 마지막 콜백 전에 빠진다.
class UserWidgetAnimationSet final {
public:
    explicit UserWidgetAnimationSet(Allocator& allocator) noexcept;
    void Remove(const UserWidgetAnimation* player) noexcept;
    Array<SharedPtr<UserWidgetAnimation>> players;
};

} // namespace rocks::ui
