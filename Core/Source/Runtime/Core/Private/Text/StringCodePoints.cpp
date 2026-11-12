#include <Core/Text/StringView.h>

#include <Platform/Fatal.h>

namespace rocks {
namespace {

// 검증된 UTF-8의 code point 시작 byte(10xxxxxx가 아닌 byte)인지 판정한다.
bool IsLeadByte(char byte) noexcept {
    return (static_cast<uint8>(byte) & 0xc0u) != 0x80u;
}

// 검증된 UTF-8에서 begin부터 count개 code point를 건너뛴 위치다. 끝에 닿으면 end에서 멈춘다.
const char* AdvanceCodePoints(const char* begin, const char* end, usize count) noexcept {
    const char* position = begin;
    while (count != 0 && position != end) {
        ++position;
        while (position != end && !IsLeadByte(*position)) {
            ++position;
        }
        --count;
    }
    return position;
}

usize CountLeadBytes(const char* begin, const char* end) noexcept {
    usize count = 0;
    for (const char* position = begin; position != end; ++position) {
        count += IsLeadByte(*position) ? 1 : 0;
    }
    return count;
}

// lead byte가 나타내는 sequence 길이다. 검증된 UTF-8에서만 의미가 있다.
usize SequenceLength(char lead) noexcept {
    const auto byte = static_cast<uint8>(lead);
    if (byte < 0x80u) {
        return 1;
    }
    if ((byte & 0xe0u) == 0xc0u) {
        return 2;
    }
    if ((byte & 0xf0u) == 0xe0u) {
        return 3;
    }
    return 4;
}

} // namespace

Result<usize> StringView::TryCountCodePoints() const noexcept {
    if (!ValidateUtf8()) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }
    return Result<usize>::Success(CountLeadBytes(data_, data_ + bytes_));
}

Result<StringView> StringView::TryLeftCodePoints(usize count) const noexcept {
    if (!ValidateUtf8()) {
        return Result<StringView>::Failure(ErrorCode::InvalidArgument);
    }
    const char* end = AdvanceCodePoints(data_, data_ + bytes_, count);
    return Result<StringView>::Success(StringView(data_, static_cast<usize>(end - data_)));
}

Result<StringView> StringView::TryRightCodePoints(usize count) const noexcept {
    if (!ValidateUtf8()) {
        return Result<StringView>::Failure(ErrorCode::InvalidArgument);
    }
    const usize total = CountLeadBytes(data_, data_ + bytes_);
    const usize skip = count >= total ? 0 : total - count;
    const char* begin = AdvanceCodePoints(data_, data_ + bytes_, skip);
    return Result<StringView>::Success(
        StringView(begin, static_cast<usize>(data_ + bytes_ - begin)));
}

Result<StringView> StringView::TryMidCodePoints(usize start, usize count) const noexcept {
    if (!ValidateUtf8()) {
        return Result<StringView>::Failure(ErrorCode::InvalidArgument);
    }
    const char* end = data_ + bytes_;
    const char* begin = AdvanceCodePoints(data_, end, start);
    const char* stop = AdvanceCodePoints(begin, end, count);
    return Result<StringView>::Success(StringView(begin, static_cast<usize>(stop - begin)));
}

Result<usize> StringView::TryByteOffsetOfCodePoint(usize index) const noexcept {
    if (!ValidateUtf8()) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }
    const char* end = data_ + bytes_;
    const char* position = data_;
    for (usize skipped = 0; skipped < index; ++skipped) {
        if (position == end) {
            return Result<usize>::Failure(ErrorCode::InvalidArgument);
        }
        position = AdvanceCodePoints(position, end, 1);
    }
    return Result<usize>::Success(static_cast<usize>(position - data_));
}

Result<CodePointRange> StringView::TryCodePoints() const noexcept {
    if (!ValidateUtf8()) {
        return Result<CodePointRange>::Failure(ErrorCode::InvalidArgument);
    }
    return Result<CodePointRange>::Success(CodePointRange(data_, data_ + bytes_));
}

char32_t CodePointRange::Iterator::operator*() const noexcept {
    if (position_ == nullptr || position_ == end_) {
        platform::Fatal("CodePointRange iterator is not dereferenceable");
    }
    const usize length = SequenceLength(*position_);
    if (static_cast<usize>(end_ - position_) < length) {
        platform::Fatal("CodePointRange source changed after validation");
    }
    const auto* bytes = reinterpret_cast<const uint8*>(position_);
    switch (length) {
    case 1:
        return bytes[0];
    case 2:
        return (static_cast<char32_t>(bytes[0] & 0x1fu) << 6) | (bytes[1] & 0x3fu);
    case 3:
        return (static_cast<char32_t>(bytes[0] & 0x0fu) << 12) |
               (static_cast<char32_t>(bytes[1] & 0x3fu) << 6) | (bytes[2] & 0x3fu);
    default:
        return (static_cast<char32_t>(bytes[0] & 0x07u) << 18) |
               (static_cast<char32_t>(bytes[1] & 0x3fu) << 12) |
               (static_cast<char32_t>(bytes[2] & 0x3fu) << 6) | (bytes[3] & 0x3fu);
    }
}

CodePointRange::Iterator& CodePointRange::Iterator::operator++() noexcept {
    if (position_ == nullptr || position_ == end_) {
        platform::Fatal("CodePointRange iterator incremented past the end");
    }
    position_ = AdvanceCodePoints(position_, end_, 1);
    return *this;
}

} // namespace rocks
