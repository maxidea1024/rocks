#pragma once

#include <Serialization/WireSlot.h>

// 지역화 문구 값(WireType::Text = 18)이다(설계: Docs/Design/ReflectionExtensions.ko.md 1.2·13절 —
// 리플렉션 rocks::Text의 영속 형태).
//
// 형식: u8 형태 + 형태별 내용. 0 = 비어 있음(내용 없음), 1 = Invariant(String 원문), 2 =
// Localized(Guid textId + U64 sourceRevision + String fallback 원문). 그 밖의 형태 byte는
// InvalidArgument다. Localized의 textId·sourceRevision은 0이 아니다(Localization Text::Localized와
// 같은 규칙 — 0이면 InvalidArgument). 필드 값 자리에서는 payload 전체가 값 하나이고, ValueArray
// 원소·Map 값 자리에서는 같은 byte를 길이 prefix 없이 이어 붙인다. 원문 byte 상한(Localization
// TextCreateLimits)은 codec이 보고, 이 계층은 Archive 값 한도(maxValueBytes)만 건다.
//
// 사용 예:
//   TextWireValue value;
//   value.form = TextWireForm::Localized;
//   value.textId = id;
//   value.sourceRevision = 3;
//   value.source = "Inventory";
//   if (auto written = WriteTextWire(out, value); !written) {
//       return written;
//   }
//
//   auto read = ReadTextWire(reader);   // read.Value().source는 reader 원본을 빌린다
namespace rocks {

// Text 영속 형태다(형태 byte 값).
enum class TextWireForm : uint8 {
    Empty = 0,
    Invariant = 1,
    Localized = 2
};

// Text 값 하나다. source는 빌린 문자열이다(쓰기는 호출 동안, 읽기는 reader 원본 수명 동안
// 유효하다).
struct TextWireValue {
    TextWireForm form = TextWireForm::Empty;
    // Invariant 원문 또는 Localized fallback 원문이다(Empty면 빈 값).
    StringView source{};
    // Localized만 쓴다(0이 아니다). 그 밖의 형태에서는 0이다.
    Guid textId{};
    uint64 sourceRevision = 0;
};

// value를 쓴다. 형태가 셋 밖이거나 형태에 맞지 않는 칸(Empty의 원문·textId·revision, Invariant의
// textId·revision, Localized의 textId 0·revision 0)이 있으면 쓰기 전에 InvalidArgument다. 원문이
// 유효한 UTF-8이 아니면 InvalidArgument, 크기·예산 한도는 CapacityExceeded, 할당 실패는
// OutOfMemory다(out의 오류). 실패하면 out의 byte는 호출 전 그대로다(소비한 예산은 환급하지 않는다).
// ArchiveWriter의 Write*처럼 원문이 out 자신의 View를 가리켜도 된다(성장 뒤 위치를 보정한다).
// 예산은 operation 하나와 쓴 byte(원문 UTF-8 검사 포함)다.
Result<void> WriteTextWire(ArchiveWriter& out, const TextWireValue& value) noexcept;

// 값 하나를 읽는다. 형태 byte가 셋 밖이거나 내용이 잘렸거나 원문이 유효한 UTF-8이 아니거나
// Localized의 textId·sourceRevision이 0이면 InvalidArgument, 원문 길이가 maxValueBytes를 넘으면
// CapacityExceeded다. 실패하면 cursor는 호출 전 위치다(소비한 예산은 환급하지 않는다). 예산은 읽은
// Read*마다 operation 하나와 byte다.
Result<TextWireValue> ReadTextWire(ArchiveReader& in) noexcept;

} // namespace rocks
