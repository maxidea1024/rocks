#pragma once

#include <Core/Encoding/Sha256.h>
#include <Meta/Annotations.h>

namespace rocks {

// 패키지 Bulk 섹션의 entry 하나를 가리키는 영속 값이다(Docs/Design/AssetSystem.ko.md 6.1절). 대용량
// 데이터(텍스처 원본·mip 등)는 리플렉션 필드가 아니라 Bulk 섹션에 두고, 객체는 이 값을 필드로
// 갖는다. 파일 offset은 BulkIndex만 알며 이 값은 bulkId·raw 크기·raw SHA-256만 담는다.
//
// - bulkId: 1부터의 패키지 안 번호다. 0은 "bulk 없음"(빈 값)이다. 저장마다 객체·필드 순서로
//   1부터 다시 매긴다(PackageWriter가 SubstituteStruct로 바꿔 쓰고 옛 id → 새 id 표를 돌려준다,
//   AS 5.5절).
// - rawBytes·sha256: 압축 전 데이터의 크기와 SHA-256이다. 로드는 BulkIndex 항목과 같은지 대조한다
//   (다르면 CorruptPackage).
//
// 사용 예(에셋 필드와 로드 뒤 읽기):
//   ROCKS_PROPERTY() BulkDataRef pixels;
//   ...
//   auto bytes = package.BulkBytes(asset.pixels);   // Span<const uint8>(rawBytes byte)
//
// 수명: 값 타입이다. 가리키는 byte는 그 패키지 객체(Package::BulkBytes)가 소유한다.
ROCKS_STRUCT()
struct BulkDataRef {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() uint32 bulkId = 0;
    ROCKS_PROPERTY() uint64 rawBytes = 0;
    ROCKS_PROPERTY() Sha256Digest sha256 {};

    bool IsEmpty() const noexcept {
        return bulkId == 0;
    }

    // 내용이 같은지다(rawBytes·sha256 — bulkId는 패키지 저장소의 지역 핸들이라 보지 않는다, AS
    // 7.1절 15). 저장 뒤·다른 패키지·다시 연 값을 비교할 때 쓴다.
    bool SameContent(const BulkDataRef& other) const noexcept {
        return rawBytes == other.rawBytes && sha256 == other.sha256;
    }

    friend bool operator==(const BulkDataRef&, const BulkDataRef&) noexcept = default;
};

} // namespace rocks
