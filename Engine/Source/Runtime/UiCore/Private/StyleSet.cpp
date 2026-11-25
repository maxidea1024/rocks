#include <UiCore/StyleSet.h>

namespace rocks::ui {

StyleSet::StyleSet(Allocator& allocator) noexcept
    : allocator_(allocator), buttons_(allocator), texts_(allocator) {
}

template <typename T>
Result<void> StyleSet::Set(Allocator& allocator, Array<Entry<T>>& entries, StringView name,
                           const T& style) noexcept {
    if (name.IsEmpty()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    for (auto& entry : entries) {
        if (entry.name.View() == name) {
            entry.style = style;
            return Result<void>::Success();
        }
    }

    String key(allocator);
    if (auto assigned = key.TryAssign(name); !assigned) {
        return assigned;
    }
    return entries.TryPushBack(Entry<T>{Move(key), style});
}

template <typename T>
const T* StyleSet::Find(const Array<Entry<T>>& entries, StringView name) noexcept {
    for (const auto& entry : entries) {
        if (entry.name.View() == name) {
            return &entry.style;
        }
    }
    return nullptr;
}

Result<void> StyleSet::SetButton(StringView name, const ButtonStyle& style) noexcept {
    return Set(allocator_, buttons_, name, style);
}

Result<void> StyleSet::SetText(StringView name, const TextStyle& style) noexcept {
    return Set(allocator_, texts_, name, style);
}

const ButtonStyle* StyleSet::FindButton(StringView name) const noexcept {
    return Find(buttons_, name);
}

const TextStyle* StyleSet::FindText(StringView name) const noexcept {
    return Find(texts_, name);
}

const ButtonStyle& StyleSet::GetButton(StringView name) const noexcept {
    const ButtonStyle* style = FindButton(name);
    return style ? *style : DefaultButtonStyle();
}

const TextStyle& StyleSet::GetText(StringView name) const noexcept {
    const TextStyle* style = FindText(name);
    return style ? *style : NormalTextStyle();
}

Result<void> AddCoreStyles(StyleSet& styles) noexcept {
    // 글꼴은 StarshipCoreStyle.cpp 34–40행(Small Regular 8, NormalBold Bold 10, Large Regular 14),
    // 색은 모두 NormalText처럼 상속 전경색이다(1252–1258행).
    const TextStyle small{{Typeface::Regular, SmallFontSize}, {ColorSource::Foreground}};
    const TextStyle primaryButton{{Typeface::Bold, NormalFontSize}, {ColorSource::Foreground}};
    const TextStyle large{{Typeface::Regular, LargeFontSize}, {ColorSource::Foreground}};
    Result<void> added = styles.SetButton("Button", DefaultButtonStyle());
    if (added) {
        added = styles.SetButton("PrimaryButton", PrimaryButtonStyle());
    }
    if (added) {
        added = styles.SetButton("SimpleButton", SimpleButtonStyle());
    }
    if (added) {
        added = styles.SetText("NormalText", NormalTextStyle());
    }
    if (added) {
        added = styles.SetText("SmallText", small);
    }
    if (added) {
        added = styles.SetText("PrimaryButtonText", primaryButton);
    }
    if (added) {
        added = styles.SetText("Text.Large", large);
    }
    return added;
}

} // namespace rocks::ui
