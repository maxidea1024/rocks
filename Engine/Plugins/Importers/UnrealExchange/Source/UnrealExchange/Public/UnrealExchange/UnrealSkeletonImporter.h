#pragma once

#include <AssetDatabase/AssetImporter.h>
#include <UnrealExchange/ExchangeBundle.h>

// 교환 스켈레톤 A2f 임포터 unreal_skeleton다(Docs/Design/UnrealExchange 6.4절 8번): 원본 byte(에셋
// JSON — 역할 asset, blob — 역할 blob:<hex>)를 다시 검사하고 ExchangeSkeleton 변환으로 만든 명세를
// ImportSkeleton에 넘긴 뒤 태그를 단다. 종류별 임포터(ExchangeServiceKind)가 서비스 임포트로
// 부르고, 묶음 파일이 그 자리에 있으면 일반 asset-reimport도 같은 결과를 낸다.
//
// 사용 예(임포터 표 등록 — 묶음은 UnrealExchangeImporterSet):
//   UnrealSkeletonImporter importer;
//   auto registered = table.Register(importer);
namespace rocks {

// 임포터 이름이다(6.4절 8번).
inline constexpr StringView UnrealSkeletonImporterName = "unreal_skeleton";

// 수명: 표 등록 토큰보다 오래 산다.
class ROCKS_UNREALEXCHANGE_API UnrealSkeletonImporter final : public AssetImporter {
public:
    UnrealSkeletonImporter() noexcept;

    const AssetImporterInfo& Info() const noexcept override {
        return info_;
    }

    // 10.6절 6번 순서로 검사하고 출력을 채운다. 실패(context.Fail — 코드 / 오류 / detail): 문서·
    // 클래스·objectPath·blob 원본은 텍스처 임포터와 같다(UnrealTextureImporter.h), 변환은
    // ExchangeSkeleton::Build의 코드·detail, 서비스(ImportSkeleton)의 진단. 출력·태그·Registry
    // 오류와 할당 실패는 진단 없이 그 오류다. 경고: 변환의 경고와 서비스의 경고.
    Result<void> Import(AssetImportContext& context) noexcept override;

private:
    AssetImporterInfo info_;
};

} // namespace rocks
