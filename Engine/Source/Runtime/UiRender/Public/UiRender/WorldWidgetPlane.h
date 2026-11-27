#pragma once

#include <Core/Math/Matrix4.h>
#include <UiRender/DrawBatchRenderer.h>

namespace rocks::ui {

// 장면 패스의 현재·직전 clip 좌표다. 카메라 jitter·해상도는 장면 렌더러 정책을 그대로 사용한다.
struct ROCKS_UIRENDER_API WorldWidgetClipPlane {
    Vec4 topLeft{};
    Vec4 topRight{};
    Vec4 bottomLeft{};
    static Result<WorldWidgetClipPlane> Project(Vec3d topLeft, Vec3d topRight, Vec3d bottomLeft,
                                                const Mat4d& viewProjection) noexcept;
};

// 기존 장면 패스에 보관 표면을 사각형으로 제출한다. 깊이는 reverse-Z이며 대상·viewport·scissor는
// 호출자가 먼저 지정한다. 속도 attachment가 있으면 현재 UV - 직전 UV를 별도로 출력한다.
class ROCKS_UIRENDER_API WorldWidgetPlane {
public:
    WorldWidgetPlane(graphics::Device& device, render::ShaderSet& shaders) noexcept;
    ~WorldWidgetPlane() noexcept;
    WorldWidgetPlane(const WorldWidgetPlane&) = delete;
    WorldWidgetPlane& operator=(const WorldWidgetPlane&) = delete;
    Result<void> Initialize(graphics::PixelFormat color, graphics::PixelFormat depth,
                            graphics::PixelFormat velocity = graphics::PixelFormat::Unknown,
                            bool writeDepth = false, uint32 sampleCount = 1) noexcept;
    Result<void> Draw(graphics::CommandContext& commands, graphics::TextureHandle texture,
                      const WorldWidgetClipPlane& current,
                      const WorldWidgetClipPlane& previous) noexcept;

private:
    graphics::Device& device_;
    render::ShaderSet& shaders_;
    graphics::PipelineHandle pipeline_{};
    // 평면 셰이더의 표면 텍스처 slot이다(Initialize가 셰이더 layout에서 찾는다).
    uint32 surfaceSlot_ = graphics::MaxResourceSlots;
    graphics::BufferHandle quad_{};
};

} // namespace rocks::ui
