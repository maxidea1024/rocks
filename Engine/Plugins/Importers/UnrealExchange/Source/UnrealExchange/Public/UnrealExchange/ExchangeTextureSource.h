#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Texture/TextureEnums.h>
#include <UnrealExchange/ExchangeBundle.h>

// 교환 텍스처 원본 변환이다(Docs/Design/UnrealExchange 4.6절 2번): 에셋 JSON의 data.source(4.5절
// 1번)와 blob byte에서 Rocks TextureSource 값(형식·크기·slice)과 픽셀 byte를 만든다. 첫 mip만
// 쓰고(Rocks 원본 mip은 1), UE 원본 형식 9종을 Rocks 형식으로 옮긴다(BGRA8 → RGBA8 교환, BGRE8 →
// RGBA32F 해제).
//
// 사용 예(A2f 임포터 — blobBytes는 asset.blobs 순으로 검사를 마친 byte):
//   ExchangeTextureSource source(allocator);
//   ExchangeTextureFailure failure;
//   if (auto converted = ConvertExchangeTextureSource(asset, blobBytes, source, failure);
//       !converted) {
//       return context.Fail(failure.code, converted.Error());
//   }
namespace rocks {

// 원본 층의 감마 공간이다(UE EGammaSpace와 같은 순서·번호 — Linear 0, Pow22 1, sRGB 2. 4.5절 1번
// layers[].gammaSpace, 4.6절 4번 대조 — GammaMismatch detail).
enum class ExchangeGammaSpace : uint8 {
    Linear = 0,
    Pow22 = 1,
    Srgb = 2
};

// 변환한 원본이다.
struct ExchangeTextureSource {
    explicit ExchangeTextureSource(Allocator& allocator) noexcept : pixels(allocator) {
    }

    TextureSourceFormat format = TextureSourceFormat::Invalid;
    uint32 sizeX = 0;
    uint32 sizeY = 0;
    uint32 numSlices = 0;
    // 층 0의 감마 공간이다.
    ExchangeGammaSpace gamma = ExchangeGammaSpace::Linear;
    // slice 순, slice 안 행 우선, 패딩 없는 Rocks 형식 texel이다.
    Array<uint8> pixels;
};

// 거부 이유다(4.6절 2번 — 코드와 오류).
struct ExchangeTextureFailure {
    AssetDiagnosticCode code = AssetDiagnosticCode::None;
    ErrorCode error = ErrorCode::None;
};

// asset(Ready, 텍스처 클래스)의 data.source를 out에 변환한다. blobBytes는 asset.blobs와 같은 순서의
// blob byte다(VerifyExchangeBlob을 마친 것 — byte 수는 형식과 다시 대조한다). 검사 순서와 거부(코드
// / 오류)는 4.6절 2번이다: 형식 위반 CorruptBundle / CorruptData, 다층·UDIM·위도경도
// UnsupportedSourceLayout / Unsupported, 클래스 slice 규칙 InvalidSource / CorruptData, 크기
// 상한 (ImageCodec ImageDecodeLimits 기본 — 파일 임포트와 같다) InvalidSource / CapacityExceeded.
//
// 반환: 거부면 failure를 채우고 그 오류, 할당 실패는 OutOfMemory(failure.code None)다. 실패하면
// out은 빈 상태다.
ROCKS_UNREALEXCHANGE_API Result<void>
ConvertExchangeTextureSource(const ExchangeAsset& asset, Span<const Span<const uint8>> blobBytes,
                             ExchangeTextureSource& out, ExchangeTextureFailure& failure) noexcept;

} // namespace rocks
