# macOS 실행 기반 — Cocoa 창·입력·Objective-C++ 빌드

개정 2 (2026-11-24, 개정 1에 UE·API 교차 검증 반영). ShaderSystem 선행 P12(`ShaderSystem/05-Prerequisites.ko.md`)의 설계다.

- 사용자 결정(2026-11-24): 구현까지 해 두고, 확인은 나중에 사용자의 Mac 기기에서 한다. 이 PC(Windows)에서는 macOS 빌드도 실행도 할 수 없다.
- 그래서 구현은 별도 브랜치에 두고, Mac에서 빌드·실행을 확인한 뒤 engine-runtime에 병합한다(8절).

## 1. 목적과 범위

macOS는 Windows와 같은 급의 정식 개발 플랫폼이다(EngineImplementationPlan 188행). 그런데 지금은 창이 없다. `PlatformWindow`는 Windows만 `Private/Win64`를 빌드하고 나머지는 `Private/Unsupported`다.

범위:
- Rocks.Build: Objective-C++(`.mm`) 컴파일과 Apple framework 링크.
- Launch: 본문을 다른 스레드에서 돌리는 hook. PlatformWindow: Cocoa 메인 스레드와 게임 스레드를 나누는 호스트.
- PlatformWindow Mac backend:
  - 창 하나: 생성·닫기·크기·포커스·최소화·Retina 배율
  - 키보드·마우스·문자 입력
  - 게임패드(GameController framework)
  - 메인 스레드 호출 API
- 그래픽 backend가 `CAMetalLayer`를 붙일 수 있는 `NSView` 핸들.

범위 밖:
- MetalGraphics(P15, SH5)
- IME 조합(PW3)
- 다중 창(PW1)
- clipboard·cursor 모양(PW4)
- 메뉴 막대 전체(Quit만 둔다)
- 접근성(PW8)
- `.app` 번들 배포·공증(배포 단계)
- 트랙패드 제스처 이벤트(TouchInput TD12)

## 2. UE 5.8 기준

경로는 `C:\github\UnrealEngine\Engine\Source` 기준이다.

| 항목 | UE 5.8 | 내용 |
|---|---|---|
| 스레드 | `Runtime/Core/Public/Mac/CocoaThread.h:7, 43, 97`, `Runtime/Core/Private/Mac/CocoaThread.cpp:11–16`(게임 스레드 스택 Debug 64 MB·그 밖 128 MB), `Runtime/Launch/Private/Mac/LaunchMac.cpp:229–242, 449` | `MAC_SEPARATE_GAME_THREAD 1`. 메인 스레드는 `NSApplication` run loop를 돌고, 엔진은 `RunGameThread`가 만든 별도 게임 스레드(`FCocoaGameThread`)에서 돈다. 메인 스레드 작업은 `MainThreadCall(block, wait)`, 반대 방향은 `GameThreadCall` |
| 이벤트 | `Runtime/ApplicationCore/Private/Mac/MacApplication.cpp:437, 602, 861` | 메인 스레드가 `NSEvent`와 창 알림을 `DeferEvent`로 큐(mutex)에 넣는다. 게임 스레드가 `ProcessDeferredEvents`에서 꺼내 처리한다 |
| 창 | `Private/Mac/CocoaWindow.cpp`, `MacWindow.cpp:59, 246` | `FCocoaWindow : NSWindow`. 위치·크기는 `backingScaleFactor`(고해상도 모드)를 곱한 픽셀이다 |
| Metal layer | `Runtime/Apple/MetalRHI/Private/MetalViewport.cpp:247–272` | 창이 아니라 RHI viewport가 `FMetalView`와 `CAMetalLayer`를 만들고 `setContentView`로 창의 내용 view를 바꾼다 |
| 키보드 | `MacApplication.cpp:694, 1351` | `[event keyCode]`(하드웨어 `kVK_*`)를 쓴다. 문자 입력은 `FCocoaTextView`의 `interpretKeyEvents`·`insertText` |
| 휠 | `MacApplication.cpp:673–675, 1305–1322` | 마우스 휠은 `[event deltaY]`(Shift면 X)이다. 트랙패드처럼 phase가 있는 스크롤은 `OnTouchGesture(Scroll)`로 따로 보낸다 |
| 게임패드 | `Private/Apple/AppleControllerInterface.cpp:149–172` | `GCController` 연결·해제 알림, `extendedGamepad` 값을 읽는다(옛 HID 경로는 10.14 이하용) |
| Objective-C++ | `Programs/UnrealBuildTool/Platform/Apple/AppleToolChain.cs:459–476` | Apple 대상에서는 `.cpp`와 `.mm`를 모두 `-x objective-c++`로 컴파일한다. ARC는 모듈 규칙(`bEnableObjCAutomaticReferenceCounting`, 기본 꺼짐) |

## 3. 스레드 모델 (Launch hook + PlatformWindow)

UE와 같이 메인 스레드와 게임 스레드를 나눈다(MD1). Cocoa는 창·이벤트를 메인 스레드에서만 받는다. 창 크기 조절이나 메뉴 추적 중에는 메인 스레드가 모달 루프에 들어가는데, 그동안에도 게임 프레임이 멈추지 않게 하려는 것이다.

- **위치:**
  - Launch(Core)는 AppKit을 모른다. 콘솔 도구가 AppKit을 링크하지 않게 하려는 것이다. Launch에는 hook `ApplicationSettings::mainThreadHost`(`MainThreadHostFunction`)만 둔다. 이 hook은 본문을 다른 스레드에서 돌리고 그 반환값을 돌려준다.
  - Cocoa run loop와 `RunOnMainThread`는 PlatformWindow(Engine)의 `platform::RunCocoaMainThreadHost`에 둔다(MD8).
  - 창을 쓰는 프로그램이 `Configure`에서 hook을 넣는다. Sandbox는 `--window`·`--self-test`일 때 넣는다.
  - 다른 플랫폼에서 `RunCocoaMainThreadHost`는 본문을 바로 부른다.
  - hook이 있으면 `ApplicationContext`는 본문 스레드(게임 스레드)에서 쓴다(Application.h 계약 개정).
- **`RunCocoaMainThreadHost`의 순서(macOS):**
  1. 메인 스레드에서 `NSApplication` 생성, `setActivationPolicy:Regular`, Quit 하나짜리 메뉴, `finishLaunching`.
  2. 게임 스레드(`platform::Thread`, 이름 "GameThread")를 띄워 본문을 돌린다. 스택은 UE와 같이 Debug 64 MB, 그 밖 128 MB다(`CocoaThread.cpp:11–16`, 이름 있는 상수). macOS 보조 스레드 기본값 512 KB로는 지금 8 MB 주 스레드에서 돌던 본문을 감당하지 못한다.
  3. 메인 스레드는 `[NSApp run]`을 돈다.
  4. 본문이 돌아오면 게임 스레드는 "메인 스레드 닫힘" 표시를 세운다. 그다음 마지막 동작으로 메인 큐에 정지 블록(`[NSApp stop:]`와 빈 이벤트 하나)을 넣고 끝난다.
  5. 메인 스레드는 run loop를 빠져나와 게임 스레드를 join하고 본문 반환값을 돌려준다. 그 뒤 Launch 정리(모듈 내리기 등)는 메인 스레드에서 돈다.
- **Quit:** Quit 메뉴(Cmd+Q)와 로그아웃·재시동의 `applicationShouldTerminate:`는 `launch::RequestTermination()`으로 잇고 `NSTerminateLater`를 돌려준다(UE `LaunchMac.cpp:229–242`). 게임 스레드가 끝나 run loop를 빠져나올 때 `replyToApplicationShouldTerminate:YES`를 부른다. `NSTerminateCancel`은 로그아웃을 취소시키므로 쓰지 않는다.
- **메인 스레드 호출:** `platform::RunOnMainThread(function, context)`(PlatformWindow 공개 API, 동기)를 둔다.
  - 구현은 `dispatch_sync_f(dispatch_get_main_queue(), …)`다. GCD 메인 큐는 run loop common modes에서 처리되므로 창 크기 조절 같은 모달 추적 중에도 막히지 않는다.
  - 메인 스레드에서 부르면 바로 실행한다. 다른 플랫폼에서도 바로 실행한다.
  - 교착 방지 규칙:
    - 게임 스레드가 메인 스레드를 기다리는 것만 허용한다. 메인 스레드는 게임 스레드를 join 말고는 기다리지 않는다.
    - "메인 스레드 닫힘" 뒤 게임 스레드가 부르면 Fatal이다(종료 중 창 파괴 순서 오류를 드러낸다).
    - 본문이 돌아가기 전에 창을 모두 파괴하는 것은 호스트 계약이다. 엔진 호스트의 `Window`는 이미 `RunEngine` 범위 안에서 사라진다.
- **꺼져 있을 때:** 콘솔 도구·시험 실행기는 지금처럼 메인 스레드 하나다. 이때 `Window::Create`는 Unsupported를 돌려준다(Cocoa run loop가 없으므로).
- **autorelease pool:**
  - 게임 스레드 본문 전체를 바깥 pool로 감싼다.
  - 프레임마다 `Window::PumpEvents`가 직전 프레임 pool을 비우고 새로 연다(`objc_autoreleasePoolPop`·`Push`). `[GCController controllers]`, 이후 `CAMetalLayer nextDrawable`처럼 autorelease 객체를 돌려주는 호출이 프레임마다 쌓이지 않게 한다.
  - 메인 스레드는 `RunOnMainThread` 블록과 이벤트 처리를 각각 pool 안에서 돈다.
- **크래시 수집:** POSIX 신호 처리기는 프로세스 전역이라 스레드가 둘이 되어도 경로가 같다(`RunWithCrashGuard`는 POSIX에서 그냥 부른다). 게임 스레드는 `platform::Thread`라 스레드 이름·보호 프레임이 같이 걸린다.

## 4. 창과 이벤트 (PlatformWindow `Private/Mac`)

- **창 생성:** `Window::Create`는 게임 스레드에서 부른다. 내부에서 `RunOnMainThread`로 다음을 만든다.
  - `NSWindow` 파생 `RocksCocoaWindow`, 내용 view `RocksCocoaView`(layer-backed, `acceptsFirstResponder`), delegate.
  - 생성 뒤 `makeKeyAndOrderFront`와 앱 활성화를 한다. 활성화는 macOS 14 이상이면 `[NSApp activate]`, 그 전이면 `activateIgnoringOtherApps:YES`(14에서 deprecated)다. macOS 14의 협조적 활성화 때문에 터미널에서 띄우면 키 포커스를 못 얻을 수 있어, Mac 확인 항목에 넣는다.
  - `WindowDesc`의 크기는 픽셀이다. 점(point) 크기는 그 화면의 `backingScaleFactor`로 나눈다.
- **이벤트 수집:** 메인 스레드의 view·delegate가 받은 것을 `input::InputEvent`로 바꿔 창이 가진 고정 크기 큐(mutex 보호, 용량은 이름 있는 상수, 넘치면 손실 수 증가)에 넣는다. UE `DeferEvent`와 같은 자리다.
- **펌프:** `Window::PumpEvents(queue)`는 게임 스레드에서 그 큐를 꺼내 호스트 `InputQueue`로 옮긴다. 넘쳐서 버린 수는 `InputQueue` 손실과 같이 센다(TouchInput 3.3절의 손실 복구 규칙이 그대로 적용된다).
- **창 상태:** 닫기 요청·크기·포커스·최소화는 메인 스레드가 쓰고 게임 스레드가 읽는다(같은 mutex).

| Cocoa 신호 | 상태·이벤트 |
|---|---|
| `windowShouldClose:` | `NO`를 돌려주고 닫기 요청을 세운다(창 파괴는 `Window` 소멸 — Windows와 같은 계약) |
| `windowDidResize:`·`windowDidChangeBackingProperties:` | 크기를 픽셀(`convertRectToBacking`)로 갱신, `ConsumeResize`가 알린다 |
| `windowDidBecomeKey:`·`windowDidResignKey:` | 포커스, 잃으면 `FocusLost` 이벤트 |
| `windowDidMiniaturize:`·`windowDidDeminiaturize:` | `IsMinimized` |
| `NSApplicationDidBecomeActive`·`DidResignActive` | 앱 수명 `Activated`·`Deactivated`(PlatformLayer 앱 수명 절. 전달은 그 절의 큐) |

- **소멸:** `Window` 소멸은 `RunOnMainThread`로 delegate를 떼고 창을 닫는다.
- **수동 메모리 관리(MRC):** `RunOnMainThread` 블록과 메인 스레드 이벤트 처리는 `@autoreleasepool` 안에서 돈다. ARC는 쓰지 않는다(UE 기본과 같다 — MD4).
- **`NativeHandle()`:** `window = NSView*`(내용 view), `instance = NSWindow*`다. 그래픽 backend는 `RunOnMainThread`로 그 view의 layer를 `CAMetalLayer`로 정한다(`setLayer`·`wantsLayer`).
  - 내용 view(입력을 받는 view)는 바꾸지 않는다. UE는 `FMetalView`를 새로 만들어 `setContentView`로 바꾸지만(`MetalViewport.cpp:247–272`), Rocks는 창이 입력 view를 소유하므로 layer만 붙인다(MD7).
  - backing 배율이 바뀌면(`windowDidChangeBackingProperties:` → `ConsumeResize`) 그래픽 backend가 `contentsScale`·`drawableSize`를 갱신한다.
  - Metal 설정은 MetalGraphics(P15)가 맡는다.
- **`DesktopSessionState`:** macOS는 Unknown이다(지금 계약 그대로).

## 5. 입력 변환

### 5.1 좌표

view 좌표(점, 좌하단 원점)를 `convertPointToBacking`으로 픽셀로 바꾸고 Y를 뒤집어 client 영역 좌상단 원점으로 만든다. Windows client 픽셀과 같은 의미다.

### 5.2 키보드

- **키:** `[event keyCode]`(`kVK_*`, 물리 위치)를 `input::Key`로 바꾸는 표를 둔다. Windows 스캔코드 표와 같은 물리 배치 의미다. 표에 없는 키는 버린다.
- **modifier:** `flagsChanged:`의 `keyCode`(`kVK_Shift`·`kVK_RightShift` 등)로 바뀐 키를 정하고, 그 키의 장치 의존 비트(UE처럼 IOKit 이름 대신 비트 값 — `MacApplication.cpp:1893–1897`)가 켜졌는지로 down·up을 정한다.
  - CapsLock의 `flagsChanged`는 물리 down·up이 아니라 토글 상태다. 이벤트마다 down과 up을 함께 만든다.
- **반복:** `[event isARepeat]`는 `repeat` 표지다.
- **문자:** `keyDown:`에서 `[event characters]`의 각 code point를 `Text` 이벤트로 넣는다. 제어 문자(0x20 미만, 0x7F)와 함수 키 사적 영역(U+F700–U+F8FF)은 뺀다. 조합(한글·일본어)은 PW3에서 `NSTextInputClient`로 바꾼다(MD5).
- **시스템 단축키:** Command 조합 중 메뉴가 처리한 Cmd+Q 외에는 view까지 오면 키 이벤트로 넘긴다.
- **Command 중 keyUp:** Command를 누른 채 뗀 키의 keyUp은 NSApplication이 view로 보내지 않는다. 그대로 두면 키가 눌린 채 남는다. 앱 local monitor(`addLocalMonitorForEventsMatchingMask:NSEventMaskKeyUp`)가 Command가 켜진 keyUp을 key window의 view로 넘긴다.

### 5.3 마우스

- **버튼:** `mouseDown`·`rightMouseDown`·`otherMouseDown`(`buttonNumber` 2 = Middle, 3·4 = X1·X2)과 각 Up을 받는다. 누른 채 창 밖으로 나가도 Cocoa가 Up을 같은 view로 보내므로 capture가 따로 필요 없다.
- **이동:** `mouseMoved`·`*Dragged`는 `MouseMove`다(`acceptsMouseMovedEvents = YES`).
- **휠:** `scrollWheel`은 UE와 같이 `[event deltaY]`(Shift면 `deltaX`)를 휠 값으로 쓴다. UE는 phase가 있는 트랙패드 스크롤을 제스처 이벤트로 돌리지만, Rocks는 OS 제스처를 범위 밖에 두므로 트랙패드도 같은 `deltaY` 휠로 보낸다(MD6). 방향은 OS "자연스러운 스크롤" 설정을 따른 값 그대로다.

### 5.4 게임패드

- `GamepadBackend::Create`가 `GCControllerDidConnectNotification`·`DidDisconnectNotification`을 메인 스레드에 등록하고, 이미 연결된 `[GCController controllers]`를 슬롯 0–3에 앉힌다.
- `Poll`(게임 스레드)이 슬롯마다 `extendedGamepad` 값을 읽어 `GamepadSnapshot`을 만들고, 바뀐 것만 이벤트로 넣는다(Windows XInput 계약과 같다).

| GameController | Rocks |
|---|---|
| buttonA·B·X·Y | South·East·West·North |
| left·rightShoulder, left·rightTrigger | 어깨 버튼, 트리거 축 [0, 1] |
| leftThumbstick·rightThumbstick(축과 버튼) | 스틱 축(위가 +), LeftThumb·RightThumb |
| buttonMenu·buttonOptions | Start·Back |
| dpad | DPad 네 방향 |

- 슬롯 배정은 연결 순서이고, 해제된 슬롯은 비운다(다음 연결이 가장 작은 빈 슬롯을 쓴다).
- `extendedGamepad`가 없는 컨트롤러(옛 micro gamepad)는 받지 않는다.

## 6. 빌드 (Rocks.Build)

- **Objective-C++ 원본:** 확장자 `.mm`이다. Apple 대상에서 `-x objective-c++ -fno-objc-arc`로 컴파일한다. 다른 대상의 Sources에 `.mm`이 있으면 입력 검증 오류다(MD2).
  - UE는 `.cpp`도 Objective-C++로 컴파일한다. Rocks는 Objective-C가 든 파일만 `.mm`으로 둔다. 이유는 둘이다.
  - 하나는 format 도구가 확장자로 언어(ObjC)를 정한다는 점이다.
  - 다른 하나는 나머지 C++가 모든 플랫폼에서 같은 언어로 컴파일되게 하려는 점이다. Objective-C++는 `Class`·`id`·`SEL`을 전역 타입으로 미리 정의한다.
- **framework:** 모듈 규칙의 `Frameworks` 필드(이미 있음, Apple 전용 검증 포함)로 `Cocoa`·`QuartzCore`·`GameController`를 링크한다.
- **도구 지원:** format·policy·include 소유 검사가 `.mm`을 C++ 원본과 같이 다룬다. clang-format은 `.mm`을 Objective-C로 처리한다. 정책 검사의 STL 금지·이름 규칙은 그대로 적용한다. Objective-C 문법(`@interface`, 메시지 식)은 정책 검사의 C++ 규칙 대상이 아니다.
- **실행 파일:** 개발 실행은 번들 없는 실행 파일이다. `setActivationPolicy:Regular`로 Dock·포커스를 얻는다. `.app` 번들(Info.plist·서명·공증)은 배포 단계에서 BuildTargets 문서로 정한다(MD3).

## 7. 검증

이 PC에서는 할 수 없으므로 모두 사용자의 Mac에서 한다. 자동 시험은 Mac에서 돌리고, 장비 확인은 사람이 한다.

| 검증 | 방법 |
|---|---|
| 빌드 | Mac에서 Debug 빌드, 경고 0, format·policy 통과 |
| 창 자체 시험 | `Sandbox --self-test`(Cocoa 모드)에 macOS 창 단계: 창 생성 → 펌프 → `SetTitle` → 크기 값 → 닫기 요청을 코드로 세움(`performClose:`) → 정상 종료. 순서는 펌프 횟수로 정한다 |
| 입력 사람 확인 | `Sandbox --engine --window`: 터미널에서 띄웠을 때 키 포커스 획득, 키(W·A·S·D·Space, Command 조합 뒤 키가 눌린 채 남지 않음)·마우스 버튼 위치(Retina 픽셀)·휠·트랙패드 스크롤·창 크기 조절 중 프레임 유지·Cmd+Q 정상 종료 |
| 게임패드 | 패드가 있으면 South·East 로그, 없으면 NOT RUN |
| 변환 단위 시험 | `kVK_*` → Key 표, modifier 플래그 차이, 좌표 변환은 Cocoa 없이 도는 순수 함수로 분리해 EngineTests에서 돌린다(Windows에서도 컴파일되는 C++ 부분) |

## 8. 작업 순서

| 단계 | 내용 | 선행 |
|---|---|---|
| MW1 | Rocks.Build `.mm` 컴파일·입력 검증·도구(format·policy·include 소유)의 `.mm` 지원 | 없음 |
| MW2 | Launch `mainThreadHost` hook(플랫폼 중립, LaunchTests로 시험), PlatformWindow `RunCocoaMainThreadHost`(메인 run loop·게임 스레드·Quit·종료 join)·`RunOnMainThread` | MW1 |
| MW3 | PlatformWindow Mac 창·이벤트 큐·키보드·마우스·문자·좌표, 변환 순수 함수와 시험 | MW2 |
| MW4 | GameController 게임패드 backend | MW3 |
| MW5 | Sandbox Cocoa 모드 연결·macOS 창 자체 시험 | MW3 |

- 구현은 브랜치 `feature/platform-mac`에 둔다. Windows에서 컴파일되는 부분(Rocks.Build C#, 순수 변환 함수, Launch 공통 코드)은 여기서 시험한다.
- `.mm`·macOS 전용 코드는 빌드되지 않은 채로 커밋한다. 사용자가 Mac에서 빌드·확인한 뒤 고쳐서 engine-runtime에 병합한다.
- 앱 수명 이벤트(Activated·Deactivated)는 PlatformLayer 결정대로 오디오 AU3와 함께 연결한다. MW3은 알림만 받아 둔다.

## 9. 결정

| ID | 결정 | 근거 |
|---|---|---|
| MD1 | 메인 스레드(Cocoa)와 게임 스레드를 나눈다 | UE `MAC_SEPARATE_GAME_THREAD 1`과 같다. 창 크기 조절·메뉴 모달 루프 중에도 게임이 돈다 |
| MD2 | Objective-C가 든 파일만 `.mm` | format이 확장자로 언어를 정하고, 나머지 C++의 언어를 플랫폼마다 같게 둔다. UE는 `.cpp`도 ObjC++로 컴파일하지만 Rocks에 그럴 이유가 없다 |
| MD3 | 개발 실행은 번들 없는 실행 파일 + `setActivationPolicy:Regular` | 빌드·시험 경로를 바꾸지 않는다. 번들은 배포 단계에서 다룬다. Retina 배율이 번들 없이 나오는지는 Mac 확인 항목이다 |
| MD4 | ARC 쓰지 않음(MRC) | UE 기본과 같다. 소유를 명시하는 Rocks 규칙과도 맞는다 |
| MD5 | 문자 입력은 `characters`부터, 조합은 PW3 | IME 조합은 UiToolkit PW3·입력 I13 범위다 |
| MD6 | 트랙패드 스크롤도 휠 값으로 | OS 제스처는 범위 밖(TouchInput TD12)인데, 제스처로 돌리면 트랙패드 스크롤이 사라진다. UE의 마우스 휠 값(`deltaY`)과 같은 단위다 |
| MD7 | 창은 입력 view를 소유하고 그래픽 backend는 그 view의 layer만 `CAMetalLayer`로 정한다 | UE는 `FMetalView`로 내용 view를 바꾸지만, Rocks는 창이 입력 view를 소유하므로 view를 교체하지 않는다 |
| MD8 | Launch는 hook만, Cocoa run loop·`RunOnMainThread`는 PlatformWindow | Launch(Core)가 AppKit을 링크하면 모든 콘솔 도구가 AppKit에 묶인다. UE는 Launch가 직접 하지만 Rocks Launch는 Engine에 의존하지 않는 층이다 |
