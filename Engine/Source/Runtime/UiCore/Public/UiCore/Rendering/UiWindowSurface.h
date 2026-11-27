#pragma once

#include <Core/Containers/SlotArray.h>

namespace rocks::ui {

// 렌더러가 발급하는 창 표면(스왑 체인) id다. 텍스처 id와 섞이지 않게 따로 감싼다.
struct UiWindowSurfaceId {
    SlotHandle handle{};

    bool IsNull() const noexcept {
        return handle.IsNull();
    }

    bool operator==(const UiWindowSurfaceId&) const noexcept = default;
};

// 창 표면을 만들 native 창이다. 값은 플랫폼 창이 주는 그대로이며 UI는 해석하지 않는다
// (Graphics NativeSurface와 같다 — Win64는 HWND와 HINSTANCE).
struct UiWindowSurfaceDesc {
    void* nativeWindow = nullptr;
    void* nativeInstance = nullptr;
    uint32 width = 0;
    uint32 height = 0;
    // 수직 동기화로 present한다. 에디터 창 기본은 UE처럼 끔이다(r.VSyncEditor 기본 0 — 창 여럿을
    // 매번 vsync로 present하면 직렬로 기다린다, Docs/Design/UiToolkit 18.4절).
    bool vsync = false;
};

} // namespace rocks::ui
