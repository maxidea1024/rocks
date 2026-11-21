#pragma once

#include <AssetRuntime/CookedManifest.h>
#include <AssetRuntime/PackageReader.h>
#include <Base/Endian.h>
#include <Core/Text/StringView.h>

// 패키지 writer·reader·로드가 같이 쓰는 내부 정의다: Summary Record 스키마·필드 ID, 정렬·비교
// 도우미, Dependencies 합성 규칙(writer와 로드 대조가 같은 함수를 써서 규칙이 갈라지지 않게 한다),
// BulkDataRef 빈 값 규칙.
namespace rocks::package_detail {

// Summary Record 스키마와 필드 ID다(AS 5.2절 Summary). 값은 포맷의 일부라 바꾸지 않는다.
inline constexpr Guid SummarySchema{0x4f380b51d9734145ull, 0xac5f901ff31085f9ull};
inline constexpr uint32 SummarySchemaVersion = 1;
inline constexpr Guid SummaryTypesField{0x230489eefd344e94ull, 0x8cbebb769572738bull};
inline constexpr Guid SummaryCustomVersionsField{0x278b489ff779464aull, 0xb31b763b7233fa90ull};
inline constexpr Guid SummarySavedByField{0x328f4624f4634f36ull, 0xbc4e46ba2abd5a0bull};
inline constexpr Guid SummaryCompatibleWithField{0x7d9444370e0e4d6eull, 0xa0bdc9f17add0c43ull};
inline constexpr Guid SummaryProfileIdField{0x91942e1fe43d4ad5ull, 0x87a78df076ee6d66ull};
inline constexpr Guid SummaryProfileSettingsField{0x8e3695ae88ea4f33ull, 0x8bcf12b5702003c6ull};
inline constexpr Guid SummaryCookerVersionField{0x9eff96ac38e74376ull, 0x8e4e00e5f9d18071ull};

// 버전 표 항목 Record(타입·커스텀 버전 공통 — {id, version})다.
inline constexpr Guid VersionItemSchema{0xfed3de0160bd48f0ull, 0xac87ebc80f031ffcull};
inline constexpr Guid VersionItemIdField{0x838cbb8df35545c4ull, 0xb6d77752d621436bull};
inline constexpr Guid VersionItemVersionField{0xb6904e5e869d4b5cull, 0x9faa1637331809baull};

// 엔진 버전 Record({주, 부, 패치})다.
inline constexpr Guid EngineVersionSchema{0xb1c3f10ce6384ba7ull, 0x931a475d68f2061dull};
inline constexpr Guid EngineVersionMajorField{0x8a3ce74fb0014d8aull, 0x9c1e133e53102d0aull};
inline constexpr Guid EngineVersionMinorField{0x9f114cc5f1fe4880ull, 0xb1d13d2ae9226863ull};
inline constexpr Guid EngineVersionPatchField{0x79aa83f6601e438cull, 0x9271d6d664029065ull};

// Dependencies 항목 flags 비트다(required).
inline constexpr uint32 DependencyRequiredFlag = 1;

constexpr uint64 AlignUp(uint64 value, uint64 alignment) noexcept {
    return (value + alignment - 1) & ~(alignment - 1);
}

constexpr bool IsPowerOfTwo(uint32 value) noexcept {
    return value != 0 && (value & (value - 1)) == 0;
}

// 이름·경로의 결정적 정렬 순서다(NFC UTF-8 byte 순 — AS 4.2절).
inline bool BytesLess(StringView left, StringView right) noexcept {
    const usize count = left.ByteCount() < right.ByteCount() ? left.ByteCount() : right.ByteCount();
    for (usize index = 0; index < count; ++index) {
        const uint8 a = static_cast<uint8>(left.Data()[index]);
        const uint8 b = static_cast<uint8>(right.Data()[index]);
        if (a != b) {
            return a < b;
        }
    }

    return left.ByteCount() < right.ByteCount();
}

// 의존 쓰임 하나다. hard면 외부 참조(import 대상 객체 하나 — 그 모든 출현을 합친 것), 아니면 Soft
// 참조 출현 하나다. editorOnly는 그 쓰임이 EditorOnly 위치에서만 있었는지, required는 Required
// 위치가 있었는지다.
struct DependencyUse {
    AssetId asset{};
    bool hard = false;
    bool editorOnly = false;
    bool required = false;
};

// 쓰임 목록을 Dependencies(Hard·Soft·EditorOnly) 항목으로 합친다. 규칙(writer와 로드 대조 공통):
// 에셋마다 hard 계열은 EditorOnly가 아닌 쓰임이 있으면 Hard, 없고 EditorOnly 쓰임만 있으면
// EditorOnly다. soft 계열도 같다(Soft·EditorOnly). 두 계열이 모두 EditorOnly면 EditorOnly 항목
// 하나다. required는 그 항목의 종류를 정한 쓰임들의 OR이다. self(자기 에셋)는 넣지 않는다. 결과는
// (assetId, kind) 순이며 out에 붙인다(expectedType은 0 — writer가 Hard에 나중에 채운다).
Result<void> MergeDependencyUses(Allocator& scratch, Span<const DependencyUse> uses, AssetId self,
                                 Array<PackageDependencyEntry>& out) noexcept;

// 빈 BulkDataRef(bulkId 0)는 rawBytes 0·SHA-256 0이어야 한다(AS 5.7절 14 — writer는 저장을,
// 로드는 Objects 손상으로 거부한다).
// 호출자는 빈 ref(bulkId 0)에만 부른다.
inline bool IsMalformedEmptyBulkRef(uint64 rawBytes, const Sha256Digest& sha256) noexcept {
    if (rawBytes != 0) {
        return true;
    }

    for (uint8 value : sha256.bytes) {
        if (value != 0) {
            return true;
        }
    }

    return false;
}

// Tags Record를 검증한다(AS 5.2절 — 키 정렬): envelope·TLV 구조(RecordReader), 필드 ID(태그 키)가
// 기록 순서로 엄격 오름차순(Guid (high, low) 순 — 중복 없음). 값 스키마는 A2가 정한다. 위반은
// InvalidArgument, 할당 실패는 OutOfMemory다. writer 옵션 검증과 reader가 같이 쓴다(정의는
// PackageReader.cpp).
Result<void> ValidateTagsRecord(Allocator& scratch, Span<const uint8> bytes) noexcept;

// BulkIndex 배치를 Bulk 섹션 크기와 대조한다(id 연속·정렬 offset·끝 = 섹션 크기·codec None의
// 크기·flags — 내용은 보지 않는다). 위반은 InvalidArgument다.
Result<void> ValidateBulkLayout(const PackageContents& contents, uint64 bulkBytes) noexcept;

// 매니페스트 경로 힌트(AS 4.2절)를 검사하고, 받으면 caseless 키('/' + 구성 요소 키를 이어 붙인
// 것)를 key 뒤에 붙인다. 규칙 위반은 InvalidArgument, scratch·key 할당 실패는 그 오류다. 매니페스트
// 검증과 로더의 FindByPath 색인이 같은 키를 쓴다(정의는 CookedManifest.cpp).
Result<void> CheckCookedPathHint(StringView path, Allocator& scratch,
                                 StringBuilderBase& key) noexcept;

// UTF-8 byte 순서(부호 없는 사전 순 — AS 4.2절 정렬)다(정의는 CookedManifest.cpp).
bool Utf8ByteLess(StringView left, StringView right) noexcept;

// WriteCookedManifest의 본문이다(AS 10.1절 7). maxFileBytes는 출력 파일(CRC 칸 포함) byte 상한이고
// 넘으면 CapacityExceeded(진단 None, out 그대로)다. 공개 함수는 CookedManifestMaxFileBytes를 넘긴다
// — 시험이 1 GiB 모델 없이 상한 경계를 보도록 상한을 인자로 받는다.
Result<void> WriteCookedManifestWithLimit(Allocator& allocator, const CookedManifest& manifest,
                                          uint64 maxFileBytes, Array<uint8>& out,
                                          AssetDiagnostic* failure) noexcept;

// (assetId, kind) 정렬 비교다.
inline bool DependencyLess(const PackageDependencyEntry& left,
                           const PackageDependencyEntry& right) noexcept {
    if (left.asset.value != right.asset.value) {
        return GuidLess(left.asset.value, right.asset.value);
    }

    return static_cast<uint32>(left.kind) < static_cast<uint32>(right.kind);
}

} // namespace rocks::package_detail
