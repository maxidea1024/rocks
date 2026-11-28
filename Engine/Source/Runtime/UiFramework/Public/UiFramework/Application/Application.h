#pragma once

#include <Input/WindowRegionMap.h>

#include <UiCore/CacheRoot.h>
#include <UiCore/CursorService.h>
#include <UiCore/HitGrid.h>
#include <UiFramework/Application/Commands.h>
#include <UiFramework/Application/AccessibleNode.h>
#include <UiCore/FocusCause.h>
#include <UiCore/NavigationConfig.h>
#include <UiCore/StyleRegistry.h>
#include <UiFramework/Application/UiTimer.h>

namespace rocks::ui {

// BuildWindowRegionMap의 작업량이다(Docs/Design/UiToolkit 19.5절 — 선형 상한 시험·진단용).
struct WindowRegionMapStats {
    // 본 hit 후보 수와 사각형 겹침 비교 수다.
    usize candidates = 0;
    usize comparisons = 0;
};

// 포인터 이벤트를 전달한다. 콜백 전에 경로의 참조를 보관하므로 처리 중 Element를 제거해도
// 현재 이벤트의 전달 대상이 소멸하지 않는다.
class ROCKS_UIFRAMEWORK_API Application {
public:
    explicit Application(Allocator& allocator) noexcept;

    ~Application() noexcept;

    // 화면 트리에 붙은 동안 interval초마다 프레임 시작에서 실행한다. 반환된 타이머의 마지막
    // 참조를 놓으면 예약이 취소된다. 지연된 프레임에서 밀린 콜백을 연속 실행하지 않는다.
    Result<SharedPtr<UiTimer>> StartTimer(const SharedPtr<Element>& owner, double interval,
                                          Delegate<void()>&& callback) noexcept;

    // 화면 트리에 붙은 요소에만 프레임 간 경과 시간을 전달한다. 토큰을 놓으면 예약이 끝난다.
    Result<SharedPtr<UiTimer>> StartFrameTimer(const SharedPtr<Element>& owner,
                                               Delegate<void(double)>&& frame) noexcept;
    // 창이 먼저 소멸할 수 있는 나중 작업은 이 토큰을 잠근 뒤 Application을 사용한다.
    Result<WeakPtr<Application*>> WeakContext() noexcept;
    // element가 붙은 트리의 Application이다(Docs/Design/UiToolkit 22.7a절·16.5절 — UE
    // FSlateApplication::Get 대응, 전역 없이 루트의 호스트 묶음으로 찾는다). 트리에 붙지 않았거나
    // Application이 없으면 nullptr이다. 돌려준 포인터는 그 Application이 사는 동안만 쓴다(나중
    // 작업은 WeakContext).
    //
    // 사용 예(Element 안에서 메뉴를 열 때):
    //   if (Application* app = Application::Find(*this)) { (void)anchor->OpenFocused(*app); }
    static Application* Find(const Element& element) noexcept;
    void SetRoot(const SharedPtr<Element>& root) noexcept;
    // 지금 루트다(툴팁 임시 호스트로 감쌌으면 그 호스트). 없으면 빈 포인터다.
    const SharedPtr<Element>& GetRoot() const noexcept;
    // 팝업 허용 영역 공급자를 등록한다(Docs/Design/UiToolkit 9.1절 UW7d — 메뉴·하위 메뉴·콤보·툴팁
    // 등 모든 팝업이 모니터 작업 영역을 넘지 않는 시스템 규칙). 창 호스트가 한 번 부른다(보통
    // platform::Window::WorkAreaAt을 잇는다). Application은 지금과 이후의 루트(툴팁 임시 호스트
    // 포함)에 단다. 빈 delegate면 뗀다(팝업은 창 경계만 지킨다).
    Result<void> SetPopupWorkArea(Element::PopupWorkArea&& provider) noexcept;

    // 창의 텍스트 입력 서비스다(Docs/Design/UiToolkit 19.2절). 창 호스트가 한 번 부르고,
    // Application은 지금과 이후의 루트에 단다. 편집 요소가 찾아 Activate하고, 입력 adapter가 IME
    // 표지 이벤트에서 Process를 부른다. 빈 포인터면 뗀다(IME 문자는 Text 이벤트로만 온다).
    Result<void> SetTextInputService(SharedPtr<TextInputService> service) noexcept;
    // IME 기록 표지(TextInputMethod)를 서비스에 넘긴다. 서비스가 없으면 false다.
    bool ProcessTextInput(uint32 record) noexcept;

    // 창의 커서 서비스다(Docs/Design/UiToolkit 23.5절). 잠근 capture(PointerReply::lockCursor)에
    // LockPointer, 풀릴 때 UnlockPointer를 부른다. 빈 포인터면 뗀다(잠금은 capture만 하고 delta는
    // 위치 차이다). 이미 잠근 capture는 잠글 때의 서비스로 푼다.
    Result<void> SetCursorService(SharedPtr<CursorService> service) noexcept;
    // 그 포인터의 capture가 커서를 잠갔는지다(시험·진단).
    bool IsCursorLocked(PointerSource source, uint32 user, uint32 pointer) const noexcept;

    // 위젯 스타일 색을 풀 테마다(UW3b — 기본은 UE 다크 테마). 바꾸면 그리기 캐시를 모두 다시
    // 만든다. BuildPaintList에 넘긴 목록은 이 Application의 테마를 빌린다.
    void SetTheme(const ThemeColors& theme) noexcept;
    const ThemeColors& Theme() const noexcept;

    // 벡터 brush(SVG 아이콘)를 래스터 자리로 바꿀 resolver다(Docs/Design/UiToolkit 17.2절 —
    // 렌더러의 VectorImageAtlas). resolver는 이 Application보다 오래 살아야 한다. resolver가
    // atlas를 비워 Revision이 바뀌면 다음 그리기 갱신이 테마 revision을 올려 캐시를 모두 다시
    // 만든다.
    void SetVectorImageResolver(VectorImageResolver* resolver) noexcept;

    // 변경된 배치를 반영하고 목록을 새로 기록한다. 지역 CachePanel의 기록은 재사용한다.
    Result<void> BuildPaintList(const Geometry& geometry, float layoutScale,
                                PaintList& list) noexcept;

    // 창 전체 캐시를 선택한 호스트의 프레임 진입점이다. Revision이 같으면 이전 배치를 재사용한다.
    Result<const CacheRoot*> UpdatePaintCache(const Geometry& geometry, float layoutScale) noexcept;

    // 오프스크린 표면을 사용하는 호스트는 배치를 끝낸 뒤 표면을 준비하고 그리기 캐시를 갱신한다.
    Result<void> PrepareLayout(const Geometry& geometry, float layoutScale) noexcept;
    Result<const CacheRoot*> UpdateArrangedPaintCache() noexcept;

    // focus는 사용자별이다. 콜백 안에서 focus를 다시 변경하면 Busy를 돌려준다.
    // 빈 scope에는 미처리 키를 받을 창 전역 명령 목록을 지정한다.
    Result<void> SetCommands(const SharedPtr<Element>& scope,
                             const SharedPtr<CommandList>& commands) noexcept;
    // 창 단위 명령 목록을 더한다(Docs/Design/UiToolkit 9.1절 UW7c R4). 초점 경로·전역 목록이
    // 처리하지 않은 키를 본다 — 메뉴 막대 가속키처럼 그 창에만 있는 명령이다. 같은 목록을 다시
    // 더하면 AlreadyExists다. 목록 소유자가 사라지기 전에 RemoveWindowCommands로 뗀다.
    Result<void> AddWindowCommands(const SharedPtr<CommandList>& commands) noexcept;
    void RemoveWindowCommands(const CommandList& commands) noexcept;
    bool IsControlDown(uint32 userId) const noexcept;
    bool IsShiftDown(uint32 userId) const noexcept;
    bool IsAltDown(uint32 userId) const noexcept;

    // 스타일 등록부다(Docs/Design/UiToolkit 20.11절 — 호스트가 하나 만들어 모든 Application에
    // 준다). 바꾸면 활성 집합 이름으로 다시 찾는다.
    void SetStyleRegistry(SharedPtr<StyleRegistry> registry) noexcept;
    // 활성 앱 스타일 집합을 이름으로 고른다(UE FAppStyle — 기본 CoreStyleName, 에디터는
    // EditorStyle). 등록부에 없으면 NotFound이고 바꾸지 않는다. 바뀌면 전체 측정·그리기를 다시
    // 한다.
    Result<void> SetAppStyle(StringView name) noexcept;

    // UI 시계다(Docs/Design/UiToolkit 20.3절 — 툴팁 지연·타이머·애니메이션 부품이 모두 이것을
    // 본다). 비우면 단조 시계다. 시험·snapshot은 ManualClock을 준다. 바꾸면 열린 툴팁을 닫고 다음
    // BeginFrame을 첫 프레임(간격 0)으로 본다.
    void SetClock(ClockFunction&& clock) noexcept;
    // 사용자의 키 → navigation 대응이다(Docs/Design/UiToolkit 20.7절 — UE 사용자별
    // FNavigationConfig). 정하지 않은 사용자는 기본값(NavigationConfig::Default)이다.
    Result<void> SetNavigationConfig(uint32 userId, const NavigationConfig& config) noexcept;
    const NavigationConfig& GetNavigationConfig(uint32 userId) const noexcept;
    // 프레임을 시작한다: 시계를 한 번 읽어 프레임 시각을 정하고(간격은 MaxFrameDeltaSeconds로
    // 자른다 — UE TickTime) host 묶음에 싣는다. 호스트는 입력 처리 전에 부른다. 부르지 않은 채
    // PrepareLayout이 오면 그것이 먼저 부른다.
    //
    // 사용 예(호스트 루프):
    //   app.BeginFrame();
    //   adapter.RouteBatch(events, consumed, dropped);
    //   app.PrepareLayout(root, scale);
    void BeginFrame() noexcept;
    // 미룬 해체 통지를 넣는다(Docs/Design/UiToolkit 21.4절 — 소멸자에서 사용자 코드를 부르지 않는
    // 요소가 쓴다). 프레임 시작(BeginFrame·암묵 프레임)과 Shutdown에서 돈다.
    Result<void> DeferTeardown(Delegate<void()>&& teardown) noexcept;
    // 대기열을 지금 돌린다(돈 항목 수). 호스트가 창을 부수기 전에 Shutdown으로 부른다.
    usize RunDeferredTeardown() noexcept;
    usize PendingTeardownCount() const noexcept;
    // 루트를 떼고 미룬 해체 통지를 모두 돌린다. 호스트가 Application을 부수기 전에 부른다 —
    // 부르지 않으면 소멸 때 남은 통지는 돌지 않고(소멸자 규칙) 경고 로그에 개수가 남는다. 대기열
    // 항목은 UserWidget 등을 RootRef로 붙잡으므로 ObjectRegistry는 이 Application보다 오래 살아야
    // 한다(버릴 때도 RootRef가 저장소를 만진다).
    void Shutdown() noexcept;
    // 바깥 시간으로 프레임을 시작한다(월드 위젯 — World 시간, UE WidgetComponent). 유한하지 않은
    // 시각은 무시하고, 간격은 0과 MaxFrameDeltaSeconds 사이로 자른다.
    void BeginFrame(const FrameTime& time) noexcept;
    const FrameTime& GetFrameTime() const noexcept;
    // 다음 타이머·툴팁 마감까지 남은 초다. 호스트가 펌프 대기 시간을 정한다(Docs/Design/UiToolkit
    // 19.2절 — caret 깜빡임·툴팁 지연·하위 메뉴 지연). 프레임 타이머나 진행 중인 툴팁 페이드가
    // 있으면 0, 기다릴 것이 없으면 NoTimerDeadline이다. 이미 지난 마감도 0이다.
    //
    // 사용 예(호스트 루프):
    //   const double wait = app.SecondsUntilNextTimer();
    //   platform::WaitForMessages(ToMilliseconds(wait));   // 입력·깨움·마감 중 먼저 온 것까지
    double SecondsUntilNextTimer() const noexcept;

    Result<void> SetToolTipTiming(double delay, double fade) noexcept;
    double ToolTipDelay() const noexcept;
    double ToolTipFade() const noexcept;
    // 활성 화면의 접근성 트리를 부모 우선으로 복사한다. 첫 노드의 parent는 IndexNone이다.
    Result<Array<AccessibleNode>> AccessibleTree(uint32 user = 0) const noexcept;
    bool IsToolTipOpen(uint32 user) const noexcept;
    void CloseToolTip(uint32 user) noexcept;

    bool IsHovered(const Element& target, uint32 user = 0) const noexcept;
    // 어느 사용자든 target(또는 그 자손) 위에 포인터를 두고 있는지다(hover 경로 조회 — 진입·이탈
    // 통지를 세지 않으므로 통지 중 경로가 바뀌어도 어긋나지 않는다).
    bool IsHoveredByAnyUser(const Element& target) const noexcept;
    void ClearHover(uint32 user) noexcept;
    bool HasAnyUserFocus(const Element& element) const noexcept;
    // 여러 가상 창을 가로지르는 포인터의 다음 이벤트를 어느 창에 보낼지 결정한다.
    bool HasPointerCapture(PointerSource source, uint32 user, uint32 pointer) const noexcept;
    bool HasMouseCapture(const Element& element, uint32 user, uint32 pointer) const noexcept;
    SharedPtr<Element> GetFocus(uint32 userId) const noexcept;
    // cause가 Navigation이면 대상에 초점 외곽선을 켠다(9.1절 UW4f1). 같은 대상에 다시 부르면
    // 원인과 외곽선만 바꾼다(키보드로 옮긴 초점을 마우스로 누르면 외곽선이 꺼진다).
    Result<void> SetFocus(uint32 userId, const SharedPtr<Element>& target,
                          FocusCause cause = FocusCause::Direct) noexcept;

    // 직접 소유자가 보관한 참조를 찾아 같은 포커스 검사를 적용한다.
    Result<void> SetFocus(uint32 userId, Element& target,
                          FocusCause cause = FocusCause::Direct) noexcept;
    // 사용자 초점의 원인이다. 초점이 없으면 Direct다.
    FocusCause GetFocusCause(uint32 userId) const noexcept;

    // focus 경로의 키·문자 이벤트를 전달한다. 문자와 키 해제는 미리보기 없이 전달한다.
    // 패드 버튼 누름·뗌을 키 경로로 보낸다(Docs/Design/UiToolkit 20.8절 — KeyEvent::chord.gamepad,
    // D-pad는 navigation, South 확인·East 뒤로). 하나라도 처리하면 true다.
    bool RouteGamepadButtons(uint32 user, uint32 pressed, uint32 released = 0,
                             const SharedPtr<Element>& scope = {}) noexcept;
    // 패드 상태 하나를 반영한다: 버튼 변화(RouteGamepadButtons)와 왼쪽 스틱 navigation(임계값·반복
    // 간격 — 프레임 시각). index는 패드 자리다. 처리하면 true다.
    Result<bool> RouteGamepadState(uint32 user, uint32 index, const input::GamepadSnapshot& pad,
                                   const SharedPtr<Element>& scope = {}) noexcept;
    // 패드 상태 기억을 지운다(창 초점 잃음·입력 넘침).
    void ResetGamepads() noexcept;
    // 패드 누름을 취소로 끝낸다(끊김·UI 밖으로 넘김 — 뗌을 보내면 버튼이 눌린다). 화면 입력
    // 경로(ScreenInputRouter)가 UI 입력을 끌 때도 부른다.
    void CancelGamepadKeys(uint32 user) noexcept;
    // 보내지 않고 패드 상태만 기억한다(UI가 이 입력을 받지 않을 때 — 다음 누름·뗌 판정이 맞게).
    void SyncGamepadState(uint32 user, uint32 index, const input::GamepadSnapshot& pad) noexcept;
    Result<bool> RouteKey(uint32 userId, const input::InputEvent& event) noexcept;

    Result<bool> RouteKeyWithin(uint32 userId, const input::InputEvent& event,
                                const SharedPtr<Element>& scope) noexcept;

    // 트리 순서로 focus를 옮긴다. wrap이 false면 끝에서 이동하지 않는다. cause는 옮긴 원인이다:
    // 사용자의 Tab·방향키·게임패드 이동만 Navigation(외곽선·키보드 툴팁)이고, 코드가 옮기는
    // 경우(메뉴·화면의 첫 초점 등)는 기본 Direct다.
    Result<bool> MoveFocus(uint32 userId, bool forward, bool wrap = false,
                           FocusCause cause = FocusCause::Direct) noexcept;

    Result<bool> MoveFocusWithin(uint32 userId, const SharedPtr<Element>& root, bool forward,
                                 bool wrap, FocusCause cause = FocusCause::Direct) noexcept;

    // 방향으로 마주 보는 요소를 고른다. 같은 행·열에 놓인 대상을 먼저 고려한다.
    Result<bool> Navigate(uint32 userId, FocusDirection direction,
                          const SharedPtr<Element>& scope = {},
                          FocusCause cause = FocusCause::Direct) noexcept;

    // 초점 요소의 툴팁을 지연 없이 연다(키보드 사용자가 닫힌 툴팁을 다시 볼 때 — InputAdapter의
    // Ctrl+F1). 툴팁이 없거나 초점이 없으면 false다.
    bool ShowFocusToolTip(uint32 userId) noexcept;

    // 열린 팝업(메뉴·콤보)과 툴팁을 모두 닫는다(창 테두리를 눌렀을 때 — UE DismissAllMenus).
    void DismissPopups() noexcept;

    // 포인터 누름 관찰자다(UE FSlateApplication이 누름마다 메뉴 스택에서 누른 경로 밖 메뉴를 닫는
    // 것 — MenuStack.DismissFrom). 누름(Down)을 경로로 보내기 전에 맞은 요소(없으면 null)와 함께
    // 부르고, 관찰자가 true를 돌려주면 그 누름을 소비한다(경로로 보내지 않는다). 트리를 바꾼
    // 관찰자(메뉴 닫기)는 true를 돌려줘야 한다. 알림 중 붙이기는 Busy, 떼기는 알림이 끝난 뒤
    // 반영된다.
    //
    // 사용 예(메뉴 막대 — 열린 메뉴 밖을 누르면 닫고 그 누름을 삼킨다):
    //   auto id = app.AddPressObserver(Application::PressObserver::CreateLambda(
    //       [this](const PointerEvent&, Element* hit) noexcept { return CloseIfOutside(hit); }));
    //   ...
    //   app.RemovePressObserver(id.Value());
    using PressObserver = Delegate<bool(const PointerEvent&, Element*)>;
    Result<uint64> AddPressObserver(PressObserver&& observer) noexcept;
    void RemovePressObserver(uint64 id) noexcept;

    // 창 포커스 상실 등으로 포인터 입력이 중단되면 모든 capture를 해제한다.
    void CancelAllPointers() noexcept;

    void CancelPointers(PointerSource source) noexcept;

    // 드래그 중에는 캡처한 위젯에, 평소에는 포인터 아래 위젯에 커서를 묻는다.
    // 플랫폼 창의 제목 표시줄 히트 검사에 연결한다. 버튼 등 포커스 가능한 자식은 드래그 영역에서
    // 제외한다.
    input::WindowRegion QueryWindowRegion(Vec2 position) const noexcept;
    // 마지막 그리기의 영역 지도를 만든다(Docs/Design/UiToolkit 19.2절 — 창 backend가 OS 영역
    // 질의에서 UI 트리 대신 읽는다). 점마다 QueryWindowRegion과 같은 답을 낸다: hit 후보를 위에서
    // 보고, Client가 아닌 영역과 그 뒤 영역과 겹치는 Client 구멍을 앞에서부터 담는다. Popup 항목은
    // 팝업 층 아래 hit 판정되는 항목이고, 팝업이 있으면 popupOpen이다. 사각형은 창 좌표(= client
    // 픽셀)로 구멍·Popup은 바깥으로, 영역은 안으로 반올림한다. 넘치면 Caption과 구멍을 빼고 버튼·
    // 시스템 메뉴·Popup만 남긴다(끌기 대신 Client — 보수적). 그리기 전(hit grid 없음)이면 빈
    // 지도다.
    //
    // 사용 예(호스트, 그리기마다):
    //   input::WindowRegionMap map;
    //   app.BuildWindowRegionMap(map);
    //   if (!map.SameAs(published)) { window->PublishRegionMap(map); published = map; }
    void BuildWindowRegionMap(input::WindowRegionMap& map,
                              WindowRegionMapStats* stats = nullptr) const noexcept;
    input::MouseCursor QueryMouseCursor(Vec2 position, uint32 userId = 0) const noexcept;

    // 창 입력이 끊어지면 모든 사용자의 포인터 캡처와 키보드 포커스를 해제한다.
    void ResetInput() noexcept;
    // 재사용될 행의 포커스와 포인터 캡처를 이전 데이터가 아직 연결된 상태에서 해제한다.
    void ReleaseInputForSubtree(const Element& subtree) noexcept;

    // hit된 Element에서 루트 방향으로 전달한다. 경로 할당에 실패하면 이벤트를 전달하지 않는다.
    Result<bool> RoutePointer(const PointerEvent& event) noexcept;
    Result<bool> RoutePointerWithin(const PointerEvent& event,
                                    const SharedPtr<Element>& scope) noexcept;

private:
    // resolver revision이 바뀌었으면 테마 revision을 올려 캐시를 다시 만든다.
    void SyncVectorRevision() noexcept;
    struct Focus {
        uint32 userId;
        WeakPtr<Element> target;
        FocusCause cause;
        // 이 항목이 대상의 초점 외곽선을 켰는지다.
        bool outline;
    };

    struct Capture {
        PointerSource source;
        uint32 userId;
        uint32 pointerId;
        input::MouseButton button;
        WeakPtr<Element> target;
        Vec2 pressPosition;
        bool allowTouchPan;
        // 끄는 동안의 커서 잠금이다(23.5절): 잠금 자리, 서비스 없을 때 이동량을 잴 직전 위치, 풀 때
        // 커서를 둘 자리, 잠근 서비스(풀 때 같은 서비스로 — 실패했으면 비어 있다).
        bool cursorLocked = false;
        Vec2 lockPosition{};
        Vec2 lastPosition{};
        Optional<Vec2> releaseAt{};
        SharedPtr<CursorService> cursor{};
    };

    // 그 포인터의 capture가 target이면 커서를 잠근다(23.5절). 이미 잠갔으면 아무것도 하지 않는다.
    void LockCaptureCursor(const PointerEvent& event, const SharedPtr<Element>& target) noexcept;
    // capture가 풀릴 때 커서를 둘 자리를 기억한다(target의 capture일 때만).
    void StoreCursorRelease(const PointerEvent& event, const SharedPtr<Element>& target,
                            Vec2 at) noexcept;

    Result<bool> NavigateInternal(uint32 user, FocusDirection direction,
                                  const SharedPtr<Element>& scope, bool wrap,
                                  FocusCause cause) noexcept;
    // 원인과 경로의 OnQueryShowFocus로 외곽선을 정한다(UE SetUserFocus 3097–3110행).
    bool ResolveFocusOutline(const Element& target, FocusCause cause) const noexcept;
    bool IsFocusablePath(const Element* element) const noexcept;

    Result<void> BuildPath(Element* target, const SharedPtr<Element>& root,
                           Array<SharedPtr<Element>>& path) const noexcept;

    Result<void> CollectFocusTargets(const SharedPtr<Element>& element,
                                     Array<SharedPtr<Element>>& targets) const noexcept;

    void ClearAllFocus() noexcept;

    void ReleaseInactiveTargets(const Element* releasing = nullptr) noexcept;

    void ReleaseCapture(PointerSource source, uint32 userId, uint32 pointerId) noexcept;

    Result<void> SetCapture(const PointerEvent& event, const SharedPtr<Element>& target,
                            bool allowTouchPan) noexcept;

    friend class MenuAnchor;

    SharedPtr<Application*> context_;
    Allocator& allocator_;
    ThemeColors theme_{};
    // SetTheme마다 늘린다(지역 캐시가 같은 주소의 테마 값이 바뀐 것을 알게 한다).
    uint64 themeRevision_ = 0;
    VectorImageResolver* vectorImages_ = nullptr;
    // 마지막으로 본 resolver revision이다.
    uint64 vectorRevision_ = 0;
    CacheRoot cache_;
    HitGrid hitGrid_;
    SharedPtr<Element> root_;
    Array<WeakPtr<UiTimer>> timers_;
    // 영역 지도가 넘쳤다는 경고를 한 번만 남겼는지다.
    mutable bool regionMapOverflowLogged_ = false;
    bool runningTimers_ = false;
    void AdvanceTimers() noexcept;
    // 다음 툴팁 마감(열기 지연·떠남 지연)까지 남은 초다. 페이드 중이면 0이다.
    double SecondsUntilNextToolTip() const noexcept;
    struct Hover {
        uint32 user;
        Array<WeakPtr<Element>> path;
    };
    Result<void> UpdateHover(uint32 user, Element* hit, const SharedPtr<Element>& scope) noexcept;
    struct ToolTipState {
        uint32 user;
        WeakPtr<Element> owner;
        SharedPtr<Element> layer;
        Vec2 point;
        double due;
        double opened;
        double leaveDue;
        uint64 revision;
        bool attempted;
        // 키보드·게임패드 초점으로 연 툴팁이다(소유자 아래에 놓고, 포인터 이동으로 닫지 않는다).
        bool keyboard;
    };
    // 키보드·게임패드로 초점을 옮긴 요소에 툴팁이 있으면 예약하고, 초점이 떠난 키보드 툴팁은
    // 닫는다.
    void ObserveFocusToolTip(uint32 user, const SharedPtr<Element>& target,
                             FocusCause cause) noexcept;
    SharedPtr<Element> FindToolTipOwner(const SharedPtr<Element>& focus) const noexcept;
    void ObserveToolTip(const PointerEvent& event, Element* hit,
                        const SharedPtr<Element>& scope) noexcept;
    void AdvanceToolTips() noexcept;
    Result<void> AttachToolTip(const PopupRequest& popup) noexcept;
    WeakPtr<Element> toolTipHost_;
    // host 서비스 묶음이다(20.2절 — 처음 필요할 때 만든다).
    SharedPtr<UiHostServices> host_;
    UiHostServices* EnsureHostServices() noexcept;
    // 지금 루트에 묶음을 단다. 할당에 실패하면 팝업은 창 경계만 지키고 IME 글자는 Text로 온다.
    void ApplyHostServices() noexcept;
    // 시계 원천 값이다(교체 이어 붙이기 차이를 더하기 전).
    double ReadSourceClock() const noexcept;
    // UI 시각이다(원천 + 교체 차이 — 20.3절).
    double ReadClock() const noexcept;
    // 대기 계산 기준 시각이다(바깥 시각 프레임 뒤에는 그 프레임 시각).
    double WaitClock() const noexcept;
    // 암묵 시작 호스트에서 프레임이 열려 있지 않으면 연다(20.3절).
    void EnsureFrame() noexcept;
    void StartFrame(const FrameTime& time, bool external) noexcept;
    // 옛 루트가 이 묶음을 들고 있으면 뗀다.
    void DetachHostServices() noexcept;
    struct UserNavigation {
        uint32 user;
        NavigationConfig config;
    };
    // 패드 자리마다의 지난 버튼과 스틱 navigation 상태다.
    struct PadState {
        uint32 buttons = 0;
        Optional<FocusDirection> analog{};
        double analogLast = 0;
        uint32 analogRepeats = 0;
        // 스틱을 민 채 이벤트가 오지 않아도 프레임마다 반복한다(UE는 analog를 매 tick 보낸다).
        float analogPressure = 0;
        uint32 user = 0;
        WeakPtr<Element> scope;
        bool scoped = false;
    };
    // 스틱 반복을 진행한다(PrepareLayout — 20.8절).
    void AdvanceAnalogRepeats() noexcept;
    Result<bool> RouteAnalogNavigation(uint32 index) noexcept;
    PadState pads_[input::MaxGamepads]{};
    // 키 경로 본체다(키보드·패드 공용 — 초점 경로 preview·명령·OnKeyEvent·navigation 응답).
    Result<bool> RouteKeyEvent(const KeyEvent& key, const SharedPtr<Element>& scope) noexcept;
    // KeyDown의 navigation 응답을 실행한다(다음·이전은 tree 순서, 방향은 기하 — 20.7절).
    Result<bool> PerformNavigation(uint32 userId, FocusDirection direction,
                                   const SharedPtr<Element>& scope) noexcept;
    // 스타일이 바뀌었으면 테마 revision을 올리고 트리 전체 측정을 다시 하게 한다.
    void SyncStyleRevision() noexcept;
    SharedPtr<StyleRegistry> styles_;
    uint64 styleRevision_ = 0;
    ClockFunction clock_;
    FrameTime frameTime_{};
    double lastClock_ = 0;
    double clockOffset_ = 0;
    uint64 frameSerial_ = 0;
    uint64 timerFrame_ = 0;
    bool clockStarted_ = false;
    bool frameBegun_ = false;
    bool explicitFrames_ = false;
    bool externalTime_ = false;
    usize FindToolTip(uint32 user) const noexcept;
    double ToolTipTime() const noexcept;
    Array<ToolTipState> toolTips_;
    double TimerTime() const noexcept;
    double toolTipDelay_ = 0.15;
    double toolTipFade_ = 0.1;
    Array<Hover> hover_;
    uint64 hoverVersion_ = 0;
    Array<Capture> captures_;
    Array<Focus> focus_;
    struct UserModifiers {
        uint32 userId;
        uint8 mask;
    };
    struct CommandScope {
        WeakPtr<Element> element;
        SharedPtr<CommandList> list;
    };
    Array<UserModifiers> modifiers_;
    Array<CommandScope> commandScopes_;
    struct PressObserverEntry {
        uint64 id;
        PressObserver observer;
        bool removed;
    };
    // 누름이 경로로 가기 전에 관찰자에게 알린다. 하나라도 소비하면 true다.
    bool NotifyPressObservers(const PointerEvent& event, Element* hit) noexcept;
    Array<PressObserverEntry> pressObservers_;
    uint64 nextPressObserver_ = 1;
    bool notifyingPress_ = false;
    SharedPtr<CommandList> globalCommands_;
    Array<SharedPtr<CommandList>> windowCommands_;
    Array<UserNavigation> navigationConfigs_;
    String appStyleName_;
    Result<KeyChord> UpdateChord(uint32 userId, const input::InputEvent& event) noexcept;
    bool ProcessCommands(const Element* scope, KeyChord chord, bool repeat) const noexcept;
    uint64 focusVersion_ = 0;
    bool cancelling_ = false;
    bool focusChanging_ = false;
};

} // namespace rocks::ui
