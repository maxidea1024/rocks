#include <Platform/SecureRandom.h>

#include <stdlib.h>

namespace rocks::platform {

bool TryFillSecureRandom(void* buffer, usize bytes) noexcept {
    if (bytes == 0) {
        return true;
    }

    if (buffer == nullptr) {
        return false;
    }

    // macOS·bionic의 arc4random_buf는 커널 엔트로피를 쓰며 실패하지 않는다.
    arc4random_buf(buffer, bytes);
    return true;
}

} // namespace rocks::platform
