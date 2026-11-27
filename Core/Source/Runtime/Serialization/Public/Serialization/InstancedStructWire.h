#pragma once

#include <Serialization/WireSlot.h>

// InstancedStruct 값(WireType::InstancedStruct = 17)이다(설계:
// Docs/Design/ReflectionExtensions.ko.md 1.2·11절 — 리플렉션 InstancedStruct<Base>).
//
// 형식: 필드 값 자리에서는 payload 전체가 내용이다 — 0 byte면 비어 있음, 아니면 Record
// 하나(schema는 담긴 struct ID, schema version은 저장 당시 struct 버전). ValueArray 원소·Map 값
// 자리에서는 u64 길이와 같은 내용이다(길이 0 = 비어 있음). 담긴 struct ID는 Record header에
// 있으므로 읽는 쪽이 header를 먼저 보고 타입을 찾은 뒤 RecordReader::OpenNestedRecord로 Record를
// 연다.
//
// 쓰기: 필드 값은 비어 있으면 아무것도 쓰지 않고(빈 payload), 값이 있으면 RecordWriter::Begin(out,
// structId, version)으로 Record 하나를 쓴다. 원소·Map 값 자리는 PrefixedRecordWriter::Begin(out,
// InstancedStruct)·End 사이에 같은 내용을 쓴다(사이에 아무것도 쓰지 않으면 비어 있음).
//
// 사용 예(읽기 — 필드 값):
//   auto value = parent.OpenValue(field);   // 호출자가 field.wireType을 먼저 확인한다
//   if (!value) {
//       return Result<void>::Failure(value.Error());
//   }
//
//   auto content = ReadInstancedStructField(value.Value());
//   if (!content) {
//       return Result<void>::Failure(content.Error());
//   }
//
//   if (!content.Value().IsEmpty()) {
//       // content.Value().schema로 타입을 찾고 그 버전 범위로 연다
//       auto record = parent.OpenNestedRecord(allocator, content.Value().record,
//                                             content.Value().schema, 1, version, maxFields);
//   }
namespace rocks {

// InstancedStruct 값 하나의 내용이다(원본을 빌린다).
struct InstancedStructWire {
    // Record 전체 byte다. 비어 있으면 빈 범위다.
    Span<const uint8> record{};
    // Record header의 schema(담긴 struct ID)와 schema version(저장 당시 struct 버전)이다. 비어
    // 있으면 0이다.
    Guid schema{};
    uint32 schemaVersion = 0;

    bool IsEmpty() const noexcept {
        return record.Size() == 0;
    }
};

// 필드 값 자리: payload의 남은 byte 전부를 내용으로 읽는다(성공하면 payload는 끝에 놓인다). 남은
// byte가 0이면 비어 있음이고, 아니면 Record 하나여야 한다(ReadRecordHeader의 오류). 남은 byte가
// reader의 maxValueBytes를 넘으면 CapacityExceeded다. 실패하면 cursor는 호출 전 위치다. 예산은
// operation 하나이고 내용 byte는 단독 reader일 때만 청구한다(ReadPrefixedRecord와 같은 규칙).
ROCKS_SERIALIZATION_API Result<InstancedStructWire>
ReadInstancedStructField(ArchiveReader& payload) noexcept;

// 원소·Map 값 자리: u64 길이 + 내용 하나를 읽는다. 길이 0은 비어 있음, 아니면 내용이 Record
// 하나여야 한다(ReadRecordHeader의 오류). 길이 prefix가 잘렸거나 길이가 남은 byte보다 크면
// CorruptData, maxValueBytes를 넘으면 CapacityExceeded다. 실패하면 cursor는 호출 전 위치다.
// 예산은 ReadPrefixedRecord와 같다.
ROCKS_SERIALIZATION_API Result<InstancedStructWire>
ReadInstancedStructElement(ArchiveReader& elements) noexcept;

} // namespace rocks
