#include <Core/Module/ModuleEntry.h>
#include <GraphicsTypes/PixelFormat.h>
#include <Texture/Texture.h>
#include <Texture/Texture.module.generated.h>

// Texture 모듈의 진입점과 등록·값 검증 hook이다(Docs/Design/ModuleLoader.ko.md 3.2절, AssetSystem
// 12.8절). 시작할 일이 없다.
ROCKS_IMPLEMENT_MODULE(rocks::DefaultModuleImplementation, Texture)

namespace rocks {

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

    // 번호는 쿠킹 데이터의 저장 형식이다 — 모르는 번호·비압축 색이 아닌 형식은 거부한다(블록 압축은
    // 압축 단계에서 받는다).
    auto format = PixelFormatFromNumber(data.pixelFormat);
    if (!format || GetPixelFormatInfo(format.Value()).kind != PixelFormatKind::Color) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<void>::Success();
}

} // namespace rocks
