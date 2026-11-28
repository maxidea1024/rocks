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
  - 넘치면: Caption과 구멍을 빼고 누르는 영역(버튼·시스템 메뉴·Popup)만 게시하고 로그를 한 번 남긴다(그 밖은 Client — 끌 수 없을 뿐 잘못 누르지는 않는다). 이전 지도를 유지하지 않는다(영구히 낡는다).
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
- `platform::HasPendingWindowEvents()`: 대기가 바로 돌아오는 조건(가져가지 않은 창 이벤트)이다. 시험은 대기 시간을 재지 않고 이 조건을 단언한다.
- `platform::WakeMessageWait()`: 다른 스레드에서 대기를 푼다(비동기 작업 완료). Win64는 `PostThreadMessage`, 나머지는 condition variable notify다.
- 대기 시간은 호스트가 모든 `Application`의 다음 타이머 마감으로 정한다(`Application::SecondsUntilNextTimer` — caret 깜빡임·툴팁·하위 메뉴 0.5초 지연). 지금의 고정 50 ms를 대신한다.
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
  - SVG는 누적 배율로 래스터한다(PaintList가 그려지는 크기로 Resolve). 글자는 지금 글리프 cache를 배율 1로 래스터해 늘려 그린다. 최종 배율 shaping은 `TextLabel`이 `FontInfo`·크기를 받아 스스로 shaping하는 3.12절 단계(병합 뒤 UW4 항목)에서 한다. 그 전에도 DPI를 인식하지 않을 때(DWM이 창 전체를 늘림)보다 나빠지지 않는다.
- **snapshot·시험.** `--snapshot`·`--self-test`는 `EnableHighDpiAwareness`를 부르지 않고 s = 1로 고정한다(결정적, 회귀 기준이 배율 1). 배율 시험은 가짜 배율(s = 2)로 한다.

**텍스트 입력 문맥(PW3, UE `ITextInputMethodContext`·`ITextInputMethodSystem`·`ITextInputMethodChangeNotifier`).**
- **`ui::TextInputContext`(UiCore, UE 대응).** 그래픽·플랫폼과 무관한 인터페이스라 UiCore에 둔다(UiWidget 편집 요소가 구현하고, UiFramework 입력 adapter가 서비스를 부른다).
  - 질의: `IsReadOnly`, `AllowsInputMethod`(비밀번호·읽기 전용이면 false), `IsComposing`, `GetTextLength`, `GetSelection`, `GetText(범위)`, `GetCharacterIndexFromPoint`, `GetTextBounds(범위)`, `GetScreenBounds`
  - UE의 `GetWindow`는 없다. 서비스가 창마다 하나라 문맥이 찾은 서비스가 곧 창이다.
  - 수정: `SetSelection`, `SetText(범위)`
  - 조합: `BeginComposition`·`UpdateCompositionRange`·`EndComposition`
  - 위치 공간은 UTF-16 code unit이고, 줄 끝은 `\n` 한 글자다(3.12절, UE와 같다). UTF-8 텍스트와의 변환은 context 구현이 한다.
  - `GetTextBounds`·`GetScreenBounds`는 UI 창 좌표(= client 픽셀)다. IMM은 그대로 쓰고, TSF는 `ClientToScreen`한다(UE의 `GetWindowRect` 빼기는 OS 테두리 창에서 틀린다 — WindowsTextInputMethodSystem.cpp 407–413행).
  - **조합 중 `SetText`는 context 안의 미리보기 층만 바꾼다.** `EndComposition`에서 한 번 커밋한다(편집 이력 하나, `TextChanged` 한 번). 조합 갱신마다 바인딩 쓰기·검증·최대 길이 판정이 일어나지 않게 하려는 것이다.
- **`ui::TextInputService`(UiCore, 창별 — UE `ITextInputMethodSystem` + `ITextInputMethodChangeNotifier`).**
  - `Activate(context)`·`Deactivate(context)`·`CancelComposition(context)`·`NotifyLayoutChanged(context)`·`Process(기록)`.
  - UE는 등록(`RegisterContext`)과 활성을 나눈다. 여기서는 활성 문맥이 창마다 하나라 Activate가 등록을 겸한다. 초점을 받으면 Activate, 잃거나 소멸하면 Deactivate한다.
  - UE 통지 중 `NotifyTextChanged`·`NotifySelectionChanged`는 IMM에 쓸 데가 없다(IMM은 조합 범위 밖 글자를 묻지 않는다). TSF(UW4f3)가 같은 인터페이스에 더한다.
  - `CancelComposition`은 앱이 조합 중에 글자를 바꿀 때(Esc 되돌리기, 바인딩, 읽기 전용 전환) 문맥이 부른다. 문맥은 부르기 전에 스스로 미리보기를 버린다. 서비스는 OS 조합을 취소하고 남은 기록을 버린다.
  - Element는 자기 창의 서비스를 루트로 올라가며 찾는다(`SetTextInputService`·`FindTextInputService` — `SetPopupWorkArea`와 같은 방식). `Application::SetTextInputService`가 루트에 둔다.
  - 서비스 구현은 창 호스트가 한다. `ui::TextInputContext`를 `platform::TextInputClient`로 감싸 창에 넘기는 어댑터다(UiDemo `DemoTextInput`). UiCore·UiFramework는 PlatformWindow에 의존하지 않는다.
- **`platform::TextInputClient`와 창 API(PlatformWindow, 창별).**
  - `TextInputClient`는 플랫폼이 부르는 문맥의 부분이다: `AllowsInputMethod`·`GetSelection`·`SetSelection`·`SetText`·`GetTextBounds`·`BeginComposition`·`UpdateCompositionRange`·`EndComposition`.
  - `Window::ActivateTextInput(client)`·`DeactivateTextInput(client)`·`CancelTextComposition(client)`·`NotifyTextInputLayoutChanged(client)`·`ProcessTextInput(기록)`. 모두 창 소유 스레드(게임 lane)에서 부른다. `thread_local` 대상 창은 없앤다.
  - 기록은 일련번호로 든다. Activate·Deactivate·CancelComposition은 남은 기록을 지우고, 번호는 다시 쓰지 않는다. 그래서 큐에 남은 옛 표지가 새 문맥의 기록을 처리하는 일이 없다(UE id·세대 대신 — CoreObjectInputDetailedPlan TextSessionId·늦은 commit 거부와 같은 효과다).
  - `ProcessTextInput(n)`은 번호 n 이하의 남은 기록을 순서대로 처리한다. 입력 큐가 넘쳐 표지 하나를 잃어도 다음 표지가 앞 기록까지 처리한다. 표지가 끝내 오지 않으면 Deactivate가 남은 기록을 먼저 처리한다.
  - Deactivate는 OS 조합을 완료시키고(IMM `CPS_COMPLETE` — 초점을 옮기면 만들던 글자가 들어가는 Windows 관례), 그 확정을 처리한 뒤, 그래도 조합 중이면 미리보기를 그대로 확정한다.
  - 기록을 문맥에 적용하는 규칙(조합 범위 추적, 시작 때 선택 영역을 조합으로 바꿈)은 플랫폼 공용 `TextInputSession`(Private/Common)이 맡는다. OS 호출이 없어 EngineTests로 시험한다.
  - 비밀번호·읽기 전용 입력란은 IME를 끈다(`ImmAssociateContext(nullptr)` — UE와 같다). 이때 확정 문자는 `WM_CHAR` → `Text`로 온다.
- **Win64 1차는 IMM이다. 순서 보장이 핵심이다.**
  - 활성 context가 있을 때:
    - `WM_IME_*`를 받으면 문자열(확정·조합)과 커서를 메시지 시점에 읽어 창 기록 버퍼에 기록한다. 처리 시점에 읽으면 한 펌프 안의 연속 확정 두 개 중 앞의 것을 `GCS_RESULTSTR`이 덮어 잃는다.
    - 그 창 입력 큐에 `TextInputMethod` 표지 이벤트(기록 번호, trivially copyable)를 넣는다.
    - `InputAdapter`가 그 위치에서 `TextInputService::Process(기록)`을 불러 키·문자 이벤트와 순서를 지킨다. 예: [확정 "한", KeyDown(Left)]에서 확정이 먼저 들어간다. UE는 `WM_CHAR`·키·IME를 한 미룬 큐에 순서대로 넣는다(WindowsApplication.cpp 2310–2330행).
    - `WM_IME_COMPOSITION`을 `DefWindowProc`에 넘기지 않거나 `WM_IME_CHAR`를 삼킨다(UE WindowsTextInputMethodSystem.cpp 1157–1161행). 그래야 확정 문자열이 `WM_CHAR`로 두 번 오지 않는다.
  - 활성 context가 없으면 `DefWindowProc` → `WM_IME_CHAR` → `WM_CHAR`로 `Text` 이벤트가 온다. 게임 문자 입력·`InputState`가 IME 글자를 계속 받는다.
  - **취소 판정(UE `CancelIMMComposition`, WindowsTextInputMethodSystem.cpp 1052–1055·1094–1097행).** `lParam == 0`과 확정 없이 빈 조합 문자열은 '끝' 기록이다. Esc에 문자열만 비우고 `WM_IME_ENDCOMPOSITION`을 보내지 않는 IME가 있어서다. 기록 세션은 조합 중이 아닐 때 온 빈 조합을 무시한다(선택 영역을 지우지 않는다).
  - **커서.** `GCS_CURSORPOS`가 있고 `CS_NOMOVECARET`가 없을 때만 커서를 읽고 아래 16 bit를 쓴다(UE 1127–1131행). 그 밖에는 조합 끝이다.
  - **읽기 실패.** 조합 메시지를 삼킨 뒤 문자열을 읽지 못하면 넘침과 같이 처리한다(조합 취소·경고 로그). DefWindowProc로 넘기면 `WM_IME_CHAR` 삼키기와 엇갈려 글자가 사라지기 때문이다.
  - **SETCONTEXT 다시 받기.** `ISC_SHOWUICOMPOSITIONWINDOW`는 `WM_IME_SETCONTEXT` 때 정해진다. 문맥 수락이 바뀌면(Activate·Deactivate·IME 허용 변경) 창에 초점이 있을 때 `WM_IME_SETCONTEXT`를 다시 보내 기본 조합 창 표시를 맞춘다. UE는 문맥이 없으면 IME를 떼서 이 문제가 없지만, Rocks는 문맥이 없을 때도 게임 문자 입력에 IME를 쓴다.
  - **늦은 확정 배출.** `CPS_COMPLETE` 뒤에도 조합 문자열이 남아 있으면(TSF·CUAS 계열이 확정을 늦게 보냄) 미리보기를 그대로 확정하고 배출 상태로 둔다. 배출 중에는 조합 메시지와 `WM_IME_CHAR`를 삼킨다. `WM_IME_ENDCOMPOSITION` 또는 다음 `WM_IME_STARTCOMPOSITION`이 배출을 끝낸다. 늦은 확정이 새 초점이나 게임에 한 번 더 들어가지 않게 하려는 것이다.
  - **표지 잃음.** 창 대기 큐가 차서 표지를 넣지 못하면 가장 큰 번호를 기억했다가 다음 `TakeEvents`에서 넘겨받는 큐 끝에 넣는다. 입력 adapter는 넘친 배치를 버릴 때도 그 배치의 가장 큰 표지 번호로 `ProcessTextInput`을 부른다. 마지막 확정이 다음 IME 메시지까지 안 보이는 일이 없게 한다.
  - 조합 창(`CFS_POINT`)은 caret 아래, 후보 창(`CFS_EXCLUDE`)은 caret 사각형을 피해 둔다(UE 413–432행).
  - 후보창 위치는 context의 `GetTextBounds`로 당겨 오고(pull), `CFS_EXCLUDE`로 줄을 가리지 않게 한다(UE).
- **기록 버퍼.** 기록 형식은 (일련번호, 종류 조합·확정·끝, 문자열 offset·길이, 커서)이고, 문자열은 UTF-16 그대로다.
  - 용량은 기록 64개·문자 4096 code unit이고 창 객체 안 고정 배열이다(OS 콜백 안 할당 없음). 처리가 끝나 비면 처음부터 다시 쓴다.
  - 넘치면 남은 조합 갱신을 지운다(뒤의 조합·확정이 미리보기를 어차피 덮는다). 확정은 지우지 않는다.
  - 그래도 넘치면 남은 기록을 모두 버리고 '끝' 기록 하나를 넣는다. 창은 OS 조합을 취소하고(`CPS_CANCEL`) 경고 로그를 남긴다. 그 동안의 확정은 잃는다(한 프레임에 4096 code unit을 넘게 확정하는 IME는 없다고 본다).
  - 처음 안의 "게임 lane이 필요하면 키운다"는 버렸다. 고정 용량에서 넘침 규칙이 정해져 있고, 키우는 시점을 펌프 앞에 따로 둘 이유가 없다.
- **TSF.** 같은 인터페이스 위의 UW4f3b다(세부 22.13절). TSF의 동기 질의는 창의 거울 문서로 답하고 수정은 기록으로 넘긴다(Mac과 같은 원칙). 그래서 펌프·대기 중 여부와 상관없이 UI를 고치지 않는다(원안의 "lane 대기 펌프면 `TS_E_SYNCHRONOUS`" 규칙은 버렸다).
- **입력 이벤트에서 조합 문자열이 빠진다.**
  - `TextComposition`·`TextCompositionCommit` 이벤트와 `InputEvent::composition`을 지우고, `Text`(code point 하나)와 `TextInputMethod` 표지만 남긴다. UE도 IME 문자열은 이벤트가 아니라 context로 간다.
  - `InputEvent`는 trivially copyable이 된다(`static_assert`).
  - 이벤트 문자열을 프레임 버퍼 offset으로 바꾸는 항목은 필요 없어진다(9.1절 UW7 행 정정).
- **Android·Mac.**
  - Android는 소프트 키보드(UE `IVirtualKeyboardEntry`)로 같은 context를 쓴다.
  - Mac은 메인 스레드가 게임 lane을 기다릴 수 없다(MacPlatform MD1). 그래서 `NSTextInputClient`의 동기 질의는 게임 lane이 게시한 snapshot(텍스트·선택·조합·caret 사각형, 잠금)으로 답하고, 수정은 기록으로 게임 lane에 넘긴다.
  - 둘 다 구현은 그 플랫폼 단계다.

### 19.5 메뉴 뒤 커서 지연 보고(UW9g — 2026-11-27 사용자 보고)

- **보고.** UiDemo에서 긴 하위 메뉴(File > Recent Levels — 40줄 + 구분선 + 지우기, 창보다 길어 스크롤)를 연 뒤, 상단 메뉴 막대 아래로 마우스를 움직이면 커서가 이전 위치에 보이다가 따라온다.
- **원인(사용자 설명으로 확정).** 늦게 따라오는 것은 커서가 아니라 **메뉴 강조**다. 강조가 이전 항목으로 잠깐 되돌아갔다가 커서가 있는 항목으로 온다. 늘 재현된다(데모에서 하위 메뉴가 있는 메뉴는 Recent Levels 하나뿐이다). 화면 안 배치(UW7d)와는 상관없다.
  - `MenuRow::IsHighlighted`는 하위 메뉴가 열린 항목을 "패널에서 hover 중인 항목이 없으면" 강조했다.
  - 포인터가 항목 사이 틈·구분선·패널 여백 위에 있는 동안에는 hover 항목이 없다. 그래서 강조가 열린 항목(Recent Levels)으로 돌아갔다.
  - 그 순간 `OnRowUnhovered`가 대기 중인 하위 메뉴 바꾸기를 취소한다(UE `CancelPendingSubMenu`와 같다). 그래서 계속 움직이는 동안 하위 메뉴가 닫히지 않고 같은 일이 되풀이된다.
  - UE는 열린 하위 메뉴 항목을 "열렸고 소유 메뉴 위젯이 hover가 아닐 때"만 hover처럼 보인다(`SMenuEntryBlock::ShouldSubMenuAppearHovered`, SMenuEntryBlock.cpp 1136–1141).
  - 고침: `MenuPanel`이 hover 경로의 진입·이탈로 포인터가 패널 위에 있는지 센다(`IsPointerInside`). 열린 항목은 그것이 거짓일 때만 강조한다. 회귀 시험은 `MenuSubMenuSurvivesPassingOverSibling`에서 구분선 위를 지날 때 강조가 없는지 본다.
  - 아래 (a)–(c)는 같은 조사에서 나온 별개 개선으로 그대로 둔다.
- **확인한 사실.**
  - 사용자가 돌린 exe는 UW8a 시점의 Debug 빌드다(그 뒤는 미빌드).
  - 앱이 OS 커서 *위치*를 늦출 수 있는 경로가 코드에 없다. `WH_MOUSE_LL`·`SetCursorPos`·`ClipCursor`(UW9c 전)·raw input을 쓰지 않는다. Windows 하드웨어 커서는 시스템 입력 스레드가 옮긴다.
  - 커서 *모양*은 `DockWindows::Render` 끝에서 정한다. 그 앞에 배치·표면 기록·paint cache·영역 지도·batch가 돈다. 그 사이 OS의 `WM_SETCURSOR`는 이전 프레임 위치의 모양을 쓴다.
  - `BuildWindowRegionMap`은 바뀐 것이 없어도 매 프레임 돈다. 겹침 판정이 O(n²)이다(Client 항목마다 그 아래 항목 전부).
  - 닫은 뒤 남는 상태(팝업 층·타이머·영역 넘침)는 찾지 못했다.
- **가설.**
  - H1 — 무거운 프레임 처리 뒤에 커서 모양·hover가 바뀌어, 새 위치에 옛 모양이 보인다. 지연은 Debug 프레임 하나다. 사용자에게는 "커서가 늦는다"로 보일 수 있다.
  - H2 — vsync가 꺼져 있고(`UiWindowSurface` 기본) 제출 묶음이 없다. GPU가 포화하면 소프트웨어 커서 구성(원격·일부 하이브리드 GPU·화면 녹화)에서 DWM 합성이 늦어 진짜 커서가 끌린다. 일반 기기에서는 가능성이 낮다.
  - 둘을 가르려면 계측이 필요하다.
- **조치.**
  - (a) 커서 모양은 입력 처리 직후 한 번 정하고, 배치 뒤에도 한 번 더 정한다. 배치가 바뀌면 커서 아래 Element가 달라질 수 있어서다. UE는 이동 이벤트 때 커서 질의를 요청만 하고(`FSlateUser::RequestCursorQuery`), 실제 질의(`QueryCursor`)는 메시지 처리 뒤 `TickPlatform`의 `UpdateCursor`에서 한다(SlateApplication.cpp 1725행, SlateUser.cpp 962–965행). Rocks도 입력 처리 직후 정하므로 순서는 같다(리뷰 2026-11-27 인용 정정).
  - (b) 영역 지도의 겹침 판정은 비 Client 항목만 먼저 모아 그것과만 대조한다(O(n·m), m은 막대·테두리 몇 개). 결과 지도는 전과 같다. 다시 만들지 않는 캐시는 두지 않는다 — 지도는 hit grid 계획에 들어 있지 않은 실시간 상태(후보의 보이기·입력 경로)에도 기대므로 계획 revision만으로는 바뀜을 알 수 없다. 게시는 지금처럼 바뀐 때만 한다(`SameAs`).
  - (c) UiDemo `--frame-stats`는 창마다 단계별 시간(입력·배치·기록·paint cache·영역 지도·batch)과 hit 후보 수를 60프레임마다 `Frame stats Window <번호>`로 남긴다(번호는 만든 순서 — 0이 주 창). 제출 대기는 모든 창을 한 번에 하므로 따로 `Frame stats Submit`으로 남긴다. 재지 않은 단계는 적지 않는다. 진단용이며 시험이 시간에 기대지 않는다.
  - (d) 프레임 페이싱(H2 — frame-latency waitable 또는 표시 주기 제출)은 (c)로 확인한 뒤 18.4절에서 정한다. 증거 없이 vsync 기본을 바꾸지 않는다.
- **곁가지 결함(같은 조사에서 찾음).**
  - `MenuBar::ScheduleHiddenCleanup`이 간격 0으로 `StartTimer`를 부르는데 `StartTimer`는 0 이하를 거부한다. 넘쳐 숨은 메뉴의 정리가 돌지 않는다. 다음 프레임 정리로 바꾼다.
  - `WindowChrome::Arrange`가 `Overlay::Arrange`를 대신하면서 팝업 층의 `SetPopupArea`를 하지 않는다. 직접 그린 테두리 창의 팝업이 모니터 작업 영역 제한(UW7d)을 받지 않는다.
- **시험.**
  - 영역 지도: 버튼 60개 + 제목 막대 트리에서 비교 횟수 상한(후보 수 × 8)을 단언하고(`WindowRegionMapStats`), 점마다 질의와 같은지 본다(`RegionMapMatchesQuery`).
  - WindowChrome 팝업 허용 영역: 메뉴 오른쪽 끝이 작업 영역 안이다(`PopupWorkAreaTests`).
  - 숨은 메뉴 정리: 넓혀졌을 때 열린 넘침 메뉴가 닫히는 기존 시험(`MenuBarOverflowHidesMenusThatDoNotFit`)이 이제 다음 프레임 정리로 통과한다.
  - 커서 호출 순서: UiDemo 창 호스트(`DockWindows`)에는 단위 시험 틀이 없다. 최종 빌드 뒤 사람 확인으로 본다.
- **사람 확인.** 최종 빌드 뒤 Debug·Release에서 `--frame-stats`로 증상을 다시 본다. 남으면 H2로 페이싱을 정한다.

### 19.3 단계

| 단계 | 범위 | 완료 판정 |
|---|---|---|
| UW7e | 스레드 계약(문서, Release 밖 단언)<br>창 모드(`SetWindowMode`·`GetWindowMode`, 두 번 누르기 표지, 제목 줄 위젯의 전체 화면 해제, `ToggleFullscreen`·`FullscreenCaption`·backend 전환 제거)<br>펌프 수준 대기(`PumpMessages`·`TakeEvents`·`WaitForMessages`·`WakeMessageWait`, `Application::SecondsUntilNextTimer`, 창 단위 `WaitForEvents` 제거 — 50 ms 결함 수정)<br>UiDemo 루프 이관 | PlatformWindow 시험: 다른 창의 미룬 이벤트가 있으면 대기가 바로 돌아온다, `WakeMessageWait`·보낸 메시지로 대기가 풀린다, 모드 왕복(실제 모드)<br>UiFramework 시험: 타이머 마감<br>self-test |
| UW7f | 영역 지도 게시(`WindowRegionMap`·`PublishRegionMap`·`BuildWindowRegionMap`, hit grid 항목 순회, `popupOpen`)<br>`RegionHitTest` delegate 제거 | 지도와 `QueryWindowRegion`의 점 격자 동치(메뉴 열림·최대화·툴팁·넘침·Reset 뒤)<br>판정 순서(가장자리가 Caption보다 먼저 — 기존 CustomFrameHitZones 이관)<br>Win64 판정 시험 |
| UW7g | DPI(프로세스 PMv2, `DpiScale`·`ConsumeDpiChange`, `AdjustWindowRectExForDpi`·숨긴 생성, `WM_DPICHANGED` 배율 먼저, 호스트 루트 변환 배율, 글꼴·SVG 배율, snapshot·self-test 배율 1) | 가짜 배율 2에서 배치·hit·영역 지도·작업 영역 시험<br>`WM_DPICHANGED`를 숨긴 창에 보내 배율이 먼저 바뀌는지<br>실제 모니터는 사람 확인 |
| UW7h | 텍스트 입력 문맥(UiCore `TextInputContext`·`TextInputService`, PlatformWindow `TextInputClient`·창 API·`TextInputSession`, Win64 IMM 기록·표지 이벤트·`WM_IME_CHAR` 삼키기, 후보창 `CFS_EXCLUDE`)<br>`EditableTextElement`가 context 구현(미리보기 층, 확정 1회)<br>조합 이벤트·`InputEvent::composition`·`ui::Platform` 조합 함수·`InlineComposition.h` 제거<br>이관 대상: InputEvent.h, InputState.cpp, Application.cpp, InputAdapter.cpp, EditableTextElement.cpp, EditableTextWidgetTests.cpp(가짜 system), GameHud.cpp, Win64·Android·Unsupported 구현 | context 시험: UTF-16 위치 변환·조합 범위·선택·확정 1회<br>IMM 기록 시험: 확정 + Left 순서, 연속 확정 두 개, context 해제 뒤 기록 버림, 넘침<br>`InputEvent` trivially copyable<br>한국어 IME 사람 확인 |

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
- 배율이 1보다 큰 모니터에서 글자는 3.12절 자체 shaping 전까지 늘려 그린다(19.2절 DPI).
- TSF는 UW4f3이다.
- 시스템 메뉴(Alt+Space) 같은 크기 조절 밖의 OS modal 루프 동안에는 프레임이 멈춘다(UE와 같다).
