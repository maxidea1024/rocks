#include <UiCore/ThemeColors.h>
#include <Core/Text/Json.h>

namespace rocks::ui {
namespace {
struct SlotDefault {
    StringView key;
    uint32 rgba;
};
// StyleColors.cpp의 다크 테마 값을 sRGB 바이트로 보관한다. 알파만 선형으로 해석한다.
constexpr SlotDefault Defaults[] = {{"Black", 0x000000FFu},
                                    {"Title", 0x151515FFu},
                                    {"WindowBorder", 0x0F0F0FFFu},
                                    {"Foldout", 0x0F0F0FFFu},
                                    {"Input", 0x0F0F0FFFu},
                                    {"InputOutline", 0x383838FFu},
                                    {"Recessed", 0x1A1A1AFFu},
                                    {"Background", 0x151515FFu},
                                    {"Panel", 0x242424FFu},
                                    {"Header", 0x2F2F2FFFu},
                                    {"Dropdown", 0x383838FFu},
                                    {"DropdownOutline", 0x4C4C4CFFu},
                                    {"Hover", 0x575757FFu},
                                    {"Hover2", 0x808080FFu},
                                    {"White", 0xFFFFFFFFu},
                                    {"White25", 0xFFFFFF40u},
                                    {"Highlight", 0x0070E0FFu},
                                    {"Primary", 0x0070E0FFu},
                                    {"PrimaryHover", 0x0E86FFFFu},
                                    {"PrimaryPress", 0x0050A0FFu},
                                    {"Secondary", 0x383838FFu},
                                    {"Foreground", 0xC0C0C0FFu},
                                    {"ForegroundHover", 0xFFFFFFFFu},
                                    {"ForegroundInverted", 0x0F0F0FFFu},
                                    {"ForegroundHeader", 0xC8C8C8FFu},
                                    {"Select", 0x0070E0FFu},
                                    {"SelectInactive", 0x40576FFFu},
                                    {"SelectParent", 0x2C323AFFu},
                                    {"SelectHover", 0x242424FFu},
                                    {"Notifications", 0x464B50FFu},
                                    {"AccentBlue", 0x26BBFFFFu},
                                    {"AccentPurple", 0xA139BFFFu},
                                    {"AccentPink", 0xFF729CFFu},
                                    {"AccentRed", 0xFF4040FFu},
                                    {"AccentOrange", 0xFE9B07FFu},
                                    {"AccentYellow", 0xFFDC1AFFu},
                                    {"AccentGreen", 0x8BC24AFFu},
                                    {"AccentBrown", 0x804D39FFu},
                                    {"AccentBlack", 0x242424FFu},
                                    {"AccentGray", 0x808080FFu},
                                    {"AccentWhite", 0xFFFFFFFFu},
                                    {"AccentFolder", 0xB68F55FFu},
                                    {"Warning", 0xFFB800FFu},
                                    {"Error", 0xEF3535FFu},
                                    {"Success", 0x1FE44BFFu},
                                    {"User1", 0x00000000u},
                                    {"User2", 0x00000000u},
                                    {"User3", 0x00000000u},
                                    {"User4", 0x00000000u},
                                    {"User5", 0x00000000u},
                                    {"User6", 0x00000000u},
                                    {"User7", 0x00000000u},
                                    {"User8", 0x00000000u},
                                    {"User9", 0x00000000u},
                                    {"User10", 0x00000000u},
                                    {"User11", 0x00000000u},
                                    {"User12", 0x00000000u},
                                    {"User13", 0x00000000u},
                                    {"User14", 0x00000000u},
                                    {"User15", 0x00000000u},
                                    {"User16", 0x00000000u}};
static_assert(sizeof(Defaults) / sizeof(Defaults[0]) == static_cast<usize>(ThemeSlot::Count));
} // namespace
ThemeColors::ThemeColors() noexcept {
    for (usize i = 0; i < static_cast<usize>(ThemeSlot::Count); ++i) {
        values_[i] = LinearColor::FromSRGB(Color::FromPackedRGBA(Defaults[i].rgba));
    }
}
const ThemeColors& DefaultThemeColors() noexcept {
    static const ThemeColors theme;
    return theme;
}

LinearColor ThemeColors::Get(ThemeSlot slot) const noexcept {
    return slot < ThemeSlot::Count ? values_[static_cast<usize>(slot)] : LinearColor{};
}
Result<void> ThemeColors::Load(Allocator& allocator, StringView engine, StringView project,
                               StringView user) noexcept {
    ThemeColors pending;
    const StringView layers[] = {engine, project, user};
    for (auto layer : layers) {
        if (layer.IsEmpty()) {
            continue;
        }
        JsonDocument doc(allocator);
        const auto parsed = doc.Parse(layer);
        if (!parsed.Succeeded()) {
            return Result<void>::Failure(parsed.code == JsonErrorCode::OutOfMemory
                                             ? ErrorCode::OutOfMemory
                                             : ErrorCode::InvalidArgument);
        }
        const auto colors = doc.Root().Find("colors");
        if (!colors.IsObject()) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
        for (usize i = 0; i < colors.Size(); ++i) {
            const auto key = colors.KeyAt(i);
            const auto value = colors.ValueAt(i);
            Color srgb;
            if (!value.IsString() || !Color::TryParseHex(value.GetString(), srgb)) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }
            usize slot = 0;
            while (slot < static_cast<usize>(ThemeSlot::Count) && Defaults[slot].key != key) {
                ++slot;
            }
            if (slot == static_cast<usize>(ThemeSlot::Count)) {
                return Result<void>::Failure(ErrorCode::NotFound);
            }
            pending.values_[slot] = LinearColor::FromSRGB(srgb);
        }
    }
    *this = pending;
    return Result<void>::Success();
}
} // namespace rocks::ui
