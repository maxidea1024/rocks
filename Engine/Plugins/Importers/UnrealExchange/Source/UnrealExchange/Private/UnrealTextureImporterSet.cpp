#include <UnrealExchange/UnrealTextureImporterSet.h>

// 교환 텍스처 A2f 임포터 묶음이다(Docs/Design/UnrealExchange 4.6절 1번).

namespace rocks {

namespace unreal_texture_importer_set {

// 등록 순서(4.6절 1번 표 순서)다.
constexpr ExchangeClass TextureClasses[UnrealTextureImporterCount] = {
    ExchangeClass::Texture2D, ExchangeClass::TextureCube, ExchangeClass::Texture2DArray,
    ExchangeClass::TextureCubeArray, ExchangeClass::VolumeTexture};

} // namespace unreal_texture_importer_set

UnrealTextureImporterSet::UnrealTextureImporterSet() noexcept
    : importers_{UnrealTextureImporter(ExchangeClass::Texture2D),
                 UnrealTextureImporter(ExchangeClass::TextureCube),
                 UnrealTextureImporter(ExchangeClass::Texture2DArray),
                 UnrealTextureImporter(ExchangeClass::TextureCubeArray),
                 UnrealTextureImporter(ExchangeClass::VolumeTexture)} {
}

Result<void> RegisterUnrealTextureImporters(void* state, AssetImporterTable& table,
                                            Array<AssetImporterRegistration>& tokens) noexcept {
    using namespace unreal_texture_importer_set;
    if (state == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto& importers = *static_cast<UnrealTextureImporterSet*>(state);
    if (auto reserved = tokens.TryReserve(tokens.Size() + UnrealTextureImporterCount); !reserved) {
        return reserved;
    }

    for (const ExchangeClass type : TextureClasses) {
        auto registered = table.Register(importers.At(type));
        if (!registered) {
            return Result<void>::Failure(registered.Error());
        }

        (void)tokens.TryPushBack(rocks::Move(registered.Value()));
    }

    return Result<void>::Success();
}

} // namespace rocks
