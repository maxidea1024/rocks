#pragma once

#include <Serialization/WireSlot.h>

// Optional 필드 값(WireType::Optional = 14)이다(설계:
// Docs/Design/ReflectionExtensions.ko.md 1.2·6절 — 리플렉션 Optional<T>).
//
// 형식: payload = u8 present. 0이면 unset이고 그 뒤에 byte가 없다. 1이면 u32 내부 wire tag + 내부
// payload다. 내부 tag는 WireSlot::OptionalInner 자리의 것이다(Bool·U32·U64·I64·F32·F64·Guid·String·
// Bytes·Record). 내부가 Record(값 struct)면 payload의 남은 byte 전부가 Record 하나다(길이 prefix
// 없음). present가 0·1 밖이면 CorruptData다. 필드가 Record에 없는 것(누락 = 기준)과
// unset(present 0)은 다른 값이다.
//
// 사용 예(쓰기):
//   if (!value.HasValue()) {
//       return OptionalWireWriter::WriteUnset(out);
//   }
//
//   auto set = OptionalWireWriter::BeginSet(out, static_cast<uint32>(WireType::U32));
//   if (!set) {
//       return Result<void>::Failure(set.Error());
//   }
//
//   if (auto written = out.WriteU32(*value); !written) {
//       return written;
//   }
//
//   return set.Value().End();
//
// 사용 예(읽기 — 부모 Record 트리와 같은 예산):
//   auto optional = OptionalWireReader::OpenField(parent, field, u32Wire);
//   if (!optional) {
//       return Result<void>::Failure(optional.Error());
//   }
//
//   if (optional.Value().IsSet()) {
//       auto inner = optional.Value().Inner().ReadU32();
//       // 실패를 확인한다
//   }
//
//   return optional.Value().Finish(); // 남은 byte는 CorruptData
namespace rocks {

// Optional payload를 검증하고 내부 값을 읽게 한다. 열 때 할당하지 않는다. 넘겨받은 ArchiveReader의
// 예산을 그대로 쓴다. 원본 byte는 reader와 RecordBytes 범위보다 오래 살아야 한다. 이동만 되며
// 이동된 원본은 unset의 빈 reader다.
class ROCKS_SERIALIZATION_API OptionalWireReader {
public:
    // payload(값 하나의 ArchiveReader, 위치 0)를 검사한다. 검사 순서(모두 할당 전):
    // innerWire·shape가 Optional 내부 자리의 것이 아니거나 모순이면 InvalidArgument(이때는
    // payload를 옮기지 않는다) → present byte가 없거나 0·1 밖이면 CorruptData → present 0인데
    // byte가 남으면 CorruptData → 내부 tag가 잘렸거나 내부 자리의 tag가 아니면 CorruptData
    // → 내부 tag ≠ innerWire면 TypeMismatch → 내부 payload 크기: Record는 ReadRecordHeader 검사(그
    // 오류 — 남은 byte 전부가 Record 하나여야 한다), 그 밖은 남은 byte가 max(wire 최소, shape
    // 최소)보다 작거나 고정 폭·exact인데 정확히 같지 않거나 exact 가변 값의 길이 prefix가 맞지
    // 않으면 InvalidArgument. Record 내부는 이 검사 뒤 내용으로 넘긴다(operation 하나, 내용 byte는
    // 단독 reader일 때만 청구 — ReadPrefixedRecord와 같은 규칙. Record가 reader의 maxValueBytes보다
    // 크면 CapacityExceeded).
    static Result<OptionalWireReader> Open(ArchiveReader&& payload, uint32 innerWire,
                                           WireValueShape shape = {}) noexcept;

    static Result<OptionalWireReader> Open(ArchiveReader&& payload, WireType innerWire,
                                           WireValueShape shape = {}) noexcept {
        return Open(Move(payload), static_cast<uint32>(innerWire), shape);
    }

    // parent가 돌려준 Optional 필드를 parent와 같은 예산·한도로 연다. operation 하나를 청구하며
    // 이동된 parent·parent 밖의 범위는 InvalidArgument, wire tag가 Optional이 아니면
    // TypeMismatch다. 그 뒤는 Open과 같다.
    static Result<OptionalWireReader> OpenField(const RecordReader& parent, const FieldView& field,
                                                uint32 innerWire,
                                                WireValueShape shape = {}) noexcept;

    static Result<OptionalWireReader> OpenField(const RecordReader& parent, const FieldView& field,
                                                WireType innerWire,
                                                WireValueShape shape = {}) noexcept {
        return OpenField(parent, field, static_cast<uint32>(innerWire), shape);
    }

    OptionalWireReader(OptionalWireReader&& other) noexcept;
    OptionalWireReader& operator=(OptionalWireReader&& other) noexcept;
    OptionalWireReader(const OptionalWireReader&) = delete;
    OptionalWireReader& operator=(const OptionalWireReader&) = delete;

    // present가 1인지다.
    bool IsSet() const noexcept;

    // 내부 값을 읽는 reader다. set이고 내부가 단일 값이면 내부 payload 시작에 놓여 있어 내부 tag에
    // 맞는 Read*를 한 번 부른다(내용 — bool 0/1·UTF-8·길이 — 은 그 Read*가 검사한다). unset이거나
    // 내부가 Record면 끝에 놓여 있다.
    ArchiveReader& Inner() noexcept;

    // set이고 내부가 Record면 그 Record의 전체 byte다(원본을 빌린다 —
    // RecordReader::OpenNestedRecord로 같은 예산에서 연다). 그 밖은 빈 범위다.
    Span<const uint8> RecordBytes() const noexcept;

    // 내부 값을 읽은 뒤 부른다. 남은 byte가 있으면 CorruptData다.
    Result<void> Finish() const noexcept;

private:
    OptionalWireReader(ArchiveReader&& archive, bool set, Span<const uint8> record) noexcept;

    ArchiveReader archive_;
    Span<const uint8> record_;
    bool set_ = false;
};

// 바깥 ArchiveWriter 위에 Optional payload를 제자리로 쓴다. unset은 WriteUnset 한 번이고, set은
// BeginSet이 present와 내부 tag를 쓴 뒤 호출자가 내부 payload(단일 값의 Write* 또는 RecordWriter로
// Record 하나)를 out에 바로 쓰고 End를 부른다. 작업량은 out의 청구다. 실패하면 out의 내용은
// 정해지지 않으므로 그 writer를 버린다. out은 이 writer보다 오래 살아야 한다.
class ROCKS_SERIALIZATION_API OptionalWireWriter {
public:
    // unset 값(present 0)을 쓴다. out의 쓰기 오류를 그대로 돌려준다(실패하면 out은 그대로다).
    static Result<void> WriteUnset(ArchiveWriter& out) noexcept;

    // set 값의 머리(present 1 + u32 innerWire)를 쓴다. innerWire가 Optional 내부 tag가 아니거나
    // shape와 모순이면 쓰기 전에 InvalidArgument다(reader와 같은 검사). out의 쓰기 오류를 그대로
    // 돌려준다.
    static Result<OptionalWireWriter> BeginSet(ArchiveWriter& out, uint32 innerWire,
                                               WireValueShape shape = {}) noexcept;

    static Result<OptionalWireWriter> BeginSet(ArchiveWriter& out, WireType innerWire,
                                               WireValueShape shape = {}) noexcept {
        return BeginSet(out, static_cast<uint32>(innerWire), shape);
    }

    // 쓴 내부 payload를 검사한다: out이 머리보다 짧으면 InvalidArgument, 내부가 Record면 Record
    // 하나가 아니면(ReadRecordHeader 검사) InvalidArgument, 그 밖은 내부 byte가 최소 크기보다
    // 작거나 고정 폭·exact인데 정확히 같지 않거나 exact 가변 값의 길이 prefix가 맞지 않으면
    // InvalidArgument다(내부 값을 잘못 쓴 호출자 오류 — reader와 같은 판정).
    Result<void> End() const noexcept;

private:
    OptionalWireWriter(ArchiveWriter& out, usize inner, uint32 wire, WireValueShape shape) noexcept
        : out_(&out), inner_(inner), wire_(wire), shape_(shape) {
    }

    ArchiveWriter* out_;
    usize inner_; // 내부 payload 시작
    uint32 wire_;
    WireValueShape shape_; // wire와 합친 크기
};

} // namespace rocks
