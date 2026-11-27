#pragma once

#include <Core/Text/String.h>
#include <Meta/Annotations.h>
#include <Object/Object.h>
#include <Object/ObjectRefs.h>

// 실행 중 필드 변경 통지 시험 대상이다(Docs/Design/ChangeNotification.ko.md CN1 — CN-D1·CN-D2):
// Notify 필드(값·문자열·참조)와 통지하지 않는 필드, Notify 계산 값 함수, 그 클래스를 물려받고
// Notify 필드·함수를 더하는 파생이다. 통지 번호 배치(필드 → 기반부터의 함수)와 구독·통지·정리를
// 본다(FieldNotificationTests).
namespace rocks::reflection_fixture {

ROCKS_CLASS()
class NotifyModel : public Object {
    ROCKS_GENERATED_BODY()

public:
    explicit NotifyModel(Allocator& allocator) noexcept : label(allocator) {
    }

    ROCKS_PROPERTY(Notify) int32 health = 0;
    ROCKS_PROPERTY(Notify) String label;
    ROCKS_PROPERTY() int32 plain = 0;
    ROCKS_PROPERTY(Notify) WeakObjectRef<NotifyModel> partner;

    // 체력의 두 배다(계산 값 — health가 바뀌면 코드가 함께 알린다).
    ROCKS_FUNCTION(Pure, Notify) int32 Doubled() const noexcept {
        return health * 2;
    }

    // 통지 대상이 아닌 Pure 함수다.
    ROCKS_FUNCTION(Pure) int32 Plain() const noexcept {
        return plain;
    }
};

ROCKS_CLASS()
class NotifyModelChild : public NotifyModel {
    ROCKS_GENERATED_BODY()

public:
    explicit NotifyModelChild(Allocator& allocator) noexcept : NotifyModel(allocator) {
    }

    ROCKS_PROPERTY(Notify) float speed = 0.0f;

    ROCKS_FUNCTION(Pure, Notify) bool Fast() const noexcept {
        return speed > 1.0f;
    }
};

} // namespace rocks::reflection_fixture
