#include <Crypto/SegmentStream.h>

#include <Base/Endian.h>
#include <Crypto/Bytes.h>

#include "AdapterCommon.h"
#include "AeadInternal.h"
#include "KdfInternal.h"
#include "KeyAccess.h"
#include "SegmentStreamInternal.h"
#include "Sodium/SodiumRuntime.h"

#include <Platform/DataIndependentTiming.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>

#include <sodium.h>

// 세그먼트 형식 v1의 헤더·크기 산술·키 파생·세그먼트 연산과 SegmentStreamKey다
// (Docs/Design/CryptoSegmentFormat.ko.md). 세그먼트 AEAD와 HKDF·SHA-256은 libsodium이 계산한다.
namespace rocks::crypto {
namespace {

static_assert(SegmentTagBytes == crypto_aead_chacha20poly1305_ietf_ABYTES &&
              SegmentTagBytes == crypto_aead_aes256gcm_ABYTES);
static_assert(detail::SegmentNonceBytes == crypto_aead_chacha20poly1305_ietf_NPUBBYTES &&
              detail::SegmentNonceBytes == crypto_aead_aes256gcm_NPUBBYTES);
static_assert(SegmentStreamKeyBytes == crypto_kdf_hkdf_sha256_KEYBYTES);
static_assert(detail::SegmentAssociatedHashBytes == crypto_hash_sha256_BYTES);
static_assert(detail::SegmentCommitmentOffset + detail::SegmentCommitmentBytes ==
              SegmentHeaderBytes);
static_assert(detail::SegmentSaltOffset + detail::SegmentSaltBytes ==
              detail::SegmentCommitmentOffset);

constexpr uint8 SegmentFormatMagic[4] = {'R', 'S', 'E', 'G'};
constexpr usize SegmentFormatVersionOffset = 4;
constexpr usize AlgorithmOffset = 5;
constexpr usize SizeLog2Offset = 6;
constexpr usize FlagsOffset = 7;
constexpr usize KeyIdOffset = 8;
constexpr usize PrefixCheckBytes = SegmentFormatVersionOffset + 1;

// HKDF info의 앞 16 byte다. 형식 버전이 바뀌면 이 label도 바꾼다.
constexpr uint8 DerivationLabel[16] = {'r', 'o', 'c', 'k', 's', '.', 's', 'e',
                                       'g', 'm', 'e', 'n', 't', '.', 'v', '1'};
constexpr usize InfoBytes =
    sizeof(DerivationLabel) + detail::SegmentCommitmentOffset + detail::SegmentAssociatedHashBytes;
constexpr usize DerivedBytes = SegmentStreamKeyBytes + detail::SegmentCommitmentBytes;

// 세그먼트 수 2^32는 ChaCha20-Poly1305 형식의 평문 상한(2^32 × (S − 16))을 정한다. AES 형식의 평문
// 상한은 세그먼트 암호문 합 상한 L = 2^40 − 16에서 나온다. S가 2^40을 나누므로 꽉 찬 세그먼트
// 2^40/S − 1개와 r = S − 16인 마지막 세그먼트가 L을 정확히 채운다: 평문 = 2^40 − 2^44/S − 16.
constexpr uint64 AesMaxBlocks = SegmentStreamAesMaxBodyBytes / SegmentTagBytes;
static_assert(AesMaxBlocks == (uint64{1} << 36) - 1);

bool SegmentFormatIsDefined(SegmentStreamAlgorithm algorithm) noexcept {
    return algorithm == SegmentStreamAlgorithm::ChaCha20Poly1305 ||
           algorithm == SegmentStreamAlgorithm::Aes256Gcm;
}

void SegmentFormatStoreKeyId(uint8* out, KeyId id) noexcept {
    StoreBigEndian<uint64>(out, id.value.high);
    StoreBigEndian<uint64>(out + 8, id.value.low);
}

KeyId SegmentFormatLoadKeyId(const uint8* in) noexcept {
    KeyId id;
    id.value.high = LoadBigEndian<uint64>(in);
    id.value.low = LoadBigEndian<uint64>(in + 8);
    return id;
}

// 헤더를 byte 순서대로 판정한다(TryPeekSegmentHeader 규칙).
Result<SegmentHeaderInfo> ParseHeader(Span<const uint8> header) noexcept {
    if (header.Size() < PrefixCheckBytes ||
        !platform::MemoryEquals(header.Data(), SegmentFormatMagic, sizeof(SegmentFormatMagic))) {
        return Result<SegmentHeaderInfo>::Failure(ErrorCode::AuthenticationFailed);
    }

    if (header[SegmentFormatVersionOffset] != detail::SegmentFormatVersion) {
        return Result<SegmentHeaderInfo>::Failure(ErrorCode::Unsupported);
    }

    if (header.Size() != SegmentHeaderBytes) {
        return Result<SegmentHeaderInfo>::Failure(ErrorCode::AuthenticationFailed);
    }

    const auto algorithm = static_cast<SegmentStreamAlgorithm>(header[AlgorithmOffset]);
    if (!SegmentFormatIsDefined(algorithm)) {
        return Result<SegmentHeaderInfo>::Failure(ErrorCode::Unsupported);
    }

    const uint8 sizeLog2 = header[SizeLog2Offset];
    const KeyId keyId = SegmentFormatLoadKeyId(header.Data() + KeyIdOffset);
    if (sizeLog2 < SegmentSizeLog2Min || sizeLog2 > SegmentSizeLog2Max ||
        header[FlagsOffset] != 0 || keyId.IsNil()) {
        return Result<SegmentHeaderInfo>::Failure(ErrorCode::AuthenticationFailed);
    }

    SegmentHeaderInfo info;
    info.algorithm = algorithm;
    info.segmentSizeLog2 = sizeLog2;
    info.keyId = keyId;
    return Result<SegmentHeaderInfo>::Success(info);
}

// K ‖ C = HKDF-SHA256(ikm = master, salt = header[24, 56), info = label ‖ header[0, 56) ‖
// SHA-256(AD), L = 64). K로 세그먼트 AeadKey를 만들고 C를 commitment에 쓴다. 호출자가 알고리즘 사용
// 가능 여부(CheckAeadUsable)를 먼저 검사한다.
Result<AeadKey> TryDeriveSegmentKey(Span<const uint8> master, Span<const uint8> header,
                                    Span<const uint8> associatedHash,
                                    Span<uint8> commitment) noexcept {
    if (master.Size() != SegmentStreamKeyBytes || header.Size() < detail::SegmentCommitmentOffset ||
        associatedHash.Size() != detail::SegmentAssociatedHashBytes ||
        commitment.Size() != detail::SegmentCommitmentBytes) {
        platform::Fatal("Crypto: segment key derivation precondition violated");
    }

    uint8 info[InfoBytes];
    platform::MemoryCopy(info, DerivationLabel, sizeof(DerivationLabel));
    platform::MemoryCopy(info + sizeof(DerivationLabel), header.Data(),
                         detail::SegmentCommitmentOffset);
    platform::MemoryCopy(info + sizeof(DerivationLabel) + detail::SegmentCommitmentOffset,
                         associatedHash.Data(), associatedHash.Size());

    uint8 derived[DerivedBytes];
    const Span<const uint8> salt =
        header.Slice(detail::SegmentSaltOffset, detail::SegmentSaltBytes);
    if (!TryHkdf(HkdfAlgorithm::Sha256, master, salt, Span<const uint8>(info),
                 Span<uint8>(derived))) {
        platform::Fatal("Crypto: segment HKDF failed");
    }

    auto keyBytes = SecretBytes::TryCopyFrom(Span<const uint8>(derived, SegmentStreamKeyBytes));
    platform::MemoryCopy(commitment.Data(), derived + SegmentStreamKeyBytes,
                         detail::SegmentCommitmentBytes);
    SecureZero(Span<uint8>(derived));
    if (!keyBytes) {
        return Result<AeadKey>::Failure(keyBytes.Error());
    }

    const auto algorithm = static_cast<SegmentStreamAlgorithm>(header[AlgorithmOffset]);
    return detail::TryMakeAeadKey(detail::SegmentAeadAlgorithm(algorithm), Move(keyBytes.Value()));
}

// 세그먼트 nonce = 0x00 × 3 ‖ BE64(index) ‖ last다.
void SegmentFormatMakeNonce(uint64 index, bool last,
                            uint8 (&nonce)[detail::SegmentNonceBytes]) noexcept {
    nonce[0] = 0;
    nonce[1] = 0;
    nonce[2] = 0;
    StoreBigEndian<uint64>(nonce + 3, index);
    nonce[11] = last ? 1 : 0;
}

const crypto_aead_aes256gcm_state* AesStateOf(const AeadKey& key) noexcept {
    return static_cast<const crypto_aead_aes256gcm_state*>(detail::KeyAccess::State(key));
}

// 열기·검증 공통이다. plaintext가 nullptr이면 libsodium 검증 전용 경로(m = NULL)를 탄다.
bool OpenRaw(const AeadKey& key, uint64 index, bool last, Span<const uint8> sealed,
             uint8* plaintext) noexcept {
    if (sealed.Size() < SegmentTagBytes) {
        platform::Fatal("Crypto: segment open precondition violated");
    }

    uint8 nonce[detail::SegmentNonceBytes];
    SegmentFormatMakeNonce(index, last, nonce);
    const usize bytes = sealed.Size() - SegmentTagBytes;
    const uint8* c = detail::InputPointer(sealed.Left(bytes));
    const uint8* tag = sealed.Data() + bytes;
    const uint8* ad = detail::InputPointer(Span<const uint8>());
    if (key.Algorithm() == AeadAlgorithm::ChaCha20Poly1305Ietf) {
        return crypto_aead_chacha20poly1305_ietf_decrypt_detached(
                   plaintext, nullptr, c, bytes, tag, ad, 0, nonce,
                   detail::KeyAccess::Bytes(key).Data()) == 0;
    }

    if (key.Algorithm() == AeadAlgorithm::Aes256Gcm) {
        return crypto_aead_aes256gcm_decrypt_detached_afternm(plaintext, nullptr, c, bytes, tag, ad,
                                                              0, nonce, AesStateOf(key)) == 0;
    }

    platform::Fatal("Crypto: segment key has an unexpected algorithm");
}

} // namespace

// ---- 형식 도우미 ----

bool detail::IsValidSegmentParams(const SegmentStreamParams& params) noexcept {
    return SegmentFormatIsDefined(params.algorithm) &&
           params.segmentSizeLog2 >= SegmentSizeLog2Min &&
           params.segmentSizeLog2 <= SegmentSizeLog2Max;
}

AeadAlgorithm detail::SegmentAeadAlgorithm(SegmentStreamAlgorithm algorithm) noexcept {
    return algorithm == SegmentStreamAlgorithm::Aes256Gcm ? AeadAlgorithm::Aes256Gcm
                                                          : AeadAlgorithm::ChaCha20Poly1305Ietf;
}

uint64 detail::SegmentMaxPlaintextBytes(const SegmentStreamParams& params) noexcept {
    const uint64 segmentBytes = uint64{1} << params.segmentSizeLog2;
    if (params.algorithm == SegmentStreamAlgorithm::Aes256Gcm) {
        return (uint64{1} << 40) - (uint64{1} << 44) / segmentBytes - SegmentTagBytes;
    }

    return SegmentStreamMaxSegments * (segmentBytes - SegmentTagBytes);
}

SegmentLayout detail::SegmentLayoutForPlaintext(const SegmentStreamParams& params,
                                                uint64 plaintextBytes) noexcept {
    const uint64 segmentBytes = uint64{1} << params.segmentSizeLog2;
    const uint64 capacity = segmentBytes - SegmentTagBytes;
    SegmentLayout layout;
    layout.segmentCount = plaintextBytes == 0 ? 1 : (plaintextBytes - 1) / capacity + 1;
    layout.segmentBytes = segmentBytes;
    layout.lastSegmentOffset = SegmentHeaderBytes + (layout.segmentCount - 1) * segmentBytes;
    layout.lastSegmentBytes =
        plaintextBytes - (layout.segmentCount - 1) * capacity + SegmentTagBytes;
    layout.plaintextBytes = plaintextBytes;
    return layout;
}

void detail::HashSegmentAssociatedData(Span<const uint8> associatedData,
                                       Span<uint8> digest) noexcept {
    if (digest.Size() != SegmentAssociatedHashBytes) {
        platform::Fatal("Crypto: segment AD digest precondition violated");
    }

    detail::EnsureSodium();
    detail::RequireSodium(crypto_hash_sha256(digest.Data(), detail::InputPointer(associatedData),
                                             associatedData.Size()),
                          "Crypto: crypto_hash_sha256 failed");
}

Result<AeadKey> detail::TryStartSegmentStream(const SegmentStreamKey& key,
                                              const SegmentStreamParams& params,
                                              Span<const uint8> associatedData,
                                              Span<const uint8> salt, Span<uint8> header) noexcept {
    if (header.Size() != SegmentHeaderBytes) {
        platform::Fatal("Crypto: segment header buffer precondition violated");
    }

    if (key.IsEmpty()) {
        return Result<AeadKey>::Failure(ErrorCode::StaleHandle);
    }

    if (!IsValidSegmentParams(params) || salt.Size() != SegmentSaltBytes) {
        return Result<AeadKey>::Failure(ErrorCode::InvalidArgument);
    }

    if (auto usable = CheckAeadUsable(SegmentAeadAlgorithm(params.algorithm)); !usable) {
        return Result<AeadKey>::Failure(usable.Error());
    }

    uint8* bytes = header.Data();
    platform::MemoryCopy(bytes, SegmentFormatMagic, sizeof(SegmentFormatMagic));
    bytes[SegmentFormatVersionOffset] = SegmentFormatVersion;
    bytes[AlgorithmOffset] = static_cast<uint8>(params.algorithm);
    bytes[SizeLog2Offset] = params.segmentSizeLog2;
    bytes[FlagsOffset] = 0;
    SegmentFormatStoreKeyId(bytes + KeyIdOffset, key.Id());
    platform::MemoryCopy(bytes + SegmentSaltOffset, salt.Data(), SegmentSaltBytes);

    uint8 associatedHash[SegmentAssociatedHashBytes];
    HashSegmentAssociatedData(associatedData, Span<uint8>(associatedHash));
    return TryDeriveSegmentKey(KeyAccess::Bytes(key), header, Span<const uint8>(associatedHash),
                               header.Slice(SegmentCommitmentOffset, SegmentCommitmentBytes));
}

Result<AeadKey> detail::TryOpenSegmentStream(Span<const uint8> master, KeyId keyId,
                                             Span<const uint8> associatedHash,
                                             Span<const uint8> header,
                                             SegmentStreamParams& params) noexcept {
    auto info = ParseHeader(header);
    if (!info) {
        return Result<AeadKey>::Failure(info.Error());
    }

    if (!(info.Value().keyId == keyId)) {
        return Result<AeadKey>::Failure(ErrorCode::AuthenticationFailed);
    }

    if (auto usable = CheckAeadUsable(SegmentAeadAlgorithm(info.Value().algorithm)); !usable) {
        return Result<AeadKey>::Failure(usable.Error());
    }

    uint8 commitment[SegmentCommitmentBytes];
    auto key = TryDeriveSegmentKey(master, header, associatedHash, Span<uint8>(commitment));
    if (!key) {
        return key;
    }

    const bool matched =
        ConstantTimeEquals(Span<const uint8>(commitment),
                           header.Slice(SegmentCommitmentOffset, SegmentCommitmentBytes));
    SecureZero(Span<uint8>(commitment));
    if (!matched) {
        return Result<AeadKey>::Failure(ErrorCode::AuthenticationFailed);
    }

    params = info.Value().Params();
    return key;
}

void detail::SealSegment(const AeadKey& key, uint64 index, bool last, Span<const uint8> plaintext,
                         Span<uint8> sealed) noexcept {
    const usize bytes = plaintext.Size();
    if (sealed.Size() != bytes + SegmentTagBytes ||
        (Overlaps(plaintext, sealed) && plaintext.Data() != sealed.Data())) {
        platform::Fatal("Crypto: segment seal precondition violated");
    }

    uint8 nonce[SegmentNonceBytes];
    SegmentFormatMakeNonce(index, last, nonce);
    uint8 scratch = 0;
    uint8* c = OutputPointer(sealed.Left(bytes), &scratch);
    uint8* tag = sealed.Data() + bytes;
    const uint8* m = InputPointer(plaintext);
    const uint8* ad = InputPointer(Span<const uint8>());
    if (key.Algorithm() == AeadAlgorithm::ChaCha20Poly1305Ietf) {
        RequireSodium(
            crypto_aead_chacha20poly1305_ietf_encrypt_detached(
                c, tag, nullptr, m, bytes, ad, 0, nullptr, nonce, KeyAccess::Bytes(key).Data()),
            "Crypto: crypto_aead_chacha20poly1305_ietf_encrypt_detached failed");
        return;
    }

    if (key.Algorithm() == AeadAlgorithm::Aes256Gcm) {
        RequireSodium(crypto_aead_aes256gcm_encrypt_detached_afternm(
                          c, tag, nullptr, m, bytes, ad, 0, nullptr, nonce, AesStateOf(key)),
                      "Crypto: crypto_aead_aes256gcm_encrypt_detached_afternm failed");
        return;
    }

    platform::Fatal("Crypto: segment key has an unexpected algorithm");
}

bool detail::OpenSegment(const AeadKey& key, uint64 index, bool last, Span<const uint8> sealed,
                         Span<uint8> plaintext) noexcept {
    if (sealed.Size() < SegmentTagBytes || plaintext.Size() != sealed.Size() - SegmentTagBytes ||
        (Overlaps(plaintext, sealed) && plaintext.Data() != sealed.Data())) {
        platform::Fatal("Crypto: segment open precondition violated");
    }

    uint8 scratch = 0;
    if (!OpenRaw(key, index, last, sealed, OutputPointer(plaintext, &scratch))) {
        SecureZero(plaintext);
        return false;
    }

    return true;
}

bool detail::VerifySegment(const AeadKey& key, uint64 index, bool last,
                           Span<const uint8> sealed) noexcept {
    return OpenRaw(key, index, last, sealed, nullptr);
}

// ---- 공개 형식 함수 ----

Result<SegmentHeaderInfo> TryPeekSegmentHeader(Span<const uint8> header) noexcept {
    return ParseHeader(header);
}

Result<uint64> TryGetSegmentStreamMaxPlaintextBytes(const SegmentStreamParams& params) noexcept {
    if (!detail::IsValidSegmentParams(params)) {
        return Result<uint64>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<uint64>::Success(detail::SegmentMaxPlaintextBytes(params));
}

Result<uint64> TryGetSegmentStreamCiphertextBytes(const SegmentStreamParams& params,
                                                  uint64 plaintextBytes) noexcept {
    if (!detail::IsValidSegmentParams(params)) {
        return Result<uint64>::Failure(ErrorCode::InvalidArgument);
    }

    if (plaintextBytes > detail::SegmentMaxPlaintextBytes(params)) {
        return Result<uint64>::Failure(ErrorCode::CapacityExceeded);
    }

    const SegmentLayout layout = detail::SegmentLayoutForPlaintext(params, plaintextBytes);
    return Result<uint64>::Success(layout.lastSegmentOffset + layout.lastSegmentBytes);
}

Result<SegmentLayout> TryGetSegmentLayout(const SegmentStreamParams& params,
                                          uint64 ciphertextBytes) noexcept {
    if (!detail::IsValidSegmentParams(params)) {
        return Result<SegmentLayout>::Failure(ErrorCode::InvalidArgument);
    }

    if (ciphertextBytes < SegmentHeaderBytes + SegmentTagBytes) {
        return Result<SegmentLayout>::Failure(ErrorCode::AuthenticationFailed);
    }

    const uint64 body = ciphertextBytes - SegmentHeaderBytes;
    const uint64 segmentBytes = uint64{1} << params.segmentSizeLog2;
    const uint64 count = (body - 1) / segmentBytes + 1;
    if (count > SegmentStreamMaxSegments) {
        return Result<SegmentLayout>::Failure(ErrorCode::AuthenticationFailed);
    }

    const uint64 last = body - (count - 1) * segmentBytes;
    if ((count >= 2 && last <= SegmentTagBytes) ||
        (params.algorithm == SegmentStreamAlgorithm::Aes256Gcm &&
         body > SegmentStreamAesMaxBodyBytes)) {
        return Result<SegmentLayout>::Failure(ErrorCode::AuthenticationFailed);
    }

    SegmentLayout layout;
    layout.segmentCount = count;
    layout.segmentBytes = segmentBytes;
    layout.lastSegmentOffset = SegmentHeaderBytes + (count - 1) * segmentBytes;
    layout.lastSegmentBytes = last;
    layout.plaintextBytes = body - count * SegmentTagBytes;
    return Result<SegmentLayout>::Success(layout);
}

// ---- SegmentStreamKey ----

Result<SegmentStreamKey> SegmentStreamKey::TryGenerate(KeyId id) noexcept {
    platform::DataIndependentTimingScope dit;
    if (id.IsNil()) {
        return Result<SegmentStreamKey>::Failure(ErrorCode::InvalidArgument);
    }

    auto key = SecretBytes::TryGenerate(SegmentStreamKeyBytes);
    if (!key) {
        return Result<SegmentStreamKey>::Failure(key.Error());
    }

    return Result<SegmentStreamKey>::Success(SegmentStreamKey(id, Move(key.Value())));
}

Result<SegmentStreamKey> SegmentStreamKey::TryImport(KeyId id,
                                                     Span<const uint8> keyBytes) noexcept {
    platform::DataIndependentTimingScope dit;
    if (id.IsNil() || keyBytes.Size() != SegmentStreamKeyBytes) {
        return Result<SegmentStreamKey>::Failure(ErrorCode::InvalidArgument);
    }

    auto key = SecretBytes::TryCopyFrom(keyBytes);
    if (!key) {
        return Result<SegmentStreamKey>::Failure(key.Error());
    }

    return Result<SegmentStreamKey>::Success(SegmentStreamKey(id, Move(key.Value())));
}

Result<SegmentStreamKey> TryDeriveSegmentStreamKey(const KdfKey& master, uint64 subkeyId,
                                                   KdfContext context, KeyId id) noexcept {
    platform::DataIndependentTimingScope dit;
    if (master.IsEmpty()) {
        return Result<SegmentStreamKey>::Failure(ErrorCode::StaleHandle);
    }

    if (id.IsNil()) {
        return Result<SegmentStreamKey>::Failure(ErrorCode::InvalidArgument);
    }

    auto secret = detail::TryDeriveTypedSecret(
        master, subkeyId, context, detail::SegmentStreamKeyTypeTag, SegmentStreamKeyBytes);
    if (!secret) {
        return Result<SegmentStreamKey>::Failure(secret.Error());
    }

    return Result<SegmentStreamKey>::Success(
        detail::KeyAccess::Make<SegmentStreamKey>(id, Move(secret.Value())));
}

Result<void> hazmat::TryExportKey(const SegmentStreamKey& key, Span<uint8> output) noexcept {
    platform::DataIndependentTimingScope dit;
    const Span<const uint8> bytes = detail::KeyAccess::Bytes(key);
    if (bytes.IsEmpty()) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (output.Size() != SegmentStreamKeyBytes || Overlaps(output, bytes)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    platform::MemoryCopy(output.Data(), bytes.Data(), SegmentStreamKeyBytes);
    return Result<void>::Success();
}

} // namespace rocks::crypto
