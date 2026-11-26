#pragma once

#include <UiWidget/UserWidget.h>
#include <UiFramework/Application/Application.h>
#include <World/SceneComponent.h>

namespace rocks::ui {

// X축 앞쪽에서 보는 YZ 평면이다. UI의 오른쪽은 로컬 -Y, 아래쪽은 -Z이며 한 픽셀은 로컬 1cm다.
// 입력과 화면 투영은 호출자가 전달한 플레이어의 레이·뷰를 사용한다. GPU 장치를 소유하지 않는다.
ROCKS_CLASS()
class ROCKS_UIWORLD_API WorldWidgetComponent final : public SceneComponent {
    ROCKS_GENERATED_BODY()
public:
    explicit WorldWidgetComponent(Allocator& allocator) noexcept;
    Result<void> SetWidget(ObjectHandle widget) noexcept;
    ObjectHandle GetWidget() const noexcept;
    Result<void> SetDrawSize(Vec2 pixels) noexcept;
    Vec2 DrawSize() const noexcept;
    Result<void> SetPivot(Vec2 pivot) noexcept;
    Vec2 Pivot() const noexcept;
    void SetScreenSpace(bool enabled, uint32 playerId = 0) noexcept;
    bool IsScreenSpace() const noexcept;

    // 가상 창의 배치를 먼저 갱신한 뒤 표면 패스를 준비한다. 할당에 실패하면 다음 프레임에
    // 재시도한다.
    Result<void> PrepareWindow() noexcept;
    Application& WindowApplication() noexcept;
    SharedPtr<Element> WindowRoot() const noexcept;

    // 양방향 레이를 평면에 투사한다. 평행·후방이면 NotFound. 경계 밖 좌표는 capture에 쓰인다.
    Result<Vec2> RayToLocal(Vec3d origin, Vec3d direction) const noexcept;
    Result<bool> RoutePointerRay(Vec3d origin, Vec3d direction, PointerEvent event) noexcept;
    Result<bool> RouteVirtualKey(uint32 userId, const input::InputEvent& event) noexcept;

    // 지정된 로컬 플레이어의 canvas에만 부착한다. clip 공간 -w..w와 깊이 0..w 밖에선 숨긴다.
    Result<bool> ProjectToPlayer(uint32 playerId, const Mat4d& viewProjection, Vec2 viewportSize,
                                 const SharedPtr<AnchorCanvas>& layer) noexcept;
    // 좌상·우상·좌하 순서다. 역변환 hit와 같은 피벗·크기·WorldMatrix를 쓴다.
    void PlaneCorners(Vec3d& topLeft, Vec3d& topRight, Vec3d& bottomLeft) const noexcept;
    void OnDisable() noexcept override;
    void OnDestroy(EndPlayReason reason) noexcept override;

private:
    Result<void> EnsureRoot() noexcept;
    void DetachLayer() noexcept;
    void ResetWindow() noexcept;
    ROCKS_PROPERTY(ReadOnly, Name = "widget") ObjectRef<UserWidget> widget_;
    ROCKS_PROPERTY(ReadOnly, Name = "drawSize") Vec2 drawSize_ { 500, 500 };
    ROCKS_PROPERTY(ReadOnly, Name = "pivot") Vec2 pivot_ { 0.5f, 0.5f };
    ROCKS_PROPERTY(ReadOnly, Name = "screenSpace") bool screenSpace_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "playerId") uint32 playerId_ = 0;
    ROCKS_NATIVE() Application window_;
    ROCKS_NATIVE() SharedPtr<Element> root_;
    ROCKS_NATIVE() WeakPtr<AnchorCanvas> layer_;
};

ROCKS_UIWORLD_API Result<void> RegisterUiWorldTypes(ModuleRegistration& registration) noexcept;

} // namespace rocks::ui
