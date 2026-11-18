#include <Core/Text/Json.h>

#include "JsonInternal.h"
#include <Platform/Memory.h>

namespace rocks {

const char* JsonErrorMessage(JsonErrorCode code) noexcept {
    switch (code) {
    case JsonErrorCode::None:
        return "no error";
    case JsonErrorCode::InputTooLarge:
        return "input is larger than the read limit";
    case JsonErrorCode::UnexpectedEnd:
        return "unexpected end of input";
    case JsonErrorCode::UnexpectedCharacter:
        return "unexpected character";
    case JsonErrorCode::InvalidLiteral:
        return "invalid literal (expected true, false or null)";
    case JsonErrorCode::InvalidNumber:
        return "invalid number";
    case JsonErrorCode::NumberOutOfRange:
        return "number is out of range";
    case JsonErrorCode::InvalidEscape:
        return "invalid escape sequence";
    case JsonErrorCode::InvalidUtf8:
        return "invalid UTF-8";
    case JsonErrorCode::UnpairedSurrogate:
        return "unpaired UTF-16 surrogate escape";
    case JsonErrorCode::ControlCharacter:
        return "unescaped control character in string";
    case JsonErrorCode::DuplicateKey:
        return "duplicate object key";
    case JsonErrorCode::TrailingContent:
        return "content after the top-level value";
    case JsonErrorCode::DepthLimitExceeded:
        return "nesting depth exceeds the read limit";
    case JsonErrorCode::StringTooLong:
        return "string is longer than the read limit";
    case JsonErrorCode::TooManyNodes:
        return "value count exceeds the read limit";
    case JsonErrorCode::OutOfMemory:
        return "out of memory";
    }

    return "unknown error";
}

const char* JsonErrorMessage(JsonErrorCode code, JsonSyntax syntax) noexcept {
    if (syntax == JsonSyntax::Json5 && code == JsonErrorCode::InvalidLiteral) {
        return "invalid literal (expected true, false, null, Infinity or NaN)";
    }

    return JsonErrorMessage(code);
}

// ---- 문서 ----

using detail::HashKeyBytes;

uint64 JsonDocument::MemberHash::operator()(const MemberKey& key) const noexcept {
    return HashKeyBytes(bytes, key.object, text->Data() + key.offset, key.length);
}

uint64 JsonDocument::MemberHash::operator()(const MemberQuery& query) const noexcept {
    return HashKeyBytes(bytes, query.object, query.key.Data(), query.key.ByteCount());
}

bool JsonDocument::MemberEqual::operator()(const MemberKey& left,
                                           const MemberKey& right) const noexcept {
    return left.object == right.object && left.length == right.length &&
           platform::MemoryCompare(text->Data() + left.offset, text->Data() + right.offset,
                                   left.length) == 0;
}

bool JsonDocument::MemberEqual::operator()(const MemberKey& left,
                                           const MemberQuery& right) const noexcept {
    return left.object == right.object && left.length == right.key.ByteCount() &&
           platform::MemoryCompare(text->Data() + left.offset, right.key.Data(), left.length) == 0;
}

JsonDocument::JsonDocument(Allocator& allocator) noexcept
    : nodes_(allocator), positions_(allocator), elements_(allocator), members_(allocator),
      text_(allocator), warnings_(allocator),
      index_(allocator, MemberHash{&text_, {}}, MemberEqual{&text_}) {
}

Span<const JsonWarning> JsonDocument::Warnings() const noexcept {
    return Span<const JsonWarning>(warnings_.Data(), warnings_.Size());
}

void JsonDocument::Clear() noexcept {
    nodes_.Clear();
    positions_.Clear();
    elements_.Clear();
    members_.Clear();
    text_.Clear();
    warnings_.Clear();
    index_.Clear();
    // 이전 핸들을 무효로 만든다(0은 기본 생성 핸들의 세대라 건너뛴다).
    ++generation_;
    if (generation_ == 0) {
        generation_ = 1;
    }
}

StringView JsonDocument::Text(uint32 offset, uint32 length) const noexcept {
    return StringView(text_.Data() + offset, length);
}

} // namespace rocks
