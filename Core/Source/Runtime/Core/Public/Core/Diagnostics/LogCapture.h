#pragma once

#include <Base/NumericLimits.h>
#include <Base/TypeTraits.h>
#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Core/Diagnostics/LogCategory.h>
#include <Core/Diagnostics/LogEvent.h>
#include <Core/Diagnostics/LogRecord.h>
#include <Core/Diagnostics/LogTemplate.h>
#include <Core/Result.h>
#include <Core/Text/StringBuilder.h>

// 로그 기록의 캡처 인코딩과 렌더다(Docs/Design/LoggingSystem.ko.md 8.1·8.3·10절).
//
// 흐름: 호출 스레드가 CaptureLog로 기록 한 건을 호출자가 준 고정 byte 버퍼(준비 버퍼·큐 슬롯)에
// 담는다. 담긴 byte는 위치와 무관하므로(포인터 대신 오프셋) 그대로 복사해 옮겨도 된다. 출력 쪽이
// RenderLogCapture로 메시지를 렌더하고 LogEvent를 채운 뒤 sink에 넘기고, 끝나면
// ReleaseLogCaptureBlocks로 빌린 블록을 돌려준다.
//
// 캡처가 담는 것: 레벨, 카테고리(정적 객체 포인터), 리터럴 템플릿 포인터 또는 복사한 런타임 템플릿,
// EventId, 호출 위치, 단조 시각·스레드·프레임(호출자가 준 값), 플래그, 인자 값.
// - 정수·실수·bool·char·포인터·enum은 원시 값, 문자열은 byte 복사(UTF-8 검증은 렌더 때),
// - 사용자 Formatter<T> 타입은 캡처할 때 FixedStringBuilder로 렌더한다(객체 수명이 끝날 수
// 있으므로).
//   그 인자를 처음 쓴 자리의 spec으로 한 번만 렌더한다. 같은 인자를 다른 spec으로 여러 번 쓰면 첫
//   spec이 모두에 쓰인다. 렌더한 결과가 슬롯에 다 들어가지 않으면 블록을 빌려 Formatter를 한 번 더
//   부른다(Formatter는 같은 값에 같은 결과를 내야 한다).
// - 슬롯에 들어가지 않는 문자열·Formatter 결과·런타임 템플릿은 LogBlockProvider에서 블록을 빌린다.
//   블록이 없으면 잘라서(UTF-8 문자 경계) LogEventFlags::ValueTruncated를 세운다.
//
// 힙 할당은 없다. 모든 쓰기는 호출자가 준 버퍼·블록과 FixedStringBuilder로 한다.
namespace rocks {

// 블록 없음 표지다.
inline constexpr uint32 NoLogBlock = NumericLimits<uint32>::Max();

// 빌린 블록 하나다. index는 provider가 블록을 다시 찾는 번호, bytes는 쓸 수 있는 영역이다.
struct LogBlock {
    uint32 index = NoLogBlock;
    Span<uint8> bytes;
};

// 큰 값을 담을 블록을 빌려 주는 쪽이다(LG3의 lock-free 블록 풀이 구현한다). 캡처는 TryBorrow로 빌려
// 쓰고, 렌더는 Resolve로 읽고, 기록을 다 쓴 쪽(출력 스레드, 또는 캡처를 버리는 생산자)이
// ReleaseLogCaptureBlocks로 돌려준다.
//
// 계약:
// - 모든 함수는 여러 스레드에서 동시에 불릴 수 있다(구현이 동기화한다). 대기하면 안 된다.
// - TryBorrow: 블록이 없으면 false(기다리지 않는다). 성공하면 block.bytes는 Release 전까지 이
// 호출자만
//   쓴다. bytes는 2 byte 이상이어야 한다.
// - Resolve: 빌려 준 index의 영역이다. 모르는 index면 빈 Span이다.
// - Release: 빌려 준 index를 돌려받는다. 같은 index를 두 번 돌려주지 않는다.
class LogBlockProvider {
public:
    virtual ~LogBlockProvider() noexcept = default;
    virtual bool TryBorrow(LogBlock& block) noexcept = 0;
    virtual Span<const uint8> Resolve(uint32 index) const noexcept = 0;
    virtual void Release(uint32 index) noexcept = 0;
};

// 캡처를 쓸 곳과 머리에 담을 실행 문맥이다.
// - buffer: 기록 한 건을 담을 고정 영역(준비 버퍼·큐 슬롯). 최소 LogCaptureMinimumBytes(인자 수).
// - blocks: 큰 값용 블록 provider. 없으면(null) 넘치는 값은 잘린다.
// - monotonicNanoseconds·threadId·frame: 렌더한 LogEvent에 그대로 실린다(기록 경로가 채운다).
struct LogCaptureTarget {
    Span<uint8> buffer;
    LogBlockProvider* blocks = nullptr;
    uint64 monotonicNanoseconds = 0;
    uint64 threadId = 0;
    uint64 frame = 0;
};

// 렌더 결과를 담을 곳이다. LogEvent의 view가 이 영역과 캡처·블록을 가리킨다.
// - message: 렌더한 메시지(끝 NUL 자리 포함, 1 byte 이상). 넘치면 잘리고 MessageTruncated.
// - propertyText: 숫자·char·포인터 속성의 텍스트를 쓰는 영역. 모자라면 잘리고 MessageTruncated.
// - properties: 인자마다 하나. 인자 수보다 작으면 앞쪽만 채우고 MessageTruncated.
struct LogRenderTarget {
    Span<char> message;
    Span<char> propertyText;
    Span<LogProperty> properties;
};

namespace detail {

// 캡처 머리(고정 크기)다. 오프셋은 캡처 시작 기준이다. 공개 API가 아니다.
struct LogCaptureValue {
    uint8 kind = 0;  // FormatArgKind. Custom은 캡처 때 렌더한 텍스트다.
    uint8 flags = 0; // LogCaptureValueFlags
    uint16 reserved = 0;
    uint32 bytes = 0;   // 텍스트 byte 수
    uint64 payload = 0; // 원시 값, 텍스트 오프셋 또는 블록 index
};

struct LogCaptureHeader {
    uint32 magic = 0;
    uint32 totalBytes = 0;
    uint16 valueCount = 0;
    uint8 level = 0;
    uint8 flags = 0;
    uint32 templateBytes = 0;
    const LogCategory* category = nullptr;
    const char* literalTemplate = nullptr;
    uint64 eventId = 0;
    SourceLocation location;
    uint64 monotonicNanoseconds = 0;
    uint64 threadId = 0;
    uint64 frame = 0;
    LogCaptureValue runtimeTemplate;
};

// 캡처에 넘길 템플릿 정보다. runtime이면 text를 캡처에 복사한다.
struct LogCaptureTemplate {
    StringView text;
    uint64 eventId = 0;
    SourceLocation location;
    bool runtime = false;
};

// 타입을 지운 인자 배열로 캡처한다. 공개 진입점은 CaptureLog다.
Result<usize> WriteLogCapture(const LogCaptureTarget& target, LogLevel level,
                              const LogCategory& category, const LogCaptureTemplate& text,
                              const FormatArg* args, usize count) noexcept;

// 로그 인자 하나를 지운다. Formatter가 없는 enum은 바탕 정수로 담는다.
template <typename T>
FormatArg MakeLogArg(const T& value) noexcept {
    using U = RemoveCvRefT<T>;
    if constexpr (!HasFormatter<U> && IsEnumV<U>) {
        using Underlying = UnderlyingTypeT<U>;
        FormatArg arg;
        if constexpr (IsSignedV<Underlying>) {
            arg.kind = FormatArgKind::Signed;
            arg.signedValue = static_cast<int64>(static_cast<Underlying>(value));
        } else {
            arg.kind = FormatArgKind::Unsigned;
            arg.unsignedValue = static_cast<uint64>(static_cast<Underlying>(value));
        }

        return arg;
    } else {
        return MakeFormatArg(value);
    }
}

template <typename... Args>
Result<usize> CaptureWithArgs(const LogCaptureTarget& target, LogLevel level,
                              const LogCategory& category, const LogCaptureTemplate& text,
                              const Args&... args) noexcept {
    static_assert(sizeof...(Args) <= MaxLogArguments,
                  "A log call takes at most MaxLogArguments arguments");
    if constexpr (sizeof...(Args) == 0) {
        return WriteLogCapture(target, level, category, text, nullptr, 0);
    } else {
        const FormatArg list[] = {MakeLogArg(args)...};
        return WriteLogCapture(target, level, category, text, list, sizeof...(Args));
    }
}

} // namespace detail

// 인자 count개의 캡처에 필요한 최소 byte 수다(머리와 값 칸). 문자열·Formatter 결과·런타임 템플릿은
// 남는 영역이나 블록에 담는다.
constexpr usize LogCaptureMinimumBytes(usize count) noexcept {
    return sizeof(detail::LogCaptureHeader) + count * sizeof(detail::LogCaptureValue);
}

// 리터럴 템플릿 기록 한 건을 target.buffer에 캡처하고 쓴 byte 수를 돌려준다.
// - category: 캡처는 포인터만 담는다. 정적 저장소(보통 모듈 헤더의 inline constexpr)여야 하며, 함수
//   지역 constexpr 카테고리는 렌더 때 사라져 있다.
// - 실패: buffer가 LogCaptureMinimumBytes(인자 수)보다 작으면 CapacityExceeded(아무것도 담지
// 않는다).
// - 넘치는 문자열 값은 블록을 빌리거나 잘린다(실패가 아니다). 사용자 Formatter의 오류는 그 자리만
//   "<format error>"로 렌더된다(실패가 아니다).
// - 빌린 블록은 이 캡처를 렌더한 뒤, 또는 캡처를 버릴 때 ReleaseLogCaptureBlocks로 돌려준다.
//
// 사용 예:
//   uint8 slot[512];
//   auto used = CaptureLog({Span<uint8>(slot)}, LogLevel::Info, LogEngine,
//                          "Loaded {Count} assets", count);
template <typename... Args>
Result<usize> CaptureLog(const LogCaptureTarget& target, LogLevel level,
                         const LogCategory& category, LogTemplate<TypeIdentityT<Args>...> text,
                         const Args&... args) noexcept {
    const detail::LogCaptureTemplate captured{text.Text(), text.EventId(), text.Location(), false};
    return detail::CaptureWithArgs(target, level, category, captured, args...);
}

// 런타임 템플릿 기록 한 건을 캡처한다. 원문을 캡처에 복사한다. 템플릿 검증은 렌더 때 한다.
// 오류는 리터럴 템플릿과 같다.
template <typename... Args>
Result<usize> CaptureLog(const LogCaptureTarget& target, LogLevel level,
                         const LogCategory& category, const RuntimeTemplate& text,
                         const Args&... args) noexcept {
    const detail::LogCaptureTemplate captured{text.Text(), text.EventId(), text.Location(), true};
    return detail::CaptureWithArgs(target, level, category, captured, args...);
}

// 캡처 한 건을 렌더하고 event를 채운다(level·category·messageTemplate·eventId·message·properties·
// location·threadId·frame·monotonicNanoseconds·flags. context·threadName·utcTicks는 건드리지
// 않는다).
// - 런타임 템플릿 오류: message = 원본 템플릿 + " [template error: <code>]", TemplateError.
// - 자리 하나의 서식 실패(Formatter 오류, 잘못된 UTF-8 문자열, null 문자열, 비ASCII char의 'c'):
//   그 자리만 "<format error>", FormatError.
// - 실패: capture가 CaptureLog가 만든 형식이 아니거나(손상), 블록을 찾을 수 없거나,
// target.message가
//   비었으면 InvalidArgument(event는 바뀌었을 수 있다).
//
// 수명: event의 view는 capture·블록·target 영역을 가리킨다. sink가 event를 다 쓸 때까지 그 영역을
// 바꾸거나 블록을 돌려주지 않는다. 힙 할당은 없다.
Result<void> RenderLogCapture(Span<const uint8> capture, const LogBlockProvider* blocks,
                              const LogRenderTarget& target, LogEvent& event) noexcept;

// 캡처가 빌린 블록을 모두 돌려준다. 렌더·sink가 끝난 뒤나, 캡처를 버릴 때(큐가 가득 참 등) 한 번
// 부른다. 손상된 캡처는 아무것도 하지 않는다.
void ReleaseLogCaptureBlocks(Span<const uint8> capture, LogBlockProvider& blocks) noexcept;

} // namespace rocks
