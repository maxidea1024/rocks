#pragma once

#include <NodeGraphTools/GraphText.h>
#include <ReflectionText/TextUse.h>

// 텍스트 codec의 내부 공용 정의다(GraphRecordText.cpp·GraphTextWrite.cpp·GraphTextRead.cpp).
namespace rocks::nodegraph::text_detail {

// 형식 이름·버전이다(9.4절).
inline constexpr char FragmentFormat[] = "rocks.graph-fragment";
inline constexpr char DocumentFormat[] = "rocks.graph-document";
inline constexpr uint32 TextFormatVersion = 1;

// Guid 표기(36자 소문자)의 길이다.
inline constexpr usize GuidTextBytes = 36;

// Guid의 36자 표기다(Formatter<Guid>).
struct GuidText {
    char bytes[GuidTextBytes] = {};

    StringView View() const noexcept {
        return StringView(bytes, GuidTextBytes);
    }
};

GuidText FormatGuidText(Guid value) noexcept;

// 레코드 값 하나를 writer에 쓴다(9.4절 레코드 값): records가 거짓이고 Record의 struct가 등록되어
// 해석·리플렉션 텍스트화가 되면 {"type", "value"}, 아니면 {"record": base64}. 빈 Record는 호출자가
// 키째 뺀다(여기 오면 InvalidArgument).
Result<void> WriteRecordValue(GraphTextContext& context, const PropertyRecord& record,
                              reflection_text::TextUse use, bool records,
                              JsonWriter& writer) noexcept;

// 레코드 값 하나를 읽는 결과다.
enum class RecordRead : uint8 {
    // 바이트를 채웠다.
    Resolved,
    // 텍스트의 type이 등록되어 있지 않다(바이트는 비어 있고 unresolvedType에 type 문자열).
    Unresolved
};

// 레코드 값 JSON 하나를 Record 바이트로 읽는다. 형식 오류·리플렉션 텍스트 오류는 진단을 남기고
// InvalidArgument다.
Result<RecordRead> ReadRecordValue(GraphTextContext& context, JsonValue value,
                                   reflection_text::TextUse use, PropertyRecord& record,
                                   String& unresolvedType,
                                   GraphTextDiagnostics& diagnostics) noexcept;

} // namespace rocks::nodegraph::text_detail
