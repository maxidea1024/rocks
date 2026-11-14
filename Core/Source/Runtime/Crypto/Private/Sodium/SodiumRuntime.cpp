#include "SodiumRuntime.h"

#include <Crypto/Crypto.h>
#include <Platform/Fatal.h>
#include <Platform/SecureRandom.h>

#include <sodium.h>

// libsodium이 내부에서 쓰는 난수를 Rocks OS 난수로 바꾸는 구현이다. randombytes_implementation은 C 함수 포인터를 받으므로
// C 언어 연결로 정의한다. libsodium은 random·implementation_name을 NULL 검사 없이 부르므로 모든 함수를 채운다. sodium_init
// 안에서 stir·buf가 불리므로 이 함수들은 Crypto 초기화에 의존하지 않는다(재귀 방지).
extern "C" {

static const char* RocksSodiumRandomName(void) {
    return "rocks-os";
}

static void RocksSodiumRandomBuffer(void* const buffer, const size_t size) {
    // randombytes_buf는 실패를 돌려줄 수 없다. 난수 없이 키·nonce를 만들 수 없으므로 멈춘다.
    if (size != 0 && !rocks::platform::TryFillSecureRandom(buffer, size)) {
        rocks::platform::Fatal("Crypto: the OS secure random generator failed inside libsodium");
    }
}

static uint32_t RocksSodiumRandom(void) {
    uint32_t value = 0;
    RocksSodiumRandomBuffer(&value, sizeof(value));
    return value;
}

static void RocksSodiumRandomStir(void) {
    // OS 난수를 매번 직접 읽으므로 다시 섞을 상태가 없다.
}

// [0, upperBound) 균등 분포다. 2^32 mod upperBound보다 작은 값을 버려 나머지 편향을 없앤다(libsodium 기본 구현과 같은 방식).
static uint32_t RocksSodiumRandomUniform(const uint32_t upperBound) {
    if (upperBound < 2) {
        return 0;
    }

    const uint32_t minimum = (0u - upperBound) % upperBound;
    uint32_t value = 0;
    do {
        value = RocksSodiumRandom();
    } while (value < minimum);
    return value % upperBound;
}

static int RocksSodiumRandomClose(void) {
    return 0;
}

// misuse handler는 libsodium API 계약 위반(어댑터가 막았어야 하는 길이·인자)에서 불린다. 원격 입력으로 여기에 닿으면
// 어댑터 결함이다(Docs/Design/Crypto.ko.md 5.1절 "misuse handler 도달 금지").
static void RocksSodiumMisuse(void) {
    rocks::platform::Fatal("Crypto: libsodium misuse (an adapter precondition was not enforced)");
}

static randombytes_implementation RocksSodiumRandomImplementation = {
    &RocksSodiumRandomName, &RocksSodiumRandom, &RocksSodiumRandomStir,
    &RocksSodiumRandomUniform, &RocksSodiumRandomBuffer, &RocksSodiumRandomClose,
};

} // extern "C"

namespace rocks::crypto {
namespace {

CryptoRuntimeInfo Initialize() noexcept {
    // 순서가 중요하다: 난수 교체는 sodium_init 전에 한 번만 할 수 있다.
    if (randombytes_set_implementation(&RocksSodiumRandomImplementation) != 0) {
        platform::Fatal("Crypto: randombytes_set_implementation failed");
    }

    if (sodium_init() < 0) {
        platform::Fatal("Crypto: sodium_init failed");
    }

    if (sodium_set_misuse_handler(&RocksSodiumMisuse) != 0) {
        platform::Fatal("Crypto: sodium_set_misuse_handler failed");
    }

    CryptoRuntimeInfo info;
    info.sodiumVersion = StringView(sodium_version_string());
    info.randomImplementation = StringView(randombytes_implementation_name());
    info.sodiumAesni = sodium_runtime_has_aesni() != 0;
    info.sodiumPclmul = sodium_runtime_has_pclmul() != 0;
    info.sodiumAvx = sodium_runtime_has_avx() != 0;
    info.sodiumAvx2 = sodium_runtime_has_avx2() != 0;
    info.sodiumArmCrypto = sodium_runtime_has_armcrypto() != 0;
    return info;
}

} // namespace

const CryptoRuntimeInfo& GetCryptoRuntimeInfo() noexcept {
    // 함수 안 static 초기화는 여러 스레드가 동시에 와도 한 번만 실행되고, 나머지는 끝날 때까지 기다린다.
    static const CryptoRuntimeInfo info = Initialize();
    return info;
}

namespace detail {

void EnsureSodium() noexcept {
    (void)GetCryptoRuntimeInfo();
}

} // namespace detail
} // namespace rocks::crypto
