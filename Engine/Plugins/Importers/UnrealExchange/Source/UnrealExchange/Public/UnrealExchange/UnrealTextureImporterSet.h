#pragma once

#include <UnrealExchange/UnrealTextureImporter.h>

// 교환 텍스처 A2f 임포터 다섯의 묶음이다(Docs/Design/UnrealExchange 4.6절 1번). 호스트가 명령 동안
// 쥐고 임포터 표 등록 hook(AssetImporterSetup 모양)에 넘긴다.
//
// 사용 예(제품 임포터 묶음):
//   UnrealTextureImporterSet unrealTextures;
//   auto registered = RegisterUnrealTextureImporters(&unrealTextures, table, tokens);
namespace rocks {

// 교환 텍스처 클래스 수다(Texture2D·TextureCube·Texture2DArray·TextureCubeArray·VolumeTexture).
inline constexpr usize UnrealTextureImporterCount = 5;

class ROCKS_UNREALEXCHANGE_API UnrealTextureImporterSet {
public:
    UnrealTextureImporterSet() noexcept;

    // 텍스처 클래스의 임포터다(전제: IsExchangeTextureClass(type)).
    UnrealTextureImporter& At(ExchangeClass type) noexcept {
        return importers_[static_cast<usize>(type) - static_cast<usize>(ExchangeClass::Texture2D)];
    }

private:
    UnrealTextureImporter importers_[UnrealTextureImporterCount];
};

// state(UnrealTextureImporterSet*)의 임포터 다섯을 table에 등록하고 토큰을 tokens에 둔다. 옵션
// 타입(UnrealExchangeImportOptions)과 텍스처 타입이 table의 TypeRegistry에 있어야 한다(없으면
// NotFound). 오류: null state InvalidArgument, 등록 오류, 할당 실패.
ROCKS_UNREALEXCHANGE_API Result<void>
RegisterUnrealTextureImporters(void* state, AssetImporterTable& table,
                               Array<AssetImporterRegistration>& tokens) noexcept;

} // namespace rocks
