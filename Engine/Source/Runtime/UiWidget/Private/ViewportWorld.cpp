#include <UiWidget/ViewportWorld.h>
#include <Core/Guid.h>
#include <Core/Math/MatrixFactory.h>
#include <Platform/Clock.h>

namespace rocks::ui {

ViewportWorld::ViewportWorld(Allocator& allocator, TypeRegistry& types) noexcept
    : allocator_(allocator), types_(types), registration_(types, allocator) {
}

Result<void> ViewportWorld::Initialize() noexcept {
    if (world_) {
        return Result<void>::Success();
    }
    auto types = RegisterWorldTypes(registration_);
    if (!types) {
        return types;
    }
    auto id = TryNewGuid();
    if (!id) {
        return Result<void>::Failure(id.Error());
    }
    WorldDesc desc{};
    desc.name = "Viewport";
    desc.context = id.Value();
    auto made = UniquePtr<World>::TryMake(allocator_, allocator_, types_, desc);
    if (!made) {
        return Result<void>::Failure(made.Error());
    }
    auto initialized = made.Value()->Initialize();
    if (!initialized) {
        return initialized;
    }
    world_ = Move(made.Value());
    return Result<void>::Success();
}

World* ViewportWorld::GetWorld() const noexcept {
    return world_.Get();
}
void ViewportWorld::SetRenderer(Renderer renderer, bool depth) noexcept {
    depth_ = depth;
    render_ = Move(renderer);
}

bool ViewportWorld::UsesDepth() const noexcept {
    return render_.IsBound() && depth_;
}

Result<void> ViewportWorld::SetCamera(Vec3 location, Quat rotation, float fieldOfView) noexcept {
    if (!location.IsFinite() || !rotation.IsNormalized(1.0e-3f) || !math::IsFinite(fieldOfView) ||
        fieldOfView <= 0 || fieldOfView >= 180) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    location_ = location;
    rotation_ = rotation;
    fieldOfView_ = fieldOfView;
    return Result<void>::Success();
}

Result<ViewportCameraView> ViewportWorld::Camera(uint32 width, uint32 height) const noexcept {
    if (!width || !height) {
        return Result<ViewportCameraView>::Failure(ErrorCode::InvalidArgument);
    }
    auto view = TryMakeLookFrom(location_, rotation_.RotateVector({1, 0, 0}),
                                rotation_.RotateVector({0, 0, 1}));
    if (!view) {
        return Result<ViewportCameraView>::Failure(view.Error());
    }
    auto projection =
        TryMakePerspective(fieldOfView_ * math::Pi<float> / 360.0f, static_cast<float>(width),
                           static_cast<float>(height), 0.1f);
    if (!projection) {
        return Result<ViewportCameraView>::Failure(projection.Error());
    }
    return Result<ViewportCameraView>::Success(
        {location_, rotation_, view.Value(), projection.Value(), width, height});
}

Result<void> ViewportWorld::Advance(uint64 frame) noexcept {
    if (!world_) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }
    if (advanced_ && frame == lastFrame_) {
        return Result<void>::Success();
    }
    if (world_->State() == WorldState::Initialized) {
        auto began = world_->BeginPlay();
        if (!began) {
            return began;
        }
    }
    const uint64 now = platform::MonotonicNanoseconds();
    // 숨겨졌다가 돌아온 화면이 보이지 않았던 시간을 한꺼번에 시뮬레이션하지 않는다.
    const double dt =
        advanced_ && frame - lastFrame_ == 1 ? static_cast<double>(now - lastTime_) * 1.0e-9 : 0.0;
    auto ticked = world_->Tick(dt);
    if (ticked) {
        lastFrame_ = frame;
        lastTime_ = now;
        advanced_ = true;
    }
    return ticked;
}

void ViewportWorld::SetBackground(LinearColor color) noexcept {
    background_ = color;
}
LinearColor ViewportWorld::Background() const noexcept {
    return background_;
}

Result<void> ViewportWorld::Draw(graphics::Device& device, graphics::CommandContext& commands,
                                 uint32 width, uint32 height) noexcept {
    if (!world_) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }
    if (!render_) {
        return Result<void>::Success();
    }
    auto camera = Camera(width, height);
    if (!camera) {
        return Result<void>::Failure(camera.Error());
    }
    return render_.Execute(*world_, camera.Value(), device, commands);
}

} // namespace rocks::ui
