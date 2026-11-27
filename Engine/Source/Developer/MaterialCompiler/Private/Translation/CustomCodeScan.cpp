#include "CustomCodeScan.h"

// Custom HLSL V13 토큰 검사다(Docs/Design/MaterialSystem 6.13절 MS2f5).
namespace rocks::material::detail {

namespace {

constexpr StringView CustomForbiddenWords[CustomForbiddenWordCount] = {
    "register", "cbuffer", "tbuffer", "groupshared", "static", "_Pragma", "__pragma"};

bool IsScanIdentifierStart(char character) noexcept {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           character == '_';
}

bool IsScanIdentifierPart(char character) noexcept {
    return IsScanIdentifierStart(character) || (character >= '0' && character <= '9');
}

bool IsScanSpace(char character) noexcept {
    return character == ' ' || character == '\t' || character == '\n' || character == '\r' ||
           character == '\v' || character == '\f';
}

} // namespace

StringView CustomForbiddenWord(usize index) noexcept {
    return index < CustomForbiddenWordCount ? CustomForbiddenWords[index] : StringView();
}

bool IsCustomIdentifier(StringView text) noexcept {
    if (text.IsEmpty() || !IsScanIdentifierStart(text.Data()[0])) {
        return false;
    }

    for (usize at = 1; at < text.ByteCount(); ++at) {
        if (!IsScanIdentifierPart(text.Data()[at])) {
            return false;
        }
    }

    return true;
}

void ScanCustomCode(StringView code, CustomCodeScan& scan) noexcept {
    const char* text = code.Data();
    const usize size = code.ByteCount();
    for (usize at = 0; at + 1 < size; ++at) {
        if (text[at] == '\\' && (text[at + 1] == '\n' ||
                                 (text[at + 1] == '\r' && at + 2 < size && text[at + 2] == '\n'))) {
            scan.lineContinuation = true;
        }
    }

    int64 depth = 0;
    // # 를 본 뒤 지시 이름(공백 다음 식별자)을 기다리는가.
    bool afterHash = false;
    // 식별자 clip 뒤 다음 토큰을 기다리는가(공백·주석은 건너뛴다).
    bool afterClip = false;
    usize at = 0;
    while (at < size) {
        const char character = text[at];
        if (character == '/' && at + 1 < size && text[at + 1] == '/') {
            scan.comment = true;
            while (at < size && text[at] != '\n') {
                ++at;
            }

            continue;
        }

        if (character == '/' && at + 1 < size && text[at + 1] == '*') {
            scan.comment = true;
            at += 2;
            while (at + 1 < size && !(text[at] == '*' && text[at + 1] == '/')) {
                ++at;
            }

            if (at + 1 >= size) {
                scan.unterminatedComment = true;
                at = size;
            } else {
                at += 2;
            }

            continue;
        }

        if (IsScanSpace(character)) {
            ++at;
            continue;
        }

        if (character == '"') {
            afterHash = false;
            afterClip = false;
            ++at;
            bool closed = false;
            while (at < size) {
                if (text[at] == '\\' && at + 1 < size) {
                    at += 2;
                    continue;
                }

                if (text[at] == '\n') {
                    break;
                }

                if (text[at] == '"') {
                    closed = true;
                    ++at;
                    break;
                }

                ++at;
            }

            scan.unterminatedString = scan.unterminatedString || !closed;
            continue;
        }

        if (IsScanIdentifierStart(character)) {
            const usize start = at;
            while (at < size && IsScanIdentifierPart(text[at])) {
                ++at;
            }

            const StringView word(text + start, at - start);
            if (afterHash && scan.directiveName.IsEmpty()) {
                scan.directiveName = word;
            }

            for (usize index = 0; index < CustomForbiddenWordCount; ++index) {
                scan.words[index] = scan.words[index] || word == CustomForbiddenWords[index];
            }

            scan.discardWord = scan.discardWord || word == StringView("discard");
            afterHash = false;
            afterClip = word == StringView("clip");
            continue;
        }

        scan.clipCall = scan.clipCall || (afterClip && character == '(');
        afterClip = false;
        afterHash = false;
        if (character == '#') {
            // 첫 # 만 이름을 기록한다(## 연산자·지시 모두 막는다).
            afterHash = !scan.directive;
            scan.directive = true;
        } else if (character == '{') {
            ++depth;
        } else if (character == '}') {
            --depth;
            scan.closesBody = scan.closesBody || depth < 0;
        }

        ++at;
    }

    scan.leavesOpen = depth > 0;
}

} // namespace rocks::material::detail
