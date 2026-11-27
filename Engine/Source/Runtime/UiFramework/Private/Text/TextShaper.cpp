#include <UiFramework/Text/TextShaper.h>

namespace rocks::ui {

Result<SharedPtr<TextLabel>> TextShaper::ShapeEllipsis(Allocator& allocator,
                                                       float fontSize) noexcept {
    auto marker = SharedPtr<TextLabel>::TryMake(allocator, allocator);
    if (!marker) {
        return marker;
    }

    auto text = Text::Invariant(allocator, "…");
    if (!text) {
        return Result<SharedPtr<TextLabel>>::Failure(text.Error());
    }

    auto shaped = Shape(*marker.Value(), text.Value(), fontSize);
    if (!shaped) {
        return Result<SharedPtr<TextLabel>>::Failure(shaped.Error());
    }

    if (marker.Value()->GetText() != "…" || marker.Value()->Layout().Lines().Size() != 1 ||
        marker.Value()->Layout().Lines()[0].width <= 0) {
        return Result<SharedPtr<TextLabel>>::Failure(ErrorCode::InvalidArgument);
    }

    return marker;
}

} // namespace rocks::ui
