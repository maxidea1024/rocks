#include <UnrealExchange/UnrealTextureImporterSet.h>

// 교환 텍스처 A2f 임포터 묶음이다(Docs/Design/UnrealExchange 4.6절 1번).

namespace rocks {

UnrealTextureImporterSet::UnrealTextureImporterSet() noexcept
    : importers_{UnrealTextureImporter(ExchangeClass::Texture2D),
                 UnrealTextureImporter(ExchangeClass::TextureCube),
                 UnrealTextureImporter(ExchangeClass::Texture2DArray),
                 UnrealTextureImporter(ExchangeClass::TextureCubeArray),
                 UnrealTextureImporter(ExchangeClass::VolumeTexture)} {
}

Result<void> RegisterUnrealTextureImporters(void* state, AssetImporterTable& table,
                                            Array<AssetImporterRegistration>& tokens) noexcept {
    if (state == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto& importers = *static_cast<UnrealTextureImporterSet*>(state);
    if (auto reserved = tokens.TryReserve(tokens.Size() + UnrealTextureImporterCount); !reserved) {
        return reserved;
    }

    for (const ExchangeClass type : UnrealTextureClasses) {
        auto registered = table.Register(importers.At(type));
        if (!registered) {
            return Result<void>::Failure(registered.Error());
        }

        (void)tokens.TryPushBack(rocks::Move(registered.Value()));
    }

    return Result<void>::Success();
}

} // namespace rocks
