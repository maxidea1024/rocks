#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/HashMap.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/SharedPtr.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Result.h>
#include <Platform/Sync.h>
#include <TextLayout/FontAtlas.h>
#include <TextLayout/GlyphGeometry.h>
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
//   // Task: cache.Value()->RunPendingDistanceFields(budget);
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

// 거리장 spread 규칙의 값이다(UE FontCache.cpp 1314–1323·1342–1349행): 안쪽 = clamp(2 화소,
// 0.05, 0.25 em), 바깥 = min(안쪽 + 외곽선, 8 em), 둘 다 거리장 ppem에서 2의 거듭제곱 화소로
// 올린다(최소 2 화소).
inline constexpr float DistanceFieldInnerSpreadPixels = 2.0f;
inline constexpr float MinDistanceFieldInnerSpread = 0.05f;
inline constexpr float MaxDistanceFieldInnerSpread = 0.25f;
inline constexpr float MaxDistanceFieldOuterSpread = 8.0f;
inline constexpr float MinDistanceFieldHalfSpreadPixels = 2.0f;

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

// cache 키다(6.6절). 거리장 모드면 크기·배율·외곽선은 spread를 정하는 데만 쓰고 글리프는 (face,
// 글리프, 채널, ppem, spread)마다 한 번 만든다(UE FSdfGlyphEntryKey — 기울임은 쓰지 않는다).
struct GlyphKey {
    BackendFace* face = nullptr;
    uint32 glyphIndex = 0;
    float size = 0.0f;
    float scale = 1.0f;
    float skew = 0.0f;
    GlyphOutline outline = {};
    FontRasterMode mode = FontRasterMode::Bitmap;
    // 거리장 해상도(face 설정에서 고른 msdfPpem 또는 sdfPpem — 6.5절)다. 거리장 모드에서만 쓴다.
    uint32 distanceFieldPpem = 0;

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
    // 거리장 글리프다. rect는 여백을 뺀 거리장 화소, bearing은 거리장 화소 경계 값이다.
    bool distanceField = false;
    // 자리표시(근사)가 들어 있고 생성 작업이 남았다(RunPendingDistanceFields가 바꾼다).
    bool pending = false;
    // 셰이더가 거리를 되돌리는 em 단위 spread다.
    float emOuterSpread = 0.0f;
    float emInnerSpread = 0.0f;
    // 목표 크기의 화소 단위 사각형이다(가장자리 반 화소를 뺀다 — UE FinalizeEntry).
    float quadBearingX = 0.0f;
    float quadBearingY = 0.0f;
    float quadWidth = 0.0f;
    float quadHeight = 0.0f;
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
    // backend는 거리장 생성에 쓴다(없으면 거리장 모드도 비트맵으로 그린다). backend는 cache보다
    // 오래 살아야 한다.
    static Result<UniquePtr<GlyphCache>> TryCreate(Allocator& allocator,
                                                   const GlyphCacheSettings& settings,
                                                   const TextBackend* backend = nullptr) noexcept;

    // 키의 글리프를 찾고, 없으면 래스터해 넣는다. 래스터·배치 실패(글리프 범위 밖, FreeType 실패,
    // 너무 큰 글리프, 텍스처 수 상한)는 valid = false로 기억해 낸다. face가 없거나 크기·배율이 0
    // 이하면 InvalidArgument, 할당 실패는 OutOfMemory(cache는 그대로)다. face는 cache를 비울
    // 때까지 살아야 한다.
    Result<CachedGlyph> FindOrAdd(const GlyphKey& key) noexcept;

    // 남은 거리장 생성 작업을 최대 maxJobs개 하고 한 수를 낸다. 생성은 cache 잠금 밖에서 하므로
    // 호스트 Task가 부른다. 생성 중 flush되면 결과를 버린다. 할당 실패는 OutOfMemory다.
    Result<uint32> RunPendingDistanceFields(uint32 maxJobs) noexcept;

    uint32 PendingDistanceFieldCount() const noexcept;

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

    GlyphCache(Allocator& allocator, const GlyphCacheSettings& settings,
               const TextBackend* backend) noexcept;

private:
    struct KeyHash {
        uint64 operator()(const GlyphKey& key) const noexcept;
    };

    struct DistanceFieldKey {
        BackendFace* face = nullptr;
        uint32 glyphIndex = 0;
        bool multichannel = true;
        uint32 ppem = 0;
        float emOuterSpread = 0.0f;
        float emInnerSpread = 0.0f;

        friend constexpr bool operator==(const DistanceFieldKey&,
                                         const DistanceFieldKey&) noexcept = default;
    };

    struct DistanceFieldKeyHash {
        uint64 operator()(const DistanceFieldKey& key) const noexcept;
    };

    struct GeometryKey {
        BackendFace* face = nullptr;
        uint32 glyphIndex = 0;

        friend constexpr bool operator==(const GeometryKey&, const GeometryKey&) noexcept = default;
    };

    struct GeometryKeyHash {
        uint64 operator()(const GeometryKey& key) const noexcept;
    };

    struct DistanceFieldEntry {
        // 거짓이면 거리장을 만들 수 없는 글리프다(비트맵으로 그린다 — UE bSupportsSdf).
        bool supported = false;
        CachedGlyph glyph = {};
    };

    struct DistanceFieldJob {
        DistanceFieldKey key = {};
        SharedPtr<const GlyphGeometry> geometry = {};
        DistanceFieldRequest request = {};
        uint32 texture = 0;
        AtlasRect inner = {};
        uint64 generation = 0;
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

    Result<CachedGlyph> FindOrAddBitmap(const GlyphKey& key) noexcept;
    Result<CachedGlyph> FindOrAddDistanceField(const GlyphKey& key) noexcept;
    Result<SharedPtr<const GlyphGeometry>> GeometryOf(BackendFace& face,
                                                      uint32 glyphIndex) noexcept;
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
    const TextBackend* backend_;
    mutable platform::Mutex mutex_;
    HashMap<GlyphKey, CachedGlyph, KeyHash> glyphs_;
    HashMap<DistanceFieldKey, DistanceFieldEntry, DistanceFieldKeyHash> distanceFields_;
    // 값이 비면 geometry를 만들 수 없는 글리프다.
    HashMap<GeometryKey, SharedPtr<const GlyphGeometry>, GeometryKeyHash> geometries_;
    Array<DistanceFieldJob> pendingJobs_;
    // flush마다 늘어 생성 중이던 작업의 결과를 버리게 한다.
    uint64 flushGeneration_ = 0;
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
