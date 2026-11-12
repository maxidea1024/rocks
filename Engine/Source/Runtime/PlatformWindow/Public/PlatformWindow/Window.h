#pragma once

#include <Core/Memory/UniquePtr.h>
#include <Core/Text/StringView.h>
#include <Input/InputEvent.h>

namespace rocks::platform {

// 창 생성 설정이다.
struct WindowDesc {
    // UTF-8 제목이다. 창이 복사한다(255자를 넘으면 자른다).
    StringView title = "Rocks";
    // client 영역 크기(픽셀)다. 0이면 InvalidArgument다.
    uint32 width = 1280;
    uint32 height = 720;
    bool resizable = true;
    bool visible = true;
};

// 그래픽 backend가 swapchain·context를 만들 때 쓰는 OS 핸들이다. Windows: window = HWND,
// instance = HINSTANCE. 창보다 오래 보관하지 않는다.
struct NativeWindowHandle {
    void* window = nullptr;
    void* instance = nullptr;
};

// OS 창 하나다. 이벤트 펌프가 창 이벤트(닫기·크기·포커스)를 상태로 기록하고, 키보드·마우스 입력은
// input::InputEvent로 바꿔 큐에 넣는다.
//
// 사용 예:
//   auto window = platform::Window::Create(allocator, {.title = "Sandbox"});
//   if (!window) { ... }                          // Unsupported(창 backend 없는 플랫폼)·OS 오류
//   input::InputQueue queue(allocator);
//   queue.TryInitialize(256);
//   while (!window.Value()->IsCloseRequested()) {
//       window.Value()->PumpEvents(queue);        // 메시지 펌프 + 입력 이벤트 수집
//       uint32 width = 0, height = 0;
//       if (window.Value()->ConsumeResize(width, height)) { swapChain.Resize(width, height); }
//       inputState.BeginFrame(queue.Events());
//       queue.Clear();
//   }
//
// 계약:
// - 생성한 스레드(메시지를 받는 스레드)에서만 사용한다. Windows의 메시지 큐는 스레드에 묶인다.
// - 닫기 버튼은 창을 파괴하지 않고 IsCloseRequested만 세운다. 실제 파괴는 Window 소멸이다.
// - 포커스를 잃으면 FocusLost 이벤트를 넣는다(InputState가 눌린 키를 모두 놓는다).
// - 키는 스캔코드(물리 위치) 기준 input::Key이고, 입력 문자는 Text 이벤트(UTF-32)로 따로 온다.
class Window {
public:
    // 창을 만든다. 크기 0은 InvalidArgument, 창 backend가 없는 플랫폼은 Unsupported, OS 실패는
    // DependencyFailed다.
    static Result<UniquePtr<Window>> Create(Allocator& allocator, const WindowDesc& desc) noexcept;

    virtual ~Window() = default;

    // 이 스레드의 OS 메시지를 처리하고 이 창의 입력 이벤트를 queue에 넣는다. 큐가 가득 차면
    // 이벤트를 버린다(InputQueue::DroppedCount).
    virtual void PumpEvents(input::InputQueue& queue) noexcept = 0;

    virtual bool IsCloseRequested() const noexcept = 0;
    virtual void ClearCloseRequest() noexcept = 0;

    // 현재 client 영역 크기다. 최소화 중이면 0일 수 있다.
    virtual uint32 Width() const noexcept = 0;
    virtual uint32 Height() const noexcept = 0;

    // 마지막 호출 이후 크기가 바뀌었으면 새 크기를 쓰고 true를 반환한다.
    virtual bool ConsumeResize(uint32& width, uint32& height) noexcept = 0;

    virtual bool HasFocus() const noexcept = 0;

    // 최소화되어 그릴 영역이 없는지다. 렌더러는 이때 present를 건너뛴다.
    virtual bool IsMinimized() const noexcept = 0;

    virtual NativeWindowHandle NativeHandle() const noexcept = 0;

    // 제목을 바꾼다(UTF-8). OS 실패는 DependencyFailed다.
    virtual Result<void> SetTitle(StringView title) noexcept = 0;
};

// 게임패드 입력 backend다. 폴링 결과를 GamepadState 이벤트로 큐에 넣는다. 상태가 바뀐
// 패드(연결·해제 포함)만 이벤트를 만든다.
//
// 사용 예:
//   auto gamepads = platform::GamepadBackend::Create(allocator);
//   gamepads.Value()->Poll(queue);               // 프레임마다 입력 수집 단계에서
class GamepadBackend {
public:
    // Windows는 XInput(최대 4개, 시스템 DLL을 실행 시 로드)이다. DLL을 찾지 못하면
    // DependencyFailed, 게임패드 backend가 없는 플랫폼은 Unsupported다.
    static Result<UniquePtr<GamepadBackend>> Create(Allocator& allocator) noexcept;

    virtual ~GamepadBackend() = default;

    // 연결된 패드를 폴링한다. 연결되지 않은 슬롯은 비용을 줄이려고 일정 간격(약 1초)으로만
    // 확인한다.
    virtual void Poll(input::InputQueue& queue) noexcept = 0;
};

} // namespace rocks::platform
