#include <Tests/Support/HostFailure.h>
#include <stdio.h>

namespace rocks::test {

FILE* OpenReportFile(const char* path) noexcept {
    if (path == nullptr) {
        return nullptr;
    }
    return fopen(path, "wb");
}

void PrepareFatalTestProcess() noexcept {
}

void LockStandardErrorForTest() noexcept {
    flockfile(stderr);
}

void UnlockStandardErrorForTest() noexcept {
    funlockfile(stderr);
}

} // namespace rocks::test
