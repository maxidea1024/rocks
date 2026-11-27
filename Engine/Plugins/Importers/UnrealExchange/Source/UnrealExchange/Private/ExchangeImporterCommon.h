#pragma once

#include <AssetDatabase/AssetImporter.h>
#include <UnrealExchange/ExchangeBundle.h>
#include <UnrealExchange/UnrealExchangeImportOptions.h>

// 교환 A2f 임포터들(텍스처·정적 메시)이 같이 쓰는 검사 단계다(Docs/Design/UnrealExchange 10.6절
// 5–7번). UnrealExchange 모듈 안에서만 쓴다.
namespace rocks::exchange_detail {

// 오류가 할당 실패면 그대로, 아니면 진단 code로 실패한다.
Result<void> FailImport(AssetImportContext& context, AssetDiagnosticCode code, ErrorCode error,
                        uint64 detail = 0) noexcept;

// 10.6절 6번 1–3: 문서(고정 역할 asset)를 읽고 클래스와 옵션 objectPath를 대조한다. 성공하면
// asset이 문서를 가지고 options가 문맥의 옵션을 가리킨다. 실패(context.Fail): 문서 없음
// MissingSource / NotFound, 문서 읽기 CorruptBundle 또는 플러그인 error 진단의 코드 / 그 오류,
// 클래스가 expected가 아님 InvalidSource / InvalidArgument, objectPath 불일치 InvalidImportOption /
// InvalidArgument.
Result<void> ReadImportDocument(AssetImportContext& context, ExchangeClass expected,
                                ExchangeAsset& asset,
                                const UnrealExchangeImportOptions*& options) noexcept;

// 10.6절 6번 4·5: 문서 blob 참조마다 역할 `blob:<hex>` 원본 byte를 찾고 검사한다(out[i] =
// asset.blobs[i]의 byte). 실패: 원본 없음 MissingSource / NotFound(detail = blob 번호), 쓰지 않는
// blob 원본 InvalidImportOption / InvalidArgument(detail = 원본 번호), 검사 CorruptBundle / 그
// 오류(detail = blob 번호).
Result<void> CollectImportBlobs(AssetImportContext& context, const ExchangeAsset& asset,
                                Array<Span<const uint8>>& out) noexcept;

// 10.6절 7번: ObjectPath, FormerPaths = 옵션 ∪ 옛 FormerPaths ∪ 옛 ObjectPath(지금과 다르면).
Result<void> AddExchangeTags(AssetImportContext& context,
                             const UnrealExchangeImportOptions& options) noexcept;

} // namespace rocks::exchange_detail
