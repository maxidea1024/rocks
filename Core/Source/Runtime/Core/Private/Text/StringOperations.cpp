#include <Core/Text/String.h>
#include <Base/Checked.h>
#include <string.h>

namespace rocks {
namespace {

bool ValidComparison(StringComparison comparison) noexcept {
    return comparison == StringComparison::CaseSensitive ||
           comparison == StringComparison::IgnoreAsciiCase;
}

bool ValidDirection(StringSearchDirection direction) noexcept {
    return direction == StringSearchDirection::FromStart ||
           direction == StringSearchDirection::FromEnd;
}

char FoldAscii(char value) noexcept {
    if (value >= 'A' && value <= 'Z') {
        return static_cast<char>(value + ('a' - 'A'));
    }

    return value;
}

bool MatchesAt(StringView text, usize offset, StringView needle,
               StringComparison comparison) noexcept {
    for (usize index = 0; index < needle.ByteCount(); ++index) {
        char left = text.Data()[offset + index];
        char right = needle.Data()[index];
        if (comparison == StringComparison::IgnoreAsciiCase) {
            left = FoldAscii(left);
            right = FoldAscii(right);
        }
        if (left != right) {
            return false;
        }
    }

    return true;
}

bool IsAsciiWhitespace(char value) noexcept {
    return value == ' ' || (value >= '\t' && value <= '\r');
}

StringView Slice(StringView input, usize start, usize count) noexcept {
    return input.Data() ? StringView(input.Data() + start, count) : StringView();
}

} // namespace

usize StringView::Find(StringView needle, StringComparison comparison,
                       StringSearchDirection direction, usize startByte) const noexcept {
    if (!ValidComparison(comparison) || !ValidDirection(direction)) {
        platform::Fatal("StringView::Find requires valid comparison and direction");
    }

    const usize size = ByteCount();
    // UE5.6의 명시적 앞 검색 시작점은 비어 있지 않을 때 마지막 위치로 제한한다.
    // Rocks의 위치는 TCHAR code unit이 아니라 byte offset이다.
    const usize start = startByte == NotFound
                            ? (direction == StringSearchDirection::FromStart ? 0 : size)
                        : direction == StringSearchDirection::FromStart && size != 0
                            ? (startByte < size ? startByte : size - 1)
                            : (startByte < size ? startByte : size);
    if (needle.IsEmpty()) {
        if (direction == StringSearchDirection::FromEnd) {
            return start == 0 ? NotFound : start - 1;
        }
        return start;
    }

    if (direction == StringSearchDirection::FromStart) {
        if (needle.ByteCount() > size - start) {
            return NotFound;
        }
        const usize last = size - needle.ByteCount();
        for (usize position = start;; ++position) {
            if (MatchesAt(*this, position, needle, comparison)) {
                return position;
            }
            if (position == last) {
                break;
            }
        }
    } else {
        if (needle.ByteCount() > start) {
            return NotFound;
        }
        usize position = start - needle.ByteCount();
        for (;;) {
            if (MatchesAt(*this, position, needle, comparison)) {
                return position;
            }
            if (position == 0) {
                break;
            }
            --position;
        }
    }

    return NotFound;
}

bool StringView::Contains(StringView needle, StringComparison comparison) const noexcept {
    return Find(needle, comparison) != NotFound;
}

bool StringView::StartsWith(StringView prefix, StringComparison comparison) const noexcept {
    if (!ValidComparison(comparison)) {
        platform::Fatal("StringView::StartsWith requires valid comparison");
    }
    return prefix.ByteCount() <= ByteCount() && MatchesAt(*this, 0, prefix, comparison);
}

bool StringView::EndsWith(StringView suffix, StringComparison comparison) const noexcept {
    if (!ValidComparison(comparison)) {
        platform::Fatal("StringView::EndsWith requires valid comparison");
    }
    return suffix.ByteCount() <= ByteCount() &&
           MatchesAt(*this, ByteCount() - suffix.ByteCount(), suffix, comparison);
}

Result<StringView> StringView::TryLeftBytes(usize count) const noexcept {
    const usize length = count < ByteCount() ? count : ByteCount();
    return TrySliceBytes(0, length);
}

Result<StringView> StringView::TryRightBytes(usize count) const noexcept {
    const usize length = count < ByteCount() ? count : ByteCount();
    return TrySliceBytes(ByteCount() - length, length);
}

Result<StringView> StringView::TryMidBytes(usize start, usize count) const noexcept {
    const usize begin = start < ByteCount() ? start : ByteCount();
    const usize remaining = ByteCount() - begin;
    const usize length = count < remaining ? count : remaining;
    return TrySliceBytes(begin, length);
}

bool StringView::Split(StringView delimiter, StringView& left, StringView& right,
                       StringComparison comparison,
                       StringSearchDirection direction) const noexcept {
    if (&left == &right) {
        platform::Fatal("StringView::Split requires distinct output views");
    }
    const usize position = Find(delimiter, comparison, direction);
    if (position == NotFound) {
        return false;
    }

    // 출력이 원본 view 객체일 수 있으므로 두 view를 먼저 만들어 일괄 게시한다.
    const StringView before = Slice(*this, 0, position);
    const usize suffix = position + delimiter.ByteCount();
    const StringView after = Slice(*this, suffix, ByteCount() - suffix);
    left = before;
    right = after;
    return true;
}

StringView StringView::TrimAsciiStart() const noexcept {
    usize begin = 0;
    while (begin < ByteCount() && IsAsciiWhitespace(Data()[begin])) {
        ++begin;
    }
    return Slice(*this, begin, ByteCount() - begin);
}

StringView StringView::TrimAsciiEnd() const noexcept {
    usize end = ByteCount();
    while (end != 0 && IsAsciiWhitespace(Data()[end - 1])) {
        --end;
    }
    return Slice(*this, 0, end);
}

StringView StringView::TrimAscii() const noexcept {
    return TrimAsciiStart().TrimAsciiEnd();
}

void String::TrimAsciiStart() noexcept {
    const StringView trimmed = View().TrimAsciiStart();
    if (trimmed.ByteCount() == bytes_) {
        return;
    }
    memmove(data_, trimmed.Data(), trimmed.ByteCount());
    bytes_ = trimmed.ByteCount();
    data_[bytes_] = '\0';
}

void String::TrimAsciiEnd() noexcept {
    const StringView trimmed = View().TrimAsciiEnd();
    if (trimmed.ByteCount() == bytes_) {
        return;
    }
    bytes_ = trimmed.ByteCount();
    data_[bytes_] = '\0';
}

void String::TrimAscii() noexcept {
    const StringView trimmed = View().TrimAscii();
    if (trimmed.ByteCount() == bytes_) {
        return;
    }
    memmove(data_, trimmed.Data(), trimmed.ByteCount());
    bytes_ = trimmed.ByteCount();
    data_[bytes_] = '\0';
}

void String::ToUpperAscii() noexcept {
    for (usize index = 0; index < bytes_; ++index) {
        if (data_[index] >= 'a' && data_[index] <= 'z') {
            data_[index] = static_cast<char>(data_[index] - ('a' - 'A'));
        }
    }
}

void String::ToLowerAscii() noexcept {
    for (usize index = 0; index < bytes_; ++index) {
        data_[index] = FoldAscii(data_[index]);
    }
}

Result<usize> String::TryReplace(StringView from, StringView to,
                                 StringComparison comparison) noexcept {
    if (!ValidComparison(comparison) || !from.ValidateUtf8() || !to.ValidateUtf8()) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }
    // UE5.6과 같이 대소문자 구분 치환의 검색어와 결과가 같으면 변경하지 않는다.
    // view가 자기 문자열을 빌린 경우에도 비교만 하며 allocation이나 invalidation이 없다.
    if (from.IsEmpty() || IsEmpty() ||
        (comparison == StringComparison::CaseSensitive && from == to)) {
        return Result<usize>::Success(0);
    }

    const StringView original = View();
    usize count = 0;
    usize position = 0;
    usize resultBytes = original.ByteCount();
    for (;;) {
        const usize match =
            original.Find(from, comparison, StringSearchDirection::FromStart, position);
        if (match == StringView::NotFound) {
            break;
        }
        if (to.ByteCount() > from.ByteCount()) {
            if (!TryAdd(resultBytes, to.ByteCount() - from.ByteCount(), resultBytes) ||
                resultBytes == MaxSize) {
                return Result<usize>::Failure(ErrorCode::Overflow);
            }
        } else {
            resultBytes -= from.ByteCount() - to.ByteCount();
        }
        ++count;
        position = match + from.ByteCount();
        if (position == original.ByteCount()) {
            break;
        }
    }
    if (count == 0) {
        return Result<usize>::Success(0);
    }

    String replacement(*allocator_);
    auto reserved = replacement.TryReserve(resultBytes);
    if (!reserved) {
        return Result<usize>::Failure(reserved.Error());
    }
    position = 0;
    for (usize index = 0; index < count; ++index) {
        const usize match =
            original.Find(from, comparison, StringSearchDirection::FromStart, position);
        auto prefix = replacement.TryAppend(Slice(original, position, match - position));
        if (!prefix) {
            return Result<usize>::Failure(prefix.Error());
        }
        auto inserted = replacement.TryAppend(to);
        if (!inserted) {
            return Result<usize>::Failure(inserted.Error());
        }
        position = match + from.ByteCount();
    }
    auto suffix = replacement.TryAppend(Slice(original, position, original.ByteCount() - position));
    if (!suffix) {
        return Result<usize>::Failure(suffix.Error());
    }
    Swap(replacement);
    return Result<usize>::Success(count);
}

} // namespace rocks
