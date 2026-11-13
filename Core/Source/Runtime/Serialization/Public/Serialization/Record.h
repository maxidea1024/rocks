#pragma once

#include <Serialization/Archive.h>

namespace rocks {

// 알려진 payload 형식의 표준 tag다. 다른 nonzero tag도 보관/열거/skip할 수 있다.
// 레코드 계층은 unknown 값을 임의 타입으로 해석하지 않는다. 사용자가 명시적으로 지원 여부를
// 판단한다.
enum class WireType : uint32 {
    Bool = 1,
    U32 = 2,
    U64 = 3,
    I64 = 4,
    F32 = 5,
    F64 = 6,
    Guid = 7,
    String = 8,
    Bytes = 9,
    Record = 10,
    // 항목 Record 목록: u32 count + count × (u64 length + 중첩 Record). Serialization/RecordList.h.
    RecordList = 11,
    // 소프트 객체 참조: Guid 16 byte(asset) + String(객체 경로). ObjectRuntime SoftObjectRef.
    SoftObjectReference = 12,
    // 단일 값 원소 배열: u32 원소 wire tag + u32 count + 원소 payload들.
    // Serialization/ValueArray.h.
    ValueArray = 13
};

struct RecordLimits {
    ArchiveLimits archive;
    uint32 maxFields = 4096;
};

// payload는 원본 record를 빌린다. index/reader를 소유한다고 원본 byte 수명이 연장되지 않는다.
// known wireType의 값 검증은 ArchiveReader로 읽고 AtEnd를 확인하는 소비자 책임이다.
struct FieldView {
    Guid id;
    uint32 wireType = 0;
    Span<const uint8> payload;
};

// magic 'ROCKSREC'(8), envelopeVersion u32=1, schema Guid(16), schemaVersion u32,
// fieldCount u32, payloadBytes u64의 44-byte header다. TLV는
// fieldGuid16/type4/byteLength8/payload다. 동시 변경/allocator 재진입으로 현재 builder나 입력을
// 변경하지 않는다. Add 순서를 보존하므로 같은 입력 순서는 deterministic하다. 정렬된 canonical
// 순서는 강제하지 않는다.
class RecordBuilder {
public:
    RecordBuilder(Allocator&, Guid schema, uint32 schemaVersion, RecordLimits = {}) noexcept;
    RecordBuilder(const RecordBuilder&) = delete;
    RecordBuilder& operator=(const RecordBuilder&) = delete;

    // id=0/type=0은 InvalidArgument, 중복 id는 AlreadyExists다. 입력은 호출 동안만 빌려 깊게
    // 복사한다. payload는 ArchiveWriter의 값 encoding 또는 명시적으로 정의한 unknown 형식이다.
    // 실패하면 기존 fields/bytes는 유지하고 이미 소비한 예산만 남는다.
    Result<void> AddField(Guid, uint32 wireType, Span<const uint8> encodedPayload) noexcept;

    Result<void> AddField(Guid id, WireType type, Span<const uint8> payload) noexcept {
        return AddField(id, static_cast<uint32>(type), payload);
    }

    // 별도 소유 buffer를 만든다. 실패해도 builder를 보존한다. 결과 Array도 allocator보다 먼저
    // 파괴한다.
    Result<Array<uint8>> Build() noexcept;
    usize FieldCount() const noexcept;
    uint64 WorkUsed() const noexcept;

private:
    Allocator* allocator_;
    Guid schema_;
    uint32 schemaVersion_;
    RecordLimits limits_;
    ArchiveWriter payload_;
    Array<Guid> ids_;
};

// 바깥 ArchiveWriter 뒤에 Record 하나를 제자리로 쓴다(중첩 Record — 리플렉션 struct 값). header와
// 필드 TLV header 자리를 먼저 쓰고 값은 out에 바로 쓴 뒤 wire·길이·개수를 채우므로, 중첩 깊이만큼
// 복사가 늘지 않는다(값 byte는 한 번만 쓴다). 같은 필드를 같은 순서로 쓰면 RecordBuilder::Build와
// 같은 byte다. 필드 ID 중복은 검사하지 않는다(호출자가 스키마 필드처럼 유일한 ID를 보증한다).
// 작업량은 out의 청구(쓰기·성장·채우기)로 센다. 실패하면 out의 내용은 정해지지 않으므로 그 writer를
// 버린다.
//
// 사용 예:
//   auto record = RecordWriter::Begin(out, structId, version, limits);
//   record.Value().BeginField(memberId);
//   out.WriteU32(value);                        // 값은 out에 바로 쓴다
//   record.Value().EndField(WireType::U32);
//   record.Value().End();
class RecordWriter {
public:
    // header 자리를 쓴다. schema 0·version 0은 InvalidArgument, limits.archive.maxTotalBytes가
    // header보다 작으면 CapacityExceeded다.
    static Result<RecordWriter> Begin(ArchiveWriter& out, Guid schema, uint32 schemaVersion,
                                      RecordLimits limits = {}) noexcept;

    // 필드 하나의 TLV header 자리를 쓴다. 앞 필드가 EndField로 끝나지 않았거나 id 0이면
    // InvalidArgument, 필드 수가 limits.maxFields에 이르렀으면 CapacityExceeded다.
    Result<void> BeginField(Guid id) noexcept;

    // 지금 필드의 wire tag와 값 길이를 채운다. 열린 필드가 없거나 wire 0이면 InvalidArgument, 값
    // 길이가 maxValueBytes를 넘으면 CapacityExceeded다.
    Result<void> EndField(uint32 wireType) noexcept;

    Result<void> EndField(WireType type) noexcept {
        return EndField(static_cast<uint32>(type));
    }

    // 필드 수·payload 크기를 채운다. 열린 필드가 있으면 InvalidArgument, 전체가 maxTotalBytes를
    // 넘으면 CapacityExceeded다.
    Result<void> End() noexcept;
    usize FieldCount() const noexcept;

private:
    static constexpr usize NoField = ~usize(0);

    RecordWriter(ArchiveWriter& out, usize start, RecordLimits limits) noexcept
        : out_(&out), start_(start), limits_(limits) {
    }

    ArchiveWriter* out_;
    usize start_;
    usize field_ = NoField;
    uint32 count_ = 0;
    RecordLimits limits_;
};

// 전체 envelope와 TLV 경계를 검증한 뒤 index를 게시한다. index만 소유하고 payload 원본은 빌린다.
// allocator는 reader보다, 원본 buffer는 reader와 모든 FieldView보다 오래 살아야 한다.
// 같은 reader의 병행 Next/Find/이동은 caller가 동기화한다. allocator 재진입으로 입력을 변경하지
// 않는다. envelopeVersion과 허용 범위 밖 schemaVersion은 Unsupported, schema Guid 불일치는
// TypeMismatch다. 실패하는 Open은 부분 reader를 반환하지 않는다. 객체 그래프·fixup·압축·암호화는 이
// 계층에 없다.
//
// 예산: Open은 이 Record 트리의 누적 예산(ArchiveBudget, RecordLimits.archive의
// maxOperations·maxWork)을 만든다. OpenValue로 연 값 reader, RecordListReader::OpenField로 연 중첩
// 목록과 그 항목 reader가 모두 같은 예산을 공유하므로(값·항목마다 새 예산을 주지 않는다) 트리를
// 순회해도 한도를 넘을 수 없다. 값을 따로 ArchiveReader::Create로 읽으면 새 예산이 생기므로
// 신뢰하지 않는 입력은 OpenValue로 읽는다.
class RecordReader {
public:
    static Result<RecordReader> Open(Allocator&, Span<const uint8>, Guid expectedSchema,
                                     uint32 minSchemaVersion, uint32 maxSchemaVersion,
                                     RecordLimits = {}) noexcept;
    // 바깥(예: 객체 그래프 하나)이 가진 누적 예산 budget을 이 Record 트리가 함께 쓴다.
    // operation·work 한도는 budget의 것이고 크기 한도(maxTotalBytes·maxValueBytes)와 필드 수는
    // limits의 것이다. 여러 Record를 같은 예산으로 열면 그 사용량이 하나로 누적되며,
    // OperationsUsed·WorkUsed도 그 공유 누계를 돌려준다. budget이 비어 있으면 InvalidArgument다.
    //
    // 사용 예(객체 그래프):
    //   auto budget = SharedPtr<ArchiveBudget>::TryMake(allocator, maxWork, maxWork);
    //   auto record = RecordReader::Open(allocator, bytes, type, version, version, limits,
    //                                    budget.Value());
    static Result<RecordReader> Open(Allocator&, Span<const uint8>, Guid expectedSchema,
                                     uint32 minSchemaVersion, uint32 maxSchemaVersion, RecordLimits,
                                     SharedPtr<ArchiveBudget> budget) noexcept;
    // index 소유권을 이전한다. 원본은 field/iteration이 0인 비어 있는 reader로 남는다.
    RecordReader(RecordReader&&) noexcept;
    RecordReader& operator=(RecordReader&&) noexcept;
    RecordReader(const RecordReader&) = delete;
    RecordReader& operator=(const RecordReader&) = delete;

    // unknown field/type은 그대로 반환한다. 소비자가 무시하면 skip된다. End/없는 id는 NotFound다.
    // 실패해도 Next의 iteration 위치는 불변이며 사용한 예산은 환급하지 않는다.
    Result<FieldView> Next() noexcept;
    Result<FieldView> Find(Guid) noexcept;
    // 이 reader가 돌려준 필드의 값을 읽는 ArchiveReader를 연다. 이 Record 트리와 같은 예산·한도를
    // 쓴다. operation 하나를 청구하며, 이 Record 밖의 범위는 InvalidArgument다.
    Result<ArchiveReader> OpenValue(const FieldView& field) const noexcept;

    // 이 reader가 돌려준 Record wire 필드(중첩 Record — 리플렉션 struct 값)를 같은 예산으로 연다.
    // 범위 검사는 OpenValue와 같고(이 Record 밖이면 InvalidArgument), wire가 Record가 아니면
    // TypeMismatch다. 크기 한도는 이 reader의 것이고 필드 수 한도는 maxFields다. 바깥 Open이
    // payload 바이트를 이미 청구했으므로 header·TLV 색인·정렬 비용만 청구한다(중첩 깊이만큼 작업이
    // 불어나지 않는다). schemaVersion 0은 InvalidArgument, 범위 밖은 Unsupported, schema 불일치는
    // TypeMismatch다.
    //
    // 사용 예:
    //   auto inner = outer.OpenRecordField(allocator, field, structId, 1, version, maxFields);
    //   if (inner) { auto member = inner.Value().Next(); ... }
    Result<RecordReader> OpenRecordField(Allocator&, const FieldView& field, Guid expectedSchema,
                                         uint32 minSchemaVersion, uint32 maxSchemaVersion,
                                         uint32 maxFields) const noexcept;
    usize Position() const noexcept;
    usize FieldCount() const noexcept;
    uint32 SchemaVersion() const noexcept;
    // 이 Record 트리가 지금까지 함께 쓴 예산이다(값·중첩 목록·항목 reader 포함).
    uint64 OperationsUsed() const noexcept;
    uint64 WorkUsed() const noexcept;

private:
    friend class RecordListReader;
    friend class ValueArrayReader;
    RecordReader(Allocator&, ArchiveReader&&, uint32 version, RecordLimits limits) noexcept;
    // shared가 있으면 그 누적 예산을 쓰고, 없으면 limits.archive로 새 예산을 만든다. indexOnly면
    // TLV payload 바이트를 청구하지 않는다(바깥 reader가 이미 청구한 중첩 Record).
    static Result<RecordReader> OpenWith(Allocator&, Span<const uint8>, Guid expectedSchema,
                                         uint32 minSchemaVersion, uint32 maxSchemaVersion,
                                         RecordLimits, SharedPtr<ArchiveBudget> shared,
                                         bool indexOnly = false) noexcept;
    // range가 이 Record 입력 안인지 본다.
    bool Contains(Span<const uint8> range) const noexcept;
    ArchiveReader archive_;
    Array<FieldView> fields_; // 기록 순서(Next)
    Array<uint32> byId_;      // id 순으로 정렬한 fields_ index(중복 검사·Find 이진 탐색)
    uint32 schemaVersion_;
    usize next_ = 0;
    RecordLimits limits_;
};

} // namespace rocks
