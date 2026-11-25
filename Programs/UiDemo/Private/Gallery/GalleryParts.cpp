#include "GalleryParts.h"

#include <Core/Diagnostics/Log.h>
#include <UiFramework/Frame.h>
#include <UiFramework/SizeBox.h>

namespace rocks::ui::gallery {

namespace {

inline constexpr LogCategory LogGalleryText{"UiDemo.Gallery"};

} // namespace

Result<SharedPtr<TextLabel>> MakeText(GalleryContext& context, StringView value,
                                      const FontStyle& font, Optional<ColorRef> color) noexcept {
    auto label = SharedPtr<TextLabel>::TryMake(context.allocator, context.allocator);
    if (!label) {
        return label;
    }

    if (color) {
        label.Value()->SetColor(*color);
    }

    if (auto set = context.text.SetText(*label.Value(), value, font); !set) {
        Log::Error(LogGalleryText, "Gallery text shaping failed ({Error}): {Text}",
                   StringView(ErrorCodeName(set.Error())), value);
        return Result<SharedPtr<TextLabel>>::Failure(set.Error());
    }

    return label;
}

Result<SharedPtr<Button>> MakeTextButton(GalleryContext& context, StringView label,
                                         const ButtonStyle& style) noexcept {
    auto text = MakeText(context, label);
    if (!text) {
        return Result<SharedPtr<Button>>::Failure(text.Error());
    }

    auto button = SharedPtr<Button>::TryMake(context.allocator, context.allocator);
    if (!button) {
        return button;
    }

    auto result = button.Value()->Construct({.content = text.Value(), .style = style});
    if (!result) {
        return Result<SharedPtr<Button>>::Failure(result.Error());
    }

    return button;
}

Result<SharedPtr<Element>> PadContent(GalleryContext& context, const SharedPtr<Element>& content,
                                      Vec4 padding) noexcept {
    auto frame = SharedPtr<Frame>::TryMake(context.allocator, context.allocator);
    if (!frame) {
        return Result<SharedPtr<Element>>::Failure(frame.Error());
    }

    Brush none;
    none.mode = BrushMode::None;
    auto result = frame.Value()->Construct({.content = content, .brush = none, .padding = padding});
    if (!result) {
        return Result<SharedPtr<Element>>::Failure(result.Error());
    }

    return Result<SharedPtr<Element>>::Success(frame.Value());
}

Result<SharedPtr<Element>> Sized(GalleryContext& context, const SharedPtr<Element>& content,
                                 Vec2 size) noexcept {
    auto box = SharedPtr<SizeBox>::TryMake(context.allocator, context.allocator);
    if (!box) {
        return Result<SharedPtr<Element>>::Failure(box.Error());
    }

    auto result = box.Value()->SetWidthOverride(size.x);
    if (result) {
        result = box.Value()->SetHeightOverride(size.y);
    }

    if (result && content) {
        result = box.Value()->TryAddChild(content);
    }

    if (!result) {
        return Result<SharedPtr<Element>>::Failure(result.Error());
    }

    return Result<SharedPtr<Element>>::Success(box.Value());
}

void BuildStatus::ReportFailure(StringView what, ErrorCode error) noexcept {
    Log::Error(LogGalleryText, "Gallery sample failed ({Error}): {Sample}",
               StringView(ErrorCodeName(error)), what.IsEmpty() ? StringView("(unnamed)") : what);
}

Result<SharedPtr<TextLabel>> MakeSectionTitle(GalleryContext& context, StringView title) noexcept {
    return MakeText(context, title, NormalFont, ColorRef::Specified(LabelColor));
}

} // namespace rocks::ui::gallery
