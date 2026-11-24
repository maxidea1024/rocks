#include "../Common/MacInputMapping.h"
#include "MacMainThread.h"

#include <Core/Memory/UniquePtr.h>
#include <Platform/Sync.h>
#include <PlatformWindow/Window.h>

// AssertMacros.h의 소문자 매크로(check·verify·require)를 만들지 않게 한다(UE와 같다).
#define __ASSERT_MACROS_DEFINE_VERSIONS_WITHOUT_UNDERSCORES 0
#include <Cocoa/Cocoa.h>
#undef __ASSERT_MACROS_DEFINE_VERSIONS_WITHOUT_UNDERSCORES

// macOS Cocoa 창이다(Docs/Design/MacPlatform.ko.md 4·5절). 창·view·delegate는 메인 스레드에서만
// 만지고(RunOnMainThread), 메인 스레드가 받은 이벤트는 창이 가진 고정 크기 큐에 넣는다. 게임
// 스레드의 PumpEvents가 그 큐를 호스트 InputQueue로 옮긴다(UE FMacApplication::DeferEvent와 같은
// 자리).
namespace rocks::platform {

class MacWindow;

} // namespace rocks::platform

// 입력을 받는 내용 view다. layer-backed이며 그래픽 backend가 layer를 CAMetalLayer로 정한다(MD7).
@interface RocksCocoaView : NSView {
@public
    rocks::platform::MacWindow* owner;
}
@end

// 창 delegate다. 닫기·크기·포커스·최소화를 창 상태로 옮긴다.
@interface RocksCocoaWindowDelegate : NSObject <NSWindowDelegate> {
@public
    rocks::platform::MacWindow* owner;
}
@end

namespace rocks::platform {

namespace {

// 메인 스레드가 게임 스레드 펌프 전까지 모아 두는 이벤트 수다. 넘치면 새 이벤트를 버리고, 다음
// 펌프에서 FocusLost를 넣어 눌린 키·버튼을 모두 놓게 한다(손실 뒤 중립 상태로 — 입력 상세
// 계획 18.3절).
constexpr usize PendingEventCapacity = 1024;

// 제목 길이 상한(UTF-16 단위)이다. Windows와 같은 규칙(WindowDesc::title)이다.
constexpr NSUInteger MaxTitleUtf16Units = 255;

// UTF-16 대리 쌍 경계다.
constexpr uint32 HighSurrogateFirst = 0xD800;
constexpr uint32 HighSurrogateLast = 0xDBFF;
constexpr uint32 LowSurrogateFirst = 0xDC00;
constexpr uint32 LowSurrogateLast = 0xDFFF;
constexpr uint32 SurrogateBase = 0x10000;
constexpr uint32 SurrogateShift = 10;

// UTF-8 제목을 NSString으로 만든다(Windows와 같은 255 UTF-16 단위, 글자 경계에서 자름). 반환값은
// autorelease 객체다. 잘못된 UTF-8은 nil이다.
NSString* TitleString(StringView title) noexcept {
    NSString* text = [[[NSString alloc] initWithBytes:title.Data()
                                               length:title.ByteCount()
                                             encoding:NSUTF8StringEncoding] autorelease];
    if (text == nil || [text length] <= MaxTitleUtf16Units) {
        return text;
    }

    const NSRange last = [text rangeOfComposedCharacterSequenceAtIndex:MaxTitleUtf16Units];
    return [text substringToIndex:last.location];
}

} // namespace

class MacWindow final : public Window {
public:
    MacWindow() noexcept = default;

    ~MacWindow() override {
        RunOnMainThread(&DestroyOnMainThread, this);
    }

    // 메인 스레드에서 창을 만든다. 실패하면 failedCall이 남는다.
    Result<void> Create(const WindowDesc& desc) noexcept {
        struct Request {
            MacWindow* window;
            const WindowDesc* desc;
            bool created;
        } request{this, &desc, false};
        RunOnMainThread(
            [](void* raw) noexcept {
                auto& value = *static_cast<Request*>(raw);
                value.created = value.window->CreateOnMainThread(*value.desc);
            },
            &request);
        return request.created ? Result<void>::Success()
                               : Result<void>::Failure(ErrorCode::DependencyFailed);
    }

    void PumpEvents(input::InputQueue& queue) noexcept override {
        mac::CycleFrameAutoreleasePool();
        LockGuard guard(mutex_);
        for (usize index = 0; index < pendingCount_; ++index) {
            (void)queue.Push(pending_[index]);
        }

        pendingCount_ = 0;
        // 넘쳐서 버린 이벤트가 있으면 마지막에 FocusLost를 넣는다. 버퍼 안 KeyDown의 짝 KeyUp이
        // 버려졌을 수 있으므로 모은 이벤트 뒤에 두어야 눌린 채 남지 않는다.
        if (overflowed_) {
            (void)queue.Push(input::InputEvent::FocusLost());
            overflowed_ = false;
        }
    }

    bool IsCloseRequested() const noexcept override {
        LockGuard guard(mutex_);
        return closeRequested_;
    }

    void ClearCloseRequest() noexcept override {
        LockGuard guard(mutex_);
        closeRequested_ = false;
    }

    uint32 Width() const noexcept override {
        LockGuard guard(mutex_);
        return width_;
    }

    uint32 Height() const noexcept override {
        LockGuard guard(mutex_);
        return height_;
    }

    bool ConsumeResize(uint32& width, uint32& height) noexcept override {
        LockGuard guard(mutex_);
        if (!resized_) {
            return false;
        }

        resized_ = false;
        width = width_;
        height = height_;
        return true;
    }

    bool HasFocus() const noexcept override {
        LockGuard guard(mutex_);
        return focused_;
    }

    bool IsMinimized() const noexcept override {
        LockGuard guard(mutex_);
        return minimized_;
    }

    NativeWindowHandle NativeHandle() const noexcept override {
        NativeWindowHandle handle;
        handle.window = view_;
        handle.instance = window_;
        return handle;
    }

    Result<void> SetTitle(StringView title) noexcept override {
        if (!title.IsValidUtf8()) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        struct Request {
            MacWindow* window;
            StringView title;
            bool applied;
        } request{this, title, false};
        RunOnMainThread(
            [](void* raw) noexcept {
                auto& value = *static_cast<Request*>(raw);
                NSString* text = TitleString(value.title);
                if (text != nil) {
                    [value.window->window_ setTitle:text];
                    value.applied = true;
                }
            },
            &request);
        return request.applied ? Result<void>::Success()
                               : Result<void>::Failure(ErrorCode::DependencyFailed);
    }

    // --- 메인 스레드 쪽(view·delegate가 부른다) ---

    void Push(const input::InputEvent& event) noexcept {
        LockGuard guard(mutex_);
        if (pendingCount_ == PendingEventCapacity) {
            overflowed_ = true;
            return;
        }

        pending_[pendingCount_++] = event;
    }

    void RequestClose() noexcept {
        LockGuard guard(mutex_);
        closeRequested_ = true;
    }

    void UpdateSize() noexcept {
        const NSRect bounds = [view_ bounds];
        const double scale = [window_ backingScaleFactor];
        LockGuard guard(mutex_);
        const uint32 width = mac::PointsToPixels(bounds.size.width, scale);
        const uint32 height = mac::PointsToPixels(bounds.size.height, scale);
        if (width != width_ || height != height_) {
            width_ = width;
            height_ = height;
            resized_ = true;
        }
    }

    void SetFocus(bool focused) noexcept {
        {
            LockGuard guard(mutex_);
            focused_ = focused;
        }

        if (!focused) {
            Push(input::InputEvent::FocusLost());
        }
    }

    void SetMinimized(bool minimized) noexcept {
        LockGuard guard(mutex_);
        minimized_ = minimized;
    }

    // view 좌표의 마우스 위치를 client 픽셀로 바꾼다.
    mac::PixelPoint PixelsOf(NSEvent* event) noexcept {
        const NSPoint point = [view_ convertPoint:[event locationInWindow] fromView:nil];
        return mac::ViewPointToPixels(point.x, point.y, [view_ bounds].size.height,
                                      [window_ backingScaleFactor]);
    }

    // keyDown의 문자를 Text 이벤트로 넣는다(UTF-16 → code point, 제어 문자·함수 키 제외).
    void PushText(NSString* characters) noexcept {
        const NSUInteger length = [characters length];
        for (NSUInteger index = 0; index < length; ++index) {
            uint32 codePoint = [characters characterAtIndex:index];
            if (codePoint >= HighSurrogateFirst && codePoint <= HighSurrogateLast &&
                index + 1 < length) {
                const uint32 low = [characters characterAtIndex:index + 1];
                if (low >= LowSurrogateFirst && low <= LowSurrogateLast) {
                    codePoint = SurrogateBase +
                                ((codePoint - HighSurrogateFirst) << SurrogateShift) +
                                (low - LowSurrogateFirst);
                    ++index;
                }
            }

            if (mac::IsTextCodePoint(codePoint)) {
                Push(input::InputEvent::Text(codePoint));
            }
        }
    }

private:
    bool CreateOnMainThread(const WindowDesc& desc) noexcept {
        NSString* title = TitleString(desc.title);
        NSScreen* screen = [NSScreen mainScreen];
        const double scale = screen != nil ? [screen backingScaleFactor] : 1.0;
        const NSRect content = NSMakeRect(0.0, 0.0, desc.width / scale, desc.height / scale);
        NSWindowStyleMask style =
            NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable;
        if (desc.resizable) {
            style |= NSWindowStyleMaskResizable;
        }

        window_ = [[NSWindow alloc] initWithContentRect:content
                                              styleMask:style
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
        if (window_ == nil) {
            failedCall = "NSWindow initWithContentRect";
            return false;
        }

        [window_ setReleasedWhenClosed:NO];
        [window_ setAcceptsMouseMovedEvents:YES];
        if (title != nil) {
            [window_ setTitle:title];
        }

        view_ = [[RocksCocoaView alloc] initWithFrame:content];
        view_->owner = this;
        [view_ setWantsLayer:YES];
        [window_ setContentView:view_];
        delegate_ = [[RocksCocoaWindowDelegate alloc] init];
        delegate_->owner = this;
        [window_ setDelegate:delegate_];
        [window_ makeFirstResponder:view_];
        [window_ center];
        // Command를 누른 채 뗀 키의 keyUp은 NSApplication이 view로 보내지 않는다. 그대로면 키가
        // 눌린 채 남으므로 앱 local monitor가 이 창의 그런 keyUp을 view로 넘긴다(5.2절).
        RocksCocoaView* view = view_;
        NSWindow* window = window_;
        keyUpMonitor_ =
            [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyUp
                                                  handler:^NSEvent*(NSEvent* event) {
                                                    if (([event modifierFlags] &
                                                         NSEventModifierFlagCommand) != 0 &&
                                                        [event window] == window) {
                                                        [view keyUp:event];
                                                    }

                                                    return event;
                                                  }];
        [keyUpMonitor_ retain];
        UpdateSize();
        {
            LockGuard guard(mutex_);
            resized_ = false;
        }

        if (desc.visible) {
            [window_ makeKeyAndOrderFront:nil];
            // macOS 14부터 협조적 활성화다. 터미널에서 띄우면 포커스를 못 얻을 수 있다(Mac 확인
            // 항목).
            if (@available(macOS 14.0, *)) {
                [NSApp activate];
            } else {
                [NSApp activateIgnoringOtherApps:YES];
            }
        }

        return true;
    }

    static void DestroyOnMainThread(void* raw) noexcept {
        auto& self = *static_cast<MacWindow*>(raw);
        if (self.keyUpMonitor_ != nil) {
            [NSEvent removeMonitor:self.keyUpMonitor_];
            [self.keyUpMonitor_ release];
            self.keyUpMonitor_ = nil;
        }

        if (self.window_ != nil) {
            [self.window_ setDelegate:nil];
            [self.window_ orderOut:nil];
            [self.window_ close];
        }

        if (self.view_ != nil) {
            self.view_->owner = nullptr;
            [self.view_ release];
            self.view_ = nil;
        }

        if (self.delegate_ != nil) {
            self.delegate_->owner = nullptr;
            [self.delegate_ release];
            self.delegate_ = nil;
        }

        if (self.window_ != nil) {
            [self.window_ release];
            self.window_ = nil;
        }
    }

public:
    // 실패한 Cocoa 호출 이름이다(Window::Create가 report로 옮긴다).
    const char* failedCall = nullptr;

private:
    // 창 상태와 이벤트 큐를 지킨다(메인 스레드가 쓰고 게임 스레드가 읽는다).
    mutable Mutex mutex_;
    input::InputEvent pending_[PendingEventCapacity];
    usize pendingCount_ = 0;
    bool overflowed_ = false;
    bool closeRequested_ = false;
    bool resized_ = false;
    bool focused_ = false;
    bool minimized_ = false;
    uint32 width_ = 0;
    uint32 height_ = 0;
    // 메인 스레드만 만진다.
    NSWindow* window_ = nil;
    RocksCocoaView* view_ = nil;
    RocksCocoaWindowDelegate* delegate_ = nil;
    id keyUpMonitor_ = nil;
};

Result<UniquePtr<Window>> Window::Create(Allocator& allocator, const WindowDesc& desc,
                                         WindowCreateReport* report) noexcept {
    auto fail = [report](ErrorCode error, const char* call) noexcept {
        if (report != nullptr) {
            *report = WindowCreateReport{};
            report->code = error;
            usize length = 0;
            while (call != nullptr && call[length] != '\0' && length + 1 < sizeof(report->call)) {
                report->call[length] = call[length];
                ++length;
            }

            report->truncated = call != nullptr && call[length] != '\0';
        }

        return Result<UniquePtr<Window>>::Failure(error);
    };

    if (desc.width == 0 || desc.height == 0 || !desc.title.IsValidUtf8()) {
        return fail(ErrorCode::InvalidArgument, nullptr);
    }

    // Cocoa 창은 메인 스레드 호스트(RunCocoaMainThreadHost)가 run loop를 도는 프로세스에서만
    // 만든다.
    if (!mac::IsCocoaHostRunning()) {
        return fail(ErrorCode::Unsupported, nullptr);
    }

    auto created = UniquePtr<MacWindow>::TryMake(allocator);
    if (!created) {
        return fail(created.Error(), nullptr);
    }

    auto opened = created.Value()->Create(desc);
    if (!opened) {
        return fail(opened.Error(), created.Value()->failedCall);
    }

    if (report != nullptr) {
        *report = WindowCreateReport{};
    }

    return Result<UniquePtr<Window>>::Success(UniquePtr<Window>(Move(created.Value())));
}

DesktopSessionState QueryDesktopSessionState() noexcept {
    return DesktopSessionState::Unknown;
}

} // namespace rocks::platform

@implementation RocksCocoaView

- (BOOL)acceptsFirstResponder {
    return YES;
}

- (BOOL)acceptsFirstMouse:(NSEvent*)event {
    (void)event;
    return YES;
}

- (void)keyDown:(NSEvent*)event {
    if (owner == nullptr) {
        return;
    }

    const rocks::input::Key key = rocks::platform::mac::KeyFromVirtualKeyCode([event keyCode]);
    if (key != rocks::input::Key::None) {
        owner->Push(rocks::input::InputEvent::KeyDown(key, [event isARepeat]));
    }

    // super를 부르지 않는다(처리하지 않은 키의 경고음을 막는다). Command·Control 조합(Cmd+C 등)은
    // 문자 입력이 아니다(Windows는 Ctrl 조합이 제어 문자로, Alt 조합은 WM_SYSCHAR로 빠진다).
    const NSEventModifierFlags shortcut = NSEventModifierFlagCommand | NSEventModifierFlagControl;
    if (([event modifierFlags] & shortcut) == 0) {
        owner->PushText([event characters]);
    }
}

- (void)keyUp:(NSEvent*)event {
    if (owner == nullptr) {
        return;
    }

    const rocks::input::Key key = rocks::platform::mac::KeyFromVirtualKeyCode([event keyCode]);
    if (key != rocks::input::Key::None) {
        owner->Push(rocks::input::InputEvent::KeyUp(key));
    }
}

- (void)flagsChanged:(NSEvent*)event {
    if (owner == nullptr) {
        return;
    }

    rocks::input::InputEvent events[rocks::platform::mac::MaxModifierEvents];
    const rocks::uint32 count = rocks::platform::mac::ModifierEvents(
        [event keyCode], static_cast<rocks::uint64>([event modifierFlags]), events);
    for (rocks::uint32 index = 0; index < count; ++index) {
        owner->Push(events[index]);
    }
}

- (void)pushButton:(NSEvent*)event down:(bool)down {
    if (owner == nullptr) {
        return;
    }

    rocks::input::MouseButton button = rocks::input::MouseButton::Left;
    if (!rocks::platform::mac::MouseButtonFromNumber([event buttonNumber], button)) {
        return;
    }

    const rocks::platform::mac::PixelPoint point = owner->PixelsOf(event);
    owner->Push(rocks::input::InputEvent::MouseMove(point.x, point.y));
    owner->Push(down ? rocks::input::InputEvent::MouseDown(button)
                     : rocks::input::InputEvent::MouseUp(button));
}

- (void)pushMove:(NSEvent*)event {
    if (owner == nullptr) {
        return;
    }

    const rocks::platform::mac::PixelPoint point = owner->PixelsOf(event);
    owner->Push(rocks::input::InputEvent::MouseMove(point.x, point.y));
}

- (void)mouseDown:(NSEvent*)event {
    [self pushButton:event down:true];
}

- (void)mouseUp:(NSEvent*)event {
    [self pushButton:event down:false];
}

- (void)rightMouseDown:(NSEvent*)event {
    [self pushButton:event down:true];
}

- (void)rightMouseUp:(NSEvent*)event {
    [self pushButton:event down:false];
}

- (void)otherMouseDown:(NSEvent*)event {
    [self pushButton:event down:true];
}

- (void)otherMouseUp:(NSEvent*)event {
    [self pushButton:event down:false];
}

- (void)mouseMoved:(NSEvent*)event {
    [self pushMove:event];
}

- (void)mouseDragged:(NSEvent*)event {
    [self pushMove:event];
}

- (void)rightMouseDragged:(NSEvent*)event {
    [self pushMove:event];
}

- (void)otherMouseDragged:(NSEvent*)event {
    [self pushMove:event];
}

// UE와 같이 deltaY(Shift면 deltaX)를 휠 값으로 쓴다. 트랙패드 스크롤도 같은 값이다(MD6).
- (void)scrollWheel:(NSEvent*)event {
    if (owner == nullptr) {
        return;
    }

    const bool shift = ([event modifierFlags] & NSEventModifierFlagShift) != 0;
    const double delta = shift ? [event deltaX] : [event deltaY];
    if (delta != 0.0) {
        owner->Push(rocks::input::InputEvent::MouseWheel(static_cast<float>(delta)));
    }
}

@end

@implementation RocksCocoaWindowDelegate

// 닫기 버튼은 창을 파괴하지 않고 닫기 요청만 세운다(Windows와 같은 계약).
- (BOOL)windowShouldClose:(NSWindow*)sender {
    (void)sender;
    if (owner != nullptr) {
        owner->RequestClose();
    }

    return NO;
}

- (void)windowDidResize:(NSNotification*)notification {
    (void)notification;
    if (owner != nullptr) {
        owner->UpdateSize();
    }
}

- (void)windowDidChangeBackingProperties:(NSNotification*)notification {
    (void)notification;
    if (owner != nullptr) {
        owner->UpdateSize();
    }
}

- (void)windowDidBecomeKey:(NSNotification*)notification {
    (void)notification;
    if (owner != nullptr) {
        owner->SetFocus(true);
    }
}

- (void)windowDidResignKey:(NSNotification*)notification {
    (void)notification;
    if (owner != nullptr) {
        owner->SetFocus(false);
    }
}

- (void)windowDidMiniaturize:(NSNotification*)notification {
    (void)notification;
    if (owner != nullptr) {
        owner->SetMinimized(true);
    }
}

- (void)windowDidDeminiaturize:(NSNotification*)notification {
    (void)notification;
    if (owner != nullptr) {
        owner->SetMinimized(false);
    }
}

@end
