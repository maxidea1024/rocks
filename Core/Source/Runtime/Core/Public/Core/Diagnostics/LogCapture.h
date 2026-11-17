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
// 캡처가 담는 것: 레벨, 카테고리 이름(정적 문자열 포인터·길이)과 해시, 리터럴 템플릿 포인터
// 또는 복사한 런타임 템플릿, EventId, 호출 위치, 단조 시각·스레드·프레임(호출자가 준 값),
// 플래그, 인자 값.
//
// 인자 값을 담는 방법:
// - 정수·실수·bool·char·포인터·enum: 원시 값
// - 문자열: byte 복사(UTF-8 검증은 렌더 때)
// - 사용자 Formatter<T> 타입: 캡처할 때 FixedStringBuilder로 렌더한다.
//   객체 수명이 끝날 수 있기 때문이다. Formatter는 한 번만 부른다.
//   같은 인자를 여러 자리에 쓰면 spec이 모두 같아야 한다(템플릿 검증).
// - 슬롯에 남은 영역에 들어가지 않는 값은 LogBlockProvider의 블록을 빌린다. 블록이 없으면
//   UTF-8 문자 경계에서 잘라 LogEventFlags::ValueTruncated를 세운다.
//
// 힙 할당은 없다. 모든 쓰기는 호출자가 준 버퍼·블록과 FixedStringBuilder로 한다.
namespace rocks {

// 블록 없음 표지다.
inline constexpr uint32 NoLogBlock = NumericLimits<uint32>::Max();

// Formatter를 렌더할 영역의 최소 byte 수다(내용 1 byte + 끝 NUL). 이보다 작은 영역·블록에는
// 렌더하지 않는다.
inline constexpr usize MinimumLogRenderBytes = 2;

// 빌린 블록 하나다. index는 provider가 블록을 다시 찾는 번호, bytes는 쓸 수 있는 영역이다.
struct LogBlock {
    uint32 index = NoLogBlock;
    Span<uint8> bytes;
};

// 큰 값을 담을 블록을 빌려 주는 쪽이다(LogBlockPool이 구현한다). 캡처는 TryBorrow로 빌려 쓰고,
// 렌더는 Resolve로 읽고, 기록을 다 쓴 쪽(출력 스레드, 또는 캡처를 버리는 생산자)이
// ReleaseLogCaptureBlocks로 돌려준다.
//
// 계약(모든 함수는 여러 스레드에서 동시에 불릴 수 있고, 기다리면 안 된다):
// - BlockBytes: 블록 하나의 크기다. 바뀌지 않는다.
// - TryBorrow: 블록이 없으면 false다. 성공하면 Release 전까지 block.bytes는 이 호출자만 쓴다.
// - Resolve: 빌려 준 index의 영역이다. 모르는 index면 빈 Span이다.
// - Release: 빌려 준 index를 돌려받는다. 같은 index를 두 번 돌려주지 않는다.
class LogBlockProvider {
public:
    virtual ~LogBlockProvider() noexcept = default;
    virtual usize BlockBytes() const noexcept = 0;
    virtual bool TryBorrow(LogBlock& block) noexcept = 0;
    virtual Span<const uint8> Resolve(uint32 index) const noexcept = 0;
    virtual void Release(uint32 index) noexcept = 0;
};

// 캡처를 쓸 곳과 머리에 담을 실행 문맥이다.
// - buffer: 기록 한 건을 담을 고정 영역(준비 버퍼·큐 슬롯)
//   최소 크기는 LogCaptureMinimumBytes(인자 수)다.
// - blocks: 큰 값용 블록 provider. 없으면(null) 넘치는 값은 잘린다.
// - monotonicNanoseconds·threadId·frame: 렌더한 LogEvent에 그대로 실린다.
struct LogCaptureTarget {
    Span<uint8> buffer;
    LogBlockProvider* blocks = nullptr;
    uint64 monotonicNanoseconds = 0;
    uint64 threadId = 0;
    uint64 frame = 0;
};

// 렌더 결과를 담을 곳이다. LogEvent의 view가 이 영역과 캡처·블록을 가리킨다.
// - message: 렌더한 메시지(끝 NUL 자리 포함, 1 byte 이상)
// - propertyText: 숫자·char·포인터 속성의 텍스트를 쓰는 영역
// - properties: 인자마다 하나
// 어느 쪽이든 모자라면 잘리고 LogEventFlags::MessageTruncated가 선다.
struct LogRenderTarget {
    Span<char> message;
    Span<char> propertyText;
    Span<LogProperty> properties;
};

namespace detail {

// 캡처의 값 칸 하나다(고정 크기). 오프셋은 캡처 시작 기준이다. 공개 API가 아니다.
struct LogCaptureValue {
    uint8 kind = 0;  // FormatArgKind. Custom은 캡처 때 렌더한 텍스트다.
    uint8 flags = 0; // 블록·잘림·서식 오류 표시
    uint16 reserved = 0;
    uint32 bytes = 0;   // 텍스트 byte 수
    uint64 payload = 0; // 원시 값, 텍스트 오프셋 또는 블록 index
};

// 캡처 머리(고정 크기)다. 카테고리는 객체가 아니라 이름(정적 문자열)을 담는다. 그래서 함수 지역
// constexpr LogCategory로 기록해도 렌더 때 유효하다(설계 8.3절).
struct LogCaptureHeader {
    uint32 magic = 0;
    uint32 totalBytes = 0;
    uint16 valueCount = 0;
    uint8 level = 0;
    uint8 flags = 0;
    uint8 categoryBytes = 0;
    uint8 reserved[3]{};
    uint32 templateBytes = 0;
    const char* categoryName = nullptr;
    uint64 categoryHash = 0;
    const char* literalTemplate = nullptr;
    uint64 eventId = 0;
    SourceLocation location;
    uint64 monotonicNanoseconds = 0;
    uint64 threadId = 0;
    uint64 frame = 0;
    LogCaptureValue runtimeTemplate;
};

static_assert(MaxLogCategoryNameBytes <= NumericLimits<uint8>::Max(),
              "category name length is stored as uint8");

// 캡처에 넘길 템플릿 정보다.
// - runtime이면 text를 캡처에 복사하고, 사용자 Formatter의 spec은 캡처 때 템플릿을 해석해 찾는다.
// - 리터럴이면 specs가 인자마다 미리 계산한 spec 위치다(LogTemplate의 consteval 생성자).
struct LogCaptureTemplate {
    StringView text;
    uint64 eventId = 0;
    SourceLocation location;
    bool runtime = false;
    const LogArgumentSpec* specs = nullptr;
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

// 리터럴 템플릿을 캡처용 정보로 바꾼다. 반환값의 specs는 text를 가리키므로 text보다 오래 쓰지
// 않는다.
template <typename... Args>
LogCaptureTemplate ToCaptureTemplate(const LogTemplate<Args...>& text) noexcept {
    return {text.Text(), text.EventId(), text.Location(), false, text.ArgumentSpecs()};
}

inline LogCaptureTemplate ToCaptureTemplate(const RuntimeTemplate& text) noexcept {
    return {text.Text(), text.EventId(), text.Location(), true, nullptr};
}

} // namespace detail

// 인자 count개의 캡처에 필요한 최소 byte 수다(머리와 값 칸). 문자열·Formatter 결과·런타임 템플릿은
// 남는 영역이나 블록에 담는다.
constexpr usize LogCaptureMinimumBytes(usize count) noexcept {
    return sizeof(detail::LogCaptureHeader) + count * sizeof(detail::LogCaptureValue);
}

// 리터럴 템플릿 기록 한 건을 target.buffer에 캡처하고 쓴 byte 수를 돌려준다.
//
// 결과:
// - buffer가 LogCaptureMinimumBytes(인자 수)보다 작으면 CapacityExceeded다(아무것도 담지 않는다).
// - 넘치는 문자열 값은 블록을 빌리거나 잘린다(실패가 아니다).
// - 사용자 Formatter의 오류는 그 자리만 "<format error>"로 렌더된다(실패가 아니다).
//
// 빌린 블록은 이 캡처를 렌더한 뒤, 또는 캡처를 버릴 때 ReleaseLogCaptureBlocks로 돌려준다.
//
// 사용 예:
//   uint8 slot[512];
//   auto used = CaptureLog({Span<uint8>(slot)}, LogLevel::Info, LogEngine,
//                          "Loaded {Count} assets", count);
template <typename... Args>
Result<usize> CaptureLog(const LogCaptureTarget& target, LogLevel level,
                         const LogCategory& category, LogTemplate<TypeIdentityT<Args>...> text,
                         const Args&... args) noexcept {
    return detail::CaptureWithArgs(target, level, category, detail::ToCaptureTemplate(text),
                                   args...);
}

// 런타임 템플릿 기록 한 건을 캡처한다. 원문을 캡처에 복사한다. 템플릿 검증은 렌더 때 한다.
// 오류는 리터럴 템플릿과 같다.
template <typename... Args>
Result<usize> CaptureLog(const LogCaptureTarget& target, LogLevel level,
                         const LogCategory& category, const RuntimeTemplate& text,
                         const Args&... args) noexcept {
    return detail::CaptureWithArgs(target, level, category, detail::ToCaptureTemplate(text),
                                   args...);
}

// 캡처 한 건을 렌더하고 event를 채운다.
//
// 채우는 칸: level, category, messageTemplate, eventId, message, properties, location, threadId,
// frame, monotonicNanoseconds, flags. context·threadName·utcTicks는 건드리지 않는다.
//
// 결과:
// - 런타임 템플릿 오류: message는 원본 템플릿 + " [template error: <code>]"이고 TemplateError다.
//   메시지 영역이 모자라면 원본을 잘라 오류 표기를 남긴다.
// - 런타임 템플릿이 캡처 때 잘렸으면 해석하지 않고 원본(잘린 것)을 그대로 쓴다(MessageTruncated).
// - 자리 하나의 서식 실패: 그 자리만 "<format error>"이고 FormatError다.
//   (Formatter 오류, 잘못된 UTF-8 문자열, null 문자열, 비ASCII char의 'c')
// - InvalidArgument: target.message가 비었거나 캡처가 손상됐다(event는 바뀌었을 수 있다).
//
// 손상 검사의 한계: 오프셋·길이·값 수·종류·레벨·블록 index처럼 캡처 안의 값만 검사한다. 캡처에 담긴
// 포인터(카테고리 이름·리터럴 템플릿·호출 위치 문자열)는 검사할 수 없으므로, 캡처는 CaptureLog가
// 같은 프로세스에서 만든 것이어야 한다.
//
// 수명: event의 view는 capture·블록·target 영역을 가리킨다. sink가 event를 다 쓸 때까지 그 영역을
// 바꾸거나 블록을 돌려주지 않는다. 힙 할당은 없다.
Result<void> RenderLogCapture(Span<const uint8> capture, const LogBlockProvider* blocks,
                              const LogRenderTarget& target, LogEvent& event) noexcept;

// 캡처가 빌린 블록을 모두 돌려준다. 렌더·sink가 끝난 뒤나, 캡처를 버릴 때(큐가 가득 참 등) 한 번
// 부른다. 손상된 캡처는 아무것도 하지 않는다.
void ReleaseLogCaptureBlocks(Span<const uint8> capture, LogBlockProvider& blocks) noexcept;

} // namespace rocks
