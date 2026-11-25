#pragma once

#include <Core/Math/Matrix4.h>
#include <UiRender/DrawBatchRenderer.h>

namespace rocks::ui {

// 장면 패스의 현재·직전 clip 좌표다. 카메라 jitter·해상도는 장면 렌더러 정책을 그대로 사용한다.
struct WorldWidgetClipPlane {
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
    explicit WorldWidgetPlane(graphics::Device& device) noexcept;
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
    graphics::PipelineHandle pipeline_{};
    graphics::BufferHandle quad_{};
};

} // namespace rocks::ui
