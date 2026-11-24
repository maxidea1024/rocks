#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/HashMap.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Result.h>
#include <Platform/Sync.h>
#include <TextLayout/FontAtlas.h>
#include <TextLayout/GlyphRaster.h>
#include <TextLayout/TextBackend.h>

// 글리프 cache의 CPU 부분이다(Docs/Design/Font 6.6·6.8절 FT2d — UE FSlateFontCache의 atlas·flush
// 부분). 글리프를 래스터해 내용 종류별 atlas 페이지(또는 atlas 밖 텍스처)에 넣고, 페이지가 늘면
// UE와 같은 규칙으로 flush를 요청해 다음 프레임 경계에서 비운다. GPU 업로드는 소비자(UiToolkit
// Render lane)가 Texture·TakeDirtyRegion으로 한다.
//
// 사용 예:
//   auto cache = text::GlyphCache::TryCreate(allocator, {});
//   auto glyph = cache.Value()->FindOrAdd({.face = shaped.face, .glyphIndex = shaped.glyphIndex,
//                                          .size = 24.0f, .scale = 1.0f});
//   // 프레임 끝: if (cache.Value()->AdvanceFrame()) { /* 텍스처를 다시 올린다 */ }
namespace rocks::text {

// atlas 크기 기본값·범위(화소)다(UE SlateRenderingManager 37–43행).
inline constexpr uint32 DefaultAlphaAtlasSize = 1024;
inline constexpr uint32 EditorAlphaAtlasSize = 2048;
inline constexpr uint32 DefaultColorAtlasSize = 512;
inline constexpr uint32 DefaultDistanceFieldAtlasSize = 512;
inline constexpr uint32 EditorDistanceFieldAtlasSize = 1024;
inline constexpr uint32 MinFontAtlasSize = 128;
inline constexpr uint32 MaxFontAtlasSize = 2048;

// flush 한도·성장 창 기본값이다(UE TextureAtlas.h 281–287행 — 모두 1).
inline constexpr int32 DefaultMaxAtlasPagesBeforeFlush = 1;
inline constexpr int32 DefaultMaxNonAtlasTexturesBeforeFlush = 1;
inline constexpr int32 DefaultGrowAtlasFrameWindow = 1;
inline constexpr int32 DefaultGrowNonAtlasFrameWindow = 1;

// 텍스처(페이지 + atlas 밖) 수 상한이다(UE uint8 텍스처 번호).
inline constexpr uint32 MaxFontTextures = 255;

struct GlyphCacheSettings {
    // 종류별 atlas 한 변이다(MinFontAtlasSize–MaxFontAtlasSize로 자른다).
    uint32 alphaAtlasSize = DefaultAlphaAtlasSize;
    uint32 colorAtlasSize = DefaultColorAtlasSize;
    uint32 distanceFieldAtlasSize = DefaultDistanceFieldAtlasSize;
    int32 maxAtlasPagesBeforeFlush = DefaultMaxAtlasPagesBeforeFlush;
    int32 maxNonAtlasTexturesBeforeFlush = DefaultMaxNonAtlasTexturesBeforeFlush;
    int32 growAtlasFrameWindow = DefaultGrowAtlasFrameWindow;
    int32 growNonAtlasFrameWindow = DefaultGrowNonAtlasFrameWindow;
    // 거짓이면 MONO로 래스터한다(ui.font.antialiasing).
    bool antialiasing = true;
    // UE Slate.OutlineFontRenderMethod = 1이다.
    bool spanRasterizer = false;
};

// cache 키다(6.6절). 거리장 키(ppem·spread)는 FT2e에서 더한다.
struct GlyphKey {
    BackendFace* face = nullptr;
    uint32 glyphIndex = 0;
    float size = 0.0f;
    float scale = 1.0f;
    float skew = 0.0f;
    GlyphOutline outline = {};

    friend constexpr bool operator==(const GlyphKey&, const GlyphKey&) noexcept = default;
};

struct CachedGlyph {
    // 거짓이면 래스터·배치에 실패한 글리프다(그리지 않는다 — UE Valid).
    bool valid = false;
    FontAtlasContent content = FontAtlasContent::Alpha;
    // Texture(index)의 번호다.
    uint32 texture = 0;
    // 텍스처 안의 사각형이다(atlas면 여백 포함 칸).
    AtlasRect rect = {};
    // 펜 위치에서 rect 왼쪽 위까지의 거리다(y는 위가 양수, 여백만큼 보정 — UE와 같다).
    int32 bearingX = 0;
    int32 bearingY = 0;
    float bitmapRenderScale = 1.0f;
    bool supportsOutline = false;
};

struct FontTextureInfo {
    FontAtlasContent content = FontAtlasContent::Alpha;
    // 거짓이면 글리프 하나뿐인 atlas 밖 텍스처다.
    bool atlas = true;
    uint32 width = 0;
    uint32 height = 0;
    // 다음 FindOrAdd·AdvanceFrame·RequestFlush 전까지 유효하다.
    Span<const uint8> pixels = {};
};

struct GlyphCacheStats {
    usize glyphs = 0;
    uint32 alphaPages = 0;
    uint32 colorPages = 0;
    uint32 distanceFieldPages = 0;
    uint32 nonAtlasTextures = 0;
    usize textureBytes = 0;
};

class ROCKS_TEXTLAYOUT_API GlyphCache {
public:
    static Result<UniquePtr<GlyphCache>> TryCreate(Allocator& allocator,
                                                   const GlyphCacheSettings& settings) noexcept;

    // 키의 글리프를 찾고, 없으면 래스터해 넣는다. 래스터·배치 실패(글리프 범위 밖, FreeType 실패,
    // 너무 큰 글리프, 텍스처 수 상한)는 valid = false로 기억해 낸다. face가 없거나 크기·배율이 0
    // 이하면 InvalidArgument, 할당 실패는 OutOfMemory(cache는 그대로)다. face는 cache를 비울
    // 때까지 살아야 한다.
    Result<CachedGlyph> FindOrAdd(const GlyphKey& key) noexcept;

    uint32 TextureCount() const noexcept;

    // 텍스처 정보다. 번호가 범위 밖이면 InvalidArgument다.
    Result<FontTextureInfo> Texture(uint32 index) const noexcept;

    // 텍스처의 마지막으로 가져간 뒤 바뀐 사각형이다(새 텍스처는 전체). 없으면 false다.
    bool TakeDirtyRegion(uint32 index, AtlasRect& out) noexcept;

    // 다음 AdvanceFrame에서 비우게 한다(ui.font.flush_cache — UE Slate.FlushFontCache).
    void RequestFlush() noexcept;

    // 프레임 경계다. 요청된 flush를 하고 했으면 true다(face를 닫는 것은 face cache 소유자 몫).
    bool AdvanceFrame() noexcept;

    GlyphCacheStats Stats() const noexcept;

    GlyphCache(Allocator& allocator, const GlyphCacheSettings& settings) noexcept;

private:
    struct KeyHash {
        uint64 operator()(const GlyphKey& key) const noexcept;
    };

    struct FontTexture {
        explicit FontTexture(Allocator& allocator) noexcept : pixels(allocator) {
        }

        FontAtlasContent content = FontAtlasContent::Alpha;
        UniquePtr<FontAtlasPage> page = {};
        // atlas 밖 텍스처의 화소다.
        Array<uint8> pixels;
        uint32 width = 0;
        uint32 height = 0;
        bool dirty = false;
    };

    // 한 종류의 flush 계수다(UE FSlateFlushableAtlasCache).
    struct FlushCounter {
        int32 currentMax = 0;
    };

    Result<CachedGlyph> Place(const GlyphBitmap& bitmap) noexcept;
    Result<bool> AddTexture(FontTexture&& texture, uint32& outIndex) noexcept;
    Array<uint32>& PagesOf(FontAtlasContent content) noexcept;
    uint32 AtlasSizeOf(FontAtlasContent content) const noexcept;
    void UpdateFlushCounters() noexcept;
    bool UpdateCounter(int32 current, FlushCounter& counter, int32 initialMax,
                       int32 frameWindow) noexcept;
    void ResetFlushCounters() noexcept;
    void Flush() noexcept;

    Allocator& allocator_;
    GlyphCacheSettings settings_;
    mutable platform::Mutex mutex_;
    HashMap<GlyphKey, CachedGlyph, KeyHash> glyphs_;
    Array<FontTexture> textures_;
    Array<uint32> alphaPages_;
    Array<uint32> colorPages_;
    Array<uint32> distanceFieldPages_;
    Array<uint32> nonAtlasTextures_;
    FlushCounter alphaCounter_;
    FlushCounter colorCounter_;
    FlushCounter distanceFieldCounter_;
    FlushCounter nonAtlasCounter_;
    uint64 frame_ = 0;
    uint64 lastFlushRequestFrame_ = 0;
    bool flushRequested_ = false;
};

} // namespace rocks::text
