#pragma once

#include <Core/Containers/SlotArray.h>
#include <Core/Delegate.h>
#include <Core/Text/String.h>
#include <UiCore/Element.h>

namespace rocks::ui {

// 도킹 트리 깊이 상한이다(저장 배치 읽기·분할이 이보다 깊으면 거부한다).
inline constexpr uint32 MaxDockTreeDepth = 64;

enum class DockSide : uint8 {
    Center,
    Left,
    Right,
    Top,
    Bottom
};
enum class DockRole : uint8 {
    Major,
    Panel,
    Global,
    Document
};

struct DockTab {
    StringView id;
    SharedPtr<Element> header;
    SharedPtr<Element> content;
    DockRole role = DockRole::Panel;
    bool closable = true;
};

struct DockTabViews {
    SharedPtr<Element> header;
    SharedPtr<Element> content;
};

using DockTabSpawner = Delegate<Result<DockTabViews>(Allocator&)>;
class DockManager;
// 포인터 아래 최상위 도킹 영역과 그 창의 클라이언트 좌표를 구한다.
using DockWindowHitTest = Delegate<SharedPtr<DockManager>(Vec2, Vec2&)>;

struct DockTabInfo {
    StringView id;
    StringView displayName;
    StringView group;
    DockRole role;
    bool listed;
    bool open;
};

struct FloatingTabPreview {
    Vec2 origin; // 원래 창 기준으로 잡은 지점을 보정한 미리보기 좌상단이다.
    Vec2 size;
    uint64 gesture;
};

// 에디터 창 하나의 도킹 영역이다. 묶음의 모든 탭 머리와 선택한 탭의 내용만 트리에 노출한다.
// 등록 ID는 레이아웃을 저장하고 다시 여는 데 쓰므로 지역화된 표시 문자열과 분리한다.
class ROCKS_UIFRAMEWORK_API DockManager final : public Element {
public:
    explicit DockManager(Allocator& allocator) noexcept;
    ~DockManager() noexcept override;

    Result<void> RegisterTab(const DockTab& tab) noexcept;
    Result<void> RegisterSpawner(StringView id, StringView displayName, StringView group,
                                 DockRole role, bool listed, bool closable,
                                 DockTabSpawner&& spawn) noexcept;
    usize RegisteredCount() const noexcept;
    Result<DockTabInfo> TabInfo(usize index) const noexcept;
    Result<void> OpenTab(StringView id) noexcept;
    Result<void> ActivateTab(StringView id) noexcept;
    Result<void> CloseTab(StringView id) noexcept;
    // 창 자체를 닫을 때는 개별 탭의 닫기 버튼 유무와 관계없이 모두 닫는다.
    void CloseWindowTabs() noexcept;
    // 분할 도킹 시 새 탭이 차지할 초기 비율이다. 기존 배치를 복원하면 저장된 비율을 쓴다.
    Result<void> Dock(StringView id, StringView relativeTo, DockSide side,
                      float share = 0.5f) noexcept;
    // 같은 할당자를 쓰는 창 사이에서 등록·내용·생성기를 함께 옮긴다. 닫힌 탭은 닫힌 채로 옮긴다.
    Result<void> TransferTabTo(StringView id, DockManager& destination, StringView relativeTo = {},
                               DockSide side = DockSide::Center) noexcept;
    Result<void> AcceptDrop(DockManager& source, StringView id, Vec2 windowPoint) noexcept;
    // 영역 밖에 놓으면 호스트가 창을 찾거나 만들 수 있도록 탭 ID와 창 좌표를 전달한다.
    void SetOnExternalDrop(Delegate<void(StringView, Vec2)>&& callback) noexcept;
    void SetWindowHitTest(DockWindowHitTest&& callback) noexcept;
    bool GetFloatingPreview(FloatingTabPreview& preview) const noexcept;
    Result<void> PaintFloatingPreview(PaintList& list) const noexcept;
    bool IsOpen(StringView id) const noexcept;
    bool IsActive(StringView id) const noexcept;
    // 열린 탭의 머리 요소(탭 여백·brush를 맡는 감싼 요소 — 그 첫 자식이 등록한 머리)다. 없거나 닫혀
    // 있으면 nullptr다. 탭 위치를 찾거나(시험·튜토리얼 강조) 머리 둘레에 표시를 그릴 때 쓴다.
    const Element* TabHeader(StringView id) const noexcept;

    // 이 창의 도킹 트리를 들여쓰기한 UTF-8 JSON으로 쓴다. 탭은 등록 ID로 식별하며
    // 묶음 안의 순서와 활성 탭, 분할 방향과 첫 영역의 비율을 보존한다. 빈 창은 root가 null이다.
    // version은 에디터가 정한 배치 버전이다. 저장에 성공한 경우에만 output을 교체한다.
    Result<void> SaveLayout(uint32 version, Array<uint8>& output) const noexcept;
    // 탭 또는 생성기를 먼저 등록한 뒤 호출한다. 실패하면 기존 배치를 유지한다: 문서가 손상됐으면
    // CorruptData, 형식·배치 버전이 다르면 Unsupported, 모르는 탭 ID는 NotFound다. 문서에 없는 등록
    // 탭은 성공 시 닫힌다.
    Result<void> RestoreLayout(uint32 version, Span<const uint8> bytes) noexcept;

    // 이전 바이너리 .layout 파일을 한 번 가져올 때 쓴다. 다음 SaveLayout은 JSON을 생성한다. 오류는
    // RestoreLayout과 같다.
    Result<void> RestoreLegacyLayout(uint32 version, Span<const uint8> bytes) noexcept;

    Result<void> TryAddChild(const SharedPtr<Element>& child) noexcept override;
    bool RemoveChild(Element& child) noexcept override;
    Span<const SharedPtr<Element>> ActiveChildren() const noexcept override;
    Vec2 ComputeDesiredSize(float scale) const noexcept override;
    Result<void> Arrange(const Geometry& geometry) noexcept override;
    Result<void> OnPaint(PaintList& list, int32 layer) const noexcept override;
    Result<void> OnPaintOverlay(PaintList& list, int32 layer) const noexcept override;
    PointerReply OnPreviewPointerDown(const PointerEvent& event) noexcept override;
    input::MouseCursor QueryMouseCursor(Vec2 position) const noexcept override;
    PointerReply OnPointerEvent(const PointerEvent& event) noexcept override;
    void OnPointerCaptureLost(PointerSource source, uint32 user, uint32 pointer) noexcept override;

private:
    enum class Kind : uint8 {
        Stack,
        Horizontal,
        Vertical
    };
    struct Node {
        explicit Node(Allocator& allocator) noexcept;
        Array<usize> tabs;
        SlotHandle parent{}, first{}, second{};
        Kind kind = Kind::Stack;
        usize selected = 0;
        float fraction = 0.5f;
        Box2<float> bounds{};
        Box2<float> handle{};
    };
    struct Tab {
        explicit Tab(Allocator& allocator) noexcept;
        String id;
        String displayName, group;
        DockTabSpawner spawn;
        bool listed = true;
        bool lazy = false;
        SharedPtr<Element> header, content;
        SlotHandle stack{};
        DockRole role = DockRole::Panel;
        bool closable = true;
        Box2<float> headerBounds{};
    };

    Result<void> ApplyLayout(SlotArray<Node>&& restored, const Array<SlotHandle>& locations,
                             SlotHandle root) noexcept;
    usize FindTab(StringView id) const noexcept;
    Result<void> AttachViews(Tab& tab, const DockTabViews& views) noexcept;
    Result<void> EnsureViews(usize index) noexcept;
    void DropViews(usize index) noexcept;
    void RebuildChildren() noexcept;
    void DetachTab(usize index) noexcept;
    Result<void> Place(usize index, SlotHandle target, DockSide side, usize insertion,
                       float share = 0.5f) noexcept;
    Result<void> Transfer(usize index, DockManager& destination, SlotHandle target, DockSide side,
                          usize insertion) noexcept;
    void ReplaceNode(SlotHandle oldNode, SlotHandle replacement) noexcept;
    void CollapseEmpty(SlotHandle stack) noexcept;
    Result<void> ArrangeNode(SlotHandle node, Vec2 origin, Vec2 size) noexcept;
    Vec2 MeasureNode(SlotHandle node) const noexcept;
    // 탭 줄 높이다. 모든 탭 머리(DockTabHeader — 탭 여백 포함)의 원하는 높이 중 가장 큰 값을 탭
    // 최대 높이로 자른다(UE SDockingTabWell::ComputeChildSize).
    float TabHeight() const noexcept;
    // 탭 사이 간격이다.
    float TabGap() const noexcept;
    usize SubtreeDepth(SlotHandle node) const noexcept;
    bool LocalPoint(Vec2 window, Vec2& local) const noexcept;
    SlotHandle FindSplitter(Vec2 local) const noexcept;
    void UpdateTarget(Vec2 local) noexcept;
    Vec2 TrackTarget(Vec2 point, Vec2 local) noexcept;
    void ClearTarget() noexcept;
    void ReleasePreview() noexcept;
    void EndDrag() noexcept;

    Allocator& allocator_;
    Array<Tab> tabs_;
    SlotArray<Node> nodes_;
    Array<SharedPtr<Element>> visible_;
    SlotHandle root_{}, dragSplit_{}, target_{};
    usize dragTab_ = MaxSize;
    usize insertion_ = MaxSize;
    DockSide side_ = DockSide::Center;
    Vec2 press_{};
    Vec2 dragPosition_{};
    uint64 gesture_ = 0;
    float handleOffset_ = 0;
    PointerSource source_ = PointerSource::Mouse;
    uint32 user_ = 0, pointer_ = 0;
    bool dragging_ = false;
    bool spawning_ = false;
    Delegate<void(StringView, Vec2)> externalDrop_;
    DockWindowHitTest windowHitTest_;
    SharedPtr<DockManager> previewWindow_;
    DockManager* previewOwner_ = nullptr;
    bool overDesktop_ = false;
    bool showOuterGuides_ = false;
};

} // namespace rocks::ui
