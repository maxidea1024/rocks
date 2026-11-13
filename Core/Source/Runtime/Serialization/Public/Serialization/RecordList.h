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

// 바깥 ArchiveWriter 위에 RecordList payload를 제자리로 쓴다(리플렉션 struct 원소 배열). Begin이
// 최종 항목 수를 쓰고, 항목마다 BeginItem이 u64 길이 자리만 쓰며 항목 Record는 호출자가
// RecordWriter::Begin으로 out에 바로 쓴 뒤 EndItem이 길이를 채운다. 항목 byte를 builder에 모았다가
// 다시 복사하지 않으므로 중첩 깊이만큼 복사가 늘지 않는다. 같은 항목을 같은 순서로 쓰면
// RecordListBuilder::Finish가 부모에 쓰는 payload와 같은 byte다. 작업량은 out의 청구다. 실패하면
// out의 내용은 정해지지 않으므로 그 writer를 버린다. out은 이 writer보다 오래 살아야 한다.
//
// 사용 예:
//   auto list = RecordListWriter::Begin(out, count, limits);
//   if (!list) {
//       return Result<void>::Failure(list.Error());
//   }
//
//   for (uint32 index = 0; index < count; ++index) {
//       if (auto begun = list.Value().BeginItem(); !begun) {
//           return begun;
//       }
//
//       auto record = RecordWriter::Begin(out, ItemSchema, 1, itemLimits);
//       if (!record) {
//           return Result<void>::Failure(record.Error());
//       }
//
//       // 항목 필드를 record.Value().BeginField·EndField 사이에 out에 바로 쓴다.
//       if (auto ended = record.Value().End(); !ended) {
//           return ended;
//       }
//
//       if (auto ended = list.Value().EndItem(); !ended) {
//           return ended;
//       }
//   }
//
//   return list.Value().End();
class RecordListWriter {
public:
    // count가 limits.maxItems를 넘거나 count 4 byte + count × 항목 최소 크기(52)가
    // limits.maxPayloadBytes를 넘으면 CapacityExceeded다(쓰기 전에 거부한다). out의 쓰기 오류를
    // 그대로 돌려준다.
    static Result<RecordListWriter> Begin(ArchiveWriter& out, uint32 count,
                                          RecordListLimits limits = {}) noexcept;

    // 항목 하나의 u64 길이 자리를 쓴다. 열린 항목이 있거나 이미 count개를 썼으면 InvalidArgument다.
    Result<void> BeginItem() noexcept;

    // 지금 항목의 길이를 채운다. 열린 항목이 없거나 out이 길이 자리보다 짧거나 항목이 Record
    // envelope(magic·길이 일관성)가 아니면 InvalidArgument, 항목이 limits.maxItemBytes를 넘거나
    // 지금까지의 payload가 limits.maxPayloadBytes를 넘으면 CapacityExceeded다.
    Result<void> EndItem() noexcept;

    // 열린 항목이 있거나 쓴 항목 수가 count와 다르면 InvalidArgument, payload 전체가
    // limits.maxPayloadBytes를 넘으면 CapacityExceeded다.
    Result<void> End() const noexcept;

    // 지금까지 끝낸 항목 수다.
    usize ItemCount() const noexcept;

private:
    static constexpr usize NoItem = ~usize(0);

    RecordListWriter(ArchiveWriter& out, usize start, uint32 count,
                     RecordListLimits limits) noexcept
        : out_(&out), start_(start), count_(count), limits_(limits) {
    }

    ArchiveWriter* out_;
    usize start_;
    usize item_ = NoItem;
    uint32 count_;
    uint32 written_ = 0;
    RecordListLimits limits_;
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
//
// 청구: 항목 byte는 한 번만 청구한다. 단독 Open은 목록을 열 때 항목 byte를 청구하고, OpenField는
// 부모 Record가 이미 청구했으므로 항목마다 operation 하나(색인)만 청구한다. OpenItem은 언제나 항목
// Record의 header·TLV 색인만 청구한다(TLV payload를 다시 청구하지 않는다). 그래서 목록 안 struct 안
// 목록처럼 중첩해도 작업이 깊이 배수로 불어나지 않는다.
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
    // InvalidArgument, schema 불일치는 TypeMismatch, 버전 범위 밖은 Unsupported(버전 0은
    // InvalidArgument), 필드 수가 항목 한도(itemLimits·부모의 maxFields)를 넘으면
    // CapacityExceeded다. OpenItem(index, 항목 한도의 maxFields)와 같다.
    Result<RecordReader> OpenItem(usize index) noexcept;
    // 필드 수 한도를 maxFields로 바꿔 연다(그래프가 남은 필드 누계로 열 때). 그 밖은
    // OpenItem(index)와 같다.
    //
    // 사용 예:
    //   auto item = list.OpenItem(index, remainingFields);
    Result<RecordReader> OpenItem(usize index, uint32 maxFields) noexcept;
    // index번째 항목 Record의 전체 byte다(예: 마이그레이션 입력). 범위 밖 index는
    // InvalidArgument다. 반환 view는 payload 원본을 빌린다. 예산을 청구하지 않는다.
    Result<Span<const uint8>> ItemBytes(usize index) const noexcept;
    // 공유 예산의 지금까지 사용량이다(부모·형제·항목 reader 포함).
    uint64 OperationsUsed() const noexcept;
    uint64 WorkUsed() const noexcept;

private:
    explicit RecordListReader(Allocator&) noexcept;
    // indexOnly면 항목 byte를 청구하지 않는다(부모 Record가 이미 청구한 중첩 목록).
    static Result<RecordListReader>
    OpenShared(Allocator&, Span<const uint8> payload, Guid itemSchema, uint32 minItemVersion,
               uint32 maxItemVersion, RecordLimits itemLimits, RecordListLimits listLimits,
               SharedPtr<ArchiveBudget> budget, bool indexOnly) noexcept;

    Allocator* allocator_;
    Guid schema_;
    uint32 minimum_ = 0;
    uint32 maximum_ = 0;
    RecordLimits limits_;
    SharedPtr<ArchiveBudget> budget_;
    Array<Span<const uint8>> items_;
};

} // namespace rocks
