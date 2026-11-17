#pragma once

#include <Base/TypeTraits.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Text/FormatParse.h>
#include <Core/Text/String.h>
#include <Core/Text/StringView.h>

// 문자열을 조각조각 이어 붙이는 builder와 타입 안전 서식("{}")이다.
//
// 언제 쓰나:
// - 로그 메시지·파일 경로·디버그 문자열처럼 여러 값을 이어 문자열 하나를 만들 때.
// - printf 계열 대신 타입이 맞지 않으면 컴파일 오류가 나는 서식이 필요할 때.
//
// 사용 예:
//   StringBuilder<> text(allocator);                    // 처음 128 byte는 객체 안 buffer를 쓴다
//   text << "frame " << frameIndex << " took " << milliseconds << "ms";
//   if (!text.Status()) { /* 메모리 부족 등 첫 실패 */ }
//   log.Write(text.View());
//
//   auto line = TryFormat(allocator, "{:>8} | {:.3f} | {:#x}", name, seconds, flags);
//   if (line) { Use(line.Value().View()); }
//
// 서식 문법("{" [인자 번호] [":" spec] "}", C++20 표준 format의 부분 집합):
// - "{}"는 다음 인자, "{0}"·"{1}"은 번호로 고른 인자다. 한 문자열에서 둘을 섞으면 오류다.
// - 이름 자리 "{Name}"은 로그 메시지 템플릿(LogTemplate) 전용이다. 여기서는 오류다.
// - "{{"·"}}"는 중괄호 문자 자체다.
// - spec은 [[채움]정렬][부호][#][0][폭][.정밀도][형식]이다.
//   - 정렬 '<' 왼쪽, '>' 오른쪽, '^' 가운데. 채움은 ASCII 문자 하나(기본 공백).
//   - 부호 '+'는 양수에도 '+', ' '는 양수 앞에 공백, '-'는 기본(음수만 '-').
//   - '#'는 접두사(0x·0X·0b·0B·0). '0'은 부호·접두사 뒤를 0으로 채운다(정렬을 지정하면 무시).
//   - 폭·정밀도는 0–65535다. 문자열의 폭·정밀도는 byte가 아니라 문자(code point) 수다.
//   - 형식: 정수 d x X b B o, 실수 f F e E g G, 문자열 s, 문자 c, 포인터 p.
// - 실수의 기본("{}")은 다시 읽으면 같은 값이 되는 가장 짧은 표기다. 첫 유효 숫자의 10진 지수가
//   -4 이상 16 미만이면 고정 소수점, 아니면 과학 표기다(Python repr과 같은 규칙. C++ 표준 format은
//   둘 중 짧은 쪽을 고르므로 0.0001·1e6 등에서 표기가 다르다).
// - 실수 출력은 C runtime·locale·부동소수점 환경을 쓰지 않는다(정수 연산만 쓰는 변환, 소수점은 항상
//   '.'). 정밀도를 지정한 반올림(예: "{:.0f}"의 2.5 → "2")은 정확한 2진 값에서 가장 가까운 값이며
//   동점은 짝수다(glibc·최신 UCRT printf와 같다). 모든 플랫폼에서 결과가 같다.
//
// 지원 타입: bool, char, 정수(8–64 bit), float, double, const char*·char 배열(첫 NUL 또는 배열
// 끝까지), StringView, String, void 포인터·nullptr, 그리고 Formatter<T>를 특수화한 사용자 타입.
// 그 밖의 포인터(const unsigned char* 등)는 주소를 출력하려면 const void*로 바꿔 넘긴다.
namespace rocks {

class StringBuilderBase;

// 사용자 타입 서식 확장점이다. 다음 정적 함수를 제공하도록 특수화하면 "{}"·"{:spec}"에 쓸 수 있다.
//
//   template <> struct Formatter<Vector3> {
//       static Result<void> Format(StringBuilderBase& out, const Vector3& value,
//                                  StringView spec) noexcept {
//           if (!spec.IsEmpty()) return Result<void>::Failure(ErrorCode::InvalidArgument);
//           return out.TryAppendFormat("({}, {}, {})", value.x, value.y, value.z);
//       }
//   };
//
// - spec은 ':' 뒤부터 '}' 앞까지의 원문이다(없으면 빈 view). 폭·정렬이 필요하면 특수화가 직접
//   해석한다.
// - out에 붙이기만 한다. out의 기존 내용을 읽거나 지우지 않는다.
// - 실패를 반환하면 그 서식 호출 전체가 실패하고 builder 내용은 호출 전으로 돌아간다(로그 렌더는
//   예외로 그 자리만 "<format error>"로 바꾼다).
// - out은 FixedStringBuilder(잘림 모드)일 수 있다. 그때 넘치는 출력은 잘리고 성공한다.
template <typename T>
struct Formatter;

namespace detail {

// 서식 인자 하나를 타입 정보와 함께 지운 값이다. 문자열·사용자 타입은 원본을 빌리므로 서식 호출
// 동안만 유효하다. 사용자 코드는 직접 만들지 않는다.
struct FormatArg {
    using Kind = FormatArgKind;

    using CustomFunction = Result<void> (*)(StringBuilderBase&, const void*, StringView) noexcept;

    struct TextValue {
        const char* data;
        usize bytes;
    };

    struct CustomValue {
        const void* object;
        CustomFunction format;
    };

    Kind kind = Kind::Bool;

    union {
        bool boolean;
        char character;
        int64 signedValue;
        uint64 unsignedValue;
        float single;
        double real;
        const void* pointer;
        TextValue text;
        CustomValue custom;
    };
};

template <typename T>
concept HasFormatter = requires(StringBuilderBase& out, const T& value, StringView spec) {
    { Formatter<T>::Format(out, value, spec) } noexcept;
};

// const·volatile·참조를 벗긴 인자 타입이다.
template <typename T>
using FormatDecay = RemoveCvRefT<T>;

template <typename T>
inline constexpr bool IsCharacterTextPointer = IsSameV<T, const char*> || IsSameV<T, char*>;

template <typename T>
inline constexpr bool IsVoidPointer = IsPointerV<T> && IsSameV<RemoveCvT<RemovePointerT<T>>, void>;

template <typename T>
inline constexpr bool IsCharacterArray = IsArrayV<T> && IsSameV<RemoveCvT<RemoveExtentT<T>>, char>;

// 인자 타입 U(const·참조를 벗긴 타입)의 서식 종류다. 서식할 수 없는 타입은 static_assert로 막는다.
// 런타임 값으로 갈리는 것(null C 문자열 → NullText)은 Text로 본다. MakeFormatArg와 로그 템플릿의
// 컴파일 타임 spec 검증이 같은 분류를 쓴다.
template <typename U>
consteval FormatArgKind FormatArgKindOf() noexcept {
    if constexpr (HasFormatter<U>) {
        return FormatArgKind::Custom;
    } else if constexpr (IsSameV<U, bool>) {
        return FormatArgKind::Bool;
    } else if constexpr (IsSameV<U, char>) {
        return FormatArgKind::Char;
    } else if constexpr (IsSameV<U, wchar_t> || IsSameV<U, char8_t> || IsSameV<U, char16_t> ||
                         IsSameV<U, char32_t>) {
        static_assert(sizeof(U) == 0, "wide/UTF character types are not formattable");
        return FormatArgKind::Custom;
    } else if constexpr (IsIntegralV<U>) {
        static_assert(sizeof(U) <= 8, "integers wider than 64 bits are not formattable");
        return IsSignedV<U> ? FormatArgKind::Signed : FormatArgKind::Unsigned;
    } else if constexpr (IsSameV<U, float>) {
        return FormatArgKind::Float;
    } else if constexpr (IsSameV<U, double>) {
        return FormatArgKind::Double;
    } else if constexpr (IsSameV<U, StringView> || IsSameV<U, String> ||
                         IsCharacterTextPointer<U> || IsCharacterArray<U>) {
        return FormatArgKind::Text;
    } else if constexpr (IsSameV<U, decltype(nullptr)> || IsVoidPointer<U>) {
        return FormatArgKind::Pointer;
    } else if constexpr (IsPointerV<U>) {
        static_assert(sizeof(U) == 0, "Only void pointers are formattable; cast to const void*");
        return FormatArgKind::Custom;
    } else {
        static_assert(sizeof(U) == 0, "Type is not formattable; specialize rocks::Formatter<T>");
        return FormatArgKind::Custom;
    }
}

template <typename T>
FormatArg MakeFormatArg(const T& value) noexcept {
    using U = FormatDecay<T>;
    static_assert(!IsVolatileV<RemoveReferenceT<T>>,
                  "volatile values are not formattable; copy the value first");
    constexpr FormatArgKind kind = FormatArgKindOf<U>();
    FormatArg arg;
    arg.kind = kind;
    if constexpr (kind == FormatArgKind::Custom) {
        arg.custom.object = &value;
        arg.custom.format = [](StringBuilderBase& out, const void* object,
                               StringView spec) noexcept -> Result<void> {
            return Formatter<U>::Format(out, *static_cast<const U*>(object), spec);
        };
    } else if constexpr (kind == FormatArgKind::Bool) {
        arg.boolean = value;
    } else if constexpr (kind == FormatArgKind::Char) {
        arg.character = value;
    } else if constexpr (kind == FormatArgKind::Signed) {
        arg.signedValue = static_cast<int64>(value);
    } else if constexpr (kind == FormatArgKind::Unsigned) {
        arg.unsignedValue = static_cast<uint64>(value);
    } else if constexpr (kind == FormatArgKind::Float) {
        arg.single = value;
    } else if constexpr (kind == FormatArgKind::Double) {
        arg.real = value;
    } else if constexpr (IsCharacterTextPointer<U>) {
        if (value == nullptr) {
            arg.kind = FormatArgKind::NullText;
        } else {
            const StringView view(value);
            arg.text.data = view.Data();
            arg.text.bytes = view.ByteCount();
        }
    } else if constexpr (kind == FormatArgKind::Text) {
        arg.text.data = value.Data();
        arg.text.bytes = value.ByteCount();
    } else if constexpr (IsSameV<U, decltype(nullptr)>) {
        arg.pointer = nullptr;
    } else {
        arg.pointer = const_cast<const void*>(static_cast<const volatile void*>(value));
    }

    return arg;
}

// char 배열은 첫 NUL까지, NUL이 없으면 배열 끝까지의 문자열이다(배열 밖은 읽지 않는다).
template <usize N>
FormatArg MakeFormatArg(const char (&value)[N]) noexcept {
    usize length = 0;
    while (length < N && value[length] != '\0') {
        ++length;
    }

    FormatArg arg;
    arg.kind = FormatArg::Kind::Text;
    arg.text.data = value;
    arg.text.bytes = length;
    return arg;
}

template <usize N>
FormatArg MakeFormatArg(char (&value)[N]) noexcept {
    return MakeFormatArg(static_cast<const char (&)[N]>(value));
}

Result<void> FormatTo(StringBuilderBase& out, StringView format,
                      Span<const FormatArg> args) noexcept;

// 인자 하나를 spec(원문)으로 서식화해 out에 붙인다. 로그 렌더처럼 자리마다 따로 서식화하는 곳이
// 쓴다. hasSpec이 거짓이면 기본 서식("{}")이다. 실패하면 out에 일부가 남을 수 있으며 되돌리는 것은
// 호출자다. 오류는 TryAppendFormat의 자리 하나와 같다.
Result<void> AppendFormatArgument(StringBuilderBase& out, const FormatArg& arg, StringView spec,
                                  bool hasSpec) noexcept;

// FixedStringBuilder가 쓰는, 모든 요청을 거절하는 allocator다. 거절한 호출 수를 호출 스레드별로
// 센다(무할당 시험이 0인지 확인한다). 여러 스레드에서 불러도 된다.
class RefusingAllocator final : public Allocator {
public:
    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override;
    Result<void> Deallocate(void* address) noexcept override;
};

// 호출 스레드에서 FixedStringBuilder의 allocator가 거절한 호출 수다(스레드별이라 다른 시험의 할당이
// 섞이지 않는다). 시험이 할당 시도가 없었음을 확인하는 데 쓴다.
uint64 FixedStringBuilderAllocationAttempts() noexcept;

// 모든 FixedStringBuilder가 함께 쓰는 거절 allocator다(상태 없는 정적 객체, 프로그램 수명 동안
// 유효). FixedStringBuilder의 생성자만 쓴다. 할당을 시도하는 경로가 생기면 그 시도는 실패하고 위
// 카운터에 남는다.
RefusingAllocator& FixedStringBuilderAllocator() noexcept;

} // namespace detail

// UTF-8 문자열을 이어 붙이는 builder의 공통 부분이다(UE5.8 TStringBuilderBase 대응). 직접 만들지
// 않고 StringBuilder<N>을 만든 뒤, 받는 함수는 StringBuilderBase&로 받는다.
//
// 보장:
// - 내용은 항상 유효한 UTF-8이고 끝에 NUL이 있다(CString()을 C API에 넘길 수 있다).
// - Try* 함수는 실패하면 내용·용량을 바꾸지 않고 오류를 돌려준다.
// - Append·AppendFormat·operator<<는 연쇄 호출용이다. 첫 실패를 기억하고(Status) 그 뒤 연쇄 호출은
//   아무것도 하지 않는다. 연쇄가 끝나면 Status()를 확인한다. Clear가 기억한 실패를 지운다.
//
// 주의:
// - View()·CString()은 다음 변경 전까지만 유효하다(용량이 늘면 주소가 바뀐다).
// - 복사·이동하지 않는다. 동시 접근의 동기화는 caller 책임이다.
// - 객체 안 buffer를 넘으면 allocator에서 할당하고, 늘릴 때 새 저장소를 받은 뒤 옛 저장소를
//   반환한다. StackAllocator처럼 역순 반환만 받는 allocator에서는 TryReserve로 필요한 용량을 먼저
//   확보한다(Array와 같은 제약). 서식 인자가 이 builder의 내용을 가리키면 별도 임시 builder를
//   쓰므로 그 경우에도 같은 제약이 있다.
class StringBuilderBase {
public:
    StringBuilderBase(const StringBuilderBase&) = delete;
    StringBuilderBase& operator=(const StringBuilderBase&) = delete;

    // 내용의 byte 수다(끝의 NUL 제외).
    usize ByteCount() const noexcept {
        return size_;
    }

    // 다시 할당하지 않고 담을 수 있는 byte 수다(NUL 제외).
    usize Capacity() const noexcept {
        return capacity_;
    }

    bool IsEmpty() const noexcept {
        return size_ == 0;
    }

    // 현재 내용이다. 다음 변경 전까지만 유효하다.
    StringView View() const noexcept {
        return StringView(data_, size_);
    }

    // NUL로 끝나는 현재 내용이다. 내용 중간에 NUL이 있을 수 있다(embedded NUL).
    const char* CString() const noexcept {
        return data_;
    }

    Allocator& GetAllocator() const noexcept {
        return *allocator_;
    }

    // 잘림 모드(FixedStringBuilder)에서 넘친 출력을 잘라 버린 적이 있는지다. Clear가 지운다. 잘림
    // 모드가 아니면 늘 거짓이다.
    bool Truncated() const noexcept {
        return truncated_;
    }

    // 연쇄 호출(Append·AppendFormat·operator<<)에서 기억한 첫 실패다. 없으면 성공이다.
    Result<void> Status() const noexcept {
        return error_ == ErrorCode::None ? Result<void>::Success() : Result<void>::Failure(error_);
    }

    bool HasFailed() const noexcept {
        return error_ != ErrorCode::None;
    }

    // 내용과 기억한 실패·잘림 표시를 지운다. 할당받은 buffer는 다음 사용을 위해 남긴다.
    void Clear() noexcept;

    // byteCapacity byte까지 다시 할당하지 않도록 용량을 확보한다.
    // 실패: Overflow(크기 표현 불가), allocator 오류(OutOfMemory 등). 실패하면 상태를 보존한다.
    Result<void> TryReserve(usize byteCapacity) noexcept;

    // text를 붙인다. text는 이 builder의 View()여도 된다.
    // 실패: 잘못된 UTF-8은 InvalidArgument, Overflow, allocator 오류. 실패하면 내용을 보존한다.
    Result<void> TryAppend(StringView text) noexcept;

    // ASCII 문자 하나(0x00–0x7F)를 붙인다. 그 밖(UTF-8 byte 하나)은 InvalidArgument다.
    Result<void> TryAppendAscii(char character) noexcept;

    // Unicode 문자(scalar value) 하나를 UTF-8로 붙인다. 예: TryAppendCodePoint(0x20AC)는 "€".
    // surrogate(0xD800–0xDFFF)·0x10FFFF 초과는 InvalidArgument다.
    Result<void> TryAppendCodePoint(uint32 codePoint) noexcept;

    // ASCII 문자를 count번 붙인다(구분선·들여쓰기용). 비ASCII는 InvalidArgument다.
    Result<void> TryAppendRepeated(char character, usize count) noexcept;

    // parts를 separator로 이어 붙인다(UE FString::Join 대응). 예: {"a","b","c"}와 ", " → "a, b, c".
    // parts·separator가 이 builder의 내용을 가리켜도 된다. 실패하면 내용을 보존한다.
    Result<void> TryAppendJoin(Span<const StringView> parts, StringView separator) noexcept;

    // format을 서식화해 붙인다(문법은 이 파일 첫머리 참고).
    // 실패: 서식 오류·인자 번호 초과·형식과 타입 불일치·잘못된 UTF-8은 InvalidArgument, 사용자
    // Formatter의 오류는 그대로, 그 밖에 Overflow·allocator 오류. 실패하면 내용을 호출 전으로
    // 되돌린다.
    template <typename... Args>
    Result<void> TryAppendFormat(StringView format, const Args&... args) noexcept {
        if constexpr (sizeof...(Args) == 0) {
            return detail::FormatTo(*this, format, Span<const detail::FormatArg>());
        } else {
            const detail::FormatArg list[] = {detail::MakeFormatArg(args)...};
            return detail::FormatTo(*this, format,
                                    Span<const detail::FormatArg>(list, sizeof...(Args)));
        }
    }

    // 내용을 allocator의 새 String으로 복사한다. builder는 바뀌지 않는다.
    Result<String> TryToString(Allocator& allocator) const noexcept;

    // ---- 연쇄 호출(첫 실패를 기억) ----

    StringBuilderBase& Append(StringView text) noexcept {
        if (!HasFailed()) {
            Remember(TryAppend(text));
        }

        return *this;
    }

    template <typename... Args>
    StringBuilderBase& AppendFormat(StringView format, const Args&... args) noexcept {
        if (!HasFailed()) {
            Remember(TryAppendFormat(format, args...));
        }

        return *this;
    }

    // 값 하나를 "{}"와 같은 기본 서식으로 붙인다. 예: text << "x=" << 1.5 << ' ' << true;
    template <typename T>
    StringBuilderBase& operator<<(const T& value) noexcept {
        return AppendFormat("{}", value);
    }

protected:
    // truncating이 참이면 잘림 모드다. 용량을 넘는 출력은 UTF-8 문자 경계에서 잘라 버리고 성공하며,
    // 한 번 잘린 뒤의 출력은 모두 버린다(뒤 조각만 붙어 앞뒤가 어긋난 내용이 되지 않게 한다).
    StringBuilderBase(Allocator& allocator, char* inlineBuffer, usize inlineCapacity,
                      bool truncating = false) noexcept;
    ~StringBuilderBase() noexcept;

    // 내용을 byteCount까지 줄이고 잘림 표시를 truncated로 되돌린다. byteCount는 현재 길이 이하이고
    // UTF-8 문자 경계여야 한다(앞서 ByteCount()로 얻은 값).
    void RewindTo(usize byteCount, bool truncated) noexcept {
        Truncate(byteCount);
        truncated_ = truncated;
    }

private:
    friend Result<void> detail::FormatTo(StringBuilderBase&, StringView,
                                         Span<const detail::FormatArg>) noexcept;

    // 검증 없이 byte를 붙인다. 호출자가 UTF-8 유효성을 보장한다.
    Result<void> AppendRaw(const char* bytes, usize count) noexcept;

    Result<void> Grow(usize required) noexcept;

    // size 뒤를 잘라 실패한 서식 호출을 되돌린다.
    void Truncate(usize size) noexcept;

    // 잘림 모드에서 count byte 중 남은 용량에 들어가는 앞부분(UTF-8 문자 경계)을 붙이고 잘림을
    // 표시한다.
    void AppendTruncated(const char* bytes, usize count) noexcept;

    // [address, address+bytes)가 현재 저장소와 겹치는지다.
    bool Overlaps(const char* address, usize bytes) const noexcept;

    void Remember(const Result<void>& result) noexcept {
        if (!result && error_ == ErrorCode::None) {
            error_ = result.Error();
        }
    }

    Allocator* allocator_;
    char* inline_;
    char* data_;
    usize size_ = 0;
    usize capacity_;
    ErrorCode error_ = ErrorCode::None;
    bool truncating_ = false;
    bool truncated_ = false;
};

namespace detail {

template <usize Bytes>
struct StringBuilderInlineStorage {
    char bytes[Bytes];
};

} // namespace detail

// 객체 안 InlineBytes byte(NUL 포함)를 먼저 쓰는 builder다(UE TStringBuilder<N> 대응). 짧은
// 문자열은 할당 없이 만든다. 스택에 두면 그만큼 스택을 쓴다(기본 128 byte).
//
//   StringBuilder<256> path(allocator);
//   path << root << '/' << fileName;
template <usize InlineBytes = 128>
class StringBuilder final : private detail::StringBuilderInlineStorage<InlineBytes>,
                            public StringBuilderBase {
    static_assert(InlineBytes >= 1);

public:
    explicit StringBuilder(Allocator& allocator) noexcept
        : detail::StringBuilderInlineStorage<InlineBytes>(),
          StringBuilderBase(allocator, this->bytes, InlineBytes - 1) {
    }
};

// 호출자가 준 고정 버퍼에 쓰는 잘림 모드 builder다. 할당하지 않는다(로그 캡처·렌더처럼 힙을 쓰면 안
// 되는 경로용, Docs/Design/LoggingSystem.ko.md 8.3절).
//
// StringBuilder와 다른 점:
// - 용량(buffer 크기 - 1, 끝 NUL 자리)을 넘으면 넘친 부분을 UTF-8 문자 경계에서 잘라 버리고
//   성공한다. Truncated()가 참이 되고, 그 뒤의 붙이기는 모두 버린다(Clear 전까지).
// - 서식 오류·잘못된 UTF-8·사용자 Formatter 오류는 StringBuilder와 같이 실패하고 호출 전으로
//   돌아간다.
// - TryReserve는 할당하지 않고 늘 성공한다(넘치는 내용은 다음 붙이기가 자른다).
// - 서식 인자가 이 builder의 내용을 가리켜도 된다(저장소가 움직이지 않는다).
//
// 수명: buffer는 builder보다 오래 살아야 한다. View()·CString()은 buffer를 가리킨다.
// 동시 접근의 동기화는 호출자 책임이다. buffer가 비어 있으면(NUL 자리도 없으면) 계약 위반으로
// Fatal이다.
//
// 사용 예:
//   char storage[256];
//   FixedStringBuilder text{Span<char>(storage)};
//   (void)text.TryAppendFormat("Loaded {} assets from {}", count, path);
//   if (text.Truncated()) { /* 일부만 담겼다 */ }
class FixedStringBuilder final : public StringBuilderBase {
public:
    explicit FixedStringBuilder(Span<char> buffer) noexcept;

    // 내용을 byteCount까지 되돌리고 잘림 표시를 truncated로 맞춘다. 자리 하나의 서식이 실패했을 때
    // 그 자리 출력만 지우는 데 쓴다. byteCount는 앞서 ByteCount()로 얻은 값이어야 한다.
    void RewindTo(usize byteCount, bool truncated) noexcept {
        StringBuilderBase::RewindTo(byteCount, truncated);
    }
};

// format을 서식화한 새 String을 만든다. 오류는 TryAppendFormat과 같다.
//   auto title = TryFormat(allocator, "{} ({}x{})", name, width, height);
template <typename... Args>
Result<String> TryFormat(Allocator& allocator, StringView format, const Args&... args) noexcept {
    StringBuilder<256> builder(allocator);
    auto formatted = builder.TryAppendFormat(format, args...);
    if (!formatted) {
        return Result<String>::Failure(formatted.Error());
    }

    return builder.TryToString(allocator);
}

} // namespace rocks
