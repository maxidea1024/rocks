#include "DemoCapture.h"
#include "SceneBoxes.h"
#include "SurfaceSelfTest.h"

#include <Core/ScopeExit.h>
#include <UiCore/Rendering/SurfaceRecorder.h>
#include <UiFramework/Application/Application.h>
#include <UiFramework/Effects/Viewport.h>
#include <UiWidget/ViewportWidget.generated.h>
#include <World/Actor.generated.h>

namespace rocks::ui {

int VerifyViewportWorld(Allocator& allocator, UiRenderer& renderer) noexcept {
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

    // 게임 lane에서 World를 읽어 표식 위치를 정하고, 그 상자를 장면 packet으로 게시한다(장치 기록은
    // Render lane — Docs/Design/UiToolkit 18.3절).
    auto sceneRenderer = ViewportWorld::Renderer::CreateLambda(
        [&](World& world, const ViewportCameraView& camera,
            UiSceneExtract& extract) noexcept -> Result<void> {
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

            return PostSceneBoxes(extract, allocator, list);
        });
    if (!widget.SetSceneRenderer(objects, Move(sceneRenderer))) {
        return 125;
    }

    auto element = widget.TakeElement(objects);
    if (!element || !element.Value()->Arrange(Geometry::Root({8, 8}, {}))) {
        return 126;
    }

    // 표면 pass를 쌓아 빈 오프스크린 대상과 함께 그리고 viewport 렌더 타깃을 읽는다.
    SurfaceRecorder recorder(allocator, renderer);
    UiDrawBuffer buffer(allocator);
    DrawBatch empty(allocator);
    auto flush = renderer.CreateRenderTarget(8, 8, UiTextureSampling::Image);
    if (!flush) {
        return 127;
    }

    ROCKS_ON_SCOPE_EXIT {
        renderer.ReleaseTexture(flush.Value());
    };
    Array<uint8> pixels(allocator);
    if (!recorder.Record(element.Value(), 0, 8, 8, buffer) ||
        !DrawAndCapture(renderer, buffer, flush.Value(), 8, 8, {0, 0, 0, 1}, empty, pixels)) {
        return 127;
    }

    const UiTextureId scene = static_cast<Viewport&>(*element.Value()).ManagedSurface();
    if (scene.IsNull() || !CaptureTarget(renderer, scene, pixels) || pixels.Size() != 8 * 8 * 4) {
        return 128;
    }

    const auto center = (4 * 8 + 4) * 4;
    return pixels[0] > 245 && pixels[1] < 10 && pixels[center] < 10 && pixels[center + 1] > 245
               ? 0
               : 129;
}

} // namespace rocks::ui
