#pragma once

#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Text/String.h>
#include <Core/Text/StringView.h>

namespace rocks {

class StringBuilderBase;

// 사용자 타입 서식 확장점이다. 특수화가 다음 정적 함수를 제공하면 "{}"·"{:spec}"에 쓸 수 있다.
//   static Result<void> Format(StringBuilderBase& out, const T& value, StringView spec) noexcept;
// spec은 ':' 뒤부터 '}' 앞까지의 원문이다(없으면 빈 view). 폭·정렬은 특수화가 직접 처리한다.
// 실패를 반환하면 그 서식 호출 전체가 실패하고 builder 내용은 호출 전으로 돌아간다.
template <typename T>
struct Formatter;

namespace detail {

// 서식 인자 하나를 타입 정보와 함께 지운 값이다. 문자열·사용자 타입은 원본을 빌리므로 서식 호출
// 동안만 유효하다.
struct FormatArg {
    enum class Kind : uint8 {
        Bool,
        Char,
        Signed,
        Unsigned,
        Float,
        Double,
        Text,
        NullText,
        Pointer,
        Custom
    };
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

template <typename T>
using FormatDecay = RemoveConstT<RemoveReferenceT<T>>;

template <typename T>
FormatArg MakeFormatArg(const T& value) noexcept {
    using U = FormatDecay<T>;
    FormatArg arg;
    if constexpr (HasFormatter<U>) {
        arg.kind = FormatArg::Kind::Custom;
        arg.custom.object = &value;
        arg.custom.format = [](StringBuilderBase& out, const void* object,
                               StringView spec) noexcept -> Result<void> {
            return Formatter<U>::Format(out, *static_cast<const U*>(object), spec);
        };
    } else if constexpr (IsSameV<U, bool>) {
        arg.kind = FormatArg::Kind::Bool;
        arg.boolean = value;
    } else if constexpr (IsSameV<U, char>) {
        arg.kind = FormatArg::Kind::Char;
        arg.character = value;
    } else if constexpr (__is_integral(U) && __is_signed(U)) {
        arg.kind = FormatArg::Kind::Signed;
        arg.signedValue = static_cast<int64>(value);
    } else if constexpr (__is_integral(U) && !IsSameV<U, wchar_t> && !IsSameV<U, char8_t> &&
                         !IsSameV<U, char16_t> && !IsSameV<U, char32_t>) {
        arg.kind = FormatArg::Kind::Unsigned;
        arg.unsignedValue = static_cast<uint64>(value);
    } else if constexpr (IsSameV<U, float>) {
        arg.kind = FormatArg::Kind::Float;
        arg.single = value;
    } else if constexpr (IsSameV<U, double>) {
        arg.kind = FormatArg::Kind::Double;
        arg.real = value;
    } else if constexpr (IsSameV<U, StringView>) {
        arg.kind = FormatArg::Kind::Text;
        arg.text.data = value.Data();
        arg.text.bytes = value.ByteCount();
    } else if constexpr (IsSameV<U, String>) {
        arg.kind = FormatArg::Kind::Text;
        arg.text.data = value.Data();
        arg.text.bytes = value.ByteCount();
    } else if constexpr (IsSameV<U, const char*> || IsSameV<U, char*>) {
        if (value == nullptr) {
            arg.kind = FormatArg::Kind::NullText;
        } else {
            const StringView view(value);
            arg.kind = FormatArg::Kind::Text;
            arg.text.data = view.Data();
            arg.text.bytes = view.ByteCount();
        }
    } else if constexpr (IsSameV<U, decltype(nullptr)>) {
        arg.kind = FormatArg::Kind::Pointer;
        arg.pointer = nullptr;
    } else if constexpr (__is_pointer(U)) {
        arg.kind = FormatArg::Kind::Pointer;
        arg.pointer = static_cast<const volatile void*>(value) == nullptr
                          ? nullptr
                          : const_cast<const void*>(static_cast<const volatile void*>(value));
    } else {
        static_assert(sizeof(U) == 0, "Type is not formattable; specialize rocks::Formatter<T>");
    }
    return arg;
}

// 배열 문자열 상수("abc")는 첫 NUL까지의 문자열로 다룬다.
template <usize N>
FormatArg MakeFormatArg(const char (&value)[N]) noexcept {
    const StringView view(value);
    FormatArg arg;
    arg.kind = FormatArg::Kind::Text;
    arg.text.data = view.Data();
    arg.text.bytes = view.ByteCount();
    return arg;
}

Result<void> FormatTo(StringBuilderBase& out, StringView format,
                      Span<const FormatArg> args) noexcept;

} // namespace detail

// UTF-8 문자열을 이어 붙이는 builder다(UE5.6 TStringBuilderBase 대응). 내용은 항상 유효한 UTF-8이며
// 끝에 NUL을 유지한다. 처음에는 파생 StringBuilder<N>의 객체 안 buffer를 쓰고 넘치면 allocator에서
// 할당한다. 복사·이동하지 않는다. 동시 접근의 동기화는 caller 책임이다.
// - Try* 함수는 실패를 바로 돌려주며 실패한 호출은 내용을 바꾸지 않는다.
// - Append·operator<<는 연쇄용이다. 첫 실패를 기억하고(Status) 그 뒤 연쇄 호출은 아무것도 하지
//   않는다. 연쇄가 끝나면 Status()를 확인한다. Clear가 기억한 실패를 지운다.
// - View()·CString()은 다음 변경 전까지 유효하다(성장하면 주소가 바뀐다).
class StringBuilderBase {
public:
    StringBuilderBase(const StringBuilderBase&) = delete;
    StringBuilderBase& operator=(const StringBuilderBase&) = delete;

    usize ByteCount() const noexcept {
        return size_;
    }

    // NUL을 뺀 payload 용량이다.
    usize Capacity() const noexcept {
        return capacity_;
    }

    bool IsEmpty() const noexcept {
        return size_ == 0;
    }

    StringView View() const noexcept {
        return StringView(data_, size_);
    }

    // NUL로 끝나는 내용이다. 내용 중간에 NUL이 있을 수 있다(embedded NUL).
    const char* CString() const noexcept {
        return data_;
    }

    Allocator& GetAllocator() const noexcept {
        return *allocator_;
    }

    // 연쇄 호출에서 기억한 첫 실패다. 없으면 성공이다.
    Result<void> Status() const noexcept {
        return error_ == ErrorCode::None ? Result<void>::Success() : Result<void>::Failure(error_);
    }

    bool HasFailed() const noexcept {
        return error_ != ErrorCode::None;
    }

    // 내용과 기억한 실패를 지운다. buffer는 재사용한다.
    void Clear() noexcept;

    // payload 용량을 확보한다. 실패하면 상태를 보존한다.
    Result<void> TryReserve(usize byteCapacity) noexcept;

    // text는 유효한 UTF-8이어야 한다(아니면 InvalidArgument). 이 builder의 View()여도 된다.
    Result<void> TryAppend(StringView text) noexcept;
    // ASCII 문자 하나(0x80 미만)를 붙인다. 그 밖은 InvalidArgument다.
    Result<void> TryAppendAscii(char character) noexcept;
    // Unicode scalar value 하나를 UTF-8로 붙인다. surrogate·0x10FFFF 초과는 InvalidArgument다.
    Result<void> TryAppendCodePoint(uint32 codePoint) noexcept;
    // ASCII 문자를 count번 붙인다.
    Result<void> TryAppendRepeated(char character, usize count) noexcept;
    // parts를 separator로 이어 붙인다(UE FString::Join). 실패하면 내용을 보존한다.
    Result<void> TryAppendJoin(Span<const StringView> parts, StringView separator) noexcept;

    // "{}"·"{0}"·"{:spec}" 서식으로 붙인다. 문법은 Docs/Implementation/CoreUtilities.ko.md의 서식
    // 절을 따른다. 서식 오류·인자 불일치·OOM이면 내용을 호출 전으로 되돌리고 오류를 반환한다.
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

    // 내용을 allocator의 String으로 복제한다. builder는 바뀌지 않는다.
    Result<String> TryToString(Allocator& allocator) const noexcept;

    // ---- 연쇄 호출(실패를 기억) ----
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

    // 값 하나를 "{}"와 같은 기본 서식으로 붙인다.
    template <typename T>
    StringBuilderBase& operator<<(const T& value) noexcept {
        return AppendFormat("{}", value);
    }

protected:
    StringBuilderBase(Allocator& allocator, char* inlineBuffer, usize inlineCapacity) noexcept;
    ~StringBuilderBase() noexcept;

private:
    friend Result<void> detail::FormatTo(StringBuilderBase&, StringView,
                                         Span<const detail::FormatArg>) noexcept;

    // 검증 없이 byte를 붙인다(호출자가 UTF-8 유효성을 보장한다).
    Result<void> AppendRaw(const char* bytes, usize count) noexcept;
    Result<void> Grow(usize required) noexcept;
    // 실패한 서식 호출을 되돌린다. size 이전 내용은 그대로다.
    void Truncate(usize size) noexcept;

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
};

namespace detail {
template <usize Bytes>
struct StringBuilderInlineStorage {
    char bytes[Bytes];
};
} // namespace detail

// 객체 안 InlineBytes byte(NUL 포함)를 먼저 쓰는 builder다(UE TStringBuilder<N> 대응).
// 사용 예: StringBuilder<> text(allocator); text << "frame " << index; text.Status();
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

// format을 서식화한 새 String을 만든다. 오류는 TryAppendFormat과 같다.
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
