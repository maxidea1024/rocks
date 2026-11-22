#pragma once

#include <Base/Types.h>
#include <Core/Result.h>
#include <Platform/Memory.h>

namespace rocks {

// IgnoreAsciiCase는 A~Z만 a~z와 같게 비교한다. Unicode case folding은 수행하지 않는다.
enum class StringComparison {
    CaseSensitive,
    IgnoreAsciiCase
};
enum class StringSearchDirection {
    FromStart,
    FromEnd
};

class CodePointRange;

// 원본 byte 범위를 빌리는 view다. UTF-8 유효성을 생성 시 자동 검사하지 않으므로
// 외부 입력은 ValidateUtf8로 검증한다. 원본 파괴·재할당·내용 변경은 caller가 통제한다.
// 길이는 code point나 grapheme 수가 아닌 byte 수이며 동등 비교도 정규화 없는 byte 비교다.
class ROCKS_CORE_API StringView {
public:
    constexpr StringView() noexcept = default;

    // C 문자열의 첫 NUL까지 읽는다. nullptr는 빈 view다.
    // embedded NUL을 포함하려면 아래 pointer/byteCount 생성자를 사용한다.
    constexpr StringView(const char* nullTerminated) noexcept
        : data_(nullTerminated),
          bytes_(nullTerminated != nullptr ? platform::CStringLength(nullTerminated) : 0) {
    }

    // caller는 byteCount만큼 읽을 수 있는 storage를 제공한다. nullptr는 길이 0에만 허용한다.
    constexpr StringView(const char* data, usize byteCount) noexcept
        : data_(data), bytes_(byteCount) {
        if (data_ == nullptr && bytes_ != 0) {
            platform::Fatal("A nonempty StringView requires storage");
        }
    }

    constexpr const char* Data() const noexcept {
        return data_;
    }

    constexpr usize ByteCount() const noexcept {
        return bytes_;
    }

    constexpr bool IsEmpty() const noexcept {
        return bytes_ == 0;
    }

    // ---- STL 호환 별칭과 byte 반복자 ----
    // UTF-8 byte 단위의 읽기 전용 contiguous 반복자(const char*)다. 문자(code point) 단위 순회는
    // CodePoints()를 사용한다. 원본 수명 규약은 view와 같다.
    using value_type = char;
    using size_type = usize;
    using difference_type = isize;
    using const_pointer = const char*;
    using const_reference = const char&;
    using const_iterator = const char*;
    using iterator = const char*;

    constexpr const char* begin() const noexcept {
        return data_;
    }

    constexpr const char* end() const noexcept {
        return data_ + bytes_;
    }

    constexpr const char* cbegin() const noexcept {
        return data_;
    }

    constexpr const char* cend() const noexcept {
        return data_ + bytes_;
    }

    // Unicode scalar의 UTF-8만 허용한다. overlong·surrogate·범위 초과·잘린 sequence는
    // InvalidArgument다. NUL은 유효한 U+0000이며 이 검사는 할당이나 정규화를 하지 않는다.
    Result<void> ValidateUtf8() const noexcept;

    // ValidateUtf8와 같은 검사의 constexpr 판정이다(overlong·surrogate·U+10FFFF 초과·잘린
    // sequence는 false). 컴파일 타임 검증(로그 메시지 템플릿 등)과 ValidateUtf8이 같은 구현을 쓴다.
    constexpr bool IsValidUtf8() const noexcept {
        return FindInvalidUtf8() == IndexNone;
    }

    // 처음 잘못된 UTF-8 sequence가 시작하는 byte 위치다. 모두 유효하면 IndexNone이다. 위치는 해독을
    // 시작한 leading byte다(잘못된 continuation byte가 뒤에 있어도 그 sequence의 첫 byte를
    // 가리킨다). 컴파일러·검증기가 "몇 번째 byte가 잘못됐는지"를 사용자에게 보일 때 쓴다(예: Sparks
    // 원문 검사 S0101). 판정 규칙은 ValidateUtf8과 같다.
    constexpr usize FindInvalidUtf8() const noexcept {
        usize offset = 0;
        while (offset < bytes_) {
            const auto leading = static_cast<uint8>(data_[offset]);
            if (leading < 0x80u) {
                ++offset;
                continue;
            }

            usize width = 0;
            uint32 scalar = 0;
            uint32 minimum = 0;
            if ((leading & 0xE0u) == 0xC0u) {
                width = 2;
                scalar = leading & 0x1Fu;
                minimum = 0x80u;
            } else if ((leading & 0xF0u) == 0xE0u) {
                width = 3;
                scalar = leading & 0x0Fu;
                minimum = 0x800u;
            } else if ((leading & 0xF8u) == 0xF0u) {
                width = 4;
                scalar = leading & 0x07u;
                minimum = 0x10000u;
            } else {
                return offset;
            }

            // 남은 길이부터 검사하여 잘린 입력에서 continuation byte를 범위 밖으로 읽지 않는다.
            if (width > bytes_ - offset) {
                return offset;
            }

            for (usize index = 1; index < width; ++index) {
                const auto next = static_cast<uint8>(data_[offset + index]);
                if ((next & 0xC0u) != 0x80u) {
                    return offset;
                }

                scalar = (scalar << 6u) | (next & 0x3Fu);
            }

            // 같은 문자를 긴 sequence로 표현한 overlong과 UTF-16 surrogate는 UTF-8로 수용하지
            // 않는다.
            if (scalar < minimum || scalar > 0x10FFFFu ||
                (scalar >= 0xD800u && scalar <= 0xDFFFu)) {
                return offset;
            }

            offset += width;
        }

        return IndexNone;
    }

    // maxBytes byte 이하이면서 UTF-8 문자 중간에서 끊기지 않는 가장 긴 앞부분이다. 잘라야 할 때
    // 끊는 자리의 byte가 continuation byte면 그 문자 전체를 뺀다. 원본 전체를 검증하지는 않는다
    // (유효한 UTF-8이면 결과도 유효하다). 고정 버퍼에 잘라 담을 때 쓴다.
    // 예: "가나"(6 byte).LeftBytesAtCodePoint(4)는 "가"(3 byte).
    constexpr StringView LeftBytesAtCodePoint(usize maxBytes) const noexcept {
        if (maxBytes >= bytes_) {
            return *this;
        }

        usize count = maxBytes;
        while (count > 0 && (static_cast<uint8>(data_[count]) & 0xC0u) == 0x80u) {
            --count;
        }

        return StringView(data_, count);
    }

    // offset부터 byteCount byte를 빌리는 부분 view다(검증하지 않는 byte 단위 — UE FStringView::Mid
    // 대응). 호출자가 경계를 알 때 쓴다(예: ASCII 구분자 자리에서 자르기 — 유효한 UTF-8을 ASCII
    // byte 자리에서 자르면 결과도 유효하다). 경계·UTF-8 검증이 필요하면 TrySliceBytes를 쓴다.
    // offset + byteCount가 ByteCount()를 넘으면 계약 위반으로 Fatal이다.
    // 예: StringView("Art/Rock.png").SliceBytes(4, 4) == "Rock"
    constexpr StringView SliceBytes(usize offset, usize byteCount) const noexcept {
        if (offset > bytes_ || byteCount > bytes_ - offset) {
            platform::Fatal("StringView::SliceBytes range is out of bounds");
        }

        return StringView(data_ + offset, byteCount);
    }

    // 원본 전체가 유효한 UTF-8인지와 양 끝이 code point 경계인지 검사한다.
    // byte 범위 초과·문자 중간 절단은 InvalidArgument다. 빈 slice도 UTF 경계가 필요하다.
    // 반환 view는 새 storage를 만들지 않으며 grapheme 경계까지 보장하지 않는다.
    Result<StringView> TrySliceBytes(usize offset, usize byteCount) const noexcept;

    // 검색은 byte 단위다. 예: if (view.Contains("rock")) { ... }
    // 기본은 UE의 ignore-case 사용법을 따르되 ASCII만 접는다. ASCII 밖의 byte는 정확히
    // 비교하며 Unicode 정규화·case folding은 하지 않는다. 이 view/needle의 UTF-8은
    // 자동 검증하지 않으므로 임의 byte도 검색할 수 있고 반환 위치는 byte offset이다.
    // startByte가 NotFound이거나 생략된 경우 앞에서는 0, 뒤에서는 ByteCount()다.
    // UE5.8과 같이 명시적 앞 검색 시작은 비어 있지 않으면 마지막 byte까지 제한하고,
    // 뒤 검색은 ByteCount()까지 제한한 배타적 끝으로 사용한다. 빈 needle의 앞 검색은
    // 제한된 시작 위치, 뒤 검색은 직전 byte 위치이며 시작이 0이면 NotFound다.
    // 위치 단위는 UE의 TCHAR code unit이 아닌 UTF-8 byte다.
    // 없으면 NotFound다. 알 수 없는 비교/방향 enum은 계약 위반으로 Fatal이다.
    static constexpr usize NotFound = IndexNone;
    usize Find(StringView needle, StringComparison comparison = StringComparison::IgnoreAsciiCase,
               StringSearchDirection direction = StringSearchDirection::FromStart,
               usize startByte = NotFound) const noexcept;
    bool Contains(StringView needle,
                  StringComparison comparison = StringComparison::IgnoreAsciiCase) const noexcept;
    bool StartsWith(StringView prefix,
                    StringComparison comparison = StringComparison::IgnoreAsciiCase) const noexcept;
    bool EndsWith(StringView suffix,
                  StringComparison comparison = StringComparison::IgnoreAsciiCase) const noexcept;

    // UE의 Left/Right/Mid처럼 범위를 문자열 길이로 제한한다. byte 인덱스가 UTF-8 문자
    // 중간이면 InvalidArgument다. 결과는 원본을 빌리므로 원본 변경·소멸 후 사용하지 않는다.
    Result<StringView> TryLeftBytes(usize count) const noexcept;
    Result<StringView> TryRightBytes(usize count) const noexcept;
    Result<StringView> TryMidBytes(usize start, usize count = MaxSize) const noexcept;

    // ---- 문자(Unicode code point) 단위 ----
    // UTF-8에서 byte 수와 글자 수가 다르므로 글자 단위로 자를 때 사용한다. 한 "글자"는 Unicode
    // scalar value 하나(U+0000–U+10FFFF, surrogate 제외)다. 예: "가나다"의 TryLeftCodePoints(2)는
    // "가나".
    // - 먼저 전체 UTF-8을 검증하며 잘못된 UTF-8은 InvalidArgument다. count·start는 byte API처럼
    //   길이로 제한하므로 범위를 넘어도 실패하지 않는다(Mid의 start가 길이 이상이면 끝의 빈 view).
    // - 결과는 원본을 빌린다. 원본 변경·소멸 후 사용하지 않는다.
    // - 화면상 한 글자가 여러 code point인 경우(결합 문자 e+U+0301, 이모지 ZWJ 시퀀스, 국기 등
    //   grapheme cluster)는 code point 단위로 나뉜다. grapheme 단위 분할은 Unicode 분할 규칙
    //   (UAX #29) 데이터가 필요하여 후속이다.
    // - UE Left/Right/Mid는 TCHAR(Windows UTF-16 code unit) 단위라 BMP 밖 문자(이모지 등)를 반으로
    //   자를 수 있지만 이 API는 code point를 자르지 않는다.
    Result<usize> TryCountCodePoints() const noexcept;
    Result<StringView> TryLeftCodePoints(usize count) const noexcept;
    Result<StringView> TryRightCodePoints(usize count) const noexcept;
    Result<StringView> TryMidCodePoints(usize start, usize count = MaxSize) const noexcept;
    // index번째 code point가 시작하는 byte offset이다. index == 글자 수이면 ByteCount()다.
    // index가 글자 수보다 크거나 UTF-8이 잘못되면 InvalidArgument다.
    Result<usize> TryByteOffsetOfCodePoint(usize index) const noexcept;
    // UTF-8을 검증한 뒤 code point 순회 range를 반환한다. 잘못된 UTF-8은 InvalidArgument다.
    Result<CodePointRange> TryCodePoints() const noexcept;

    // 첫 또는 마지막 구분자를 기준으로 byte view 두 개를 빌린다. 일치하지 않으면 false이고
    // 출력은 바꾸지 않는다. 동일 출력 객체를 두 번 전달하는 것은 계약 위반이다.
    // 빈 구분자의 위치는 Find의 앞/뒤 규약을 따른다. 포함 NUL과 비UTF-8 byte도 처리한다.
    bool Split(StringView delimiter, StringView& left, StringView& right,
               StringComparison comparison = StringComparison::IgnoreAsciiCase,
               StringSearchDirection direction = StringSearchDirection::FromStart) const noexcept;

    // ASCII 공백(SP, TAB, CR, LF, VT, FF)만 제거한 빌린 byte view다. Unicode 공백은
    // 변경하지 않는다. 원본 전체의 UTF-8 유효성 검사나 저장소 할당은 수행하지 않는다.
    StringView TrimAsciiStart() const noexcept;
    StringView TrimAsciiEnd() const noexcept;
    StringView TrimAscii() const noexcept;

    // byte 순 비교다(음수: left가 앞, 0: 같음, 양수: left가 뒤). 앞부분이 같으면 짧은 쪽이 앞이다.
    // 유효한 UTF-8끼리는 code point 순과 같다. 경로·이름을 결정적인 순서로 정렬할 때 쓴다
    // (locale·대소문자 접기 없음). 할당하지 않는다.
    static int CompareBytes(StringView left, StringView right) noexcept {
        const usize shared = left.bytes_ < right.bytes_ ? left.bytes_ : right.bytes_;
        const int prefix = platform::MemoryCompare(left.data_, right.data_, shared);
        if (prefix != 0 || left.bytes_ == right.bytes_) {
            return prefix;
        }

        return left.bytes_ < right.bytes_ ? -1 : 1;
    }

    friend constexpr bool operator==(StringView left, StringView right) noexcept {
        if (left.bytes_ != right.bytes_) {
            return false;
        }

        for (usize index = 0; index < left.bytes_; ++index) {
            if (left.data_[index] != right.data_[index]) {
                return false;
            }
        }

        return true;
    }

private:
    const char* data_ = nullptr;
    usize bytes_ = 0;
};

// ASCII 대소문자만 무시한 byte 같음이다(A–Z와 a–z만 접는다. 그 밖의 byte는 정확히 비교한다).
// HashStringBytesIgnoringAsciiCase(Core/Hash.h)와 짝이 맞는 같음이며, constexpr이라 컴파일 타임
// 비교에도 쓴다.
//
// 사용 예:
//   static_assert(EqualsIgnoringAsciiCase("Graphics", "GRAPHICS"));
constexpr bool EqualsIgnoringAsciiCase(StringView left, StringView right) noexcept {
    if (left.ByteCount() != right.ByteCount()) {
        return false;
    }

    for (usize index = 0; index < left.ByteCount(); ++index) {
        char a = left.Data()[index];
        char b = right.Data()[index];
        a = a >= 'A' && a <= 'Z' ? static_cast<char>(a + ('a' - 'A')) : a;
        b = b >= 'A' && b <= 'Z' ? static_cast<char>(b + ('a' - 'A')) : b;
        if (a != b) {
            return false;
        }
    }

    return true;
}

// 검증된 UTF-8 view의 code point를 char32_t로 순회하는 range다. StringView::TryCodePoints로 만든다.
//   auto points = view.TryCodePoints();
//   if (points) { for (char32_t point : points.Value()) { ... } }
// 생성 시 UTF-8을 검증하므로 순회는 디코딩 오류를 반환하지 않는다. 생성 후 원본 byte가 바뀌어
// 잘린 sequence를 만나면 범위 밖 읽기 대신 Fatal이다. 반복자는 STL 전방 반복자 연산을 제공한다.
class CodePointRange {
public:
    class Iterator {
    public:
        using value_type = char32_t;
        using reference = char32_t;
        using difference_type = isize;

        Iterator() noexcept = default;

        char32_t operator*() const noexcept;

        Iterator& operator++() noexcept;

        Iterator operator++(int) noexcept {
            Iterator previous = *this;
            ++*this;
            return previous;
        }

        friend bool operator==(const Iterator& left, const Iterator& right) noexcept {
            return left.position_ == right.position_;
        }

        // 현재 code point가 시작하는 원본 byte 위치다.
        const char* Position() const noexcept {
            return position_;
        }

    private:
        friend class CodePointRange;

        Iterator(const char* position, const char* end) noexcept : position_(position), end_(end) {
        }

        const char* position_ = nullptr;
        const char* end_ = nullptr;
    };

    using value_type = char32_t;
    using iterator = Iterator;
    using const_iterator = Iterator;

    CodePointRange() noexcept = default;

    Iterator begin() const noexcept {
        return Iterator(begin_, end_);
    }

    Iterator end() const noexcept {
        return Iterator(end_, end_);
    }

private:
    friend class StringView;

    CodePointRange(const char* begin, const char* end) noexcept : begin_(begin), end_(end) {
    }

    const char* begin_ = nullptr;
    const char* end_ = nullptr;
};

} // namespace rocks
