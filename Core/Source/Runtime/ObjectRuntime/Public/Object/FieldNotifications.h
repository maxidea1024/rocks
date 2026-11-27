#pragma once

#include <Base/Guid.h>
#include <Base/NumericLimits.h>
#include <Core/Delegate.h>
#include <Object/ObjectHandle.h>

// 실행 중 필드 변경 통지의 공개 형식이다(Docs/Design/ChangeNotification.ko.md 3.1절 — CN1, UE
// INotifyFieldValueChanged·FFieldId 대응). 구독·통지 API는 ObjectRegistry 멤버다(TrySubscribe·
// TrySubscribeAll·Unsubscribe·UnsubscribeAll·Notify·NotifyIndex).
//
// 사용 예(UI 바인딩이 체력 필드를 구독한다):
//   auto handle = registry.TrySubscribe(access, actor, NotifyTarget{healthId},
//                                       FieldChangeDelegate::CreateLambda(
//                                           [&](ObjectHandle, Guid) noexcept { bar.Refresh(); }));
//   ...
//   registry.Unsubscribe(access, handle.Value());
namespace rocks {

// 통지 대상이다(필드는 FieldId, ROCKS_FUNCTION(Notify) 계산 값 함수는 FunctionId — CN-D3).
struct NotifyTarget {
    Guid id;
};

// 통지를 받는 delegate다: (바뀌었을 수 있는 객체, 통지 대상 ID). 통지의 뜻은 "바뀌었을 수 있음"이고
// 놓침은 없다 — 같은 값을 쓴 경우에도 올 수 있고 옛 값은 싣지 않는다(CN-D3).
using FieldChangeDelegate = Delegate<void(ObjectHandle, Guid)>;

// 구독 하나의 해제 key다. 기본값은 무효다. 객체가 파괴 요청·회수되면 그 구독은 사라지고 handle은
// 무효가 된다(Unsubscribe는 false — CN-D11).
struct FieldNotifyHandle {
    // 모든 대상 구독(TrySubscribeAll)의 target 표지다.
    static constexpr uint32 AllTargets = NumericLimits<uint32>::Max();

    ObjectHandle object{};
    uint32 target = AllTargets;
    DelegateHandle delegate{};
    // 구독 목록의 일련번호다(목록이 사라졌다 다시 생기면 다르다 — 옛 handle이 새 구독을 지우지
    // 않는다).
    uint64 serial = 0;

    bool IsValid() const noexcept {
        return !object.IsNull() && delegate.IsValid();
    }
};

// 통지 중첩 깊이 상한이다(Registry 단위 — 다른 객체로 이어지는 연쇄 포함, CN-D6). 넘는 통지는
// 구독자를 부르지 않고 버리며 ObjectStats::droppedNotifications에 센다. SL 5.4 연쇄 상한과 같은
// 수다.
inline constexpr uint32 MaxNotifyDepth = 16;

} // namespace rocks
