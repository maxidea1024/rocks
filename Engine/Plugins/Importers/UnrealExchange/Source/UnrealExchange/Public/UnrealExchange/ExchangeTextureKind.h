#pragma once

#include <UnrealExchange/ExchangeImportRun.h>

// 교환 텍스처의 종류별 임포터다(Docs/Design/UnrealExchange 10.6절 1–3번, 4.6절 1번): 묶음 파일을
// 원본(역할 asset·blob:<hex>)으로, 옵션(objectPath·formerPaths·references)을 대입 텍스트로 넘겨
// 서비스 임포트(새 에셋 Import, 기존 에셋 Reimport — 원본 전체 교체)를 부른다. 에셋 내용은 A2f
// 임포터 UnrealTextureImporter가 만든다.
//
// 사용 예(명령 문맥의 종류별 임포터 등록 hook):
//   ExchangeTextureKind textures;
//   unrealContext.kinds = {&RegisterExchangeTextureKinds, &textures};
namespace rocks {

class ROCKS_UNREALEXCHANGE_API ExchangeTextureKind final : public ExchangeKindImporter {
public:
    // 실패: 서비스 실패는 묶음 진단(코드·오류 그대로 — 코드가 없으면 InvalidSource, 세부 규칙
    // ServiceFailure, text = 대상 경로)을 더하고 그 오류다. 텍스처가 아닌 클래스 InvalidArgument,
    // 할당 실패.
    Result<AssetId> Import(const ExchangeKindRequest& request) noexcept override;
};

// state(ExchangeTextureKind*)를 텍스처 다섯 클래스에 등록한다(ExchangeKindSetup 모양). 오류: null
// state InvalidArgument, 표 등록 오류.
ROCKS_UNREALEXCHANGE_API Result<void> RegisterExchangeTextureKinds(void* state,
                                                                   ExchangeKindTable& table) noexcept;

} // namespace rocks
