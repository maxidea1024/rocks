#include <fcntl.h>
#include <io.h>
#include <stdio.h>

namespace rocks::test {
bool ConfigureLoggerFailureProbe() noexcept {
    int descriptors[2]{};
    if (_pipe(descriptors, 4096, _O_BINARY) != 0) {
        return false;
    }
    const bool closed = _close(descriptors[0]) == 0;
    const bool redirected = _dup2(descriptors[1], _fileno(stderr)) == 0;
    const bool released = _close(descriptors[1]) == 0;
    clearerr(stderr);
    return closed && redirected && released;
}
} // namespace rocks::test
