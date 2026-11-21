#pragma once

#include <Core/Text/StringView.h>

// 문자열 리터럴 원문의 escape를 푸는 읽개다(Docs/Design/SparksLanguage.ko.md 3.6절). switch의
// 문자열 비교(CheckStatements.cpp)와 bytecode 문자열 표(LC4b)가 쓴다.

namespace rocks::sparks::detail {

// 문자열 리터럴 원문(`"..."`·`@"..."`)을 해석한 byte를 하나씩 내는 읽개다(3.10절 switch — escape를
// 푼 값으로 비교한다). 어휘 분석이 원문을 검사했으므로 escape는 올바르다고 본다.
class StringLiteralReader final {
public:
    explicit StringLiteralReader(StringView text) noexcept : text_(text) {
        verbatim_ = text_.ByteCount() >= 2 && text_.Data()[0] == '@';
        at_ = verbatim_ ? 2 : 1;
        end_ = text_.ByteCount() > 0 ? text_.ByteCount() - 1 : 0; // 닫는 따옴표 앞
    }

    // 보간 문자열의 글자 조각(따옴표 없는 원문, escape는 풀지 않은 것)을 읽는다.
    static StringLiteralReader ForPiece(StringView piece) noexcept {
        StringLiteralReader reader(piece);
        reader.verbatim_ = false;
        reader.at_ = 0;
        reader.end_ = piece.ByteCount();
        return reader;
    }

    // 다음 byte를 out에 두고 true, 끝이면 false다.
    bool Next(uint8& out) noexcept {
        if (pendingAt_ < pendingCount_) {
            out = pending_[pendingAt_++];
            return true;
        }

        if (at_ >= end_) {
            return false;
        }

        const char c = text_.Data()[at_];
        if (verbatim_) {
            at_ += c == '"' ? 2 : 1; // `""`는 `"`
            out = static_cast<uint8>(c);
            return true;
        }

        if (c != '\\' || at_ + 1 >= end_) {
            ++at_;
            out = static_cast<uint8>(c);
            return true;
        }

        const char escape = text_.Data()[at_ + 1];
        at_ += 2;
        if (escape != 'u') {
            out = static_cast<uint8>(escape == 'n' ? '\n' : escape == 't' ? '\t' : escape);
            return true;
        }

        // `\u{X}`: 16진수 code point를 UTF-8로.
        uint32 code = 0;
        at_ += 1; // `{`
        while (at_ < end_ && text_.Data()[at_] != '}') {
            const char digit = text_.Data()[at_++];
            code = code * 16 + static_cast<uint32>(digit <= '9'   ? digit - '0'
                                                   : digit <= 'F' ? digit - 'A' + 10
                                                                  : digit - 'a' + 10);
        }

        at_ += 1; // `}`
        pendingAt_ = 0;
        if (code < 0x80) {
            pendingCount_ = 1;
            pending_[0] = static_cast<uint8>(code);
        } else if (code < 0x800) {
            pendingCount_ = 2;
            pending_[0] = static_cast<uint8>(0xC0 | (code >> 6));
            pending_[1] = static_cast<uint8>(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            pendingCount_ = 3;
            pending_[0] = static_cast<uint8>(0xE0 | (code >> 12));
            pending_[1] = static_cast<uint8>(0x80 | ((code >> 6) & 0x3F));
            pending_[2] = static_cast<uint8>(0x80 | (code & 0x3F));
        } else {
            pendingCount_ = 4;
            pending_[0] = static_cast<uint8>(0xF0 | (code >> 18));
            pending_[1] = static_cast<uint8>(0x80 | ((code >> 12) & 0x3F));
            pending_[2] = static_cast<uint8>(0x80 | ((code >> 6) & 0x3F));
            pending_[3] = static_cast<uint8>(0x80 | (code & 0x3F));
        }

        out = pending_[pendingAt_++];
        return true;
    }

private:
    static constexpr usize MaxUtf8Bytes = 4;

    StringView text_;
    bool verbatim_ = false;
    usize at_ = 0;
    usize end_ = 0;
    uint8 pending_[MaxUtf8Bytes] = {};
    usize pendingAt_ = 0;
    usize pendingCount_ = 0;
};

} // namespace rocks::sparks::detail
