#include "GameHud.h"

#include <PlatformWindow/Clipboard.h>
#include <PlatformWindow/InlineComposition.h>

#include <UiWidget/WidgetAsset.h>
#include <UiWidget/WidgetAsset.generated.h>

#include <UiWidget/Widget.generated.h>
#include <UiWidget/ProgressBarWidget.generated.h>
#include <UiWidget/WidgetTree.generated.h>
#include <UiWidget/UserWidget.generated.h>

namespace rocks::ui {

Result<GameHud> CreateGameHud(ObjectRegistry& objects) noexcept {
    const auto access = objects.OwnerAccess();
    auto asset = objects.NewObject<WidgetAsset>(access, GeneratedType<WidgetAsset>::Id());
    if (!asset) {
        return Result<GameHud>::Failure(asset.Error());
    }

    auto editor = objects.ResolveForWrite<WidgetAsset>(access, asset.Value().Handle());
    if (!editor) {
        return Result<GameHud>::Failure(editor.Error());
    }

    auto* blueprint = editor.Value().Get().Value();
    auto ready = blueprint->Initialize(objects);
    if (!ready) {
        return Result<GameHud>::Failure(ready.Error());
    }

    auto tree = objects.ResolveForWrite<WidgetTree>(access, blueprint->Tree());
    if (!tree) {
        return Result<GameHud>::Failure(tree.Error());
    }

    auto* design = tree.Value().Get().Value();
    auto bar = design->ConstructWidget(objects, GeneratedType<ProgressBarWidget>::Id(), "Progress");
    if (!bar) {
        return Result<GameHud>::Failure(bar.Error());
    }

    auto attached = design->SetRoot(objects, bar.Value().Handle());
    if (!attached) {
        return Result<GameHud>::Failure(attached.Error());
    }

    auto source = objects.Resolve<Widget>(access, bar.Value().Handle());
    if (!source) {
        return Result<GameHud>::Failure(source.Error());
    }

    const auto id = source.Value().Get().Value()->StableId();

    auto compiled = blueprint->Compile(objects, objects.ObjectAllocator());
    if (!compiled) {
        return Result<GameHud>::Failure(compiled.Error());
    }

    auto instance = compiled.Value().Instantiate(objects);
    if (!instance) {
        return Result<GameHud>::Failure(instance.Error());
    }

    auto screen = objects.ResolveForWrite<UserWidget>(access, instance.Value().Handle());
    if (!screen) {
        return Result<GameHud>::Failure(screen.Error());
    }

    auto* user = screen.Value().Get().Value();
    auto live = objects.ResolveForWrite<WidgetTree>(access, user->Tree());
    if (!live) {
        return Result<GameHud>::Failure(live.Error());
    }

    live.Value().Get().Value()->SetPlatform(
        {platform::ReadClipboardText, platform::WriteClipboardText, platform::SetInlineComposition,
         platform::MoveCompositionWindow});
    auto target = live.Value().Get().Value()->FindWidget(objects, id);
    if (!target) {
        return Result<GameHud>::Failure(target.Error());
    }

    auto root = user->TakeElement(objects);
    if (!root) {
        return Result<GameHud>::Failure(root.Error());
    }

    return Result<GameHud>::Success({Move(root.Value()), target.Value()});
}

} // namespace rocks::ui
