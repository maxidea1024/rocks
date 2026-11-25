// FreeType backend의 face 열기·정보·metric이다(Docs/Design/Font 6.2·6.8절, FT2a). metric 계산은 UE
// FFreeTypeFace·FreeTypeUtils(FontCacheFreeType.cpp 142–297행, FontCacheFreeType.h 269–293행)와
// 같다.
#include "FontDistanceField.h"
#include "FontRaster.h"
#include "FontShaping.h"
#include "IcuText.h"
#include "TextLibraries.h"

#include <Core/Diagnostics/Log.h>
#include <TextLayoutBackend/TextLayoutBackend.h>
#include <freetype/freetype.h>
#include <freetype/fterrors.h>
#include <freetype/ftmodapi.h>
#include <freetype/tttables.h>
#include <stddef.h>
#include <string.h>

namespace rocks::text {

namespace {

inline constexpr LogCategory LogTextLayoutBackend{"TextLayoutBackend"};

// FreeType 블록 정렬(malloc과 같은 보장)이다.
constexpr usize FreeTypeBlockAlignment = alignof(max_align_t);

// 26.6 고정소수의 1이다.
constexpr float Fixed26Dot6Scale = 64.0f;

// 26.6 올림·내림 마스크다(UE FFreeTypeFace::GetAscender·GetDescender).
constexpr FT_Pos Fixed26Dot6FractionMask = 0b111111;

struct FreeTypeMemory {
    FT_MemoryRec_ record{};
};

void* FreeTypeAlloc(FT_Memory memory, long size) {
    Result<void*> block = static_cast<Allocator*>(memory->user)
                              ->Allocate(static_cast<usize>(size), FreeTypeBlockAlignment);
    return block ? block.Value() : nullptr;
}

void FreeTypeFree(FT_Memory memory, void* block) {
    if (block != nullptr) {
        (void)static_cast<Allocator*>(memory->user)->Deallocate(block);
    }
}

// FreeType이 이전 크기를 주므로 새 블록에 옮기고 이전 블록을 놓는다.
void* FreeTypeRealloc(FT_Memory memory, long currentSize, long newSize, void* block) {
    void* resized = FreeTypeAlloc(memory, newSize);
    if (resized != nullptr && block != nullptr) {
        memcpy(resized, block, static_cast<size_t>(currentSize < newSize ? currentSize : newSize));
        FreeTypeFree(memory, block);
    }

    return resized;
}

const char* FreeTypeErrorText(FT_Error error) noexcept {
    const char* text = FT_Error_String(error);
    return text != nullptr ? text : "unknown FreeType error";
}

float ToPixels(FT_Pos value) noexcept {
    return static_cast<float>(value) / Fixed26Dot6Scale;
}

class FreeTypeFace final : public BackendFace {
public:
    FreeTypeFace(FT_Library library, FT_Face face, const FontFaceDesc& desc) noexcept
        : library_(library), face_(face), desc_(desc) {
    }

    ~FreeTypeFace() override {
        if (hbFont_ != nullptr) {
            hb_font_destroy(hbFont_);
        }

        FT_Done_Face(face_);
    }

    Result<void> Info(FontFaceInfo& out) const noexcept override {
        if (auto family = out.family.TryAssign(
                StringView(face_->family_name != nullptr ? face_->family_name : ""));
            !family) {
            return family;
        }

        if (auto style = out.style.TryAssign(
                StringView(face_->style_name != nullptr ? face_->style_name : ""));
            !style) {
            return style;
        }

        out.glyphCount = static_cast<uint32>(face_->num_glyphs);
        out.unitsPerEm = face_->units_per_EM;
        out.scalable = FT_IS_SCALABLE(face_) != 0;
        out.color = FT_HAS_COLOR(face_) != 0;
        out.fixedSizeCount = face_->num_fixed_sizes;
        out.bold = (face_->style_flags & FT_STYLE_FLAG_BOLD) != 0;
        out.italic = (face_->style_flags & FT_STYLE_FLAG_ITALIC) != 0;
        return Result<void>::Success();
    }

    Result<FontMetrics> Metrics(float size, float scale) noexcept override {
        if (!(size > 0.0f) || !(scale > 0.0f)) {
            return Result<FontMetrics>::Failure(ErrorCode::InvalidArgument);
        }

        if (auto applied = ApplySize(ComputeFontPixelSize(size, scale)); !applied) {
            return Result<FontMetrics>::Failure(applied.Error());
        }

        const bool bounding = desc_.layoutMethod == FontLayoutMethod::BoundingBox;
        const FT_Fixed yScale = face_->size->metrics.y_scale;
        FT_Pos ascender = 0;
        FT_Pos descender = 0;
        FT_Pos height = 0;
        if (FT_IS_SCALABLE(face_)) {
            // 둥근 size metric 대신 글꼴 단위 값에 y_scale을 곱한다(UE와 같다).
            ascender = FT_MulFix(bounding ? face_->bbox.yMax : face_->ascender, yScale);
            descender = FT_MulFix(bounding ? face_->bbox.yMin : face_->descender, yScale);
            height =
                FT_MulFix(bounding ? face_->bbox.yMax - face_->bbox.yMin : face_->height, yScale);
        } else if (FT_HAS_FIXED_SIZES(face_)) {
            ascender = FT_MulFix(face_->size->metrics.ascender, yScale);
            descender = FT_MulFix(face_->size->metrics.descender, yScale);
            height = FT_MulFix(face_->size->metrics.height, yScale);
        }

        if (desc_.overrideAscent) {
            ascender = (FT_MulFix(desc_.ascentOverride, yScale) + Fixed26Dot6FractionMask) &
                       ~Fixed26Dot6FractionMask;
        }

        if (desc_.overrideDescent) {
            descender = FT_MulFix(desc_.descentOverride, yScale) & ~Fixed26Dot6FractionMask;
        }

        if (desc_.overrideAscent || desc_.overrideDescent) {
            height = ascender - descender;
        }

        FontMetrics metrics;
        metrics.ascender = ToPixels(ascender);
        metrics.descender = ToPixels(descender);
        metrics.lineHeight = ToPixels(height);
        metrics.underlinePosition = ToPixels(FT_MulFix(face_->underline_position, yScale));
        metrics.underlineThickness = ToPixels(FT_MulFix(face_->underline_thickness, yScale));
        return Result<FontMetrics>::Success(metrics);
    }

    uint32 GlyphIndex(uint32 codePoint) noexcept override {
        return FT_Get_Char_Index(face_, codePoint);
    }

    Result<void> Shape(const ShapeRunRequest& request, Array<ShapedGlyph>& out) noexcept override {
        if (!(request.size > 0.0f) || !(request.scale > 0.0f)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        if (auto applied = ApplySize(ComputeFontPixelSize(request.size, request.scale)); !applied) {
            return applied;
        }

        return detail::ShapeRun(face_, hbFont_, this, desc_.hinting, request, out);
    }

    Result<void> Rasterize(const GlyphRasterRequest& request, GlyphBitmap& out) noexcept override {
        if (!(request.size > 0.0f) || !(request.scale > 0.0f)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        if (auto applied = ApplySize(ComputeFontPixelSize(request.size, request.scale)); !applied) {
            return applied;
        }

        return detail::RasterizeGlyph(library_, face_, desc_.hinting, request, out);
    }

    Result<void> ExtractGeometry(uint32 glyphIndex, GlyphGeometry& out) noexcept override {
        return detail::ExtractGlyphGeometry(face_, glyphIndex, out);
    }

private:
    // UE ApplySizeAndScale과 같다: scalable은 픽셀 크기를 정하고, 비트맵 글꼴은 가장 알맞은
    // strike를 골라 그 strike에서 요청 크기로의 배율을 x_scale·y_scale에 둔다.
    Result<void> ApplySize(uint32 pixelSize) noexcept {
        if (FT_IS_SCALABLE(face_)) {
            const FT_Error error = FT_Set_Pixel_Sizes(face_, pixelSize, pixelSize);
            return error == FT_Err_Ok ? Result<void>::Success()
                                      : Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        if (!FT_HAS_FIXED_SIZES(face_)) {
            return Result<void>::Failure(ErrorCode::Unsupported);
        }

        const FT_Pos required =
            static_cast<FT_Pos>(pixelSize) * static_cast<FT_Pos>(Fixed26Dot6Scale);
        int32 best = -1;
        FT_Pos bestHeight = 0;
        for (int32 index = 0; index < face_->num_fixed_sizes; ++index) {
            const FT_Pos height = face_->available_sizes[index].y_ppem;
            if (height == required) {
                best = index;
                break;
            }

            if (best < 0) {
                best = index;
                bestHeight = height;
                continue;
            }

            if (bestHeight < required) {
                if (height > bestHeight) {
                    best = index;
                    bestHeight = height;
                }

                continue;
            }

            if (bestHeight > required && height < bestHeight && height > required) {
                best = index;
                bestHeight = height;
            }
        }

        if (FT_Select_Size(face_, best) != FT_Err_Ok) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        // 26.6을 16.16으로 옮겨 FT_DivFix로 strike → 요청 크기 배율을 구한다.
        const FT_Fixed strikeScale =
            FT_DivFix(required << 10, face_->available_sizes[best].y_ppem << 10);
        face_->size->metrics.x_scale = strikeScale;
        face_->size->metrics.y_scale = strikeScale;
        return Result<void>::Success();
    }

    FT_Library library_;
    FT_Face face_;
    FontFaceDesc desc_;
    hb_font_t* hbFont_ = nullptr;
};

class FreeTypeBackend final : public TextBackend {
public:
    explicit FreeTypeBackend(Allocator& allocator) noexcept : allocator_(allocator) {
        memory_.record.user = &allocator_;
        memory_.record.alloc = FreeTypeAlloc;
        memory_.record.free = FreeTypeFree;
        memory_.record.realloc = FreeTypeRealloc;
    }

    ~FreeTypeBackend() override {
        if (library_ != nullptr) {
            FT_Done_Library(library_);
        }
    }

    Result<void> Create() noexcept {
        if (FT_New_Library(&memory_.record, &library_) != FT_Err_Ok) {
            library_ = nullptr;
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }

        FT_Add_Default_Modules(library_);
        FT_Set_Default_Properties(library_);
        return Result<void>::Success();
    }

    Result<int32> CountSubFaces(const FontFaceData& data) noexcept override {
        FT_Face face = nullptr;
        const Span<const uint8> bytes = data.Bytes();
        // face 번호 −1은 face를 만들지 않고 하위 face 수만 알아낸다(FreeType 규약).
        const FT_Error error = FT_New_Memory_Face(library_, bytes.Data(),
                                                  static_cast<FT_Long>(bytes.Size()), -1, &face);
        if (error != FT_Err_Ok) {
            Log::Warning(LogTextLayoutBackend, "Font data is not a readable font: {Error}",
                         FreeTypeErrorText(error));
            return Result<int32>::Failure(ErrorCode::InvalidArgument);
        }

        const int32 count = static_cast<int32>(face->num_faces);
        FT_Done_Face(face);
        return Result<int32>::Success(count);
    }

    Result<UniquePtr<BackendFace>> OpenFace(const FontFaceDesc& desc) noexcept override {
        if (desc.data.Get() == nullptr || desc.subFaceIndex < 0) {
            return Result<UniquePtr<BackendFace>>::Failure(ErrorCode::InvalidArgument);
        }

        FT_Face face = nullptr;
        const Span<const uint8> bytes = desc.data->Bytes();
        const FT_Error error = FT_New_Memory_Face(
            library_, bytes.Data(), static_cast<FT_Long>(bytes.Size()), desc.subFaceIndex, &face);
        if (error != FT_Err_Ok) {
            Log::Warning(LogTextLayoutBackend, "Cannot open font sub-face {Index}: {Error}",
                         desc.subFaceIndex, FreeTypeErrorText(error));
            return Result<UniquePtr<BackendFace>>::Failure(ErrorCode::InvalidArgument);
        }

        auto made = UniquePtr<FreeTypeFace>::TryMake(allocator_, library_, face, desc);
        if (!made) {
            FT_Done_Face(face);
            return Result<UniquePtr<BackendFace>>::Failure(made.Error());
        }

        return Result<UniquePtr<BackendFace>>::Success(UniquePtr<BackendFace>(Move(made.Value())));
    }

    Result<void> FindBoundaries(TextBoundaryKind kind, StringView utf8, StringView culture,
                                Array<usize>& out) noexcept override {
        return detail::FindBoundaries(kind, utf8, culture, out);
    }

    Result<void> ResolveBidi(StringView utf8, TextBaseDirection base,
                             Array<BidiRun>& out) noexcept override {
        return detail::ResolveBidi(utf8, base, out);
    }

    Result<void> GenerateDistanceField(const GlyphGeometry& geometry,
                                       const DistanceFieldRequest& request,
                                       GlyphBitmap& out) const noexcept override {
        return detail::GenerateGlyphDistanceField(geometry, request, out);
    }

private:
    Allocator& allocator_;
    FreeTypeMemory memory_;
    FT_Library library_ = nullptr;
};

} // namespace

Result<void> InitializeTextLibraries(Span<const uint8> icuData) noexcept {
    return detail::Initialize(icuData);
}

Result<UniquePtr<TextBackend>> CreateTextBackend(const TextBackendOptions& options) noexcept {
    if (options.allocator == nullptr) {
        return Result<UniquePtr<TextBackend>>::Failure(ErrorCode::InvalidArgument);
    }

    if (!detail::IsInitialized()) {
        return Result<UniquePtr<TextBackend>>::Failure(ErrorCode::NotReady);
    }

    auto made = UniquePtr<FreeTypeBackend>::TryMake(*options.allocator, *options.allocator);
    if (!made) {
        return Result<UniquePtr<TextBackend>>::Failure(made.Error());
    }

    if (auto created = made.Value()->Create(); !created) {
        return Result<UniquePtr<TextBackend>>::Failure(created.Error());
    }

    return Result<UniquePtr<TextBackend>>::Success(UniquePtr<TextBackend>(Move(made.Value())));
}

} // namespace rocks::text
