#include "DemoText.h"

#include "DemoFiles.h"

#include <Core/Diagnostics/Log.h>
#include <TextLayout/TextShaper.h>
#include <TextLayoutBackend/TextLayoutBackend.h>

// 데모 레이블 텍스트다(Docs/Design/UiToolkit 9.1절 UW2).
namespace rocks::ui {

namespace {

inline constexpr LogCategory LogUiDemo{"UiDemo"};

// 엔진 폴더 기준 ICU 데이터·시험 글꼴 경로다(Engine/Source/ThirdParty).
constexpr char IcuDataPath[] = "Source/ThirdParty/IcuData/icudata-79.1-rocks/icudt79l.dat";
// UE 기본 합성 글꼴의 typeface다(LegacySlateFontInfoCache.cpp 111–122행과 같은 이름). VeryLight는
// UE처럼 Light 파일이고(UE는 hinting만 다르다), BoldCondensed는 Roboto 3의 RobotoCondensed-Bold다.
struct LatinFace {
    const char* typeface;
    const char* path;
};
constexpr char RobotoDirectory[] =
    "Source/ThirdParty/TestFontRoboto/roboto-3-classic-v3.016/hinted/static/";
constexpr LatinFace LatinFaces[] = {{"Regular", "Roboto-Regular.ttf"},
                                    {"Medium", "Roboto-Medium.ttf"},
                                    {"Light", "Roboto-Light.ttf"},
                                    {"VeryLight", "Roboto-Light.ttf"},
                                    {"Bold", "Roboto-Bold.ttf"},
                                    {"Black", "Roboto-Black.ttf"},
                                    {"Italic", "Roboto-Italic.ttf"},
                                    {"BoldItalic", "Roboto-BoldItalic.ttf"},
                                    {"BlackItalic", "Roboto-BlackItalic.ttf"},
                                    {"BoldCondensed", "RobotoCondensed-Bold.ttf"},
                                    {"BoldCondensedItalic", "RobotoCondensed-BoldItalic.ttf"}};
// UE Mono typeface(DroidSansMono)다.
constexpr char MonoFontPath[] =
    "Source/ThirdParty/TestFontDroidSansMono/aosp-android-4.4_r1/DroidSansMono.ttf";
constexpr usize LatinFaceCount = sizeof(LatinFaces) / sizeof(LatinFaces[0]);
// 경로 버퍼 byte 수다.
constexpr usize FacePathBytes = 256;
constexpr char HangulFontPath[] = "Source/ThirdParty/TestFontNanumGothic/google-fonts-06bfe0d62a98/"
                                  "ofl/nanumgothic/NanumGothic-Regular.ttf";
constexpr char ArabicFontPath[] = "Source/ThirdParty/TestFontNotoNaskhArabic/"
                                  "google-fonts-06bfe0d62a98/ofl/notonaskharabic/"
                                  "NotoNaskhArabic-wght.ttf";
constexpr char EmojiFontPath[] =
    "Source/ThirdParty/TestFontGenerated/generated-2026-11-24/NotoColorEmoji-U1F600.ttf";

// 보조 typeface의 code point 구간이다(한글 자모·호환 자모·음절, 아랍 문자 기본·보충·표시형).
constexpr text::CodePointRange HangulRanges[] = {
    {0x1100, 0x11FF}, {0x3130, 0x318F}, {0xA960, 0xA97F}, {0xAC00, 0xD7AF}, {0xD7B0, 0xD7FF}};
constexpr text::CodePointRange ArabicRanges[] = {
    {0x0600, 0x06FF}, {0x0750, 0x077F}, {0x08A0, 0x08FF}, {0xFB50, 0xFDFF}, {0xFE70, 0xFEFF}};

// 줄바꿈 code point다(강제 줄바꿈 클러스터).
constexpr char LineFeed = '\n';

// 기준 글자(줄 metric을 읽을 face를 고른다)다.
constexpr uint32 MetricsCodePoint = 'A';

// 정렬된 경계 목록에서 byte 위치가 경계인지 본다.
bool IsBoundary(Span<const usize> boundaries, usize offset) noexcept {
    usize low = 0;
    usize high = boundaries.Size();
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        if (boundaries[middle] < offset) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    return low < boundaries.Size() && boundaries[low] == offset;
}

// byte 위치가 속한 클러스터 번호다(ends는 클러스터 끝 byte, 오름차순).
usize ClusterOf(Span<const usize> ends, usize offset) noexcept {
    usize low = 0;
    usize high = ends.Size();
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        if (ends[middle] <= offset) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    return low < ends.Size() ? low : ends.Size() - 1;
}

} // namespace

DemoText::DemoText(Allocator& allocator, graphics::Device& device,
                   SlotArray<BrushTexture>& textures, graphics::PipelineHandle maskPipeline,
                   graphics::PipelineHandle imagePipeline) noexcept
    : allocator_(allocator), textures_(allocator, device, textures), maskPipeline_(maskPipeline),
      imagePipeline_(imagePipeline), icuData_(allocator) {
}

DemoText::~DemoText() noexcept {
    // 텍스처를 먼저 놓고 cache·글꼴·backend 순으로 파괴한다(cache가 face를 가리킨다).
    textures_.Reset();
    cache_ = UniquePtr<text::GlyphCache>();
    font_ = UniquePtr<text::ResolvedCompositeFont>();
}

Result<void> DemoText::LoadFace(StringView relativePath,
                                SharedPtr<const text::FontFaceData>& out) noexcept {
    Array<uint8> bytes(allocator_);
    if (!ReadEngineFile(relativePath, bytes)) {
        Log::Error(LogUiDemo, "Font file not found: {Path}", relativePath);
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    auto data = text::FontFaceData::TryCreate(allocator_, bytes.View());
    if (!data) {
        return Result<void>::Failure(data.Error());
    }

    out = Move(data.Value());
    return Result<void>::Success();
}

Result<void> DemoText::Initialize() noexcept {
    // ICU 데이터는 프로세스 끝까지 살아야 한다(InitializeTextLibraries 계약) — 이 객체가 main
    // 수명이다.
    if (!ReadEngineFile(IcuDataPath, icuData_) || icuData_.Size() == 0) {
        Log::Error(LogUiDemo, "ICU data not found: {Path}", StringView(IcuDataPath));
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    if (auto ready = text::InitializeTextLibraries(icuData_.View()); !ready) {
        return ready;
    }

    auto backend = text::CreateTextBackend({.allocator = &allocator_});
    if (!backend) {
        return Result<void>::Failure(backend.Error());
    }

    backend_ = Move(backend.Value());
    SharedPtr<const text::FontFaceData> latin[LatinFaceCount];
    SharedPtr<const text::FontFaceData> mono;
    SharedPtr<const text::FontFaceData> hangul;
    SharedPtr<const text::FontFaceData> arabic;
    SharedPtr<const text::FontFaceData> emoji;
    for (usize i = 0; i < LatinFaceCount; ++i) {
        char path[FacePathBytes] = {};
        usize length = 0;
        const char* parts[] = {RobotoDirectory, LatinFaces[i].path};
        for (const char* part : parts) {
            for (const char* c = part; *c != '\0' && length + 1 < FacePathBytes; ++c) {
                path[length++] = *c;
            }
        }

        if (auto loaded = LoadFace(StringView(path, length), latin[i]); !loaded) {
            return loaded;
        }
    }

    if (auto loaded = LoadFace(MonoFontPath, mono); !loaded) {
        return loaded;
    }

    if (auto loaded = LoadFace(HangulFontPath, hangul); !loaded) {
        return loaded;
    }

    if (auto loaded = LoadFace(ArabicFontPath, arabic); !loaded) {
        return loaded;
    }

    if (auto loaded = LoadFace(EmojiFontPath, emoji); !loaded) {
        return loaded;
    }

    text::CompositeTypefaceEntry latinFace[LatinFaceCount + 1];
    for (usize i = 0; i < LatinFaceCount; ++i) {
        latinFace[i] = {LatinFaces[i].typeface, {.data = latin[i]}};
    }

    latinFace[LatinFaceCount] = {"Mono", {.data = mono}};

    const text::CompositeTypefaceEntry hangulFace[] = {{"Regular", {.data = hangul}}};
    const text::CompositeTypefaceEntry arabicFace[] = {{"Regular", {.data = arabic}}};
    const text::CompositeSubTypeface subTypefaces[] = {
        {.typeface = hangulFace, .ranges = HangulRanges},
        {.typeface = arabicFace, .ranges = ArabicRanges}};
    text::CompositeFontSpec spec;
    spec.defaultTypeface = latinFace;
    spec.fallbackTypeface = hangulFace;
    spec.subTypefaces = subTypefaces;
    spec.lastResort = {.data = emoji};
    auto font = text::ResolvedCompositeFont::TryCreate(allocator_, *backend_, spec);
    if (!font) {
        return Result<void>::Failure(font.Error());
    }

    font_ = Move(font.Value());
    auto cache = text::GlyphCache::TryCreate(allocator_, {}, backend_.Get());
    if (!cache) {
        return Result<void>::Failure(cache.Error());
    }

    cache_ = Move(cache.Value());
    return Result<void>::Success();
}

Result<void> DemoText::SetText(TextLabel& label, StringView value, int32 pixelHeight) noexcept {
    if (pixelHeight <= 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 픽셀 높이 → 96 DPI 포인트 크기다.
    const float size = static_cast<float>(pixelHeight) *
                       static_cast<float>(text::FontPointsPerInch) /
                       static_cast<float>(text::FontRenderDpi);
    return Shape(label, value, size, "Regular", 0);
}

Result<void> DemoText::SetText(TextLabel& label, StringView value, const FontStyle& font) noexcept {
    if (!(font.size > 0)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return Shape(label, value, font.size, TypefaceName(font.typeface), font.letterSpacing);
}

Result<void> DemoText::Shape(TextLabel& label, StringView value, float size, StringView typeface,
                             int32 letterSpacing) noexcept {
    if (font_.Get() == nullptr) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }

    Array<text::ShapedGlyph> shaped(allocator_);
    if (auto made = text::ShapeText(
            *backend_, *font_, value,
            {.size = size, .typeface = typeface, .letterSpacing = letterSpacing}, shaped);
        !made) {
        return made;
    }

    // 클러스터는 grapheme 단위(논리 순서)이고, 줄 경계·BiDi 수준을 붙인다.
    Array<usize> graphemes(allocator_);
    Array<usize> lineBreaks(allocator_);
    Array<text::BidiRun> runs(allocator_);
    if (auto found =
            backend_->FindBoundaries(text::TextBoundaryKind::Grapheme, value, "", graphemes);
        !found) {
        return found;
    }

    if (auto found = backend_->FindBoundaries(text::TextBoundaryKind::Line, value, "", lineBreaks);
        !found) {
        return found;
    }

    if (auto resolved = backend_->ResolveBidi(value, text::TextBaseDirection::Auto, runs);
        !resolved) {
        return resolved;
    }

    const text::FontSelection metricsFace =
        font_->Select(MetricsCodePoint, typeface, {}, text::FontFallback::None);
    auto metrics = metricsFace.face != nullptr
                       ? metricsFace.face->Metrics(size, 1.0f)
                       : Result<text::FontMetrics>::Failure(ErrorCode::NotFound);
    if (!metrics) {
        return Result<void>::Failure(metrics.Error());
    }

    Array<text::TextCluster> clusters(allocator_);
    if (auto reserved = clusters.TryReserve(graphemes.Size()); !reserved) {
        return reserved;
    }

    usize start = 0;
    for (const usize end : graphemes) {
        uint8 level = 0;
        for (const text::BidiRun& run : runs) {
            if (start >= run.start && start < run.start + run.length) {
                level = run.rightToLeft ? 1 : 0;
            }
        }

        const bool hardBreak = end > 0 && value.Data()[end - 1] == LineFeed;
        const bool breakAfter = end < value.ByteCount() && IsBoundary(lineBreaks.View(), end);
        (void)clusters.TryPushBack({end, 0.0f, level, breakAfter || hardBreak, hardBreak});
        start = end;
    }

    // 글리프(시각 순서)를 클러스터에 나눈다. 클러스터 안 위치는 그 클러스터에서 앞선 글리프의
    // advance 합이다.
    struct Pending {
        LabelGlyph glyph;
        uint32 texture;
    };

    Array<Pending> pending(allocator_);
    Array<float> pens(allocator_);
    if (auto sized = pens.TryResize(clusters.Size()); !sized) {
        return sized;
    }

    for (float& pen : pens) {
        pen = 0.0f;
    }

    for (const text::ShapedGlyph& glyph : shaped) {
        if (clusters.Size() == 0) {
            break;
        }

        const usize cluster = ClusterOf(graphemes.View(), glyph.cluster);
        // 강제 줄바꿈 클러스터는 그리지 않고 폭도 없다(UE FTextLayout은 줄을 먼저 나누고 shaping한다 —
        // TextLabel은 줄바꿈 클러스터의 글리프를 받지 않는다).
        if (clusters[cluster].hardBreak) {
            continue;
        }

        auto cached = cache_->FindOrAdd(
            {.face = glyph.face, .glyphIndex = glyph.glyphIndex, .size = size, .scale = 1.0f});
        if (!cached) {
            return Result<void>::Failure(cached.Error());
        }

        const text::CachedGlyph& entry = cached.Value();
        if (entry.valid && entry.rect.width > 0 && entry.rect.height > 0) {
            auto info = cache_->Texture(entry.texture);
            if (!info) {
                return Result<void>::Failure(info.Error());
            }

            const float width = static_cast<float>(info.Value().width);
            const float height = static_cast<float>(info.Value().height);
            const Vec2 extent(static_cast<float>(entry.rect.width),
                              static_cast<float>(entry.rect.height));
            const Vec2 low(static_cast<float>(entry.rect.x) / width,
                           static_cast<float>(entry.rect.y) / height);
            PaintGlyph paint;
            paint.position =
                Vec2(pens[cluster] + glyph.offsetX + static_cast<float>(entry.bearingX),
                     metrics.Value().ascender - static_cast<float>(entry.bearingY) - glyph.offsetY);
            paint.size = extent;
            paint.uv = {low, low + Vec2(extent.x / width, extent.y / height)};
            if (auto added = pending.TryPushBack({{cluster, paint}, entry.texture}); !added) {
                return added;
            }
        }

        pens[cluster] += glyph.advance;
        clusters[cluster].advance += glyph.advance;
    }

    // 새 글리프가 든 cache 텍스처를 GPU에 맞춘 뒤 브러시 handle을 채운다.
    if (auto synced = textures_.Sync(*cache_, maskPipeline_, imagePipeline_); !synced) {
        return synced;
    }

    Array<LabelGlyph> glyphs(allocator_);
    if (auto reserved = glyphs.TryReserve(pending.Size()); !reserved) {
        return reserved;
    }

    for (Pending& item : pending) {
        item.glyph.glyph.resource = textures_.Resource(item.texture);
        (void)glyphs.TryPushBack(item.glyph);
    }

    // TextLabel은 글리프를 논리 클러스터 순서로 받는다(ShapeText는 시각 순서다). 같은 클러스터 안의
    // 순서는 유지한다(안정 삽입 정렬 — 레이블 글리프 수는 작다).
    for (usize index = 1; index < glyphs.Size(); ++index) {
        const LabelGlyph moving = glyphs[index];
        usize slot = index;
        while (slot > 0 && glyphs[slot - 1].clusterIndex > moving.clusterIndex) {
            glyphs[slot] = glyphs[slot - 1];
            --slot;
        }

        glyphs[slot] = moving;
    }

    return label.SetShapedText(value, clusters.View(), glyphs.View(), metrics.Value().lineHeight,
                               false);
}

} // namespace rocks::ui
