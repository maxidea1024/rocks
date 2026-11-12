#include <Core/Text/Json.h>

#include <Core/Hash.h>
#include <Core/Text/NumberText.h>
#include <Platform/Fatal.h>
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
        return "number is out of double range";
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

// ---- 문서 ----

namespace {

// 입력 상한: 노드·문자열 위치가 uint32이고 텍스트(이스케이프를 푼 문자열 + 숫자 원문)는 입력을 넘지
// 않는다.
constexpr uint64 HardInputLimit = 1ull << 30;

uint64 HashKeyBytes(uint32 owner, const char* bytes, usize length) noexcept {
    return MixHash(HashStringBytes(StringView(bytes, length)) ^
                   (static_cast<uint64>(owner) * 0x9E3779B97F4A7C15ull));
}

bool Overlaps(const char* data, usize size, const char* begin, usize count) noexcept {
    const auto value = reinterpret_cast<usize>(data);
    const auto start = reinterpret_cast<usize>(begin);
    return size != 0 && count != 0 && value < start + count && start < value + size;
}

} // namespace

uint64 JsonDocument::MemberHash::operator()(const MemberKey& key) const noexcept {
    return HashKeyBytes(key.object, text->Data() + key.offset, key.length);
}

uint64 JsonDocument::MemberHash::operator()(const MemberQuery& query) const noexcept {
    return HashKeyBytes(query.object, query.key.Data(), query.key.ByteCount());
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
    : nodes_(allocator), elements_(allocator), members_(allocator), text_(allocator),
      warnings_(allocator), index_(allocator, MemberHash{&text_}, MemberEqual{&text_}) {
}

Span<const JsonWarning> JsonDocument::Warnings() const noexcept {
    return Span<const JsonWarning>(warnings_.Data(), warnings_.Size());
}

void JsonDocument::Clear() noexcept {
    nodes_.Clear();
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

// ---- 파서 ----

class JsonParser {
public:
    JsonParser(JsonDocument& document, StringView input, const JsonReadOptions& options) noexcept
        : document_(document), data_(reinterpret_cast<const uint8*>(input.Data())),
          size_(input.ByteCount()), options_(options), frames_(document.nodes_.GetAllocator()),
          pendingElements_(document.nodes_.GetAllocator()),
          pendingMembers_(document.nodes_.GetAllocator()) {
    }

    JsonError Run() noexcept;

private:
    struct Frame {
        uint32 node;
        bool object;
        uint32 pendingStart;
    };

    using Node = JsonDocument::Node;
    using Member = JsonDocument::Member;

    // 오류 위치를 기록하고 false를 돌려준다(첫 오류만 남긴다).
    bool Fail(JsonErrorCode code, usize offset) noexcept {
        if (errorCode_ == JsonErrorCode::None) {
            errorCode_ = code;
            errorOffset_ = offset;
        }

        return false;
    }

    bool Memory(Result<void> result) noexcept {
        return result ? true : Fail(JsonErrorCode::OutOfMemory, position_);
    }

    void SkipWhitespace() noexcept {
        while (position_ < size_) {
            const uint8 byte = data_[position_];
            if (byte != ' ' && byte != '\t' && byte != '\n' && byte != '\r') {
                return;
            }

            ++position_;
        }
    }

    // 노드를 추가한다. at은 값의 시작 위치(한도 초과 보고용)다. 스칼라는 서브트리 끝이 자기
    // 다음이다.
    bool AddNode(Node node, uint32& index, usize at) noexcept {
        if (document_.nodes_.Size() >= options_.maxNodes) {
            return Fail(JsonErrorCode::TooManyNodes, at);
        }

        index = static_cast<uint32>(document_.nodes_.Size());
        node.end = index + 1;
        return Memory(document_.nodes_.TryPushBack(node));
    }

    bool ParseValue(uint32& node) noexcept;
    bool ParseString(uint32& offset, uint32& length) noexcept;
    bool ParseNumber(uint32& node) noexcept;
    bool ParseLiteral(uint32& node) noexcept;
    bool ReadHex(usize escape, uint32& unit) noexcept;
    bool AppendText(const void* bytes, usize count) noexcept;
    bool AppendCodePoint(uint32 codePoint) noexcept;
    bool BeginKey() noexcept;
    bool CloseContainer() noexcept;
    JsonPosition PositionOf(usize offset) const noexcept;

    JsonDocument& document_;
    const uint8* data_;
    usize size_;
    const JsonReadOptions& options_;
    usize position_ = 0;
    usize bomLength_ = 0;
    Array<Frame> frames_;
    Array<uint32> pendingElements_;
    Array<Member> pendingMembers_;
    JsonErrorCode errorCode_ = JsonErrorCode::None;
    usize errorOffset_ = 0;
};

JsonPosition JsonParser::PositionOf(usize offset) const noexcept {
    // 오류 때만 앞에서부터 센다. CRLF는 줄바꿈 1개, CR 단독도 줄바꿈, column은 code point(UTF-8
    // 이어지는 byte는 세지 않는다). 건너뛴 BOM은 세지 않는다.
    JsonPosition position;
    position.byteOffset = offset;
    for (usize index = bomLength_; index < offset && index < size_; ++index) {
        const uint8 byte = data_[index];
        if (byte == '\n' || (byte == '\r' && !(index + 1 < size_ && data_[index + 1] == '\n'))) {
            ++position.line;
            position.column = 1;
        } else if (byte == '\r') {
            continue; // CRLF의 CR: 다음 LF가 줄을 바꾼다
        } else if ((byte & 0xC0) != 0x80) {
            ++position.column;
        }
    }

    return position;
}

bool JsonParser::AppendText(const void* bytes, usize count) noexcept {
    return Memory(
        document_.text_.TryAppend(Span<const char>(static_cast<const char*>(bytes), count)));
}

bool JsonParser::AppendCodePoint(uint32 codePoint) noexcept {
    char bytes[4];
    usize count = 0;
    if (codePoint < 0x80) {
        bytes[count++] = static_cast<char>(codePoint);
    } else if (codePoint < 0x800) {
        bytes[count++] = static_cast<char>(0xC0 | (codePoint >> 6));
        bytes[count++] = static_cast<char>(0x80 | (codePoint & 0x3F));
    } else if (codePoint < 0x10000) {
        bytes[count++] = static_cast<char>(0xE0 | (codePoint >> 12));
        bytes[count++] = static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
        bytes[count++] = static_cast<char>(0x80 | (codePoint & 0x3F));
    } else {
        bytes[count++] = static_cast<char>(0xF0 | (codePoint >> 18));
        bytes[count++] = static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F));
        bytes[count++] = static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
        bytes[count++] = static_cast<char>(0x80 | (codePoint & 0x3F));
    }

    return AppendText(bytes, count);
}

namespace {

int HexValue(uint8 byte) noexcept {
    if (byte >= '0' && byte <= '9') {
        return byte - '0';
    }

    if (byte >= 'a' && byte <= 'f') {
        return byte - 'a' + 10;
    }

    if (byte >= 'A' && byte <= 'F') {
        return byte - 'A' + 10;
    }

    return -1;
}

// data[position]에서 시작하는 UTF-8 한 글자의 길이(잘못되면 0). overlong·surrogate 부호화·U+10FFFF
// 초과·잘림을 거부한다.
usize Utf8Length(const uint8* data, usize size, usize position) noexcept {
    const uint8 lead = data[position];
    usize length = 0;
    uint32 minimum = 0;
    uint32 codePoint = 0;
    if (lead >= 0xC2 && lead <= 0xDF) {
        length = 2;
        minimum = 0x80;
        codePoint = lead & 0x1F;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        length = 3;
        minimum = 0x800;
        codePoint = lead & 0x0F;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        length = 4;
        minimum = 0x10000;
        codePoint = lead & 0x07;
    } else {
        return 0;
    }

    if (position + length > size) {
        return 0;
    }

    for (usize index = 1; index < length; ++index) {
        const uint8 next = data[position + index];
        if ((next & 0xC0) != 0x80) {
            return 0;
        }

        codePoint = (codePoint << 6) | (next & 0x3F);
    }

    if (codePoint < minimum || codePoint > 0x10FFFF ||
        (codePoint >= 0xD800 && codePoint <= 0xDFFF)) {
        return 0;
    }

    return length;
}

} // namespace

// \u 뒤 16진수 4자리(position_은 첫 자리). 잘못된 자리가 있으면 그 이스케이프(escape) 위치에서
// InvalidEscape, 모두 16진수인데 입력이 먼저 끝났으면 UnexpectedEnd다.
bool JsonParser::ReadHex(usize escape, uint32& unit) noexcept {
    unit = 0;
    for (usize index = 0; index < 4; ++index) {
        if (position_ + index >= size_) {
            return Fail(JsonErrorCode::UnexpectedEnd, size_);
        }

        const int digit = HexValue(data_[position_ + index]);
        if (digit < 0) {
            return Fail(JsonErrorCode::InvalidEscape, escape);
        }

        unit = (unit << 4) | static_cast<uint32>(digit);
    }

    position_ += 4;
    return true;
}

bool JsonParser::ParseString(uint32& offset, uint32& length) noexcept {
    // position_은 여는 따옴표다.
    const usize start = position_;
    ++position_;
    const usize textStart = document_.text_.Size();
    offset = static_cast<uint32>(textStart);
    auto withinLimit = [this, textStart](usize adding) noexcept {
        return document_.text_.Size() - textStart + adding <= options_.maxStringBytes;
    };
    while (true) {
        // 이스케이프·제어 문자·비 ASCII가 아닌 구간은 한 번에 붙인다(한도는 붙이기 전에 본다).
        usize run = position_;
        while (run < size_ && data_[run] != '"' && data_[run] != '\\' && data_[run] >= 0x20 &&
               data_[run] < 0x80) {
            ++run;
        }

        if (run != position_) {
            if (!withinLimit(run - position_)) {
                return Fail(JsonErrorCode::StringTooLong, start);
            }

            if (!AppendText(data_ + position_, run - position_)) {
                return false;
            }

            position_ = run;
            continue;
        }

        if (position_ >= size_) {
            return Fail(JsonErrorCode::UnexpectedEnd, position_);
        }

        const uint8 byte = data_[position_];
        if (byte == '"') {
            ++position_;
            break;
        }

        if (byte < 0x20) {
            return Fail(JsonErrorCode::ControlCharacter, position_);
        }

        if (byte >= 0x80) {
            const usize count = Utf8Length(data_, size_, position_);
            if (count == 0) {
                return Fail(JsonErrorCode::InvalidUtf8, position_);
            }

            if (!withinLimit(count)) {
                return Fail(JsonErrorCode::StringTooLong, start);
            }

            if (!AppendText(data_ + position_, count)) {
                return false;
            }

            position_ += count;
            continue;
        }

        // 이스케이프.
        const usize escape = position_;
        if (position_ + 1 >= size_) {
            return Fail(JsonErrorCode::UnexpectedEnd, size_);
        }

        const uint8 kind = data_[position_ + 1];
        position_ += 2;
        char simple = 0;
        switch (kind) {
        case '"':
            simple = '"';
            break;
        case '\\':
            simple = '\\';
            break;
        case '/':
            simple = '/';
            break;
        case 'b':
            simple = '\b';
            break;
        case 'f':
            simple = '\f';
            break;
        case 'n':
            simple = '\n';
            break;
        case 'r':
            simple = '\r';
            break;
        case 't':
            simple = '\t';
            break;
        case 'u':
            break;
        default:
            return Fail(JsonErrorCode::InvalidEscape, escape);
        }

        if (kind != 'u') {
            if (!withinLimit(1)) {
                return Fail(JsonErrorCode::StringTooLong, start);
            }

            if (!AppendText(&simple, 1)) {
                return false;
            }

            continue;
        }

        uint32 unit = 0;
        if (!ReadHex(escape, unit)) {
            return false;
        }

        if (unit >= 0xDC00 && unit <= 0xDFFF) {
            return Fail(JsonErrorCode::UnpairedSurrogate, escape);
        }

        if (unit >= 0xD800 && unit <= 0xDBFF) {
            // 바로 뒤에 낮은 surrogate 이스케이프가 와야 한다. 그 전에 입력이 끝나면 문자열이
            // 끝나지 않은 것이다.
            if (position_ >= size_ || (data_[position_] == '\\' && position_ + 1 >= size_)) {
                return Fail(JsonErrorCode::UnexpectedEnd, size_);
            }

            if (data_[position_] != '\\' || data_[position_ + 1] != 'u') {
                return Fail(JsonErrorCode::UnpairedSurrogate, escape);
            }

            const usize second = position_;
            position_ += 2;
            uint32 low = 0;
            if (!ReadHex(second, low)) {
                return false;
            }

            if (low < 0xDC00 || low > 0xDFFF) {
                return Fail(JsonErrorCode::UnpairedSurrogate, second);
            }

            unit = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
        }

        if (!withinLimit(4)) {
            return Fail(JsonErrorCode::StringTooLong, start);
        }

        if (!AppendCodePoint(unit)) {
            return false;
        }
    }

    length = static_cast<uint32>(document_.text_.Size() - textStart);
    return true;
}

bool JsonParser::ParseNumber(uint32& node) noexcept {
    // -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?
    const usize start = position_;
    usize index = position_;
    auto isDigit = [this](usize at) noexcept {
        return at < size_ && data_[at] >= '0' && data_[at] <= '9';
    };
    if (index < size_ && data_[index] == '-') {
        ++index;
    }

    if (!isDigit(index)) {
        return Fail(index >= size_ ? JsonErrorCode::UnexpectedEnd : JsonErrorCode::InvalidNumber,
                    index);
    }

    if (data_[index] == '0') {
        ++index;
        if (isDigit(index)) {
            return Fail(JsonErrorCode::InvalidNumber, index); // 앞자리 0
        }
    } else {
        while (isDigit(index)) {
            ++index;
        }
    }

    bool integer = true;
    if (index < size_ && data_[index] == '.') {
        integer = false;
        ++index;
        if (!isDigit(index)) {
            return Fail(index >= size_ ? JsonErrorCode::UnexpectedEnd
                                       : JsonErrorCode::InvalidNumber,
                        index);
        }

        while (isDigit(index)) {
            ++index;
        }
    }

    if (index < size_ && (data_[index] == 'e' || data_[index] == 'E')) {
        integer = false;
        ++index;
        if (index < size_ && (data_[index] == '+' || data_[index] == '-')) {
            ++index;
        }

        if (!isDigit(index)) {
            return Fail(index >= size_ ? JsonErrorCode::UnexpectedEnd
                                       : JsonErrorCode::InvalidNumber,
                        index);
        }

        while (isDigit(index)) {
            ++index;
        }
    }

    position_ = index;
    const StringView token(reinterpret_cast<const char*>(data_ + start), index - start);
    Node value{};
    value.first = static_cast<uint32>(document_.text_.Size());
    value.count = static_cast<uint32>(token.ByteCount());
    if (!AppendText(token.Data(), token.ByteCount())) {
        return false;
    }

    const bool negativeZero = token == StringView("-0");
    if (integer && !negativeZero) {
        if (auto parsed = TryParseInt64(token)) {
            value.type = JsonType::Integer;
            value.bits = static_cast<uint64>(parsed.Value());
            return AddNode(value, node, start);
        }

        if (token.Data()[0] != '-') {
            if (auto parsed = TryParseUInt64(token)) {
                value.type = JsonType::Unsigned;
                value.bits = parsed.Value();
                return AddNode(value, node, start);
            }
        }
    }

    auto parsed = TryParseDouble(token);
    if (!parsed) {
        return Fail(JsonErrorCode::NumberOutOfRange, start);
    }

    value.type = JsonType::Double;
    value.bits = platform::BitCast<uint64>(parsed.Value());
    return AddNode(value, node, start);
}

bool JsonParser::ParseLiteral(uint32& node) noexcept {
    struct Literal {
        const char* text;
        usize length;
        JsonType type;
        bool value;
    };
    const Literal literals[] = {{"true", 4, JsonType::Bool, true},
                                {"false", 5, JsonType::Bool, false},
                                {"null", 4, JsonType::Null, false}};
    for (const Literal& literal : literals) {
        if (data_[position_] != static_cast<uint8>(literal.text[0])) {
            continue;
        }

        if (position_ + literal.length > size_ ||
            platform::MemoryCompare(data_ + position_, literal.text, literal.length) != 0) {
            // 입력이 리터럴의 앞부분에서 끝났으면 UnexpectedEnd다.
            const usize available = size_ - position_;
            if (available < literal.length &&
                platform::MemoryCompare(data_ + position_, literal.text, available) == 0) {
                return Fail(JsonErrorCode::UnexpectedEnd, size_);
            }

            return Fail(JsonErrorCode::InvalidLiteral, position_);
        }

        const usize start = position_;
        position_ += literal.length;
        Node value{};
        value.type = literal.type;
        value.boolean = literal.value;
        return AddNode(value, node, start);
    }

    return Fail(JsonErrorCode::UnexpectedCharacter, position_);
}

bool JsonParser::ParseValue(uint32& node) noexcept {
    // position_은 공백 뒤 값의 첫 byte다(스칼라만 — 컨테이너는 Run이 처리한다).
    const usize start = position_;
    const uint8 byte = data_[position_];
    if (byte == '"') {
        Node value{};
        value.type = JsonType::String;
        return ParseString(value.first, value.count) && AddNode(value, node, start);
    }

    if (byte == '-' || (byte >= '0' && byte <= '9')) {
        return ParseNumber(node);
    }

    return ParseLiteral(node);
}

// 객체 안에서 키 문자열과 ':'까지 읽는다(position_은 공백 뒤).
bool JsonParser::BeginKey() noexcept {
    if (position_ >= size_) {
        return Fail(JsonErrorCode::UnexpectedEnd, position_);
    }

    if (data_[position_] != '"') {
        return Fail(JsonErrorCode::UnexpectedCharacter, position_);
    }

    const usize keyStart = position_;
    Member member{};
    if (!ParseString(member.keyOffset, member.keyLength)) {
        return false;
    }

    const uint32 object = frames_[frames_.Size() - 1].node;
    auto inserted =
        document_.index_.Insert(JsonDocument::MemberKey{object, member.keyOffset, member.keyLength},
                                static_cast<uint32>(0));
    if (!inserted) {
        return Fail(JsonErrorCode::OutOfMemory, keyStart);
    }

    if (inserted.Value() == InsertOutcome::AlreadyPresent) {
        return Fail(JsonErrorCode::DuplicateKey, keyStart);
    }

    if (!Memory(pendingMembers_.TryPushBack(member))) {
        return false;
    }

    SkipWhitespace();
    if (position_ >= size_) {
        return Fail(JsonErrorCode::UnexpectedEnd, position_);
    }

    if (data_[position_] != ':') {
        return Fail(JsonErrorCode::UnexpectedCharacter, position_);
    }

    ++position_;
    SkipWhitespace();
    return true;
}

// 맨 위 컨테이너를 닫는다: 대기 중인 자식을 최종 배열로 옮기고 노드에 범위와 서브트리 끝을
// 기록한다.
bool JsonParser::CloseContainer() noexcept {
    const Frame frame = frames_[frames_.Size() - 1];
    Node& container = document_.nodes_[frame.node];
    container.end = static_cast<uint32>(document_.nodes_.Size());
    if (frame.object) {
        const usize count = pendingMembers_.Size() - frame.pendingStart;
        container.first = static_cast<uint32>(document_.members_.Size());
        container.count = static_cast<uint32>(count);
        if (!Memory(document_.members_.TryAppend(
                Span<const Member>(pendingMembers_.Data() + frame.pendingStart, count)))) {
            return false;
        }

        // 색인 값(멤버 위치)을 최종 위치로 고친다.
        for (usize index = 0; index < count; ++index) {
            const Member& member = document_.members_[container.first + index];
            uint32* slot = document_.index_.Find(
                JsonDocument::MemberKey{frame.node, member.keyOffset, member.keyLength});
            *slot = static_cast<uint32>(container.first + index);
        }

        static_cast<void>(
            pendingMembers_.TryResize(frame.pendingStart)); // 줄이기는 할당하지 않는다
    } else {
        const usize count = pendingElements_.Size() - frame.pendingStart;
        container.first = static_cast<uint32>(document_.elements_.Size());
        container.count = static_cast<uint32>(count);
        if (!Memory(document_.elements_.TryAppend(
                Span<const uint32>(pendingElements_.Data() + frame.pendingStart, count)))) {
            return false;
        }

        static_cast<void>(pendingElements_.TryResize(frame.pendingStart));
    }

    static_cast<void>(frames_.TryResize(frames_.Size() - 1));
    return true;
}

JsonError JsonParser::Run() noexcept {
    JsonError result;
    if (size_ > options_.maxInputBytes || size_ > HardInputLimit) {
        result.code = JsonErrorCode::InputTooLarge;
        return result;
    }

    if (size_ >= 3 && data_[0] == 0xEF && data_[1] == 0xBB && data_[2] == 0xBF) {
        position_ = 3;
        bomLength_ = 3;
        if (!Memory(document_.warnings_.TryPushBack(
                JsonWarning{JsonWarningCode::BomIgnored, JsonPosition{}}))) {
            result.code = errorCode_;
            return result;
        }
    }

    bool ok = true;
    bool haveValue = false;
    uint32 value = 0;
    SkipWhitespace();
    while (ok) {
        if (!haveValue) {
            if (position_ >= size_) {
                ok = Fail(JsonErrorCode::UnexpectedEnd, position_);
                break;
            }

            const uint8 byte = data_[position_];
            if (byte == '{' || byte == '[') {
                if (frames_.Size() >= options_.maxDepth) {
                    ok = Fail(JsonErrorCode::DepthLimitExceeded, position_);
                    break;
                }

                const bool object = byte == '{';
                Node container{};
                container.type = object ? JsonType::Object : JsonType::Array;
                uint32 index = 0;
                if (!AddNode(container, index, position_) ||
                    !Memory(frames_.TryPushBack(
                        Frame{index, object,
                              static_cast<uint32>(object ? pendingMembers_.Size()
                                                         : pendingElements_.Size())}))) {
                    ok = false;
                    break;
                }

                ++position_;
                SkipWhitespace();
                if (position_ < size_ && data_[position_] == (object ? '}' : ']')) {
                    ++position_;
                    ok = CloseContainer();
                    value = index;
                    haveValue = true;
                    continue;
                }

                if (object) {
                    ok = BeginKey();
                }

                continue;
            }

            ok = ParseValue(value);
            haveValue = ok;
            continue;
        }

        // 값 하나가 끝났다.
        if (frames_.IsEmpty()) {
            break;
        }

        const Frame frame = frames_[frames_.Size() - 1];
        if (frame.object) {
            pendingMembers_[pendingMembers_.Size() - 1].value = value;
        } else if (!Memory(pendingElements_.TryPushBack(value))) {
            ok = false;
            break;
        }

        SkipWhitespace();
        if (position_ >= size_) {
            ok = Fail(JsonErrorCode::UnexpectedEnd, position_);
            break;
        }

        const uint8 byte = data_[position_];
        if (byte == ',') {
            ++position_;
            SkipWhitespace();
            haveValue = false;
            if (frame.object) {
                ok = BeginKey();
            }

            continue;
        }

        if (byte == (frame.object ? '}' : ']')) {
            ++position_;
            value = frame.node;
            ok = CloseContainer();
            continue;
        }

        ok = Fail(JsonErrorCode::UnexpectedCharacter, position_);
    }

    if (ok) {
        SkipWhitespace();
        if (position_ != size_) {
            ok = Fail(JsonErrorCode::TrailingContent, position_);
        }
    }

    if (!ok) {
        result.code = errorCode_;
        result.position = PositionOf(errorOffset_);
        document_.Clear();
    }

    return result;
}

JsonError JsonDocument::Parse(StringView input, const JsonReadOptions& options) noexcept {
    // 입력이 이 문서의 문자열이면(GetString) 비우기 전에 복사한다(새 텍스트가 읽기 전 입력을 덮지
    // 않게).
    Array<char> copy(text_.GetAllocator());
    if (Overlaps(input.Data(), input.ByteCount(), text_.Data(), text_.Size())) {
        if (!copy.TryAppend(Span<const char>(input.Data(), input.ByteCount()))) {
            Clear();
            JsonError error;
            error.code = JsonErrorCode::OutOfMemory;
            return error;
        }

        input = StringView(copy.Data(), copy.Size());
    }

    Clear();
    JsonParser parser(*this, input, options);
    return parser.Run();
}

// ---- 값 ----

bool JsonValue::IsValid() const noexcept {
    return document_ != nullptr && document_->generation_ == generation_;
}

void JsonValue::CheckValid(const char* operation) const noexcept {
    if (!IsValid()) {
        platform::Fatal(operation);
    }
}

JsonType JsonValue::Type() const noexcept {
    CheckValid("JsonValue::Type on an invalid or stale value");
    return document_->nodes_[node_].type;
}

bool JsonValue::IsNull() const noexcept {
    return IsValid() && Type() == JsonType::Null;
}

bool JsonValue::IsBool() const noexcept {
    return IsValid() && Type() == JsonType::Bool;
}

bool JsonValue::IsNumber() const noexcept {
    if (!IsValid()) {
        return false;
    }

    const JsonType type = Type();
    return type == JsonType::Integer || type == JsonType::Unsigned || type == JsonType::Double;
}

bool JsonValue::IsString() const noexcept {
    return IsValid() && Type() == JsonType::String;
}

bool JsonValue::IsArray() const noexcept {
    return IsValid() && Type() == JsonType::Array;
}

bool JsonValue::IsObject() const noexcept {
    return IsValid() && Type() == JsonType::Object;
}

bool JsonValue::GetBool() const noexcept {
    if (!IsBool()) {
        platform::Fatal("JsonValue::GetBool on a non-bool, invalid or stale value");
    }

    return document_->nodes_[node_].boolean;
}

Result<int64> JsonValue::TryGetInt64() const noexcept {
    if (IsValid()) {
        const JsonDocument::Node& node = document_->nodes_[node_];
        if (node.type == JsonType::Integer) {
            return Result<int64>::Success(static_cast<int64>(node.bits));
        }
    }

    return Result<int64>::Failure(ErrorCode::InvalidArgument);
}

Result<uint64> JsonValue::TryGetUInt64() const noexcept {
    if (IsValid()) {
        const JsonDocument::Node& node = document_->nodes_[node_];
        if (node.type == JsonType::Unsigned ||
            (node.type == JsonType::Integer && static_cast<int64>(node.bits) >= 0)) {
            return Result<uint64>::Success(node.bits);
        }
    }

    return Result<uint64>::Failure(ErrorCode::InvalidArgument);
}

Result<double> JsonValue::TryGetDouble() const noexcept {
    if (IsValid()) {
        const JsonDocument::Node& node = document_->nodes_[node_];
        switch (node.type) {
        case JsonType::Integer:
            return Result<double>::Success(static_cast<double>(static_cast<int64>(node.bits)));
        case JsonType::Unsigned:
            return Result<double>::Success(static_cast<double>(node.bits));
        case JsonType::Double:
            return Result<double>::Success(platform::BitCast<double>(node.bits));
        default:
            break;
        }
    }

    return Result<double>::Failure(ErrorCode::InvalidArgument);
}

StringView JsonValue::RawNumber() const noexcept {
    if (!IsNumber()) {
        platform::Fatal("JsonValue::RawNumber on a non-number, invalid or stale value");
    }

    const JsonDocument::Node& node = document_->nodes_[node_];
    return document_->Text(node.first, node.count);
}

StringView JsonValue::GetString() const noexcept {
    if (!IsString()) {
        platform::Fatal("JsonValue::GetString on a non-string, invalid or stale value");
    }

    const JsonDocument::Node& node = document_->nodes_[node_];
    return document_->Text(node.first, node.count);
}

usize JsonValue::Size() const noexcept {
    if (!IsArray() && !IsObject()) {
        return 0;
    }

    return document_->nodes_[node_].count;
}

JsonValue JsonValue::At(usize index) const noexcept {
    if (!IsArray() || index >= Size()) {
        platform::Fatal("JsonValue::At outside an array");
    }

    return JsonValue(document_, document_->elements_[document_->nodes_[node_].first + index],
                     generation_);
}

StringView JsonValue::KeyAt(usize index) const noexcept {
    if (!IsObject() || index >= Size()) {
        platform::Fatal("JsonValue::KeyAt outside an object");
    }

    const JsonDocument::Member& member =
        document_->members_[document_->nodes_[node_].first + index];
    return document_->Text(member.keyOffset, member.keyLength);
}

JsonValue JsonValue::ValueAt(usize index) const noexcept {
    if (!IsObject() || index >= Size()) {
        platform::Fatal("JsonValue::ValueAt outside an object");
    }

    return JsonValue(document_, document_->members_[document_->nodes_[node_].first + index].value,
                     generation_);
}

JsonValue JsonValue::Find(StringView key) const noexcept {
    if (!IsObject()) {
        return JsonValue();
    }

    const uint32* member = document_->index_.Find(JsonDocument::MemberQuery{node_, key});
    return member == nullptr
               ? JsonValue()
               : JsonValue(document_, document_->members_[*member].value, generation_);
}

namespace {

// 숫자 노드의 정확한 값: 부호와 크기(정수) 또는 "정수가 아니거나 2^64 이상".
struct ExactInteger {
    bool integral; // 2^64 미만의 정수인가
    bool negative;
    uint64 magnitude;
};

// double 비트를 정수 연산으로 풀어 정확한 정수인지 본다(부동소수점 환경과 무관).
ExactInteger DoubleAsInteger(uint64 bits) noexcept {
    const bool negative = (bits >> 63) != 0;
    const uint32 biased = static_cast<uint32>((bits >> 52) & 0x7FF);
    const uint64 fraction = bits & 0x000FFFFFFFFFFFFFull;
    if (biased == 0 && fraction == 0) {
        return {true, false, 0}; // ±0은 0
    }

    if (biased == 0) {
        return {false, negative, 0}; // 비정규 수는 0과 1 사이
    }

    const uint64 significand = fraction | (uint64{1} << 52);
    const int32 shift = static_cast<int32>(biased) - 1075; // 값 = significand·2^shift
    if (shift >= 0) {
        if (shift > 11) {
            return {false, negative, 0}; // 2^64 이상
        }

        return {true, negative, significand << shift};
    }

    if (shift <= -64) {
        return {false, negative, 0};
    }

    const uint64 dropped = significand & ((uint64{1} << -shift) - 1u);
    return {dropped == 0, negative, significand >> -shift};
}

ExactInteger IntegerOf(JsonType type, uint64 bits) noexcept {
    if (type == JsonType::Unsigned) {
        return {true, false, bits};
    }

    const int64 value = static_cast<int64>(bits);
    return value < 0 ? ExactInteger{true, true, static_cast<uint64>(-(value + 1)) + 1u}
                     : ExactInteger{true, false, bits};
}

// 두 숫자 노드의 수치 비교(정수 연산만). double끼리는 비트가 같거나 둘 다 ±0이면 같다(NaN·Inf는
// 없다).
bool SameNumber(JsonType leftType, uint64 leftBits, JsonType rightType, uint64 rightBits) noexcept {
    const bool leftDouble = leftType == JsonType::Double;
    const bool rightDouble = rightType == JsonType::Double;
    if (leftDouble && rightDouble) {
        return leftBits == rightBits || ((leftBits | rightBits) << 1) == 0;
    }

    const ExactInteger left =
        leftDouble ? DoubleAsInteger(leftBits) : IntegerOf(leftType, leftBits);
    const ExactInteger right =
        rightDouble ? DoubleAsInteger(rightBits) : IntegerOf(rightType, rightBits);
    if (!left.integral || !right.integral) {
        return false;
    }

    return left.magnitude == right.magnitude &&
           (left.negative == right.negative || left.magnitude == 0);
}

bool IsNumberType(JsonType type) noexcept {
    return type == JsonType::Integer || type == JsonType::Unsigned || type == JsonType::Double;
}

} // namespace

bool JsonEquals(JsonValue left, JsonValue right) noexcept {
    if (!left.IsValid() || !right.IsValid()) {
        return left.IsValid() == right.IsValid();
    }

    // 서브트리는 노드 배열의 연속 구간(전위 순서)이다. 컨테이너의 원소·멤버 수까지 같은 두 전위
    // 열은 같은 트리이므로 두 구간을 나란히 한 번 훑으면 된다(재귀 없음).
    const JsonDocument& leftDocument = *left.document_;
    const JsonDocument& rightDocument = *right.document_;
    uint32 a = left.node_;
    uint32 b = right.node_;
    const uint32 leftEnd = leftDocument.nodes_[a].end;
    if (leftEnd - a != rightDocument.nodes_[b].end - b) {
        return false;
    }

    for (; a < leftEnd; ++a, ++b) {
        const JsonDocument::Node& x = leftDocument.nodes_[a];
        const JsonDocument::Node& y = rightDocument.nodes_[b];
        if (IsNumberType(x.type) && IsNumberType(y.type)) {
            if (!SameNumber(x.type, x.bits, y.type, y.bits)) {
                return false;
            }

            continue;
        }

        if (x.type != y.type) {
            return false;
        }

        switch (x.type) {
        case JsonType::Bool:
            if (x.boolean != y.boolean) {
                return false;
            }

            break;
        case JsonType::String:
            if (leftDocument.Text(x.first, x.count) != rightDocument.Text(y.first, y.count)) {
                return false;
            }

            break;
        case JsonType::Array:
            if (x.count != y.count) {
                return false;
            }

            break;
        case JsonType::Object:
            if (x.count != y.count) {
                return false;
            }

            for (uint32 index = 0; index < x.count; ++index) {
                const JsonDocument::Member& p = leftDocument.members_[x.first + index];
                const JsonDocument::Member& q = rightDocument.members_[y.first + index];
                if (leftDocument.Text(p.keyOffset, p.keyLength) !=
                    rightDocument.Text(q.keyOffset, q.keyLength)) {
                    return false;
                }
            }

            break;
        default:
            break; // Null
        }
    }

    return true;
}

// ---- 쓰기 ----

uint64 JsonWriter::WrittenKeyHash::operator()(const WrittenKey& key) const noexcept {
    return HashKeyBytes(key.object, out->View().Data() + key.offset, key.length);
}

bool JsonWriter::WrittenKeyEqual::operator()(const WrittenKey& left,
                                             const WrittenKey& right) const noexcept {
    const char* data = out->View().Data();
    return left.object == right.object && left.length == right.length &&
           platform::MemoryCompare(data + left.offset, data + right.offset, left.length) == 0;
}

JsonWriter::JsonWriter(StringBuilderBase& out, JsonNumberMode mode, uint32 maxDepth) noexcept
    : out_(&out), mode_(mode), maxDepth_(maxDepth), frames_(out.GetAllocator()),
      keys_(out.GetAllocator(), WrittenKeyHash{&out}, WrittenKeyEqual{&out}) {
}

Result<void> JsonWriter::Fail(ErrorCode code) noexcept {
    failed_ = true;
    return Result<void>::Failure(code);
}

Result<void> JsonWriter::Check(Result<void> result) noexcept {
    if (!result) {
        failed_ = true;
    }

    return result;
}

Result<void> JsonWriter::WriteIndent(usize depth) noexcept {
    if (auto newline = Check(out_->TryAppendAscii('\n')); !newline) {
        return newline;
    }

    return Check(out_->TryAppendRepeated(' ', depth * 2));
}

// 값 앞의 구분자(쉼표·줄바꿈·들여쓰기)를 쓴다.
Result<void> JsonWriter::BeforeValue() noexcept {
    if (failed_ || finished_) {
        return Fail(ErrorCode::InvalidArgument);
    }

    if (frames_.IsEmpty()) {
        return rootWritten_ ? Fail(ErrorCode::InvalidArgument) : Result<void>::Success();
    }

    Frame& top = frames_[frames_.Size() - 1];
    if (top.object) {
        // 객체: Key 뒤에만 값이 온다(구분자는 Key가 썼다).
        if (!expectingValue_) {
            return Fail(ErrorCode::InvalidArgument);
        }

        expectingValue_ = false;
        return Result<void>::Success();
    }

    const bool first = !top.hasItems;
    top.hasItems = true;
    if (!first) {
        if (auto comma = Check(out_->TryAppendAscii(',')); !comma) {
            return comma;
        }
    }

    return WriteIndent(frames_.Size());
}

Result<void> JsonWriter::AfterValue() noexcept {
    if (frames_.IsEmpty()) {
        rootWritten_ = true;
    }

    return Result<void>::Success();
}

Result<void> JsonWriter::WriteEscaped(StringView text) noexcept {
    // 입력이 out의 View()이면 쓰는 동안 주소가 바뀔 수 있으므로 먼저 복사한다.
    Array<char> copy(out_->GetAllocator());
    if (Overlaps(text.Data(), text.ByteCount(), out_->View().Data(), out_->ByteCount())) {
        if (!copy.TryAppend(Span<const char>(text.Data(), text.ByteCount()))) {
            return Fail(ErrorCode::OutOfMemory);
        }

        text = StringView(copy.Data(), copy.Size());
    }

    if (!text.ValidateUtf8()) {
        return Fail(ErrorCode::InvalidArgument);
    }

    if (auto quote = Check(out_->TryAppendAscii('"')); !quote) {
        return quote;
    }

    const char* data = text.Data();
    usize start = 0;
    for (usize index = 0; index <= text.ByteCount(); ++index) {
        const uint8 byte = index < text.ByteCount() ? static_cast<uint8>(data[index]) : 0;
        const bool needsEscape =
            index < text.ByteCount() && (byte == '"' || byte == '\\' || byte < 0x20);
        if (!needsEscape && index < text.ByteCount()) {
            continue;
        }

        if (index > start) {
            if (auto run = Check(out_->TryAppend(StringView(data + start, index - start))); !run) {
                return run;
            }
        }

        if (index == text.ByteCount()) {
            break;
        }

        char escape[6] = {'\\', 0, 0, 0, 0, 0};
        usize length = 2;
        switch (byte) {
        case '"':
            escape[1] = '"';
            break;
        case '\\':
            escape[1] = '\\';
            break;
        case '\b':
            escape[1] = 'b';
            break;
        case '\f':
            escape[1] = 'f';
            break;
        case '\n':
            escape[1] = 'n';
            break;
        case '\r':
            escape[1] = 'r';
            break;
        case '\t':
            escape[1] = 't';
            break;
        default: {
            constexpr char Hex[] = "0123456789abcdef";
            escape[1] = 'u';
            escape[2] = '0';
            escape[3] = '0';
            escape[4] = Hex[byte >> 4];
            escape[5] = Hex[byte & 0xF];
            length = 6;
            break;
        }
        }

        if (auto written = Check(out_->TryAppend(StringView(escape, length))); !written) {
            return written;
        }

        start = index + 1;
    }

    return Check(out_->TryAppendAscii('"'));
}

Result<void> JsonWriter::BeginContainer(bool object) noexcept {
    if (auto before = BeforeValue(); !before) {
        return before;
    }

    if (frames_.Size() >= maxDepth_) {
        return Fail(ErrorCode::CapacityExceeded);
    }

    if (auto open = Check(out_->TryAppendAscii(object ? '{' : '[')); !open) {
        return open;
    }

    if (!frames_.TryPushBack(Frame{object, false, object ? nextSerial_++ : 0})) {
        return Fail(ErrorCode::OutOfMemory);
    }

    return Result<void>::Success();
}

Result<void> JsonWriter::EndContainer(bool object) noexcept {
    if (failed_ || finished_ || frames_.IsEmpty() || frames_[frames_.Size() - 1].object != object ||
        expectingValue_) {
        return Fail(ErrorCode::InvalidArgument);
    }

    const bool hasItems = frames_[frames_.Size() - 1].hasItems;
    static_cast<void>(frames_.TryResize(frames_.Size() - 1)); // 줄이기는 할당하지 않는다
    if (hasItems) {
        if (auto indent = WriteIndent(frames_.Size()); !indent) {
            return indent;
        }
    }

    if (auto close = Check(out_->TryAppendAscii(object ? '}' : ']')); !close) {
        return close;
    }

    return AfterValue();
}

Result<void> JsonWriter::BeginObject() noexcept {
    return BeginContainer(true);
}

Result<void> JsonWriter::BeginArray() noexcept {
    return BeginContainer(false);
}

Result<void> JsonWriter::EndObject() noexcept {
    return EndContainer(true);
}

Result<void> JsonWriter::EndArray() noexcept {
    return EndContainer(false);
}

Result<void> JsonWriter::Key(StringView key) noexcept {
    if (failed_ || finished_ || frames_.IsEmpty() || !frames_[frames_.Size() - 1].object ||
        expectingValue_) {
        return Fail(ErrorCode::InvalidArgument);
    }

    Frame& top = frames_[frames_.Size() - 1];
    const bool first = !top.hasItems;
    const uint32 serial = top.serial;
    top.hasItems = true;
    if (!first) {
        if (auto comma = Check(out_->TryAppendAscii(',')); !comma) {
            return comma;
        }
    }

    if (auto indent = WriteIndent(frames_.Size()); !indent) {
        return indent;
    }

    const usize start = out_->ByteCount();
    if (auto written = WriteEscaped(key); !written) {
        return written;
    }

    // 같은 객체의 같은 키는 자기 reader가 거부하므로 쓰지 않는다(이스케이프가 결정적이라
    // 이스케이프된 byte로 비교한다).
    auto inserted =
        keys_.Insert(WrittenKey{serial, start, out_->ByteCount() - start}, static_cast<uint8>(0));
    if (!inserted) {
        return Fail(ErrorCode::OutOfMemory);
    }

    if (inserted.Value() == InsertOutcome::AlreadyPresent) {
        return Fail(ErrorCode::InvalidArgument);
    }

    if (auto separator = Check(out_->TryAppend(StringView(": "))); !separator) {
        return separator;
    }

    expectingValue_ = true;
    return Result<void>::Success();
}

Result<void> JsonWriter::Null() noexcept {
    if (auto before = BeforeValue(); !before) {
        return before;
    }

    if (auto written = Check(out_->TryAppend(StringView("null"))); !written) {
        return written;
    }

    return AfterValue();
}

Result<void> JsonWriter::Bool(bool value) noexcept {
    if (auto before = BeforeValue(); !before) {
        return before;
    }

    if (auto written = Check(out_->TryAppend(StringView(value ? "true" : "false"))); !written) {
        return written;
    }

    return AfterValue();
}

Result<void> JsonWriter::Int64(int64 value) noexcept {
    if (auto before = BeforeValue(); !before) {
        return before;
    }

    if (auto written = Check(out_->TryAppendFormat("{}", value)); !written) {
        return written;
    }

    return AfterValue();
}

Result<void> JsonWriter::UInt64(uint64 value) noexcept {
    if (auto before = BeforeValue(); !before) {
        return before;
    }

    if (auto written = Check(out_->TryAppendFormat("{}", value)); !written) {
        return written;
    }

    return AfterValue();
}

Result<void> JsonWriter::Double(double value) noexcept {
    // NaN·Inf는 JSON에 없다(비트로 판정 — 부동소수점 환경과 무관).
    if ((platform::BitCast<uint64>(value) & 0x7FF0000000000000ull) == 0x7FF0000000000000ull) {
        return Fail(ErrorCode::InvalidArgument);
    }

    if (auto before = BeforeValue(); !before) {
        return before;
    }

    const usize start = out_->ByteCount();
    if (auto written = Check(out_->TryAppendFormat("{}", value)); !written) {
        return written;
    }

    // 최단 표기에 '.'·'e'가 없으면 ".0"을 붙인다(−0도 "-0" → "-0.0"). 다시 읽어도 double이다.
    const StringView text = out_->View();
    bool hasMarker = false;
    for (usize index = start; index < text.ByteCount(); ++index) {
        const char character = text.Data()[index];
        hasMarker = hasMarker || character == '.' || character == 'e' || character == 'E';
    }

    if (!hasMarker) {
        if (auto suffix = Check(out_->TryAppend(StringView(".0"))); !suffix) {
            return suffix;
        }
    }

    return AfterValue();
}

Result<void> JsonWriter::String(StringView value) noexcept {
    if (auto before = BeforeValue(); !before) {
        return before;
    }

    if (auto written = WriteEscaped(value); !written) {
        return written;
    }

    return AfterValue();
}

Result<void> JsonWriter::WriteNumber(JsonValue value) noexcept {
    if (mode_ == JsonNumberMode::Preserve) {
        if (auto before = BeforeValue(); !before) {
            return before;
        }

        if (auto written = Check(out_->TryAppend(value.RawNumber())); !written) {
            return written;
        }

        return AfterValue();
    }

    switch (value.Type()) {
    case JsonType::Integer:
        return Int64(value.TryGetInt64().Value());
    case JsonType::Unsigned:
        return UInt64(value.TryGetUInt64().Value());
    default:
        return Double(value.TryGetDouble().Value());
    }
}

Result<void> JsonWriter::Value(JsonValue value) noexcept {
    if (!value.IsValid()) {
        return Fail(ErrorCode::InvalidArgument);
    }

    // 명시 스택으로 쓴다: 컨테이너를 열면 (컨테이너, 다음 자식 위치)를 쌓는다.
    struct Pending {
        JsonValue container;
        usize next;
    };
    Array<Pending> stack(out_->GetAllocator());
    auto emit = [this, &stack](JsonValue item) noexcept -> Result<void> {
        switch (item.Type()) {
        case JsonType::Null:
            return Null();
        case JsonType::Bool:
            return Bool(item.GetBool());
        case JsonType::Integer:
        case JsonType::Unsigned:
        case JsonType::Double:
            return WriteNumber(item);
        case JsonType::String:
            return String(item.GetString());
        case JsonType::Array:
        case JsonType::Object: {
            if (auto begin = BeginContainer(item.Type() == JsonType::Object); !begin) {
                return begin;
            }

            if (!stack.TryPushBack(Pending{item, 0})) {
                return Fail(ErrorCode::OutOfMemory);
            }

            return Result<void>::Success();
        }
        }

        return Fail(ErrorCode::InvalidArgument);
    };
    if (auto first = emit(value); !first) {
        return first;
    }

    while (!stack.IsEmpty()) {
        Pending& top = stack[stack.Size() - 1];
        const JsonValue container = top.container;
        if (top.next == container.Size()) {
            static_cast<void>(stack.TryResize(stack.Size() - 1));
            if (auto end = EndContainer(container.Type() == JsonType::Object); !end) {
                return end;
            }

            continue;
        }

        const usize index = top.next++;
        JsonValue child;
        if (container.Type() == JsonType::Object) {
            if (auto key = Key(container.KeyAt(index)); !key) {
                return key;
            }

            child = container.ValueAt(index);
        } else {
            child = container.At(index);
        }

        if (auto written = emit(child); !written) {
            return written;
        }
    }

    return Result<void>::Success();
}

Result<void> JsonWriter::Finish() noexcept {
    if (failed_ || finished_ || !frames_.IsEmpty() || !rootWritten_) {
        return Fail(ErrorCode::InvalidArgument);
    }

    if (auto newline = Check(out_->TryAppendAscii('\n')); !newline) {
        return newline;
    }

    finished_ = true;
    return Result<void>::Success();
}

} // namespace rocks
