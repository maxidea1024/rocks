#pragma once

#include <Base/Types.h>

namespace rocks::ui {

// Render lane에서 장면 pass를 기록할 때 받는 문맥이다. UiCore에서는 불완전 타입이고 UiRender가
// 정의한다(그래프·대상 텍스처·크기·장치 — Docs/Design/UiToolkit 18.3절). 기록 함수를 정의하는 앱
// 번역 단위만 <UiRender/UiScenePassContext.h>를 include한다.
class UiScenePassContext;

// 프레임 저장소(Render lane 쪽)로 옮긴 장면 packet 하나다. Record가 기록 함수를, Destroy가
// 소멸자를 부른다. Destroy는 한 번만 부른다. 저장소 회수는 소유자(프레임 arena)가 한다.
struct UiSceneRecord {
    using InvokeFunction = void (*)(UiScenePassContext& context, void* object,
                                    void (*function)()) noexcept;
    using DestroyFunction = void (*)(void* object) noexcept;

    void* object = nullptr;
    InvokeFunction invoke = nullptr;
    void (*function)() = nullptr;
    DestroyFunction destroy = nullptr;

    void Record(UiScenePassContext& context) const noexcept {
        invoke(context, object, function);
    }

    void Destroy() noexcept {
        if (object) {
            destroy(object);
            object = nullptr;
        }
    }
};

} // namespace rocks::ui
