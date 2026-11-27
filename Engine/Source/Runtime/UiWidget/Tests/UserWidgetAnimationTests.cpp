#include <Core/Memory/SystemAllocator.h>
#include <ObjectSerialization/ObjectGraph.h>
#include <TestSupport/Test.h>
#include <UiWidget/SpacerWidget.generated.h>
#include <UiWidget/UserWidget.generated.h>
#include <UiWidget/WidgetAnimation.generated.h>
#include <UiWidget/WidgetTree.generated.h>

namespace rocks::ui {

TEST(UiWidget_AnimationsSerializeCurvesAndTargetStableWidgetIds) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    ModuleRegistration modules(types, allocator);
    REQUIRE(RegisterWidgetTypes(modules));
    ObjectRegistry source(allocator, types, {0x554957, 1511});
    ObjectRegistry target(allocator, types, {0x554957, 1512});
    Array<uint8> bytes(allocator);
    const Guid id{1, 17};
    {
        const auto access = source.OwnerAccess();
        ObjectAccessScope scope(access);
        auto root = source.NewObject<WidgetAnimation>(access, GeneratedType<WidgetAnimation>::Id());
        REQUIRE(root);
        auto clip = source.ResolveForWrite<WidgetAnimation>(access, root.Value().Handle());
        REQUIRE(clip);
        TangentCurve curve(allocator);
        REQUIRE(curve.TryAddKey(0, 0));
        REQUIRE(curve.TryAddKey(2, 20));
        REQUIRE(clip.Value().Get().Value()->SetDuration(2));
        REQUIRE(
            clip.Value().Get().Value()->SetTrack(id, WidgetAnimationChannel::TranslationY, curve));
        auto saved = SaveObjectGraph(allocator, source, access, root.Value().Handle());
        REQUIRE(saved);
        bytes = Move(saved.Value());
    }

    {
        const auto access = target.OwnerAccess();
        ObjectAccessScope scope(access);
        auto loaded = LoadObjectGraph(allocator, target, types, access, bytes.View());
        REQUIRE(loaded);
        auto clip = target.Resolve<WidgetAnimation>(access, loaded.Value().Handle());
        REQUIRE(clip);
        const auto* animation = clip.Value().Get().Value();
        CHECK(animation->Duration() == 2);
        auto tracks = animation->Tracks();
        REQUIRE(tracks.Size() == 1);
        CHECK(tracks[0].target == id);
        CHECK(tracks[0].channel == static_cast<uint8>(WidgetAnimationChannel::TranslationY));
        CHECK(tracks[0].curve.Evaluate(1) == 10);
    }
}

TEST(UiWidget_AnimationFramesPauseReverseSeekLoopAndRestoreState) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    ModuleRegistration modules(types, allocator);
    REQUIRE(RegisterWidgetTypes(modules));
    ObjectRegistry objects(allocator, types, {0x554957, 1513});
    const auto access = objects.OwnerAccess();
    ObjectAccessScope scope(access);
    auto root = objects.NewObject<UserWidget>(access, GeneratedType<UserWidget>::Id());
    REQUIRE(root);
    auto owner = objects.ResolveForWrite<UserWidget>(access, root.Value().Handle());
    REQUIRE(owner);
    auto* screen = owner.Value().Get().Value();
    REQUIRE(screen->Initialize(objects));
    auto tree = objects.ResolveForWrite<WidgetTree>(access, screen->Tree());
    REQUIRE(tree);
    auto* widgets = tree.Value().Get().Value();
    auto id = widgets->ConstructWidget(objects, GeneratedType<SpacerWidget>::Id(), "Movable");
    REQUIRE(id && widgets->SetRoot(objects, id.Value().Handle()));
    auto spacer = objects.ResolveForWrite<SpacerWidget>(access, id.Value().Handle());
    REQUIRE(spacer);
    auto* child = spacer.Value().Get().Value();
    Application app(allocator);
    auto native = screen->TakeElement(objects);
    REQUIRE(native);
    app.SetRoot(native.Value());

    TangentCurve opacity(allocator), position(allocator);
    REQUIRE(opacity.TryAddKey(0, 0) && opacity.TryAddKey(1, 1));
    REQUIRE(position.TryAddKey(0, 0) && position.TryAddKey(1, 40));
    auto animation =
        objects.NewObject<WidgetAnimation>(access, GeneratedType<WidgetAnimation>::Id());
    REQUIRE(animation);
    auto clip = objects.ResolveForWrite<WidgetAnimation>(access, animation.Value().Handle());
    REQUIRE(clip);
    auto& definition = *clip.Value().Get().Value();
    REQUIRE(definition.SetTrack({}, WidgetAnimationChannel::Opacity, opacity));
    REQUIRE(definition.SetTrack(child->StableId(), WidgetAnimationChannel::TranslationX, position));
    const auto area = Geometry::Root({100, 80}, {});
    uint32 starts = 0, ends = 0;
    WidgetAnimationEnd reason = WidgetAnimationEnd::Failed;
    WidgetAnimationPlayback options;
    options.mode = WidgetAnimationMode::PingPong;
    options.restoreState = true;
    options.onStarted = Delegate<void()>::CreateLambda([&starts]() noexcept { ++starts; });
    options.onEnded =
        Delegate<void(WidgetAnimationEnd)>::CreateLambda([&](WidgetAnimationEnd why) noexcept {
            ++ends;
            reason = why;
        });
    auto player = screen->QueuePlayAnimation(objects, app, definition, Move(options));
    REQUIRE(player);
    CHECK(starts == 0 && screen->IsAnyAnimationPlaying());
    REQUIRE(screen->SetRenderOpacity(0.75f));
    REQUIRE(app.UpdatePaintCache(area, 1));
    CHECK(starts == 1 && screen->GetRenderOpacity() == 0);
    auto& control = *player.Value();
    REQUIRE(control.Advance(0.5));
    CHECK(screen->GetRenderOpacity() == 0.5f && child->GetRenderTransform().translation.x == 20);
    REQUIRE(control.Pause());
    CHECK(!screen->IsAnyAnimationPlaying());
    REQUIRE(control.Advance(1));
    CHECK(control.CurrentTime() == 0.5f);
    REQUIRE(control.Seek(0.25f));
    REQUIRE(control.SetSpeed(2));
    REQUIRE(control.Resume());
    REQUIRE(control.Advance(0.5));
    CHECK(control.IsReversed() && control.CurrentTime() == 0.75f);
    REQUIRE(control.Advance(1));
    CHECK(ends == 1 && reason == WidgetAnimationEnd::Finished && !screen->IsAnyAnimationPlaying());
    CHECK(screen->GetRenderOpacity() == 0.75f && child->GetRenderTransform().translation.x == 0);

    auto forever = screen->PlayAnimation(objects, app, definition,
                                         {.mode = WidgetAnimationMode::Reverse, .loops = 0});
    REQUIRE(forever);
    CHECK(forever.Value()->CurrentTime() == 1);
    REQUIRE(forever.Value()->Advance(3000.25));
    CHECK(forever.Value()->CurrentTime() == 0.75f);
    REQUIRE(forever.Value()->Reverse());
    REQUIRE(forever.Value()->Advance(0.125));
    CHECK(forever.Value()->CurrentTime() == 0.875f);
    screen->ReleaseElements();
    CHECK(forever.Value()->Status() == WidgetAnimationStatus::Ended);
    app.SetRoot({});
}

} // namespace rocks::ui
