#include <Crypto/Channel.h>

#include <Base/Endian.h>
#include <Crypto/Bytes.h>
#include <Crypto/Random.h>

#include "AdapterCommon.h"
#include "AeadInternal.h"
#include "ChannelInternal.h"
#include "KeyAccess.h"
#include "SecretArena.h"
#include "Sodium/SodiumRuntime.h"

#include <Core/Move.h>
#include <Platform/DataIndependentTiming.h>
#include <Platform/Memory.h>

#include <sodium.h>

// 서버 정적 키와 Noise_NK_25519_ChaChaPoly_SHA256
// handshake다(Docs/Design/SecureChannel.ko.md 3.1절). Noise rev 34의
// CipherState(5.1절)·SymmetricState(5.2절)·HandshakeState(5.3절)를 NK 패턴 하나로 펼쳐 둔다. 메시지
// 토큰 처리는 패턴 표 대신 두 메시지의 고정 순서다.
namespace rocks::crypto {
namespace {

constexpr usize DhBytes = 32;
constexpr usize HashBytes = 32;
constexpr usize CipherKeyBytes = 32;
constexpr usize ChannelHandshakeNonceBytes = 12;
constexpr usize NoiseTagBytes = 16;

static_assert(ChannelKeyBytes == crypto_scalarmult_BYTES &&
              ChannelKeyBytes == crypto_scalarmult_SCALARBYTES && DhBytes == ChannelKeyBytes);
static_assert(HashBytes == crypto_hash_sha256_BYTES && HashBytes == ChannelHashBytes);
static_assert(CipherKeyBytes == crypto_aead_chacha20poly1305_ietf_KEYBYTES &&
              CipherKeyBytes == crypto_kdf_hkdf_sha256_KEYBYTES);
static_assert(ChannelHandshakeNonceBytes == crypto_aead_chacha20poly1305_ietf_NPUBBYTES);
static_assert(NoiseTagBytes == crypto_aead_chacha20poly1305_ietf_ABYTES);
static_assert(NoiseMessageOverheadBytes == DhBytes + NoiseTagBytes);

// 프로토콜 이름이 HASHLEN(32) 이하면 0으로 채워 h로 쓰고, 넘으면 해시한다(Noise 5.2절
// InitializeSymmetric). 이 이름은 정확히 32 byte다.
constexpr char ProtocolName[] = "Noise_NK_25519_ChaChaPoly_SHA256";
static_assert(sizeof(ProtocolName) - 1 == HashBytes);

// handshake 비밀 상태다(키 저장소 슬롯). Split·실패·소멸 때 슬롯 전체를 지운다.
struct NoiseState {
    // SymmetricState: ck, h.
    uint8 chainingKey[HashBytes];
    uint8 hash[HashBytes];
    // CipherState: k, n. hasKey가 false면 k는 비어 있다(NK의 두 payload 자리에서는 늘 true다).
    uint8 cipherKey[CipherKeyBytes];
    uint64 nonce;
    bool hasKey;
    // HandshakeState: e(비밀·공개), s(서버 비밀키 사본, responder만), re, rs(initiator만).
    uint8 localEphemeral[DhBytes];
    uint8 localEphemeralPublic[DhBytes];
    uint8 localStatic[DhBytes];
    uint8 remoteEphemeral[DhBytes];
    uint8 remoteStatic[DhBytes];
    // 계산 중 값(DH 결과, HKDF temp_key·출력)이다. 쓰고 바로 지운다.
    uint8 sharedSecret[DhBytes];
    uint8 temporaryKey[HashBytes];
    uint8 hkdfOutput[2 * HashBytes];
    // initiator가 둘째 메시지를 읽기 전의 SymmetricState·CipherState다. 읽기가 실패하면 되돌려
    // 다음 메시지를 다시 읽을 수 있게 한다(SaveForRead·RestoreAfterFailedRead).
    uint8 savedChainingKey[HashBytes];
    uint8 savedHash[HashBytes];
    uint8 savedCipherKey[CipherKeyBytes];
    uint64 savedNonce;
    bool savedHasKey;
};

NoiseState& StateOf(void* state) noexcept {
    return *static_cast<NoiseState*>(state);
}

// h = SHA-256(h ‖ data)다.
void MixHash(NoiseState& state, Span<const uint8> data) noexcept {
    crypto_hash_sha256_state hasher;
    detail::RequireSodium(crypto_hash_sha256_init(&hasher), "Crypto: sha256 init failed");
    detail::RequireSodium(crypto_hash_sha256_update(&hasher, state.hash, HashBytes),
                          "Crypto: sha256 update failed");
    detail::RequireSodium(
        crypto_hash_sha256_update(&hasher, detail::InputPointer(data), data.Size()),
        "Crypto: sha256 update failed");
    detail::RequireSodium(crypto_hash_sha256_final(&hasher, state.hash),
                          "Crypto: sha256 final failed");
}

// Noise HKDF(ck, ikm, 2)를 hkdfOutput에 쓴다: temp_key = HMAC(ck, ikm), output1 = HMAC(temp_key,
// 0x01), output2 = HMAC(temp_key, output1 ‖ 0x02). RFC 5869 HKDF-Extract(salt = ck, ikm)와 빈
// info의 Expand(64 byte)와 같은 byte다.
void Hkdf(NoiseState& state, Span<const uint8> inputKeyMaterial) noexcept {
    detail::RequireSodium(crypto_kdf_hkdf_sha256_extract(
                              state.temporaryKey, state.chainingKey, HashBytes,
                              detail::InputPointer(inputKeyMaterial), inputKeyMaterial.Size()),
                          "Crypto: hkdf extract failed");
    detail::RequireSodium(crypto_kdf_hkdf_sha256_expand(state.hkdfOutput, sizeof(state.hkdfOutput),
                                                        "", 0, state.temporaryKey),
                          "Crypto: hkdf expand failed");
    sodium_memzero(state.temporaryKey, sizeof(state.temporaryKey));
}

// ck, k = HKDF(ck, DH 결과)이고 n = 0이다(MixKey). DH 결과와 HKDF 출력을 지운다.
void MixKey(NoiseState& state) noexcept {
    Hkdf(state, Span<const uint8>(state.sharedSecret));
    platform::MemoryCopy(state.chainingKey, state.hkdfOutput, HashBytes);
    platform::MemoryCopy(state.cipherKey, state.hkdfOutput + HashBytes, CipherKeyBytes);
    state.nonce = 0;
    state.hasKey = true;
    sodium_memzero(state.hkdfOutput, sizeof(state.hkdfOutput));
    sodium_memzero(state.sharedSecret, sizeof(state.sharedSecret));
}

// DH(secret, publicKey)를 sharedSecret에 쓴다. 결과가 0(small-order 점)이면 false다.
bool Dh(NoiseState& state, const uint8* secret, const uint8* publicKey) noexcept {
    return crypto_scalarmult(state.sharedSecret, secret, publicKey) == 0;
}

// ChaChaPoly nonce = 0^32 ‖ LE64(n)이다(Noise 12.3절).
void ChannelHandshakeMakeNonce(uint64 counter,
                               uint8 (&nonce)[ChannelHandshakeNonceBytes]) noexcept {
    platform::MemoryZero(nonce, 4);
    StoreLittleEndian<uint64>(nonce + 4, counter);
}

// c = ENCRYPT(k, n++, h, plaintext)를 ciphertext(plaintext + 16 byte)에 쓰고 MixHash(c)다.
void EncryptAndHash(NoiseState& state, Span<const uint8> plaintext,
                    Span<uint8> ciphertext) noexcept {
    uint8 nonce[ChannelHandshakeNonceBytes];
    ChannelHandshakeMakeNonce(state.nonce, nonce);
    unsigned long long written = 0;
    detail::RequireSodium(crypto_aead_chacha20poly1305_ietf_encrypt(
                              ciphertext.Data(), &written, detail::InputPointer(plaintext),
                              plaintext.Size(), state.hash, HashBytes, nullptr, nonce,
                              state.cipherKey),
                          "Crypto: chacha20poly1305 encrypt failed");
    ++state.nonce;
    MixHash(state, Span<const uint8>(ciphertext));
}

// plaintext = DECRYPT(k, n++, h, ciphertext), MixHash(ciphertext)다. 실패하면 false이고 상태를
// 바꾸지 않는다(호출자가 handshake를 버린다).
bool DecryptAndHash(NoiseState& state, Span<const uint8> ciphertext,
                    Span<uint8> plaintext) noexcept {
    uint8 nonce[ChannelHandshakeNonceBytes];
    ChannelHandshakeMakeNonce(state.nonce, nonce);
    uint8 scratch = 0;
    unsigned long long written = 0;
    if (crypto_aead_chacha20poly1305_ietf_decrypt(
            detail::OutputPointer(plaintext, &scratch), &written, nullptr, ciphertext.Data(),
            ciphertext.Size(), state.hash, HashBytes, nonce, state.cipherKey) != 0) {
        return false;
    }

    ++state.nonce;
    MixHash(state, ciphertext);
    return true;
}

// h = 프로토콜 이름, ck = h, MixHash(prologue), MixHash(서버 정적 공개키)다(pre-message `<- s`).
void InitializeState(NoiseState& state, Span<const uint8> prologue,
                     const uint8* serverPublic) noexcept {
    platform::MemoryCopy(state.hash, ProtocolName, HashBytes);
    platform::MemoryCopy(state.chainingKey, state.hash, HashBytes);
    MixHash(state, prologue);
    MixHash(state, Span<const uint8>(serverPublic, ChannelKeyBytes));
}

// 읽기 전 상태를 같은 슬롯 안에 저장한다.
void SaveForRead(NoiseState& state) noexcept {
    platform::MemoryCopy(state.savedChainingKey, state.chainingKey, HashBytes);
    platform::MemoryCopy(state.savedHash, state.hash, HashBytes);
    platform::MemoryCopy(state.savedCipherKey, state.cipherKey, CipherKeyBytes);
    state.savedNonce = state.nonce;
    state.savedHasKey = state.hasKey;
}

// 저장한 사본을 지운다.
void ClearSavedForRead(NoiseState& state) noexcept {
    sodium_memzero(state.savedChainingKey, sizeof(state.savedChainingKey));
    sodium_memzero(state.savedHash, sizeof(state.savedHash));
    sodium_memzero(state.savedCipherKey, sizeof(state.savedCipherKey));
    state.savedNonce = 0;
    state.savedHasKey = false;
}

// 실패한 읽기를 되돌린다. 읽는 동안 바뀐 값(re, DH 결과)을 지우고 저장한 상태를 복원한다.
void RestoreAfterFailedRead(NoiseState& state) noexcept {
    platform::MemoryCopy(state.chainingKey, state.savedChainingKey, HashBytes);
    platform::MemoryCopy(state.hash, state.savedHash, HashBytes);
    platform::MemoryCopy(state.cipherKey, state.savedCipherKey, CipherKeyBytes);
    state.nonce = state.savedNonce;
    state.hasKey = state.savedHasKey;
    sodium_memzero(state.remoteEphemeral, sizeof(state.remoteEphemeral));
    sodium_memzero(state.sharedSecret, sizeof(state.sharedSecret));
    ClearSavedForRead(state);
}

Result<void*> TryAllocateState() noexcept {
    auto slot = detail::TryAllocateSecretSlot(sizeof(NoiseState), alignof(NoiseState));
    if (!slot) {
        return slot;
    }

    InitializeAt<NoiseState>(slot.Value());
    return slot;
}

// X25519 비밀키(키 저장소)로 공개키를 계산해 서버 키를 만든다. 오류는 key의 것이다.
Result<ChannelServerKey> TryMakeServerKey(KeyId id, Result<SecretBytes>&& key) noexcept {
    if (!key) {
        return Result<ChannelServerKey>::Failure(key.Error());
    }

    ChannelPublicKey publicKey;
    publicKey.id = id;
    detail::EnsureSodium();
    // clamp한 스칼라는 0이 아니므로 기저점 곱은 실패하지 않는다.
    detail::RequireSodium(crypto_scalarmult_base(publicKey.bytes, key.Value().View().Data()),
                          "Crypto: crypto_scalarmult_base failed");
    return Result<ChannelServerKey>::Success(
        detail::KeyAccess::Make<ChannelServerKey>(Move(key.Value()), publicKey));
}

} // namespace

// ---- 서버 정적 키 ----

Result<ChannelPublicKey> ChannelPublicKey::TryFromBytes(KeyId id,
                                                        Span<const uint8> bytes) noexcept {
    if (id.IsNil() || bytes.Size() != ChannelKeyBytes) {
        return Result<ChannelPublicKey>::Failure(ErrorCode::InvalidArgument);
    }

    ChannelPublicKey key;
    key.id = id;
    platform::MemoryCopy(key.bytes, bytes.Data(), ChannelKeyBytes);
    return Result<ChannelPublicKey>::Success(key);
}

Result<ChannelServerKey> ChannelServerKey::TryGenerate(KeyId id) noexcept {
    platform::DataIndependentTimingScope dit;
    if (id.IsNil()) {
        return Result<ChannelServerKey>::Failure(ErrorCode::InvalidArgument);
    }

    // 비밀키를 키 저장소에 바로 만들어 스택 사본을 남기지 않는다(crypto_box_keypair와 같은 방식).
    return TryMakeServerKey(id, SecretBytes::TryGenerate(ChannelKeyBytes));
}

Result<ChannelServerKey> ChannelServerKey::TryImport(KeyId id,
                                                     Span<const uint8> secretKey) noexcept {
    platform::DataIndependentTimingScope dit;
    if (id.IsNil() || secretKey.Size() != ChannelKeyBytes) {
        return Result<ChannelServerKey>::Failure(ErrorCode::InvalidArgument);
    }

    return TryMakeServerKey(id, SecretBytes::TryCopyFrom(secretKey));
}

Result<void> hazmat::TryExportKey(const ChannelServerKey& key, Span<uint8> output) noexcept {
    platform::DataIndependentTimingScope dit;
    const Span<const uint8> bytes = detail::KeyAccess::Bytes(key);
    if (bytes.IsEmpty()) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (output.Size() != bytes.Size() || Overlaps(output, bytes)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    platform::MemoryCopy(output.Data(), bytes.Data(), bytes.Size());
    return Result<void>::Success();
}

// ---- handshake ----

NoiseHandshake::~NoiseHandshake() noexcept {
    Release();
}

NoiseHandshake::NoiseHandshake(NoiseHandshake&& other) noexcept
    : state_(other.state_), initiator_(other.initiator_), step_(other.step_) {
    other.state_ = nullptr;
    other.initiator_ = false;
    other.step_ = NoiseStep::None;
}

NoiseHandshake& NoiseHandshake::operator=(NoiseHandshake&& other) noexcept {
    if (this != &other) {
        Release();
        state_ = other.state_;
        initiator_ = other.initiator_;
        step_ = other.step_;
        other.state_ = nullptr;
        other.initiator_ = false;
        other.step_ = NoiseStep::None;
    }

    return *this;
}

void NoiseHandshake::Release() noexcept {
    // 슬롯 반환이 전체를 지운다.
    detail::FreeSecretSlot(state_);
    state_ = nullptr;
}

Result<NoiseHandshake> NoiseHandshake::TryCreateInitiator(const ChannelPublicKey& server,
                                                          Span<const uint8> prologue) noexcept {
    platform::DataIndependentTimingScope dit;
    if (server.id.IsNil()) {
        return Result<NoiseHandshake>::Failure(ErrorCode::InvalidArgument);
    }

    auto slot = TryAllocateState();
    if (!slot) {
        return Result<NoiseHandshake>::Failure(slot.Error());
    }

    detail::EnsureSodium();
    NoiseState& state = StateOf(slot.Value());
    platform::MemoryCopy(state.remoteStatic, server.bytes, ChannelKeyBytes);
    InitializeState(state, prologue, server.bytes);
    return Result<NoiseHandshake>::Success(
        NoiseHandshake(slot.Value(), true, NoiseStep::WriteMessage));
}

Result<NoiseHandshake> NoiseHandshake::TryCreateResponder(const ChannelServerKey& key,
                                                          Span<const uint8> prologue) noexcept {
    platform::DataIndependentTimingScope dit;
    const Span<const uint8> secret = detail::KeyAccess::Bytes(key);
    if (secret.IsEmpty()) {
        return Result<NoiseHandshake>::Failure(ErrorCode::StaleHandle);
    }

    auto slot = TryAllocateState();
    if (!slot) {
        return Result<NoiseHandshake>::Failure(slot.Error());
    }

    detail::EnsureSodium();
    NoiseState& state = StateOf(slot.Value());
    platform::MemoryCopy(state.localStatic, secret.Data(), ChannelKeyBytes);
    const ChannelPublicKey publicKey = key.PublicKey();
    InitializeState(state, prologue, publicKey.bytes);
    return Result<NoiseHandshake>::Success(
        NoiseHandshake(slot.Value(), false, NoiseStep::ReadMessage));
}

Result<usize> NoiseHandshake::TryWriteMessage(Span<const uint8> payload,
                                              Span<uint8> message) noexcept {
    platform::DataIndependentTimingScope dit;
    return detail::ChannelFactory::TryWriteMessage(*this, Span<const uint8>(), payload, message);
}

Result<usize> detail::ChannelFactory::TryWriteMessage(NoiseHandshake& handshake,
                                                      Span<const uint8> ephemeralSecret,
                                                      Span<const uint8> payload,
                                                      Span<uint8> message) noexcept {
    platform::DataIndependentTimingScope dit;
    if (handshake.step_ == NoiseStep::None) {
        return Result<usize>::Failure(ErrorCode::StaleHandle);
    }

    // 실패 뒤에도 겹침을 먼저 본다. 지우기가 호출자의 입력을 덮지 않게 한다.
    if (Overlaps(payload, message)) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    if (handshake.step_ == NoiseStep::Failed) {
        SecureZero(message);
        return Result<usize>::Failure(ErrorCode::AuthenticationFailed);
    }

    if (handshake.step_ != NoiseStep::WriteMessage ||
        (!ephemeralSecret.IsEmpty() && ephemeralSecret.Size() != DhBytes)) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    if (payload.Size() > NoiseMaxPayloadBytes ||
        message.Size() < payload.Size() + NoiseMessageOverheadBytes) {
        return Result<usize>::Failure(ErrorCode::CapacityExceeded);
    }

    NoiseState& state = StateOf(handshake.state_);
    // ephemeral 비밀키는 키 저장소에 바로 만든다. OS 난수가 실패하면 TryFillRandom이 지운다(상태
    // 불변: 아직 h·ck를 바꾸지 않았다).
    if (ephemeralSecret.IsEmpty()) {
        if (auto filled = TryFillRandom(Span<uint8>(state.localEphemeral)); !filled) {
            return Result<usize>::Failure(filled.Error());
        }
    } else {
        platform::MemoryCopy(state.localEphemeral, ephemeralSecret.Data(), DhBytes);
    }

    // clamp한 스칼라는 0이 아니므로 기저점 곱은 실패하지 않는다.
    detail::RequireSodium(crypto_scalarmult_base(state.localEphemeralPublic, state.localEphemeral),
                          "Crypto: crypto_scalarmult_base failed");

    // e: 공개키를 싣고 MixHash. 그 뒤 initiator는 es = DH(e, rs), responder는 ee = DH(e, re)다.
    const usize messageBytes = payload.Size() + NoiseMessageOverheadBytes;
    const Span<uint8> output = message.Left(messageBytes);
    platform::MemoryCopy(output.Data(), state.localEphemeralPublic, DhBytes);
    MixHash(state, Span<const uint8>(state.localEphemeralPublic));
    const uint8* peer = handshake.initiator_ ? state.remoteStatic : state.remoteEphemeral;
    if (!Dh(state, state.localEphemeral, peer)) {
        // initiator가 가진 서버 공개키가 small-order 점이다(responder의 re는 읽을 때 이미 걸렀다).
        SecureZero(message);
        handshake.Release();
        handshake.step_ = NoiseStep::Failed;
        return Result<usize>::Failure(ErrorCode::AuthenticationFailed);
    }

    MixKey(state);
    // responder의 e는 ee 뒤로 쓰이지 않는다. initiator의 e는 둘째 메시지의 ee에 다시 쓴다.
    if (!handshake.initiator_) {
        sodium_memzero(state.localEphemeral, sizeof(state.localEphemeral));
    }

    EncryptAndHash(state, payload, output.RightChop(DhBytes));
    handshake.step_ = handshake.initiator_ ? NoiseStep::ReadMessage : NoiseStep::Split;
    return Result<usize>::Success(messageBytes);
}

Result<usize> NoiseHandshake::TryReadMessage(Span<const uint8> message,
                                             Span<uint8> payload) noexcept {
    platform::DataIndependentTimingScope dit;
    if (step_ == NoiseStep::None) {
        return Result<usize>::Failure(ErrorCode::StaleHandle);
    }

    if (Overlaps(message, payload)) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    if (step_ == NoiseStep::Failed) {
        SecureZero(payload);
        return Result<usize>::Failure(ErrorCode::AuthenticationFailed);
    }

    if (step_ != NoiseStep::ReadMessage) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    // initiator는 실패한 읽기를 되돌려 ReadMessage에 머문다. 서버 주소로 위장한 경로 밖 공격자가
    // 가짜 둘째 메시지 하나로 handshake를 끝내지 못하게 한다(사본으로 계산하고 성공할 때만
    // 반영한다, WireGuard와 같은 방식). responder는 첫 메시지마다 새 객체를 쓰므로 실패하면 버린다.
    if (message.Size() < NoiseMessageOverheadBytes || message.Size() > NoiseMaxMessageBytes) {
        SecureZero(payload);
        if (!initiator_) {
            Release();
            step_ = NoiseStep::Failed;
        }

        return Result<usize>::Failure(ErrorCode::AuthenticationFailed);
    }

    const usize payloadBytes = message.Size() - NoiseMessageOverheadBytes;
    if (payload.Size() < payloadBytes) {
        return Result<usize>::Failure(ErrorCode::CapacityExceeded);
    }

    // e: re를 읽고 MixHash. 그 뒤 responder는 es = DH(s, re), initiator는 ee = DH(e, re)다. re를
    // 상태에 한 번 복사해 그 사본으로만 계산한다.
    NoiseState& state = StateOf(state_);
    if (initiator_) {
        SaveForRead(state);
    }

    platform::MemoryCopy(state.remoteEphemeral, message.Data(), DhBytes);
    MixHash(state, Span<const uint8>(state.remoteEphemeral));
    const uint8* secret = initiator_ ? state.localEphemeral : state.localStatic;
    const bool agreed = Dh(state, secret, state.remoteEphemeral);
    if (agreed) {
        MixKey(state);
    }

    if (!agreed || !DecryptAndHash(state, message.RightChop(DhBytes), payload.Left(payloadBytes))) {
        SecureZero(payload);
        if (initiator_) {
            RestoreAfterFailedRead(state);
        } else {
            Release();
            step_ = NoiseStep::Failed;
        }

        return Result<usize>::Failure(ErrorCode::AuthenticationFailed);
    }

    // 남은 단계에서 쓰지 않는 비밀을 지운다(responder의 s 사본, initiator의 e와 되돌리기 사본).
    sodium_memzero(initiator_ ? state.localEphemeral : state.localStatic, DhBytes);
    if (initiator_) {
        ClearSavedForRead(state);
    }

    step_ = initiator_ ? NoiseStep::Split : NoiseStep::WriteMessage;
    return Result<usize>::Success(payloadBytes);
}

Result<ChannelSession> NoiseHandshake::TrySplit() noexcept {
    platform::DataIndependentTimingScope dit;
    if (step_ == NoiseStep::None) {
        return Result<ChannelSession>::Failure(ErrorCode::StaleHandle);
    }

    if (step_ == NoiseStep::Failed) {
        return Result<ChannelSession>::Failure(ErrorCode::AuthenticationFailed);
    }

    if (step_ != NoiseStep::Split) {
        return Result<ChannelSession>::Failure(ErrorCode::InvalidArgument);
    }

    // k1, k2 = HKDF(ck, 빈 값, 2)다. initiator는 k1로 보내고 k2로 받으며, responder는 반대다.
    // 실패해도 ck는 그대로라 다시 부르면 같은 키가 나온다.
    NoiseState& state = StateOf(state_);
    Hkdf(state, Span<const uint8>());
    const Span<const uint8> output(state.hkdfOutput);
    auto first = SecretBytes::TryCopyFrom(output.Left(CipherKeyBytes));
    auto second = SecretBytes::TryCopyFrom(output.Right(CipherKeyBytes));
    sodium_memzero(state.hkdfOutput, sizeof(state.hkdfOutput));
    if (!first || !second) {
        return Result<ChannelSession>::Failure(!first ? first.Error() : second.Error());
    }

    constexpr AeadAlgorithm Algorithm = AeadAlgorithm::ChaCha20Poly1305Ietf;
    auto sendKey =
        detail::TryMakeAeadKey(Algorithm, Move(initiator_ ? first.Value() : second.Value()));
    if (!sendKey) {
        return Result<ChannelSession>::Failure(sendKey.Error());
    }

    auto receiveKey =
        detail::TryMakeAeadKey(Algorithm, Move(initiator_ ? second.Value() : first.Value()));
    if (!receiveKey) {
        return Result<ChannelSession>::Failure(receiveKey.Error());
    }

    ChannelSession session;
    session.send = DatagramSealer(Move(sendKey.Value()));
    session.receive = DatagramOpener(Move(receiveKey.Value()));
    platform::MemoryCopy(session.handshakeHash, state.hash, ChannelHashBytes);
    Release();
    step_ = NoiseStep::Done;
    return Result<ChannelSession>::Success(Move(session));
}

} // namespace rocks::crypto
