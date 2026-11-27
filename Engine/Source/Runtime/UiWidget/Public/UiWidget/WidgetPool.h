#pragma once

#include <UiWidget/UserWidget.h>

namespace rocks::ui {

struct WidgetPoolEntry {
    ObjectHandle widget;
    SharedPtr<Element> element;
};

// 같은 클래스의 목록 엔트리를 재사용한다. 풀과 대여한 Element는 객체 저장소보다 먼저 해제한다.
class ROCKS_UIWIDGET_API WidgetPool final {
public:
    WidgetPool(ObjectRegistry& objects, TypeId widgetClass) noexcept;
    ~WidgetPool() noexcept;
    WidgetPool(const WidgetPool&) = delete;
    WidgetPool& operator=(const WidgetPool&) = delete;

    Result<WidgetPoolEntry> Acquire() noexcept;
    // 화면에서 분리한 뒤 반납한다. Element까지 보관하면 다음 대여에서 구성 알림을 생략한다.
    Result<void> Release(ObjectHandle widget, bool cacheElement) noexcept;
    Result<void> Reset() noexcept;
    usize ActiveCount() const noexcept;
    usize InactiveCount() const noexcept;

private:
    struct Entry {
        RootRef<UserWidget> widget;
        SharedPtr<Element> element;
    };

    ObjectRegistry& objects_;
    TypeId widgetClass_;
    Array<Entry> entries_;
    usize activeCount_ = 0;
    bool updating_ = false;
};

} // namespace rocks::ui
