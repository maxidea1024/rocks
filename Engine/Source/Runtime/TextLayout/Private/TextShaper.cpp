// 조합 글꼴 shaping이다(Docs/Design/Font 6.4·6.8절). 규칙은 UE
// FSlateTextShaper(SlateTextShaper.cpp)와 같다.
#include <Core/Text/Unicode.h>
#include <TextLayout/TextShaper.h>
#include <TextLayout/Utf8.h>

namespace rocks::text {

namespace {

// 1/1000 em 단위(UE LetterSpacing)다.
constexpr float LetterSpacingUnitsPerEm = 1000.0f;

// UE CharRequiresFullShaping의 구간 표다(SlateTextShaper.cpp 166–231행).
struct ShapingRange {
    char32_t first;
    char32_t last;
};

constexpr ShapingRange FullShapingRanges[] = {
    {0x0300, 0x036F}, {0x1AB0, 0x1AFF}, {0x1DC0, 0x1DFF}, {0x20D0, 0x20FF},
    {0x3099, 0x309A}, {0x31C0, 0x31EF}, {0xFE00, 0xFE0F}, {0xFE20, 0xFE2F}, // 결합 문자
    {0x0A00, 0x0A7F},                                                       // Gurmukhi
    {0x0900, 0x097F}, {0xA8E0, 0xA8FF}, {0x1CD0, 0x1CFF},                   // Devanagari
    {0x0980, 0x09FF},                                                       // Bengali
    {0x0A80, 0x0AFF},                                                       // Gujarati
    {0x0B00, 0x0B7F},                                                       // Odia
    {0x0B80, 0x0BFF},                                                       // Tamil
    {0x0C00, 0x0C7F},                                                       // Telugu
    {0x0C80, 0x0CFF},                                                       // Kannada
    {0x0D00, 0x0D7F},                                                       // Malayalam
    {0x0E00, 0x0E7F},                                                       // Thai
    {0x0F00, 0x0FFF},                                                       // Tibetan
    {0x1780, 0x17FF}, {0x19E0, 0x19FF},                                     // Khmer
    {0x0D80, 0x0DFF},                                                       // Sinhala
    {0x1900, 0x194F},                                                       // Limbu
    {0x1A20, 0x1AAF},                                                       // Tai Tham
    {0xAA80, 0xAADF},                                                       // Tai Viet
    {0x1BC0, 0x1BFF},                                                       // Batak
};

constexpr char32_t ZeroWidthJoiner = 0x200D;
constexpr char32_t AsciiLast = 0x7F;
constexpr char32_t BasicPlaneLast = 0xFFFF;

bool CharRequiresFullShaping(char32_t codePoint) noexcept {
    if (codePoint <= AsciiLast) {
        return false;
    }

    // UTF-16 대리 쌍이 되는 BMP 밖 code point(UE는 surrogate 쌍을 본다).
    if (codePoint > BasicPlaneLast || codePoint == ZeroWidthJoiner) {
        return true;
    }

    for (const ShapingRange& range : FullShapingRanges) {
        if (codePoint >= range.first && codePoint <= range.last) {
            return true;
        }
    }

    return false;
}

// 공백류(Zs)·탭은 face를 고르지 않고 앞 face를 이어 쓴다(UE SlateTextShaper 26–32행).
bool IsWhitespace(char32_t codePoint) noexcept {
    return codePoint == '\t' || GetGeneralCategory(codePoint) == GeneralCategory::Zs;
}

// face 하나로 shaping할 묶음이다.
struct FaceRun {
    usize start = 0;
    usize length = 0;
    BackendFace* face = nullptr;
    float scalingFactor = 1.0f;
};

} // namespace

bool RequiresFullShaping(StringView utf8) noexcept {
    usize offset = 0;
    while (offset < utf8.ByteCount()) {
        char32_t codePoint = 0;
        const usize length = DecodeUtf8(utf8, offset, codePoint);
        if (length == 0) {
            return true;
        }

        if (CharRequiresFullShaping(codePoint)) {
            return true;
        }

        offset += length;
    }

    return false;
}

float MeasureWidth(Span<const ShapedGlyph> glyphs) noexcept {
    float width = 0;
    for (const ShapedGlyph& glyph : glyphs) {
        width += glyph.advance;
    }

    return width;
}

Result<void> ShapeText(TextBackend& backend, ResolvedCompositeFont& font, StringView utf8,
                       const ShapeOptions& options, Array<ShapedGlyph>& out) noexcept {
    out.Clear();
    if (!(options.size > 0.0f) || !(options.scale > 0.0f) || !utf8.ValidateUtf8()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (utf8.ByteCount() == 0) {
        return Result<void>::Success();
    }

    Allocator& allocator = out.GetAllocator();
    Array<BidiRun> bidiRuns(allocator);
    if (auto resolved = backend.ResolveBidi(utf8, options.direction, bidiRuns); !resolved) {
        return resolved;
    }

    Array<usize> graphemeEnds(allocator);
    if (auto found =
            backend.FindBoundaries(TextBoundaryKind::Grapheme, utf8, font.Culture(), graphemeEnds);
        !found) {
        return found;
    }

    Array<FaceRun> faceRuns(allocator);
    Array<ShapedGlyph> runGlyphs(allocator);
    for (const BidiRun& bidi : bidiRuns.View()) {
        // 시각 순서 run은 start가 오름차순이 아니므로 run마다 grapheme 위치를 처음부터 찾는다.
        usize graphemeIndex = 0;
        faceRuns.Clear();
        const usize runEnd = bidi.start + bidi.length;
        bool multiCodePointGrapheme = false;
        usize graphemeStart = bidi.start;
        while (graphemeIndex < graphemeEnds.Size() && graphemeEnds[graphemeIndex] <= bidi.start) {
            ++graphemeIndex;
        }

        while (graphemeStart < runEnd) {
            usize graphemeEnd =
                graphemeIndex < graphemeEnds.Size() ? graphemeEnds[graphemeIndex] : runEnd;
            if (graphemeEnd > runEnd) {
                graphemeEnd = runEnd;
            } else {
                ++graphemeIndex;
            }

            char32_t first = 0;
            const usize firstLength = DecodeUtf8(utf8, graphemeStart, first);
            if (firstLength == 0) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }

            multiCodePointGrapheme =
                multiCodePointGrapheme || graphemeEnd - graphemeStart > firstLength;
            FontSelection selection;
            if (IsWhitespace(first) && faceRuns.Size() != 0) {
                selection.face = faceRuns[faceRuns.Size() - 1].face;
                selection.scalingFactor = faceRuns[faceRuns.Size() - 1].scalingFactor;
            } else {
                selection = font.Select(first, options.typeface, options.style, options.fallback);
            }

            if (faceRuns.Size() != 0 && faceRuns[faceRuns.Size() - 1].face == selection.face &&
                faceRuns[faceRuns.Size() - 1].scalingFactor == selection.scalingFactor) {
                faceRuns[faceRuns.Size() - 1].length += graphemeEnd - graphemeStart;
            } else if (auto added =
                           faceRuns.TryPushBack({graphemeStart, graphemeEnd - graphemeStart,
                                                 selection.face, selection.scalingFactor});
                       !added) {
                return added;
            }

            graphemeStart = graphemeEnd;
        }

        const bool spacing =
            options.letterSpacing != 0 && !bidi.rightToLeft && !multiCodePointGrapheme;
        const usize glyphStart = out.Size();
        // RTL run은 face 묶음을 시각 순서(뒤에서 앞으로)로 잇는다. 묶음 안 글리프는 backend가 시각
        // 순서로 낸다.
        for (usize step = 0; step < faceRuns.Size(); ++step) {
            const FaceRun& run = faceRuns[bidi.rightToLeft ? faceRuns.Size() - 1 - step : step];
            const StringView text(utf8.Data() + run.start, run.length);
            const bool full = options.method == TextShapingMethod::FullShaping ||
                              (options.method == TextShapingMethod::Auto &&
                               (bidi.rightToLeft || RequiresFullShaping(text)));
            ShapeRunRequest request;
            request.text = text;
            request.clusterBase = static_cast<uint32>(run.start);
            request.size = options.size * run.scalingFactor;
            request.scale = options.scale;
            request.rightToLeft = bidi.rightToLeft;
            request.fullShaping = full;
            request.disableLigatures = options.letterSpacing != 0;
            request.language = font.Culture();
            runGlyphs.Clear();
            if (auto shaped = run.face->Shape(request, runGlyphs); !shaped) {
                return shaped;
            }

            if (auto appended =
                    out.TryAppend(Span<const ShapedGlyph>(runGlyphs.Data(), runGlyphs.Size()));
                !appended) {
                return appended;
            }
        }

        const float letterAdvance = static_cast<float>(options.letterSpacing) * options.scale *
                                    options.size / LetterSpacingUnitsPerEm;
        const float monoAdvance = options.monospacedWidth * options.size * options.scale;
        for (usize index = glyphStart; index < out.Size(); ++index) {
            if (options.monospaced) {
                out[index].advance = monoAdvance;
            }

            if (spacing) {
                out[index].advance += letterAdvance;
            }
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::text
