#include "DemoShaders.h"
#include "SurfaceSelfTest.h"
#include <Core/ScopeExit.h>
#include <UiWidget/BorderWidget.generated.h>
#include <UiWidget/WidgetTree.generated.h>
#include <UiWidget/UserWidget.generated.h>
#include <World/Actor.generated.h>
#include <World/World.h>
#include <WorldIntegration/WorldWidgetComponent.generated.h>
#include <WorldIntegration/WorldWidgetPresenter.h>

namespace rocks::ui {

int VerifyWorldWidgetPlane(Allocator& allocator, graphics::Device& device) noexcept {
    TypeRegistry types(allocator);
    ModuleRegistration registration(types, allocator);
    if (!RegisterWorldIntegrationTypes(registration)) {
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

    SlotArray<BrushTexture> textures(allocator);
    WorldWidgetPresenter presenter(allocator, device, ActiveShaders(), textures);
    auto initial = presenter.Prepare(widget, 0), idle = presenter.Prepare(widget, 1);
    if (!initial || !initial.Value() || !idle || idle.Value()) {
        return 151;
    }

    WorldWidgetPlane plane(device, ActiveShaders());
    using graphics::PixelFormat;
    if (!plane.Initialize(PixelFormat::RGBA8UnormSrgb, PixelFormat::D32Float,
                          PixelFormat::RGBA8Unorm, true)) {
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
    graphics::TextureDesc desc;
    desc.width = 16;
    desc.height = 16;
    desc.usage = graphics::TextureUsage::RenderTarget;
    auto color = device.CreateTexture(desc, {});
    if (!color) {
        return 153;
    }

    ROCKS_ON_SCOPE_EXIT {
        device.Retire(color.Value());
    };
    desc.format = PixelFormat::RGBA8Unorm;
    auto velocity = device.CreateTexture(desc, {});
    if (!velocity) {
        return 154;
    }

    ROCKS_ON_SCOPE_EXIT {
        device.Retire(velocity.Value());
    };
    desc.format = PixelFormat::D32Float;
    desc.usage = graphics::TextureUsage::DepthStencil;
    auto depth = device.CreateTexture(desc, {});
    if (!depth) {
        return 155;
    }

    ROCKS_ON_SCOPE_EXIT {
        device.Retire(depth.Value());
    };
    graphics::PassDesc pass;
    pass.colorCount = 2;
    pass.colors[0].texture = color.Value();
    pass.colors[0].clear = {1, 0, 0, 1};
    pass.colors[1].texture = velocity.Value();
    pass.colors[1].clear = {0, 0, 0, 0};
    pass.depth.texture = depth.Value();
    Vec3d tl, tr, bl;
    widget.PlaneCorners(tl, tr, bl);
    auto previous = WorldWidgetClipPlane::Project(tl, tr, bl, projection);
    if (!previous) {
        return 156;
    }

    previous.Value().topLeft.x -= 0.25f;
    previous.Value().topRight.x -= 0.25f;
    previous.Value().bottomLeft.x -= 0.25f;
    Array<uint8> pixels(allocator);
    if (!pixels.TryResize(16 * 16 * 4)) {
        return 157;
    }

    const usize center = (8 * 16 + 8) * 4;
    for (uint32 i = 0; i < 2; ++i) {
        pass.depth.clearDepth = i == 0 ? 0.f : 0.8f;
        auto frame = device.BeginPass(pass);
        if (!frame) {
            return 158;
        }

        frame.Value()->SetViewport({0, 0, 16, 16, 0, 1});
        frame.Value()->SetScissor({0, 0, 16, 16});
        auto drawn = presenter.Submit(*frame.Value(), plane, widget, projection, &previous.Value());
        auto ended = device.EndFrame();
        if (!drawn || !ended || !device.ReadPixels(color.Value(), pixels.View())) {
            return 159;
        }

        if (pixels[0] < 245 || pixels[1] > 10 ||
            (i == 0 ? pixels[center + 1] < 245 : pixels[center] < 245)) {
            return 160;
        }

        if (!device.ReadPixels(velocity.Value(), pixels.View()) ||
            (i == 0 ? pixels[center] < 30 || pixels[center] > 34 : pixels[center] != 0)) {
            return 161;
        }
    }

    return 0;
}
} // namespace rocks::ui
