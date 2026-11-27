#pragma once

#include <UiCore/DrawBatch.h>
#include <UiCore/Rendering/SurfaceRecorder.h>
#include <UiRender/WorldWidgetPlane.h>
#include <UiWorld/WorldWidgetComponent.h>

namespace rocks::ui {

// 월드 위젯 한 개의 게임 lane 쪽 기록기다(Docs/Design/UiToolkit 18.4절 UiWorld). 위젯 트리를 보관
// 렌더 타깃에 그리는 pass(와 그 안 표면 pass)를 draw buffer에 쌓는다. UI 캐시와 자식 표면이
// 그대로면 다시 그리지 않는다. 3D 평면 합성은 장면 pass 기록 함수가 Render lane에서
// WorldWidgetPlane으로 한다(Resource의 렌더 handle과 Project 결과를 장면 packet에 담는다).
//
// 사용 예(게임 lane, 한 프레임):
//   auto changed = presenter.Prepare(component, frame, buffer);   // 보관 pass를 쌓는다
//   auto plane = presenter.Project(component, viewProjection);     // 장면 packet에 담는다
//   ... buffer.AddOffscreen·AddWindow로 pass를 닫는다(합성하는 장면 대상) ...
//
// 수명: renderer는 이 객체보다 오래 산다. 쌓은 pass의 DrawBatch는 이 객체가 소유한다(DrawWindows가
// 끝날 때까지 다시 Prepare하지 않는다).
class ROCKS_UIWORLD_API WorldWidgetPresenter {
public:
    WorldWidgetPresenter(Allocator& allocator, UiRenderer& renderer) noexcept;
    ~WorldWidgetPresenter() noexcept;

    WorldWidgetPresenter(const WorldWidgetPresenter&) = delete;
    WorldWidgetPresenter& operator=(const WorldWidgetPresenter&) = delete;

    // 위젯을 배치하고, 바뀌었으면 보관 pass를 buffer에 쌓는다. true면 이번에 다시 그린다.
    Result<bool> Prepare(WorldWidgetComponent& component, uint64 frame,
                         UiDrawBuffer& buffer) noexcept;

    // 보관 렌더 타깃이다(Premultiplied). Prepare 전에는 빈 id다.
    UiTextureId Resource() const noexcept;

    // 평면 꼭짓점을 viewProjection으로 투영한다. 비활성·화면 공간·아직 그리지 않은 위젯은
    // NotReady다.
    Result<WorldWidgetClipPlane> Project(const WorldWidgetComponent& component,
                                         const Mat4d& viewProjection) const noexcept;

private:
    UiRenderer& renderer_;
    SurfaceRecorder children_;
    DrawBatch batch_;
    UiTextureId surface_{};
    WeakPtr<Element> root_;
    uint64 revision_ = 0;
    Vec2 size_{};
};

} // namespace rocks::ui
