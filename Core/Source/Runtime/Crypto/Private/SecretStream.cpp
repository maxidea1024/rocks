#include <Crypto/SecretStream.h>

#include <Base/Compiler.h>
#include <Crypto/Bytes.h>
#include <Crypto/Random.h>

#include "AdapterCommon.h"
#include "KdfInternal.h"
#include "KeyAccess.h"
#include "SecretArena.h"
#include "SecretStreamInternal.h"
#include "Sodium/SodiumRuntime.h"

#include <Platform/DataIndependentTiming.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>

#include <sodium.h>

// secretstream(Crypto/SecretStream.h)의 libsodium 어댑터다: 인자·단계·한도 검사(misuse 조건을
// 부르기 전에 거른다), 키 저장소 슬롯 수명, sticky 실패와 출력 소거.
namespace rocks::crypto {
namespace {

using SodiumState = crypto_secretstream_xchacha20poly1305_state;

static_assert(SecretStreamKeyBytes == crypto_secretstream_xchacha20poly1305_KEYBYTES);
static_assert(SecretStreamWriter::HeaderBytes == crypto_secretstream_xchacha20poly1305_HEADERBYTES);
static_assert(SecretStreamWriter::OverheadBytes == crypto_secretstream_xchacha20poly1305_ABYTES);
static_assert(SecretStreamMaxMessageBytes ==
              crypto_secretstream_xchacha20poly1305_MESSAGEBYTES_MAX);
static_assert(
    static_cast<uint8>(SecretStreamTag::Message) ==
        crypto_secretstream_xchacha20poly1305_TAG_MESSAGE &&
    static_cast<uint8>(SecretStreamTag::Push) == crypto_secretstream_xchacha20poly1305_TAG_PUSH &&
    static_cast<uint8>(SecretStreamTag::Rekey) == crypto_secretstream_xchacha20poly1305_TAG_REKEY &&
    static_cast<uint8>(SecretStreamTag::Final) == crypto_secretstream_xchacha20poly1305_TAG_FINAL);

// 시험 접근점이 쓰는 배치(SecretStreamInternal.h)가 libsodium 정의와 같은지 고정한다.
static_assert(sizeof(SodiumState) == detail::SecretStreamStateBytes);
static_assert(ROCKS_OFFSET_OF(SodiumState, k) == detail::SecretStreamStateKeyOffset &&
              sizeof(SodiumState::k) == detail::SecretStreamStateKeyBytes);
static_assert(ROCKS_OFFSET_OF(SodiumState, nonce) == detail::SecretStreamStateCounterOffset &&
              sizeof(SodiumState::nonce) ==
                  detail::SecretStreamStateCounterBytes + detail::SecretStreamStateInonceBytes);
static_assert(detail::SecretStreamStateInonceOffset ==
              detail::SecretStreamStateCounterOffset + detail::SecretStreamStateCounterBytes);

constexpr usize SecretStreamStateAlignment = 64;

bool SecretStreamIsDefined(SecretStreamTag tag) noexcept {
    return static_cast<uint8>(tag) <= static_cast<uint8>(SecretStreamTag::Final);
}

SodiumState* SecretStreamStateOf(void* state) noexcept {
    return static_cast<SodiumState*>(state);
}

// header와 키로 상태 슬롯을 만든다. 키 저장소가 찼으면 OutOfMemory다.
// crypto_secretstream_xchacha20poly1305_init_pull은 header에서 하위 키(HChaCha20)와 inonce를 만들고
// 카운터를 1로 둔다. init_push도 무작위 header를 만든 뒤 같은 계산을 하므로, 보내는 쪽도 header를
// TryFillRandom으로 만들고 이 함수를 쓴다(OS 난수 실패를 Fatal 대신 DependencyFailed로 돌려준다).
Result<void*> TryStartState(const SecretStreamKey& key, Span<const uint8> header) noexcept {
    auto slot = detail::TryAllocateSecretSlot(sizeof(SodiumState), SecretStreamStateAlignment);
    if (!slot) {
        return slot;
    }

    detail::EnsureSodium();
    detail::RequireSodium(
        crypto_secretstream_xchacha20poly1305_init_pull(
            SecretStreamStateOf(slot.Value()), header.Data(), detail::KeyAccess::Bytes(key).Data()),
        "Crypto: crypto_secretstream_xchacha20poly1305_init_pull failed");
    return slot;
}

void SecretStreamFreeState(void*& state) noexcept {
    if (state != nullptr) {
        detail::FreeSecretSlot(state);
        state = nullptr;
    }
}

} // namespace

// ---- 키 ----

Result<SecretStreamKey> SecretStreamKey::TryGenerate() noexcept {
    platform::DataIndependentTimingScope dit;
    auto key = SecretBytes::TryGenerate(SecretStreamKeyBytes);
    if (!key) {
        return Result<SecretStreamKey>::Failure(key.Error());
    }

    return Result<SecretStreamKey>::Success(SecretStreamKey(Move(key.Value())));
}

Result<SecretStreamKey> SecretStreamKey::TryImport(Span<const uint8> keyBytes) noexcept {
    platform::DataIndependentTimingScope dit;
    if (keyBytes.Size() != SecretStreamKeyBytes) {
        return Result<SecretStreamKey>::Failure(ErrorCode::InvalidArgument);
    }

    auto key = SecretBytes::TryCopyFrom(keyBytes);
    if (!key) {
        return Result<SecretStreamKey>::Failure(key.Error());
    }

    return Result<SecretStreamKey>::Success(SecretStreamKey(Move(key.Value())));
}

Result<SecretStreamKey> TryDeriveSecretStreamKey(const KdfKey& master, uint64 subkeyId,
                                                 KdfContext context) noexcept {
    platform::DataIndependentTimingScope dit;
    auto secret = detail::TryDeriveTypedSecret(
        master, subkeyId, context, detail::SecretStreamKeyTypeTag, SecretStreamKeyBytes);
    if (!secret) {
        return Result<SecretStreamKey>::Failure(secret.Error());
    }

    return Result<SecretStreamKey>::Success(
        detail::KeyAccess::Make<SecretStreamKey>(Move(secret.Value())));
}

Result<void> hazmat::TryExportKey(const SecretStreamKey& key, Span<uint8> output) noexcept {
    platform::DataIndependentTimingScope dit;
    const Span<const uint8> bytes = detail::KeyAccess::Bytes(key);
    if (bytes.IsEmpty()) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (output.Size() != SecretStreamKeyBytes || Overlaps(output, bytes)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    platform::MemoryCopy(output.Data(), bytes.Data(), SecretStreamKeyBytes);
    return Result<void>::Success();
}

// ---- Writer ----

SecretStreamWriter::~SecretStreamWriter() noexcept {
    Reset();
}

SecretStreamWriter::SecretStreamWriter(SecretStreamWriter&& other) noexcept
    : state_(other.state_), finished_(other.finished_) {
    other.state_ = nullptr;
    other.finished_ = false;
}

SecretStreamWriter& SecretStreamWriter::operator=(SecretStreamWriter&& other) noexcept {
    if (this != &other) {
        Reset();
        state_ = other.state_;
        finished_ = other.finished_;
        other.state_ = nullptr;
        other.finished_ = false;
    }

    return *this;
}

void SecretStreamWriter::Reset() noexcept {
    SecretStreamFreeState(state_);
    finished_ = false;
}

Result<SecretStreamWriter> SecretStreamWriter::TryCreate(const SecretStreamKey& key,
                                                         Span<uint8> header) noexcept {
    platform::DataIndependentTimingScope dit;
    if (key.IsEmpty()) {
        return Result<SecretStreamWriter>::Failure(ErrorCode::StaleHandle);
    }

    if (header.Size() != HeaderBytes) {
        return Result<SecretStreamWriter>::Failure(ErrorCode::InvalidArgument);
    }

    uint8 nonce[HeaderBytes];
    if (auto filled = TryFillRandom(Span<uint8>(nonce)); !filled) {
        return Result<SecretStreamWriter>::Failure(filled.Error());
    }

    auto state = TryStartState(key, Span<const uint8>(nonce));
    if (!state) {
        return Result<SecretStreamWriter>::Failure(state.Error());
    }

    platform::MemoryCopy(header.Data(), nonce, HeaderBytes);
    SecretStreamWriter writer;
    writer.state_ = state.Value();
    return Result<SecretStreamWriter>::Success(Move(writer));
}

Result<usize> SecretStreamWriter::TryPush(Span<const uint8> message,
                                          Span<const uint8> associatedData, SecretStreamTag tag,
                                          Span<uint8> output) noexcept {
    platform::DataIndependentTimingScope dit;
    if (state_ == nullptr) {
        return Result<usize>::Failure(ErrorCode::StaleHandle);
    }

    if (finished_ || !SecretStreamIsDefined(tag) || Overlaps(output, message) ||
        Overlaps(output, associatedData)) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    // libsodium은 MESSAGEBYTES_MAX를 넘는 길이에서 misuse handler(Fatal)로 간다. 부르기 전에
    // 거른다.
    if (static_cast<uint64>(message.Size()) > SecretStreamMaxMessageBytes ||
        output.Size() < message.Size() + OverheadBytes) {
        return Result<usize>::Failure(ErrorCode::CapacityExceeded);
    }

    detail::RequireSodium(crypto_secretstream_xchacha20poly1305_push(
                              SecretStreamStateOf(state_), output.Data(), nullptr,
                              detail::InputPointer(message), message.Size(),
                              detail::InputPointer(associatedData), associatedData.Size(),
                              static_cast<uint8>(tag)),
                          "Crypto: crypto_secretstream_xchacha20poly1305_push failed");
    if (tag == SecretStreamTag::Final) {
        finished_ = true;
    }

    return Result<usize>::Success(message.Size() + OverheadBytes);
}

void SecretStreamWriter::Rekey() noexcept {
    platform::DataIndependentTimingScope dit;
    if (state_ != nullptr) {
        crypto_secretstream_xchacha20poly1305_rekey(SecretStreamStateOf(state_));
    }
}

// ---- Reader ----

SecretStreamReader::~SecretStreamReader() noexcept {
    Reset();
}

SecretStreamReader::SecretStreamReader(SecretStreamReader&& other) noexcept
    : state_(other.state_), finished_(other.finished_), failed_(other.failed_) {
    other.state_ = nullptr;
    other.finished_ = false;
    other.failed_ = false;
}

SecretStreamReader& SecretStreamReader::operator=(SecretStreamReader&& other) noexcept {
    if (this != &other) {
        Reset();
        state_ = other.state_;
        finished_ = other.finished_;
        failed_ = other.failed_;
        other.state_ = nullptr;
        other.finished_ = false;
        other.failed_ = false;
    }

    return *this;
}

void SecretStreamReader::Reset() noexcept {
    SecretStreamFreeState(state_);
    finished_ = false;
    failed_ = false;
}

Result<SecretStreamReader> SecretStreamReader::TryCreate(const SecretStreamKey& key,
                                                         Span<const uint8> header) noexcept {
    platform::DataIndependentTimingScope dit;
    if (key.IsEmpty()) {
        return Result<SecretStreamReader>::Failure(ErrorCode::StaleHandle);
    }

    if (header.Size() != SecretStreamWriter::HeaderBytes) {
        return Result<SecretStreamReader>::Failure(ErrorCode::AuthenticationFailed);
    }

    auto state = TryStartState(key, header);
    if (!state) {
        return Result<SecretStreamReader>::Failure(state.Error());
    }

    SecretStreamReader reader;
    reader.state_ = state.Value();
    return Result<SecretStreamReader>::Success(Move(reader));
}

Result<SecretStreamMessage> SecretStreamReader::Fail(Span<uint8> output) noexcept {
    failed_ = true;
    SecureZero(output);
    if (state_ != nullptr) {
        SecureZero(Span<uint8>(static_cast<uint8*>(state_), sizeof(SodiumState)));
    }

    return Result<SecretStreamMessage>::Failure(ErrorCode::AuthenticationFailed);
}

Result<SecretStreamMessage> SecretStreamReader::TryPull(Span<const uint8> sealed,
                                                        Span<const uint8> associatedData,
                                                        Span<uint8> output) noexcept {
    platform::DataIndependentTimingScope dit;
    if (state_ == nullptr) {
        return Result<SecretStreamMessage>::Failure(ErrorCode::StaleHandle);
    }

    if (Overlaps(output, sealed) || Overlaps(output, associatedData)) {
        return Result<SecretStreamMessage>::Failure(ErrorCode::InvalidArgument);
    }

    // 실패 뒤, Final 뒤의 입력, 길이 위반은 메시지에서 온 조건이다. libsodium은 MESSAGEBYTES_MAX를
    // 넘는 길이에서 misuse handler(Fatal)로 가므로 부르기 전에 거른다.
    constexpr usize Overhead = SecretStreamWriter::OverheadBytes;
    if (failed_ || finished_ || sealed.Size() < Overhead ||
        static_cast<uint64>(sealed.Size() - Overhead) > SecretStreamMaxMessageBytes) {
        return Fail(output);
    }

    const usize messageBytes = sealed.Size() - Overhead;
    if (output.Size() < messageBytes) {
        return Result<SecretStreamMessage>::Failure(ErrorCode::CapacityExceeded);
    }

    uint8 scratch = 0;
    unsigned long long written = 0;
    uint8 tag = 0;
    const int status = crypto_secretstream_xchacha20poly1305_pull(
        SecretStreamStateOf(state_), detail::OutputPointer(output, &scratch), &written, &tag,
        sealed.Data(), sealed.Size(), detail::InputPointer(associatedData), associatedData.Size());
    if (status != 0 || tag > static_cast<uint8>(SecretStreamTag::Final)) {
        return Fail(output);
    }

    if (tag == static_cast<uint8>(SecretStreamTag::Final)) {
        finished_ = true;
    }

    SecretStreamMessage message;
    message.bytes = static_cast<usize>(written);
    message.tag = static_cast<SecretStreamTag>(tag);
    return Result<SecretStreamMessage>::Success(message);
}

void SecretStreamReader::Rekey() noexcept {
    platform::DataIndependentTimingScope dit;
    if (state_ != nullptr && !failed_) {
        crypto_secretstream_xchacha20poly1305_rekey(SecretStreamStateOf(state_));
    }
}

Result<void> SecretStreamReader::TryFinish() noexcept {
    if (state_ == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (failed_ || !finished_) {
        failed_ = true;
        return Result<void>::Failure(ErrorCode::AuthenticationFailed);
    }

    return Result<void>::Success();
}

} // namespace rocks::crypto
