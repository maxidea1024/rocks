#include <Crypto/PasswordHash.h>

#include <Crypto/Bytes.h>

#include "AdapterCommon.h"
#include "Sodium/SodiumRuntime.h"

#include <Core/Move.h>
#include <Platform/Memory.h>

#include <sodium.h>

namespace rocks::crypto {
namespace {

static_assert(PasswordSaltBytes == crypto_pwhash_argon2id_SALTBYTES);
static_assert(PasswordHashStringBytes == crypto_pwhash_argon2id_STRBYTES);
static_assert(MinPasswordMemoryBytes >= crypto_pwhash_argon2id_MEMLIMIT_MIN);
static_assert(MaxPasswordOperations <= crypto_pwhash_argon2id_OPSLIMIT_MAX);
static_assert(MinPasswordSecretBytes >= crypto_pwhash_argon2id_BYTES_MIN);
static_assert(crypto_pwhash_argon2id_OPSLIMIT_MIN == 1);

constexpr StringView Argon2Prefix("$argon2");
constexpr StringView Argon2idPrefix("$argon2id$v=19$m=");

// 신뢰할 수 없는 곳에서 온 비용인지와 관계없이 같은 규칙으로 검사한다. 범위 밖(0회·8 KiB 미만)은
// InvalidArgument, 상한 초과는 CapacityExceeded다.
Result<void> CheckCost(PasswordCost cost) noexcept {
    if (cost.operations < 1 || cost.memoryBytes < MinPasswordMemoryBytes) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (cost.operations > MaxPasswordOperations || cost.memoryBytes > MaxPasswordMemoryBytes) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    return Result<void>::Success();
}

bool IsValidPasswordLength(Span<const uint8> password) noexcept {
    return password.Size() <= crypto_pwhash_argon2id_PASSWD_MAX;
}

// 빈 비밀번호도 null이 아닌 주소로 넘긴다(libsodium은 길이 0이어도 포인터를 받는다).
const char* PasswordPointer(Span<const uint8> password) noexcept {
    return password.IsEmpty() ? "" : reinterpret_cast<const char*>(password.Data());
}

// text[offset..]에서 10진 숫자를 읽는다. 숫자가 없거나 너무 길면(10 자리 초과) false다.
bool ReadNumber(StringView text, usize& offset, uint64& value) noexcept {
    const usize start = offset;
    value = 0;
    while (offset < text.ByteCount() && text.Data()[offset] >= '0' && text.Data()[offset] <= '9') {
        if (offset - start == 10) {
            return false;
        }

        value = value * 10 + static_cast<uint64>(text.Data()[offset] - '0');
        ++offset;
    }

    return offset != start;
}

bool Expect(StringView text, usize& offset, char expected) noexcept {
    if (offset >= text.ByteCount() || text.Data()[offset] != expected) {
        return false;
    }

    ++offset;
    return true;
}

bool StartsWith(StringView text, StringView prefix) noexcept {
    return text.ByteCount() >= prefix.ByteCount() &&
           platform::MemoryCompare(text.Data(), prefix.Data(), prefix.ByteCount()) == 0;
}

// "$argon2id$v=19$m=<KiB>,t=<회>,p=<lane>$..."의 비용을 읽고 libsodium에 넘길 NUL 종료 사본을
// 만든다. 형식이 틀리면 AuthenticationFailed(저장소에서 온 byte다), Argon2id v19가 아니면
// Unsupported다.
Result<PasswordCost> ParseEncoded(StringView encoded,
                                  char (&copy)[PasswordHashStringBytes]) noexcept {
    if (!StartsWith(encoded, Argon2Prefix)) {
        return Result<PasswordCost>::Failure(ErrorCode::AuthenticationFailed);
    }

    if (!StartsWith(encoded, Argon2idPrefix)) {
        return Result<PasswordCost>::Failure(ErrorCode::Unsupported);
    }

    if (encoded.ByteCount() >= PasswordHashStringBytes) {
        return Result<PasswordCost>::Failure(ErrorCode::AuthenticationFailed);
    }

    usize offset = Argon2idPrefix.ByteCount();
    uint64 memoryKiB = 0;
    uint64 operations = 0;
    uint64 lanes = 0;
    if (!ReadNumber(encoded, offset, memoryKiB) || !Expect(encoded, offset, ',') ||
        !Expect(encoded, offset, 't') || !Expect(encoded, offset, '=') ||
        !ReadNumber(encoded, offset, operations) || !Expect(encoded, offset, ',') ||
        !Expect(encoded, offset, 'p') || !Expect(encoded, offset, '=') ||
        !ReadNumber(encoded, offset, lanes) || !Expect(encoded, offset, '$')) {
        return Result<PasswordCost>::Failure(ErrorCode::AuthenticationFailed);
    }

    for (usize i = 0; i < encoded.ByteCount(); ++i) {
        if (encoded.Data()[i] == '\0') {
            return Result<PasswordCost>::Failure(ErrorCode::AuthenticationFailed);
        }
    }

    platform::MemoryCopy(copy, encoded.Data(), encoded.ByteCount());
    copy[encoded.ByteCount()] = '\0';
    PasswordCost cost;
    // 10 자리로 막았으므로 KiB → byte 곱이 넘치지 않는다. 상한 검사는 호출자가 CheckCost로 한다.
    cost.operations = operations > MaxPasswordOperations ? MaxPasswordOperations + 1
                                                         : static_cast<uint32>(operations);
    cost.memoryBytes = static_cast<usize>(memoryKiB) * 1024;
    return Result<PasswordCost>::Success(cost);
}

} // namespace

Result<SecretBytes> TryDerivePasswordSecret(Span<const uint8> password, Span<const uint8> salt,
                                            PasswordCost cost, usize bytes) noexcept {
    if (salt.Size() != PasswordSaltBytes || bytes < MinPasswordSecretBytes ||
        bytes > MaxPasswordSecretBytes || !IsValidPasswordLength(password)) {
        return Result<SecretBytes>::Failure(ErrorCode::InvalidArgument);
    }

    if (auto valid = CheckCost(cost); !valid) {
        return Result<SecretBytes>::Failure(valid.Error());
    }

    auto secret = SecretBytes::TryCreate(bytes);
    if (!secret) {
        return secret;
    }

    detail::EnsureSodium();
    // 인자를 모두 검사했으므로 실패는 Argon2 작업 메모리 할당 실패다. secret은 소멸하며 지워진다.
    if (crypto_pwhash(secret.Value().MutableView().Data(), bytes, PasswordPointer(password),
                      password.Size(), salt.Data(), cost.operations, cost.memoryBytes,
                      crypto_pwhash_ALG_ARGON2ID13) != 0) {
        return Result<SecretBytes>::Failure(ErrorCode::OutOfMemory);
    }

    return secret;
}

Result<KdfKey> TryDeriveKdfKeyFromPassword(Span<const uint8> password, Span<const uint8> salt,
                                           PasswordCost cost) noexcept {
    auto secret = TryDerivePasswordSecret(password, salt, cost, KdfKeyBytes);
    if (!secret) {
        return Result<KdfKey>::Failure(secret.Error());
    }

    // 중간 SecretBytes는 소멸하며 지워진다.
    return KdfKey::TryImport(secret.Value().View());
}

Result<usize> TryHashPassword(Span<const uint8> password, PasswordCost cost,
                              Span<char> text) noexcept {
    if (!IsValidPasswordLength(password) || Overlaps(password, text)) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    if (auto valid = CheckCost(cost); !valid) {
        return Result<usize>::Failure(valid.Error());
    }

    if (text.Size() < PasswordHashStringBytes) {
        return Result<usize>::Failure(ErrorCode::CapacityExceeded);
    }

    detail::EnsureSodium();
    if (crypto_pwhash_str(text.Data(), PasswordPointer(password), password.Size(), cost.operations,
                          cost.memoryBytes) != 0) {
        SecureZero(AsWritableBytes(text.Left(PasswordHashStringBytes)));
        return Result<usize>::Failure(ErrorCode::OutOfMemory);
    }

    return Result<usize>::Success(platform::CStringLength(text.Data()));
}

Result<void> TryVerifyPassword(StringView encoded, Span<const uint8> password) noexcept {
    if (!IsValidPasswordLength(password)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    char copy[PasswordHashStringBytes];
    auto cost = ParseEncoded(encoded, copy);
    if (!cost) {
        return Result<void>::Failure(cost.Error());
    }

    // 상한을 넘는 비용은 계산하지 않는다(저장소를 조작한 자원 고갈 방지). 범위 밖 하한은
    // libsodium이 거부한다.
    if (cost.Value().operations > MaxPasswordOperations ||
        cost.Value().memoryBytes > MaxPasswordMemoryBytes) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    detail::EnsureSodium();
    // libsodium은 불일치·형식 오류·작업 메모리 부족을 구분하지 않고 -1을 돌려준다.
    const int status = crypto_pwhash_str_verify(copy, PasswordPointer(password), password.Size());
    return status == 0 ? Result<void>::Success()
                       : Result<void>::Failure(ErrorCode::AuthenticationFailed);
}

Result<bool> TryPasswordNeedsRehash(StringView encoded, PasswordCost cost) noexcept {
    if (auto valid = CheckCost(cost); !valid) {
        return Result<bool>::Failure(valid.Error());
    }

    char copy[PasswordHashStringBytes];
    auto parsed = ParseEncoded(encoded, copy);
    if (!parsed) {
        return Result<bool>::Failure(parsed.Error());
    }

    detail::EnsureSodium();
    const int status = crypto_pwhash_str_needs_rehash(copy, cost.operations, cost.memoryBytes);
    if (status < 0) {
        return Result<bool>::Failure(ErrorCode::AuthenticationFailed);
    }

    return Result<bool>::Success(status == 1);
}

} // namespace rocks::crypto
