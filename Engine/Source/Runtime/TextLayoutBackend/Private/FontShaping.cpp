// FreeType face의 shaping이다(Docs/Design/Font 6.4절). FullShaping은 HarfBuzz(hb-ft — FreeType
// face를 공유), KerningOnly는 UE FSlateTextShaper::PerformKerningOnlyTextShaping처럼 cmap 글리프 +
// advance + kerning이다.
#include "FontShaping.h"

#include <freetype/ftadvanc.h>

#include <TextLayout/Utf8.h>
#include <hb-ft.h>

namespace rocks::text::detail {

namespace {

// 26.6·16.16 고정소수 → 픽셀이다.
constexpr float Fixed26Dot6Scale = 64.0f;
constexpr float Fixed16Dot16Scale = 65536.0f;

// 합자 끄기 feature의 범위(문자열 전체)다.
constexpr unsigned int FeatureGlobalStart = HB_FEATURE_GLOBAL_START;
constexpr unsigned int FeatureGlobalEnd = HB_FEATURE_GLOBAL_END;

Result<void> ShapeKerningOnly(FT_Face face, BackendFace* owner, FT_Int32 flags,
                              const ShapeRunRequest& request, Array<ShapedGlyph>& out) noexcept {
    const usize first = out.Size();
    usize offset = 0;
    FT_UInt previous = 0;
    while (offset < request.text.ByteCount()) {
        char32_t codePoint = 0;
        const usize length = DecodeUtf8(request.text, offset, codePoint);
        if (length == 0) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        const FT_UInt glyph = FT_Get_Char_Index(face, codePoint);
        FT_Fixed advance = 0;
        FT_Get_Advance(face, glyph, flags, &advance);
        if (previous != 0 && glyph != 0 && FT_HAS_KERNING(face)) {
            FT_Vector kerning{};
            if (FT_Get_Kerning(face, previous, glyph, FT_KERNING_DEFAULT, &kerning) == FT_Err_Ok) {
                out[out.Size() - 1].advance += static_cast<float>(kerning.x) / Fixed26Dot6Scale;
            }
        }

        ShapedGlyph shaped;
        shaped.face = owner;
        shaped.glyphIndex = glyph;
        shaped.cluster = request.clusterBase + static_cast<uint32>(offset);
        shaped.advance = static_cast<float>(advance) / Fixed16Dot16Scale;
        if (auto added = out.TryPushBack(shaped); !added) {
            return added;
        }

        previous = glyph;
        offset += length;
    }

    // KerningOnly는 RTL을 다루지 않는다(Auto는 RTL을 FullShaping으로 보낸다). 그래도 RTL이면 시각
    // 순서로 뒤집는다.
    if (request.rightToLeft) {
        for (usize low = first, high = out.Size(); low + 1 < high; ++low, --high) {
            ShapedGlyph swapped = out[low];
            out[low] = out[high - 1];
            out[high - 1] = swapped;
        }
    }

    return Result<void>::Success();
}

} // namespace

FT_Int32 LoadFlags(FontHinting hinting, bool antialiasing, bool scalable) noexcept {
    FT_Int32 flags = FT_LOAD_COLOR;
    if (scalable) {
        flags |= FT_LOAD_NO_BITMAP;
    }

    if (!antialiasing) {
        return flags | FT_LOAD_TARGET_MONO | FT_LOAD_FORCE_AUTOHINT;
    }

    switch (hinting) {
    case FontHinting::Auto:
        return flags | FT_LOAD_FORCE_AUTOHINT;
    case FontHinting::AutoLight:
        return flags | FT_LOAD_TARGET_LIGHT;
    case FontHinting::Monochrome:
        return flags | FT_LOAD_TARGET_MONO | FT_LOAD_FORCE_AUTOHINT;
    case FontHinting::None:
        return flags | FT_LOAD_NO_AUTOHINT | FT_LOAD_NO_HINTING;
    case FontHinting::Default:
    default:
        return flags | FT_LOAD_TARGET_NORMAL;
    }
}

Result<void> ShapeRun(FT_Face face, hb_font_t*& hbFont, BackendFace* owner, FontHinting hinting,
                      const ShapeRunRequest& request, Array<ShapedGlyph>& out) noexcept {
    const FT_Int32 flags = LoadFlags(hinting, true, FT_IS_SCALABLE(face) != 0);
    if (!request.fullShaping) {
        return ShapeKerningOnly(face, owner, flags, request, out);
    }

    if (hbFont == nullptr) {
        hbFont = hb_ft_font_create_referenced(face);
        if (hbFont == nullptr) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }
    }

    // 크기가 바뀌었을 수 있으므로 hb-ft에 다시 알린다(face의 지금 크기를 쓴다).
    hb_ft_font_changed(hbFont);
    hb_ft_font_set_load_flags(hbFont, flags);
    hb_buffer_t* buffer = hb_buffer_create();
    if (!hb_buffer_allocation_successful(buffer)) {
        hb_buffer_destroy(buffer);
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    const int textLength = static_cast<int>(request.text.ByteCount());
    hb_buffer_add_utf8(buffer, request.text.Data(), textLength, 0, textLength);
    hb_buffer_guess_segment_properties(buffer);
    hb_buffer_set_direction(buffer, request.rightToLeft ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
    if (request.language.ByteCount() != 0) {
        hb_buffer_set_language(
            buffer, hb_language_from_string(request.language.Data(),
                                            static_cast<int>(request.language.ByteCount())));
    }

    const hb_feature_t noLigatures = {HB_TAG('l', 'i', 'g', 'a'), 0, FeatureGlobalStart,
                                      FeatureGlobalEnd};
    hb_shape(hbFont, buffer, request.disableLigatures ? &noLigatures : nullptr,
             request.disableLigatures ? 1u : 0u);
    unsigned int count = 0;
    const hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(buffer, &count);
    const hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(buffer, &count);
    Result<void> result = out.TryReserve(out.Size() + count);
    for (unsigned int index = 0; result && index < count; ++index) {
        ShapedGlyph shaped;
        shaped.face = owner;
        shaped.glyphIndex = infos[index].codepoint;
        shaped.cluster = request.clusterBase + infos[index].cluster;
        shaped.advance = static_cast<float>(positions[index].x_advance) / Fixed26Dot6Scale;
        shaped.offsetX = static_cast<float>(positions[index].x_offset) / Fixed26Dot6Scale;
        shaped.offsetY = static_cast<float>(positions[index].y_offset) / Fixed26Dot6Scale;
        result = out.TryPushBack(shaped);
    }

    hb_buffer_destroy(buffer);
    return result;
}

} // namespace rocks::text::detail
