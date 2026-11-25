#include "DemoFont.h"
#include <Core/ScopeExit.h>

#if defined(_WIN32)
#include <Platform/Win64/WindowsHeadersBegin.h>
#include <Windows.h>
#include <Platform/Win64/WindowsHeadersEnd.h>
#endif

namespace rocks::ui {

namespace {
constexpr uint32 AtlasSize = 512;
}

DemoFont::DemoFont(Allocator& allocator, graphics::Device& device,
                   SlotArray<BrushTexture>& textures, graphics::PipelineHandle pipeline) noexcept
    : allocator_(allocator), glyphs_(allocator), page_(allocator), texture_(device, textures),
      pipeline_(pipeline) {
}

Result<void> DemoFont::Initialize() noexcept {
    auto ready = page_.Initialize(text::GlyphAtlasFormat::Alpha, AtlasSize);
    if (!ready) {
        return ready;
    }
    return Upload();
}

Result<void> DemoFont::Upload() noexcept {
    if (uploaded_ == page_.Revision()) {
        return Result<void>::Success();
    }
    text::GlyphAtlasSnapshot snapshot(allocator_);
    auto copied = page_.CopySnapshot(snapshot);
    if (!copied) {
        return copied;
    }
    auto result = texture_.Upload(snapshot, pipeline_);
    if (result) {
        uploaded_ = snapshot.revision;
    }
    return result;
}

Result<DemoFont::Glyph> DemoFont::FindGlyph(char32_t code, int32 height) noexcept {
    for (const auto& glyph : glyphs_) {
        if (glyph.code == code && glyph.height == height) {
            return Result<Glyph>::Success(glyph);
        }
    }
#if defined(_WIN32)
    if (code > 0xffff || height <= 0 || height > 64) {
        return Result<Glyph>::Failure(ErrorCode::Unsupported);
    }
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) {
        return Result<Glyph>::Failure(ErrorCode::NotReady);
    }
    ROCKS_ON_SCOPE_EXIT {
        DeleteDC(dc);
    };
    HFONT font = CreateFontW(-height, 0, 0, 0, height >= 24 ? FW_BOLD : FW_NORMAL, FALSE, FALSE,
                             FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                             ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Malgun Gothic");
    if (!font) {
        return Result<Glyph>::Failure(ErrorCode::NotReady);
    }
    ROCKS_ON_SCOPE_EXIT {
        DeleteObject(font);
    };
    HGDIOBJ previous = SelectObject(dc, font);
    if (!previous || previous == HGDI_ERROR) {
        return Result<Glyph>::Failure(ErrorCode::NotReady);
    }
    ROCKS_ON_SCOPE_EXIT {
        SelectObject(dc, previous);
    };

    MAT2 matrix{};
    matrix.eM11.value = matrix.eM22.value = 1;
    GLYPHMETRICS metrics{};
    TEXTMETRICW face{};
    const DWORD bytes = GetGlyphOutlineW(dc, static_cast<UINT>(code), GGO_GRAY8_BITMAP, &metrics, 0,
                                         nullptr, &matrix);
    if (bytes == GDI_ERROR || !GetTextMetricsW(dc, &face)) {
        return Result<Glyph>::Failure(ErrorCode::Unsupported);
    }
    Glyph result{code,
                 height,
                 static_cast<float>(metrics.gmCellIncX),
                 {static_cast<float>(metrics.gmptGlyphOrigin.x),
                  static_cast<float>(face.tmAscent - metrics.gmptGlyphOrigin.y)},
                 {}};
    if (metrics.gmBlackBoxX && metrics.gmBlackBoxY && bytes) {
        Array<uint8> pixels(allocator_);
        auto allocated = pixels.TryResize(bytes);
        if (!allocated) {
            return Result<Glyph>::Failure(allocated.Error());
        }
        if (GetGlyphOutlineW(dc, static_cast<UINT>(code), GGO_GRAY8_BITMAP, &metrics, bytes,
                             pixels.Data(), &matrix) == GDI_ERROR) {
            return Result<Glyph>::Failure(ErrorCode::Unsupported);
        }
        // GDI의 회색조 덮임률은 0~64이고 행 시작은 4바이트 경계다.
        for (auto& pixel : pixels) {
            pixel = static_cast<uint8>((static_cast<uint32>(pixel) * 255 + 32) / 64);
        }
        auto rect = page_.TryInsert(metrics.gmBlackBoxX, metrics.gmBlackBoxY,
                                    (metrics.gmBlackBoxX + 3u) & ~3u, pixels.View());
        if (!rect) {
            return Result<Glyph>::Failure(rect.Error());
        }
        result.rect = rect.Value();
    }
    auto stored = glyphs_.TryPushBack(result);
    return stored ? Result<Glyph>::Success(result) : Result<Glyph>::Failure(stored.Error());
#else
    return Result<Glyph>::Failure(ErrorCode::Unsupported);
#endif
}

Result<void> DemoFont::SetText(TextLabel& label, StringView value, int32 height) noexcept {
    auto points = value.TryCodePoints();
    if (!points) {
        return Result<void>::Failure(points.Error());
    }
    Array<text::TextCluster> clusters(allocator_);
    Array<LabelGlyph> glyphs(allocator_);
    usize end = 0;
    for (const auto point : points.Value()) {
        auto found = FindGlyph(point, height);
        if (!found) {
            return Result<void>::Failure(found.Error());
        }
        const auto& glyph = found.Value();
        // 다음 선행 바이트까지 이동한다. 순회 범위가 이미 UTF-8 전체를 검증했다.
        do {
            ++end;
        } while (end < value.ByteCount() && (static_cast<uint8>(value.Data()[end]) & 0xc0) == 0x80);
        const usize index = clusters.Size();
        auto added = clusters.TryPushBack({end, glyph.advance, 0, point == U' '});
        if (!added) {
            return added;
        }
        if (!glyph.rect.IsEmpty()) {
            const auto size = glyph.rect.Size();
            const Vec2 lo(static_cast<float>(glyph.rect.min.x),
                          static_cast<float>(glyph.rect.min.y));
            const Vec2 extent(static_cast<float>(size.x), static_cast<float>(size.y));
            added =
                glyphs.TryPushBack({index,
                                    {glyph.offset,
                                     extent,
                                     texture_.Resource(),
                                     {lo / float(AtlasSize), (lo + extent) / float(AtlasSize)}}});
            if (!added) {
                return added;
            }
        }
    }
    auto uploaded = Upload();
    if (!uploaded) {
        return uploaded;
    }
    return label.SetShapedText(value, clusters.View(), glyphs.View(),
                               static_cast<float>(height + 6), false);
}

} // namespace rocks::ui
