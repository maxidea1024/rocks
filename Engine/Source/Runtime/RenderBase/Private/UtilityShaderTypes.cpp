#include <RenderBase/UtilityShaderTypes.h>

// storage image 형식 대응이다(Docs/Design/EngineShaders 14.4절 4·5항).
namespace rocks::render {

Result<StorageImageFormat> StorageImageFormatOf(graphics::PixelFormat format) noexcept {
    using graphics::PixelFormat;
    switch (format) {
    case PixelFormat::RGBA8Unorm:
        return Result<StorageImageFormat>::Success(StorageImageFormat::Rgba8);
    case PixelFormat::RGBA8Snorm:
        return Result<StorageImageFormat>::Success(StorageImageFormat::Rgba8Snorm);
    case PixelFormat::RGBA16Float:
        return Result<StorageImageFormat>::Success(StorageImageFormat::Rgba16f);
    case PixelFormat::RGBA32Float:
        return Result<StorageImageFormat>::Success(StorageImageFormat::Rgba32f);
    case PixelFormat::R32Float:
        return Result<StorageImageFormat>::Success(StorageImageFormat::R32f);
    case PixelFormat::R32Uint:
        return Result<StorageImageFormat>::Success(StorageImageFormat::R32ui);
    case PixelFormat::R32Sint:
        return Result<StorageImageFormat>::Success(StorageImageFormat::R32i);
    case PixelFormat::RGBA32Uint:
        return Result<StorageImageFormat>::Success(StorageImageFormat::Rgba32ui);
    case PixelFormat::RG16Float:
        return Result<StorageImageFormat>::Success(StorageImageFormat::Rg16f);
    case PixelFormat::R16Float:
        return Result<StorageImageFormat>::Success(StorageImageFormat::R16f);
    case PixelFormat::RG32Float:
        return Result<StorageImageFormat>::Success(StorageImageFormat::Rg32f);
    case PixelFormat::RG11B10Ufloat:
        return Result<StorageImageFormat>::Success(StorageImageFormat::R11g11b10f);
    default:
        return Result<StorageImageFormat>::Failure(ErrorCode::Unsupported);
    }
}

} // namespace rocks::render
