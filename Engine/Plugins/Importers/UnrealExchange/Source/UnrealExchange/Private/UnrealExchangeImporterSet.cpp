#include <UnrealExchange/UnrealExchangeImporterSet.h>

// 교환 A2f 임포터 묶음이다(Docs/Design/UnrealExchange 4.6절 1번, 5.5절 10번).

namespace rocks {

UnrealExchangeImporterSet::UnrealExchangeImporterSet() noexcept
    : textures_{UnrealTextureImporter(ExchangeClass::Texture2D),
                UnrealTextureImporter(ExchangeClass::TextureCube),
                UnrealTextureImporter(ExchangeClass::Texture2DArray),
                UnrealTextureImporter(ExchangeClass::TextureCubeArray),
                UnrealTextureImporter(ExchangeClass::VolumeTexture)} {
}

Result<void> RegisterUnrealExchangeImporters(void* state, AssetImporterTable& table,
                                             Array<AssetImporterRegistration>& tokens) noexcept {
    if (state == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto& importers = *static_cast<UnrealExchangeImporterSet*>(state);
    if (auto reserved = tokens.TryReserve(tokens.Size() + UnrealExchangeImporterCount); !reserved) {
        return reserved;
    }

    AssetImporter* all[UnrealExchangeImporterCount] = {};
    for (usize index = 0; index < UnrealTextureImporterCount; ++index) {
        all[index] = &importers.TextureImporter(UnrealTextureClasses[index]);
    }

    all[UnrealTextureImporterCount] = &importers.StaticMeshImporter();
    all[UnrealTextureImporterCount + 1] = &importers.SkeletonImporter();
    all[UnrealTextureImporterCount + 2] = &importers.SkeletalMeshImporter();
    for (AssetImporter* importer : all) {
        auto registered = table.Register(*importer);
        if (!registered) {
            return Result<void>::Failure(registered.Error());
        }

        (void)tokens.TryPushBack(rocks::Move(registered.Value()));
    }

    return Result<void>::Success();
}

} // namespace rocks
