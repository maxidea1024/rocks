#include <Core/Build/EngineVersion.h>

namespace rocks {
namespace {

// value의 10진 표기(앞자리 0 없음)를 out[position]부터 쓰고 다음 위치를 돌려준다.
// uint16은 최대 5자다.
usize AppendDecimal(char* out, usize position, uint16 value) noexcept {
    char digits[5];
    usize count = 0;
    uint32 rest = value;
    do {
        digits[count] = static_cast<char>('0' + rest % 10u);
        ++count;
        rest /= 10u;
    } while (rest != 0);

    while (count != 0) {
        --count;
        out[position] = digits[count];
        ++position;
    }

    return position;
}

} // namespace

usize EngineVersion::ToChars(char (&out)[TextBytes]) const noexcept {
    usize length = AppendDecimal(out, 0, majorVersion);
    out[length] = '.';
    length = AppendDecimal(out, length + 1, minorVersion);
    out[length] = '.';
    length = AppendDecimal(out, length + 1, patchVersion);
    out[length] = '\0';
    return length;
}

} // namespace rocks
