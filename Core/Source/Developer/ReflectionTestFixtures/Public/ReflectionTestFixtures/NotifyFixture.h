#pragma once

#include <Core/Containers/Array.h>
#include <Core/Text/String.h>
#include <Meta/Annotations.h>
#include <Object/Object.h>
#include <Object/ObjectRefArray.h>
#include <Object/ObjectRefMap.h>
#include <Object/ObjectRefs.h>
#include <Object/StructRefArray.h>

// 실행 중 필드 변경 통지 시험 대상이다(Docs/Design/ChangeNotification.ko.md CN1 — CN-D1·CN-D2):
// Notify 필드(값·문자열·참조)와 통지하지 않는 필드, Notify 계산 값 함수, 그 클래스를 물려받고
// Notify 필드·함수를 더하는 파생이다. 통지 번호 배치(필드 → 기반부터의 함수)와 구독·통지·정리를
// 본다(FieldNotificationTests). NotifyHolder는 쓰기 경로의 통지(CN1b — CN-D10) 대상이다: 강한 참조·
// 참조 배열·참조 struct 배열·참조 Map·참조 struct·값 배열·실수 Notify 필드와 통지하지 않는 참조
// 배열(FieldNotificationWriteTests·TextAssignNotifyTests).
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

// enum 키 Map Notify 필드(NotifyHolder::byKind — 생성 함수표의 키 enum 칸 뒤 storage 칸)의 키다.
ROCKS_ENUM()
enum class NotifyKind : uint8 {
    First,
    Second
};

// 인라인 참조 struct다(NotifyHolder::point — 경로 쓰기가 안의 참조를 SetReference로 쓴다).
ROCKS_STRUCT()
struct NotifyPoint {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() int32 x = 0;
    ROCKS_PROPERTY() int32 y = 0;
    ROCKS_PROPERTY() ObjectRef<NotifyModel> pin;
};

// 참조 struct 원소다(StructRefArray<NotifySlot>).
ROCKS_STRUCT()
struct NotifySlot {
    ROCKS_GENERATED_BODY()

    explicit NotifySlot(Allocator&) noexcept {
    }

    ROCKS_PROPERTY() ObjectRef<NotifyModel> item;
    ROCKS_PROPERTY() int32 count = 0;
};

ROCKS_CLASS()
class NotifyHolder : public Object {
    ROCKS_GENERATED_BODY()

public:
    explicit NotifyHolder(Allocator& allocator) noexcept
        : members(allocator), quiet(allocator), slots(allocator), byKey(allocator),
          byKind(allocator), numbers(allocator) {
    }

    ROCKS_PROPERTY(Notify) ObjectRef<NotifyModel> target;
    ROCKS_PROPERTY(Notify) ObjectRefArray<NotifyModel> members;
    ROCKS_PROPERTY() ObjectRefArray<NotifyModel> quiet;
    ROCKS_PROPERTY(Notify) StructRefArray<NotifySlot> slots;
    ROCKS_PROPERTY(Notify) ObjectRefMap<uint32, NotifyModel> byKey;
    ROCKS_PROPERTY(Notify) ObjectRefMap<NotifyKind, NotifyModel> byKind;
    ROCKS_PROPERTY(Notify) NotifyPoint point;
    ROCKS_PROPERTY(Notify) Array<int32> numbers;
    ROCKS_PROPERTY(Notify) float ratio = 0.0f;
};

// 상속한 Notify 컨테이너를 파생 객체에서 Registry API로 바꾼다(storage thunk가 파생 객체에서 기반
// 멤버를 잰다).
ROCKS_CLASS()
class NotifyHolderChild : public NotifyHolder {
    ROCKS_GENERATED_BODY()

public:
    explicit NotifyHolderChild(Allocator& allocator) noexcept : NotifyHolder(allocator) {
    }
};

} // namespace rocks::reflection_fixture
