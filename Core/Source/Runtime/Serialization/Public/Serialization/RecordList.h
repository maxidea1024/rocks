#pragma once

#include <Serialization/Record.h>

// 항목 Record의 목록 필드(WireType::RecordList)다(설계: Docs/Design/CoreDataFoundations.ko.md 4절).
//
// 형식: payload = u32 count + count × (u64 length + 중첩 Record). 각 length는 중첩 Record의 전체
// 길이(헤더 44 byte + payloadBytes)와 같아야 한다. 빈 목록도 count 0으로 기록한다(Record에는
// "없으면 기본값" 규칙이 없다). 범용 도구·리플렉션은 wire tag로 목록을 구분한다.
//
// 사용 예:
//   RecordListBuilder list(allocator);
//   for (const Item& item : items) {
//       auto bytes = BuildItemRecord(item);   // RecordBuilder::Build 결과(Array<uint8>)
//       if (!bytes || !list.AddItem(bytes.Value().View())) { ... }
//   }
//   if (!list.Finish(parent, ItemsFieldId)) { ... }
//
//   auto field = reader.Find(ItemsFieldId);
//   auto items = RecordListReader::OpenField(reader, field.Value(), ItemSchema, 1, 1);
//   for (usize index = 0; index < items.Value().ItemCount(); ++index) {
//       auto item = items.Value().OpenItem(index);   // 부모 Record와 같은 예산을 쓴다
//       ...
//   }
//
// 오류는 Archive 규약을 따른다: 잘림·length 불일치·남은 바이트·잘못된 항목 envelope는
// InvalidArgument, 한도 초과는 CapacityExceeded, 할당 실패는 OutOfMemory. 실패하면 builder·부모
// Record는 호출 전 상태 그대로다.
namespace rocks {

// 목록 한도. maxPayloadBytes는 목록 payload 전체(u32 count + 모든 length·항목), maxItemBytes는 항목
// Record 하나의 전체 크기다. 기본값은 기본 ArchiveLimits::maxValueBytes(4 MiB)에 맞춘다: payload가
// 부모 Record의 값 하나이므로 기본 한도로 만든 목록은 기본 한도의 부모에 그대로 들어가고, 항목
// 하나짜리 목록도 count 4 + length 8 byte를 더해 4 MiB를 넘지 않는다. 부모 Record의 한도
// (maxValueBytes·maxTotalBytes)는 이와 별개로 적용되므로 패키지처럼 큰 목록을 쓰는 곳은 두 한도를
// 함께 올린다.
struct RecordListLimits {
    uint32 maxItems = 65536;
    usize maxPayloadBytes = 4 * 1024 * 1024;
    usize maxItemBytes = 4 * 1024 * 1024 - 12;
};

// 완성된 항목 Record 바이트를 모아 부모 Record의 RecordList 필드로 쓴다. RecordBuilder는 중첩
// 스트리밍 기록이 없으므로 항목은 완성된 바이트로 받는다. builder는 완성된 payload를 기하 성장
// buffer에 바로 쌓고(항목마다 전체를 다시 복사하지 않는다) Finish는 그것을 부모에 한 번 복사한다.
// 목록 데이터의 사본은 이 builder buffer(성장 여유 용량 포함, 성장 중에는 이전·새 buffer가 잠시
// 함께 있다), 부모 builder, 부모 Build 결과로 여러 벌이 된다. 큰 목록은 항목을 하나씩 만들어 바로
// AddItem하고, Finish 뒤에는 builder를 일찍 파괴한다. 복사·이동하지 않는다.
class RecordListBuilder {
public:
    explicit RecordListBuilder(Allocator&, RecordListLimits = {}) noexcept;
    RecordListBuilder(const RecordListBuilder&) = delete;
    RecordListBuilder& operator=(const RecordListBuilder&) = delete;

    // 항목 Record(RecordBuilder::Build 결과)를 깊게 복사해 붙인다. envelope magic과 길이 일관성(44
    // + payloadBytes == 크기)을 검사하며 항목 schema는 reader가 본다. 잘못된 envelope는
    // InvalidArgument, maxItems·maxItemBytes·maxPayloadBytes 초과는 CapacityExceeded다. 실패하면
    // 변경 없음.
    Result<void> AddItem(Span<const uint8> builtRecord) noexcept;
    // 모은 항목을 부모의 fieldId 필드(WireType::RecordList)로 추가한다. 부모 AddField의 오류(중복
    // id는 AlreadyExists, 값 크기 초과는 CapacityExceeded 등)를 그대로 돌려준다. 실패하면 부모와 이
    // builder는 그대로다. 성공해도 이 builder의 항목은 남는다(같은 목록을 다른 부모에 다시 쓸 수
    // 있다).
    Result<void> Finish(RecordBuilder& parent, Guid fieldId) noexcept;
    usize ItemCount() const noexcept;

private:
    RecordListLimits limits_;
    Array<uint8> items_; // 완성된 payload: u32 count + count × (u64 length + 항목). 비면 0 byte
    uint32 count_ = 0;
};

// RecordList payload를 검증하고 항목별 RecordReader를 연다. 열 때 count·모든 length·항목 envelope
// 경계를 검증한 뒤 index를 게시하고(count는 할당 전에 남은 byte / 52(항목 최소 크기)와 비교한다),
// 항목은 OpenItem으로 schema·버전 범위를 검사하며 연다.
//
// 예산: OpenField로 열면 부모 Record 트리의 누적 예산을, Open으로 열면 새 예산을 목록과 그것이 연
// 모든 항목 reader(와 그 안의 값·중첩 목록)가 공유한다. 값·항목마다 새 예산을 주지
// 않으므로("환급하지 않는다" 원칙) 중첩 트리를 순회해도 한도를 넘지 못한다. 예산은 참조 계수로
// 공유되므로 reader의 파괴 순서는 자유롭다. 한 항목에서 work 한도를 넘으면 공유 사용량이 한도로
// 포화되어 형제 항목과 목록의 이후 호출도 실패한다(나쁜 항목만 건너뛰고 계속할 수 없다). 같은
// 예산을 공유하는 서로 다른 reader는 여러 스레드에서 써도 된다. payload 원본은 모든
// reader·FieldView보다 오래 살아야 한다. 이동만 된다.
class RecordListReader {
public:
    // 새 예산으로 연다(최상위 목록). itemLimits는 항목 Record와 예산(maxOperations·maxWork)의
    // 한도이고, 목록 payload 전체에는 listLimits.maxPayloadBytes가 적용된다.
    static Result<RecordListReader> Open(Allocator&, Span<const uint8> payload, Guid itemSchema,
                                         uint32 minItemVersion, uint32 maxItemVersion,
                                         RecordLimits itemLimits = {},
                                         RecordListLimits listLimits = {}) noexcept;
    // parent가 돌려준 RecordList 필드를 parent와 같은 예산·한도로 연다(중첩 목록). wire tag가
    // RecordList가 아니면 TypeMismatch, parent 밖의 범위면 InvalidArgument다.
    static Result<RecordListReader> OpenField(const RecordReader& parent, const FieldView& field,
                                              Guid itemSchema, uint32 minItemVersion,
                                              uint32 maxItemVersion,
                                              RecordListLimits listLimits = {}) noexcept;
    RecordListReader(RecordListReader&&) noexcept = default;
    RecordListReader& operator=(RecordListReader&&) noexcept = default;
    RecordListReader(const RecordListReader&) = delete;
    RecordListReader& operator=(const RecordListReader&) = delete;

    usize ItemCount() const noexcept;
    // index번째 항목의 RecordReader(공유 예산). operation 하나를 청구하며 범위 밖 index는
    // InvalidArgument, schema 불일치는 TypeMismatch, 버전 범위 밖은 Unsupported다.
    Result<RecordReader> OpenItem(usize index) noexcept;
    // 공유 예산의 지금까지 사용량이다(부모·형제·항목 reader 포함).
    uint64 OperationsUsed() const noexcept;
    uint64 WorkUsed() const noexcept;

private:
    explicit RecordListReader(Allocator&) noexcept;
    static Result<RecordListReader> OpenShared(Allocator&, Span<const uint8> payload,
                                               Guid itemSchema, uint32 minItemVersion,
                                               uint32 maxItemVersion, RecordLimits itemLimits,
                                               RecordListLimits listLimits,
                                               SharedPtr<ArchiveBudget> budget) noexcept;

    Allocator* allocator_;
    Guid schema_;
    uint32 minimum_ = 0;
    uint32 maximum_ = 0;
    RecordLimits limits_;
    SharedPtr<ArchiveBudget> budget_;
    Array<Span<const uint8>> items_;
};

} // namespace rocks
