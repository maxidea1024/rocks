#include "DemoShaders.h"
#include "SurfaceSelfTest.h"
#include <UiFramework/Application.h>
#include <UiFramework/Viewport.h>
#include <UiRender/WidgetSurfaceHost.h>
#include <UiWidget/ViewportWidget.generated.h>
#include <World/Actor.generated.h>

namespace rocks::ui {

int VerifyViewportWorld(Allocator& allocator, graphics::Device& device,
                        graphics::PipelineHandle solid) noexcept {
    TypeRegistry types(allocator);
    ModuleRegistration registration(types, allocator);
    if (!RegisterWidgetTypes(registration)) {
        return 121;
    }
    ObjectRegistry objects(allocator, types, {0x554957, 93});
    const auto access = objects.OwnerAccess();
    ObjectAccessScope scope(access);
    auto owner = objects.NewObject<ViewportWidget>(access, GeneratedType<ViewportWidget>::Id());
    if (!owner) {
        return 122;
    }
    auto borrowed = objects.ResolveForWrite<ViewportWidget>(access, owner.Value().Handle());
    if (!borrowed) {
        return 123;
    }
    auto& widget = *borrowed.Value().Get().Value();
    auto lease = widget.AcquireViewportWorld(objects);
    if (!lease) {
        return 124;
    }
    auto actor = widget.Spawn(objects, GeneratedType<Actor>::Id());
    if (!actor || !widget.SetCamera({-4, 0, 0}, Quat::Identity(), 90) ||
        !widget.SetBackground({1, 0, 0, 1})) {
        return 124;
    }
    auto renderer = ViewportWorld::Renderer::CreateLambda(
        [&](World& world, const ViewportCameraView& camera, graphics::Device& gpu,
            graphics::CommandContext& commands) noexcept -> Result<void> {
            PaintList list(allocator);
            // 검증용 렌더러는 월드 원점의 액터를 카메라로 투영한 뒤 그 위치에 표식을 그린다.
            auto clip = (camera.view * camera.projection).TransformVector4({0, 0, 0, 1});
            bool present = false;
            world.ForEachActor([&](ObjectHandle h) noexcept { present |= h == actor.Value(); });
            if (!present || clip.w <= 0) {
                return Result<void>::Failure(ErrorCode::NotFound);
            }
            const float x = (clip.x / clip.w + 1) * 0.5f * camera.width;
            const float y = (1 - clip.y / clip.w) * 0.5f * camera.height;
            auto added = list.TryAddBox(
                {Geometry::Root({2, 2}, {1, 0, 0, 1, x - 1, y - 1}), {0, 1, 0, 1}, 0});
            if (!added) {
                return added;
            }
            DrawBatch batch(allocator);
            auto built = batch.Build(list);
            if (!built) {
                return built;
            }
            return DrawBatchRenderer::Draw(
                gpu, ActiveShaders(), commands, solid, batch,
                {static_cast<float>(camera.width), static_cast<float>(camera.height)});
        });
    if (!widget.SetSceneRenderer(objects, Move(renderer))) {
        return 125;
    }
    auto element = widget.TakeElement(objects);
    if (!element || !element.Value()->Arrange(Geometry::Root({8, 8}, {}))) {
        return 126;
    }
    SlotArray<BrushTexture> textures(allocator);
    WidgetSurfaceHost host(allocator, device, ActiveShaders(), textures);
    if (!host.Prepare(element.Value(), 0, 8, 8, solid)) {
        return 127;
    }
    auto* brush = textures.Find(static_cast<Viewport&>(*element.Value()).ManagedSurface());
    Array<uint8> pixels(allocator);
    if (!brush || !pixels.TryResize(8 * 8 * 4) ||
        !device.ReadPixels(brush->texture, pixels.View())) {
        return 128;
    }
    const auto center = (4 * 8 + 4) * 4;
    return pixels[0] > 245 && pixels[1] < 10 && pixels[center] < 10 && pixels[center + 1] > 245
               ? 0
               : 129;
}

} // namespace rocks::ui
