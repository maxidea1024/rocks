## 19. 창·입력 계약(UW7 상세 설계)

9.1절 UW7의 상세다. UW7a(크기 조절 중 그리기)·UW7b(직접 그린 창 테두리)·UW7c(주 메뉴)·UW7d(팝업 허용 영역)는 사용자 요청으로 먼저 끝났다. 이 장은 남은 계약이다: 영역 판정, 스레드 계약, 창 모드, 펌프 수준 대기, DPI, 텍스트 입력 문맥(PW2·PW3·PW7), 이벤트 문자열. 2026-11-27 조사(PlatformWindow·UiFramework·UE 5.8 ApplicationCore·Slate)와 리뷰(리뷰어 1명 — 18건)를 반영했다.

### 19.1 지금 상태

- **영역 판정.** Win64는 `RegionHitTest` delegate를 동기로 부른다. 부르는 곳은 다음과 같다.
  - Custom 테두리의 `WM_NCHITTEST`(`HitZone`, Win64Window.cpp 748행)
  - System 테두리의 client 판정(710행)
  - `WM_NCLBUTTONDBLCLK`(1006행)
  - 공개 `HitZone`(시험)

  UiDemo는 그 안에서 `Application::QueryWindowRegion`으로 UI 트리를 읽는다. 마지막 hit grid가 없으면 실시간 `HitTest`를 한다. 판정 순서는 Popup → 크기 조절 가장자리 → UI 영역이다(755–811행, UE SWindow.cpp 1945–1993행과 같다).
- **스레드.** 문서 계약은 "창을 만든 스레드에서만"(Window.h 116행) 하나이고 검사가 없다. Android는 UI 스레드·입력 스레드·게임 스레드로 나뉘고 mutex로 넘긴다(AndroidWindow.cpp 23–27행).
- **창 모드.** `ToggleFullscreen`·`IsFullscreen`만 있다. Win64 backend가 전체 화면 제목 줄 두 번 누르기에 스스로 전환한다(1006–1009행). 모드 열거와 명시 설정 API가 없다.
- **대기.** `Window::WaitForEvents`는 이름이 창 단위일 뿐, 스레드 전체 OS 큐를 기다린다(`MsgWaitForMultipleObjectsEx`). 결함은 Win64 `PumpEvents`가 다른 창 이벤트를 그 창의 미룬 큐에 쌓는데, 대기가 이 미룬 큐를 보지 않는다는 점이다. 그래서 이미 펌프한 창의 미룬 큐에 이벤트가 남아도 루프가 최대 50 ms 잠든다. Android는 기다리지 않는다(바쁜 루프).
- **DPI.** 프로세스가 DPI를 인식하지 않는다(manifest·`SetProcessDpiAwareness*` 없음 — 스플래시만 스레드 단위 PMv2, Win64SplashPresenter.cpp 164행). DWM이 늘려 그린다. `GetDpiForWindow`는 크기 조절 가장자리 한 곳에만 쓰이고 늘 96이다. UI 배율은 늘 1이다.
- **텍스트 입력.** push형 IMM이다(`SetInlineComposition`·`MoveCompositionWindow` 전역 함수, `thread_local` 대상 창). 조합 문자열은 OS 메시지 처리 중 힙에 할당한 `SharedPtr<String>`으로 `InputEvent`에 실려 온다. 위치는 UTF-8 byte다. TSF와 `TextInputContext`는 없다.
- **이벤트 문자열.** `InputEvent`가 `SharedPtr<const String> composition`을 들고 있어 복사마다 원자 참조 계수가 움직인다.

### 19.2 결정

**영역 지도 게시(UE보다 강한 계약).**
- UE 5.8은 `WM_NCHITTEST`에서 위젯 트리를 동기로 판정한다(WindowsApplication.cpp 2825–2862행, SWindow.cpp 1930–2031행). 이 판정은 마지막 hit grid를 쓴다.
- Rocks는 판정할 사각형을 UI가 미리 게시한다. 근거는 셋이다.
  - (1) OS 콜백이 UI 코드로 재진입하는 일을 없앤다.
  - (2) Mac은 메인 스레드가 OS 이벤트를 받고 게임 스레드와 늘 나뉜다(MacPlatform MD1).
  - (3) DWM 스냅 등 다른 프로세스의 `WM_NCHITTEST`가 아무 때나 온다.
- **`platform::WindowRegionMap`.**
  - 항목은 (client 픽셀 사각형, `WindowRegion`)이고, 최대 `MaxWindowRegionRects`(64)개다. 앞 항목이 이긴다(앞 = 위).
  - `popupOpen` 표지를 둔다. 열린 팝업(메뉴)이 있으면 backend가 크기 조절 가장자리를 끈다(UE `AnyMenusVisible`, SWindow.cpp 2011–2016행).
- **게시.** `Window::PublishRegionMap(const WindowRegionMap&)`가 backend 자기 표에 복사한다. 잠금으로 보호하므로 다른 스레드의 OS 콜백이 읽어도 된다.
- **판정 순서**(backend는 표만 읽는다):
  1. Popup 항목
  2. 크기 조절 가장자리 — Custom 테두리·크기 조절 가능·최대화나 전체 화면 아님·`popupOpen` 아님일 때. backend가 DPI로 계산한다.
  3. 나머지 지도 항목(앞부터)
  4. Client
- **지도 생성** `Application::BuildWindowRegionMap(WindowRegionMap&)`.
  - 규칙은 `QueryWindowRegion`과 같다. 지도 조회와 `QueryWindowRegion`이 점마다 같은 답을 내야 한다(시험).
  - hit grid에 항목 순회 API를 더한다. 마지막 그리기의 hit grid 항목을 위에서부터 보고, 각 항목에서 조상 사슬로 올라간다. 처음 만난 Client가 아닌 영역 또는 `SupportsKeyboardFocus` Element가 그 사각형의 답이다.
  - Popup 항목은 팝업 층 아래에서 hit 판정되는(Visible) 항목의 사각형이다. 층 geometry 전체를 쓰지 않는다 — 툴팁 층은 hit 판정되지 않고, 막대 메뉴 층은 메뉴 영역만 받는다.
  - 사각형은 클립 사슬과 교차하고, 변환이 있으면 AABB로 바꾼다.
  - 반올림: 구멍(Client)·Popup은 바깥으로, 영역(Caption·버튼)은 안으로 한다.
  - 넘치면: 넘친 구멍의 부모 영역 사각형을 Client로 바꿔 보수적으로 게시하고, 로그를 한 번 남긴다. 이전 지도를 유지하지 않는다(영구히 낡는다).
  - hit grid가 비었으면(`Reset` 뒤, 루트 교체 뒤) 빈 지도를 게시한다(전부 Client, 가장자리만).
  - 그리기마다 만들고, 내용이 같으면 게시하지 않는다.
- **늦음.** 판정은 마지막 그리기 기준이다(UE도 마지막 hit grid). 그리기 없이 크기·최대화가 바뀌면 다음 그리기까지 지도가 늦다. 크기 변경은 다음 프레임에 다시 그리므로 한 프레임이다.

**스레드 계약.**
- `Window`의 모든 함수와 펌프 함수는 창 객체를 소유한 게임 lane에서 부른다. Release 밖(Debug·Development) 구성에서 단언한다. Mac은 OS 창을 메인 스레드가 만들지만 Rocks `Window` 객체는 게임 lane이 소유한다.
- 예외가 둘 있다.
  - `PublishRegionMap`의 표 읽기: backend 내부, 잠금.
  - live resize 콜백: OS modal 루프 안에서 같은 스레드에 동기로 부른다(UE `OnSizeChanged`와 같다).
- live resize 콜백과 lane 대기 펌프 안에서는 `PumpMessages`·`WaitForMessages`를 부르지 않는다.

**창 모드(UE `EWindowMode`).**
- 열거는 `enum class WindowMode : uint8 { Windowed, WindowedFullscreen, Fullscreen }`이다.
  - Win64의 `Fullscreen`은 지금 `WindowedFullscreen`과 같다. 독점 전체 화면은 렌더러 스왑 체인 몫이다(UE `Renderer->UpdateFullscreenState`).
  - `GetWindowMode()`는 실제 모드를 돌려준다(UE). 그래서 Win64에서 `SetWindowMode(Fullscreen)` 뒤에는 `WindowedFullscreen`이다.
  - Android는 `WindowedFullscreen`(몰입 모드)만 되고, 다른 값은 Unsupported다.
- API는 `Window::SetWindowMode(WindowMode)`·`GetWindowMode()`다. `ToggleFullscreen`·`IsFullscreen`은 지운다(하위호환 없음).
- 최대화·복원은 창 모드가 아니라 OS 창 상태다(`IsMaximized`·`ToggleMaximize`).
- **backend는 스스로 모드를 바꾸지 않는다.** UE `SWindowTitleBarArea`처럼 전체 화면에서 제목 줄은 Client를 돌려준다(SWindowTitleBarArea.cpp 253–274행). 두 번 누르기는 위젯이 받아 호스트 delegate로 `SetWindowMode(Windowed)`를 부른다.
- 이를 위해 Win64 `WM_LBUTTONDBLCLK`에 두 번 누르기 표지를 싣는다(지금은 MouseDown과 구분되지 않는다).
- `WindowRegion::FullscreenCaption`과 backend의 `WM_NCLBUTTONDBLCLK` 전환은 지운다.

**펌프 수준 대기.**
- `platform::PumpMessages()`는 이 스레드의 OS 메시지를 모두 디스패치해 창마다 대기 큐로 나눈다(Win64 창 대기 큐 256). Android·Mac은 OS 스레드가 넘긴 이벤트를 창 큐로 옮긴다.
- `Window::TakeEvents(InputQueue&)`는 그 창의 대기 큐만 옮긴다(OS 호출 없음). 넘친 수는 `DroppedCount`로 넘긴다. 터치 프레임 마감(`touches_.EndFrame`)은 여기로 옮긴다.
- `platform::WaitForMessages(uint32 ms)`.
  - 어느 창의 대기 큐가 비어 있지 않으면 바로 돌아온다.
  - 아니면 OS 메시지·깨움·제한 시간 중 먼저 온 것까지 기다린다. Win64는 `MsgWaitForMultipleObjectsEx(QS_ALLINPUT, MWMO_INPUTAVAILABLE)`, Android·Mac은 OS 스레드가 신호하는 condition variable이다(Android는 surface 소실 같은 UI 스레드 콜백에서도 신호한다).
- `platform::WakeMessageWait()`: 다른 스레드에서 대기를 푼다(비동기 작업 완료). Win64는 `PostThreadMessage`, 나머지는 condition variable notify다.
- 대기 시간은 호스트가 모든 `Application`의 다음 타이머 마감으로 정한다(`Application::NextTimerDeadline` — caret 깜빡임·툴팁·하위 메뉴 0.5초 지연). 지금의 고정 50 ms를 대신한다.
- 창이 `TakeEvents`하지 않으면 그 큐가 차서 대기가 늘 바로 돌아온다. 호스트는 매 루프 모든 창을 `TakeEvents`해야 한다(계약).
- `Window::PumpEvents`는 창 하나짜리 호스트 편의 함수(`PumpMessages` + `TakeEvents`)로 남긴다. `Window::WaitForEvents`는 지운다.
- UE는 대기 API 없이 엔진 루프가 펌프하고 쉰다(LaunchEngineLoop.cpp 5784–5797행). Rocks는 에디터식 UI 앱이 입력이 없을 때 CPU를 쓰지 않게 대기를 둔다.

**DPI(PW2, UE `SetHighDPIMode`·`WM_DPICHANGED`·창 루트 변환).**
- `platform::EnableHighDpiAwareness()`는 실행 파일 시작 때 스플래시보다 먼저 한 번 부른다.
  - Win64는 `SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2)`다(UE는 `PROCESS_PER_MONITOR_DPI_AWARE` — v2는 비client 배율까지 OS가 한다).
  - 스플래시의 스레드 단위 PMv2와 충돌하지 않는다. SplashScreen 9.2절의 "프로세스 DPI 모드는 바꾸지 않는다"는 이 함수를 부르지 않은 앱 기준으로 고친다.
  - 부르지 않은 앱(Sandbox 등)은 지금 동작 그대로다.
- **`Window::DpiScale()`(유효 DPI / 96).**
  - `WindowDesc` 크기는 96 DPI 논리 크기다. 생성은 `AdjustWindowRectExForDpi`로 한다. 위치를 모르는 생성(`CW_USEDEFAULT`)은 숨긴 채 만들고 `GetDpiForWindow`로 크기를 다시 맞춘 뒤 보인다.
  - `desktopPosition`은 물리 픽셀이다. 떠 있는 탭 창 크기는 원래 창 배율로 논리 크기로 바꾼다.
- **`WM_DPICHANGED`(크기·이동 modal 루프 안 포함).** backend가 배율을 먼저 바꾸고, OS가 제안한 사각형으로 `SetWindowPos`한다(UE WindowsApplication.cpp 4001–4021행). 그 `WM_SIZE`는 live resize로 새 배율로 그린다. `ConsumeDpiChange(float&)`는 글꼴 cache 비우기 같은 부수 처리용이다.
- **UI 좌표 모델: 루트 변환으로 배율을 준다(UE 창 루트 변환 `AppScale × DPI`와 같다).**
  - 호스트는 `PrepareLayout(Geometry::Root(viewport / s, Scale(s)), s)`로 그린다. s = `DpiScale × 앱 배율`이고, 매 Render에서 읽는다(live resize 포함).
  - `Geometry`는 창 좌표까지 누적된 변환이므로, UI 창 좌표가 곧 client 물리 픽셀이다. 그래서 `InputAdapter`·영역 지도·`WorkAreaAt`은 좌표를 변환하지 않는다.
  - 글꼴·SVG는 누적 배율로 래스터한다.
- **snapshot·시험.** `--snapshot`·`--self-test`는 `EnableHighDpiAwareness`를 부르지 않고 s = 1로 고정한다(결정적, 회귀 기준이 배율 1). 배율 시험은 가짜 배율(s = 2)로 한다.

**텍스트 입력 문맥(PW3, UE `ITextInputMethodContext`·`ITextInputMethodSystem`·`ITextInputMethodChangeNotifier`).**
- **`ui::TextInputContext`(UiFramework, UE 대응).**
  - 질의: `IsReadOnly`, `IsComposing`, `GetTextLength`, `GetSelection`, `GetText(범위)`, `GetCharacterIndexFromPoint`, `GetTextBounds(범위)`, `GetScreenBounds`, `GetWindow`
  - 수정: `SetSelection`, `SetText(범위)`
  - 조합: `BeginComposition`·`UpdateCompositionRange`·`EndComposition`
  - 위치 공간은 UTF-16 code unit이고, 줄 끝은 `\n` 한 글자다(3.12절, UE와 같다). UTF-8 텍스트와의 변환은 context 구현이 한다.
  - `GetTextBounds`·`GetScreenBounds`는 UI 창 좌표(= client 픽셀)다. IMM은 그대로 쓰고, TSF는 `ClientToScreen`한다(UE의 `GetWindowRect` 빼기는 OS 테두리 창에서 틀린다 — WindowsTextInputMethodSystem.cpp 407–413행).
  - **조합 중 `SetText`는 context 안의 미리보기 층만 바꾼다.** `EndComposition`에서 한 번 커밋한다(편집 이력 하나, `TextChanged` 한 번). 조합 갱신마다 바인딩 쓰기·검증·최대 길이 판정이 일어나지 않게 하려는 것이다.
- **변경 통지(`TextInputChangeNotifier`, UE 146–162행).** `CancelComposition`·`NotifyTextChanged`·`NotifySelectionChanged`·`NotifyLayoutChanged`. 앱이 조합 중에 글자를 바꾸면(Esc 되돌리기, 바인딩) 조합을 취소한다.
- **`platform::TextInputSystem`(PlatformWindow, 창별).**
  - `RegisterContext`·`UnregisterContext`·`ActivateContext(context)`·`DeactivateContext`. 창은 context가 준다(`GetWindow`). `thread_local` 대상 창은 없앤다.
  - context는 플랫폼 중립 `platform::TextInputClient`로 받는다. UiFramework 어댑터가 `ui::TextInputContext`를 감싼다.
  - system은 context를 원시 포인터가 아니라 id·세대로 든다. 해제된 context로 가는 미룬 기록은 버린다(CoreObjectInputDetailedPlan TextSessionId·늦은 commit 거부·정확히 한 번 commit과 맞춘다).
  - Element는 자기 창의 system을 `Application`의 host 서비스로 찾는다(`SetPopupWorkArea`와 같은 방식). 소멸·가상화 행 재사용 때 Unregister한다.
  - 비밀번호 입력란은 IME를 끈다(`ImmAssociateContext(nullptr)` — UE와 같다).
- **Win64 1차는 IMM이다. 순서 보장이 핵심이다.**
  - 활성 context가 있을 때:
    - `WM_IME_*`를 받으면 문자열(확정·조합)과 커서를 메시지 시점에 읽어 system 버퍼에 기록한다. 처리 시점에 읽으면 한 펌프 안의 연속 확정 두 개 중 앞의 것을 `GCS_RESULTSTR`이 덮어 잃는다.
    - 그 창 입력 큐에 `TextInputMethod` 표지 이벤트(기록 번호, trivially copyable)를 넣는다.
    - `InputAdapter`가 그 위치에서 `TextInputSystem::Process(기록)`을 불러 키·문자 이벤트와 순서를 지킨다. 예: [확정 "한", KeyDown(Left)]에서 확정이 먼저 들어간다. UE는 `WM_CHAR`·키·IME를 한 미룬 큐에 순서대로 넣는다(WindowsApplication.cpp 2310–2330행).
    - `WM_IME_COMPOSITION`을 `DefWindowProc`에 넘기지 않거나 `WM_IME_CHAR`를 삼킨다(UE WindowsTextInputMethodSystem.cpp 1157–1161행). 그래야 확정 문자열이 `WM_CHAR`로 두 번 오지 않는다.
  - 활성 context가 없으면 `DefWindowProc` → `WM_IME_CHAR` → `WM_CHAR`로 `Text` 이벤트가 온다. 게임 문자 입력·`InputState`가 IME 글자를 계속 받는다.
  - 후보창 위치는 context의 `GetTextBounds`로 당겨 오고(pull), `CFS_EXCLUDE`로 줄을 가리지 않게 한다(UE).
- **system 버퍼.** 기록 형식은 (종류, context id, 문자열 offset·길이, 커서)이고, 문자열은 UTF-16 그대로다.
  - 용량은 기록 64개·문자 4096 code unit이다.
  - 넘치면 조합 갱신은 마지막 것만 남기고, 확정은 버리지 않는다. 그래도 넘치면 조합을 취소하고 로그를 남긴다.
  - 게임 lane이 `PumpMessages` 전에 필요하면 키운다(OS 콜백 안 할당 없음).
- **TSF.** 같은 인터페이스 위의 UW4f3(병합 뒤)이다. TSF의 동기 질의는 펌프 중에만 받고, lane 대기 펌프 중이면 잠금 거부(`TS_E_SYNCHRONOUS`)로 답한다.
- **입력 이벤트에서 조합 문자열이 빠진다.**
  - `TextComposition`·`TextCompositionCommit` 이벤트와 `InputEvent::composition`을 지우고, `Text`(code point 하나)와 `TextInputMethod` 표지만 남긴다. UE도 IME 문자열은 이벤트가 아니라 context로 간다.
  - `InputEvent`는 trivially copyable이 된다(`static_assert`).
  - 이벤트 문자열을 프레임 버퍼 offset으로 바꾸는 항목은 필요 없어진다(9.1절 UW7 행 정정).
- **Android·Mac.**
  - Android는 소프트 키보드(UE `IVirtualKeyboardEntry`)로 같은 context를 쓴다.
  - Mac은 메인 스레드가 게임 lane을 기다릴 수 없다(MacPlatform MD1). 그래서 `NSTextInputClient`의 동기 질의는 게임 lane이 게시한 snapshot(텍스트·선택·조합·caret 사각형, 잠금)으로 답하고, 수정은 기록으로 게임 lane에 넘긴다.
  - 둘 다 구현은 그 플랫폼 단계다.

### 19.3 단계

| 단계 | 범위 | 완료 판정 |
|---|---|---|
| UW7e | 스레드 계약(문서, Release 밖 단언)<br>창 모드(`SetWindowMode`·`GetWindowMode`, 두 번 누르기 표지, 제목 줄 위젯의 전체 화면 해제, `ToggleFullscreen`·`FullscreenCaption`·backend 전환 제거)<br>펌프 수준 대기(`PumpMessages`·`TakeEvents`·`WaitForMessages`·`WakeMessageWait`, `Application::NextTimerDeadline`, 창 단위 `WaitForEvents` 제거 — 50 ms 결함 수정)<br>UiDemo 루프 이관 | PlatformWindow 시험: 다른 창의 미룬 이벤트가 있으면 대기가 바로 돌아온다, `WakeMessageWait`·보낸 메시지로 대기가 풀린다, 모드 왕복(실제 모드)<br>UiFramework 시험: 타이머 마감<br>self-test |
| UW7f | 영역 지도 게시(`WindowRegionMap`·`PublishRegionMap`·`BuildWindowRegionMap`, hit grid 항목 순회, `popupOpen`)<br>`RegionHitTest` delegate 제거 | 지도와 `QueryWindowRegion`의 점 격자 동치(메뉴 열림·최대화·툴팁·넘침·Reset 뒤)<br>판정 순서(가장자리가 Caption보다 먼저 — 기존 CustomFrameHitZones 이관)<br>Win64 판정 시험 |
| UW7g | DPI(프로세스 PMv2, `DpiScale`·`ConsumeDpiChange`, `AdjustWindowRectExForDpi`·숨긴 생성, `WM_DPICHANGED` 배율 먼저, 호스트 루트 변환 배율, 글꼴·SVG 배율, snapshot·self-test 배율 1) | 가짜 배율 2에서 배치·hit·영역 지도·작업 영역 시험<br>`WM_DPICHANGED`를 숨긴 창에 보내 배율이 먼저 바뀌는지<br>실제 모니터는 사람 확인 |
| UW7h | 텍스트 입력 문맥(UiFramework `TextInputContext`·변경 통지, PlatformWindow `TextInputSystem`·`TextInputClient`, Win64 IMM 기록·표지 이벤트·`WM_IME_CHAR` 삼키기, 후보창 `CFS_EXCLUDE`)<br>`EditableTextElement`가 context 구현(미리보기 층, 확정 1회)<br>조합 이벤트·`InputEvent::composition`·`ui::Platform` 조합 함수·`InlineComposition.h` 제거<br>이관 대상: InputEvent.h, InputState.cpp, Application.cpp, InputAdapter.cpp, EditableTextElement.cpp, EditableTextWidgetTests.cpp(가짜 system), GameHud.cpp, Win64·Android·Unsupported 구현 | context 시험: UTF-16 위치 변환·조합 범위·선택·확정 1회<br>IMM 기록 시험: 확정 + Left 순서, 연속 확정 두 개, context 해제 뒤 기록 버림, 넘침<br>`InputEvent` trivially copyable<br>한국어 IME 사람 확인 |

**Android·Mac 대조(완료 판정의 "구현 가능성").**

| 계약 | Android | Mac(MacPlatform MD1 — 메인·게임 스레드 분리) |
|---|---|---|
| 영역 지도 | 해당 없음(시스템 테두리만) | 직접 그린 창의 `mouseDown`·커서 판정에서 메인 스레드가 지도를 읽는다(잠금) |
| 펌프·대기 | 입력 스레드가 넘긴 이벤트 이동, condition variable 대기(UI 스레드 콜백도 신호) | 메인 스레드가 이벤트를 잠금 큐에 넣고 게임 lane이 옮긴다, condition variable 대기(Android와 같다) |
| DPI | density × 창 upscale(UE AndroidPlatformApplicationMisc.cpp 187–230행) | `backingScaleFactor`, `windowDidChangeBackingProperties` |
| 텍스트 입력 | 소프트 키보드 context | `NSTextInputClient` 질의는 게임 lane이 게시한 snapshot으로, 수정은 기록으로 넘김 |
| 창 모드 | 몰입 모드 = WindowedFullscreen(그 밖 Unsupported) | `toggleFullScreen:` = WindowedFullscreen |

### 19.4 한계

- 영역 판정은 마지막 그리기 기준이다(UE와 같다). 그리기 없이 바뀐 상태는 다음 그리기까지 늦다.
- 독점 전체 화면은 렌더러 지원 뒤다.
- TSF는 UW4f3이다.
- 시스템 메뉴(Alt+Space) 같은 크기 조절 밖의 OS modal 루프 동안에는 프레임이 멈춘다(UE와 같다).
