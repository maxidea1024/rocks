#pragma once

#include <Core/Result.h>
#include <Core/Text/StringBuilder.h>

// 생성 원문의 float 리터럴 표기다(Docs/Design/MaterialSystem 7.7절 "리터럴"). 최단 왕복
// 표기(StringBuilder "{}" — C runtime·locale 무관)에 숫자·부호만이면 ".0"을 붙인다(5 → 5.0, −0 →
// -0.0, 과학 표기 1e+20은 그대로). 생성 원문(Material.rsh)과 문맥 원문(MaterialContext.rsh)이 함께
// 쓴다.
namespace rocks::material::detail {

// value의 표기를 out 끝에 붙인다. 오류: 할당 실패.
inline Result<void> AppendHlslFloatLiteral(StringBuilder<>& out, float value) noexcept {
    StringBuilder<> number(out.GetAllocator());
    number << value;
    if (auto status = number.Status(); !status) {
        return status;
    }

    const StringView digits = number.View();
    bool integral = true;
    for (usize index = 0; index < digits.ByteCount(); ++index) {
        const char c = digits.Data()[index];
        integral = integral && ((c >= '0' && c <= '9') || c == '-');
    }

    out << digits;
    if (integral) {
        out << ".0";
    }

    return out.Status();
}

} // namespace rocks::material::detail
