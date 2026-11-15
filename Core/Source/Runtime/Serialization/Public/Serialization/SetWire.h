#pragma once

#include <Serialization/WireSlot.h>

// 키 원소 집합 필드(WireType::Set = 15)다(설계: Docs/Design/ReflectionExtensions.ko.md 1.2·7.2절 —
// 리플렉션 OrderedSet<T>).
//
// 형식: payload = u32 원소 wire tag + u32 count + count개의 원소 payload(삽입 순서). 배치는
// ValueArray와 같지만 wire tag가 달라 Record만 읽어도 집합임이 드러나고, Array → Set 같은 schema
// 변경이 조용히 통과하지 않는다. 원소 tag는 WireSlot::Key 자리의 것이다(Bool·U32·U64·I64·Guid·
// String·Bytes·SoftObjectReference). 빈 집합도 count 0으로 기록한다.
//
// 이 계층은 형식(tag·count·길이·잘림)만 본다. 원소 중복은 codec 1단계가 종류 규칙으로
// 판정한다(String은 byte, Name은 Core Name 규칙 — 중복은 InvalidArgument).
//
// 사용 예(쓰기 — 원소는 out에 바로 쓴다):
//   auto set = SetWireWriter::Begin(out, static_cast<uint32>(WireType::String), count, maxBytes);
//   if (!set) {
//       return Result<void>::Failure(set.Error());
//   }
//
//   for (StringView element : elements) {
//       if (auto written = out.WriteString(element); !written) {
//           return written;
//       }
//   }
//
//   return set.Value().End();
//
// 사용 예(읽기 — 부모 Record 트리와 같은 예산):
//   auto set = SetWireReader::OpenField(parent, field, stringWire, maxCount);
//   if (!set) {
//       return Result<void>::Failure(set.Error());
//   }
//
//   for (uint32 index = 0; index < set.Value().Count(); ++index) {
//       auto element = set.Value().Elements().ReadStringView(); // 원소마다 operation 1 + byte
//       if (!element) {
//           return Result<void>::Failure(element.Error());
//       }
//   }
//
//   return set.Value().Finish(); // 남은 byte는 InvalidArgument
//
// 오류는 ValueArray와 같다: 잘림·받지 않는 tag·크기 불일치·남은 byte는 InvalidArgument, 기대 원소
// tag와 다른 저장 tag는 TypeMismatch, 개수·크기 한도 초과는 CapacityExceeded다.
namespace rocks {

// Set payload를 header부터 검증하고 원소를 차례로 읽게 한다. 열 때 할당하지 않고 원소 수만큼의
// work를 먼저 청구한다. 넘겨받은 ArchiveReader의 예산을 그대로 쓴다. 원본 byte는 reader보다 오래
// 살아야 한다. 이동만 되며 이동된 원본은 원소 0개의 빈 reader다.
class SetWireReader {
public:
    // payload(값 하나의 ArchiveReader, 위치 0)의 header를 검사한다. 검사 순서는
    // ValueArrayReader::Open과 같고 원소 tag 자리만 WireSlot::Key다: elementWire·shape가 키 자리의
    // 것이 아니거나 모순이면 InvalidArgument(이때는 payload를 옮기지 않는다) → 저장 tag가 키 tag가
    // 아니면 InvalidArgument → 저장 tag ≠ elementWire면 TypeMismatch → count > maxCount면
    // CapacityExceeded → count × 원소 최소 크기가 남은 byte보다 크면(고정 폭·exact는 정확히 같지
    // 않으면) InvalidArgument → count만큼 work 청구 → exact 가변 원소의 길이 prefix 검사.
    static Result<SetWireReader> Open(ArchiveReader&& payload, uint32 elementWire, uint32 maxCount,
                                      WireValueShape shape = {}) noexcept;

    static Result<SetWireReader> Open(ArchiveReader&& payload, WireType elementWire,
                                      uint32 maxCount, WireValueShape shape = {}) noexcept {
        return Open(Move(payload), static_cast<uint32>(elementWire), maxCount, shape);
    }

    // parent가 돌려준 Set 필드를 parent와 같은 예산·한도로 연다. operation 하나를 청구하며 이동된
    // parent·parent 밖의 범위는 InvalidArgument, wire tag가 Set이 아니면 TypeMismatch다. 그 뒤는
    // Open과 같다.
    static Result<SetWireReader> OpenField(const RecordReader& parent, const FieldView& field,
                                           uint32 elementWire, uint32 maxCount,
                                           WireValueShape shape = {}) noexcept;

    static Result<SetWireReader> OpenField(const RecordReader& parent, const FieldView& field,
                                           WireType elementWire, uint32 maxCount,
                                           WireValueShape shape = {}) noexcept {
        return OpenField(parent, field, static_cast<uint32>(elementWire), maxCount, shape);
    }

    SetWireReader(SetWireReader&& other) noexcept;
    SetWireReader& operator=(SetWireReader&& other) noexcept;
    SetWireReader(const SetWireReader&) = delete;
    SetWireReader& operator=(const SetWireReader&) = delete;

    // header의 원소 수다.
    uint32 Count() const noexcept;

    // 원소를 읽는 reader다(header 바로 뒤). 원소 tag에 맞는 Read*를 원소마다 부른다
    // (ValueArrayReader::Elements와 같다).
    ArchiveReader& Elements() noexcept;

    // 모든 원소를 읽은 뒤 부른다. 남은 byte가 있으면 InvalidArgument다.
    Result<void> Finish() const noexcept;

private:
    SetWireReader(ArchiveReader&& archive, uint32 count) noexcept;

    ArchiveReader archive_;
    uint32 count_ = 0;
};

// 바깥 ArchiveWriter 위에 Set payload를 제자리로 쓴다(ValueArrayWriter와 같은 규약). maxBytes는
// header를 포함한 payload 전체의 상한이다. 작업량은 out의 청구다. 실패하면 out의 내용은 정해지지
// 않으므로 그 writer를 버린다. out은 이 writer보다 오래 살아야 한다.
class SetWireWriter {
public:
    // elementWire가 키 tag가 아니거나 shape와 모순이면 InvalidArgument(reader와 같은 검사), count >
    // maxCount나 header 8 byte + count × 원소 최소 크기 > maxBytes면 쓰기 전에 CapacityExceeded다.
    // maxCount·shape는 reader에 넘길 값과 같게 준다. out의 쓰기 오류를 그대로 돌려준다.
    static Result<SetWireWriter> Begin(ArchiveWriter& out, uint32 elementWire, uint32 count,
                                       usize maxBytes,
                                       uint32 maxCount = NumericLimits<uint32>::Max(),
                                       WireValueShape shape = {}) noexcept;

    static Result<SetWireWriter> Begin(ArchiveWriter& out, WireType elementWire, uint32 count,
                                       usize maxBytes,
                                       uint32 maxCount = NumericLimits<uint32>::Max(),
                                       WireValueShape shape = {}) noexcept {
        return Begin(out, static_cast<uint32>(elementWire), count, maxBytes, maxCount, shape);
    }

    // 지금까지 쓴 payload가 maxBytes 안인지 본다(넘으면 CapacityExceeded). out이 Begin 위치보다
    // 짧아졌으면(호출자가 out을 줄였다) InvalidArgument다.
    Result<void> Check() const noexcept;

    // out이 Begin 위치보다 짧아졌으면 InvalidArgument, payload가 maxBytes를 넘으면
    // CapacityExceeded, 원소 byte가 count × 원소 최소 크기보다 작거나 고정 폭·exact인데 정확히 같지
    // 않으면 InvalidArgument다.
    Result<void> End() const noexcept;

private:
    SetWireWriter(ArchiveWriter& out, usize start, uint32 count, usize maxBytes,
                  WireValueShape shape) noexcept
        : out_(&out), start_(start), count_(count), maxBytes_(maxBytes), shape_(shape) {
    }

    ArchiveWriter* out_;
    usize start_;
    uint32 count_;
    usize maxBytes_;
    WireValueShape shape_; // wire와 합친 크기
};

} // namespace rocks
