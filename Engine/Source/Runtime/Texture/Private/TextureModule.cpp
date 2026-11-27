#include <Core/Module/ModuleEntry.h>
#include <GraphicsTypes/PixelFormat.h>
#include <Texture/Texture.h>
#include <Texture/Texture.module.generated.h>

// Texture 모듈의 진입점과 등록·값 검증 hook이다(Docs/Design/ModuleLoader.ko.md 3.2절, AssetSystem
// 12.8절). 시작할 일이 없다.
ROCKS_IMPLEMENT_MODULE(rocks::DefaultModuleImplementation, Texture)

namespace rocks {

using graphics::GetPixelFormatInfo;
using graphics::PixelFormat;
using graphics::PixelFormatFromNumber;
using graphics::PixelFormatKind;

Result<void> RegisterTextureTypes(ModuleRegistration& registration) noexcept {
    return generated::Texture::RegisterModuleTypes(registration);
}

uint32 TextureSourceTexelBytes(TextureSourceFormat format) noexcept {
    switch (format) {
    case TextureSourceFormat::Invalid:
        return 0;
    case TextureSourceFormat::G8:
        return 1;
    case TextureSourceFormat::GA8:
    case TextureSourceFormat::G16:
    case TextureSourceFormat::R16F:
        return 2;
    case TextureSourceFormat::GA16:
    case TextureSourceFormat::RGBA8:
    case TextureSourceFormat::R32F:
        return 4;
    case TextureSourceFormat::RGBA16:
    case TextureSourceFormat::RGBA16F:
        return 8;
    case TextureSourceFormat::RGBA32F:
        return 16;
    }

    return 0;
}

Result<void> TextureSource::Validate(const TextureSource& source) noexcept {
    if (source.format == TextureSourceFormat::Invalid) {
        return Result<void>::Success();
    }

    if (source.sizeX == 0 || source.sizeY == 0 || source.numSlices == 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<void>::Success();
}

Result<void> TexturePlatformData::Validate(const TexturePlatformData& data) noexcept {
    // 0은 아직 쿠킹하지 않은 데이터다(에디터 패키지의 기본값).
    if (data.pixelFormat == 0) {
        return Result<void>::Success();
    }

    // 번호는 쿠킹 데이터의 저장 형식이다 — 모르는 번호와 색이 아닌 형식(깊이·다중 plane)은
    // 거부한다. 비압축 색과 블록 압축 색(BC·ASTC·ETC2 — AssetSystem 12.17.5)을 받는다.
    auto format = graphics::PixelFormatFromNumber(data.pixelFormat);
    const graphics::PixelFormatKind kind = format
                                               ? graphics::GetPixelFormatInfo(format.Value()).kind
                                               : graphics::PixelFormatKind::Unknown;
    if (kind != graphics::PixelFormatKind::Color && kind != graphics::PixelFormatKind::Compressed) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<void>::Success();
}

} // namespace rocks
