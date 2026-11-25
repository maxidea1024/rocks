#pragma once

#include <Core/Delegate.h>
#include <Core/Diagnostics/LogCategory.h>
#include <Core/Math/IntVector.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Text/StringView.h>
#include <Input/InputEvent.h>

namespace rocks::platform {

// 창 생성 설정이다.
struct WindowDesc {
    // UTF-8 제목이다. 창이 복사한다(UTF-16 255단위를 넘으면 글자 경계에서 자른다). 잘못된 UTF-8은
    // InvalidArgument다.
    StringView title = "Rocks";
    // client 영역 크기(픽셀)다. 0이면 InvalidArgument다.
    uint32 width = 1280;
    uint32 height = 720;
    bool resizable = true;
    bool visible = true;
    bool useDesktopPosition = false;
    IntVec2 desktopPosition{};
    bool positionClientOrigin = false;
};

// 창 계층의 로그 카테고리다(Docs/Design/LoggingSystem.ko.md 13절). 창 생성 실패는 창을 여는
// 호출자가 WindowCreateReport로 이 카테고리에 남긴다.
inline constexpr LogCategory LogPlatformWindow{"Platform.Window"};

// 창 생성 실패의 원인 기록이다(DeviceCreateReport와 같은 모양). 호출자 스택에 두고 Create에 넘긴다.
// 힙을 쓰지 않으며 call은 실패한 OS 호출 이름(영어)이고 넘치면 자른다(truncated).
//
// 사용 예:
//   platform::WindowCreateReport report;
//   auto window = platform::Window::Create(allocator, desc, &report);
//   if (!window) {
//       Log::Error(LogPlatformWindow, "Window creation failed at {Call}: {OsError}", report.call,
//                  report.osError);
//   }
struct WindowCreateReport {
    // Create가 돌려준 오류와 같다(성공이면 None).
    ErrorCode code = ErrorCode::None;
    // 원본 OS 오류다(Windows GetLastError). 없으면 0이다.
    int64 osError = 0;
    // 실패한 OS 호출 이름이다(예: "CreateWindowExW").
    char call[48]{};
    bool truncated = false;
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
class ROCKS_PLATFORMWINDOW_API Window {
public:
    // 창을 만든다. 크기 0·잘못된 UTF-8 제목은 InvalidArgument(창을 만들기 전에 거른다), 창
    // backend가 없는 플랫폼은 Unsupported, OS 실패는 DependencyFailed다. report가 null이 아니면
    // 결과를 채운다(OS 실패면 실패한 호출과 GetLastError). 실패 로그는 호출자가 남긴다.
    static Result<UniquePtr<Window>> Create(Allocator& allocator, const WindowDesc& desc,
                                            WindowCreateReport* report = nullptr) noexcept;

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

    // 노출·복원 등으로 표시 내용만 다시 제출할 필요가 있는지 소비한다.
    virtual bool ConsumeRepaint() noexcept;
    // 클라이언트 영역과 이 창이 마우스를 캡처한 동안 사용할 커서다.
    using RegionHitTest = Delegate<input::WindowRegion(IntVec2)>;
    // 클라이언트 좌표를 UI의 영역으로 바꾼다. 호스트를 파괴하기 전에 빈 콜백으로 해제한다.
    virtual void SetRegionHitTest(RegionHitTest&& hitTest) noexcept;
    virtual void SetMouseCursor(input::MouseCursor cursor) noexcept;
    // 입력 구동 호스트가 다른 창을 포함한 스레드 메시지 또는 제한 시간까지 대기한다.
    virtual void WaitForEvents(uint32 milliseconds) noexcept;

    virtual bool HasFocus() const noexcept = 0;

    // 최소화되어 그릴 영역이 없는지다. 렌더러는 이때 present를 건너뛴다.
    virtual bool IsMinimized() const noexcept = 0;

    virtual NativeWindowHandle NativeHandle() const noexcept = 0;

    // 부동 창의 드롭 위치를 정할 때 사용하는 화면 좌표 변환과 최상위 창 판정이다.
    virtual Result<IntVec2> ClientToDesktop(IntVec2 point) const noexcept;
    virtual Result<IntVec2> DesktopToClient(IntVec2 point) const noexcept;
    virtual bool IsAtDesktopPoint(IntVec2 point) const noexcept;

    // 제목을 바꾼다(UTF-8, WindowDesc::title과 같은 자름 규칙). 잘못된 UTF-8은 InvalidArgument이고
    // 제목은 그대로다. OS 실패는 DependencyFailed다.
    virtual Result<void> SetTitle(StringView title) noexcept = 0;

    // 렌더 surface를 잃었는지다(Android: 창이 사라진다 — Docs/Design/AndroidPlatform.ko.md 4절).
    // true를 돌려받은 호스트는 그 창의 swapchain·EGL window surface를 놓고 AcknowledgeSurfaceLost를
    // 부른다. 그 전까지 OS 콜백이 기다린다(제한 시간 4초). 데스크톱은 늘 false다. 게임 스레드에서
    // 부른다.
    virtual bool ConsumeSurfaceLost() noexcept {
        return false;
    }

    // 렌더가 잃은 surface를 놓았다고 알린다(ConsumeSurfaceLost 뒤). 데스크톱은 아무것도 하지
    // 않는다.
    virtual void AcknowledgeSurfaceLost() noexcept {
    }

    // 렌더러가 없는 호스트(Sandbox 헤드리스 창·시험)가 창에 빈(검은) 프레임 한 장을 낸다. Android는
    // 창 surface에 한 번도 프레임이 오지 않으면 창을 그려지지 않은 것으로 보고 입력을 보내지 않아
    // ANR이 되므로(Docs/Design/AndroidPlatform.ko.md 4절), 새 surface마다(ConsumeResize가 true일
    // 때) 부른다. Android는 CPU로 그리므로 그 surface에는 뒤에 GPU swapchain을 만들 수 없다 —
    // 렌더러가 있는 호스트는 부르지 않는다. 데스크톱은 아무것도 하지 않는다. 게임 스레드에서
    // 부른다.
    virtual void PresentBlankFrame() noexcept {
    }
};

// 데스크톱 위를 따라다니는 드래그 이미지다. 입력과 포커스는 원래 창에 남는다.
class ROCKS_PLATFORMWINDOW_API DragImage {
public:
    static Result<UniquePtr<DragImage>> Create(Allocator& allocator, const Window& owner,
                                               IntVec2 position, IntVec2 size,
                                               Span<const uint8> rgba, uint8 opacity) noexcept;
    virtual ~DragImage() = default;
    virtual void MoveTo(IntVec2 position) noexcept = 0;
};

// 게임패드 입력 backend다. 폴링 결과를 GamepadState 이벤트로 큐에 넣는다. 상태가 바뀐
// 패드(연결·해제 포함)만 이벤트를 만든다.
//
// 사용 예:
//   auto gamepads = platform::GamepadBackend::Create(allocator);
//   gamepads.Value()->Poll(queue);               // 프레임마다 입력 수집 단계에서
class ROCKS_PLATFORMWINDOW_API GamepadBackend {
public:
    // Windows는 XInput(최대 4개, 시스템 DLL을 실행 시 로드)이다. DLL을 찾지 못하면
    // DependencyFailed, 게임패드 backend가 없는 플랫폼은 Unsupported다.
    static Result<UniquePtr<GamepadBackend>> Create(Allocator& allocator) noexcept;

    virtual ~GamepadBackend() = default;

    // 연결된 패드를 폴링한다. 연결되지 않은 슬롯은 비용을 줄이려고 일정 간격(약 1초)으로만
    // 확인한다.
    virtual void Poll(input::InputQueue& queue) noexcept = 0;
};

// 이 프로세스가 도는 데스크톱 세션이 표시 장치에 연결되어 있는지다(QueryDesktopSessionState).
enum class DesktopSessionState : uint8 {
    // 사용자가 세션을 보고 있다(콘솔 또는 원격 연결이 붙어 있다).
    Connected,
    // 세션은 살아 있으나 표시 장치가 떨어졌다(원격 연결을 끊었거나 사용자를 전환했다).
    Disconnected,
    // 판정할 수 없다(조회 실패, 세션 개념이 없는 플랫폼).
    Unknown
};

// 이 프로세스의 데스크톱 세션 연결 상태다. Windows는 WTSQuerySessionInformation(WTSConnectState)로
// 묻는다: WTSActive는 Connected, WTSDisconnected는 Disconnected, 그 밖과 조회 실패는 Unknown이다.
// 다른 플랫폼은 Unknown이다. 어느 스레드에서나 부를 수 있고, 상태는 언제든 바뀔 수 있으므로 쓰는
// 지점에서 다시 묻는다.
//
// 끊긴 세션에서는 숨긴 창 DC로의 GDI 그리기(StretchBlt)가 실패한다. GDI로 창에 내보내는 소프트웨어
// 경로(lavapipe의 Windows WSI)는 이 상태에서 창 출력을 할 수 없다. DXGI·WGL과 하드웨어 Vulkan
// 드라이버의 present는 영향을 받지 않는다(Docs/Design/VulkanGraphics.ko.md 17절).
//
// 사용 예:
//   if (platform::QueryDesktopSessionState() == platform::DesktopSessionState::Disconnected) {
//       // GDI 기반 창 출력 검사는 근거를 남기고 건너뛴다
//   }
ROCKS_PLATFORMWINDOW_API DesktopSessionState QueryDesktopSessionState() noexcept;

} // namespace rocks::platform
