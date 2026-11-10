#include <Core/Assert.h>
#include <Platform/Fatal.h>

namespace rocks {
[[noreturn]] void AssertFailed(const char* expression, const char* file, uint32 line) noexcept {
    // 할당기나 로그 시스템 자체가 깨진 상황에서도 보고할 수 있도록
    // 고정 크기 스택 버퍼와 플랫폼 Fatal만 사용한다. 긴 메시지는 끝을 잘라 종료 문자를 유지한다.
    char buffer[2048]{};
    usize used = 0;
    auto append = [&](const char* text) noexcept {
        if (text == nullptr) {
            return;
        }
        while (*text != '\0' && used + 1 < sizeof(buffer)) {
            buffer[used++] = *text++;
        }
    };
    append("Rocks invariant failed: ");
    append(expression);
    append(" (");
    append(file);
    append(":");
    char digits[11]{};
    usize count = 0;
    do {
        digits[count++] = static_cast<char>('0' + line % 10);
        line /= 10;
    } while (line != 0);
    while (count != 0 && used + 1 < sizeof(buffer)) {
        buffer[used++] = digits[--count];
    }
    append(")");
    buffer[used] = '\0';
    platform::Fatal(buffer);
}
} // namespace rocks
