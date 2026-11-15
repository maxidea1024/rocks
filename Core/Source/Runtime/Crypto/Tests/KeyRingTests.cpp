#include <Core/Move.h>
#include <Crypto/Aead.h>
#include <Crypto/Bytes.h>
#include <Crypto/Kdf.h>
#include <Crypto/KeyBlob.h>
#include <Crypto/KeyRing.h>
#include <Crypto/Secret.h>
#include <Crypto/SegmentStream.h>
#include <Platform/Atomics.h>
#include <Platform/Fatal.h>
#include <Platform/Sync.h>
#include <Platform/Thread.h>
#include <TestSupport/Test.h>

#include <string.h>

using namespace rocks;
using namespace rocks::crypto;

// KeyRing·공급자(Crypto/KeyRing.h)의 계약 시험이다(Docs/Design/Crypto.ko.md 10.3절).
// - 등록·조회·제거·현재 키, 오류 순서(StaleHandle → InvalidArgument → AlreadyExists·NotFound).
// - 수명: 제거한 키와 KeyRing보다 오래 사는 lease, 마지막 lease가 끝날 때 키 저장소 반환.
// - 공급자: single-flight(성공 결과를 한 번만 가져온다), 실패·빈 키·ID 불일치·재진입, KeyBlob 표
//   공급자(KEK KeyRing, 변조 blob).
// - 동시성: 여러 스레드가 등록·조회·제거를 섞는다(--seed로 순서를 바꾼다).
namespace {

KeyId MakeId(uint64 high, uint64 low) noexcept {
    return KeyId{Guid{high, low}};
}

bool SameKeyBytes(const SegmentStreamKey& left, const SegmentStreamKey& right) noexcept {
    uint8 a[SegmentStreamKeyBytes];
    uint8 b[SegmentStreamKeyBytes];
    return hazmat::TryExportKey(left, Span<uint8>(a)) &&
           hazmat::TryExportKey(right, Span<uint8>(b)) &&
           ConstantTimeEquals(Span<const uint8>(a), Span<const uint8>(b));
}

SegmentStreamKey CopyKey(const SegmentStreamKey& key) noexcept {
    uint8 bytes[SegmentStreamKeyBytes];
    if (!hazmat::TryExportKey(key, Span<uint8>(bytes))) {
        return SegmentStreamKey();
    }

    auto copy = SegmentStreamKey::TryImport(key.Id(), Span<const uint8>(bytes));
    SecureZero(Span<uint8>(bytes));
    return copy ? Move(copy.Value()) : SegmentStreamKey();
}

// 공급자 기록이다. 부른 횟수를 세고, 정해 둔 ID만 키를 돌려준다.
struct CountingProvider {
    uint32 calls = 0;
    KeyId known;
    const SegmentStreamKey* source = nullptr;
    ErrorCode failure = ErrorCode::NotFound;
    platform::ManualResetEvent* release = nullptr;

    Result<SegmentStreamKey> Fetch(KeyId id) noexcept {
        platform::AtomicFetchAdd(&calls, uint32{1});
        if (release != nullptr) {
            (void)release->Wait(platform::InfiniteTimeout);
        }

        if (id == known && source != nullptr) {
            return Result<SegmentStreamKey>::Success(CopyKey(*source));
        }

        return Result<SegmentStreamKey>::Failure(failure);
    }
};

struct AcquireContext {
    KeyRing<SegmentStreamKey>* ring;
    KeyId id;
    ErrorCode result = ErrorCode::None;
};

void AcquireOnce(void* raw) noexcept {
    auto& context = *static_cast<AcquireContext*>(raw);
    auto lease = context.ring->TryAcquire(context.id);
    context.result = lease ? ErrorCode::None : lease.Error();
    if (lease && lease.Value().Id() != context.id) {
        context.result = ErrorCode::TypeMismatch;
    }
}

void StartOrDie(platform::Thread& thread, platform::Thread::Entry entry, void* context) noexcept {
    if (thread.Start(entry, context) != platform::ThreadStatus::Success) {
        platform::Fatal("KeyRingTests: thread failed to start");
    }
}

void JoinOrDie(platform::Thread& thread) noexcept {
    if (thread.Join() != platform::ThreadStatus::Success) {
        platform::Fatal("KeyRingTests: thread failed to join");
    }
}

} // namespace

TEST(CryptoKeyRingAddAcquireRemove) {
    const KeyId first = MakeId(1, 1);
    const KeyId second = MakeId(1, 2);
    KeyRing<SegmentStreamKey> ring;
    CHECK(ring.Count() == 0);

    // 오류 순서: 빈 키 StaleHandle → Nil·Id 불일치 InvalidArgument. 실패하면 키는 호출자에게
    // 남는다.
    SegmentStreamKey empty;
    CHECK(ring.TryAdd(first, Move(empty)).Error() == ErrorCode::StaleHandle);
    auto key = SegmentStreamKey::TryGenerate(first);
    REQUIRE(key);
    CHECK(ring.TryAdd(KeyId{}, Move(key.Value())).Error() == ErrorCode::InvalidArgument);
    CHECK(ring.TryAdd(second, Move(key.Value())).Error() == ErrorCode::InvalidArgument);
    CHECK(!key.Value().IsEmpty());

    const SegmentStreamKey reference = CopyKey(key.Value());
    REQUIRE(ring.TryAdd(first, Move(key.Value())));
    CHECK(key.Value().IsEmpty());
    CHECK(ring.Count() == 1);

    // 이미 있으면 AlreadyExists이고 새 키는 호출자에게 남는다. 기존 항목은 바뀌지 않는다.
    auto duplicate = SegmentStreamKey::TryGenerate(first);
    REQUIRE(duplicate);
    CHECK(ring.TryAdd(first, Move(duplicate.Value())).Error() == ErrorCode::AlreadyExists);
    CHECK(!duplicate.Value().IsEmpty());

    auto lease = ring.TryAcquire(first);
    REQUIRE(lease);
    CHECK(lease.Value().Id() == first);
    CHECK(SameKeyBytes(lease.Value().Key(), reference));
    CHECK(ring.TryAcquire(KeyId{}).Error() == ErrorCode::InvalidArgument);
    CHECK(ring.TryAcquire(second).Error() == ErrorCode::NotFound); // 공급자 없음

    // 현재 키: 등록된 키만, 제거하면 현재 키도 없어진다.
    CHECK(ring.TryAcquireCurrent().Error() == ErrorCode::NotFound);
    CHECK(ring.TrySetCurrent(KeyId{}).Error() == ErrorCode::InvalidArgument);
    CHECK(ring.TrySetCurrent(second).Error() == ErrorCode::NotFound);
    REQUIRE(ring.TrySetCurrent(first));
    CHECK(ring.CurrentId() == first);
    auto current = ring.TryAcquireCurrent();
    REQUIRE(current);
    CHECK(current.Value().Id() == first);
    ring.ClearCurrent();
    CHECK(ring.CurrentId().IsNil());
    REQUIRE(ring.TrySetCurrent(first));

    CHECK(ring.TryRemove(KeyId{}).Error() == ErrorCode::InvalidArgument);
    CHECK(ring.TryRemove(second).Error() == ErrorCode::NotFound);
    REQUIRE(ring.TryRemove(first));
    CHECK(ring.Count() == 0);
    CHECK(ring.CurrentId().IsNil());
    CHECK(ring.TryAcquire(first).Error() == ErrorCode::NotFound);
    // 제거한 키라도 받은 lease는 유효하다.
    CHECK(SameKeyBytes(lease.Value().Key(), reference));

    // 제거한 뒤에는 같은 ID로 다시 등록할 수 있다(키 회전으로 옛 키를 되살리는 경우).
    REQUIRE(ring.TryAdd(first, Move(duplicate.Value())));
    CHECK(ring.Count() == 1);

    KeyLease<SegmentStreamKey> none;
    CHECK(none.IsEmpty() && none.Id().IsNil());
}

TEST(CryptoKeyRingLeaseLifetime) {
    const KeyId id = MakeId(2, 1);
    // 처음 쓰는 경로의 지연 초기화가 숫자를 흔들지 않게 같은 일을 한 번 해 둔다.
    {
        auto warm = SegmentStreamKey::TryGenerate(id);
        REQUIRE(warm);
        uint8 header[SegmentHeaderBytes];
        CHECK(SegmentStreamParallelWriter::TryCreate(warm.Value(), {}, {}, 0, Span<uint8>(header))
                  .IsOk());
    }

    const SecretMemoryStatus before = GetSecretMemoryStatus();
    KeyLease<SegmentStreamKey> lease;
    KeyLease<SegmentStreamKey> copy;
    {
        KeyRing<SegmentStreamKey> ring;
        auto key = SegmentStreamKey::TryGenerate(id);
        REQUIRE(key);
        REQUIRE(ring.TryAdd(id, Move(key.Value())));
        auto acquired = ring.TryAcquire(id);
        REQUIRE(acquired);
        lease = acquired.Value();
        copy = lease;
        CHECK(GetSecretMemoryStatus().arenaUsedBytes > before.arenaUsedBytes);
    }

    // KeyRing이 먼저 사라져도 lease는 키를 쓸 수 있다(세그먼트 스트림을 만들어 본다).
    REQUIRE(!lease.IsEmpty() && lease.Id() == id);
    {
        uint8 header[SegmentHeaderBytes];
        CHECK(SegmentStreamParallelWriter::TryCreate(lease.Key(), {}, {}, 0, Span<uint8>(header))
                  .IsOk());
    }

    // 마지막 lease가 끝날 때 키를 지우고 키 저장소에 돌려준다.
    lease.Reset();
    CHECK(lease.IsEmpty());
    CHECK(!copy.IsEmpty());
    CHECK(GetSecretMemoryStatus().arenaUsedBytes > before.arenaUsedBytes);
    copy.Reset();
    CHECK(GetSecretMemoryStatus().arenaUsedBytes == before.arenaUsedBytes);
}

TEST(CryptoKeyRingProviderContract) {
    const KeyId known = MakeId(3, 1);
    const KeyId missing = MakeId(3, 2);
    auto source = SegmentStreamKey::TryGenerate(known);
    REQUIRE(source);

    KeyRing<SegmentStreamKey> ring;
    CountingProvider provider;
    provider.known = known;
    provider.source = &source.Value();
    ring.SetProvider(
        KeyRing<SegmentStreamKey>::Provider::CreateRaw(&provider, &CountingProvider::Fetch));

    // 없는 키는 공급자가 가져와 등록한다. 두 번째 요청은 표에서 나온다.
    auto fetched = ring.TryAcquire(known);
    REQUIRE(fetched);
    CHECK(SameKeyBytes(fetched.Value().Key(), source.Value()));
    CHECK(ring.Count() == 1);
    REQUIRE(ring.TryAcquire(known));
    CHECK(provider.calls == 1);

    // 공급자 실패는 그대로 돌려주고 기억하지 않는다.
    CHECK(ring.TryAcquire(missing).Error() == ErrorCode::NotFound);
    provider.failure = ErrorCode::DependencyFailed;
    CHECK(ring.TryAcquire(missing).Error() == ErrorCode::DependencyFailed);
    CHECK(provider.calls == 3);
    CHECK(ring.Count() == 1);

    // 공급자를 떼면 NotFound다.
    ring.SetProvider({});
    CHECK(ring.TryAcquire(missing).Error() == ErrorCode::NotFound);
    CHECK(provider.calls == 3);
}

namespace {

// 공급자 계약 위반·재진입을 만드는 공급자다.
struct MisbehavingProvider {
    KeyRing<SegmentStreamKey>* ring = nullptr;
    KeyId registered;
    KeyId other;
    ErrorCode reentryMissing = ErrorCode::None;
    ErrorCode reentryRegistered = ErrorCode::None;
    uint32 mode = 0;

    Result<SegmentStreamKey> Fetch(KeyId id) noexcept {
        if (mode == 0) {
            return Result<SegmentStreamKey>::Success(SegmentStreamKey());
        }

        if (mode == 1) {
            // 요청과 다른 ID를 품은 키(같은 KEK 아래 blob이 통째로 바뀐 경우)다.
            return SegmentStreamKey::TryGenerate(other);
        }

        // 공급자 안에서 같은 KeyRing을 부른다: 등록된 키는 되고, 없는 키는 Busy다.
        auto missing = ring->TryAcquire(other);
        reentryMissing = missing ? ErrorCode::None : missing.Error();
        auto present = ring->TryAcquire(registered);
        reentryRegistered = present ? ErrorCode::None : present.Error();
        return SegmentStreamKey::TryGenerate(id);
    }
};

} // namespace

TEST(CryptoKeyRingProviderMisuse) {
    const KeyId registered = MakeId(4, 1);
    const KeyId requested = MakeId(4, 2);
    KeyRing<SegmentStreamKey> ring;
    auto key = SegmentStreamKey::TryGenerate(registered);
    REQUIRE(key);
    REQUIRE(ring.TryAdd(registered, Move(key.Value())));

    MisbehavingProvider provider;
    provider.ring = &ring;
    provider.registered = registered;
    provider.other = MakeId(4, 3);
    ring.SetProvider(
        KeyRing<SegmentStreamKey>::Provider::CreateRaw(&provider, &MisbehavingProvider::Fetch));

    provider.mode = 0;
    CHECK(ring.TryAcquire(requested).Error() == ErrorCode::StaleHandle);
    provider.mode = 1;
    CHECK(ring.TryAcquire(requested).Error() == ErrorCode::AuthenticationFailed);
    CHECK(ring.Count() == 1);

    provider.mode = 2;
    auto fetched = ring.TryAcquire(requested);
    REQUIRE(fetched);
    CHECK(fetched.Value().Key().Id() == requested);
    CHECK(provider.reentryMissing == ErrorCode::Busy);
    CHECK(provider.reentryRegistered == ErrorCode::None);
    CHECK(ring.Count() == 2);

    // AeadKey처럼 ID를 품지 않는 키도 같은 규칙이다(빈 키 StaleHandle, Nil InvalidArgument).
    KeyRing<AeadKey> aeadRing;
    auto aead = AeadKey::TryGenerate(AeadAlgorithm::XChaCha20Poly1305);
    REQUIRE(aead);
    CHECK(aeadRing.TryAdd(KeyId{}, Move(aead.Value())).Error() == ErrorCode::InvalidArgument);
    REQUIRE(aeadRing.TryAdd(MakeId(4, 4), Move(aead.Value())));
    AeadKey moved;
    CHECK(aeadRing.TryAdd(MakeId(4, 5), Move(moved)).Error() == ErrorCode::StaleHandle);
}

TEST(CryptoKeyRingSingleFlight) {
    constexpr usize Threads = 8;
    const KeyId known = MakeId(5, 1);
    auto source = SegmentStreamKey::TryGenerate(known);
    REQUIRE(source);

    // 성공한 가져오기: 공급자가 막혀 있는 동안 모인 요청과 그 뒤 요청 모두 한 번의 호출로 끝난다
    // (늦게 온 요청은 표에서 찾는다).
    KeyRing<SegmentStreamKey> ring;
    platform::ManualResetEvent release;
    CountingProvider provider;
    provider.known = known;
    provider.source = &source.Value();
    provider.release = &release;
    ring.SetProvider(
        KeyRing<SegmentStreamKey>::Provider::CreateRaw(&provider, &CountingProvider::Fetch));

    AcquireContext contexts[Threads];
    platform::Thread threads[Threads];
    for (usize i = 0; i < Threads; ++i) {
        contexts[i] = AcquireContext{&ring, known};
        StartOrDie(threads[i], AcquireOnce, &contexts[i]);
    }

    platform::SleepFor(20);
    release.Signal();
    for (platform::Thread& thread : threads) {
        JoinOrDie(thread);
    }

    for (const AcquireContext& context : contexts) {
        CHECK(context.result == ErrorCode::None);
    }

    CHECK(provider.calls == 1);
    CHECK(ring.Count() == 1);

    // 실패한 가져오기: 같은 시점에 모인 요청은 같은 오류를 받는다. 늦게 온 요청은 다시 부른다.
    const KeyId failing = MakeId(5, 2);
    provider.failure = ErrorCode::DependencyFailed;
    provider.calls = 0;
    release.Reset();
    for (usize i = 0; i < Threads; ++i) {
        contexts[i] = AcquireContext{&ring, failing};
        StartOrDie(threads[i], AcquireOnce, &contexts[i]);
    }

    platform::SleepFor(20);
    release.Signal();
    for (platform::Thread& thread : threads) {
        JoinOrDie(thread);
    }

    for (const AcquireContext& context : contexts) {
        CHECK(context.result == ErrorCode::DependencyFailed);
    }

    CHECK(provider.calls >= 1 && provider.calls <= Threads);
    CHECK(ring.Count() == 1);
}

namespace {

constexpr usize ChurnThreads = 8;
constexpr usize ChurnKeys = 48;

struct ChurnContext {
    KeyRing<SegmentStreamKey>* ring;
    uint64 thread;
    uint64 seed;
    uint32 failures = 0;
};

// 자기 키를 등록하고, 다른 스레드의 키를 무작위로 빌리고, 짝수 키를 제거한다. 다른 스레드의 키는
// 아직 없거나 이미 제거됐을 수 있으므로 NotFound는 정상이다.
void Churn(void* raw) noexcept {
    auto& context = *static_cast<ChurnContext*>(raw);
    uint64 state = context.seed ^ (context.thread * 0x9E3779B97F4A7C15ull);
    const auto next = [&state]() noexcept {
        state += 0x9E3779B97F4A7C15ull;
        uint64 z = state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    };

    for (uint64 k = 0; k < ChurnKeys; ++k) {
        const KeyId id = MakeId(100 + context.thread, k + 1);
        auto key = SegmentStreamKey::TryGenerate(id);
        if (!key || !context.ring->TryAdd(id, Move(key.Value()))) {
            ++context.failures;
        }

        for (usize probe = 0; probe < 4; ++probe) {
            const uint64 owner = next() % ChurnThreads;
            const uint64 index = next() % ChurnKeys;
            const KeyId target = MakeId(100 + owner, index + 1);
            auto lease = context.ring->TryAcquire(target);
            if (lease ? lease.Value().Key().Id() != target : lease.Error() != ErrorCode::NotFound) {
                ++context.failures;
            }
        }

        if (k % 2 == 0 && !context.ring->TryRemove(id)) {
            ++context.failures;
        }
    }
}

} // namespace

TEST(CryptoKeyRingConcurrentChurn) {
    KeyRing<SegmentStreamKey> ring;
    ChurnContext contexts[ChurnThreads];
    platform::Thread threads[ChurnThreads];
    for (usize i = 0; i < ChurnThreads; ++i) {
        contexts[i] = ChurnContext{&ring, i, test::CurrentSeed()};
        StartOrDie(threads[i], Churn, &contexts[i]);
    }

    for (platform::Thread& thread : threads) {
        JoinOrDie(thread);
    }

    for (const ChurnContext& context : contexts) {
        CHECK(context.failures == 0);
    }

    // 홀수 키만 남는다.
    CHECK(ring.Count() == ChurnThreads * ChurnKeys / 2);
    for (uint64 thread = 0; thread < ChurnThreads; ++thread) {
        for (uint64 k = 0; k < ChurnKeys; ++k) {
            const ErrorCode error = ring.TryAcquire(MakeId(100 + thread, k + 1)).Error();
            CHECK(error == (k % 2 == 0 ? ErrorCode::NotFound : ErrorCode::None));
        }
    }
}

TEST(CryptoKeyRingKeyBlobProvider) {
    const KeyId kekId = MakeId(6, 1);
    const KeyId keyId = MakeId(6, 2);
    auto kek = KdfKey::TryGenerate();
    auto key = SegmentStreamKey::TryGenerate(keyId);
    REQUIRE(kek && key);

    uint8 blob[KeyBlobMaxBytes];
    auto written = TryWrapKey(kek.Value(), kekId, key.Value(), keyId, Span<uint8>(blob));
    REQUIRE(written);
    const Span<const uint8> stored = Span<const uint8>(blob).Left(written.Value());

    KeyRing<KdfKey> keks;
    KeyBlobProvider<SegmentStreamKey> blobs(keks);
    KeyRing<SegmentStreamKey> ring;
    ring.SetProvider(blobs.AsProvider());

    // 표에 넣을 때 형식·타입·방식을 본다(인증은 가져올 때).
    CHECK(blobs.TryAddBlob(stored.Left(10)).Error() == ErrorCode::AuthenticationFailed);
    CHECK(blobs.TryAddBlob(stored.Left(stored.Size() - 1)).Error() ==
          ErrorCode::AuthenticationFailed);
    auto aead = AeadKey::TryGenerate(AeadAlgorithm::XChaCha20Poly1305);
    REQUIRE(aead);
    uint8 aeadBlob[KeyBlobMaxBytes];
    auto aeadWritten =
        TryWrapKey(kek.Value(), kekId, aead.Value(), MakeId(6, 3), Span<uint8>(aeadBlob));
    REQUIRE(aeadWritten);
    CHECK(blobs.TryAddBlob(Span<const uint8>(aeadBlob).Left(aeadWritten.Value())).Error() ==
          ErrorCode::AuthenticationFailed);
    uint8 passwordBlob[KeyBlobMaxBytes];
    const uint8 password[] = {'p', 'w'};
    auto passwordWritten = TryWrapKeyWithPassword(Span<const uint8>(password), {1, 8 * 1024},
                                                  key.Value(), keyId, Span<uint8>(passwordBlob));
    REQUIRE(passwordWritten);
    CHECK(blobs.TryAddBlob(Span<const uint8>(passwordBlob).Left(passwordWritten.Value())).Error() ==
          ErrorCode::Unsupported);

    REQUIRE(blobs.TryAddBlob(stored));
    CHECK(blobs.TryAddBlob(stored).Error() == ErrorCode::AlreadyExists);

    // KEK가 없으면 KEK KeyRing의 NotFound가 그대로 나온다. KEK를 등록하면 풀린다.
    CHECK(ring.TryAcquire(keyId).Error() == ErrorCode::NotFound);
    REQUIRE(keks.TryAdd(kekId, Move(kek.Value())));
    auto lease = ring.TryAcquire(keyId);
    REQUIRE(lease);
    CHECK(SameKeyBytes(lease.Value().Key(), key.Value()));
    CHECK(lease.Value().Key().Id() == keyId);
    CHECK(ring.TryAcquire(MakeId(6, 9)).Error() == ErrorCode::NotFound);

    // 변조된 blob은 AuthenticationFailed다(commitment·tag).
    const KeyId tamperedId = MakeId(6, 4);
    auto other = SegmentStreamKey::TryGenerate(tamperedId);
    REQUIRE(other);
    auto kekLease = keks.TryAcquire(kekId);
    REQUIRE(kekLease);
    uint8 tampered[KeyBlobMaxBytes];
    auto tamperedWritten =
        TryWrapKey(kekLease.Value().Key(), kekId, other.Value(), tamperedId, Span<uint8>(tampered));
    REQUIRE(tamperedWritten);
    tampered[tamperedWritten.Value() - 1] ^= 0x01;
    REQUIRE(blobs.TryAddBlob(Span<const uint8>(tampered).Left(tamperedWritten.Value())));
    CHECK(ring.TryAcquire(tamperedId).Error() == ErrorCode::AuthenticationFailed);
    CHECK(ring.Count() == 1);

    REQUIRE(blobs.TryRemoveBlob(tamperedId));
    CHECK(blobs.TryRemoveBlob(tamperedId).Error() == ErrorCode::NotFound);
    ring.SetProvider({});
}
