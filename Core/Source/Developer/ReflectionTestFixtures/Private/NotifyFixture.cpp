#include <ReflectionTestFixtures/NotifyFixture.generated.h>

// 코드 통지 fixture의 시험 진입점이다(Docs/Design/ChangeNotification.ko.md CN-D9 — NotifyChanged·
// SetAndNotify는 대상 클래스의 생성 헤더(NotifyId·ObjectSetter.h)가 보이는 번역 단위에서 쓴다. 공개
// 헤더는 자기 생성 헤더를 include할 수 없다).
namespace rocks::reflection_fixture {

Result<bool> NotifyWidget::PutCount(int32 next) noexcept {
    return SetAndNotify<&NotifyWidget::count>(next);
}

Result<bool> NotifyWidget::PutValues(const Array<int32>& next) noexcept {
    return SetAndNotify<&NotifyWidget::values>(next);
}

Result<bool> NotifyWidget::PutCached(const NotifyCached& next) noexcept {
    return SetAndNotify<&NotifyWidget::cached>(next);
}

Result<void> NotifyWidget::TouchTotal() noexcept {
    return NotifyChanged<&NotifyWidget::Total>();
}

Result<void> NotifyWidget::TouchScale() noexcept {
    return NotifyChanged<&NotifyWidget::scale>();
}

Result<bool> NotifyWidgetChild::PutBaseCount(int32 next) noexcept {
    return SetAndNotify<&NotifyWidget::count>(next);
}

Result<bool> NotifyWidgetChild::PutLevel(int32 next) noexcept {
    return SetAndNotify<&NotifyWidgetChild::level>(next);
}

Result<void> NotifyWidgetChild::TouchBaseTotal() noexcept {
    return NotifyChanged<&NotifyWidget::Total>();
}

} // namespace rocks::reflection_fixture
