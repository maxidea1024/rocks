#pragma once

#include <UnrealExchange/UnrealSkeletalMeshImporter.h>
#include <UnrealExchange/UnrealSkeletonImporter.h>
#include <UnrealExchange/UnrealStaticMeshImporter.h>
#include <UnrealExchange/UnrealTextureImporter.h>

// 교환 A2f 임포터 묶음이다(Docs/Design/UnrealExchange 4.6절 1번 텍스처 다섯, 5.5절 10번 정적
// 메시, 6.4절 8번 스켈레톤·스킨 메시). 호스트가 명령 동안 쥐고 임포터 표 등록
// hook(AssetImporterSetup 모양)에 넘긴다.
//
// 사용 예(제품 임포터 묶음):
//   UnrealExchangeImporterSet unrealImporters;
//   auto registered = RegisterUnrealExchangeImporters(&unrealImporters, table, tokens);
namespace rocks {

// 묶음의 임포터 수다(텍스처 다섯 + 정적 메시 + 스켈레톤 + 스킨 메시).
inline constexpr usize UnrealExchangeImporterCount = UnrealTextureImporterCount + 3;

class ROCKS_UNREALEXCHANGE_API UnrealExchangeImporterSet {
public:
    UnrealExchangeImporterSet() noexcept;

    // 텍스처 클래스의 임포터다(전제: IsExchangeTextureClass(type)).
    UnrealTextureImporter& TextureImporter(ExchangeClass type) noexcept {
        return textures_[static_cast<usize>(type) - static_cast<usize>(ExchangeClass::Texture2D)];
    }

    UnrealStaticMeshImporter& StaticMeshImporter() noexcept {
        return staticMesh_;
    }

    UnrealSkeletonImporter& SkeletonImporter() noexcept {
        return skeleton_;
    }

    UnrealSkeletalMeshImporter& SkeletalMeshImporter() noexcept {
        return skeletalMesh_;
    }

private:
    UnrealTextureImporter textures_[UnrealTextureImporterCount];
    UnrealStaticMeshImporter staticMesh_;
    UnrealSkeletonImporter skeleton_;
    UnrealSkeletalMeshImporter skeletalMesh_;
};

// state(UnrealExchangeImporterSet*)의 임포터를 모두 table에 등록하고 토큰을 tokens에 둔다. 옵션
// 타입(UnrealExchangeImportOptions)과 출력 타입(텍스처·StaticMesh·Skeleton·SkeletalMesh)이 table의
// TypeRegistry에 있어야 한다(없으면 NotFound). 오류: null state InvalidArgument, 등록 오류, 할당
// 실패.
ROCKS_UNREALEXCHANGE_API Result<void>
RegisterUnrealExchangeImporters(void* state, AssetImporterTable& table,
                                Array<AssetImporterRegistration>& tokens) noexcept;

} // namespace rocks
