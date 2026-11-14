#pragma once

#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <Crypto/Kdf.h>
#include <Crypto/Secret.h>

// 비밀번호 해시(Argon2id, RFC 9106)다(Docs/Design/Crypto.ko.md 4·10.3절). 구현은 libsodium
// crypto_pwhash다.
//
// - 비밀번호에서 키 얻기: TryDeriveKdfKeyFromPassword(KdfKey, 이어서 Kdf.h로 용도별 키를 뽑는다)와
//   TryDerivePasswordSecret(정해진 길이의 비밀). salt는 16 byte이고 비밀번호마다 TryFillRandom으로
//   만든다.
// - 비밀번호 저장: TryHashPassword가 "$argon2id$v=19$m=...,t=...,p=...$salt$hash" 문자열을 만들고,
//   TryVerifyPassword가 확인한다. 문자열에 비용과 salt가 들어 있으므로 따로 저장하지 않는다.
//
// 비용(PasswordCost)은 반복 횟수와 메모리다. 프리셋은 libsodium과 같다(INTERACTIVE 2회·64 MiB,
// MODERATE 3회·256 MiB, SENSITIVE 4회·1 GiB). 저장된 문자열·파일처럼 신뢰할 수 없는 곳에서 온
// 비용은 상한(반복 32회, 메모리 1 GiB)으로 먼저 검사해 자원 고갈을 막는다(넘으면 CapacityExceeded,
// 계산하지 않는다).
//
// Argon2id는 일부러 느리고 메모리를 많이 쓴다(INTERACTIVE 기준 수십–수백 ms, 64 MiB). 작업 메모리는
// libsodium이 직접 할당한다(Rocks 할당기 밖, 5.1절 예외). 게임 스레드에서 부르지 않는다. 여러
// 스레드에서 동시에 불러도 된다. Argon2id는 첫 pass 뒤 데이터에 따라 메모리를 읽는 설계라 완전한
// 상수 시간이 아니다(2절).
//
// 사용 예:
//   using namespace rocks::crypto;
//   char stored[PasswordHashStringBytes];
//   auto written = TryHashPassword(password, PasswordCostInteractive, Span<char>(stored));
//   if (!written) { return written.Error(); }
//   // 나중에 로그인할 때(StringView(stored, written.Value())를 저장해 둔다)
//   auto ok = TryVerifyPassword(StringView(stored, written.Value()), attempt);
//   if (!ok) { return ok.Error(); }
namespace rocks::crypto {

inline constexpr usize PasswordSaltBytes = 16;
// TryHashPassword 출력 버퍼 크기(NUL 포함 최대 길이)다.
inline constexpr usize PasswordHashStringBytes = 128;

struct PasswordCost {
    // 반복(pass) 횟수다. 1–32.
    uint32 operations = 0;
    // 작업 메모리 byte다. 8 KiB–1 GiB. Argon2id는 1 KiB 단위로 내려 쓴다.
    usize memoryBytes = 0;
};

inline constexpr PasswordCost PasswordCostInteractive{2, usize{64} << 20};
inline constexpr PasswordCost PasswordCostModerate{3, usize{256} << 20};
inline constexpr PasswordCost PasswordCostSensitive{4, usize{1} << 30};

inline constexpr uint32 MaxPasswordOperations = 32;
inline constexpr usize MinPasswordMemoryBytes = 8 * 1024;
inline constexpr usize MaxPasswordMemoryBytes = usize{1} << 30;

inline constexpr usize MinPasswordSecretBytes = 16;
inline constexpr usize MaxPasswordSecretBytes = 4096;

// password·salt·cost에서 bytes(16–4096) 길이의 비밀을 뽑는다. salt는 정확히 16 byte다. 인자가 범위
// 밖이면 InvalidArgument, cost가 상한을 넘으면 CapacityExceeded, Argon2 작업 메모리를 얻지 못하면
// OutOfMemory다.
[[nodiscard]] Result<SecretBytes> TryDerivePasswordSecret(Span<const uint8> password,
                                                          Span<const uint8> salt, PasswordCost cost,
                                                          usize bytes) noexcept;

// 비밀번호에서 KdfKey(32 byte)를 얻는다. 오류는 TryDerivePasswordSecret와 같다.
[[nodiscard]] Result<KdfKey> TryDeriveKdfKeyFromPassword(Span<const uint8> password,
                                                         Span<const uint8> salt,
                                                         PasswordCost cost) noexcept;

// 저장용 문자열을 만든다(salt는 OS 난수). text에 NUL로 끝나는 문자열을 쓰고 NUL 앞 길이를 돌려준다.
// text가 PasswordHashStringBytes보다 작으면 CapacityExceeded다. 나머지 오류는
// TryDerivePasswordSecret와 같다.
[[nodiscard]] Result<usize> TryHashPassword(Span<const uint8> password, PasswordCost cost,
                                            Span<char> text) noexcept;

// 저장된 문자열로 비밀번호를 확인한다. 맞으면 성공이다.
// - 비밀번호가 다르거나 문자열 형식이 틀리면 AuthenticationFailed다(저장소에서 온 byte다).
//   libsodium이 구분하지 않으므로 Argon2 작업 메모리를 얻지 못한 경우도 AuthenticationFailed로
//   돌아온다.
// - Argon2id가 아닌 문자열(예: "$argon2i$")은 Unsupported다.
// - 문자열의 비용이 상한을 넘으면 계산하지 않고 CapacityExceeded다.
[[nodiscard]] Result<void> TryVerifyPassword(StringView encoded,
                                             Span<const uint8> password) noexcept;

// 저장된 문자열의 비용이 cost와 다르면 true다(다음 로그인 때 다시 해시해 저장할 때가 되었다).
// 문자열 형식이 틀리면 AuthenticationFailed, Argon2id가 아니면 Unsupported다.
[[nodiscard]] Result<bool> TryPasswordNeedsRehash(StringView encoded, PasswordCost cost) noexcept;

} // namespace rocks::crypto
