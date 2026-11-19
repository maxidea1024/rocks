#pragma once

#include <Core/Text/Json.h>

// JSON 파서 내부 선언이다(공개 API가 아니다). 엄격 문법과 공통 구조는 JsonParser.cpp, JSON5 어휘
// (공백·주석·문자열·식별자 키·숫자·단어)는 Json5Parser.cpp가 구현한다.
namespace rocks {

namespace detail {

// 16진수 한 자리의 값(아니면 −1)이다.
inline int HexDigitValue(uint8 byte) noexcept {
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

// codePoint의 UTF-8 byte 수다.
inline usize Utf8EncodedLength(uint32 codePoint) noexcept {
    if (codePoint < 0x80) {
        return 1;
    }

    if (codePoint < 0x800) {
        return 2;
    }

    return codePoint < 0x10000 ? 3 : 4;
}

// data[position]에서 시작하는 UTF-8 한 글자를 읽어 길이(잘못되면 0)를 돌려준다. overlong·surrogate
// 부호화·U+10FFFF 초과·잘림을 거부한다. 성공하면 codePoint에 값을 쓴다.
usize DecodeUtf8(const uint8* data, usize size, usize position, uint32& codePoint) noexcept;

} // namespace detail

class JsonParser {
public:
    JsonParser(JsonDocument& document, StringView input, const JsonReadOptions& options) noexcept
        : document_(document), data_(reinterpret_cast<const uint8*>(input.Data())),
          size_(input.ByteCount()), options_(options), json5_(options.syntax == JsonSyntax::Json5),
          frames_(document.nodes_.GetAllocator()), pendingElements_(document.nodes_.GetAllocator()),
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

    // 단어(true·Infinity 등) 일치 결과다. Truncated는 입력이 단어 앞부분에서 끝난 경우다.
    enum class WordMatch : uint8 {
        Match,
        Truncated,
        Mismatch
    };

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

    // 공백(과 allowLineComments면 줄 주석, Json5면 JSON5 공백·주석)을 건너뛴다. 주석 안의 잘못된
    // UTF-8은 그 byte에서 첫 오류(InvalidUtf8)로 기록하고 멈춘다 — 위치가 그 byte에 남으므로 다음
    // 토큰 해석도 실패하고, Run은 기록된 첫 오류를 돌려준다.
    void SkipWhitespace() noexcept;

    // 노드를 추가한다. at은 값의 시작 위치(한도 초과 보고용)다. 스칼라는 서브트리 끝이 자기
    // 다음이다.
    bool AddNode(Node node, uint32& index, usize at) noexcept {
        if (document_.nodes_.Size() >= options_.maxNodes) {
            return Fail(JsonErrorCode::TooManyNodes, at);
        }

        index = static_cast<uint32>(document_.nodes_.Size());
        node.end = index + 1;
        // line·column은 성공한 뒤 FillPositions가 한 번에 채운다(at은 입력 상한 1 GiB 아래다).
        return Memory(document_.nodes_.TryPushBack(node)) &&
               Memory(document_.positions_.TryPushBack(
                   JsonDocument::NodePosition{static_cast<uint32>(at), 0, 0}));
    }

    bool ParseValue(uint32& node) noexcept;
    bool ParseString(uint32& offset, uint32& length) noexcept;
    bool ParseNumber(uint32& node) noexcept;
    bool ParseLiteral(uint32& node) noexcept;
    // text_[textStart, 끝)에 붙인 RFC 8259 숫자 토큰을 종류(Integer·Unsigned·Double)로 판정해
    // 노드를 추가한다. integer는 토큰에 소수부·지수가 없다는 뜻이다. start는 값의 입력 위치다.
    bool AddNumberNode(usize textStart, bool integer, usize start, uint32& node) noexcept;
    bool ReadHex(usize escape, uint32& unit) noexcept;
    // \u 이스케이프 하나(높은 surrogate면 바로 뒤의 낮은 surrogate 이스케이프까지)를 읽는다.
    // position_은 'u' 뒤 첫 16진수 자리, escape는 '\\' 위치다.
    bool ReadUnicodeEscape(usize escape, uint32& codePoint) noexcept;
    bool AppendText(const void* bytes, usize count) noexcept;
    bool AppendCodePoint(uint32 codePoint) noexcept;
    bool BeginKey() noexcept;
    bool CloseContainer() noexcept;
    JsonPosition PositionOf(usize offset) const noexcept;
    void Advance(JsonPosition& position, usize& index, usize offset) const noexcept;
    void FillPositions() noexcept;

    // 문자열·키 하나(text_[textStart, 끝))에 adding byte를 더해도 maxStringBytes 안인가.
    bool WithinStringLimit(usize textStart, usize adding) const noexcept {
        return document_.text_.Size() - textStart + adding <= options_.maxStringBytes;
    }

    // JSON5(Json5Parser.cpp).
    void SkipJson5Whitespace() noexcept;
    // "//"·"/*" 뒤(position_)부터 주석 끝까지 건너뛴다. 실패(잘못된 UTF-8, 닫히지 않은 블록 주석)는
    // 기록하고 false다.
    bool SkipJson5Comment(bool block) noexcept;
    bool ParseJson5Value(uint32& node) noexcept;
    bool ParseJson5String(uint32& offset, uint32& length) noexcept;
    // 문자열 안 이스케이프 하나(position_은 '\\')를 풀어 붙인다. stringStart는 여는 따옴표,
    // textStart는 이 문자열의 text_ 시작이다(길이 한도).
    bool ParseJson5Escape(usize stringStart, usize textStart) noexcept;
    bool ParseJson5Identifier(uint32& offset, uint32& length) noexcept;
    bool ParseJson5Number(uint32& node) noexcept;
    bool ParseJson5Word(uint32& node) noexcept;
    // at에서 word(length byte)가 온전한 단어로 오는지(뒤에 식별자 글자가 붙지 않는지) 본다.
    WordMatch MatchWord(usize at, const char* word, usize length) const noexcept;
    // at의 글자가 식별자 시작 글자(또는 '\\')인가 / 식별자 글자(또는 '\\')인가. 입력 끝·잘못된
    // UTF-8은 거짓이다.
    bool IsIdentifierStartAt(usize at) const noexcept;
    bool IsIdentifierContinuation(usize at) const noexcept;

    JsonDocument& document_;
    const uint8* data_;
    usize size_;
    const JsonReadOptions& options_;
    bool json5_;
    usize position_ = 0;
    usize bomLength_ = 0;
    Array<Frame> frames_;
    Array<uint32> pendingElements_;
    Array<Member> pendingMembers_;
    JsonErrorCode errorCode_ = JsonErrorCode::None;
    usize errorOffset_ = 0;
};

} // namespace rocks
