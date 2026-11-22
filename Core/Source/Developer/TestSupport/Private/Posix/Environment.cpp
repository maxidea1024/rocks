#include <TestSupport/Environment.h>

#include <stdlib.h>
#include <string.h>

namespace rocks::test {

bool ReadEnvironmentVariable(const char* name, StringBuilderBase& out) noexcept {
    const char* value = getenv(name);
    if (value == nullptr) {
        return false;
    }

    return static_cast<bool>(out.TryAppend(StringView(value, strlen(value))));
}

} // namespace rocks::test
