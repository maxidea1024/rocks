#pragma once

#include <AssetDatabase/AssetImporter.h>
#include <UnrealExchange/ExchangeBundle.h>

// 교환 텍스처 A2f 임포터다(Docs/Design/UnrealExchange 4.6절, 10.6절): 원본 byte(에셋 JSON — 역할
// asset, blob — 역할 blob:<hex>)를 다시 검사하고 원본 변환·설정 번역·참조·태그로 텍스처 출력을
// 채운다. 종류별 임포터(ExchangeTextureKind)가 서비스 임포트로 부르고, 묶음 파일이 그 자리에 있으면
// 일반 asset-reimport도 같은 결과를 낸다.
//
// 사용 예(임포터 표 등록 — 묶음은 UnrealTextureImporterSet):
//   UnrealTextureImporter importer(ExchangeClass::Texture2D);
//   auto registered = table.Register(importer);
namespace rocks {

// 교환 텍스처 클래스의 임포터 이름이다(4.6절 1번 표). 텍스처 클래스가 아니면 빈 view다.
ROCKS_UNREALEXCHANGE_API StringView UnrealTextureImporterName(ExchangeClass type) noexcept;

// 교환 텍스처 클래스 하나의 임포터다(주 출력 타입이 클래스마다 다르다 — A2f 계약). 수명: 표 등록
// 토큰보다 오래 산다.
class ROCKS_UNREALEXCHANGE_API UnrealTextureImporter final : public AssetImporter {
public:
    // type은 텍스처 다섯 클래스 가운데 하나다(아니면 Info().id가 null이라 등록이 거부한다).
    explicit UnrealTextureImporter(ExchangeClass type) noexcept;

    const AssetImporterInfo& Info() const noexcept override {
        return info_;
    }

    ExchangeClass Class() const noexcept {
        return type_;
    }

    // 4.6절·10.6절 6번 순서로 검사하고 출력을 채운다. 실패(context.Fail — 코드 / 오류 / detail):
    // 문서 읽기 CorruptBundle 또는 InvalidSource(플러그인 error 진단) / 그 오류, 클래스
    // InvalidSource / InvalidArgument, objectPath InvalidImportOption / InvalidArgument, blob 원본
    // 없음 MissingSource / NotFound(detail = blob 번호), 쓰지 않는 blob 원본 InvalidImportOption /
    // InvalidArgument(detail = 원본 번호), blob 검사 CorruptBundle / InvalidArgument(detail = blob
    // 번호), 원본 변환의 코드, 설정 번역·ImportJson InvalidSource / InvalidArgument.
    // 출력·bulk·태그·Registry 오류와 할당 실패는 진단 없이 그 오류다. 경고:
    // UnsupportedSetting(detail = 설정 표 행 번호), GammaMismatch(detail = UE EGammaSpace 번호).
    Result<void> Import(AssetImportContext& context) noexcept override;

private:
    ExchangeClass type_;
    AssetImporterInfo info_;
};

} // namespace rocks
