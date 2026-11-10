#include <stdio.h>

namespace rocks::test {
bool ConfigureLoggerFailureProbe() noexcept {
    FILE* stream = nullptr;
    return freopen_s(&stream, "NUL", "r", stderr) == 0 && stream != nullptr;
}
} // namespace rocks::test
