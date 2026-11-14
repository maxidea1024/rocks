#include <Crypto/Hash.h>

#include <Crypto/Bytes.h>

#include "Sodium/SodiumRuntime.h"

#include <Core/Move.h>
#include <Platform/Fatal.h>

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

// 출력이 입력 조각이나 조각 배열과 한 byte라도 겹치는가(7절 제자리 규칙).
bool OutputOverlaps(Span<const Span<const uint8>> pieces, Span<uint8> output) noexcept {
    if (Overlaps(pieces, output)) {
        return true;
    }

    for (Span<const uint8> piece : pieces) {
        if (Overlaps(piece, output)) {
            return true;
        }
    }

    return false;
}

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

// 인자는 TryCreate가 이미 검사했으므로 libsodium이 실패하면 어댑터 결함이다.
void Require(int status, const char* message) noexcept {
    if (status != 0) {
        platform::Fatal(message);
    }
}

void InitState(HashAlgorithm algorithm, void* state, usize digestBytes) noexcept {
    switch (algorithm) {
    case HashAlgorithm::Blake2b:
        Require(crypto_generichash_init(static_cast<crypto_generichash_state*>(state), nullptr, 0,
                                        digestBytes),
                "Crypto: crypto_generichash_init failed");
        return;
    case HashAlgorithm::Sha512:
        Require(crypto_hash_sha512_init(static_cast<crypto_hash_sha512_state*>(state)),
                "Crypto: crypto_hash_sha512_init failed");
        return;
    case HashAlgorithm::Sha3_256:
        Require(crypto_hash_sha3256_init(static_cast<crypto_hash_sha3256_state*>(state)),
                "Crypto: crypto_hash_sha3256_init failed");
        return;
    case HashAlgorithm::Sha3_512:
        Require(crypto_hash_sha3512_init(static_cast<crypto_hash_sha3512_state*>(state)),
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
        Require(crypto_generichash_update(static_cast<crypto_generichash_state*>(state),
                                          input.Data(), input.Size()),
                "Crypto: crypto_generichash_update failed");
        return;
    case HashAlgorithm::Sha512:
        Require(crypto_hash_sha512_update(static_cast<crypto_hash_sha512_state*>(state),
                                          input.Data(), input.Size()),
                "Crypto: crypto_hash_sha512_update failed");
        return;
    case HashAlgorithm::Sha3_256:
        Require(crypto_hash_sha3256_update(static_cast<crypto_hash_sha3256_state*>(state),
                                           input.Data(), input.Size()),
                "Crypto: crypto_hash_sha3256_update failed");
        return;
    case HashAlgorithm::Sha3_512:
        Require(crypto_hash_sha3512_update(static_cast<crypto_hash_sha3512_state*>(state),
                                           input.Data(), input.Size()),
                "Crypto: crypto_hash_sha3512_update failed");
        return;
    }
}

void FinishState(HashAlgorithm algorithm, void* state, Span<uint8> digest) noexcept {
    switch (algorithm) {
    case HashAlgorithm::Blake2b:
        Require(crypto_generichash_final(static_cast<crypto_generichash_state*>(state),
                                         digest.Data(), digest.Size()),
                "Crypto: crypto_generichash_final failed");
        return;
    case HashAlgorithm::Sha512:
        Require(
            crypto_hash_sha512_final(static_cast<crypto_hash_sha512_state*>(state), digest.Data()),
            "Crypto: crypto_hash_sha512_final failed");
        return;
    case HashAlgorithm::Sha3_256:
        Require(crypto_hash_sha3256_final(static_cast<crypto_hash_sha3256_state*>(state),
                                          digest.Data()),
                "Crypto: crypto_hash_sha3256_final failed");
        return;
    case HashAlgorithm::Sha3_512:
        Require(crypto_hash_sha3512_final(static_cast<crypto_hash_sha3512_state*>(state),
                                          digest.Data()),
                "Crypto: crypto_hash_sha3512_final failed");
        return;
    }
}

void InitState(XofAlgorithm algorithm, void* state, uint8 domain) noexcept {
    switch (algorithm) {
    case XofAlgorithm::Shake128:
        Require(crypto_xof_shake128_init(static_cast<crypto_xof_shake128_state*>(state)),
                "Crypto: crypto_xof_shake128_init failed");
        return;
    case XofAlgorithm::Shake256:
        Require(crypto_xof_shake256_init(static_cast<crypto_xof_shake256_state*>(state)),
                "Crypto: crypto_xof_shake256_init failed");
        return;
    case XofAlgorithm::TurboShake128:
        Require(crypto_xof_turboshake128_init_with_domain(
                    static_cast<crypto_xof_turboshake128_state*>(state), domain),
                "Crypto: crypto_xof_turboshake128_init_with_domain failed");
        return;
    case XofAlgorithm::TurboShake256:
        Require(crypto_xof_turboshake256_init_with_domain(
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
        Require(crypto_xof_shake128_update(static_cast<crypto_xof_shake128_state*>(state),
                                           input.Data(), input.Size()),
                "Crypto: crypto_xof_shake128_update failed");
        return;
    case XofAlgorithm::Shake256:
        Require(crypto_xof_shake256_update(static_cast<crypto_xof_shake256_state*>(state),
                                           input.Data(), input.Size()),
                "Crypto: crypto_xof_shake256_update failed");
        return;
    case XofAlgorithm::TurboShake128:
        Require(crypto_xof_turboshake128_update(static_cast<crypto_xof_turboshake128_state*>(state),
                                                input.Data(), input.Size()),
                "Crypto: crypto_xof_turboshake128_update failed");
        return;
    case XofAlgorithm::TurboShake256:
        Require(crypto_xof_turboshake256_update(static_cast<crypto_xof_turboshake256_state*>(state),
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
        Require(crypto_xof_shake128_squeeze(static_cast<crypto_xof_shake128_state*>(state),
                                            output.Data(), output.Size()),
                "Crypto: crypto_xof_shake128_squeeze failed");
        return;
    case XofAlgorithm::Shake256:
        Require(crypto_xof_shake256_squeeze(static_cast<crypto_xof_shake256_state*>(state),
                                            output.Data(), output.Size()),
                "Crypto: crypto_xof_shake256_squeeze failed");
        return;
    case XofAlgorithm::TurboShake128:
        Require(
            crypto_xof_turboshake128_squeeze(static_cast<crypto_xof_turboshake128_state*>(state),
                                             output.Data(), output.Size()),
            "Crypto: crypto_xof_turboshake128_squeeze failed");
        return;
    case XofAlgorithm::TurboShake256:
        Require(
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

template <typename State>
Result<void> TurboShake(XofAlgorithm algorithm, Span<const uint8> input, Span<uint8> output,
                        uint8 domain) noexcept {
    if (!IsValidTurboShakeDomain(domain) || Overlaps(input, output)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    detail::EnsureSodium();
    State state;
    InitState(algorithm, &state, domain);
    UpdateState(algorithm, &state, input);
    SqueezeState(algorithm, &state, output);
    sodium_memzero(&state, sizeof(state));
    return Result<void>::Success();
}

template <typename State>
Result<void> Shake(XofAlgorithm algorithm, Span<const uint8> input, Span<uint8> output) noexcept {
    if (Overlaps(input, output)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    detail::EnsureSodium();
    State state;
    InitState(algorithm, &state, TurboShakeStandardDomain);
    UpdateState(algorithm, &state, input);
    SqueezeState(algorithm, &state, output);
    sodium_memzero(&state, sizeof(state));
    return Result<void>::Success();
}

// 할당한 상태를 지우고 돌려준다. 해제 실패는 상태가 그 allocator 것이 아니라는 결함이다.
void ReleaseState(Allocator* allocator, void* state, usize bytes) noexcept {
    SecureZero(Span<uint8>(static_cast<uint8*>(state), bytes));
    if (!allocator->Deallocate(state)) {
        platform::Fatal("Crypto: hash state was not owned by its allocator");
    }
}

} // namespace

// ---- 일회성 ----

Result<void> TryBlake2b(Span<const uint8> input, Span<uint8> digest) noexcept {
    const Span<const uint8> pieces[1] = {input};
    return TryBlake2b(Span<const Span<const uint8>>(pieces), digest);
}

Result<void> TryBlake2b(Span<const Span<const uint8>> pieces, Span<uint8> digest) noexcept {
    if (digest.Size() < Blake2bMinDigestBytes || digest.Size() > Blake2bMaxDigestBytes ||
        OutputOverlaps(pieces, digest)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    detail::EnsureSodium();
    HashPieces<crypto_generichash_state>(HashAlgorithm::Blake2b, pieces, digest, digest.Size());
    return Result<void>::Success();
}

Sha512Digest Sha512(Span<const uint8> input) noexcept {
    return FixedHash<Sha512Digest, crypto_hash_sha512_state>(HashAlgorithm::Sha512, input);
}

Sha512Digest Sha512(Span<const Span<const uint8>> pieces) noexcept {
    return FixedHash<Sha512Digest, crypto_hash_sha512_state>(HashAlgorithm::Sha512, pieces);
}

Sha3_256Digest Sha3_256(Span<const uint8> input) noexcept {
    return FixedHash<Sha3_256Digest, crypto_hash_sha3256_state>(HashAlgorithm::Sha3_256, input);
}

Sha3_256Digest Sha3_256(Span<const Span<const uint8>> pieces) noexcept {
    return FixedHash<Sha3_256Digest, crypto_hash_sha3256_state>(HashAlgorithm::Sha3_256, pieces);
}

Sha3_512Digest Sha3_512(Span<const uint8> input) noexcept {
    return FixedHash<Sha3_512Digest, crypto_hash_sha3512_state>(HashAlgorithm::Sha3_512, input);
}

Sha3_512Digest Sha3_512(Span<const Span<const uint8>> pieces) noexcept {
    return FixedHash<Sha3_512Digest, crypto_hash_sha3512_state>(HashAlgorithm::Sha3_512, pieces);
}

Result<void> TryShake128(Span<const uint8> input, Span<uint8> output) noexcept {
    return Shake<crypto_xof_shake128_state>(XofAlgorithm::Shake128, input, output);
}

Result<void> TryShake256(Span<const uint8> input, Span<uint8> output) noexcept {
    return Shake<crypto_xof_shake256_state>(XofAlgorithm::Shake256, input, output);
}

Result<void> TryTurboShake128(Span<const uint8> input, Span<uint8> output, uint8 domain) noexcept {
    return TurboShake<crypto_xof_turboshake128_state>(XofAlgorithm::TurboShake128, input, output,
                                                      domain);
}

Result<void> TryTurboShake256(Span<const uint8> input, Span<uint8> output, uint8 domain) noexcept {
    return TurboShake<crypto_xof_turboshake256_state>(XofAlgorithm::TurboShake256, input, output,
                                                      domain);
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
    const Span<const uint8> pieces[1] = {input};
    return TryUpdate(Span<const Span<const uint8>>(pieces));
}

Result<void> HashState::TryUpdate(Span<const Span<const uint8>> pieces) noexcept {
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
    if (state_ == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (finished_ || digest.Size() != digestBytes_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    FinishState(algorithm_, state_, digest);
    finished_ = true;
    return Result<void>::Success();
}

void HashState::Reset() noexcept {
    if (state_ != nullptr) {
        InitState(algorithm_, state_, digestBytes_);
        finished_ = false;
    }
}

void HashState::Release() noexcept {
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
    if (state_ == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (squeezing_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    UpdateState(algorithm_, state_, input);
    return Result<void>::Success();
}

Result<void> XofState::TrySqueeze(Span<uint8> output) noexcept {
    if (state_ == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    SqueezeState(algorithm_, state_, output);
    squeezing_ = true;
    return Result<void>::Success();
}

void XofState::Reset() noexcept {
    if (state_ != nullptr) {
        InitState(algorithm_, state_, domain_);
        squeezing_ = false;
    }
}

void XofState::Release() noexcept {
    if (state_ != nullptr) {
        ReleaseState(allocator_, state_, StateBytes(algorithm_));
        state_ = nullptr;
        allocator_ = nullptr;
    }
}

} // namespace rocks::crypto
