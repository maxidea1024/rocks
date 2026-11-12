#include <Platform/SecureRandom.h>

#include <Windows.h>
#include <bcrypt.h>

#pragma comment(lib, "bcrypt")

namespace rocks::platform {

bool TryFillSecureRandom(void* buffer, usize bytes) noexcept {
    if (bytes == 0) {
        return true;
    }

    if (buffer == nullptr) {
        return false;
    }

    // BCryptGenRandom은 ULONG 길이를 받으므로 나눠서 채운다.
    auto* cursor = static_cast<unsigned char*>(buffer);
    while (bytes != 0) {
        const ULONG chunk = bytes > 0x40000000u ? 0x40000000u : static_cast<ULONG>(bytes);
        const NTSTATUS status =
            BCryptGenRandom(nullptr, cursor, chunk, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (status < 0) {
            return false;
        }

        cursor += chunk;
        bytes -= chunk;
    }

    return true;
}

} // namespace rocks::platform
