#include "MacMainThread.h"

#include <Launch/Application.h>
#include <Platform/Atomics.h>
#include <Platform/Fatal.h>
#include <Platform/Thread.h>
#include <PlatformWindow/Window.h>

// AssertMacros.h의 소문자 매크로(check·verify·require)를 만들지 않게 한다(UE와 같다).
#define __ASSERT_MACROS_DEFINE_VERSIONS_WITHOUT_UNDERSCORES 0
#include <Cocoa/Cocoa.h>
#include <dispatch/dispatch.h>
#include <stdlib.h>
#undef __ASSERT_MACROS_DEFINE_VERSIONS_WITHOUT_UNDERSCORES

// objc runtime이 내보내는 autorelease pool 함수다(공개 헤더에 선언이 없다). 프레임 단위 pool은 블록
// 문법(@autoreleasepool)으로 표현할 수 없어 직접 부른다.
extern "C" void* objc_autoreleasePoolPush(void);
extern "C" void objc_autoreleasePoolPop(void* pool);

namespace rocks::platform {

namespace {

// 게임 스레드 스택이다. UE와 같이 Debug 64 MB, 그 밖 128 MB다(CocoaThread.cpp:11–16). macOS 보조
// 스레드 기본값(512 KB)으로는 주 스레드(8 MB)에서 돌던 본문을 감당하지 못한다.
#if ROCKS_BUILD_DEBUG
constexpr usize GameThreadStackBytes = usize{64} << 20;
#else
constexpr usize GameThreadStackBytes = usize{128} << 20;
#endif

constexpr char GameThreadName[] = "GameThread";

// 호스트 상태다. 프로세스에 호스트는 하나다.
struct HostState {
    int (*body)(void*) noexcept = nullptr;
    void* context = nullptr;
    int exitCode = 0;
    // 메인 스레드가 run loop를 도는 동안 1이다(AtomicLoad·AtomicStore로만 읽고 쓴다).
    uint32 running = 0;
    // 게임 스레드의 본문이 돌아온 뒤 1이다. 그 뒤 RunOnMainThread는 Fatal이다(메인 스레드가 join
    // 중).
    uint32 closed = 0;
    // 게임 스레드의 프레임 autorelease pool이다(게임 스레드만 만진다). pool은 LIFO라 창마다 두지
    // 않고 스레드에 하나만 둔다.
    void* framePool = nullptr;
    // 시스템 종료 요청(로그아웃·Dock 종료)에 NSTerminateLater를 돌려주고 아직 응답하지 않았다(메인
    // 스레드만 만진다).
    bool terminatePending = false;
    // 게임 스레드다. applicationWillTerminate:가 join한다(메인 스레드만 만진다).
    Thread* gameThread = nullptr;
};

HostState hostState;

// 게임 스레드를 join한다(메인 스레드). 실패는 Fatal이다.
void JoinGameThread() noexcept {
    if (hostState.gameThread != nullptr && hostState.gameThread->IsJoinable() &&
        hostState.gameThread->Join() != ThreadStatus::Success) {
        Fatal("RunCocoaMainThreadHost could not join the game thread");
    }
}

// 메인 스레드에서 run loop를 멈춘다(게임 스레드 본문이 끝난 뒤). 시스템 종료 요청에 응답을 미뤄
// 두었으면 지금 YES로 응답한다 — AppKit은 그 응답을 모달 run loop에서 기다리고 있어 [NSApp
// stop:]으로는 바깥 run이 돌아오지 않는다. YES 뒤에는 AppKit이 applicationWillTerminate:를 거쳐
// 프로세스를 끝낸다. 그 밖에는 stop이 다음 이벤트 처리 뒤에 효과가 있으므로 빈 이벤트를 하나
// 넣는다.
void StopRunLoop(void*) noexcept {
    if (hostState.terminatePending) {
        hostState.terminatePending = false;
        [NSApp replyToApplicationShouldTerminate:YES];
        return;
    }

    [NSApp stop:nil];
    NSEvent* wake = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                       location:NSZeroPoint
                                  modifierFlags:0
                                      timestamp:0
                                   windowNumber:0
                                        context:nil
                                        subtype:0
                                          data1:0
                                          data2:0];
    [NSApp postEvent:wake atStart:YES];
}

// 게임 스레드 본문이다. 본문 전체를 바깥 autorelease pool로 감싸고, 돌아오면 프레임 pool·바깥
// pool을 닫고 닫힘 표시를 세운 뒤 마지막 동작으로 메인 큐에 정지를 넣는다.
void RunGameThread(void* raw) noexcept {
    auto& state = *static_cast<HostState*>(raw);
    void* pool = objc_autoreleasePoolPush();
    state.exitCode = state.body(state.context);
    if (state.framePool != nullptr) {
        objc_autoreleasePoolPop(state.framePool);
        state.framePool = nullptr;
    }

    objc_autoreleasePoolPop(pool);
    AtomicStore(&state.closed, uint32{1});
    dispatch_async_f(dispatch_get_main_queue(), nullptr, &StopRunLoop);
}

// RunOnMainThread가 메인 큐로 넘기는 호출이다.
struct MainThreadCall {
    void (*function)(void*) noexcept = nullptr;
    void* context = nullptr;
};

void RunMainThreadCall(void* raw) noexcept {
    @autoreleasepool {
        auto& call = *static_cast<MainThreadCall*>(raw);
        call.function(call.context);
    }
}

} // namespace

} // namespace rocks::platform

// 앱 delegate다(Docs/Design/MacPlatform.ko.md 3절).
// - Quit 메뉴(Cmd+Q)는 terminate:가 아니라 requestQuit:로 Launch 종료 요청 다리만 부른다. 엔진이
// 다음
//   프레임에 정상 종료하고, 호스트가 돌아온 뒤 Launch 정리까지 돈다.
// - 시스템 종료 요청(로그아웃·재시동·Dock 종료)은 종료 요청 다리로 잇고 늘 NSTerminateLater를
//   돌려준다. 본문이 끝나면 StopRunLoop이 YES로 응답하고, applicationWillTerminate:가 게임 스레드를
//   join한 뒤 본문 종료 코드로 끝낸다. 이 경로에서는 호스트 뒤의 Launch 정리가 돌지 않는다(AppKit이
//   프로세스를 끝낸다 — UE도 같다). NSTerminateCancel은 로그아웃을 취소시키므로 쓰지 않는다(UE
//   LaunchMac.cpp:229–242).
@interface RocksApplicationDelegate : NSObject <NSApplicationDelegate>
- (void)requestQuit:(id)sender;
@end

@implementation RocksApplicationDelegate

- (void)requestQuit:(id)sender {
    (void)sender;
    rocks::launch::RequestTermination();
}

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
    (void)sender;
    rocks::platform::hostState.terminatePending = true;
    rocks::launch::RequestTermination();
    // 본문이 이미 끝나 정지가 메인 큐에 들어가 있으면 그 정지가 곧 응답한다.
    return NSTerminateLater;
}

- (void)applicationWillTerminate:(NSNotification*)notification {
    (void)notification;
    rocks::platform::JoinGameThread();
    exit(rocks::platform::hostState.exitCode);
}

@end

namespace rocks::platform {

namespace {

// Quit(Cmd+Q) 하나짜리 앱 메뉴를 단다. 대상은 delegate의 requestQuit:다.
void InstallApplicationMenu(RocksApplicationDelegate* delegate) noexcept {
    NSMenu* mainMenu = [[NSMenu alloc] initWithTitle:@""];
    NSMenuItem* applicationItem = [[NSMenuItem alloc] initWithTitle:@""
                                                             action:nil
                                                      keyEquivalent:@""];
    NSMenu* applicationMenu = [[NSMenu alloc] initWithTitle:@""];
    NSString* quitTitle =
        [@"Quit " stringByAppendingString:[[NSProcessInfo processInfo] processName]];
    NSMenuItem* quitItem = [[NSMenuItem alloc] initWithTitle:quitTitle
                                                      action:@selector(requestQuit:)
                                               keyEquivalent:@"q"];
    [quitItem setTarget:delegate];
    [applicationMenu addItem:quitItem];
    [applicationItem setSubmenu:applicationMenu];
    [mainMenu addItem:applicationItem];
    [NSApp setMainMenu:mainMenu];
    [quitItem release];
    [applicationMenu release];
    [applicationItem release];
    [mainMenu release];
}

} // namespace

namespace mac {

bool IsCocoaHostRunning() noexcept {
    return AtomicLoad(&hostState.running) != 0;
}

void CycleFrameAutoreleasePool() noexcept {
    if (AtomicLoad(&hostState.running) == 0 || [NSThread isMainThread]) {
        return;
    }

    if (hostState.framePool != nullptr) {
        objc_autoreleasePoolPop(hostState.framePool);
    }

    hostState.framePool = objc_autoreleasePoolPush();
}

} // namespace mac

int RunCocoaMainThreadHost(int (*body)(void*) noexcept, void* context) noexcept {
    if (![NSThread isMainThread]) {
        Fatal("RunCocoaMainThreadHost must run on the process main thread");
    }

    if (AtomicLoad(&hostState.running) != 0) {
        Fatal("RunCocoaMainThreadHost is already running");
    }

    RocksApplicationDelegate* delegate = nil;
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        delegate = [[RocksApplicationDelegate alloc] init];
        [NSApp setDelegate:delegate];
        InstallApplicationMenu(delegate);
        [NSApp finishLaunching];
    }

    Thread gameThread;
    hostState.body = body;
    hostState.context = context;
    hostState.exitCode = 0;
    hostState.framePool = nullptr;
    hostState.terminatePending = false;
    hostState.gameThread = &gameThread;
    AtomicStore(&hostState.closed, uint32{0});
    AtomicStore(&hostState.running, uint32{1});
    ThreadOptions options;
    options.name = GameThreadName;
    options.stackBytes = GameThreadStackBytes;
    if (gameThread.Start(&RunGameThread, &hostState, options) != ThreadStatus::Success) {
        Fatal("RunCocoaMainThreadHost could not start the game thread");
    }

    @autoreleasepool {
        [NSApp run];
    }

    JoinGameThread();
    hostState.gameThread = nullptr;
    AtomicStore(&hostState.running, uint32{0});
    [NSApp setDelegate:nil];
    [delegate release];
    return hostState.exitCode;
}

void RunOnMainThread(void (*function)(void*) noexcept, void* context) noexcept {
    if ([NSThread isMainThread] || AtomicLoad(&hostState.running) == 0) {
        function(context);
        return;
    }

    if (AtomicLoad(&hostState.closed) != 0) {
        Fatal("RunOnMainThread was called after the game thread body returned");
    }

    // GCD 메인 큐는 run loop common modes에서 처리되므로 창 크기 조절 같은 모달 추적 중에도 막히지
    // 않는다.
    MainThreadCall call{function, context};
    dispatch_sync_f(dispatch_get_main_queue(), &call, &RunMainThreadCall);
}

} // namespace rocks::platform
