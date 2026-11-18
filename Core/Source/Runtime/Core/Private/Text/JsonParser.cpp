#include <Core/Text/Json.h>

#include "JsonInternal.h"
#include "JsonParser.h"
#include <Core/Text/NumberText.h>
#include <Platform/Memory.h>

namespace rocks {

using detail::Overlaps;

namespace {

// 입력 상한: 노드·문자열 위치가 uint32다. 텍스트(이스케이프를 푼 문자열 + 숫자 토큰)는 엄격
// 문법에서 입력을 넘지 않고, Json5의 숫자 정규화(".5" → "0.5", 16진수 → 10진수)도 토큰
// 길이의 1.5배를 넘지 않으므로 텍스트는 1.5 GiB 아래다(uint32 위치로 충분하다).
constexpr uint64 HardInputLimit = 1ull << 30;

} // namespace

// ---- 파서 ----

JsonPosition JsonParser::PositionOf(usize offset) const noexcept {
    // 오류 때만 앞에서부터 센다. 건너뛴 BOM은 세지 않는다.
    JsonPosition position;
    usize index = bomLength_;
    Advance(position, index, offset);
    return position;
}

// position(입력 index 앞까지 센 위치)을 offset 앞까지 진행한다. CRLF는 줄바꿈 1개, CR 단독도
// 줄바꿈, column은 code point(UTF-8 이어지는 byte는 세지 않는다)다. U+2028·U+2029는 Json5 문법에서
// 줄 끝(주석 끝·줄 이음)이어도 위치 계산에서는 줄바꿈이 아니다(편집기·LSP의 줄 번호와 맞춘다 — C#
// Json5Reader도 같다).
void JsonParser::Advance(JsonPosition& position, usize& index, usize offset) const noexcept {
    position.byteOffset = offset;
    for (; index < offset && index < size_; ++index) {
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
}

// 성공한 문서의 노드 위치에 line·column을 채운다. 노드는 문서 순서(전위)라 시작 위치가 줄지
// 않으므로 입력을 한 번만 훑는다(입력 + 노드 수에 비례).
void JsonParser::FillPositions() noexcept {
    JsonPosition position;
    usize index = bomLength_;
    for (usize node = 0; node < document_.positions_.Size(); ++node) {
        JsonDocument::NodePosition& entry = document_.positions_[node];
        Advance(position, index, entry.byteOffset);
        entry.line = position.line;
        entry.column = position.column;
    }
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

// data[position]에서 시작하는 UTF-8 한 글자의 길이(잘못되면 0)다(detail::DecodeUtf8 참고).
usize Utf8Length(const uint8* data, usize size, usize position) noexcept {
    uint32 codePoint = 0;
    return detail::DecodeUtf8(data, size, position, codePoint);
}

} // namespace

usize detail::DecodeUtf8(const uint8* data, usize size, usize position,
                         uint32& codePoint) noexcept {
    const uint8 lead = data[position];
    usize length = 0;
    uint32 minimum = 0;
    uint32 value = 0;
    if (lead >= 0xC2 && lead <= 0xDF) {
        length = 2;
        minimum = 0x80;
        value = lead & 0x1F;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        length = 3;
        minimum = 0x800;
        value = lead & 0x0F;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        length = 4;
        minimum = 0x10000;
        value = lead & 0x07;
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

        value = (value << 6) | (next & 0x3F);
    }

    if (value < minimum || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
        return 0;
    }

    codePoint = value;
    return length;
}

void JsonParser::SkipWhitespace() noexcept {
    if (json5_) {
        SkipJson5Whitespace();
        return;
    }

    while (position_ < size_) {
        const uint8 byte = data_[position_];
        if (byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r') {
            ++position_;
            continue;
        }

        const bool comment = options_.allowLineComments && byte == '/' && position_ + 1 < size_ &&
                             data_[position_ + 1] == '/';
        if (!comment) {
            return;
        }

        // 줄 끝(LF·CR) 앞까지 건너뛴다. 줄바꿈은 다음 반복이 공백으로 소비한다(위치 계산이 줄을
        // 센다).
        position_ += 2;
        while (position_ < size_ && data_[position_] != '\n' && data_[position_] != '\r') {
            if (data_[position_] < 0x80) {
                ++position_;
                continue;
            }

            const usize count = Utf8Length(data_, size_, position_);
            if (count == 0) {
                static_cast<void>(Fail(JsonErrorCode::InvalidUtf8, position_));
                return;
            }

            position_ += count;
        }
    }
}

// \u 뒤 16진수 4자리(position_은 첫 자리). 잘못된 자리가 있으면 그 이스케이프(escape) 위치에서
// InvalidEscape, 모두 16진수인데 입력이 먼저 끝났으면 UnexpectedEnd다.
bool JsonParser::ReadHex(usize escape, uint32& unit) noexcept {
    unit = 0;
    for (usize index = 0; index < 4; ++index) {
        if (position_ + index >= size_) {
            return Fail(JsonErrorCode::UnexpectedEnd, size_);
        }

        const int digit = detail::HexDigitValue(data_[position_ + index]);
        if (digit < 0) {
            return Fail(JsonErrorCode::InvalidEscape, escape);
        }

        unit = (unit << 4) | static_cast<uint32>(digit);
    }

    position_ += 4;
    return true;
}

bool JsonParser::ReadUnicodeEscape(usize escape, uint32& codePoint) noexcept {
    uint32 unit = 0;
    if (!ReadHex(escape, unit)) {
        return false;
    }

    if (unit >= 0xDC00 && unit <= 0xDFFF) {
        return Fail(JsonErrorCode::UnpairedSurrogate, escape);
    }

    if (unit >= 0xD800 && unit <= 0xDBFF) {
        // 바로 뒤에 낮은 surrogate 이스케이프가 와야 한다. 그 전에 입력이 끝나면 문자열이 끝나지
        // 않은 것이다.
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

    codePoint = unit;
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
        if (!ReadUnicodeEscape(escape, unit)) {
            return false;
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
    const usize textStart = document_.text_.Size();
    if (!AppendText(data_ + start, index - start)) {
        return false;
    }

    return AddNumberNode(textStart, integer, start, node);
}

bool JsonParser::AddNumberNode(usize textStart, bool integer, usize start, uint32& node) noexcept {
    const StringView token(document_.text_.Data() + textStart, document_.text_.Size() - textStart);
    Node value{};
    value.first = static_cast<uint32>(textStart);
    value.count = static_cast<uint32>(token.ByteCount());
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
    if (json5_) {
        return ParseJson5Value(node);
    }

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

// 객체 안에서 키와 ':'까지 읽는다(position_은 공백 뒤). 키는 문자열이고, Json5면 작은따옴표
// 문자열과 식별자 이름도 받는다.
bool JsonParser::BeginKey() noexcept {
    if (position_ >= size_) {
        return Fail(JsonErrorCode::UnexpectedEnd, position_);
    }

    const usize keyStart = position_;
    Member member{};
    if (json5_) {
        const uint8 quote = data_[position_];
        const bool parsed = quote == '"' || quote == '\''
                                ? ParseJson5String(member.keyOffset, member.keyLength)
                                : ParseJson5Identifier(member.keyOffset, member.keyLength);
        if (!parsed) {
            return false;
        }
    } else {
        if (data_[position_] != '"') {
            return Fail(JsonErrorCode::UnexpectedCharacter, position_);
        }

        if (!ParseString(member.keyOffset, member.keyLength)) {
            return false;
        }
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
            // Json5는 원소·멤버 뒤 끝 쉼표 하나를 받는다(빈 컨테이너의 쉼표·연속 쉼표는 다음 값
            // 해석이 거부한다).
            if (json5_ && position_ < size_ && data_[position_] == (frame.object ? '}' : ']')) {
                ++position_;
                value = frame.node;
                ok = CloseContainer();
                continue;
            }

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

    // 주석 안의 잘못된 UTF-8(SkipWhitespace)은 그 자리에서 해석을 멈추지 않고 기록만 하므로, 기록된
    // 오류가 있으면 실패다.
    if (ok && errorCode_ != JsonErrorCode::None) {
        ok = false;
    }

    if (ok) {
        FillPositions();
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

} // namespace rocks
