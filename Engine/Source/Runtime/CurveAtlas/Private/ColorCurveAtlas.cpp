#include <CurveAtlas/ColorCurveAtlas.h>

#include <Base/Compiler.h>

// 색 커브 atlas의 기본 설정과 행 조회다(Docs/Design/CurveAssets 4.2절).
namespace rocks {

ColorCurveAtlas::ColorCurveAtlas(Allocator& allocator) noexcept
    : Texture2D(allocator), curves(allocator) {
    // UE UCurveLinearColorAtlas 생성자(CurveLinearColorAtlas.cpp:14-34) 값이다.
#if ROCKS_WITH_EDITOR_DATA
    mipGenSettings = TextureMipGenSettings::NoMipmaps;
#endif
    filter = TextureFilter::Bilinear;
    srgb = 0;
    addressX = TextureAddress::Clamp;
    addressY = TextureAddress::Clamp;
    compressionSettings = TextureCompressionSettings::HDR;
}

usize ColorCurveAtlas::RowOf(ObjectHandle curve) const noexcept {
    if (curve.IsNull()) {
        return IndexNone;
    }

    const usize at = curves.IndexOf(curve);
    return at == curves.Size() ? IndexNone : at;
}

uint32 ColorCurveAtlas::EffectiveWidth() const noexcept {
    return width < MinColorCurveAtlasWidth ? MinColorCurveAtlasWidth : width;
}

} // namespace rocks
