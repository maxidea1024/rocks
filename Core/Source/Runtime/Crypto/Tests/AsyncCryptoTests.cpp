#include <Core/Containers/Array.h>
#include <Core/Function.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Move.h>
#include <Crypto/Aead.h>
#include <Crypto/AsyncCrypto.h>
#include <Crypto/Bytes.h>
#include <Crypto/KeyRing.h>
#include <Crypto/Private/AsyncCryptoInternal.h>
#include <Crypto/Private/SegmentStreamInternal.h>
#include <Crypto/Secret.h>
#include <Crypto/SegmentStream.h>
#include <Platform/Atomics.h>
#include <Platform/Sync.h>
#include <Task/WorkerScheduler.h>
#include <TestSupport/Test.h>

#include <string.h>

using namespace rocks;
namespace async_crypto_tests_file {

using namespace rocks::crypto;

// 비동기 세그먼트 처리(Crypto/AsyncCrypto.h)의 시험이다(Docs/Design/Crypto.ko.md 9.7·12절).
// - byte 일치: 고정 salt(모듈 내부 경로 crypto::detail::TrySubmitSegmentEncryptWithSalt)로 만든
// 비동기
//   암호문이 동기 순차 암호기(crypto::detail::SegmentStreamFactory, 같은 salt)와 같다. 작업 수
//   1·3·8·기본, 세그먼트 경계 크기, --seed 무작위 크기. 비동기 복호가 원문을 되찾는다.
// - 오류 일치: 변조·다른 키·AD·절단·버전·짧은 입력·작은 출력에서 동기 경로(Peek → 크기 배치 →
//   TryOpen → TryOpenSegment)와 같은 오류이고 실패하면 출력 전체가 0이다. 제출 오류는 task·출력을
//   바꾸지 않는다.
// - 취소: 시작 전(의존 대기 중), 의존 실패, 진행 중(probe 확인 횟수, 실제 RequestCancel). 결과는
//   Cancelled이고 출력은 0이다.
// - 수명: 제출 뒤 키·KeyRing 항목을 버려도 결과가 같다. 진행 중인 task를 소멸하면 취소하고
// 기다린다.
//   의존으로 이어 붙인 작업은 성공일 때만 실행된다.
namespace {

constexpr uint8 SegmentLog2 = 12; // 4 KiB 세그먼트(세그먼트 수를 늘려 병렬 경로를 탄다)
constexpr usize Plain = (usize{1} << SegmentLog2) - SegmentTagBytes;
constexpr uint8 Fill = 0xA5;

struct Fixture {
    SystemAllocator allocator;
    WorkerScheduler scheduler{allocator};

    Fixture() noexcept {
        WorkerSchedulerConfig config;
        config.workerCount = 4;
        if (!scheduler.Start(config)) {
            platform::Fatal("AsyncCryptoTests: scheduler failed to start");
        }
    }
};

void GoldenSalt(uint8 (&salt)[32]) noexcept {
    for (usize i = 0; i < 32; ++i) {
        salt[i] = static_cast<uint8>(0x40 + i);
    }
}

Span<const uint8> View(const Array<uint8>& bytes) noexcept {
    return Span<const uint8>(bytes.Data(), bytes.Size());
}

Span<uint8> Writable(Array<uint8>& bytes) noexcept {
    return Span<uint8>(bytes.Data(), bytes.Size());
}

bool Same(Span<const uint8> left, Span<const uint8> right) noexcept {
    return left.Size() == right.Size() &&
           (left.IsEmpty() || memcmp(left.Data(), right.Data(), left.Size()) == 0);
}

bool AllEqual(Span<const uint8> bytes, uint8 value) noexcept {
    for (uint8 byte : bytes) {
        if (byte != value) {
            return false;
        }
    }

    return true;
}

bool Resize(Array<uint8>& bytes, usize size, uint8 value) noexcept {
    if (!bytes.TryResize(size)) {
        return false;
    }

    if (size != 0) {
        memset(bytes.Data(), value, size);
    }

    return true;
}

void Pattern(Array<uint8>& bytes, uint64 seed) noexcept {
    uint64 state = seed;
    for (usize i = 0; i < bytes.Size(); ++i) {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        bytes[i] = static_cast<uint8>(state >> 56);
    }
}

// 동기 순차 암호기(고정 salt)로 봉인한다.
bool SyncSeal(const SegmentStreamKey& key, const SegmentStreamParams& params, Span<const uint8> ad,
              Span<const uint8> salt, Span<const uint8> plaintext, Array<uint8>& out) noexcept {
    auto total = TryGetSegmentStreamCiphertextBytes(params, plaintext.Size());
    if (!total || !Resize(out, static_cast<usize>(total.Value()), 0)) {
        return false;
    }

    auto encryptor = crypto::detail::SegmentStreamFactory::TryCreateEncryptor(key, params, ad, salt,
                                                                              GetSecretAllocator());
    if (!encryptor) {
        return false;
    }

    usize produced = 0;
    Span<const uint8> pending = plaintext;
    while (!pending.IsEmpty()) {
        auto step = encryptor.Value().TryProcess(pending, Writable(out).RightChop(produced));
        if (!step || (step.Value().consumed == 0 && step.Value().produced == 0)) {
            return false;
        }

        pending = pending.RightChop(step.Value().consumed);
        produced += step.Value().produced;
    }

    for (;;) {
        auto step = encryptor.Value().TryFinish(Writable(out).RightChop(produced));
        if (!step) {
            return false;
        }

        produced += step.Value().produced;
        if (step.Value().complete) {
            break;
        }
    }

    return produced == out.Size();
}

// 동기 사용자가 전체를 여는 순서(Peek → 크기 배치 → TryOpen → 출력 크기 → 세그먼트마다
// TryOpenSegment)의 오류다.
ErrorCode SyncOpenAll(const SegmentStreamKey& key, Span<const uint8> ad,
                      Span<const uint8> ciphertext, Span<uint8> plaintext) noexcept {
    auto info = TryPeekSegmentHeader(ciphertext.Left(SegmentHeaderBytes));
    if (!info) {
        return info.Error();
    }

    auto layout = TryGetSegmentLayout(info.Value().Params(), ciphertext.Size());
    if (!layout) {
        return layout.Error();
    }

    const SegmentLayout& value = layout.Value();
    auto reader = SegmentStreamRandomAccess::TryOpen(
        key, ad, ciphertext.Left(SegmentHeaderBytes),
        ciphertext.Mid(static_cast<usize>(value.lastSegmentOffset),
                       static_cast<usize>(value.lastSegmentBytes)),
        ciphertext.Size());
    if (!reader) {
        return reader.Error();
    }

    if (plaintext.Size() < value.plaintextBytes) {
        return ErrorCode::CapacityExceeded;
    }

    const usize plain = static_cast<usize>(value.segmentBytes) - SegmentTagBytes;
    for (uint64 index = 0; index < value.segmentCount; ++index) {
        const bool last = index + 1 == value.segmentCount;
        const usize sealed = static_cast<usize>(last ? value.lastSegmentBytes : value.segmentBytes);
        auto opened = reader.Value().TryOpenSegment(
            index,
            ciphertext.Mid(static_cast<usize>(SegmentHeaderBytes + index * value.segmentBytes),
                           sealed),
            plaintext.Mid(static_cast<usize>(index) * plain, sealed - SegmentTagBytes));
        if (!opened) {
            return opened.Error();
        }
    }

    return ErrorCode::None;
}

// 비동기 복호의 결과 오류다(성공이면 None). 실패면 출력 전체가 0인지도 본다.
ErrorCode AsyncOpenAll(WorkerScheduler& scheduler, const SegmentStreamKey& key,
                       Span<const uint8> ad, Span<const uint8> ciphertext, usize outputBytes,
                       bool& zeroed) noexcept {
    Array<uint8> plaintext(GetSecretAllocator());
    zeroed = false;
    if (!Resize(plaintext, outputBytes, Fill)) {
        return ErrorCode::OutOfMemory;
    }

    SegmentCryptoTask task;
    auto submitted =
        TrySubmitSegmentDecrypt(scheduler, task, key, ad, ciphertext, Writable(plaintext));
    if (!submitted) {
        return submitted.Error();
    }

    auto result = task.TryWaitResult();
    zeroed = AllEqual(View(plaintext), 0);
    return result ? ErrorCode::None : result.Error();
}

// N번째 확인부터 취소를 보고한다(여러 작업이 함께 센다).
struct CountdownProbe {
    uint32 after;
    uint32 checks = 0;

    static bool Check(void* raw) noexcept {
        auto& self = *static_cast<CountdownProbe*>(raw);
        return platform::AtomicFetchAdd(&self.checks, uint32{1}) >= self.after;
    }
};

// N번째 확인에서 멈춰 시험 스레드가 RequestCancel할 때까지 기다린다(취소는 작업 자신의 요청으로
// 관측된다).
struct PausingProbe {
    explicit PausingProbe(uint32 pauseAt) noexcept : at(pauseAt) {
    }

    uint32 at;
    uint32 checks = 0;
    platform::ManualResetEvent reached;
    platform::ManualResetEvent resume;

    static bool Check(void* raw) noexcept {
        auto& self = *static_cast<PausingProbe*>(raw);
        if (platform::AtomicFetchAdd(&self.checks, uint32{1}) == self.at) {
            self.reached.Signal();
            (void)self.resume.Wait(platform::InfiniteTimeout);
        }

        return false;
    }
};

WorkerTaskState StateOf(const WorkerTaskHandle& handle) noexcept {
    return handle.Status().Value().state;
}

} // namespace

TEST(CryptoAsyncSegmentMatchesSync) {
    Fixture fixture;
    WorkerScheduler& scheduler = fixture.scheduler;
    uint8 salt[32];
    GoldenSalt(salt);
    const uint8 ad[] = {'a', 's', 's', 'e', 't', '/', '7'};
    auto key = SegmentStreamKey::TryGenerate(KeyId{Guid{0xA11CE, 0xB0B}});
    REQUIRE(key);

    uint64 random = test::CurrentSeed() * 0x9E3779B97F4A7C15ull + 1;
    const usize randomSize = static_cast<usize>((random >> 17) % (Plain * 40));
    const usize sizes[] = {0,         1,         Plain - 1,      Plain,
                           Plain + 1, 2 * Plain, 37 * Plain + 5, randomSize};
    SegmentStreamAlgorithm algorithms[2] = {SegmentStreamAlgorithm::ChaCha20Poly1305};
    usize algorithmCount = 1;
    if (TryGetAeadInfo(AeadAlgorithm::Aes256Gcm).Value().available) {
        algorithms[algorithmCount++] = SegmentStreamAlgorithm::Aes256Gcm;
    }

    const uint32 taskCounts[] = {0, 1, 3, SegmentCryptoMaxTasks};
    for (usize a = 0; a < algorithmCount; ++a) {
        const SegmentStreamParams params{algorithms[a], SegmentLog2};
        for (usize size : sizes) {
            Array<uint8> plaintext(GetSecretAllocator());
            REQUIRE(Resize(plaintext, size, 0));
            Pattern(plaintext, size + a);
            Array<uint8> expected(GetSecretAllocator());
            REQUIRE(SyncSeal(key.Value(), params, Span<const uint8>(ad), Span<const uint8>(salt),
                             View(plaintext), expected));

            for (uint32 tasks : taskCounts) {
                // 출력 뒤 꼬리는 건드리지 않는다.
                Array<uint8> ciphertext(GetSecretAllocator());
                REQUIRE(Resize(ciphertext, expected.Size() + 7, 0xCC));
                SegmentCryptoTask sealing;
                REQUIRE(crypto::detail::TrySubmitSegmentEncryptWithSalt(
                    scheduler, sealing, key.Value(), params, Span<const uint8>(ad), View(plaintext),
                    Writable(ciphertext), SegmentCryptoOptions{tasks}, {}, Span<const uint8>(salt),
                    {}));
                const uint64 segments =
                    (size + Plain - 1) / Plain == 0 ? 1 : (size + Plain - 1) / Plain;
                const uint32 limit = tasks == 0 ? 4 : tasks;
                CHECK(sealing.Handles().Size() == (segments < limit ? segments : limit));
                auto sealed = sealing.TryWaitResult();
                REQUIRE(sealed);
                CHECK(sealed.Value() == expected.Size());
                CHECK(Same(View(ciphertext).Left(expected.Size()), View(expected)));
                CHECK(AllEqual(View(ciphertext).RightChop(expected.Size()), 0xCC));

                Array<uint8> opened(GetSecretAllocator());
                REQUIRE(Resize(opened, size + 3, Fill));
                SegmentCryptoTask opening;
                REQUIRE(TrySubmitSegmentDecrypt(scheduler, opening, key.Value(),
                                                Span<const uint8>(ad), View(expected),
                                                Writable(opened), SegmentCryptoOptions{tasks}));
                auto plain = opening.TryWaitResult();
                REQUIRE(plain);
                CHECK(plain.Value() == size);
                CHECK(Same(View(opened).Left(size), View(plaintext)));
                CHECK(AllEqual(View(opened).RightChop(size), Fill));
            }
        }
    }

    // 공개 경로(OS 난수 salt)도 동기 임의 접근으로 열린다. 64 KiB 세그먼트 여러 개.
    Array<uint8> large(GetSecretAllocator());
    REQUIRE(Resize(large, (usize{1} << 20) + 12345, 0));
    Pattern(large, 99);
    const SegmentStreamParams wide{};
    auto total = TryGetSegmentStreamCiphertextBytes(wide, large.Size());
    REQUIRE(total);
    Array<uint8> sealedLarge(GetSecretAllocator());
    REQUIRE(Resize(sealedLarge, static_cast<usize>(total.Value()), 0));
    SegmentCryptoTask sealing;
    REQUIRE(TrySubmitSegmentEncrypt(scheduler, sealing, key.Value(), wide, Span<const uint8>(ad),
                                    View(large), Writable(sealedLarge)));
    REQUIRE(sealing.TryWaitResult());
    Array<uint8> openedLarge(GetSecretAllocator());
    REQUIRE(Resize(openedLarge, large.Size(), 0));
    CHECK(SyncOpenAll(key.Value(), Span<const uint8>(ad), View(sealedLarge),
                      Writable(openedLarge)) == ErrorCode::None);
    CHECK(Same(View(openedLarge), View(large)));
}

TEST(CryptoAsyncSegmentErrorsMatchSync) {
    Fixture fixture;
    WorkerScheduler& scheduler = fixture.scheduler;
    uint8 salt[32];
    GoldenSalt(salt);
    const uint8 ad[] = {'s', 'a', 'v', 'e'};
    const uint8 otherAd[] = {'s', 'a', 'v', 'f'};
    const KeyId id{Guid{7, 7}};
    auto key = SegmentStreamKey::TryGenerate(id);
    auto otherId = SegmentStreamKey::TryGenerate(KeyId{Guid{7, 8}});
    auto sameIdOtherBytes = SegmentStreamKey::TryGenerate(id);
    REQUIRE(key && otherId && sameIdOtherBytes);

    const SegmentStreamParams params{SegmentStreamAlgorithm::ChaCha20Poly1305, SegmentLog2};
    Array<uint8> plaintext(GetSecretAllocator());
    REQUIRE(Resize(plaintext, 5 * Plain + 100, 0));
    Pattern(plaintext, 5);
    Array<uint8> good(GetSecretAllocator());
    REQUIRE(SyncSeal(key.Value(), params, Span<const uint8>(ad), Span<const uint8>(salt),
                     View(plaintext), good));

    // 변형마다 동기와 비동기의 오류가 같고, 비동기 실패는 출력 전체를 지운다.
    const auto compare = [&](const SegmentStreamKey& useKey, Span<const uint8> useAd,
                             Span<const uint8> ciphertext, usize outputBytes, ErrorCode expected) {
        Array<uint8> scratch(GetSecretAllocator());
        REQUIRE(Resize(scratch, outputBytes, 0));
        const ErrorCode sync = SyncOpenAll(useKey, useAd, ciphertext, Writable(scratch));
        bool zeroed = false;
        const ErrorCode async =
            AsyncOpenAll(scheduler, useKey, useAd, ciphertext, outputBytes, zeroed);
        CHECK(sync == expected);
        CHECK(async == expected);
        CHECK(zeroed);
    };

    Array<uint8> tampered(GetSecretAllocator());
    REQUIRE(tampered.TryAppend(View(good)));
    tampered[SegmentHeaderBytes + 2 * (Plain + SegmentTagBytes) + 9] ^= 0x01;
    compare(key.Value(), Span<const uint8>(ad), View(tampered), plaintext.Size(),
            ErrorCode::AuthenticationFailed);
    compare(otherId.Value(), Span<const uint8>(ad), View(good), plaintext.Size(),
            ErrorCode::AuthenticationFailed);
    compare(sameIdOtherBytes.Value(), Span<const uint8>(ad), View(good), plaintext.Size(),
            ErrorCode::AuthenticationFailed);
    compare(key.Value(), Span<const uint8>(otherAd), View(good), plaintext.Size(),
            ErrorCode::AuthenticationFailed);
    compare(key.Value(), Span<const uint8>(ad), View(good).Left(good.Size() - 1), plaintext.Size(),
            ErrorCode::AuthenticationFailed);
    compare(key.Value(), Span<const uint8>(ad), View(good).Left(10), plaintext.Size(),
            ErrorCode::AuthenticationFailed);
    compare(key.Value(), Span<const uint8>(ad), View(good), plaintext.Size() - 1,
            ErrorCode::CapacityExceeded);
    Array<uint8> versioned(GetSecretAllocator());
    REQUIRE(versioned.TryAppend(View(good)));
    versioned[4] = 2;
    compare(key.Value(), Span<const uint8>(ad), View(versioned), plaintext.Size(),
            ErrorCode::Unsupported);

    // 제출 오류는 task·출력을 바꾸지 않는다.
    Array<uint8> output(GetSecretAllocator());
    REQUIRE(Resize(output, good.Size(), Fill));
    SegmentCryptoTask task;
    SegmentStreamKey empty;
    CHECK(TrySubmitSegmentEncrypt(scheduler, task, empty, params, {}, View(plaintext),
                                  Writable(output))
              .Error() == ErrorCode::StaleHandle);
    CHECK(
        TrySubmitSegmentDecrypt(scheduler, task, empty, {}, View(good), Writable(output)).Error() ==
        ErrorCode::StaleHandle);
    CHECK(TrySubmitSegmentEncrypt(scheduler, task, key.Value(),
                                  {SegmentStreamAlgorithm::ChaCha20Poly1305, 11}, {},
                                  View(plaintext), Writable(output))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(TrySubmitSegmentEncrypt(scheduler, task, key.Value(), params, {}, View(output).Left(100),
                                  Writable(output))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(TrySubmitSegmentDecrypt(scheduler, task, key.Value(), {}, View(output),
                                  Writable(output).Mid(10, 100))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(TrySubmitSegmentEncrypt(scheduler, task, key.Value(), params, {}, View(plaintext),
                                  Writable(output).Left(good.Size() - 1))
              .Error() == ErrorCode::CapacityExceeded);
    CHECK(crypto::detail::TrySubmitSegmentEncryptWithSalt(scheduler, task, key.Value(), params, {},
                                                          View(plaintext), Writable(output), {}, {},
                                                          Span<const uint8>(salt).Left(31), {})
              .Error() == ErrorCode::InvalidArgument);
    if (!TryGetAeadInfo(AeadAlgorithm::Aes256Gcm).Value().available) {
        CHECK(TrySubmitSegmentEncrypt(scheduler, task, key.Value(),
                                      {SegmentStreamAlgorithm::Aes256Gcm, SegmentLog2}, {},
                                      View(plaintext), Writable(output))
                  .Error() == ErrorCode::Unsupported);
    }

    CHECK(task.Handles().IsEmpty());
    CHECK(task.TryGetResult().Error() == ErrorCode::StaleHandle);
    CHECK(task.TryWaitResult(0).Error() == ErrorCode::StaleHandle);
    CHECK(task.RequestCancel().Error() == ErrorCode::StaleHandle);
    CHECK(AllEqual(View(output), Fill));

    // 진행 중이면 Busy다(의존 이벤트로 붙잡아 둔다).
    auto gate = scheduler.TryCreateEvent();
    REQUIRE(gate);
    REQUIRE(TrySubmitSegmentEncrypt(scheduler, task, key.Value(), params, {}, View(plaintext),
                                    Writable(output), {},
                                    Span<const WorkerTaskHandle>(&gate.Value().Handle(), 1)));
    CHECK(task.TryGetResult().Error() == ErrorCode::Busy);
    CHECK(task.TryWaitResult(0).Error() == ErrorCode::Busy);
    CHECK(TrySubmitSegmentEncrypt(scheduler, task, key.Value(), params, {}, View(plaintext),
                                  Writable(output))
              .Error() == ErrorCode::Busy);
    REQUIRE(gate.Value().TryTrigger());
    auto done = task.TryWaitResult();
    REQUIRE(done);
    CHECK(done.Value() == good.Size());
}

TEST(CryptoAsyncSegmentCancellation) {
    Fixture fixture;
    WorkerScheduler& scheduler = fixture.scheduler;
    auto key = SegmentStreamKey::TryGenerate(KeyId{Guid{9, 1}});
    REQUIRE(key);
    const SegmentStreamParams params{SegmentStreamAlgorithm::ChaCha20Poly1305, SegmentLog2};
    constexpr usize Segments = 64;
    Array<uint8> plaintext(GetSecretAllocator());
    REQUIRE(Resize(plaintext, Segments * Plain, 0));
    Pattern(plaintext, 11);
    auto total = TryGetSegmentStreamCiphertextBytes(params, plaintext.Size());
    REQUIRE(total);
    Array<uint8> sealed(GetSecretAllocator());
    REQUIRE(Resize(sealed, static_cast<usize>(total.Value()), 0));
    {
        SegmentCryptoTask task;
        REQUIRE(TrySubmitSegmentEncrypt(scheduler, task, key.Value(), params, {}, View(plaintext),
                                        Writable(sealed)));
        REQUIRE(task.TryWaitResult());
    }

    Array<uint8> output(GetSecretAllocator());

    // 시작 전 취소: 의존이 끝나기 전에 취소하면 body 없이 Cancelled이고 출력은 0이다.
    {
        REQUIRE(Resize(output, plaintext.Size(), Fill));
        auto gate = scheduler.TryCreateEvent();
        REQUIRE(gate);
        SegmentCryptoTask task;
        REQUIRE(TrySubmitSegmentDecrypt(scheduler, task, key.Value(), {}, View(sealed),
                                        Writable(output), {},
                                        Span<const WorkerTaskHandle>(&gate.Value().Handle(), 1)));
        CHECK(task.TryGetResult().Error() == ErrorCode::Busy);
        REQUIRE(task.RequestCancel());
        CHECK(task.TryWaitResult().Error() == ErrorCode::Cancelled);
        CHECK(AllEqual(View(output), 0));
        for (const WorkerTaskHandle& handle : task.Handles()) {
            CHECK(StateOf(handle) == WorkerTaskState::Cancelled);
        }
    }

    // 의존 실패: 실행되지 않고 DependencyFailed, 출력은 0이다.
    {
        REQUIRE(Resize(output, sealed.Size(), Fill));
        auto gate = scheduler.TryCreateEvent();
        REQUIRE(gate);
        SegmentCryptoTask task;
        REQUIRE(TrySubmitSegmentEncrypt(scheduler, task, key.Value(), params, {}, View(plaintext),
                                        Writable(output), {},
                                        Span<const WorkerTaskHandle>(&gate.Value().Handle(), 1)));
        REQUIRE(gate.Value().TryFail(ErrorCode::NotFound));
        CHECK(task.TryWaitResult().Error() == ErrorCode::DependencyFailed);
        CHECK(AllEqual(View(output), 0));
    }

    // 진행 중 취소(probe): 세그먼트 경계에서 멈추고 나머지 작업도 멈춘다. 작업 수와 관계없이
    // 결과는 Cancelled이고 출력은 0이다.
    const uint32 taskCounts[] = {1, 4};
    for (uint32 tasks : taskCounts) {
        REQUIRE(Resize(output, plaintext.Size(), Fill));
        CountdownProbe probe{10};
        SegmentCryptoTask task;
        REQUIRE(crypto::detail::TrySubmitSegmentDecryptWithProbe(
            scheduler, task, key.Value(), {}, View(sealed), Writable(output),
            SegmentCryptoOptions{tasks}, {},
            crypto::detail::SegmentCryptoProbe{&CountdownProbe::Check, &probe}));
        CHECK(task.TryWaitResult().Error() == ErrorCode::Cancelled);
        CHECK(AllEqual(View(output), 0));
        CHECK(platform::AtomicLoad(&probe.checks) < Segments + tasks + 1);

        REQUIRE(Resize(output, sealed.Size(), Fill));
        CountdownProbe sealProbe{10};
        uint8 salt[32];
        GoldenSalt(salt);
        SegmentCryptoTask sealing;
        REQUIRE(crypto::detail::TrySubmitSegmentEncryptWithSalt(
            scheduler, sealing, key.Value(), params, {}, View(plaintext), Writable(output),
            SegmentCryptoOptions{tasks}, {}, Span<const uint8>(salt),
            crypto::detail::SegmentCryptoProbe{&CountdownProbe::Check, &sealProbe}));
        CHECK(sealing.TryWaitResult().Error() == ErrorCode::Cancelled);
        CHECK(AllEqual(View(output), 0));
    }

    // 진행 중 취소(RequestCancel): 작업이 세그먼트 몇 개를 처리한 뒤 취소를 요청하면 다음 경계에서
    // 멈추고, 작업 상태도 Cancelled다.
    {
        REQUIRE(Resize(output, plaintext.Size(), Fill));
        PausingProbe probe(5);
        SegmentCryptoTask task;
        REQUIRE(crypto::detail::TrySubmitSegmentDecryptWithProbe(
            scheduler, task, key.Value(), {}, View(sealed), Writable(output),
            SegmentCryptoOptions{1}, {},
            crypto::detail::SegmentCryptoProbe{&PausingProbe::Check, &probe}));
        REQUIRE(probe.reached.Wait(platform::InfiniteTimeout) == platform::WaitResult::Signaled);
        CHECK(task.TryGetResult().Error() == ErrorCode::Busy);
        REQUIRE(task.RequestCancel());
        probe.resume.Signal();
        CHECK(task.TryWaitResult().Error() == ErrorCode::Cancelled);
        CHECK(AllEqual(View(output), 0));
        REQUIRE(task.Handles().Size() == 1);
        CHECK(StateOf(task.Handles()[0]) == WorkerTaskState::Cancelled);
    }
}

TEST(CryptoAsyncSegmentLifetime) {
    Fixture fixture;
    WorkerScheduler& scheduler = fixture.scheduler;
    const KeyId id{Guid{11, 1}};
    const SegmentStreamParams params{SegmentStreamAlgorithm::ChaCha20Poly1305, SegmentLog2};
    Array<uint8> plaintext(GetSecretAllocator());
    REQUIRE(Resize(plaintext, 9 * Plain + 77, 0));
    Pattern(plaintext, 13);
    auto total = TryGetSegmentStreamCiphertextBytes(params, plaintext.Size());
    REQUIRE(total);
    const usize sealedBytes = static_cast<usize>(total.Value());

    uint8 keyBytes[SegmentStreamKeyBytes];
    for (usize i = 0; i < sizeof(keyBytes); ++i) {
        keyBytes[i] = static_cast<uint8>(0x20 + i);
    }

    // 제출 뒤 키를 없애도 결과는 같다(작업은 의존 이벤트 뒤에 시작한다).
    Array<uint8> sealed(GetSecretAllocator());
    REQUIRE(Resize(sealed, sealedBytes, 0));
    {
        auto gate = scheduler.TryCreateEvent();
        REQUIRE(gate);
        SegmentCryptoTask task;
        {
            auto key = SegmentStreamKey::TryImport(id, Span<const uint8>(keyBytes));
            REQUIRE(key);
            REQUIRE(TrySubmitSegmentEncrypt(
                scheduler, task, key.Value(), params, {}, View(plaintext), Writable(sealed), {},
                Span<const WorkerTaskHandle>(&gate.Value().Handle(), 1)));
        }

        REQUIRE(gate.Value().TryTrigger());
        REQUIRE(task.TryWaitResult());
    }

    // KeyRing lease로 제출하고 곧바로 KeyRing에서 빼고 lease를 놓아도(키가 지워져도) 복호된다.
    // 암호문은 의존 작업이 끝난 뒤에 채운다(파일 읽기 완료 → 복호, 헤더는 작업 시작 때 읽는다).
    {
        auto key = SegmentStreamKey::TryImport(id, Span<const uint8>(keyBytes));
        REQUIRE(key);
        KeyRing<SegmentStreamKey> ring;
        REQUIRE(ring.TryAdd(id, Move(key.Value())));
        auto lease = ring.TryAcquire(id);
        REQUIRE(lease);

        Array<uint8> arriving(GetSecretAllocator());
        REQUIRE(Resize(arriving, sealedBytes, 0));
        Array<uint8> opened(GetSecretAllocator());
        REQUIRE(Resize(opened, plaintext.Size(), Fill));
        auto read = scheduler.TryCreateEvent();
        REQUIRE(read);
        SegmentCryptoTask task;
        REQUIRE(TrySubmitSegmentDecrypt(scheduler, task, lease.Value().Key(), {}, View(arriving),
                                        Writable(opened), {},
                                        Span<const WorkerTaskHandle>(&read.Value().Handle(), 1)));
        REQUIRE(ring.TryRemove(id));
        lease.Value().Reset();
        memcpy(arriving.Data(), sealed.Data(), sealedBytes);
        REQUIRE(read.Value().TryTrigger());
        auto result = task.TryWaitResult();
        REQUIRE(result);
        CHECK(result.Value() == plaintext.Size());
        CHECK(Same(View(opened), View(plaintext)));

        // 이어 붙인 작업은 성공일 때만 실행된다.
        uint32 ran = 0;
        auto body = Function<void(WorkerTaskContext&)>::TryCreate(
            fixture.allocator,
            [&ran](WorkerTaskContext&) noexcept { platform::AtomicStore(&ran, uint32{1}); });
        REQUIRE(body);
        auto next = scheduler.Submit(Move(body.Value()), task.Handles());
        REQUIRE(next);
        REQUIRE(next.Value().Wait());
        CHECK(StateOf(next.Value()) == WorkerTaskState::Succeeded);
        CHECK(platform::AtomicLoad(&ran) == 1);
    }

    // 실패한 복호 뒤에 이어 붙인 작업은 실행되지 않는다(DependencyFailed).
    {
        auto key = SegmentStreamKey::TryImport(id, Span<const uint8>(keyBytes));
        REQUIRE(key);
        Array<uint8> broken(GetSecretAllocator());
        REQUIRE(broken.TryAppend(View(sealed)));
        broken[broken.Size() - 1] ^= 0x80;
        Array<uint8> opened(GetSecretAllocator());
        REQUIRE(Resize(opened, plaintext.Size(), Fill));
        SegmentCryptoTask task;
        REQUIRE(TrySubmitSegmentDecrypt(scheduler, task, key.Value(), {}, View(broken),
                                        Writable(opened)));
        uint32 ran = 0;
        auto body = Function<void(WorkerTaskContext&)>::TryCreate(
            fixture.allocator,
            [&ran](WorkerTaskContext&) noexcept { platform::AtomicStore(&ran, uint32{1}); });
        REQUIRE(body);
        auto next = scheduler.Submit(Move(body.Value()), task.Handles());
        REQUIRE(next);
        CHECK(task.TryWaitResult().Error() == ErrorCode::AuthenticationFailed);
        REQUIRE(next.Value().Wait());
        CHECK(next.Value().Status().Value().error == ErrorCode::DependencyFailed);
        CHECK(platform::AtomicLoad(&ran) == 0);
        CHECK(AllEqual(View(opened), 0));
    }

    // 진행 중인 task를 소멸하면 취소하고 기다린다. 출력은 0이 된다.
    {
        auto key = SegmentStreamKey::TryImport(id, Span<const uint8>(keyBytes));
        REQUIRE(key);
        Array<uint8> opened(GetSecretAllocator());
        REQUIRE(Resize(opened, plaintext.Size(), Fill));
        auto gate = scheduler.TryCreateEvent();
        REQUIRE(gate);
        {
            SegmentCryptoTask task;
            REQUIRE(TrySubmitSegmentDecrypt(
                scheduler, task, key.Value(), {}, View(sealed), Writable(opened), {},
                Span<const WorkerTaskHandle>(&gate.Value().Handle(), 1)));
        }

        CHECK(AllEqual(View(opened), 0));
    }
}
} // namespace async_crypto_tests_file
