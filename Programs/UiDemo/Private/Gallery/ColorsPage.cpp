#include "ColorsPage.h"

#include <Core/Text/StringBuilder.h>
#include <UiFramework/Layout/Frame.h>
#include <UiFramework/Controls/Image.h>
#include <UiFramework/Layout/Overlay.h>
#include <UiFramework/Scrolling/ScrollView.h>
#include <UiFramework/Layout/SizeBox.h>
#include <UiFramework/Layout/VerticalStack.h>
#include <UiFramework/Layout/WrapStack.h>

namespace rocks::ui::gallery {

namespace {

// 견본 하나다(UE GenerateColorButton). inverted는 밝은 색 위에 어두운 글자, outline은 바탕과 같은
// 색이라 외곽선(Foldout)으로 경계를 보인다.
struct Swatch {
    const char* title;
    ThemeSlot slot;
    bool inverted = false;
    bool outline = false;
};

// UE 견본 크기 120x100에서 SBox 여백 8을 뺀 내부 크기, 둥근 반경 6, 글자 여백 12다.
constexpr Vec2 SwatchSize{104, 84};
constexpr float SwatchPadding = 8;
constexpr float SwatchRadius = 6;
constexpr float SwatchTextPadding = 12;
// 페이지 여백 48, 묶음 제목 여백 (8, 24, 8, 8)이다.
constexpr float PagePadding = 48;
constexpr Vec4 SectionPadding{8, 24, 8, 8};

constexpr Swatch BaseColors[] = {
    {"BLACK", ThemeSlot::Black},
    {"TITLE", ThemeSlot::Title},
    {"WINDOW\nBORDER", ThemeSlot::WindowBorder},
    {"FOLDOUT", ThemeSlot::Foldout},
    {"INPUT", ThemeSlot::Input},
    {"RECESSED", ThemeSlot::Recessed},
    {"BACKGROUND", ThemeSlot::Background, false, true},
    {"HEADER", ThemeSlot::Header},
    {"DROPDOWN", ThemeSlot::Dropdown},
    {"HOVER", ThemeSlot::Hover},
    {"HOVER2", ThemeSlot::Hover2},
    {"WHITE", ThemeSlot::White, true},
    {"WHITE25", ThemeSlot::White25},
    {"PRIMARY", ThemeSlot::Primary, true},
    {"PRIMARY\nHOVER", ThemeSlot::PrimaryHover, true},
    {"PRIMARY\nPRESS", ThemeSlot::PrimaryPress, true},
    {"SECONDARY", ThemeSlot::Secondary, true},
};

constexpr Swatch ForegroundColors[] = {
    {"FOREGROUND", ThemeSlot::Foreground},
    {"FOREGROUND\nHOVER", ThemeSlot::ForegroundHover, true},
    {"FOREGROUND\nINVERTED", ThemeSlot::ForegroundInverted},
    {"HIGHLIGHT", ThemeSlot::Highlight},
    {"FOREGROUND\nHEADER", ThemeSlot::ForegroundHeader, true},
    {"SELECT", ThemeSlot::Select, true},
    {"SELECT\nHOVER", ThemeSlot::SelectHover},
    {"SELECT\nPARENT", ThemeSlot::SelectParent},
    {"SELECT\nINACTIVE", ThemeSlot::SelectInactive},
};

constexpr Swatch AccentColors[] = {
    {"BLUE", ThemeSlot::AccentBlue, true},     {"PURPLE", ThemeSlot::AccentPurple},
    {"PINK", ThemeSlot::AccentPink, true},     {"RED", ThemeSlot::AccentRed, true},
    {"ORANGE", ThemeSlot::AccentOrange, true}, {"YELLOW", ThemeSlot::AccentYellow, true},
    {"GREEN", ThemeSlot::AccentGreen, true},   {"BROWN", ThemeSlot::AccentBrown},
    {"FOLDER", ThemeSlot::AccentFolder},       {"BLACK", ThemeSlot::AccentBlack, false, true},
    {"GRAY", ThemeSlot::AccentGray},           {"WHITE", ThemeSlot::AccentWhite, true},
};

// UE 갤러리에 없는 나머지 slot이다(Rocks 묶음 — 61 slot 전부를 보인다).
constexpr Swatch StatusColors[] = {
    {"WARNING", ThemeSlot::Warning, true},
    {"ERROR", ThemeSlot::Error, true},
    {"SUCCESS", ThemeSlot::Success, true},
    {"NOTIFICATIONS", ThemeSlot::Notifications},
};

constexpr Swatch OutlineColors[] = {
    {"PANEL", ThemeSlot::Panel},
    {"INPUT\nOUTLINE", ThemeSlot::InputOutline},
    {"DROPDOWN\nOUTLINE", ThemeSlot::DropdownOutline},
};

constexpr Swatch UserColors[] = {
    {"USER 1", ThemeSlot::User1},   {"USER 2", ThemeSlot::User2},   {"USER 3", ThemeSlot::User3},
    {"USER 4", ThemeSlot::User4},   {"USER 5", ThemeSlot::User5},   {"USER 6", ThemeSlot::User6},
    {"USER 7", ThemeSlot::User7},   {"USER 8", ThemeSlot::User8},   {"USER 9", ThemeSlot::User9},
    {"USER 10", ThemeSlot::User10}, {"USER 11", ThemeSlot::User11}, {"USER 12", ThemeSlot::User12},
    {"USER 13", ThemeSlot::User13}, {"USER 14", ThemeSlot::User14}, {"USER 15", ThemeSlot::User15},
    {"USER 16", ThemeSlot::User16},
};

struct Section {
    const char* title;
    Span<const Swatch> swatches;
};

// brush 견본 크기와 값이다.
constexpr Vec2 BrushSampleSize{104, 64};
constexpr float SampleRadius = 6;
constexpr Vec4 MixedRadii{0, 8, 16, 4};
constexpr float ThinOutline = 1;
constexpr float ThickOutline = 3;
constexpr Vec2 FrameImageSize{32, 32};
constexpr LinearColor SampleTint{1.0f, 0.7f, 0.7f, 1.0f};

// brush 견본 하나(그림 + 이름 + Rocks 생성식)다.
Result<SharedPtr<Element>> MakeBrushSample(GalleryContext& context, StringView name,
                                           StringView code, const Brush& brush) noexcept {
    Allocator& allocator = context.allocator;
    auto image = SharedPtr<Image>::TryMake(allocator, allocator);
    auto column = SharedPtr<VerticalStack>::TryMake(allocator, allocator);
    auto title = MakeText(context, name, SmallBoldFont);
    auto rocks = MakeText(context, code, SmallFont, ColorRef::FromTheme(ThemeSlot::White25));
    if (const ErrorCode error = FirstError(image, column, title, rocks); error != ErrorCode::None) {
        return Result<SharedPtr<Element>>::Failure(error);
    }

    auto result = image.Value()->SetBrush(brush);
    auto sized = result ? Sized(context, image.Value(), BrushSampleSize)
                        : Result<SharedPtr<Element>>::Failure(result.Error());
    if (!sized) {
        return sized;
    }

    result =
        column.Value()->TryAddSlot(sized.Value(), {.after = 4, .alignment = StackAlignment::Start});
    if (result) {
        result = column.Value()->TryAddSlot(title.Value(), {.alignment = StackAlignment::Start});
    }

    if (result) {
        result = column.Value()->TryAddSlot(rocks.Value(), {.alignment = StackAlignment::Start});
    }

    if (!result) {
        return Result<SharedPtr<Element>>::Failure(result.Error());
    }

    return Result<SharedPtr<Element>>::Success(column.Value());
}

// BRUSHES 묶음: 둥근 상자 변형, 단색, 이미지, 9분할 Box·Border, mirror, tint.
Result<SharedPtr<Element>> MakeBrushes(GalleryContext& context, const ThemeColors& theme) noexcept {
    auto wrap = SharedPtr<WrapStack>::TryMake(context.allocator, context.allocator);
    if (!wrap) {
        return Result<SharedPtr<Element>>::Failure(wrap.Error());
    }

    wrap.Value()->SetUseAllottedSize(true);

    const LinearColor fill = theme.Get(ThemeSlot::Secondary);
    const LinearColor outline = theme.Get(ThemeSlot::Primary);
    Brush solid;
    solid.mode = BrushMode::Box;
    solid.tint = theme.Get(ThemeSlot::AccentBlue);
    Brush image;
    image.resource = context.textures.Checker();
    Brush box;
    box.mode = BrushMode::Box;
    box.resource = context.textures.Frame();
    box.imageSize = FrameImageSize;
    box.margin = {GalleryTextures::FrameMargin, GalleryTextures::FrameMargin,
                  GalleryTextures::FrameMargin, GalleryTextures::FrameMargin};
    Brush border = box;
    border.mode = BrushMode::Border;
    Brush gradient;
    gradient.resource = context.textures.Gradient();
    Brush mirrored = gradient;
    mirrored.mirrorX = true;
    Brush tinted = gradient;
    tinted.tint = SampleTint;

    struct Sample {
        const char* name;
        const char* code;
        Brush brush;
    };

    const Sample samples[] = {
        {"ROUNDED", "Brush::RoundedBox(c, 6)", Brush::RoundedBox(fill, SampleRadius)},
        {"FOUR RADII", "RoundedBox(c, {0,8,16,4}, o, 1)",
         Brush::RoundedBox(fill, MixedRadii, outline, ThinOutline)},
        {"PILL", "Brush::PillBox(c)", Brush::PillBox(fill)},
        {"OUTLINE 1", "RoundedBox(c, 6, o, 1)",
         Brush::RoundedBox(theme.Get(ThemeSlot::Recessed), SampleRadius, outline, ThinOutline)},
        {"OUTLINE 3", "RoundedBox(c, 6, o, 3)",
         Brush::RoundedBox(theme.Get(ThemeSlot::Recessed), SampleRadius, outline, ThickOutline)},
        {"COLOR", "BrushMode::Box, no image", solid},
        {"IMAGE", "BrushMode::Image", image},
        {"BOX 9-SLICE", "Box + margin 0.25", box},
        {"BORDER 9-SLICE", "Border (center empty)", border},
        {"MIRROR X", "Brush::mirrorX", mirrored},
        {"TINT", "Brush::tint", tinted},
    };
    for (const Sample& sample : samples) {
        auto made = MakeBrushSample(context, sample.name, sample.code, sample.brush);
        if (!made) {
            return made;
        }

        auto added = wrap.Value()->TryAddSlot(
            made.Value(), {.placement = {.padding = {SwatchPadding, SwatchPadding, SwatchPadding,
                                                     SwatchPadding}}});
        if (!added) {
            return Result<SharedPtr<Element>>::Failure(added.Error());
        }
    }

    return Result<SharedPtr<Element>>::Success(wrap.Value());
}

Result<SharedPtr<Element>> MakeSwatch(GalleryContext& context, const ThemeColors& theme,
                                      const Swatch& swatch) noexcept {
    Allocator& allocator = context.allocator;
    const LinearColor fill = theme.Get(swatch.slot);
    // 글자는 UE처럼 흰색(뒤집으면 Background)이고, 16진 값은 sRGB RRGGBBAA다(FColor::ToHex).
    const ColorRef ink =
        ColorRef::FromTheme(swatch.inverted ? ThemeSlot::Background : ThemeSlot::White);
    const Color encoded = fill.ToSRGB();
    StringBuilder<> hex(allocator);
    auto formatted =
        hex.TryAppendFormat("{:02X}{:02X}{:02X}{:02X}", encoded.r, encoded.g, encoded.b, encoded.a);
    if (!formatted) {
        return Result<SharedPtr<Element>>::Failure(formatted.Error());
    }

    auto title = MakeText(context, swatch.title, SmallBoldFont, ink);
    auto value = MakeText(context, hex.View(), SmallFont, ink);
    auto box = SharedPtr<SizeBox>::TryMake(allocator, allocator);
    auto layers = SharedPtr<Overlay>::TryMake(allocator, allocator);
    auto face = SharedPtr<Frame>::TryMake(allocator, allocator);
    auto text = SharedPtr<VerticalStack>::TryMake(allocator, allocator);
    if (const ErrorCode error = FirstError(title, value, box, layers, face, text);
        error != ErrorCode::None) {
        return Result<SharedPtr<Element>>::Failure(error);
    }

    const LinearColor outline = swatch.outline ? theme.Get(ThemeSlot::Foldout) : LinearColor{};
    auto result = face.Value()->SetBrush(
        Brush::RoundedBox(fill, SwatchRadius, outline, swatch.outline ? 1.0f : 0.0f));
    if (result) {
        // UE 견본처럼 이름과 16진 값을 왼쪽 아래에 쌓는다(이름 칸이 남은 높이를 받고 아래에
        // 붙는다).
        result = text.Value()->TryAddSlot(title.Value(), {.rule = StackSizeRule::Fill,
                                                          .before = SwatchTextPadding,
                                                          .crossBefore = SwatchTextPadding,
                                                          .crossAfter = SwatchTextPadding,
                                                          .alignment = StackAlignment::Start,
                                                          .mainAlignment = StackAlignment::End});
    }

    if (result) {
        result = text.Value()->TryAddSlot(value.Value(), {.after = SwatchTextPadding,
                                                          .crossBefore = SwatchTextPadding,
                                                          .crossAfter = SwatchTextPadding,
                                                          .alignment = StackAlignment::Start});
    }

    if (result) {
        result = layers.Value()->TryAddSlot(face.Value(), 0);
    }

    if (result) {
        result = layers.Value()->TryAddSlot(text.Value(), 1);
    }

    if (result) {
        result = box.Value()->SetWidthOverride(SwatchSize.x);
    }

    if (result) {
        result = box.Value()->SetHeightOverride(SwatchSize.y);
    }

    if (result) {
        result = box.Value()->TryAddChild(layers.Value());
    }

    if (!result) {
        return Result<SharedPtr<Element>>::Failure(result.Error());
    }

    return Result<SharedPtr<Element>>::Success(box.Value());
}

} // namespace

Result<SharedPtr<Element>> BuildColorsPage(GalleryContext& context,
                                           const ThemeColors& theme) noexcept {
    Allocator& allocator = context.allocator;
    auto page = SharedPtr<VerticalStack>::TryMake(allocator, allocator);
    auto scroll = SharedPtr<ScrollView>::TryMake(allocator, allocator);
    if (const ErrorCode error = FirstError(page, scroll); error != ErrorCode::None) {
        return Result<SharedPtr<Element>>::Failure(error);
    }

    const Section sections[] = {
        {"BASE COLORS", BaseColors},       {"FOREGROUND COLORS", ForegroundColors},
        {"ACCENT COLORS", AccentColors},   {"STATUS COLORS", StatusColors},
        {"OUTLINE COLORS", OutlineColors}, {"USER COLORS", UserColors}};
    Result<void> result = Result<void>::Success();
    for (const Section& section : sections) {
        auto title = MakeText(context, section.title, NormalFont, ColorRef::Specified(LabelColor));
        auto wrap = SharedPtr<WrapStack>::TryMake(allocator, allocator);
        if (const ErrorCode error = FirstError(title, wrap); error != ErrorCode::None) {
            return Result<SharedPtr<Element>>::Failure(error);
        }

        // 견본 줄은 할당 폭에서 줄을 바꾼다(UE SUniformWrapPanel).
        wrap.Value()->SetUseAllottedSize(true);

        for (const Swatch& swatch : section.swatches) {
            auto made = MakeSwatch(context, theme, swatch);
            if (!made) {
                return made;
            }

            result = wrap.Value()->TryAddSlot(
                made.Value(), {.placement = {.padding = {SwatchPadding, SwatchPadding,
                                                         SwatchPadding, SwatchPadding}}});
            if (!result) {
                return Result<SharedPtr<Element>>::Failure(result.Error());
            }
        }

        result = page.Value()->TryAddSlot(title.Value(), {.before = SectionPadding.y,
                                                          .after = SectionPadding.w,
                                                          .crossBefore = SectionPadding.x,
                                                          .crossAfter = SectionPadding.z,
                                                          .alignment = StackAlignment::Start});
        if (result) {
            result = page.Value()->TryAddSlot(wrap.Value(), {});
        }

        if (!result) {
            return Result<SharedPtr<Element>>::Failure(result.Error());
        }
    }

    // 테마 slot 다음은 brush 견본이다.
    auto brushes = MakeBrushes(context, theme);
    auto brushTitle = MakeSectionTitle(context, "BRUSHES");
    if (!brushes || !brushTitle) {
        return Result<SharedPtr<Element>>::Failure(!brushes ? brushes.Error() : brushTitle.Error());
    }

    result = page.Value()->TryAddSlot(brushTitle.Value(), {.before = SectionPadding.y,
                                                           .after = SectionPadding.w,
                                                           .crossBefore = SectionPadding.x,
                                                           .crossAfter = SectionPadding.z,
                                                           .alignment = StackAlignment::Start});
    if (result) {
        result = page.Value()->TryAddSlot(brushes.Value(), {});
    }

    if (!result) {
        return Result<SharedPtr<Element>>::Failure(result.Error());
    }

    auto padded =
        PadContent(context, page.Value(), {PagePadding, PagePadding, PagePadding, PagePadding});
    if (!padded) {
        return padded;
    }

    if (auto added = scroll.Value()->TryAddChild(padded.Value()); !added) {
        return Result<SharedPtr<Element>>::Failure(added.Error());
    }

    return Result<SharedPtr<Element>>::Success(scroll.Value());
}

} // namespace rocks::ui::gallery
