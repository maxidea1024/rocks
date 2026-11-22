#pragma once

#include <Serialization/Record.h>

// 컨테이너 안 단일 값 자리(ValueArray 원소·Set 원소·Map 키와 값·Optional 내부)의 wire
// 규칙이다(설계: Docs/Design/ReflectionExtensions.ko.md 1.2절,
// Docs/Design/ReflectionFoundations.ko.md 5.4절).
//
// 자리마다 받는 wire tag와 값 하나의 최소 크기가 다르다(WireSlot·WireSlotShape). 값 payload는 그
// wire의 Archive 표현을 이어 붙인다: Bool 1, U32·F32 4, U64·I64·F64 8, Guid 16, String·Bytes는 u64
// 길이와 byte, SoftObjectReference는 Guid와 String, Text는 u8 형태와 형태별 내용(TextWire.h)이다.
// 길이 prefix가 없는 Record·InstancedStruct는 자리에 따라 배치가 다르다. Map 값 Record와 원소·Map
// 값 자리 InstancedStruct는 u64 길이와 내용이고(PrefixedRecordWriter·ReadPrefixedRecord·
// InstancedStructWire.h), Optional 내부 Record는 길이 prefix 없이 payload의 남은 byte 전부다
// (OptionalWire.h).
//
// 오류는 Archive 규약을 따른다: 그 자리가 받지 않는 tag·잘림·길이 불일치는 InvalidArgument, 크기
// 한도 초과는 CapacityExceeded다.
namespace rocks {

// 스키마 값 하나의 wire 크기다(ValueArray·Set·Map·Optional reader·writer가 자리의 wire 크기와 합쳐
// 쓴다). minElementBytes는 값 하나의 최소 byte(0이면 wire 최소만 쓴다), exact면 모든 값이 정확히 그
// 크기다. 고정 폭 wire는 언제나 exact이고, 길이 prefix 하나로 크기가 정해지는 가변
// wire(String·Bytes·SoftObjectReference)에 exact를 주면 값마다 길이 prefix가 정확히 그 크기를
// 만드는지까지 검사한다(예: Digest = Bytes {40, true} — 길이 prefix 8 + 32 byte). 모순 조합(고정 폭
// wire에 다른 폭, exact인데 wire 최소보다 작은 크기, 내용이 길이 prefix 하나로 정해지지 않는
// Record·InstancedStruct·Text에 exact)과 Map 키·값 최소 크기의 합이 usize를 넘는 shape는
// InvalidArgument다.
struct WireValueShape {
    usize minElementBytes = 0;
    bool exact = false;
};

// 단일 값이 놓이는 컨테이너 안 자리다. 자리마다 받는 tag는 아래와 같다(Record·RecordList·
// ValueArray·Optional·Set·Map·0·모르는 값은 어느 자리에서도 받지 않는다). wire가 같은 종류끼리는
// tag로 구별할 수 없으므로, 그 자리에 올 수 없는 종류 중 받는 tag와 wire가 같은 것(Optional 내부의
// ClassRef·WeakObjectRef = Guid·U32, Set 원소·Map 키의 WeakObjectRef·ObjectRef = U32)은 codec이
// schema로 거른다.
enum class WireSlot : uint8 {
    // ValueArray 원소(13): Bool·U32·U64·I64·F32·F64·Guid·String·Bytes·SoftObjectReference·
    // InstancedStruct(u64 길이 + 내용)·Text. struct 원소 배열은 RecordList다.
    ArrayElement,
    // Set 원소(15)·Map 키(16): Bool·U32·U64·I64·Guid·String·Bytes·SoftObjectReference. 실수는
    // 같음이 byte 같음과 다르고 InstancedStruct·Text는 키 종류가 없어 받지 않는다(1.3절 성질 표).
    Key,
    // Map 값(16): ArrayElement의 tag와 Record(u64 길이 + Record — 값 struct).
    MapValue,
    // Optional 내부(14): Bool·U32·U64·I64·F32·F64·Guid·String·Bytes·Record(payload의 남은 byte
    // 전부). 참조(SoftObjectReference)·InstancedStruct·Text는 Optional 내부 종류가 없어 받지
    // 않는다.
    OptionalInner
};

// slot 자리에서 wire tag 값 하나의 크기다: Bool 1·U32·F32 4·U64·I64·F64 8·Guid 16은 고정 폭(exact),
// String·Bytes 8·SoftObjectReference 24·InstancedStruct 8(u64 길이)·Text 1(형태 byte)·Map 값 Record
// 52(u64 길이 + header 44)·Optional 내부 Record 44는 최소 크기다. 그 자리가 받지 않는 tag는
// InvalidArgument다.
//
// 사용 예:
//   auto shape = WireSlotShape(WireSlot::Key, static_cast<uint32>(WireType::Guid)); // {16, true}
Result<WireValueShape> WireSlotShape(WireSlot slot, uint32 wire) noexcept;

inline Result<WireValueShape> WireSlotShape(WireSlot slot, WireType wire) noexcept {
    return WireSlotShape(slot, static_cast<uint32>(wire));
}

// 원소·Map 값 자리의 길이 prefix 내용(u64 길이 + 내용)을 바깥 ArchiveWriter 위에 제자리로 쓴다. Map
// 값 Record(wire Record — 내용은 Record 하나)와 원소·Map 값 자리 InstancedStruct(wire
// InstancedStruct — 내용은 0 byte(비어 있음) 또는 Record 하나)에 쓴다. Begin이 u64 길이 자리를
// 쓰고, 호출자가 내용을 RecordWriter::Begin으로 out에 바로 쓴 뒤 End가 내용을 검사하고 길이를
// 채운다(RecordListWriter 항목과 같은 배치 — 내용 byte는 한 번만 쓴다). 작업량은 out의 청구다.
// 실패하면 out의 내용은 정해지지 않으므로 그 writer를 버린다. out은 이 writer보다 오래 살아야 한다.
// 복사하지 않는다(이동은 Result로 돌려주는 데만 쓴다).
//
// 사용 예(Map<K, struct>의 값 하나 — 키 payload를 쓴 직후):
//   auto value = PrefixedRecordWriter::Begin(out, static_cast<uint32>(WireType::Record));
//   if (!value) {
//       return Result<void>::Failure(value.Error());
//   }
//
//   auto record = RecordWriter::Begin(out, structId, version, limits);
//   // ... 멤버를 record.Value().BeginField·EndField 사이에 out에 쓴다
//   if (auto ended = record.Value().End(); !ended) {
//       return ended;
//   }
//
//   return value.Value().End();
class ROCKS_SERIALIZATION_API PrefixedRecordWriter {
public:
    // wire가 Record·InstancedStruct가 아니면 쓰기 전에 InvalidArgument다. out의 쓰기 오류를 그대로
    // 돌려준다.
    static Result<PrefixedRecordWriter> Begin(ArchiveWriter& out, uint32 wire) noexcept;

    static Result<PrefixedRecordWriter> Begin(ArchiveWriter& out, WireType wire) noexcept {
        return Begin(out, static_cast<uint32>(wire));
    }

    PrefixedRecordWriter(const PrefixedRecordWriter&) = delete;
    PrefixedRecordWriter& operator=(const PrefixedRecordWriter&) = delete;
    PrefixedRecordWriter(PrefixedRecordWriter&&) noexcept = default;
    PrefixedRecordWriter& operator=(PrefixedRecordWriter&&) noexcept = default;

    // 내용을 검사하고 u64 길이를 채운다. 이미 끝냈거나 out이 길이 자리보다 짧으면 InvalidArgument,
    // 내용이 Record 하나가 아니면 InvalidArgument다(ReadRecordHeader 검사 — wire InstancedStruct는
    // 0 byte(비어 있음)도 받는다).
    Result<void> End() noexcept;

private:
    PrefixedRecordWriter(ArchiveWriter& out, usize start, uint32 wire) noexcept
        : out_(&out), start_(start), wire_(wire) {
    }

    ArchiveWriter* out_;
    usize start_; // u64 길이 자리
    uint32 wire_;
    bool ended_ = false;
};

// 원소·Map 값 자리의 Record(u64 길이 + Record)를 읽어 Record byte를 돌려준다(원본을 빌린다).
// cursor는 성공할 때만 옮긴다(실패하면 호출 전 위치). 길이 prefix가 잘렸거나 길이가 남은 byte보다
// 크면 InvalidArgument, 길이가 reader의 maxValueBytes를 넘으면 CapacityExceeded, 내용이 Record가
// 아니면 ReadRecordHeader의 오류다(0 byte도 InvalidArgument).
//
// 예산: operation 하나와 길이 prefix 8을 청구한다. 내용 byte는 단독 reader(ArchiveReader::Create)일
// 때만 청구한다 — Record 트리의 공유 예산 reader(RecordReader::OpenValue·각 reader의 OpenField)면
// 그 byte를 트리의 Open이 이미 청구했으므로 다시 청구하지 않는다(값 Record 안 Map 값 Record처럼
// 중첩해도 작업이 깊이 배수로 불어나지 않는다). 돌려준 Record는 RecordReader::OpenNestedRecord로
// 같은 예산에서 연다.
//
// 사용 예:
//   auto record = ReadPrefixedRecord(map.Value().Entries());
//   if (!record) {
//       return Result<void>::Failure(record.Error());
//   }
//
//   auto value = parent.OpenNestedRecord(allocator, record.Value(), structId, 1, version, limit);
ROCKS_SERIALIZATION_API Result<Span<const uint8>>
ReadPrefixedRecord(ArchiveReader& entries) noexcept;

} // namespace rocks
