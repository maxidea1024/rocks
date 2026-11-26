#include "TextPage.h"

#include <Core/Text/StringBuilder.h>
#include <UiFramework/Grid.h>
#include <UiFramework/HorizontalStack.h>
#include <UiFramework/ScrollView.h>
#include <UiFramework/VerticalStack.h>

namespace rocks::ui::gallery {

namespace {

// UE 페이지 여백 48, 열 좌우 여백 64, 설명 위 64·아래 4, 견본 위아래 4다(ConstructTextGallery).
constexpr float PagePadding = 48;
// UE는 열 좌우 64이지만 Rocks는 넷째 열(문자 체계)을 더해 1280 폭에 맞게 줄인다.
constexpr float ColumnPadding = 24;
constexpr float DescriptionBefore = 64;
constexpr float SampleSpacing = 4;
// 아래 묶음의 제목 위 여백과 표 칸 여백이다.
constexpr float SectionBefore = 48;
constexpr Vec4 CellPadding{0, 6, 32, 6};
// 문단 견본 폭이다.
constexpr float ParagraphWidth = 320;
constexpr float EllipsisWidth = 240;
// UE Menu.Heading 자간(1/1000 em, StarshipCoreStyle.cpp 2532–2538행)이다.
constexpr int32 MenuHeadingLetterSpacing = 250;
// 그림자 견본의 오프셋이다.
constexpr Vec2 ShadowOffset{1, 1};
// 표의 열 수(이름·글꼴 설명·견본)다.
constexpr uint32 TableColumns = 3;

// UE 원래 텍스트 탭의 한 견본이다. text가 null이면 key로 문자열 표에서 찾는다.
struct UeSample {
    const char* description;
    const char* text;
    const char* key;
    FontStyle font;
};

constexpr UeSample RegularSamples[] = {
    {"ROBOTO 10 SLATE    [ ROBOTO 13 FIGMA ]",
     "Normal Text\n\nThe quick brown fox jumps over the lazy dog.\n\nNORMAL TEXT CAPITAL", nullptr,
     NormalFont},
    {"ROBOTO 8 SLATE   [ ROBOTO 11 FIGMA ]",
     "Small Text\n\nThe quick brown fox jumps over the lazy dog.\n\nSMALL TEXT CAPITAL", nullptr,
     SmallFont},
};

constexpr UeSample BoldSamples[] = {
    {"ROBOTO BOLD 10 SLATE   [ ROBOTO 13 FIGMA ]",
     "Normal Text Bold \n\nThe quick brown fox jumps over the lazy dog.\n\nNORMAL TEXT BOLD "
     "CAPITAL",
     nullptr, NormalBoldFont},
    {"ROBOTO BOLD 8 SLATE   [ ROBOTO 11 FIGMA ]",
     "Small Text Bold \n\nThe quick brown fox jumps over the lazy dog.\n\nSMALL TEXT BOLD CAPITAL",
     nullptr, SmallBoldFont},
};

// UE 셋째 열(SUB-TYPEFACES - MATH·이모지, 591–605행)이다.
constexpr UeSample SubTypefaceSamples[] = {
    {"SUB-TYPEFACES - MATH", nullptr, "sample.math", NormalFont},
    {"SUB-TYPEFACES - EMOJI", nullptr, "sample.emoji", NormalFont},
};

// Rocks 데모 글꼴의 문자 체계 열이다(넷째 열).
constexpr UeSample ScriptSamples[] = {
    {"HANGUL (NANUM GOTHIC)", nullptr, "pangram.hangul", NormalFont},
    {"ARABIC (NOTO NASKH, RTL)", nullptr, "sample.arabic", NormalFont},
    {"BIDI", nullptr, "sample.bidi", NormalFont},
};

// 글꼴 지정 한 행이다(14장 2절).
struct FontRow {
    const char* key;
    FontStyle font;
    ColorRef color;
    const char* sampleKey;
};

constexpr ColorRef Inherit{ColorSource::Foreground};
constexpr ColorRef Subdued{ColorSource::SubduedForeground};
constexpr ColorRef White25{ColorSource::Theme, ThemeSlot::White25};
constexpr ColorRef White{ColorSource::Theme, ThemeSlot::White};

constexpr FontRow CoreFonts[] = {
    {"Normal (NormalFont)", {Typeface::Regular, 10}, Inherit, "pangram.latin"},
    {"NormalItalic", {Typeface::Italic, 10}, Inherit, "pangram.latin"},
    {"NormalBold (NormalFontBold)", {Typeface::Bold, 10}, Inherit, "pangram.latin"},
    {"NormalBoldItalic", {Typeface::BoldItalic, 10}, Inherit, "pangram.latin"},
    {"Small (SmallFont)", {Typeface::Regular, 8}, Inherit, "pangram.latin"},
    {"SmallBold (SmallFontBold)", {Typeface::Bold, 8}, Inherit, "pangram.latin"},
    {"Large (Font.Large)", {Typeface::Regular, 14}, Inherit, "pangram.latin"},
    {"LargeBold (Font.Large.Bold)", {Typeface::Bold, 14}, Inherit, "pangram.latin"},
};

constexpr FontRow HeadingFonts[] = {
    {"HeadingMedium", {Typeface::BoldCondensed, 33}, Inherit, "pangram.latin.caps"},
    {"HeadingSmall", {Typeface::BoldCondensed, 21}, Inherit, "pangram.latin.caps"},
    {"HeadingExtraSmall", {Typeface::BoldCondensed, 15}, Inherit, "pangram.latin.caps"},
};

constexpr FontRow PurposeFonts[] = {
    {"ToolTip.Font", {Typeface::Regular, 9}, Inherit, "pangram.latin"},
    {"HintText", {Typeface::Italic, 8}, Subdued, "pangram.latin"},
    {"MonospacedText", {Typeface::Mono, 10}, Inherit, "sample.mono"},
    {"Log.Normal", {Typeface::Mono, 9}, Inherit, "sample.log"},
    {"Menu.Heading", {Typeface::Bold, 7, MenuHeadingLetterSpacing}, White25, "pangram.latin.caps"},
    {"Menu.Keybinding", {Typeface::Regular, 8}, Inherit, "pangram.latin"},
    {"PrimaryButtonText", {Typeface::Bold, 10}, Inherit, "pangram.latin"},
    {"NotificationList.FontBold", {Typeface::Bold, 12}, Inherit, "pangram.latin"},
    {"NotificationList.FontLight", {Typeface::Light, 12}, Inherit, "pangram.latin"},
    {"PropertyWindow.NormalFont", {Typeface::Regular, 8}, Inherit, "pangram.latin"},
    {"PropertyWindow.BoldFont", {Typeface::Bold, 8}, Inherit, "pangram.latin"},
    {"DetailsView.CategoryFontStyle", {Typeface::Bold, 8}, Inherit, "pangram.latin"},
    {"DetailsView.NameTextBlockStyle", {Typeface::Regular, 11}, White, "pangram.latin"},
};

constexpr FontRow TypefaceFonts[] = {
    {"Regular", {Typeface::Regular, 14}, Inherit, "pangram.latin"},
    {"Medium", {Typeface::Medium, 14}, Inherit, "pangram.latin"},
    {"Light", {Typeface::Light, 14}, Inherit, "pangram.latin"},
    {"VeryLight", {Typeface::VeryLight, 14}, Inherit, "pangram.latin"},
    {"Bold", {Typeface::Bold, 14}, Inherit, "pangram.latin"},
    {"Black", {Typeface::Black, 14}, Inherit, "pangram.latin"},
    {"Italic", {Typeface::Italic, 14}, Inherit, "pangram.latin"},
    {"BoldItalic", {Typeface::BoldItalic, 14}, Inherit, "pangram.latin"},
    {"BlackItalic", {Typeface::BlackItalic, 14}, Inherit, "pangram.latin"},
    {"BoldCondensed", {Typeface::BoldCondensed, 14}, Inherit, "pangram.latin"},
    {"BoldCondensedItalic", {Typeface::BoldCondensedItalic, 14}, Inherit, "pangram.latin"},
    {"Mono", {Typeface::Mono, 14}, Inherit, "pangram.latin"},
};

constexpr FontRow ScriptFonts[] = {
    {"Latin (Roboto)", {Typeface::Regular, 14}, Inherit, "pangram.latin"},
    {"Hangul (NanumGothic)", {Typeface::Regular, 14}, Inherit, "sample.hangul"},
    {"Arabic (Noto Naskh, RTL)", {Typeface::Regular, 14}, Inherit, "sample.arabic"},
    {"Mixed direction (BiDi)", {Typeface::Regular, 14}, Inherit, "sample.bidi"},
    {"Emoji (last resort)", {Typeface::Regular, 14}, Inherit, "sample.emoji"},
    {"Math symbols", {Typeface::Regular, 14}, Inherit, "sample.math"},
};

struct FontSection {
    const char* title;
    Span<const FontRow> rows;
};

// 글꼴 지정 설명(예 "BoldCondensed 33")을 만든다.
Result<SharedPtr<TextLabel>> MakeSpec(GalleryContext& context, const FontStyle& font) noexcept {
    StringBuilder<> spec(context.allocator);
    auto formatted =
        spec.TryAppendFormat("{} {}", TypefaceName(font.typeface), static_cast<int32>(font.size));
    if (formatted && font.letterSpacing != 0) {
        formatted = spec.TryAppendFormat(", spacing {}", font.letterSpacing);
    }

    if (!formatted) {
        return Result<SharedPtr<TextLabel>>::Failure(formatted.Error());
    }

    return MakeText(context, spec.View(), SmallFont, ColorRef::Specified(LabelColor));
}

Result<SharedPtr<Element>> MakeUeColumn(GalleryContext& context,
                                        Span<const UeSample> samples) noexcept {
    auto stack = SharedPtr<VerticalStack>::TryMake(context.allocator, context.allocator);
    if (!stack) {
        return Result<SharedPtr<Element>>::Failure(stack.Error());
    }

    for (const UeSample& sample : samples) {
        const StringView value =
            sample.text ? StringView(sample.text) : context.strings.Get(sample.key);
        auto description = MakeSectionTitle(context, sample.description);
        auto text = MakeText(context, value, sample.font);
        if (!description || !text) {
            return Result<SharedPtr<Element>>::Failure(!description ? description.Error()
                                                                    : text.Error());
        }

        auto added =
            stack.Value()->TryAddSlot(description.Value(), {.before = DescriptionBefore,
                                                            .after = SampleSpacing,
                                                            .alignment = StackAlignment::Start});
        if (added) {
            added = stack.Value()->TryAddSlot(text.Value(), {.before = SampleSpacing,
                                                             .after = SampleSpacing,
                                                             .alignment = StackAlignment::Start});
        }

        if (!added) {
            return Result<SharedPtr<Element>>::Failure(added.Error());
        }
    }

    return Result<SharedPtr<Element>>::Success(stack.Value());
}

// 행 [이름][글꼴 설명][견본]을 격자에 더한다.
Result<void> AddRow(GalleryContext& context, Grid& grid, uint32 row, StringView name,
                    const SharedPtr<Element>& spec, const SharedPtr<Element>& sample) noexcept {
    auto label = MakeText(context, name, NormalFont, ColorRef::Specified(LabelColor));
    if (!label) {
        return Result<void>::Failure(label.Error());
    }

    const SharedPtr<Element> cells[TableColumns] = {label.Value(), spec, sample};
    for (uint32 column = 0; column < TableColumns; ++column) {
        auto added = grid.TryAddSlot(cells[column], {.column = column,
                                                     .row = row,
                                                     .padding = CellPadding,
                                                     .horizontal = StackAlignment::Start,
                                                     .vertical = StackAlignment::Center});
        if (!added) {
            return added;
        }
    }

    return Result<void>::Success();
}

Result<SharedPtr<Element>> MakeFontTable(GalleryContext& context,
                                         Span<const FontRow> rows) noexcept {
    auto grid = SharedPtr<Grid>::TryMake(context.allocator, context.allocator);
    if (!grid) {
        return Result<SharedPtr<Element>>::Failure(grid.Error());
    }

    for (usize i = 0; i < rows.Size(); ++i) {
        const FontRow& row = rows[i];
        auto spec = MakeSpec(context, row.font);
        auto sample = MakeText(context, context.strings.Get(row.sampleKey), row.font, row.color);
        if (!spec || !sample) {
            return Result<SharedPtr<Element>>::Failure(!spec ? spec.Error() : sample.Error());
        }

        auto added = AddRow(context, *grid.Value(), static_cast<uint32>(i), row.key, spec.Value(),
                            sample.Value());
        if (!added) {
            return Result<SharedPtr<Element>>::Failure(added.Error());
        }
    }

    return Result<SharedPtr<Element>>::Success(grid.Value());
}

// 문단 기능 견본이다(줄바꿈·정렬·말줄임·밑줄·취소선·그림자).
Result<SharedPtr<Element>> MakeParagraphTable(GalleryContext& context) noexcept {
    auto grid = SharedPtr<Grid>::TryMake(context.allocator, context.allocator);
    if (!grid) {
        return Result<SharedPtr<Element>>::Failure(grid.Error());
    }

    uint32 row = 0;
    BuildStatus status;
    const auto add = [&](StringView name, StringView usage,
                         Result<SharedPtr<TextLabel>> sample) noexcept {
        if (!status.Take(sample)) {
            return;
        }

        auto spec = MakeText(context, usage, SmallFont, ColorRef::Specified(LabelColor));
        if (!status.Take(spec)) {
            return;
        }

        auto added = AddRow(context, *grid.Value(), row++, name, spec.Value(), sample.Value());
        status.Take(added);
    };
    const StringView wrap = context.strings.Get("sample.wrap");
    const auto aligned = [&](text::TextAlignment alignment) noexcept {
        auto label = MakeText(context, wrap);
        if (label) {
            auto set = label.Value()->SetLayoutOptions(ParagraphWidth, alignment, true);
            if (!set) {
                return Result<SharedPtr<TextLabel>>::Failure(set.Error());
            }
        }

        return label;
    };
    add("Wrap - left", "SetLayoutOptions(320, Left, autoWrap)", aligned(text::TextAlignment::Left));
    add("Wrap - center", "SetLayoutOptions(320, Center, autoWrap)",
        aligned(text::TextAlignment::Center));
    add("Wrap - right", "SetLayoutOptions(320, Right, autoWrap)",
        aligned(text::TextAlignment::Right));

    {
        auto label = MakeText(context, context.strings.Get("sample.ellipsis"));
        auto marker = MakeText(context, "...");
        if (status.Take(label) && status.Take(marker)) {
            label.Value()->SetEllipsisMarker(marker.Value());
            auto set =
                label.Value()->SetLayoutOptions(EllipsisWidth, text::TextAlignment::Left, false);
            if (status.Take(set)) {
                add("Ellipsis", "SetEllipsisMarker", Move(label));
            }
        }
    }

    {
        auto underline = MakeText(context, context.strings.Get("pangram.latin"));
        auto strike = MakeText(context, context.strings.Get("pangram.latin"));
        auto shadow = MakeText(context, context.strings.Get("pangram.latin"), LargeFont);
        if (status.Take(underline) && status.Take(strike) && status.Take(shadow)) {
            underline.Value()->SetDecoration(true, false);
            strike.Value()->SetDecoration(false, true);
            auto set = shadow.Value()->SetParagraphStyle({}, ShadowOffset, {0, 0, 0, 1});
            add("Underline", "SetDecoration(underline)", Move(underline));
            add("Strikethrough", "SetDecoration(strike)", Move(strike));
            if (status.Take(set)) {
                add("Shadow", "SetParagraphStyle(shadow)", Move(shadow));
            }
        }
    }

    if (!status.Ok()) {
        return Result<SharedPtr<Element>>::Failure(status.Error());
    }

    return Result<SharedPtr<Element>>::Success(grid.Value());
}

} // namespace

Result<SharedPtr<Element>> BuildTextPage(GalleryContext& context) noexcept {
    Allocator& allocator = context.allocator;
    auto page = SharedPtr<VerticalStack>::TryMake(allocator, allocator);
    auto columns = SharedPtr<HorizontalStack>::TryMake(allocator, allocator);
    auto scroll = SharedPtr<ScrollView>::TryMake(allocator, allocator);
    if (const ErrorCode error = FirstError(page, columns, scroll); error != ErrorCode::None) {
        return Result<SharedPtr<Element>>::Failure(error);
    }

    // UE 원래 텍스트 탭의 세 열 + Rocks 문자 체계 열.
    const Span<const UeSample> ueColumns[] = {RegularSamples, BoldSamples, SubTypefaceSamples,
                                              ScriptSamples};
    for (const Span<const UeSample>& samples : ueColumns) {
        auto column = MakeUeColumn(context, samples);
        if (!column) {
            return column;
        }

        auto added = columns.Value()->TryAddSlot(
            column.Value(),
            {.before = ColumnPadding, .after = ColumnPadding, .alignment = StackAlignment::Start});
        if (!added) {
            return Result<SharedPtr<Element>>::Failure(added.Error());
        }
    }

    auto added = page.Value()->TryAddSlot(columns.Value(), {.alignment = StackAlignment::Start});
    if (!added) {
        return Result<SharedPtr<Element>>::Failure(added.Error());
    }

    const auto section = [&](StringView title, Result<SharedPtr<Element>> body) noexcept {
        if (!body) {
            return Result<void>::Failure(body.Error());
        }

        auto heading = MakeSectionTitle(context, title);
        if (!heading) {
            return Result<void>::Failure(heading.Error());
        }

        auto result = page.Value()->TryAddSlot(
            heading.Value(),
            {.before = SectionBefore, .after = SampleSpacing, .alignment = StackAlignment::Start});
        if (!result) {
            return result;
        }

        return page.Value()->TryAddSlot(body.Value(), {.alignment = StackAlignment::Start});
    };
    const FontSection sections[] = {{"CORE FONTS", CoreFonts},
                                    {"HEADINGS", HeadingFonts},
                                    {"PURPOSE FONTS", PurposeFonts},
                                    {"TYPEFACES", TypefaceFonts},
                                    {"WRITING SYSTEMS", ScriptFonts}};
    for (const FontSection& entry : sections) {
        if (auto result = section(entry.title, MakeFontTable(context, entry.rows)); !result) {
            return Result<SharedPtr<Element>>::Failure(result.Error());
        }
    }

    if (auto result = section("PARAGRAPH", MakeParagraphTable(context)); !result) {
        return Result<SharedPtr<Element>>::Failure(result.Error());
    }

    auto padded =
        PadContent(context, page.Value(), {PagePadding, PagePadding, PagePadding, PagePadding});
    if (!padded) {
        return padded;
    }

    if (auto result = scroll.Value()->TryAddChild(padded.Value()); !result) {
        return Result<SharedPtr<Element>>::Failure(result.Error());
    }

    return Result<SharedPtr<Element>>::Success(scroll.Value());
}

} // namespace rocks::ui::gallery
