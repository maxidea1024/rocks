#include "DemoCapture.h"
#include "SurfaceSelfTest.h"

#include <Core/ScopeExit.h>
#include <UiRender/UiLaneRenderer.h>
#include <UiWidget/BorderWidget.generated.h>
#include <UiWidget/WidgetTree.generated.h>
#include <UiWidget/UserWidget.generated.h>
#include <World/Actor.generated.h>
#include <World/World.h>
#include <UiWorld/WorldWidgetComponent.generated.h>
#include <UiWorld/WorldWidgetPresenter.h>

namespace rocks::ui::detail {

// 평면 합성 검사 lane 작업 값이다. ui는 렌더 lane 쪽 객체이고, 결과는 그 객체의 화면 읽기 표에
// capture 번호로 보관한다(게임 lane 주소를 넘기지 않는다).
struct WorldPlaneCheck {
    UiLaneRenderer* ui = nullptr;
    render::RenderHandle surface{};
    WorldWidgetClipPlane current{};
    WorldWidgetClipPlane previous{};
    float clearDepth = 0;
    uint64 colorCapture = 0;
    uint64 velocityCapture = 0;
};

} // namespace rocks::ui::detail

ROCKS_LANE_TRANSFERABLE(rocks::ui::detail::WorldPlaneCheck);

namespace rocks::ui {

namespace {

// 평면 검사 결과의 capture 번호다(LaneUiRenderer가 발급하는 번호와 겹치지 않는 구간).
constexpr uint64 WorldPlaneCaptureBase = 1ull << 62;
// 검사 대상 한 변(화소)과 RGBA byte 수다.
constexpr uint32 WorldPlaneTargetSize = 16;
constexpr usize WorldPlaneRgbaBytes = 4;

// 렌더 lane에서 장치로 평면을 그린다: 색·속도 attachment와 깊이(reverse-Z)를 만들고 보관 표면을
// WorldWidgetPlane으로 합성한 뒤 두 attachment를 읽는다(장면 렌더러가 할 일 — 18.4절 UiWorld).
void CheckWorldPlaneOnLane(render::RenderLaneContext& context,
                           detail::WorldPlaneCheck& task) noexcept {
    graphics::Device& device = context.Device();
    const auto fail = [&](ErrorCode error) noexcept {
        task.ui->StoreCapture(task.colorCapture, Array<uint8>(task.ui->GetAllocator()), error);
        task.ui->StoreCapture(task.velocityCapture, Array<uint8>(task.ui->GetAllocator()), error);
    };
    auto surface = context.Handles().Texture(task.surface);
    if (!surface || !task.ui->Shaders()) {
        return fail(surface ? ErrorCode::NotReady : surface.Error());
    }

    using graphics::PixelFormat;
    WorldWidgetPlane plane(device, *task.ui->Shaders());
    if (auto initialized = plane.Initialize(PixelFormat::RGBA8UnormSrgb, PixelFormat::D32Float,
                                            PixelFormat::RGBA8Unorm, true);
        !initialized) {
        return fail(initialized.Error());
    }

    graphics::TextureDesc desc;
    desc.width = WorldPlaneTargetSize;
    desc.height = WorldPlaneTargetSize;
    desc.usage = graphics::TextureUsage::RenderTarget;
    auto color = device.CreateTexture(desc, {});
    desc.format = PixelFormat::RGBA8Unorm;
    auto velocity = device.CreateTexture(desc, {});
    desc.format = PixelFormat::D32Float;
    desc.usage = graphics::TextureUsage::DepthStencil;
    auto depth = device.CreateTexture(desc, {});
    ROCKS_ON_SCOPE_EXIT {
        if (color) {
            device.Retire(color.Value());
        }

        if (velocity) {
            device.Retire(velocity.Value());
        }

        if (depth) {
            device.Retire(depth.Value());
        }
    };
    if (!color || !velocity || !depth) {
        return fail(!color ? color.Error() : !velocity ? velocity.Error() : depth.Error());
    }

    graphics::PassDesc pass;
    pass.colorCount = 2;
    pass.colors[0].texture = color.Value();
    pass.colors[0].clear = {1, 0, 0, 1};
    pass.colors[1].texture = velocity.Value();
    pass.colors[1].clear = {0, 0, 0, 0};
    pass.depth.texture = depth.Value();
    pass.depth.clearDepth = task.clearDepth;
    auto frame = device.BeginPass(pass);
    if (!frame) {
        return fail(frame.Error());
    }

    frame.Value()->SetViewport({0, 0, WorldPlaneTargetSize, WorldPlaneTargetSize, 0, 1});
    frame.Value()->SetScissor({0, 0, WorldPlaneTargetSize, WorldPlaneTargetSize});
    auto drawn = plane.Draw(*frame.Value(), surface.Value(), task.current, task.previous);
    auto ended = device.EndFrame();
    if (!drawn || !ended) {
        return fail(!drawn ? drawn.Error() : ended.Error());
    }

    const usize bytes =
        static_cast<usize>(WorldPlaneTargetSize) * WorldPlaneTargetSize * WorldPlaneRgbaBytes;
    Array<uint8> colorPixels(task.ui->GetAllocator());
    Array<uint8> velocityPixels(task.ui->GetAllocator());
    if (auto sized = colorPixels.TryResize(bytes); !sized) {
        return fail(sized.Error());
    }

    if (auto sized = velocityPixels.TryResize(bytes); !sized) {
        return fail(sized.Error());
    }

    if (auto read = device.ReadPixels(color.Value(), colorPixels.View()); !read) {
        return fail(read.Error());
    }

    if (auto read = device.ReadPixels(velocity.Value(), velocityPixels.View()); !read) {
        return fail(read.Error());
    }

    task.ui->StoreCapture(task.colorCapture, Move(colorPixels), ErrorCode::None);
    task.ui->StoreCapture(task.velocityCapture, Move(velocityPixels), ErrorCode::None);
}

} // namespace

int VerifyWorldWidgetPlane(Allocator& allocator, LaneUiRenderer& renderer,
                           render::RenderLane& lane) noexcept {
    TypeRegistry types(allocator);
    ModuleRegistration registration(types, allocator);
    if (!RegisterUiWorldTypes(registration)) {
        return 141;
    }

    WorldDesc options;
    options.name = "WorldUi";
    options.context = {0x575549, 1};
    World world(allocator, types, options);
    if (!world.Initialize()) {
        return 142;
    }

    ROCKS_ON_SCOPE_EXIT {
        (void)world.Shutdown();
    };
    auto actor = world.SpawnActor<Actor>();
    if (!actor) {
        return 143;
    }

    auto component = world.AddComponent<WorldWidgetComponent>(actor.Value());
    if (!component) {
        return 144;
    }

    auto& objects = world.Objects();
    const auto access = world.Access();
    ObjectAccessScope scope(access);
    auto user = objects.NewObject<UserWidget>(access, GeneratedType<UserWidget>::Id());
    if (!user) {
        return 145;
    }

    auto ui = objects.ResolveForWrite<UserWidget>(access, user.Value().Handle());
    if (!ui || !ui.Value().Get().Value()->Initialize(objects)) {
        return 146;
    }

    auto tree = objects.ResolveForWrite<WidgetTree>(access, ui.Value().Get().Value()->Tree());
    if (!tree) {
        return 147;
    }

    auto content = tree.Value().Get().Value()->ConstructWidget(
        objects, GeneratedType<BorderWidget>::Id(), "Content");
    if (!content || !tree.Value().Get().Value()->SetRoot(objects, content.Value().Handle())) {
        return 148;
    }

    auto border = objects.ResolveForWrite<BorderWidget>(access, content.Value().Handle());
    auto borrowed = objects.ResolveForWrite<WorldWidgetComponent>(access, component.Value());
    if (!border || !borrowed || !border.Value().Get().Value()->SetTint({0, 1, 0, 1})) {
        return 149;
    }

    auto& widget = *borrowed.Value().Get().Value();
    if (!widget.SetDrawSize({8, 8}) || !widget.SetWidget(user.Value().Handle())) {
        return 150;
    }

    // 위젯 트리를 보관 렌더 타깃에 그리는 pass를 쌓고(게임 lane) 빈 오프스크린 대상과 함께 그린다.
    // 두 번째 Prepare는 바뀐 것이 없어 pass를 쌓지 않는다.
    WorldWidgetPresenter presenter(allocator, renderer);
    UiDrawBuffer buffer(allocator);
    auto initial = presenter.Prepare(widget, 0, buffer);
    const usize pending = buffer.PendingPassCount();
    auto idle = presenter.Prepare(widget, 1, buffer);
    if (!initial || !initial.Value() || !idle || idle.Value() || pending == 0 ||
        buffer.PendingPassCount() != pending) {
        return 151;
    }

    auto flush = renderer.CreateRenderTarget(1, 1, UiTextureSampling::Image);
    if (!flush) {
        return 152;
    }

    ROCKS_ON_SCOPE_EXIT {
        renderer.ReleaseTexture(flush.Value());
    };
    DrawBatch empty(allocator);
    Array<uint8> ignored(allocator);
    if (!DrawAndCapture(renderer, buffer, flush.Value(), 1, 1, {0, 0, 0, 1}, empty, ignored)) {
        return 152;
    }

    Mat4d projection{};
    for (auto& value : projection.m) {
        value = 0;
    }

    projection.m[4] = -0.125;
    projection.m[9] = 0.125;
    projection.m[14] = 0.5;
    projection.m[15] = 1;
    auto current = presenter.Project(widget, projection);
    auto previous = presenter.Project(widget, projection);
    if (!current || !previous) {
        return 156;
    }

    previous.Value().topLeft.x -= 0.25f;
    previous.Value().topRight.x -= 0.25f;
    previous.Value().bottomLeft.x -= 0.25f;
    Array<uint8> pixels(allocator);
    const usize center = (8 * 16 + 8) * 4;
    for (uint32 i = 0; i < 2; ++i) {
        detail::WorldPlaneCheck check;
        check.ui = &renderer.LaneRenderer();
        check.surface = renderer.HandleOf(presenter.Resource());
        check.current = current.Value();
        check.previous = previous.Value();
        check.clearDepth = i == 0 ? 0.f : 0.8f;
        check.colorCapture = WorldPlaneCaptureBase + i * 2;
        check.velocityCapture = check.colorCapture + 1;
        const uint64 velocityCapture = check.velocityCapture;
        if (!lane.PostSurfaceAndWait(Move(check), &CheckWorldPlaneOnLane) ||
            !renderer.LaneRenderer().TakeCapture(WorldPlaneCaptureBase + i * 2, pixels) ||
            pixels.Size() != 16 * 16 * 4) {
            return 159;
        }

        if (pixels[0] < 245 || pixels[1] > 10 ||
            (i == 0 ? pixels[center + 1] < 245 : pixels[center] < 245)) {
            return 160;
        }

        if (!renderer.LaneRenderer().TakeCapture(velocityCapture, pixels) ||
            (i == 0 ? pixels[center] < 30 || pixels[center] > 34 : pixels[center] != 0)) {
            return 161;
        }
    }

    return 0;
}
} // namespace rocks::ui
