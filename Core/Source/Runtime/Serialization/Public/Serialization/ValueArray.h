#pragma once

#include <Serialization/Record.h>

// 단일 값 원소 배열 필드(WireType::ValueArray = 13)다(설계: Docs/Design/ReflectionFoundations.ko.md
// 5.4·5.6절).
//
// 형식: payload = u32 원소 wire tag + u32 count + count개의 원소 payload. 원소 payload는 그 wire
// tag의 Archive 표현을 그대로 이어 붙인다(Bool 1, U32·F32 4, U64·I64·F64 8, Guid 16, String·Bytes
// u64 길이 + byte, SoftObjectReference Guid + String). Record·RecordList·ValueArray와 모르는 tag는
// 원소가 될 수 없다(struct 원소 배열은 RecordList다). 원소마다 Record envelope(최소 52 byte)를 쓰지
// 않으므로 큰 배열도 값 하나의 byte 한도 안에 든다. 빈 배열도 count 0으로 기록한다.
//
// 사용 예(쓰기 — 원소는 out에 바로 쓴다):
//   auto array = ValueArrayWriter::Begin(out, WireType::U32, count, maxBytes);
//   if (!array) {
//       return Result<void>::Failure(array.Error());
//   }
//
//   for (uint32 value : values) {
//       if (auto written = out.WriteU32(value); !written) {
//           return written;
//       }
//   }
//
//   return array.Value().End();
//
// 사용 예(읽기 — 부모 Record 트리와 같은 예산):
//   auto array = ValueArrayReader::OpenField(parent, field, WireType::U32, maxCount);
//   if (!array) {
//       return Result<void>::Failure(array.Error());
//   }
//
//   for (uint32 index = 0; index < array.Value().Count(); ++index) {
//       auto value = array.Value().Elements().ReadU32(); // 원소마다 operation 1 + byte
//       if (!value) {
//           return Result<void>::Failure(value.Error());
//       }
//   }
//
//   return array.Value().Finish(); // 남은 byte는 InvalidArgument
//
// 오류는 Archive 규약을 따른다: 잘림·잘못된 tag·크기 불일치·남은 byte는 InvalidArgument, 기대 원소
// tag와 다른 저장 tag는 TypeMismatch, 개수·크기 한도 초과는 CapacityExceeded다.
namespace rocks {

// 스키마 원소 하나의 wire 크기다(reader·writer가 wire 크기와 합쳐 쓴다). minElementBytes는 원소
// 하나의 최소 byte(0이면 wire 최소만 쓴다), exact면 모든 원소가 정확히 그 크기다. 고정 폭 wire는
// 언제나 exact이고, 가변 wire(String·Bytes·SoftObjectReference)에 exact를 주면 원소마다 길이
// prefix가 정확히 그 크기를 만드는지까지 검사한다(예: Digest = Bytes {40, true} — 길이 prefix 8 +
// 32 byte). 모순 조합(고정 폭 wire에 다른 폭, exact인데 wire 최소보다 작은 크기)은
// InvalidArgument다.
struct ValueArrayShape {
    usize minElementBytes = 0;
    bool exact = false;
};

// 원소 wire tag의 크기다: Bool 1·U32·F32 4·U64·I64·F64 8·Guid 16은 고정 폭(exact), String·Bytes 8과
// SoftObjectReference 24는 최소 크기다. 그 밖의 tag(Record·RecordList·ValueArray·모르는 값·0)는
// InvalidArgument다.
//
// 사용 예:
//   auto shape = ValueArrayElementShape(WireType::Guid); // {16, true}
Result<ValueArrayShape> ValueArrayElementShape(uint32 elementWire) noexcept;

inline Result<ValueArrayShape> ValueArrayElementShape(WireType elementWire) noexcept {
    return ValueArrayElementShape(static_cast<uint32>(elementWire));
}

// ValueArray payload를 header부터 검증하고 원소를 차례로 읽게 한다. 열 때 할당하지 않고 원소
// 수만큼의 work를 먼저 청구한다. 넘겨받은 ArchiveReader의 예산을 그대로 쓰므로
// RecordReader::OpenValue로 연 값(또는 OpenField)이면 그 Record 트리의 공유 예산이다. 원본 byte는
// reader보다 오래 살아야 한다. 이동만 되며 이동된 원본은 원소 0개의 빈 reader다.
class ValueArrayReader {
public:
    // payload(값 하나의 ArchiveReader, 위치 0)의 header를 읽고 검사한다. 검사 순서(모두 할당 전):
    // elementWire 자체가 원소 tag가 아니거나 shape가 elementWire와 모순이면 InvalidArgument → 저장
    // tag가 Record·RecordList·ValueArray·모르는 값이면 InvalidArgument → 저장 tag가 elementWire와
    // 다르면 TypeMismatch → count > maxCount면 CapacityExceeded → count × max(wire 최소, shape
    // 최소)가 남은 byte보다 크면 InvalidArgument(wire가 고정 폭이거나 shape.exact면 남은 byte가
    // 정확히 count × 크기가 아닐 때도 InvalidArgument) → count만큼 work를 청구한다(한도면
    // CapacityExceeded) → 가변 wire에 shape.exact면 원소마다 길이 prefix가 그 크기를 만드는지 본다
    // (아니면 InvalidArgument — 남은 byte를 크기 간격으로 훑으며 할당하지 않는다). shape 기본값은
    // wire 크기만 쓴다.
    static Result<ValueArrayReader> Open(ArchiveReader&& payload, uint32 elementWire,
                                         uint32 maxCount, ValueArrayShape shape = {}) noexcept;

    static Result<ValueArrayReader> Open(ArchiveReader&& payload, WireType elementWire,
                                         uint32 maxCount, ValueArrayShape shape = {}) noexcept {
        return Open(Move(payload), static_cast<uint32>(elementWire), maxCount, shape);
    }

    // parent가 돌려준 ValueArray 필드를 parent와 같은 예산·한도로 연다. operation 하나를 청구하며
    // 이동된 parent·parent 밖의 범위는 InvalidArgument, wire tag가 ValueArray가 아니면
    // TypeMismatch다. 그 뒤는 Open과 같다.
    static Result<ValueArrayReader> OpenField(const RecordReader& parent, const FieldView& field,
                                              uint32 elementWire, uint32 maxCount,
                                              ValueArrayShape shape = {}) noexcept;

    static Result<ValueArrayReader> OpenField(const RecordReader& parent, const FieldView& field,
                                              WireType elementWire, uint32 maxCount,
                                              ValueArrayShape shape = {}) noexcept {
        return OpenField(parent, field, static_cast<uint32>(elementWire), maxCount, shape);
    }

    ValueArrayReader(ValueArrayReader&& other) noexcept;
    ValueArrayReader& operator=(ValueArrayReader&& other) noexcept;
    ValueArrayReader(const ValueArrayReader&) = delete;
    ValueArrayReader& operator=(const ValueArrayReader&) = delete;

    // header의 원소 수다.
    uint32 Count() const noexcept;

    // 원소를 읽는 reader다(header 바로 뒤에 놓여 있다). 원소 tag에 맞는 Read*를 원소마다 부른다
    // (String은 ReadStringView 또는 ReadString, SoftObjectReference는 ReadGuid 다음 문자열,
    // Digest는 ReadBytes). 원소 내용(bool 0/1·UTF-8·길이)은 그 Read*가 검사한다.
    ArchiveReader& Elements() noexcept;

    // 모든 원소를 읽은 뒤 부른다. 남은 byte가 있으면 InvalidArgument다.
    Result<void> Finish() const noexcept;

private:
    ValueArrayReader(ArchiveReader&& archive, uint32 count) noexcept;

    ArchiveReader archive_;
    uint32 count_ = 0;
};

// 바깥 ArchiveWriter 위에 ValueArray payload를 제자리로 쓴다. Begin이 header(원소 tag·최종 count)를
// 쓰고, 호출자는 원소를 out에 바로 쓴 뒤 End를 부른다(값 byte는 한 번만 쓴다). maxBytes는 header를
// 포함한 payload 전체의 상한이다(보통 값 하나의 한도). 작업량은 out의 청구다. 실패하면 out의 내용은
// 정해지지 않으므로 그 writer를 버린다. out은 이 writer보다 오래 살아야 한다.
class ValueArrayWriter {
public:
    // elementWire가 원소 tag가 아니거나 shape가 그와 모순이면 InvalidArgument(reader와 같은 검사),
    // count > maxCount면 CapacityExceeded, header 8 byte + count × max(wire 최소, shape 최소)가
    // maxBytes를 넘으면 CapacityExceeded다(쓰기 전에 거부한다). maxCount·shape는 reader에 넘길 값과
    // 같게 준다(기본값은 개수 제한 없음·wire 크기). out의 쓰기 오류를 그대로 돌려준다.
    static Result<ValueArrayWriter> Begin(ArchiveWriter& out, uint32 elementWire, uint32 count,
                                          usize maxBytes, uint32 maxCount = 0xffffffffu,
                                          ValueArrayShape shape = {}) noexcept;

    static Result<ValueArrayWriter> Begin(ArchiveWriter& out, WireType elementWire, uint32 count,
                                          usize maxBytes, uint32 maxCount = 0xffffffffu,
                                          ValueArrayShape shape = {}) noexcept {
        return Begin(out, static_cast<uint32>(elementWire), count, maxBytes, maxCount, shape);
    }

    // 지금까지 쓴 payload가 maxBytes 안인지 본다(넘으면 CapacityExceeded). 가변 원소를 쓰는 동안
    // 큰 배열을 일찍 끊는 데 쓴다.
    Result<void> Check() const noexcept;

    // payload가 maxBytes를 넘으면 CapacityExceeded, 쓴 원소 byte가 count × 원소 최소 크기보다
    // 작거나 고정 폭·exact shape인데 정확히 같지 않으면 InvalidArgument다(원소 수를 잘못 쓴 호출자
    // 오류). 가변 원소는 byte 합계의 하한만 잡으며 원소 하나하나의 경계는 검사하지 않는다(reader의
    // exact 검사가 본다).
    Result<void> End() const noexcept;

private:
    ValueArrayWriter(ArchiveWriter& out, usize start, uint32 count, usize maxBytes,
                     ValueArrayShape shape) noexcept
        : out_(&out), start_(start), count_(count), maxBytes_(maxBytes), shape_(shape) {
    }

    ArchiveWriter* out_;
    usize start_;
    uint32 count_;
    usize maxBytes_;
    ValueArrayShape shape_; // wire와 합친 크기
};

} // namespace rocks
