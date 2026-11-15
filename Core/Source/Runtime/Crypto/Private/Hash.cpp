#include <Crypto/Hash.h>

#include <Crypto/Bytes.h>

#include "AdapterCommon.h"
#include "Sodium/SodiumRuntime.h"

#include <Core/Move.h>
#include <Platform/Fatal.h>

#include <Platform/DataIndependentTiming.h>

#include <sodium.h>

namespace rocks::crypto {
namespace {

// libsodium 상태 구조체의 최대 정렬(BLAKE2b 64 byte)이다. 모든 상태를 이 정렬로 할당한다.
constexpr usize StateAlignment = 64;
constexpr uint8 TurboShakeStandardDomain = 0x1F;

static_assert(alignof(crypto_generichash_state) <= StateAlignment);
static_assert(alignof(crypto_hash_sha512_state) <= StateAlignment);
static_assert(alignof(crypto_hash_sha3256_state) <= StateAlignment);
static_assert(alignof(crypto_hash_sha3512_state) <= StateAlignment);
static_assert(alignof(crypto_xof_shake128_state) <= StateAlignment);
static_assert(alignof(crypto_xof_shake256_state) <= StateAlignment);
static_assert(alignof(crypto_xof_turboshake128_state) <= StateAlignment);
static_assert(alignof(crypto_xof_turboshake256_state) <= StateAlignment);
static_assert(Blake2bMinDigestBytes == crypto_generichash_BYTES_MIN &&
              Blake2bMaxDigestBytes == crypto_generichash_BYTES_MAX &&
              Blake2bDefaultDigestBytes == crypto_generichash_BYTES);

bool IsValidTurboShakeDomain(uint8 domain) noexcept {
    return domain >= 0x01 && domain <= 0x7F;
}

// ---- 증분 상태 공통 ----

bool IsDefined(HashAlgorithm algorithm) noexcept {
    return algorithm == HashAlgorithm::Blake2b || algorithm == HashAlgorithm::Sha512 ||
           algorithm == HashAlgorithm::Sha3_256 || algorithm == HashAlgorithm::Sha3_512;
}

bool IsDefined(XofAlgorithm algorithm) noexcept {
    return algorithm == XofAlgorithm::Shake128 || algorithm == XofAlgorithm::Shake256 ||
           algorithm == XofAlgorithm::TurboShake128 || algorithm == XofAlgorithm::TurboShake256;
}

usize StateBytes(HashAlgorithm algorithm) noexcept {
    switch (algorithm) {
    case HashAlgorithm::Blake2b:
        return sizeof(crypto_generichash_state);
    case HashAlgorithm::Sha512:
        return sizeof(crypto_hash_sha512_state);
    case HashAlgorithm::Sha3_256:
        return sizeof(crypto_hash_sha3256_state);
    case HashAlgorithm::Sha3_512:
        return sizeof(crypto_hash_sha3512_state);
    }

    return 0;
}

usize StateBytes(XofAlgorithm algorithm) noexcept {
    switch (algorithm) {
    case XofAlgorithm::Shake128:
        return sizeof(crypto_xof_shake128_state);
    case XofAlgorithm::Shake256:
        return sizeof(crypto_xof_shake256_state);
    case XofAlgorithm::TurboShake128:
        return sizeof(crypto_xof_turboshake128_state);
    case XofAlgorithm::TurboShake256:
        return sizeof(crypto_xof_turboshake256_state);
    }

    return 0;
}

// 고정 길이 알고리즘의 digest 길이다. BLAKE2b는 0(가변)이다.
usize FixedDigestBytes(HashAlgorithm algorithm) noexcept {
    switch (algorithm) {
    case HashAlgorithm::Blake2b:
        return 0;
    case HashAlgorithm::Sha512:
        return crypto_hash_sha512_BYTES;
    case HashAlgorithm::Sha3_256:
        return crypto_hash_sha3256_BYTES;
    case HashAlgorithm::Sha3_512:
        return crypto_hash_sha3512_BYTES;
    }

    return 0;
}

void InitState(HashAlgorithm algorithm, void* state, usize digestBytes) noexcept {
    switch (algorithm) {
    case HashAlgorithm::Blake2b:
        detail::RequireSodium(crypto_generichash_init(static_cast<crypto_generichash_state*>(state),
                                                      nullptr, 0, digestBytes),
                              "Crypto: crypto_generichash_init failed");
        return;
    case HashAlgorithm::Sha512:
        detail::RequireSodium(
            crypto_hash_sha512_init(static_cast<crypto_hash_sha512_state*>(state)),
            "Crypto: crypto_hash_sha512_init failed");
        return;
    case HashAlgorithm::Sha3_256:
        detail::RequireSodium(
            crypto_hash_sha3256_init(static_cast<crypto_hash_sha3256_state*>(state)),
            "Crypto: crypto_hash_sha3256_init failed");
        return;
    case HashAlgorithm::Sha3_512:
        detail::RequireSodium(
            crypto_hash_sha3512_init(static_cast<crypto_hash_sha3512_state*>(state)),
            "Crypto: crypto_hash_sha3512_init failed");
        return;
    }
}

void UpdateState(HashAlgorithm algorithm, void* state, Span<const uint8> input) noexcept {
    if (input.IsEmpty()) {
        return;
    }

    switch (algorithm) {
    case HashAlgorithm::Blake2b:
        detail::RequireSodium(
            crypto_generichash_update(static_cast<crypto_generichash_state*>(state), input.Data(),
                                      input.Size()),
            "Crypto: crypto_generichash_update failed");
        return;
    case HashAlgorithm::Sha512:
        detail::RequireSodium(
            crypto_hash_sha512_update(static_cast<crypto_hash_sha512_state*>(state), input.Data(),
                                      input.Size()),
            "Crypto: crypto_hash_sha512_update failed");
        return;
    case HashAlgorithm::Sha3_256:
        detail::RequireSodium(
            crypto_hash_sha3256_update(static_cast<crypto_hash_sha3256_state*>(state), input.Data(),
                                       input.Size()),
            "Crypto: crypto_hash_sha3256_update failed");
        return;
    case HashAlgorithm::Sha3_512:
        detail::RequireSodium(
            crypto_hash_sha3512_update(static_cast<crypto_hash_sha3512_state*>(state), input.Data(),
                                       input.Size()),
            "Crypto: crypto_hash_sha3512_update failed");
        return;
    }
}

void FinishState(HashAlgorithm algorithm, void* state, Span<uint8> digest) noexcept {
    switch (algorithm) {
    case HashAlgorithm::Blake2b:
        detail::RequireSodium(
            crypto_generichash_final(static_cast<crypto_generichash_state*>(state), digest.Data(),
                                     digest.Size()),
            "Crypto: crypto_generichash_final failed");
        return;
    case HashAlgorithm::Sha512:
        detail::RequireSodium(
            crypto_hash_sha512_final(static_cast<crypto_hash_sha512_state*>(state), digest.Data()),
            "Crypto: crypto_hash_sha512_final failed");
        return;
    case HashAlgorithm::Sha3_256:
        detail::RequireSodium(crypto_hash_sha3256_final(
                                  static_cast<crypto_hash_sha3256_state*>(state), digest.Data()),
                              "Crypto: crypto_hash_sha3256_final failed");
        return;
    case HashAlgorithm::Sha3_512:
        detail::RequireSodium(crypto_hash_sha3512_final(
                                  static_cast<crypto_hash_sha3512_state*>(state), digest.Data()),
                              "Crypto: crypto_hash_sha3512_final failed");
        return;
    }
}

void InitState(XofAlgorithm algorithm, void* state, uint8 domain) noexcept {
    switch (algorithm) {
    case XofAlgorithm::Shake128:
        detail::RequireSodium(
            crypto_xof_shake128_init(static_cast<crypto_xof_shake128_state*>(state)),
            "Crypto: crypto_xof_shake128_init failed");
        return;
    case XofAlgorithm::Shake256:
        detail::RequireSodium(
            crypto_xof_shake256_init(static_cast<crypto_xof_shake256_state*>(state)),
            "Crypto: crypto_xof_shake256_init failed");
        return;
    case XofAlgorithm::TurboShake128:
        detail::RequireSodium(crypto_xof_turboshake128_init_with_domain(
                                  static_cast<crypto_xof_turboshake128_state*>(state), domain),
                              "Crypto: crypto_xof_turboshake128_init_with_domain failed");
        return;
    case XofAlgorithm::TurboShake256:
        detail::RequireSodium(crypto_xof_turboshake256_init_with_domain(
                                  static_cast<crypto_xof_turboshake256_state*>(state), domain),
                              "Crypto: crypto_xof_turboshake256_init_with_domain failed");
        return;
    }
}

void UpdateState(XofAlgorithm algorithm, void* state, Span<const uint8> input) noexcept {
    if (input.IsEmpty()) {
        return;
    }

    switch (algorithm) {
    case XofAlgorithm::Shake128:
        detail::RequireSodium(
            crypto_xof_shake128_update(static_cast<crypto_xof_shake128_state*>(state), input.Data(),
                                       input.Size()),
            "Crypto: crypto_xof_shake128_update failed");
        return;
    case XofAlgorithm::Shake256:
        detail::RequireSodium(
            crypto_xof_shake256_update(static_cast<crypto_xof_shake256_state*>(state), input.Data(),
                                       input.Size()),
            "Crypto: crypto_xof_shake256_update failed");
        return;
    case XofAlgorithm::TurboShake128:
        detail::RequireSodium(
            crypto_xof_turboshake128_update(static_cast<crypto_xof_turboshake128_state*>(state),
                                            input.Data(), input.Size()),
            "Crypto: crypto_xof_turboshake128_update failed");
        return;
    case XofAlgorithm::TurboShake256:
        detail::RequireSodium(
            crypto_xof_turboshake256_update(static_cast<crypto_xof_turboshake256_state*>(state),
                                            input.Data(), input.Size()),
            "Crypto: crypto_xof_turboshake256_update failed");
        return;
    }
}

void SqueezeState(XofAlgorithm algorithm, void* state, Span<uint8> output) noexcept {
    if (output.IsEmpty()) {
        return;
    }

    switch (algorithm) {
    case XofAlgorithm::Shake128:
        detail::RequireSodium(
            crypto_xof_shake128_squeeze(static_cast<crypto_xof_shake128_state*>(state),
                                        output.Data(), output.Size()),
            "Crypto: crypto_xof_shake128_squeeze failed");
        return;
    case XofAlgorithm::Shake256:
        detail::RequireSodium(
            crypto_xof_shake256_squeeze(static_cast<crypto_xof_shake256_state*>(state),
                                        output.Data(), output.Size()),
            "Crypto: crypto_xof_shake256_squeeze failed");
        return;
    case XofAlgorithm::TurboShake128:
        detail::RequireSodium(
            crypto_xof_turboshake128_squeeze(static_cast<crypto_xof_turboshake128_state*>(state),
                                             output.Data(), output.Size()),
            "Crypto: crypto_xof_turboshake128_squeeze failed");
        return;
    case XofAlgorithm::TurboShake256:
        detail::RequireSodium(
            crypto_xof_turboshake256_squeeze(static_cast<crypto_xof_turboshake256_state*>(state),
                                             output.Data(), output.Size()),
            "Crypto: crypto_xof_turboshake256_squeeze failed");
        return;
    }
}

// 스택 상태로 조각 입력을 해시한다. 끝나면 상태를 지운다(입력이 비밀일 수 있다).
template <typename State, typename Algorithm>
void HashPieces(Algorithm algorithm, Span<const Span<const uint8>> pieces, Span<uint8> digest,
                usize digestBytes) noexcept {
    State state;
    InitState(algorithm, &state, digestBytes);
    for (Span<const uint8> piece : pieces) {
        UpdateState(algorithm, &state, piece);
    }

    FinishState(algorithm, &state, digest);
    sodium_memzero(&state, sizeof(state));
}

template <typename Digest, typename State>
Digest FixedHash(HashAlgorithm algorithm, Span<const Span<const uint8>> pieces) noexcept {
    detail::EnsureSodium();
    Digest digest;
    HashPieces<State>(algorithm, pieces, Span<uint8>(digest.bytes, Digest::Bytes), Digest::Bytes);
    return digest;
}

template <typename Digest, typename State>
Digest FixedHash(HashAlgorithm algorithm, Span<const uint8> input) noexcept {
    const Span<const uint8> pieces[1] = {input};
    return FixedHash<Digest, State>(algorithm, Span<const Span<const uint8>>(pieces));
}

// 스택 상태로 조각 입력을 짜낸다. domain은 TurboSHAKE만 쓴다(SHAKE에는 표준 값을 넘긴다). 끝나면
// 상태를 지운다.
template <typename State>
Result<void> Xof(XofAlgorithm algorithm, Span<const Span<const uint8>> pieces, Span<uint8> output,
                 uint8 domain) noexcept {
    if (!IsValidTurboShakeDomain(domain) || detail::OutputOverlaps(pieces, output)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    detail::EnsureSodium();
    State state;
    InitState(algorithm, &state, domain);
    for (Span<const uint8> piece : pieces) {
        UpdateState(algorithm, &state, piece);
    }

    SqueezeState(algorithm, &state, output);
    sodium_memzero(&state, sizeof(state));
    return Result<void>::Success();
}

void WipeState(void* state, usize bytes) noexcept {
    SecureZero(Span<uint8>(static_cast<uint8*>(state), bytes));
}

// 할당한 상태를 지우고 돌려준다. 해제 실패는 상태가 그 allocator 것이 아니라는 결함이다.
void ReleaseState(Allocator* allocator, void* state, usize bytes) noexcept {
    WipeState(state, bytes);
    if (!allocator->Deallocate(state)) {
        platform::Fatal("Crypto: hash state was not owned by its allocator");
    }
}

} // namespace

// ---- 일회성 ----

Result<void> TryBlake2b(Span<const uint8> input, Span<uint8> digest) noexcept {
    platform::DataIndependentTimingScope dit;
    const Span<const uint8> pieces[1] = {input};
    return TryBlake2b(Span<const Span<const uint8>>(pieces), digest);
}

Result<void> TryBlake2b(Span<const Span<const uint8>> pieces, Span<uint8> digest) noexcept {
    platform::DataIndependentTimingScope dit;
    if (digest.Size() < Blake2bMinDigestBytes || digest.Size() > Blake2bMaxDigestBytes ||
        detail::OutputOverlaps(pieces, digest)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    detail::EnsureSodium();
    HashPieces<crypto_generichash_state>(HashAlgorithm::Blake2b, pieces, digest, digest.Size());
    return Result<void>::Success();
}

Sha512Digest Sha512(Span<const uint8> input) noexcept {
    platform::DataIndependentTimingScope dit;
    return FixedHash<Sha512Digest, crypto_hash_sha512_state>(HashAlgorithm::Sha512, input);
}

Sha512Digest Sha512(Span<const Span<const uint8>> pieces) noexcept {
    platform::DataIndependentTimingScope dit;
    return FixedHash<Sha512Digest, crypto_hash_sha512_state>(HashAlgorithm::Sha512, pieces);
}

Sha3_256Digest Sha3_256(Span<const uint8> input) noexcept {
    platform::DataIndependentTimingScope dit;
    return FixedHash<Sha3_256Digest, crypto_hash_sha3256_state>(HashAlgorithm::Sha3_256, input);
}

Sha3_256Digest Sha3_256(Span<const Span<const uint8>> pieces) noexcept {
    platform::DataIndependentTimingScope dit;
    return FixedHash<Sha3_256Digest, crypto_hash_sha3256_state>(HashAlgorithm::Sha3_256, pieces);
}

Sha3_512Digest Sha3_512(Span<const uint8> input) noexcept {
    platform::DataIndependentTimingScope dit;
    return FixedHash<Sha3_512Digest, crypto_hash_sha3512_state>(HashAlgorithm::Sha3_512, input);
}

Sha3_512Digest Sha3_512(Span<const Span<const uint8>> pieces) noexcept {
    platform::DataIndependentTimingScope dit;
    return FixedHash<Sha3_512Digest, crypto_hash_sha3512_state>(HashAlgorithm::Sha3_512, pieces);
}

Result<void> TryShake128(Span<const uint8> input, Span<uint8> output) noexcept {
    platform::DataIndependentTimingScope dit;
    const Span<const uint8> pieces[1] = {input};
    return TryShake128(Span<const Span<const uint8>>(pieces), output);
}

Result<void> TryShake128(Span<const Span<const uint8>> pieces, Span<uint8> output) noexcept {
    platform::DataIndependentTimingScope dit;
    return Xof<crypto_xof_shake128_state>(XofAlgorithm::Shake128, pieces, output,
                                          TurboShakeStandardDomain);
}

Result<void> TryShake256(Span<const uint8> input, Span<uint8> output) noexcept {
    platform::DataIndependentTimingScope dit;
    const Span<const uint8> pieces[1] = {input};
    return TryShake256(Span<const Span<const uint8>>(pieces), output);
}

Result<void> TryShake256(Span<const Span<const uint8>> pieces, Span<uint8> output) noexcept {
    platform::DataIndependentTimingScope dit;
    return Xof<crypto_xof_shake256_state>(XofAlgorithm::Shake256, pieces, output,
                                          TurboShakeStandardDomain);
}

Result<void> TryTurboShake128(Span<const uint8> input, Span<uint8> output, uint8 domain) noexcept {
    platform::DataIndependentTimingScope dit;
    const Span<const uint8> pieces[1] = {input};
    return TryTurboShake128(Span<const Span<const uint8>>(pieces), output, domain);
}

Result<void> TryTurboShake128(Span<const Span<const uint8>> pieces, Span<uint8> output,
                              uint8 domain) noexcept {
    platform::DataIndependentTimingScope dit;
    return Xof<crypto_xof_turboshake128_state>(XofAlgorithm::TurboShake128, pieces, output, domain);
}

Result<void> TryTurboShake256(Span<const uint8> input, Span<uint8> output, uint8 domain) noexcept {
    platform::DataIndependentTimingScope dit;
    const Span<const uint8> pieces[1] = {input};
    return TryTurboShake256(Span<const Span<const uint8>>(pieces), output, domain);
}

Result<void> TryTurboShake256(Span<const Span<const uint8>> pieces, Span<uint8> output,
                              uint8 domain) noexcept {
    platform::DataIndependentTimingScope dit;
    return Xof<crypto_xof_turboshake256_state>(XofAlgorithm::TurboShake256, pieces, output, domain);
}

// ---- HashState ----

HashState::~HashState() noexcept {
    Release();
}

HashState::HashState(HashState&& other) noexcept
    : allocator_(other.allocator_), state_(other.state_), algorithm_(other.algorithm_),
      digestBytes_(other.digestBytes_), finished_(other.finished_) {
    other.allocator_ = nullptr;
    other.state_ = nullptr;
}

HashState& HashState::operator=(HashState&& other) noexcept {
    if (this != &other) {
        Release();
        allocator_ = other.allocator_;
        state_ = other.state_;
        algorithm_ = other.algorithm_;
        digestBytes_ = other.digestBytes_;
        finished_ = other.finished_;
        other.allocator_ = nullptr;
        other.state_ = nullptr;
    }

    return *this;
}

Result<HashState> HashState::TryCreate(HashAlgorithm algorithm, usize digestBytes,
                                       Allocator& allocator) noexcept {
    platform::DataIndependentTimingScope dit;
    if (!IsDefined(algorithm)) {
        return Result<HashState>::Failure(ErrorCode::InvalidArgument);
    }

    const usize fixed = FixedDigestBytes(algorithm);
    if (fixed == 0) {
        digestBytes = digestBytes == 0 ? Blake2bDefaultDigestBytes : digestBytes;
        if (digestBytes < Blake2bMinDigestBytes || digestBytes > Blake2bMaxDigestBytes) {
            return Result<HashState>::Failure(ErrorCode::InvalidArgument);
        }
    } else if (digestBytes != 0 && digestBytes != fixed) {
        return Result<HashState>::Failure(ErrorCode::InvalidArgument);
    } else {
        digestBytes = fixed;
    }

    detail::EnsureSodium();
    auto memory = allocator.Allocate(StateBytes(algorithm), StateAlignment);
    if (!memory) {
        return Result<HashState>::Failure(memory.Error());
    }

    HashState state;
    state.allocator_ = &allocator;
    state.state_ = memory.Value();
    state.algorithm_ = algorithm;
    state.digestBytes_ = digestBytes;
    InitState(algorithm, state.state_, digestBytes);
    return Result<HashState>::Success(Move(state));
}

Result<void> HashState::TryUpdate(Span<const uint8> input) noexcept {
    platform::DataIndependentTimingScope dit;
    const Span<const uint8> pieces[1] = {input};
    return TryUpdate(Span<const Span<const uint8>>(pieces));
}

Result<void> HashState::TryUpdate(Span<const Span<const uint8>> pieces) noexcept {
    platform::DataIndependentTimingScope dit;
    if (state_ == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (finished_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    for (Span<const uint8> piece : pieces) {
        UpdateState(algorithm_, state_, piece);
    }

    return Result<void>::Success();
}

Result<void> HashState::TryFinish(Span<uint8> digest) noexcept {
    platform::DataIndependentTimingScope dit;
    if (state_ == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (finished_ || digest.Size() != digestBytes_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 끝난 상태는 Reset 전까지 쓰지 않으므로 바로 지운다. SHA-3 상태는 순열이 가역이라 남겨 두면
    // rate보다 짧은 입력을 되살릴 수 있다.
    FinishState(algorithm_, state_, digest);
    WipeState(state_, StateBytes(algorithm_));
    finished_ = true;
    return Result<void>::Success();
}

void HashState::Reset() noexcept {
    platform::DataIndependentTimingScope dit;
    if (state_ != nullptr) {
        // libsodium SHA-512 init은 입력 버퍼를 지우지 않는다. 앞 입력이 남지 않게 먼저 지운다.
        WipeState(state_, StateBytes(algorithm_));
        InitState(algorithm_, state_, digestBytes_);
        finished_ = false;
    }
}

void HashState::Release() noexcept {
    platform::DataIndependentTimingScope dit;
    if (state_ != nullptr) {
        ReleaseState(allocator_, state_, StateBytes(algorithm_));
        state_ = nullptr;
        allocator_ = nullptr;
    }
}

// ---- XofState ----

XofState::~XofState() noexcept {
    Release();
}

XofState::XofState(XofState&& other) noexcept
    : allocator_(other.allocator_), state_(other.state_), algorithm_(other.algorithm_),
      domain_(other.domain_), squeezing_(other.squeezing_) {
    other.allocator_ = nullptr;
    other.state_ = nullptr;
}

XofState& XofState::operator=(XofState&& other) noexcept {
    if (this != &other) {
        Release();
        allocator_ = other.allocator_;
        state_ = other.state_;
        algorithm_ = other.algorithm_;
        domain_ = other.domain_;
        squeezing_ = other.squeezing_;
        other.allocator_ = nullptr;
        other.state_ = nullptr;
    }

    return *this;
}

Result<XofState> XofState::TryCreate(XofAlgorithm algorithm, uint8 domain,
                                     Allocator& allocator) noexcept {
    platform::DataIndependentTimingScope dit;
    const bool turbo =
        algorithm == XofAlgorithm::TurboShake128 || algorithm == XofAlgorithm::TurboShake256;
    if (!IsDefined(algorithm) || (turbo && !IsValidTurboShakeDomain(domain)) ||
        (!turbo && domain != TurboShakeStandardDomain)) {
        return Result<XofState>::Failure(ErrorCode::InvalidArgument);
    }

    detail::EnsureSodium();
    auto memory = allocator.Allocate(StateBytes(algorithm), StateAlignment);
    if (!memory) {
        return Result<XofState>::Failure(memory.Error());
    }

    XofState state;
    state.allocator_ = &allocator;
    state.state_ = memory.Value();
    state.algorithm_ = algorithm;
    state.domain_ = domain;
    InitState(algorithm, state.state_, domain);
    return Result<XofState>::Success(Move(state));
}

Result<void> XofState::TryUpdate(Span<const uint8> input) noexcept {
    platform::DataIndependentTimingScope dit;
    const Span<const uint8> pieces[1] = {input};
    return TryUpdate(Span<const Span<const uint8>>(pieces));
}

Result<void> XofState::TryUpdate(Span<const Span<const uint8>> pieces) noexcept {
    platform::DataIndependentTimingScope dit;
    if (state_ == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (squeezing_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    for (Span<const uint8> piece : pieces) {
        UpdateState(algorithm_, state_, piece);
    }

    return Result<void>::Success();
}

Result<void> XofState::TrySqueeze(Span<uint8> output) noexcept {
    platform::DataIndependentTimingScope dit;
    if (state_ == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    SqueezeState(algorithm_, state_, output);
    squeezing_ = true;
    return Result<void>::Success();
}

void XofState::Reset() noexcept {
    platform::DataIndependentTimingScope dit;
    if (state_ != nullptr) {
        WipeState(state_, StateBytes(algorithm_));
        InitState(algorithm_, state_, domain_);
        squeezing_ = false;
    }
}

void XofState::Release() noexcept {
    platform::DataIndependentTimingScope dit;
    if (state_ != nullptr) {
        ReleaseState(allocator_, state_, StateBytes(algorithm_));
        state_ = nullptr;
        allocator_ = nullptr;
    }
}

} // namespace rocks::crypto
