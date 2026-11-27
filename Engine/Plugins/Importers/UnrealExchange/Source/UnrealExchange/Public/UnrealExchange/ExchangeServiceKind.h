#pragma once

#include <UnrealExchange/ExchangeImportRun.h>

// 교환 에셋의 종류별 임포터다(Docs/Design/UnrealExchange 10.6절 1–3번, 4.6절 1번, 5.5절 11번): 묶음
// 파일을 원본(역할 asset·blob:<hex>)으로, 옵션(objectPath·formerPaths·references, 메시는
// meshNormals)을 대입 텍스트로 넘겨 서비스 임포트(새 에셋 Import, 기존 에셋 Reimport — 원본 전체
// 교체)를 부른다. 원본·옵션·서비스 호출은 클래스와 무관하고 클래스는 A2f 임포터 이름만 정한다
// (ExchangeServiceImporterName). 에셋 내용은 A2f 임포터(UnrealTextureImporter·
// UnrealStaticMeshImporter)가 만든다.
//
// 사용 예(명령 문맥의 종류별 임포터 등록 hook):
//   ExchangeServiceKind services;
//   unrealContext.kinds = {&RegisterExchangeServiceKinds, &services};
namespace rocks {

// 서비스 임포트로 들이는 교환 클래스 수와 순서다(텍스처 다섯 + 정적 메시 + 스켈레톤 + 스킨 메시).
inline constexpr usize ExchangeServiceClassCount = 8;
inline constexpr ExchangeClass ExchangeServiceClasses[ExchangeServiceClassCount] = {
    ExchangeClass::Texture2D,        ExchangeClass::TextureCube,   ExchangeClass::Texture2DArray,
    ExchangeClass::TextureCubeArray, ExchangeClass::VolumeTexture, ExchangeClass::StaticMesh,
    ExchangeClass::Skeleton,         ExchangeClass::SkeletalMesh};

// 교환 클래스의 A2f 임포터 이름이다(4.6절 1번 표, 5.5절 10번, 6.4절 8번). 표에 없는 클래스는 빈
// view다.
ROCKS_UNREALEXCHANGE_API StringView ExchangeServiceImporterName(ExchangeClass type) noexcept;

class ROCKS_UNREALEXCHANGE_API ExchangeServiceKind final : public ExchangeKindImporter {
public:
    // 실패: 서비스 실패는 묶음 진단(코드·오류 그대로 — 코드가 없으면 InvalidSource, 세부 규칙
    // ServiceFailure, text = 대상 경로)을 더하고 그 오류다. 표에 없는 클래스 InvalidArgument, 할당
    // 실패.
    Result<AssetId> Import(const ExchangeKindRequest& request) noexcept override;
};

// state(ExchangeServiceKind*)를 ExchangeServiceClasses에 등록한다(ExchangeKindSetup 모양). 오류:
// null state InvalidArgument, 표 등록 오류.
ROCKS_UNREALEXCHANGE_API Result<void>
RegisterExchangeServiceKinds(void* state, ExchangeKindTable& table) noexcept;

} // namespace rocks
