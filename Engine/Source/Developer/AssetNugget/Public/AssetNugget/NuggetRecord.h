#pragma once

#include <Core/Containers/Array.h>
#include <Core/Encoding/Sha256.h>
#include <Core/Guid.h>
#include <Core/Result.h>
#include <Core/Text/String.h>

// 에셋 너겟 Record다(Docs/Design/AssetSystem 9.1절 결정 5 — Rockpile.ko.md 5.2·5.3절
// `RockpileRecord`). 키 하나의 결과 묶음: echo(namespace·bucket·key), 값 표(160 byte 행, ValueId
// 오름차순), 소비 입력(bucket·key·RecordDigest 정렬 목록), meta(결정적 정보). 불변이며 정규 형식만
// 받는다(순서·중복·한도 위반은 NonCanonical — InvalidArgument).
//
// 사용 예:
//   NuggetRecord record(allocator);
//   record.bucket = ...; record.key = key; record.values = rows;
//   auto bytes = WriteNuggetRecord(allocator, record);   // Record TLV
//   auto back = ReadNuggetRecord(allocator, bytes.Value().View());
//   const Sha256Digest digest = NuggetRecordDigest(back.Value());
namespace rocks {

// Record schema(Record TLV)와 필드다(9.1절 결정 5 — Rockpile 명세 파일이 생기면 같은 값을 옮긴다).
inline constexpr Guid NuggetRecordSchema{0x524f434b50494c45ull, 0x0000000000000001ull};
inline constexpr Guid NuggetInputSchema{0x524f434b50494c45ull, 0x0000000000000002ull};
inline constexpr uint32 NuggetRecordSchemaVersion = 1;
inline constexpr Guid NuggetRecordNamespaceField{0, 1};
inline constexpr Guid NuggetRecordBucketField{0, 2};
inline constexpr Guid NuggetRecordKeyField{0, 3};
inline constexpr Guid NuggetRecordValuesField{0, 4};
inline constexpr Guid NuggetRecordInputsField{0, 5};
inline constexpr Guid NuggetRecordMetaField{0, 6};
inline constexpr Guid NuggetInputBucketField{0, 1};
inline constexpr Guid NuggetInputKeyField{0, 2};
inline constexpr Guid NuggetInputDigestField{0, 3};

// 한도다(Rockpile 5.2절·8.3절).
inline constexpr usize NuggetRecordMaxBytes = 1024 * 1024;
inline constexpr usize NuggetMetaMaxBytes = 64 * 1024;
inline constexpr uint64 NuggetValueMaxBytes = 16ull * 1024 * 1024 * 1024;
inline constexpr usize NuggetValueRowBytes = 160;
// 조각 해시 표의 조각 크기다(storedSize ≥ 이 값이면 표를 만든다).
inline constexpr uint64 NuggetChunkBytes = 1024 * 1024;
// namespace·bucket 이름의 최대 byte 수다.
inline constexpr usize NuggetNameMaxBytes = 64;

// 에셋 너겟 namespace다.
inline constexpr StringView NuggetNamespace = "rocks.nuggets";

// 처리기 종류(bucket)다 — `[a-z][a-z0-9_]{0,63}`, 고정 소문자 ASCII 상수.
inline constexpr StringView NuggetBucketTexture = "texture";
// 정적 메시 쿠킹 LOD(Docs/Design/MeshAnimationAssets 5.5.2절 10)다.
inline constexpr StringView NuggetBucketMesh = "mesh";
inline constexpr StringView NuggetBucketShader = "shader";
inline constexpr StringView NuggetBucketScc = "scc";

// bucket 이름 규칙(`[a-z][a-z0-9_]{0,63}`)인지다.
ROCKS_ASSETNUGGET_API bool IsValidNuggetBucket(StringView bucket) noexcept;

// namespace 이름 규칙(`[a-z0-9][a-z0-9._-]{0,63}`)인지다.
ROCKS_ASSETNUGGET_API bool IsValidNuggetNamespace(StringView name) noexcept;

// 값의 저장 표현이다. Codec(사용자 등록 형식)은 계약이다(A3에서는 읽으면 늘 미적중).
enum class NuggetEncoding : uint8 {
    Identity = 0,
    Rcbk = 1,
    Codec = 2
};

// 값 표의 행 하나다(160 byte — Rockpile 5.2절). indexLength·indexHash는 Rcbk만(그 밖은 0),
// chunkTableHash는 storedSize ≥ NuggetChunkBytes인 값만(그 밖은 0).
struct NuggetValueRow {
    uint32 id = 0;
    NuggetEncoding encoding = NuggetEncoding::Identity;
    uint64 rawSize = 0;
    uint64 storedSize = 0;
    uint64 indexLength = 0;
    Sha256Digest rawHash{};
    Sha256Digest contentHash{};
    Sha256Digest indexHash{};
    Sha256Digest chunkTableHash{};
};

// 소비 입력 하나다(이 Record를 만들 때 쓴 다른 Record).
struct NuggetInput {
    explicit NuggetInput(Allocator& allocator) noexcept : bucket(allocator) {
    }

    String bucket;
    Sha256Digest key{};
    Sha256Digest digest{};
};

struct NuggetRecord {
    explicit NuggetRecord(Allocator& allocator) noexcept
        : nameSpace(allocator), bucket(allocator), values(allocator), inputs(allocator),
          meta(allocator) {
    }

    String nameSpace;
    String bucket;
    Sha256Digest key{};
    Array<NuggetValueRow> values;
    Array<NuggetInput> inputs;
    Array<uint8> meta;
};

// 값 행 하나의 정규 형식이다(아래 ValidateNuggetRecord의 행 규칙 — encoding 알려진 값, 크기 ≤
// NuggetValueMaxBytes, Identity의 storedSize = rawSize·ContentHash = RawHash, 색인은 Rcbk만(48 +
// 8n, IndexHash 있음, ≤ storedSize), 표 해시는 storedSize ≥ NuggetChunkBytes일 때만).
bool IsValidNuggetValueRow(const NuggetValueRow& row) noexcept;

// 정규 형식 검사다: namespace·bucket 규칙, 값 ValueId 오름차순·중복 없음, encoding 알려진 값,
// Identity는 storedSize = rawSize·ContentHash = RawHash, Rcbk·Codec이 아니면 indexLength·IndexHash
// 0, 크기 ≤ NuggetValueMaxBytes, ChunkTableHash는 storedSize ≥ NuggetChunkBytes일 때만 0이 아니다,
// inputs bucket 규칙·정규 인코딩 byte 사전순·중복 없음, meta ≤ NuggetMetaMaxBytes. 위반은
// InvalidArgument(NonCanonical)다.
ROCKS_ASSETNUGGET_API Result<void> ValidateNuggetRecord(const NuggetRecord& record) noexcept;

// Record TLV byte다(필드 1–6 순서). 오류: 정규 형식 위반 InvalidArgument, 1 MiB 초과
// CapacityExceeded, 할당 실패.
ROCKS_ASSETNUGGET_API Result<Array<uint8>> WriteNuggetRecord(Allocator& allocator,
                                                             const NuggetRecord& record) noexcept;

// Record TLV를 읽는다. schema·버전(다른 schema CorruptData, 더 새 버전 Unsupported), 필드
// 1–6이 정확히 그 순서·wire로 있는지, 값 표 길이(160의 배수), 정규 형식을 본다. 오류: 형식·정규
// 위반 CorruptData(읽은 데이터 손상 — AssetSystem 19장), 1 MiB 초과 CapacityExceeded, 더 새 버전
// Unsupported, 할당 실패.
ROCKS_ASSETNUGGET_API Result<NuggetRecord> ReadNuggetRecord(Allocator& allocator,
                                                            Span<const uint8> bytes) noexcept;

// 논리 digest다(Rockpile 5.3절): SHA-256(echo, ValueId 순 {ValueId, rawSize, RawHash}, inputs,
// SHA-256(meta)) — 저장 표현(encoding·storedSize·ContentHash·IndexHash·ChunkTableHash)은 넣지
// 않는다. 정수는 little-endian, 문자열은 u64 길이 + byte다.
ROCKS_ASSETNUGGET_API Sha256Digest NuggetRecordDigest(const NuggetRecord& record) noexcept;

// 소비 입력의 정규 인코딩 순서 비교다(u64 길이 + bucket, key, digest의 byte 사전순).
ROCKS_ASSETNUGGET_API int32 CompareNuggetInputs(const NuggetInput& left,
                                                const NuggetInput& right) noexcept;

} // namespace rocks
