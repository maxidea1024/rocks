#pragma once

#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Core/Result.h>
#include <Core/Text/String.h>
#include <Core/Text/StringView.h>

// 진단 문구를 조각(문자열·10진 수)으로 만든다(Docs/Design/MaterialSystem 6.12절 MS2e2·MS2e3 — UE
// Errorf의 %s·%i 자리).
//
// 사용 예:
//   const DiagnosticTextPart parts[] = {DiagnosticWord("Layer "), DiagnosticNumber(index)};
//   AppendDiagnosticText(message, parts);
namespace rocks::material::detail {

// 10진 수 하나의 최대 자릿수다(uint64).
inline constexpr usize DiagnosticNumberDigits = 20;
// 10진 진법이다.
inline constexpr uint64 DiagnosticNumberBase = 10;

struct DiagnosticTextPart {
    StringView text;
    bool number = false;
    uint64 value = 0;
};

inline DiagnosticTextPart DiagnosticWord(StringView text) noexcept {
    return DiagnosticTextPart{text, false, 0};
}

inline DiagnosticTextPart DiagnosticNumber(uint64 value) noexcept {
    return DiagnosticTextPart{StringView(), true, value};
}

// parts를 text 뒤에 붙인다. 오류: 할당 실패.
inline Result<void> AppendDiagnosticText(String& text,
                                         Span<const DiagnosticTextPart> parts) noexcept {
    for (const DiagnosticTextPart& part : parts) {
        if (!part.number) {
            if (auto appended = text.TryAppend(part.text); !appended) {
                return appended;
            }

            continue;
        }

        char digits[DiagnosticNumberDigits];
        usize length = 0;
        uint64 value = part.value;
        do {
            digits[length++] = static_cast<char>('0' + value % DiagnosticNumberBase);
            value /= DiagnosticNumberBase;
        } while (value != 0 && length < DiagnosticNumberDigits);

        char ordered[DiagnosticNumberDigits];
        for (usize at = 0; at < length; ++at) {
            ordered[at] = digits[length - 1 - at];
        }

        if (auto appended = text.TryAppend(StringView(ordered, length)); !appended) {
            return appended;
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::material::detail
