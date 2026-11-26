#include "EffectsPage.h"

#include "GalleryTextures.h"

#include <UiFramework/Effects/BackgroundBlur.h>
#include <UiFramework/Effects/CachePanel.h>
#include <UiFramework/Layout/Frame.h>
#include <UiFramework/Layout/AnchorCanvas.h>
#include <UiFramework/Layout/HorizontalStack.h>
#include <UiFramework/Controls/Image.h>
#include <UiFramework/Layout/Overlay.h>
#include <UiFramework/Effects/RetainerBox.h>
#include <UiFramework/Controls/Slider.h>
#include <UiFramework/Effects/Viewport.h>

namespace rocks::ui::gallery {

namespace {

constexpr Vec2 EffectStage{240, 90};
constexpr Vec2 SliderSize{160, 20};
constexpr float StageRadius = 4;
constexpr Vec4 BlurInset{40, 20, 40, 20};
// 색 줄은 무대 가운데 띠로 둔다.
constexpr Vec4 StripeInset{0, 30, 0, 30};
// Viewport 위 UI 버튼의 크기다(장면이 보이도록 가운데 작게 둔다).
constexpr Vec2 SceneButtonSize{120, 24};
// 흐림 강도(픽셀) 범위와 기본값이다.
constexpr float MaximumBlur = 20;
constexpr float DefaultBlur = 6;
// 리테이너 effect tint 견본 색이다.
constexpr LinearColor RetainerTint{1.0f, 0.8f, 0.6f, 1.0f};
// 흐림 뒤 줄무늬 색 순서다.
constexpr ThemeSlot StripeColors[] = {ThemeSlot::AccentBlue,   ThemeSlot::AccentGreen,
                                      ThemeSlot::AccentOrange, ThemeSlot::AccentPurple,
                                      ThemeSlot::AccentPink,   ThemeSlot::AccentYellow};

Result<SharedPtr<Element>> MakeStage(GalleryContext& context,
                                     const SharedPtr<Element>& content) noexcept {
    auto frame = SharedPtr<Frame>::TryMake(context.allocator, context.allocator);
    if (!frame) {
        return Result<SharedPtr<Element>>::Failure(frame.Error());
    }

    const ThemeColors& theme = DefaultThemeColors();
    auto result = frame.Value()->Construct(
        {.content = content,
         .brush = Brush::RoundedBox(theme.Get(ThemeSlot::Recessed), StageRadius,
                                    theme.Get(ThemeSlot::Dropdown), 1.0f)});
    if (!result) {
        return Result<SharedPtr<Element>>::Failure(result.Error());
    }

    frame.Value()->SetClipToBounds(true);
    return Sized(context, frame.Value(), EffectStage);
}

// 줄무늬 위에 흐림 상자를 겹친다. lowQuality면 흐림 대신 대체 brush를 그린다.
Result<SharedPtr<BackgroundBlur>> MakeBlur(GalleryContext& context, bool lowQuality,
                                           SharedPtr<Element>& stage) noexcept {
    Allocator& allocator = context.allocator;
    auto overlay = SharedPtr<Overlay>::TryMake(allocator, allocator);
    auto stripes = SharedPtr<HorizontalStack>::TryMake(allocator, allocator);
    auto blur = SharedPtr<BackgroundBlur>::TryMake(allocator, allocator);
    auto label = MakeText(context, lowQuality ? "Low quality fallback" : "Blurred backdrop");
    if (const ErrorCode error = FirstError(overlay, stripes, blur, label);
        error != ErrorCode::None) {
        return Result<SharedPtr<BackgroundBlur>>::Failure(error);
    }

    // 뒤 그림은 체크무늬(절차 생성 텍스처)와 색 줄이다. 흐림이 경계를 뭉개는 것이 잘 보인다.
    auto checker = SharedPtr<Image>::TryMake(allocator, allocator);
    Result<void> result =
        checker ? Result<void>::Success() : Result<void>::Failure(checker.Error());
    if (result) {
        Brush pattern;
        pattern.resource = context.textures.Checker();
        result = checker.Value()->SetBrush(pattern);
    }

    for (usize i = 0; result && i < sizeof(StripeColors) / sizeof(StripeColors[0]); ++i) {
        auto block = MakeBlock(context, "|||", StripeColors[i]);
        result = block ? stripes.Value()->TryAddSlot(block.Value(),
                                                     {.rule = StackSizeRule::Fill, .weight = 1})
                       : Result<void>::Failure(block.Error());
    }

    if (result) {
        const ThemeColors& theme = DefaultThemeColors();
        LinearColor fallback = theme.Get(ThemeSlot::Panel);
        result = blur.Value()->Construct(
            {.content = label.Value(),
             .strength = DefaultBlur,
             .cornerRadius = {StageRadius, StageRadius, StageRadius, StageRadius},
             .lowQuality = lowQuality,
             .fallback = Brush::RoundedBox(fallback, StageRadius)});
    }

    auto inset = result ? PadContent(context, blur.Value(), BlurInset)
                        : Result<SharedPtr<Element>>::Failure(result.Error());
    if (!inset) {
        return Result<SharedPtr<BackgroundBlur>>::Failure(inset.Error());
    }

    result = overlay.Value()->TryAddSlot(checker.Value(), 0);
    if (result) {
        auto band = PadContent(context, stripes.Value(), StripeInset);
        result = band ? overlay.Value()->TryAddSlot(band.Value(), 1)
                      : Result<void>::Failure(band.Error());
    }

    if (result) {
        result = overlay.Value()->TryAddSlot(inset.Value(), 2);
    }

    auto made = result ? MakeStage(context, overlay.Value())
                       : Result<SharedPtr<Element>>::Failure(result.Error());
    if (!made) {
        return Result<SharedPtr<BackgroundBlur>>::Failure(made.Error());
    }

    stage = made.Value();
    return blur;
}

} // namespace

Result<EffectsPage> BuildEffectsPage(GalleryContext& context) noexcept {
    Allocator& allocator = context.allocator;
    auto grid = UniquePtr<GalleryGrid>::TryMake(allocator, context);
    if (!grid) {
        return Result<EffectsPage>::Failure(grid.Error());
    }

    GalleryGrid& rows = *grid.Value();
    BuildStatus status;

    // 배경 흐림: 강도 슬라이더로 바꾼다.
    {
        SharedPtr<Element> stage;
        auto blur = MakeBlur(context, false, stage);
        auto slider = SharedPtr<Slider>::TryMake(allocator, allocator);
        if (status.Take(blur) && status.Take(slider)) {
            auto set = slider.Value()->SetValue(DefaultBlur / MaximumBlur);
            slider.Value()->SetOnChanged(Delegate<void(float)>::CreateLambda(
                [target = WeakPtr<BackgroundBlur>(blur.Value())](float value) noexcept {
                    if (auto locked = target.Lock()) {
                        (void)locked->SetStrength(value * MaximumBlur);
                    }
                }));
            auto sized = Sized(context, slider.Value(), SliderSize);
            if (status.Take(set) && status.Take(sized)) {
                const SharedPtr<Element> parts[] = {stage, sized.Value()};
                rows.AddRow("Background Blur", "BackgroundBlur::SetStrength", parts);
            }
        }

        SharedPtr<Element> lowStage;
        auto low = MakeBlur(context, true, lowStage);
        if (status.Take(low)) {
            rows.AddRow("Background Blur (low quality)", "BackgroundBlur lowQuality + fallback",
                        lowStage);
        }
    }

    // 리테이너: 그린 결과를 표면에 보관하고 effect tint를 곱한다.
    {
        auto retainer = SharedPtr<RetainerBox>::TryMake(allocator, allocator);
        auto block = MakeBlock(context, "Retained surface", ThemeSlot::AccentGreen);
        if (status.Take(retainer) && status.Take(block)) {
            auto built =
                retainer.Value()->Construct({.content = block.Value(), .effectTint = RetainerTint});
            auto stage = built ? MakeStage(context, retainer.Value())
                               : Result<SharedPtr<Element>>::Failure(built.Error());
            if (status.Take(stage)) {
                rows.AddRow("Retainer Box", "RetainerBox (effect tint)", stage.Value());
            }
        }
    }

    // 캐시 영역: 하위 트리의 그리기 기록을 재사용한다.
    {
        auto cache = SharedPtr<CachePanel>::TryMake(allocator, allocator);
        auto block = MakeBlock(context, "Cached subtree", ThemeSlot::AccentPink);
        if (status.Take(cache) && status.Take(block)) {
            auto added = cache.Value()->TryAddChild(block.Value());
            auto stage = added ? MakeStage(context, cache.Value())
                               : Result<SharedPtr<Element>>::Failure(added.Error());
            if (status.Take(stage)) {
                rows.AddRow("Cache Panel", "CachePanel (cached paint list)", stage.Value());
            }
        }
    }

    // Viewport: 장면 텍스처(절차 생성 그라디언트) 위에 UI 자식을 올린다.
    {
        auto viewport = SharedPtr<Viewport>::TryMake(allocator, allocator);
        auto overlay = MakeTextButton(context, "UI over scene", DefaultButtonStyle());
        if (status.Take(viewport) && status.Take(overlay)) {
            // 버튼은 앵커 배치로 가운데 작게 두어 뒤 장면이 보이게 한다.
            auto canvas = SharedPtr<AnchorCanvas>::TryMake(allocator, allocator);
            auto placed = canvas ? canvas.Value()->TryAddSlot(
                                       overlay.Value(),
                                       {.anchorMin = {0.5f, 0.5f},
                                        .anchorMax = {0.5f, 0.5f},
                                        .offset = {0, 0, SceneButtonSize.x, SceneButtonSize.y}})
                                 : Result<void>::Failure(canvas.Error());
            auto built = placed
                             ? viewport.Value()->Construct({.content = canvas.Value(),
                                                            .scene = context.textures.Gradient()})
                             : Move(placed);
            auto stage = built ? MakeStage(context, viewport.Value())
                               : Result<SharedPtr<Element>>::Failure(built.Error());
            if (status.Take(stage)) {
                rows.AddRow("Viewport", "Viewport (scene texture + UI)", stage.Value());
            }
        }
    }

    if (!status.Ok()) {
        return Result<EffectsPage>::Failure(status.Error());
    }

    auto root = rows.Finish();
    if (!root) {
        return Result<EffectsPage>::Failure(root.Error());
    }

    return Result<EffectsPage>::Success({root.Value(), Move(grid.Value())});
}

} // namespace rocks::ui::gallery
