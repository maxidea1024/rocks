#pragma once

#include <Core/Diagnostics/Correlation.h>
#include <Base/NumericLimits.h>
#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Core/Diagnostics/LogRecord.h>
#include <Core/Diagnostics/SourceLocation.h>
#include <Core/Text/StringView.h>

namespace rocks {

// LogProperty 값의 종류다. JSON lines sink(LG4)가 숫자·bool을 JSON 숫자·불리언으로 내는 데 쓴다.
// - String: text만 의미가 있다(문자열, char, 포인터, 사용자 Formatter 결과, NaN·무한대 실수, 펼친
//   값 안의 enum 이름·"<unsupported>"·"<format error>")
// - Signed·Unsigned: signedValue·unsignedValue(enum은 바탕 정수 타입을 따른다)
// - Float: floatValue(유한한 실수만. float는 정확히 double로 넓힌다)
// - Bool: boolValue
// - Structure: 펼친 객체("{@Name}", LG16). typeTag와 자식 노드(subtreeBegin·subtreeCount)가 있다.
// - Array: 펼친 값 안의 배열. unsignedValue가 전체 원소 수이고 자식 노드는 담은 원소다.
// - Null: 펼친 값 안의 null(빈 Optional·null 참조·null C 문자열)
enum class LogPropertyKind : uint8 {
    String,
    Signed,
    Unsigned,
    Float,
    Bool,
    Structure,
    Array,
    Null
};

// LogProperty::parent의 "맨 위 속성의 직접 자식" 표지다.
inline constexpr uint32 NoLogPropertyParent = NumericLimits<uint32>::Max();

// 이름·값 한 쌍이다. 메시지 템플릿의 속성(`{Count}` → Count=3)과 문맥(`World=Main`)에 쓴다.
// - name: 이름 자리면 그 이름, 위치 자리면 Arg0·Arg1…, 펼친 값의 필드면
//   필드 이름(배열 원소는 빈 값)
// - text: 값을 기본 서식("{}")으로 렌더한 UTF-8 텍스트다. 모든 종류에
//   채운다. 맨 위 Structure는 메시지에 렌더한 펼친 텍스트
//   (`Player { Hp: 30 }`)이고, 펼친 값 안의 Structure·Array 노드는 빈 값이다.
// - kind와 원시 값: kind에 맞는 칸 하나만 의미가 있다(String이면 원시 값 없음).
// view는 모두 LogEvent와 같은 수명(sink 콜백 동안)만 유효하다.
//
// 펼친 값(LG16)의 트리: Structure·Array의 자식과 그 자손은 LogEvent::structureNodes의
// [subtreeBegin, subtreeBegin + subtreeCount)에 앞 순서(pre-order)로 이어진다. 직접 자식은 첫
// 노드부터 LogPropertySubtreeSpan만큼 건너뛰며 찾는다. parent는 structureNodes 안 부모 번호다(맨 위
// 속성의 직접 자식이면 NoLogPropertyParent).
//
// 사용 예(JSON sink):
//   if (property.kind == LogPropertyKind::Signed) { json.WriteInteger(property.signedValue); }
//   else { json.WriteString(property.text); }
//
// 사용 예(직접 자식 순회):
//   for (uint32 index = node.subtreeBegin; index < node.subtreeBegin + node.subtreeCount;
//        index += LogPropertySubtreeSpan(event.structureNodes[index])) {
//       const LogProperty& child = event.structureNodes[index];
//   }
struct LogProperty {
    StringView name;
    StringView text;
    // Structure의 타입 태그(LogStructure<T>::TypeTag, 리플렉션 타입은 스키마 이름)다. 없으면 빈 값.
    StringView typeTag;
    LogPropertyKind kind = LogPropertyKind::String;
    // Structure·Array: 상한(깊이·필드·원소 수)·공간·노드 칸 때문에 내용
    // 일부를 버렸다. 그 안에서 잃은 내용이 있으면 조상도 모두 선다.
    // 펼친 값 안의 String: 공간 때문에 값을 잘랐다.
    bool truncated = false;
    uint32 parent = NoLogPropertyParent;
    uint32 subtreeBegin = 0;
    uint32 subtreeCount = 0;

    union {
        int64 signedValue = 0;
        uint64 unsignedValue;
        double floatValue;
        bool boolValue;
    };
};

// node와 그 자손이 structureNodes에서 차지하는 칸 수다(다음 형제까지의 거리).
constexpr uint32 LogPropertySubtreeSpan(const LogProperty& node) noexcept {
    const bool container =
        node.kind == LogPropertyKind::Structure || node.kind == LogPropertyKind::Array;
    return 1 + (container ? node.subtreeCount : 0);
}

// LogEvent::flags의 비트다. 여러 비트를 함께 켤 수 있다.
enum class LogEventFlags : uint32 {
    None = 0,
    // 렌더 버퍼(메시지·속성 텍스트·속성 칸)가 모자라 message나 properties가 잘렸다.
    MessageTruncated = 1u << 0,
    // 문맥 스택 깊이를 넘어 안쪽 문맥을 버렸다(후속 단계 LG4가 켠다).
    ContextTruncated = 1u << 1,
    // 캡처할 때 값(문자열·Formatter 결과·런타임 템플릿)이 슬롯·블록에 다 들어가지 않아 잘렸거나,
    // 펼친 값("{@Name}")이 깊이·필드·원소 상한이나 공간 때문에 일부만 담겼다.
    ValueTruncated = 1u << 2,
    // 런타임 템플릿이 잘못돼 message가 원본 템플릿 + "[template error: <code>]"다.
    TemplateError = 1u << 3,
    // 자리 하나 이상의 서식이 실패해 그 자리가 "<format error>"다(사용자 Formatter 오류, 잘못된
    // UTF-8 문자열, null 문자열 등).
    FormatError = 1u << 4
};

constexpr LogEventFlags operator|(LogEventFlags left, LogEventFlags right) noexcept {
    return static_cast<LogEventFlags>(static_cast<uint32>(left) | static_cast<uint32>(right));
}

constexpr LogEventFlags operator&(LogEventFlags left, LogEventFlags right) noexcept {
    return static_cast<LogEventFlags>(static_cast<uint32>(left) & static_cast<uint32>(right));
}

// value에 flags 중 하나라도 켜져 있는지다.
constexpr bool HasAnyLogEventFlags(LogEventFlags value, LogEventFlags flags) noexcept {
    return (value & flags) != LogEventFlags::None;
}

// sink가 받는 로그 기록 한 건이다(Docs/Design/LoggingSystem.ko.md 9절). 렌더된 메시지와 원본 정보를
// 함께 담아 sink가 출력 형식을 고를 수 있게 한다.
//
// 수명: 모든 view(문자열·Span)는 RuntimeLogSink::Write 콜백 동안만 유효하다. 보관하거나 다른
// 스레드로 넘기려면 복사한다. location의 문자열만 정적 저장소다.
//
// 기록 경로가 아직 채우지 않는 칸은 빈 값·0이다. Logger::Write는 level·category·message·location·
// threadId·monotonicNanoseconds를 채운다. 캡처 렌더(RenderLogCapture, LogCapture.h)는 여기에
// messageTemplate·eventId·properties·frame·flags를 더한다. 나머지는 칸 주석의 단계에서 채운다.
//
// 사용 예(sink 구현):
//   Result<void> Write(const LogEvent& event) noexcept override {
//       if (event.level >= LogLevel::Error) {
//           ++errorCount_;
//       }
//
//       return CopyToRing(event.category, event.message); // 보관하려면 복사한다
//   }
struct LogEvent {
    LogLevel level = LogLevel::Info;
    // 카테고리 이름(LogCategory::Name). ASCII이며 점으로 구간을 나눈다.
    StringView category;
    // 원본 메시지 템플릿(`"Loaded {Count} assets"`). 캡처 렌더(RenderLogCapture)가 채운다.
    // Logger::Write 경로는 빈 값이다.
    StringView messageTemplate;
    // 템플릿 텍스트의 해시로 만든 사건 종류 id(LogTemplate::EventId). 캡처 렌더가 채운다.
    uint64 eventId = 0;
    // 렌더된 메시지. 유효한 UTF-8이며 embedded NUL을 포함할 수 있다.
    StringView message;
    // 템플릿 인자의 속성(인자 순서). 캡처 렌더가 채운다.
    Span<const LogProperty> properties;
    // 펼친 값("{@Name}")의 노드다. properties의 Structure 속성이 subtreeBegin·subtreeCount로
    // 가리킨다 (LogProperty 설명).
    Span<const LogProperty> structureNodes;
    // 기록 시점의 문맥(LogContextScope). LG4에서 채운다. 그 전에는 비어 있다.
    Span<const LogProperty> context;
    // 기록을 부른 소스 위치.
    SourceLocation location;
    // 기록한 스레드의 OS id(platform::CurrentThreadId).
    uint64 threadId = 0;
    // 기록한 스레드의 이름. LG3에서 채운다. 그 전에는 빈 값이다(출력은 id로 대신한다).
    StringView threadName;
    // 엔진 프레임 번호. 캡처에 담긴 값이다(LG6 LogSystem::SetFrame 전에는 0).
    uint64 frame = 0;
    // 기록 시각, 단조 시계 ns(platform::MonotonicNanoseconds와 같은 관계로 tick에서 환산). UTC가
    // 아니다.
    uint64 monotonicNanoseconds = 0;
    // 기록 시각, 캡처한 단조 tick(platform::MonotonicTicks 원천). trace 기록이 변환 없이 쓴다.
    uint64 monotonicTicks = 0;
    // 기록한 스레드의 상관 id(CorrelationScope 맨 위)다. hasCorrelation이 false면 비었다.
    CorrelationIds correlation;
    bool hasCorrelation = false;
    // 기록 시각, UTC tick(platform::UtcTicks 단위). LG3 출력 스레드가 환산해 채운다. 그 전에는
    // 0이다.
    int64 utcTicks = 0;
    LogEventFlags flags = LogEventFlags::None;
};

} // namespace rocks
