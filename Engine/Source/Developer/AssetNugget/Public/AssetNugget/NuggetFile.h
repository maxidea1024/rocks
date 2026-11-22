#pragma once

#include <AssetNugget/NuggetRecord.h>

// `.rnug` v2 파일이다(Docs/Design/AssetSystem.ko.md 9.1절 결정 6): { magic "RNUG", u32 버전 2,
// u64 Record byte 수, Record SHA-256, Record, 값 저장 byte(ValueId 순), 조각 해시 표(storedSize ≥
// 1 MiB인 값만, ValueId 순) }, little-endian. 파일 크기는 각 부분의 합과 정확히 같아야 한다.
//
// 사용 예(쓰기 — 메모리):
//   const NuggetStoredValue values[] = {{0, NuggetEncoding::Identity, raw.Size(),
//                                        Sha256::Hash(raw), 0, {}, raw}};
//   auto plan = PlanNuggetFile(allocator, NuggetBucketTexture, key, values, {}, {});
//   Array<uint8> file(allocator);
//   auto written = AppendNuggetFile(plan.Value(), values, file);
// 사용 예(읽기):
//   NuggetMemoryReader reader(file.View());
//   auto view = OpenNuggetFile(allocator, reader);           // 머리·Record·표 검증
//   Array<uint8> stored(allocator);
//   auto read = ReadNuggetValue(view.Value(), reader, 0, stored);   // ContentHash 검증
//   auto raw = DecodeNuggetValue(allocator, view.Value().record.values[0], stored.View(), out);
namespace rocks {

inline constexpr uint32 NuggetFileVersion = 2;
// 머리 크기다(magic 4 + 버전 4 + Record byte 수 8 + Record SHA-256 32).
inline constexpr usize NuggetFileHeadBytes = 48;

// 파일의 범위 읽기다(메모리·파일 구현).
class NuggetRangeReader {
public:
    virtual ~NuggetRangeReader() = default;
    // 파일 전체 byte 수다.
    virtual uint64 Size() const noexcept = 0;
    // [offset, offset + out.Size())를 out에 정확히 읽는다. 범위가 밖이면 InvalidArgument, IO 오류는
    // 그 오류다.
    virtual Result<void> ReadAt(uint64 offset, Span<uint8> out) noexcept = 0;
};

// 메모리 byte의 범위 읽기다(bytes는 reader보다 오래 살아야 한다).
class NuggetMemoryReader final : public NuggetRangeReader {
public:
    explicit NuggetMemoryReader(Span<const uint8> bytes) noexcept : bytes_(bytes) {
    }

    uint64 Size() const noexcept override {
        return bytes_.Size();
    }

    Result<void> ReadAt(uint64 offset, Span<uint8> out) noexcept override;

private:
    Span<const uint8> bytes_;
};

// 쓰기 입력 값 하나다(저장 byte와 표현). rawHash는 해제한 원본의 SHA-256, Identity면 stored가
// 원본이다. indexLength·indexHash는 Rcbk만.
struct NuggetStoredValue {
    uint32 id = 0;
    NuggetEncoding encoding = NuggetEncoding::Identity;
    uint64 rawSize = 0;
    Sha256Digest rawHash{};
    uint64 indexLength = 0;
    Sha256Digest indexHash{};
    Span<const uint8> stored{};
};

// 쓰기 계획이다: Record(행의 ContentHash·storedSize·ChunkTableHash를 채운 것), 머리 + Record byte,
// 값마다 조각 해시 표(없으면 빈 배열).
struct NuggetFilePlan {
    explicit NuggetFilePlan(Allocator& allocator) noexcept
        : record(allocator), head(allocator), tables(allocator) {
    }

    NuggetRecord record;
    Array<uint8> head;
    Array<Array<uint8>> tables;
    // 파일 전체 byte 수다.
    uint64 fileBytes = 0;
};

// 쓰기 계획을 만든다(값 저장 byte를 해시하고 조각 해시 표를 만든다 — 값 byte는 복사하지 않는다).
// values는 ValueId 오름차순이어야 한다. 오류: 정규 형식 위반(bucket·순서·한도·Identity의 크기
// 불일치 등) InvalidArgument, Record 1 MiB 초과 CapacityExceeded, 할당 실패.
Result<NuggetFilePlan> PlanNuggetFile(Allocator& allocator, StringView bucket,
                                      const Sha256Digest& key, Span<const NuggetStoredValue> values,
                                      Span<const NuggetInput> inputs,
                                      Span<const uint8> meta) noexcept;

// 계획대로 파일 byte를 out 끝에 붙인다(values는 계획을 만든 값과 같아야 한다 — 크기가 다르면
// InvalidArgument). 할당 실패는 OutOfMemory(out은 정해지지 않는다).
Result<void> AppendNuggetFile(const NuggetFilePlan& plan, Span<const NuggetStoredValue> values,
                              Array<uint8>& out) noexcept;

// 연 파일이다: Record, 논리 digest, 값마다 파일 안 위치와 조각 해시 표(없으면 빈 배열).
struct NuggetFileView {
    explicit NuggetFileView(Allocator& allocator) noexcept
        : record(allocator), offsets(allocator), tables(allocator) {
    }

    NuggetRecord record;
    Sha256Digest digest{};
    Array<uint64> offsets;
    Array<Array<uint8>> tables;
};

// 파일을 연다: 머리(magic·버전), Record byte 수(≤ 1 MiB), Record SHA-256, Record 정규 형식, 파일
// 크기 = 부분 합, 조각 해시 표마다 크기(⌈storedSize / 1 MiB⌉ × 32)와 ChunkTableHash를 검증한다(값
// byte는 읽지 않는다). 오류: 손상(형식·해시·크기) InvalidArgument, 다른 파일 버전·더 새 Record
// schema 버전 Unsupported(손상이 아니다 — 지우지 않는다), 읽기 IO 오류, 할당 실패.
Result<NuggetFileView> OpenNuggetFile(Allocator& allocator, NuggetRangeReader& reader) noexcept;

// index번째 값의 저장 byte를 out 끝에 붙이고 ContentHash를 검증한다. 오류: index 범위 밖
// InvalidArgument, 해시 불일치 InvalidArgument(out은 정해지지 않는다), 읽기 IO 오류, 할당 실패.
Result<void> ReadNuggetValue(const NuggetFileView& view, NuggetRangeReader& reader, usize index,
                             Array<uint8>& out) noexcept;

// 저장 byte를 원본으로 풀어 out 끝에 붙이고 RawHash·rawSize를 검증한다(Identity는 복사, Rcbk는 블록
// 컨테이너 해제와 IndexHash 검증). 행은 먼저 IsValidNuggetValueRow로 본다(검증되지 않은 행도
// 받는다). 오류: 정규 형식이 아닌 행·손상 InvalidArgument, Codec은 Unsupported(계약 — 그 코덱
// 없음), 할당 실패.
Result<void> DecodeNuggetValue(Allocator& allocator, const NuggetValueRow& row,
                               Span<const uint8> stored, Array<uint8>& out) noexcept;

} // namespace rocks
