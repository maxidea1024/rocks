#pragma once

#include <Core/Containers/Array.h>
#include <Core/Memory/SharedPtr.h>
#include <Input/InputEvent.h>
#include <UiCore/Geometry.h>
#include <UiCore/KeyChord.h>
#include <UiCore/KeyEvent.h>
#include <UiCore/KeyReply.h>
#include <UiCore/NavigationRule.h>
#include <Core/Text/String.h>
#include <Core/Optional.h>
#include <UiCore/ColorRef.h>
#include <UiCore/FocusCause.h>
#include <UiCore/PaintList.h>
#include <UiCore/SlotPlacement.h>
#include <UiCore/Styles/StyleBrush.h>
#include <UiCore/UiHostServices.h>

namespace rocks::ui {

class Element;

struct PopupRequest {
    SharedPtr<Element> layer;
    SharedPtr<Element> anchor;
    void (*close)(Element&) noexcept = nullptr;
};

// 요소가 커서를 정하지 않았다는 표지다(cursorOverride_ — 부모나 기본 커서를 쓴다).
inline constexpr uint8 NoCursorOverride = 255;

// 중립 접근성 역할이다. 플랫폼 어댑터는 제어 이름과 역할을 별개로 전달한다.
enum class AccessibleRole : uint8 {
    Group,
    Text,
    Button,
    CheckBox,
    Slider,
    ScrollBar,
    SpinBox,
    Image,
    Progress,
    List
};
enum class AccessibleBehavior : uint8 {
    Auto,
    Custom,
    Summary,
    ToolTip,
    Excluded
};
class ElementSemantics;
class WidgetSurface;
class CacheRoot;
class AttributeBinding;

enum class Visibility : uint8 {
    Visible,
    Collapsed,
    Hidden,
    HitTestInvisible,
    SelfHitTestInvisible
};

enum class PointerEventType : uint8 {
    Down,
    Up,
    Move,
    Wheel,
    Cancel
};

enum class PointerSource : uint8 {
    Mouse,
    Touch
};

struct PointerEvent {
    PointerEventType type;
    Vec2 position;
    uint32 pointerId = 0;
    uint32 userId = 0;
    PointerSource source = PointerSource::Mouse;
    input::MouseButton button = input::MouseButton::Left;
    float wheel = 0;
    // 누름의 연속 횟수다(두 번 누르기면 2). Down에만 뜻이 있다.
    uint8 clickCount = 1;
};

// 이벤트 소비와 포인터 capture 변경 요청이다.
struct PointerReply {
    bool handled = false;
    bool capture = false;
    bool releaseCapture = false;
    // 터치 탭을 감시하는 캡처다. 실제 팬이 시작되면 조상 스크롤 영역이 넘겨받을 수 있다.
    bool allowTouchPan = false;
};

// Game lane에서만 사용하는 트리다. 부모가 자식을 소유하며 부모 포인터는 소유하지 않는다.
// Geometry는 배치 호출자가 창 좌표 기준으로 설정한다.
class ROCKS_UICORE_API Element {
public:
    explicit Element(Allocator& allocator) noexcept;

    Element(const Element&) = delete;
    Element& operator=(const Element&) = delete;

    virtual ~Element() noexcept;

    // 자식을 소유한다. 이미 부모가 있거나 순환을 만드는 자식은 거부한다.
    virtual Result<void> TryAddChild(const SharedPtr<Element>& child) noexcept;

    // 화면의 팝업 호스트를 찾아 전면 레이어를 붙인다. 호스트가 없으면 NotFound를 반환한다.
    virtual Result<void> AttachPopupLayer(const PopupRequest& popup) noexcept;
    // 화면이 비활성화될 때 이 호스트에서 열린 메뉴를 닫는다.
    virtual void DismissHostedPopups() noexcept;
    // child가 이 요소가 품은 팝업 층인지다(팝업 호스트 Overlay가 답한다). 창 영역 판정이 팝업 위
    // 지점을 크기 조절 가장자리보다 앞세울 때 쓴다(input::WindowRegion::Popup).
    virtual bool IsHostedPopupLayer(const Element& child) const noexcept;
    // 팝업 층이 허용 영역(모니터 작업 영역)을 고를 기준 창 좌표 지점이다. 비우면 호스트가 앵커의
    // 왼쪽 위를 쓴다(UE CalculatePopupWindowPosition — 앵커 왼쪽 위 1 px로 가장 가까운 모니터).
    // 커서에 붙는 툴팁 층은 커서 지점을 준다(UE CalculateTooltipWindowPosition). 여러 모니터에
    // 걸친 창에서 팝업이 어느 모니터 안에 놓일지를 정한다.
    virtual Optional<Vec2> PopupWorkAreaPoint() const noexcept;
    // 팝업 호스트가 이 팝업 층에 배치 경계(층 로컬 사각형 — 화면 안 허용 영역)를 알린다. 층 자신은
    // 호스트 전체를 덮어 바깥 누름을 놓치지 않고, 내용만 이 경계 안에 놓는다(뒤집기·밀어 넣기·맞춤
    // 높이). 비우면 층 전체다. 기본은 아무것도 하지 않는다(팝업 층이 아닌 요소).
    virtual void SetPopupArea(const Optional<Box2<float>>& area) noexcept;

    // 자식을 분리한다. 해당 자식이 없으면 false를 돌려준다.
    virtual bool RemoveChild(Element& child) noexcept;

    // 동일 애셋의 두 인스턴스는 소유 트리 식별자로 구분한다.
    void SetNavigationIdentity(Guid domain, Guid identity) noexcept;
    Guid NavigationIdentity() const noexcept;
    Guid NavigationDomain() const noexcept;
    Result<void> SetNavigationRule(FocusDirection direction, NavigationBoundary boundary) noexcept;
    NavigationBoundary GetNavigationRule(FocusDirection direction) const noexcept;
    Result<void> SetNavigationHandler(NavigationHandler&& handler) noexcept;
    Guid ResolveCustomNavigation(uint32 user, FocusDirection direction) noexcept;

    // 화면 판독기에 제공할 이름·요약의 원문을 복사한다. 기본 텍스트는 요소가 직접 제공할 수 있다.
    Result<void> SetAccessibility(StringView name, StringView summary, AccessibleBehavior behavior,
                                  AccessibleBehavior summaryBehavior, bool allowChildren) noexcept;
    Result<void> GetAccessibleText(String& output, bool summary = false) const noexcept;
    bool IsAccessible() const noexcept;
    bool AllowsAccessibleChildren() const noexcept;
    virtual AccessibleRole GetAccessibleRole() const noexcept;
    virtual StringView DefaultAccessibleText() const noexcept;

    // 창 호스트 서비스 묶음이다(Docs/Design/UiToolkit 20.2절 — 팝업 허용 영역·텍스트 입력·프레임
    // 시각). Application이 루트에 단다(빈 포인터면 뗀다). Element는 아래 Find 함수로 루트까지
    // 올라가 찾는다.
    //
    // 사용 예:
    //   if (auto service = FindTextInputService()) { service->Activate(*this); }
    //   const double now = FindFrameTime().current;
    using PopupWorkArea = UiHostServices::PopupWorkArea;
    Result<void> SetUiHostServices(const SharedPtr<UiHostServices>& services) noexcept;
    // 이 요소에 직접 단 묶음이다(조상 것은 보지 않는다). 없으면 nullptr이다.
    const UiHostServices* GetUiHostServices() const noexcept;
    // 자신부터 조상으로 올라가 가장 가까운 묶음이다. 없으면 nullptr이다(트리에 붙기 전·헤드리스).
    // 돌려준 주소는 그 묶음을 단 Element가 살아 있는 동안 유효하다.
    const UiHostServices* FindUiHostServices() const noexcept;
    // 팝업 허용 영역을 묻는다(9.1절 UW7d). 묶음이나 공급자가 없거나 모르면 false다.
    bool QueryPopupWorkArea(Vec2 window, Box2<float>& area) const noexcept;
    // 창의 텍스트 입력 서비스다(19.2절). 없으면 빈 포인터다.
    SharedPtr<TextInputService> FindTextInputService() const noexcept;
    // 그 프레임의 UI 시각이다(20.3절). 묶음이 없으면 0이다.
    FrameTime FindFrameTime() const noexcept;

    using ToolTipFactory = Delegate<Result<SharedPtr<Element>>()>;
    Result<void> SetToolTipFactory(ToolTipFactory&& factory, StringView description = {},
                                   bool interactive = false) noexcept;
    bool HasToolTip() const noexcept;
    bool IsToolTipInteractive() const noexcept;
    Result<SharedPtr<Element>> MakeToolTip() noexcept;
    uint64 ToolTipRevision() const noexcept;
    Element* Parent() const noexcept;
    // 자신을 포함해 지정한 루트 아래에 붙어 있는지 검사한다.
    bool IsWithin(const Element& root) const noexcept;
    // root까지의 경로가 입력을 받을 수 있는지다(보이기·scope·활성). requireEnabled가 false면 활성은
    // 보지 않는다(hit 판정 — 비활성은 경로 자름으로 다룬다, Docs/Design/UiToolkit 20.5절).
    bool IsInputPathActive(const Element& root, bool pointer,
                           bool requireEnabled = true) const noexcept;

    usize ChildCount() const noexcept;

    // 측정·그리기·입력에 참여하는 자식이다. 트리를 변경하면 뷰가 무효화된다.
    virtual Span<const SharedPtr<Element>> ActiveChildren() const noexcept;

    Element* ChildAt(usize index) const noexcept;

    // 범위 밖이면 빈 참조다. 반환한 참조는 트리에서 제거된 뒤에도 자식을 유지한다.
    SharedPtr<Element> ChildRefAt(usize index) const noexcept;

    // 직접 소유한 자식이면 공유 참조를 돌려준다.
    SharedPtr<Element> FindChild(const Element* child) const noexcept;

    // 누름 이벤트를 루트에서 대상 방향으로 전달할 때 먼저 호출한다.
    // 자식이 누름을 캡처한 경우에도 해제·취소를 부모에서 관찰할 수 있다. 이벤트를 소비하지 않는다.
    // 시작점에서 움직임이 충분하면 해당 스크롤 영역이 캡처를 요청한다. 안쪽 영역부터 질의한다.
    virtual PointerReply OnTouchPan(const PointerEvent& move, Vec2 pressPosition) noexcept;
    virtual void OnPointerReleaseInPath(const PointerEvent& event) noexcept;
    virtual PointerReply OnPreviewPointerDown(const PointerEvent&) noexcept;
    // 사용자 정의 제목 표시줄은 Caption을 반환한다. 포커스를 받는 자식은 자체 입력을 유지한다.
    virtual input::WindowRegion GetWindowRegion() const noexcept;
    void SetCursorOverride(input::MouseCursor cursor) noexcept;
    void ResetCursorOverride() noexcept;
    input::MouseCursor ResolveMouseCursor(Vec2 position) const noexcept;
    virtual void OnMouseEnter(uint32 user) noexcept;
    virtual void OnMouseLeave(uint32 user) noexcept;
    virtual input::MouseCursor QueryMouseCursor(Vec2 position) const noexcept;

    // 기본 구현은 이벤트를 소비하지 않는다.
    virtual PointerReply OnPointerEvent(const PointerEvent&) noexcept;

    // capture가 해제되거나 루트에서 분리된 뒤 다음 입력이 들어오면 호출한다.
    virtual void OnPointerCaptureLost(PointerSource, uint32, uint32) noexcept;

    // 키보드 focus를 받을 수 있는 Element만 재정의한다.
    virtual bool SupportsKeyboardFocus() const noexcept;

    // 키 누름은 미리보기에서 루트부터, 본 이벤트에서 focus 대상부터 전달된다.
    // 보조 키가 필요한 입력 컨트롤은 이 경로에서 키를 선점한다. 기본 구현은 기존 미리보기로
    // 전달한다.
    virtual bool OnPreviewKeyChord(uint32 user, const input::InputEvent& event,
                                   KeyChord chord) noexcept;
    // 게임패드 스냅숏에서 새로 눌린 버튼을 포커스 위젯에 먼저 보낸다.
    virtual bool OnPreviewKeyDown(uint32, const input::InputEvent&) noexcept;

    // 키 이벤트다(Docs/Design/UiToolkit 20.7절 — UE SWidget::OnKeyDown). 기본 처리는 초점 받을 수
    // 있는 요소의 KeyDown이 navigation 방향을 가지면 navigation 응답이다. 키를 쓰는 override는
    // KeyReply::Handled를, 아니면 Super::OnKeyEvent(key)를 돌려준다.
    virtual KeyReply OnKeyEvent(const KeyEvent& key) noexcept;
    // 초점 경로를 초점 요소에서 루트로 올라가며 navigation 방향마다 부른다(UE OnNavigation).
    // Escape가 아니면 거기서 멈추고 그 규칙으로 탐색한다. 기본은 SetNavigationRule로 정한 규칙이다.
    // 방향을 스스로 처리한 요소(메뉴·목록)는 NavigationBoundary::Handled()를 돌려준다.
    virtual NavigationBoundary OnNavigation(uint32 user, FocusDirection direction) noexcept;
    // 기본 키 대응(NavigationConfig::Default)으로 KeyEvent를 만들어 이 요소의 OnKeyEvent에 보낸다
    // (시험·위젯 전달용 — 라우팅을 거치지 않는다).
    KeyReply SendKey(uint32 user, const input::InputEvent& event,
                     const KeyChord& chord = {}) noexcept;
    // 패드 버튼 누름(pressed) 또는 뗌을 기본 대응으로 KeyEvent(chord.gamepad)로 만들어 보낸다.
    KeyReply SendGamepadButton(uint32 user, input::GamepadButton button,
                               bool pressed = true) noexcept;

    // 장치 연결이 끊기면 누른 키를 활성화 없이 해제한다.
    virtual void OnKeyCancelled(uint32 userId, input::Key key) noexcept;

    virtual void OnFocusReceived(uint32) noexcept;

    virtual void OnFocusLost(uint32) noexcept;

    // 키보드 초점 외곽선을 켜고 끈다(Docs/Design/UiToolkit 9.1절 UW4f1). Application이 키보드·
    // 게임패드 이동으로 옮긴 초점에만 켠다. 여러 사용자가 같은 요소에 켤 수 있어 켠 수를 센다.
    // 켜져 있으면 자식과 OnPaintOverlay 뒤에 FocusOutlineBrush()를 요소 크기로 그린다.
    void SetFocusOutlineVisible(bool visible) noexcept;
    bool IsFocusOutlineVisible() const noexcept;

    // 이 요소가 초점 잎이거나 그 조상일 때 외곽선을 보일지 정한다(UE SWidget::OnQueryShowFocus).
    // Application은 초점 잎부터 루트 방향으로 물어 처음 값이 있는 답을 쓰고, 모두 없으면 원인이
    // Navigation일 때만 보인다. 예: 키보드 이동에도 외곽선이 어울리지 않는 그리기 영역은 false,
    // 코드가 옮긴 초점에도 늘 보여야 하는 접근성 모드 화면은 true.
    virtual Optional<bool> OnQueryShowFocus(FocusCause cause) const noexcept;

    // 초점 외곽선 brush다. 기본은 FocusOutlineStyle()이다. 테두리 색으로 초점을 보이는 부품(편집
    // 텍스트)이나 외곽선이 어울리지 않는 부품(뷰포트)은 nullptr을 돌려 그리지 않는다.
    virtual const StyleBrush* FocusOutlineBrush() const noexcept;

    // 자기 자신을 그리기 목록에 기록한다. 기본 Element는 그리지 않는다.
    virtual Result<void> OnPaint(PaintList&, int32) const noexcept;

    // 자식을 그린 뒤 자기 전경을 기록한다.
    virtual Result<void> OnPaintOverlay(PaintList&, int32) const noexcept;

    // 부모를 먼저 기록하고 자식을 순서대로 기록한다. 실패 시 이미 기록된 명령은 유지한다.
    Result<void> Paint(PaintList& list, int32 layer) const noexcept;

    // 활성 자식이나 배치가 바뀔 때만 렌더 호스트가 표면 소유자 목록을 다시 찾는다.
    uint64 PaintPlanRevision() const noexcept;
    virtual WidgetSurface* GetWidgetSurface() noexcept;

    void SetGeometry(const Geometry& geometry) noexcept;

    // 피벗은 로컬 크기에 대한 비율이다. 렌더 변환은 원하는 크기를 바꾸지 않는다.
    Result<void> SetRenderTransform(const AffineTransform2D& transform, Vec2 pivot) noexcept;
    void ClearRenderTransform() noexcept;

    // 부모가 할당한 Geometry를 적용한다. 패널은 자식 배치도 수행한다.
    virtual Result<void> Arrange(const Geometry& geometry) noexcept;
    Result<void> ArrangeIfNeeded(const Geometry& geometry) noexcept;

    // 자식부터 원하는 크기를 계산해 캐시한다. scale은 DPI 등을 반영한 양수 배율이다.
    Result<void> Prepass(float scale) noexcept;

    // 원하는 크기에 영향을 주는 파생 Element의 속성 setter에서도 호출해야 한다.
    // 부모는 자식의 원하는 크기가 실제로 달라졌을 때 다시 측정한다.
    void InvalidateLayout() noexcept;
    // 원하는 크기는 유지하고 자식의 위치·크기만 다시 배치한다.
    void InvalidateArrangement() noexcept;
    void InvalidatePaint() noexcept;
    // 이 요소와 표시 중인 자손은 캐시 안에서도 매 프레임 그린다.
    void SetVolatile(bool enabled) noexcept;
    Result<void> SetRenderOpacity(float opacity) noexcept;
    float GetRenderOpacity() const noexcept;

    // 자식에게 물려줄 전경색이다(UE SWidget::GetForegroundColor·FWidgetStyle, UiToolkit 9.1절
    // UW3c). 기본은 부모 전경색 상속(ColorSource::Foreground)이다. 텍스트·아이콘은 그릴 때
    // PaintList::ForegroundColor()로 상속 색을 푼다. 파생 클래스는 상태별 색(버튼 올림 등)을 돌려줄
    // 수 있고, 바꿀 때 InvalidatePaint를 부른다. 잘못된 ColorRef는 InvalidArgument다.
    Result<void> SetForegroundColor(ColorRef color) noexcept;
    virtual ColorRef GetForegroundColor() const noexcept;

protected:
    // 그릴 때 자식에게 물려줄 전경색이다. disabled는 이 요소나 조상이 비활성인지다(UE는 비활성이면
    // DisabledForegroundColor). 기본은 GetForegroundColor()다.
    virtual ColorRef PaintForegroundColor(bool disabled) const noexcept;

public:
    bool IsVolatile() const noexcept;
    // 게임 위젯이 지정한 상시 갱신 요청이다. 네이티브 애니메이션의 자체 갱신 상태와 독립적이다.
    void SetForceVolatile(bool enabled) noexcept;

    // 표시 중인 트리에서 바인딩된 속성만 레이아웃 계산 전에 갱신한다.
    void UpdateBoundAttributes() noexcept;

    Vec2 GetDesiredSize() const noexcept;

    virtual Vec2 ComputeDesiredSize(float) const noexcept;

    // 폭 width 안에 놓을 때의 원하는 크기다(줄바꿈 글자가 좁은 칸에서 높아지는 것을 배치 전에 안다
    // — UE는 다음 프레임에 바로잡는다). 기본은 GetDesiredSize()다. 글자·틀·세로 쌓기가 재정의한다.
    // 배치 직전에만 부르고, 글자는 그 폭으로 줄을 미리 나눠 둔다.
    virtual Vec2 MeasureForWidth(float width) noexcept;

    const Geometry& GetGeometry() const noexcept;

    // 보이기 값을 준다. 묶음(BindVisibility)이 있으면 푼다.
    void SetVisibility(Visibility visibility) noexcept;
    // 보이기를 묶는다(Docs/Design/UiToolkit 20.6절 — UE Visibility 속성, 사유 Layout). getter는
    // 속성 갱신 단계(UpdateBoundAttributes)에서 이 요소의 다른 묶음보다 먼저, Collapsed여도
    // 평가된다 — 접힌 요소가 묶음으로 다시 보일 수 있다. getter는 UI 트리를 바꾸면 안 된다. 빈
    // delegate면 푼다.
    //
    // 사용 예:
    //   (void)panel->BindVisibility(Delegate<Visibility()>::CreateLambda(
    //       [&model]() noexcept { return model.open ? Visibility::Visible : Visibility::Collapsed;
    //       }));
    // 이 요소의 묶음을 평가하는 중(getter 안)에 부르면 옛 묶음은 그 갱신이 끝난 뒤 해제된다.
    Result<void> BindVisibility(Delegate<Visibility()>&& getter) noexcept;

    Visibility GetVisibility() const noexcept;

    // 부모 slot 안에서의 여백·정렬이다(Docs/Design/UiToolkit 20.10절 — UE FSimpleSlot·패널 slot).
    // 단일 자식 호스트(Frame·SizeBox·ScaleLayer·Switcher·Overlay·StackPanel)가 이 요소를
    // 측정·배치할 때 쓴다(SlotDesiredSize·MeasureInSlot·ArrangeInSlot). 기본값이면 지운다. 바꾸면
    // 부모 배치를 다시 한다.
    Result<void> SetSlotPlacement(const SlotPlacement& placement) noexcept;
    // 기본값이면 nullptr이다.
    const SlotPlacement* GetSlotPlacement() const noexcept;
    // slot 여백을 더한 원하는 크기다.
    Vec2 SlotDesiredSize() const noexcept;
    // slot 여백을 뺀 폭으로 측정하고 여백을 더한다.
    Vec2 MeasureInSlot(float width) noexcept;
    // slot 여백·정렬을 적용해 area 안에 배치한다.
    Result<void> ArrangeInSlot(const Geometry& area) noexcept;

    void SetEnabled(bool enabled) noexcept;

    bool IsEnabled() const noexcept;

    // 자기 자신과 자식의 그리기·입력을 경계 안으로 제한한다.
    void SetClipToBounds(bool enabled) noexcept;

    bool ClipsToBounds() const noexcept;

    // 앞에 그려진 자식부터 검사하며, 경계 클리핑이 켜진 조상 밖의 점은 제외한다. 맞은 요소까지의
    // 경로를 이 요소에서 내려가며 첫 비활성 요소 앞에서 자른다(UE GetBubblePath — 20.5절).
    // ignoreEnabled면 자르지 않는다(툴팁·영역 지도).
    Element* HitTest(Vec2 position, bool ignoreEnabled = false) noexcept;

protected:
    // 기존 자식 위치에 빈 배치 노드를 삽입한다. 슬롯 순서와 포커스, 입력 캡처는 그대로 이어진다.
    static Result<void> WrapAttachedChild(Element& child,
                                          const SharedPtr<Element>& wrapper) noexcept;
    // 자식 목록 정리가 끝난 뒤 조상에게 분리된 서브트리를 알린다.
    virtual void OnDescendantDetached(const Element& subtree) noexcept;
    // 직접 그리는 자식의 범위다. 오프스크린 결과를 표시할 때 입력·측정 트리를 유지한다.
    virtual Span<const SharedPtr<Element>> PaintChildren() const noexcept;
    // hit 판정이 걷는 자식이다(Docs/Design/UiToolkit 20.4절 — 그리기 순서, 뒤가 위). 기본은
    // PaintChildren이다. 자식을 오프스크린 결과로 대신 그리는 동안에도 같은 자리에 보이는 요소
    // (RetainerBox)는 활성 자식을 돌려준다. 돌려주는 목록이 바뀌면 plan revision을 올려야 한다.
    virtual Span<const SharedPtr<Element>> HitTestChildren() const noexcept;
    // 보이기·clip만 보는 hit 판정이다(HitTest가 자르기 전에 쓴다).
    Element* HitTestVisible(Vec2 position) noexcept;
    void InvalidatePaintPlan() noexcept;

    virtual CacheRoot* LocalPaintCache() const noexcept;
    float MeasuredLayoutScale() const noexcept;

    // 연속해서 그릴 두 자식 사이에 새 레이어가 필요하면 true다.
    virtual bool StartsPaintLayer(usize previous, usize next) const noexcept;

    virtual float RelativeLayoutScale() const noexcept;

    // 패널이 자식의 그리기 순서를 정해 삽입한다.
    // 같은 부모 안에서 그리기 순서만 옮긴다. 기존 포커스·포인터 캡처·바인딩을 유지한다.
    void MoveChildWithin(usize from, usize to) noexcept;
    Result<void> TryInsertChild(usize index, const SharedPtr<Element>& child) noexcept;

private:
    Result<void> EnsureSemantics() noexcept;
    // 묶음 없이 보이기를 바꾼다(SetVisibility·보이기 묶음 평가가 쓴다).
    void ApplyVisibility(Visibility visibility) noexcept;
    void UnbindVisibility() noexcept;
    void EvaluateVisibilityBinding(uint64 pass) noexcept;
    // 갱신 중에 풀린 보이기 묶음을 해제한다(UpdateBoundAttributes 끝).
    void ReleaseRetiredVisibility() noexcept;
    bool updatingBindings_ = false;
    SharedPtr<ElementSemantics> semantics_;
    Guid navigationIdentity_{};
    Guid navigationDomain_{};
    friend class CacheRoot;
    friend class AttributeBinding;
    friend class VisibilityAttribute;
    friend class HitGrid;
    AttributeBinding* bindings_ = nullptr;
    usize subtreeBindings_ = 0;
    uint64 bindingPass_ = 0;

    // 부모 전경색 inherited 아래에서 이 요소가 자식에게 물려줄 전경색이다. disabled는 이 요소나
    // 조상이 비활성인지다.
    LinearColor ResolveForeground(const ThemeColors& theme, LinearColor inherited,
                                  bool disabled) const noexcept;
    Result<void> PaintSubtree(PaintList& list, int32 layer, int32& highest, CacheRoot* cache,
                              bool inheritedVolatile) const noexcept;
    Result<void> PaintContents(PaintList& list, int32 layer, int32& highest, CacheRoot* cache,
                               bool inheritedVolatile) const noexcept;
    // OnPaintOverlay와 그 위의 초점 외곽선이다(cache 기록도 이 경로를 탄다).
    Result<void> PaintOverlayPass(PaintList& list, int32 layer) const noexcept;

    Array<SharedPtr<Element>> children_;
    Element* parent_ = nullptr;
    Geometry geometry_{};
    AffineTransform2D renderTransform_{};
    Vec2 renderPivot_{};
    uint8 cursorOverride_ = NoCursorOverride;
    float renderOpacity_ = 1;
    ColorRef foregroundColor_{ColorSource::Foreground};
    bool hasRenderTransform_ = false;
    Vec2 desiredSize_{};
    Vec2 desiredSeenByParent_{};
    float measuredScale_ = 0;
    bool layoutDirty_ = true;
    bool measureChildren_ = true;
    bool arrangeDirty_ = true;
    bool volatile_ = false;
    bool forceVolatile_ = false;
    Geometry allotted_{};
    uint64 paintRevision_ = 0;
    uint64 subtreePaintRevision_ = 0;
    uint64 planRevision_ = 0;
    Visibility visibility_ = Visibility::Visible;
    bool enabled_ = true;
    bool clipToBounds_ = false;
    // 초점 외곽선을 켠 사용자 수다.
    uint8 focusOutlines_ = 0;
};

} // namespace rocks::ui
