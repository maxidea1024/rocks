#pragma once

#include <UiRender/DrawBatchBuffers.h>
#include <UiRender/WidgetSurfaceHost.h>
#include <UiRender/WorldWidgetPlane.h>
#include <UiWorld/WorldWidgetComponent.h>

namespace rocks::ui {

// 단일 스레드 게임 호스트의 월드 위젯 한 개에 대한 표면이다. 장면 패스 전에 Prepare,
// 장면 패스 안에서 Submit을 호출한다. UI 캐시와 자식 표면이 그대로면 다시 래스터화하지 않는다.
class ROCKS_UIWORLD_API WorldWidgetPresenter {
public:
    WorldWidgetPresenter(
        Allocator& allocator, graphics::Device& device, render::ShaderSet& shaders,
        SlotArray<BrushTexture>& textures,
        graphics::PixelFormat surfaceFormat = graphics::PixelFormat::RGBA8UnormSrgb) noexcept;
    ~WorldWidgetPresenter() noexcept;
    WorldWidgetPresenter(const WorldWidgetPresenter&) = delete;
    WorldWidgetPresenter& operator=(const WorldWidgetPresenter&) = delete;

    Result<bool> Prepare(WorldWidgetComponent& component, uint64 frame) noexcept;
    SlotHandle Resource() const noexcept;
    // previous는 같은 로컬 플레이어의 직전 프레임 결과다. 첫 프레임은 null로 속도를 0으로 한다.
    Result<WorldWidgetClipPlane> Submit(graphics::CommandContext& commands, WorldWidgetPlane& plane,
                                        const WorldWidgetComponent& component,
                                        const Mat4d& viewProjection,
                                        const WorldWidgetClipPlane* previous = nullptr) noexcept;

private:
    graphics::Device& device_;
    render::ShaderSet& shaders_;
    SlotArray<BrushTexture>& textures_;
    graphics::PixelFormat format_;
    WidgetSurfaceHost children_;
    DrawBatch batch_;
    DrawBatchBuffers buffers_;
    RetainedSurface surface_;
    WeakPtr<Element> root_;
    uint64 revision_ = 0;
    Vec2 size_{};
    graphics::PipelineHandle solid_{};
    graphics::PipelineHandle composite_{};
};

} // namespace rocks::ui
