#include "ShaderPreprocessorInternal.h"

#include <Base/PlacementNew.h>
#include <Platform/Memory.h>

// 전처리기 텍스트 단계다(C 번역 단계 1–3의 부분집합): 원문 정규화, 줄 이음·주석을 처리한 논리
// 줄, 전처리 토큰 나누기.
namespace rocks::shader::preprocess_detail {

namespace preprocess_text_detail {

// UTF-8 BOM이다.
constexpr uint8 Utf8Bom[] = {0xEFu, 0xBBu, 0xBFu};

// trigraph(??x)의 셋째 문자다.
constexpr StringView TrigraphFinals("=/'()!<>-");

// 세 글자·두 글자 구두점이다(긴 것부터 맞춘다).
constexpr StringView ThreeCharacterPunctuators[] = {"<<=", ">>=", "...", "->*"};
constexpr StringView TwoCharacterPunctuators[] = {
    "->", "++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||",
    "*=", "/=", "%=", "+=", "-=", "&=", "^=", "|=", "##", "::", ".*"};
constexpr StringView SingleCharacterPunctuators("{}[]()<>;:,.?+-*/%^&|~!=#");

bool IsHorizontalSpace(char value) noexcept {
    return value == ' ' || value == '\t' || value == '\f' || value == '\v';
}

bool IsDigit(char value) noexcept {
    return value >= '0' && value <= '9';
}

bool Contains(StringView set, char value) noexcept {
    for (const char candidate : set) {
        if (candidate == value) {
            return true;
        }
    }

    return false;
}

bool IsTrigraphAt(const char* text, usize size, usize offset) noexcept {
    return offset + 2 < size && text[offset] == '?' && text[offset + 1] == '?' &&
           Contains(TrigraphFinals, text[offset + 2]);
}

// UTF-8 code point 하나의 byte 수다(lead byte 기준, 원문은 이미 검증되었다).
usize Utf8Width(uint8 lead) noexcept {
    if (lead < 0x80u) {
        return 1;
    }

    if ((lead & 0xE0u) == 0xC0u) {
        return 2;
    }

    if ((lead & 0xF0u) == 0xE0u) {
        return 3;
    }

    return 4;
}

// offset의 줄 번호(1부터, 정규화 전 byte의 CRLF·CR·LF를 줄바꿈 하나로 센다)다.
uint32 LineOfOffset(const uint8* data, usize offset) noexcept {
    uint32 line = 1;
    for (usize index = 0; index < offset; ++index) {
        if (data[index] == '\n') {
            ++line;
        } else if (data[index] == '\r' && (index + 1 >= offset || data[index + 1] != '\n')) {
            ++line;
        }
    }

    return line;
}

// text[offset]에서 시작하는 전처리 토큰 하나의 길이다. 리터럴이 줄 안에서 닫히지 않으면
// unterminated다.
usize LexAt(StringView text, usize offset, TokenKind& kind, bool& unterminated) noexcept {
    const char* data = text.Data();
    const usize size = text.ByteCount();
    unterminated = false;
    const char first = data[offset];
    if (IsIdentifierStart(first)) {
        usize end = offset + 1;
        while (end < size && IsIdentifierContinue(data[end])) {
            ++end;
        }

        kind = TokenKind::Identifier;
        return end - offset;
    }

    if (IsDigit(first) || (first == '.' && offset + 1 < size && IsDigit(data[offset + 1]))) {
        usize end = offset + 1;
        while (end < size) {
            const char value = data[end];
            if ((value == 'e' || value == 'E' || value == 'p' || value == 'P') && end + 1 < size &&
                (data[end + 1] == '+' || data[end + 1] == '-')) {
                end += 2;
            } else if (IsIdentifierContinue(value) || value == '.') {
                ++end;
            } else {
                break;
            }
        }

        kind = TokenKind::Number;
        return end - offset;
    }

    if (first == '"' || first == '\'') {
        usize end = offset + 1;
        while (end < size) {
            if (data[end] == '\\' && end + 1 < size) {
                end += 2;
                continue;
            }

            if (data[end] == first) {
                kind = first == '"' ? TokenKind::String : TokenKind::Character;
                return end + 1 - offset;
            }

            ++end;
        }

        unterminated = true;
        kind = first == '"' ? TokenKind::String : TokenKind::Character;
        return size - offset;
    }

    const StringView rest(data + offset, size - offset);
    for (const StringView punctuator : ThreeCharacterPunctuators) {
        if (rest.StartsWith(punctuator, StringComparison::CaseSensitive)) {
            kind = TokenKind::Punctuator;
            return punctuator.ByteCount();
        }
    }

    for (const StringView punctuator : TwoCharacterPunctuators) {
        if (rest.StartsWith(punctuator, StringComparison::CaseSensitive)) {
            kind = TokenKind::Punctuator;
            return punctuator.ByteCount();
        }
    }

    if (Contains(SingleCharacterPunctuators, first)) {
        kind = TokenKind::Punctuator;
        return 1;
    }

    kind = TokenKind::Other;
    const usize width = Utf8Width(static_cast<uint8>(first));
    return width <= size - offset ? width : size - offset;
}

} // namespace preprocess_text_detail

TextArena::~TextArena() noexcept {
    ReleaseFrom(head_);
    head_ = nullptr;
}

void TextArena::ReleaseFrom(Block* block) noexcept {
    while (block != nullptr) {
        Block* next = block->next;
        (void)allocator_->Deallocate(block);
        block = next;
    }
}

void* TextArena::Allocate(usize bytes, usize alignment) noexcept {
    if (alignment == 0 || alignment > alignof(Block) || (alignment & (alignment - 1)) != 0) {
        return nullptr;
    }

    if (head_ != nullptr) {
        const usize offset = (head_->used + alignment - 1) & ~(alignment - 1);
        if (offset <= head_->capacity && bytes <= head_->capacity - offset) {
            head_->used = offset + bytes;
            return BlockData(head_) + offset;
        }
    }

    if (bytes > MaxSize - sizeof(Block) - BlockBytes) {
        return nullptr;
    }

    const usize capacity = bytes > BlockBytes ? bytes : BlockBytes;
    const Result<void*> memory = allocator_->Allocate(sizeof(Block) + capacity, alignof(Block));
    if (!memory || memory.Value() == nullptr) {
        return nullptr;
    }

    Block* block = ::new (memory.Value()) Block{head_, capacity, bytes};
    head_ = block;
    return BlockData(block);
}

bool TextArena::Copy(StringView text, StringView& out) noexcept {
    char* storage = static_cast<char*>(Allocate(text.ByteCount()));
    if (storage == nullptr) {
        return false;
    }

    if (!text.IsEmpty()) {
        platform::MemoryCopy(storage, text.Data(), text.ByteCount());
    }

    out = StringView(storage, text.ByteCount());
    return true;
}

void TextArena::Reset() noexcept {
    if (head_ == nullptr) {
        return;
    }

    // 가장 오래된 블록(목록의 끝)만 남긴다.
    Block* oldest = head_;
    Block* newer = nullptr;
    while (oldest->next != nullptr) {
        newer = oldest;
        oldest = oldest->next;
    }

    if (newer != nullptr) {
        newer->next = nullptr;
        ReleaseFrom(head_);
    }

    oldest->used = 0;
    head_ = oldest;
}

bool IsIdentifierStart(char value) noexcept {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || value == '_';
}

bool IsIdentifierContinue(char value) noexcept {
    return IsIdentifierStart(value) || preprocess_text_detail::IsDigit(value);
}

bool IsCompilerBuiltinName(StringView name) noexcept {
    return name.ByteCount() >= 2 && name.Data()[0] == '_' && name.Data()[1] == '_' &&
           name != StringView("__LINE__") && name != StringView("__FILE__");
}

ShaderSourceStatus NormalizeSource(Span<const uint8> bytes, StringView path, TextArena& arena,
                                   StringView& out) noexcept {
    using namespace preprocess_text_detail;
    usize start = 0;
    if (bytes.Size() >= sizeof(Utf8Bom) && bytes[0] == Utf8Bom[0] && bytes[1] == Utf8Bom[1] &&
        bytes[2] == Utf8Bom[2]) {
        start = sizeof(Utf8Bom);
    }

    const usize size = bytes.Size() - start;
    const char* source = reinterpret_cast<const char*>(bytes.Data() + start);
    const StringView view(source, size);
    const usize invalid = view.FindInvalidUtf8();
    if (invalid != IndexNone) {
        return source_detail::MakeSourceStatus(ShaderSourceError::InvalidEncoding, path,
                                               LineOfOffset(bytes.Data(), start + invalid),
                                               "shader source is not valid UTF-8");
    }

    for (usize index = 0; index < size; ++index) {
        if (source[index] == '\0') {
            return source_detail::MakeSourceStatus(ShaderSourceError::InvalidEncoding, path,
                                                   LineOfOffset(bytes.Data(), start + index),
                                                   "shader source contains a NUL byte");
        }
    }

    char* storage = static_cast<char*>(arena.Allocate(size));
    if (storage == nullptr) {
        return source_detail::MakeSourceStatus(ShaderSourceError::OutOfMemory, path, 0,
                                               "out of memory");
    }

    usize written = 0;
    for (usize index = 0; index < size; ++index) {
        if (source[index] == '\r') {
            storage[written++] = '\n';
            if (index + 1 < size && source[index + 1] == '\n') {
                ++index;
            }
        } else {
            storage[written++] = source[index];
        }
    }

    out = StringView(storage, written);
    return ShaderSourceStatus{};
}

bool LineReader::Next(LogicalLine& line, TextArena& scratch, StringView path,
                      ShaderSourceStatus& status) noexcept {
    using namespace preprocess_text_detail;
    status = ShaderSourceStatus{};
    const char* text = text_.Data();
    const usize size = text_.ByteCount();
    if (position_ >= size) {
        return false;
    }

    buffer_.Clear();
    breaks_.Clear();
    const uint32 firstLine = line_;
    bool memoryOk = true;
    const auto push = [this, &memoryOk](char value) noexcept {
        memoryOk = memoryOk && static_cast<bool>(buffer_.TryPushBack(value));
    };
    const auto splice = [this, &memoryOk]() noexcept {
        ++line_;
        memoryOk =
            memoryOk && static_cast<bool>(breaks_.TryPushBack(static_cast<uint32>(buffer_.Size())));
    };
    const auto trigraph = [&status, &path, this]() noexcept {
        status = source_detail::MakeSourceStatus(ShaderSourceError::Trigraph, path, line_,
                                                 "trigraphs are not supported");
        return false;
    };

    usize cursor = position_;
    while (cursor < size) {
        const char value = text[cursor];
        if (value == '\\' && cursor + 1 < size && text[cursor + 1] == '\n') {
            cursor += 2;
            splice();
            continue;
        }

        if (value == '\n') {
            ++cursor;
            ++line_;
            break;
        }

        if (IsTrigraphAt(text, size, cursor)) {
            return trigraph();
        }

        if (value == '/' && cursor + 1 < size && text[cursor + 1] == '/') {
            // 줄 주석: 줄 이음으로 이어진 다음 줄까지 주석이다(C 번역 단계 2가 먼저다).
            cursor += 2;
            while (cursor < size) {
                if (text[cursor] == '\\' && cursor + 1 < size && text[cursor + 1] == '\n') {
                    cursor += 2;
                    splice();
                    continue;
                }

                if (text[cursor] == '\n') {
                    break;
                }

                ++cursor;
            }

            push(' ');
            continue;
        }

        if (value == '/' && cursor + 1 < size && text[cursor + 1] == '*') {
            const uint32 commentLine = line_;
            cursor += 2;
            bool closed = false;
            while (cursor < size) {
                if (text[cursor] == '*' && cursor + 1 < size && text[cursor + 1] == '/') {
                    cursor += 2;
                    closed = true;
                    break;
                }

                if (text[cursor] == '\n') {
                    splice();
                }

                ++cursor;
            }

            if (!closed) {
                status =
                    source_detail::MakeSourceStatus(ShaderSourceError::UnterminatedComment, path,
                                                    commentLine, "unterminated /* comment");
                return false;
            }

            push(' ');
            continue;
        }

        if (value == '"' || value == '\'') {
            // 리터럴 안의 '//'·'/*'는 주석이 아니다. 줄 안에서 닫히지 않으면 토큰 나누기가 활성
            // 구역에서만 오류로 보고한다(비활성 구역의 아포스트로피는 오류가 아니다).
            push(value);
            ++cursor;
            while (cursor < size) {
                const char inner = text[cursor];
                if (inner == '\\' && cursor + 1 < size && text[cursor + 1] == '\n') {
                    cursor += 2;
                    splice();
                    continue;
                }

                if (inner == '\n') {
                    break;
                }

                if (IsTrigraphAt(text, size, cursor)) {
                    return trigraph();
                }

                if (inner == '\\' && cursor + 1 < size && text[cursor + 1] != '\n') {
                    push(inner);
                    push(text[cursor + 1]);
                    cursor += 2;
                    continue;
                }

                push(inner);
                ++cursor;
                if (inner == value) {
                    break;
                }
            }

            continue;
        }

        push(value);
        ++cursor;
    }

    position_ = cursor;
    if (!memoryOk) {
        status = source_detail::MakeSourceStatus(ShaderSourceError::OutOfMemory, path, firstLine,
                                                 "out of memory");
        return false;
    }

    StringView copied;
    uint32* breaks =
        static_cast<uint32*>(scratch.Allocate(breaks_.Size() * sizeof(uint32), alignof(uint32)));
    if (!scratch.Copy(StringView(buffer_.Data(), buffer_.Size()), copied) || breaks == nullptr) {
        status = source_detail::MakeSourceStatus(ShaderSourceError::OutOfMemory, path, firstLine,
                                                 "out of memory");
        return false;
    }

    if (!breaks_.IsEmpty()) {
        platform::MemoryCopy(breaks, breaks_.Data(), breaks_.Size() * sizeof(uint32));
    }

    line.text = copied;
    line.breaks = Span<const uint32>(breaks, breaks_.Size());
    line.firstLine = firstLine;
    line.directive = false;
    for (const char value : copied) {
        if (!IsHorizontalSpace(value)) {
            line.directive = value == '#';
            break;
        }
    }

    return true;
}

ShaderSourceStatus Tokenize(const LogicalLine& line, StringView path, Array<Token>& out) noexcept {
    using namespace preprocess_text_detail;
    const char* data = line.text.Data();
    const usize size = line.text.ByteCount();
    usize cursor = 0;
    usize breakIndex = 0;
    while (true) {
        const usize spaceStart = cursor;
        while (cursor < size && IsHorizontalSpace(data[cursor])) {
            ++cursor;
        }

        if (cursor >= size) {
            break;
        }

        while (breakIndex < line.breaks.Size() && line.breaks[breakIndex] <= cursor) {
            ++breakIndex;
        }

        Token token;
        token.space = StringView(data + spaceStart, cursor - spaceStart);
        token.leadingSpace = cursor > spaceStart;
        token.line = line.firstLine + static_cast<uint32>(breakIndex);
        bool unterminated = false;
        const usize length = LexAt(line.text, cursor, token.kind, unterminated);
        if (unterminated) {
            return source_detail::MakeSourceStatus(
                ShaderSourceError::UnterminatedLiteral, path, token.line,
                token.kind == TokenKind::String ? "missing terminating '\"' character"
                                                : "missing terminating ' character");
        }

        token.text = StringView(data + cursor, length);
        if (!out.TryPushBack(token)) {
            return source_detail::MakeSourceStatus(ShaderSourceError::OutOfMemory, path, token.line,
                                                   "out of memory");
        }

        cursor += length;
    }

    return ShaderSourceStatus{};
}

bool IsSingleToken(StringView text, TokenKind& kind) noexcept {
    if (text.IsEmpty() || preprocess_text_detail::IsHorizontalSpace(text.Data()[0])) {
        return false;
    }

    bool unterminated = false;
    const usize length = preprocess_text_detail::LexAt(text, 0, kind, unterminated);
    return !unterminated && length == text.ByteCount();
}

usize FirstTokenLength(StringView text) noexcept {
    if (text.IsEmpty() || preprocess_text_detail::IsHorizontalSpace(text.Data()[0])) {
        return 0;
    }

    TokenKind kind = TokenKind::Other;
    bool unterminated = false;
    return preprocess_text_detail::LexAt(text, 0, kind, unterminated);
}

} // namespace rocks::shader::preprocess_detail
