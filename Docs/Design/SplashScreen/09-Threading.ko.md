# 시작 화면(스플래시) — 9. 스레드와 수명

[목차](README.ko.md)

## 9. 스레드와 수명

### 9.1 호출 계약

| 호출 | 스레드 |
|---|---|
| `Show`·`Hide`·`Suspend`·`Resume`·`State`·소멸 | 호스트 본문 스레드 하나(Show를 부른 스레드) |
| `SetText` | 아무 스레드(작은 잠금 + 갱신 알림) |
| `SplashStatusSource::poll`·`release` | 스플래시 스레드(Windows)·메인 스레드(macOS). 원천은 다른 스레드와 함께 불려도 안전해야 한다 |
| 대화상자 hook(9.5절) | 아무 스레드, 잠금 없음 |

### 9.2 Windows

- **Show는 기다리지 않는다**(UE `WPS` 780–787행과 같다). 호스트 스레드에서는 설명 검사·복사와 이미지 찾기(파일 존재 확인뿐 — `ImageNotFound`는 여기서 바로 돌려준다)만 하고, `platform::Thread`(`ThreadOptions` — 이름 `"Splash"`, 스택 `kSplashThreadStackBytes` 256 KiB)를 띄운 뒤 돌아온다. 디코딩·배율 맞춤이 시작 경로에 들어가지 않는다(요구 4).
- 스플래시 스레드: `SetThreadDpiAwarenessContext(PER_MONITOR_AWARE_V2)` → `CoInitializeEx(APARTMENTTHREADED)` → 이미지 디코딩·배율 맞춤 → 글꼴 → `CreateWindowExW(WS_POPUP)`(게임 `WS_EX_TOOLWINDOW`, Studio `WS_EX_APPWINDOW` + 창 제목 = Title 칸) → DWM 둥근 모서리 → 생성 완료 이벤트 → 루프. 실패하면 스플래시 스레드가 `LogSplash` Warning(사유·원본 OS 오류·실패한 호출 이름)을 남기고 상태를 `Failed`로 한 뒤 끝난다. 시작은 계속된다.
- **DPI는 스레드 단위다.** 스레드 DPI 문맥은 그 스레드가 만든 창에만 적용되므로, 프로세스의 DPI 모드와 무관하게 스플래시가 선명하고 주 창 동작을 바꾸지 않는다. 프로세스 per-monitor v2와 `Window`의 `WM_DPICHANGED` 처리는 PlatformWindow의 별도 작업이다(18절).
- 루프는 `MsgWaitForMultipleObjectsEx(0개, 제한 시간 = 다음 갱신까지, QS_ALLINPUT, MWMO_INPUTAVAILABLE)` → 메시지 처리 → 시각이 되면 `poll`·모델 갱신·바뀐 영역 무효화다. UE의 1/60 s 고정 Sleep을 쓰지 않는다.
- Studio 창은 끌어서 옮길 수 있다(`WM_NCHITTEST` → `HTCAPTION`, UE와 같다). 게임은 옮기지 않는다.
- `Hide`: 생성 완료 이벤트를 기다리고(창이 생기기 전 Hide의 교착 방지 — UE와 같다), 창이 있으면 `WM_CLOSE`를 보낸 뒤 스레드를 join하고 원천의 `release`를 부른다. 창·DIB·글꼴은 스플래시 스레드가 지운다. 멱등이다.
- `Suspend`·`Resume`: 창을 숨기고 다시 보인다(`SW_HIDE`·`SW_SHOWNA` 메시지). 갱신은 계속한다.
- **시험 seam.** presenter(창·디코딩·그리기)는 인터페이스 뒤에 있고, 시험은 시험 접근 헤더(TestAccess 규칙)로 가짜 presenter를 넣는다. 가짜 presenter는 생성 gate·그리기 기록을 가져 14절 시험 8이 OS 창 없이 돈다.

### 9.3 macOS

- 창 프로그램은 `mainThreadHost = RunCocoaMainThreadHost`로 돌아(Launch, MacPlatform 3절) 프로세스 메인 스레드가 Cocoa 이벤트 루프를 돈다. 스플래시는 그 메인 스레드에서 산다.
- `Show`·`Hide`는 `platform::RunOnMainThread`(PlatformWindow, 동기 — MacPlatform 3절)로 창을 만들고 닫는다(맨 `dispatch_sync`를 쓰지 않는다 — 메인 스레드가 닫힌 뒤의 Fatal 규칙을 받기 위해). 그래서 macOS의 Show는 창 생성(디코딩 포함)을 기다린다. 메인 스레드는 이벤트 루프뿐이라 짧다.
- 창은 테두리 없는 `NSWindow`다(`NSPanel`은 기본 `hidesOnDeactivate`라 다른 앱으로 옮기면 사라진다 — UE도 `NSWindow`, `MPS` 290행). 그림자, 주 화면 가운데, `backingScaleFactor`로 이미지를 고른다.
- 갱신은 메인 run loop의 `NSTimer`(0.1 s, 보간 중 1/60 s)를 `NSRunLoopCommonModes`에 건 것이다(이벤트 추적 모드에서도 돈다). 타이머가 `poll`을 부른다.
- **잠금 규칙.** 메인 스레드의 `poll`은 hub 잠금과 스플래시 잠금을 짧게 잡는다. 그러므로 어떤 스레드도 이 두 잠금을 쥔 채 `RunOnMainThread`를 부르지 않는다(Show·Hide는 잠금 없이 부르고, ProgressHub는 `RunOnMainThread`를 부르지 않는다).
- 호스트가 메인 스레드에서 본문을 돌리면(mainThreadHost 없음) 메인 스레드가 막혀 스플래시가 갱신되지 않으므로 띄우지 않는다(4절 `NoMainThreadHost`). UE는 펌프 호출에 기댄다(1.5절).

### 9.4 주 창으로 넘기기

1. 호스트는 주 창을 숨긴 채 만들고 첫 프레임을 렌더한다(화면 마일스톤 뒤 — 그 전 Sandbox는 렌더 없이 창을 보인다).
2. 주 창을 보이고 **보인 뒤 한 프레임을 더 present한다**(숨긴 창에 그린 present가 보이는 순간 화면에 남는다는 보장이 flip model·DWM에 없다).
3. 주 창을 앞으로 가져온다(`SetForegroundWindow`). 같은 프로세스의 스플래시가 전경이면 허용된다. 사용자가 시작 중에 다른 앱으로 옮겼으면 실패하는데, 이때는 강제하지 않는다(OS가 작업 표시줄 단추를 깜빡인다).
4. `StartupProgress::Finish()` → `SplashScreen::Hide()`.
- 주 창을 먼저 보이므로 UE의 guard 창(1.4절)이 필요 없고 빈 화면 틈도 없다. 시작 중 Fatal이나 정상 종료 요청으로 주 창 없이 끝나면 `SplashScreen` 소멸이 Hide한다.

### 9.5 대화상자

- `PlatformBase`에 `platform::SetSystemDialogHook(void (*)() noexcept)`·`platform::ClearSystemDialogHook()`·`platform::NotifySystemDialogOpening()`을 둔다. hook은 원자 함수 포인터 **하나**다. 이미 걸려 있으면 `SetSystemDialogHook`은 `AlreadyExists`다(지금 쓰는 곳은 스플래시뿐 — 둘째 사용자가 생기면 목록으로 바꾼다).
- OS 모달 대화상자를 띄우는 코드(앞으로 생길 Fatal·assert 대화상자, 메시지 상자 감싸개)는 띄우기 직전에 `NotifySystemDialogOpening()`을 부른다. 스플래시는 Show 때 hook을 걸고 Hide 때 푼다.
  - Windows: hook은 창을 숨기는 메시지를 `PostMessage`한다(잠금 없음, 크래시 경로에서도 부를 수 있다).
  - macOS: hook은 원자 표시만 세운다. 크래시 경로에서 AppKit을 만지지 않는다. 다음 타이머가 표시를 보고 창을 숨긴다.
  - 대화상자 뒤 다시 보이지 않는다(대개 종료 경로다).
- 호스트가 띄우는 일반 모달 UI(Studio 자동 저장 복구 등)는 `Suspend`·`Resume`으로 감싼다(UE `UEM` 371–378행과 같은 자리).
- 스플래시 창은 맨 위(`TOPMOST`)로 두지 않는다(UE와 같다 — 사용자가 다른 창으로 옮길 수 있다).

### 9.6 크래시·종료

- 스플래시 스레드는 `platform::Thread`로 만드므로 다른 스레드와 같이 크래시 보고 대상이다(CrashHandlingAndLaunch 8.6절 `RunWithCrashGuard`). 스플래시는 종료 콜백을 걸지 않는다(OS가 창을 치운다).
- `RequestExitWithoutStaticTeardown` 경로는 Hide 없이 끝나도 된다.
