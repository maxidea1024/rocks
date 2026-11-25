#pragma once

#include <Core/Delegate.h>
#include <Core/Math/Matrix4.h>
#include <Core/Math/Quaternion.h>
#include <Core/Memory/UniquePtr.h>
#include <UiCore/WidgetSurfaceScene.h>
#include <World/World.h>

namespace rocks::ui {

// Rocks 좌표에서 +X가 전방, +Z가 위다. 행 벡터에 view * projection을 곱한다.
// 실제 타깃 크기로 투영 행렬을 만들어 레이아웃 크기와 화면 비율이 달라져도 대응한다.
struct ViewportCameraView {
    Vec3 location{};
    Quat rotation{};
    Mat4 view{};
    Mat4 projection{};
    uint32 width = 0;
    uint32 height = 0;
};

// 인게임 뷰포트 하나가 독립 레지스트리, 레벨, 액터 수명 주기를 소유한다.
// 마지막 참조를 반환하기 전에 이 월드에서 빌린 Borrow와 RootRef를 반환해야 한다.
class ROCKS_UIWIDGET_API ViewportWorld final : public WidgetSurfaceScene {
public:
    // 준비가 끝난 렌더 타깃에 그리기 명령만 기록한다. 패스 시작과 제출은 표면 호스트가 담당한다.
    using Renderer = Delegate<Result<void>(World&, const ViewportCameraView&, graphics::Device&,
                                           graphics::CommandContext&)>;

    ViewportWorld(Allocator& allocator, TypeRegistry& types) noexcept;
    Result<void> Initialize() noexcept;
    World* GetWorld() const noexcept;
    void SetRenderer(Renderer renderer, bool depth = false) noexcept;
    bool UsesDepth() const noexcept override;
    Result<void> SetCamera(Vec3 location, Quat rotation, float fieldOfView) noexcept;
    Result<ViewportCameraView> Camera(uint32 width, uint32 height) const noexcept;
    void SetBackground(LinearColor color) noexcept;
    LinearColor Background() const noexcept override;
    Result<void> Advance(uint64 frame) noexcept override;
    Result<void> Draw(graphics::Device& device, graphics::CommandContext& commands, uint32 width,
                      uint32 height) noexcept override;

private:
    Allocator& allocator_;
    TypeRegistry& types_;
    ModuleRegistration registration_;
    UniquePtr<World> world_;
    Renderer render_;
    bool depth_ = false;
    Vec3 location_{};
    Quat rotation_{};
    float fieldOfView_ = 90;
    LinearColor background_{0, 0, 0, 1};
    uint64 lastFrame_ = 0;
    uint64 lastTime_ = 0;
    bool advanced_ = false;
};

} // namespace rocks::ui
