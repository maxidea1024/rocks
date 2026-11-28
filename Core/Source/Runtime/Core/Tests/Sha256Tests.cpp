#include <Base/Compiler.h>
#include <Core/Encoding/Sha256.h>
#include <Core/Private/Encoding/Sha256Kernels.h>
#include <Platform/Cpu.h>
#include <TestSupport/Test.h>
#include <stdio.h>
#include <string.h>

using namespace rocks;
namespace sha256_tests_file {

// FIPS 180-4 / NIST CSRC 예제 벡터와 분할 입력 동치성을 확인한다.
namespace {

Span<const uint8> Bytes(const char* text) noexcept {
    return Span<const uint8>(reinterpret_cast<const uint8*>(text), strlen(text));
}

bool HexEquals(const Sha256Digest& digest, const char* expected) noexcept {
    char hex[Sha256Digest::HexBytes + 1];
    digest.ToHex(hex);
    return strcmp(hex, expected) == 0;
}

} // namespace

TEST(Sha256_MatchesNistVectors) {
    CHECK(HexEquals(Sha256::Hash(Bytes("")),
                    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK(HexEquals(Sha256::Hash(Bytes("abc")),
                    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK(HexEquals(Sha256::Hash(Bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
                    "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    CHECK(HexEquals(Sha256::Hash(Bytes("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
                                       "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu")),
                    "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"));
}

TEST(Sha256_MillionAsInChunks) {
    uint8 chunk[1000];
    memset(chunk, 'a', sizeof(chunk));
    Sha256 hasher;
    for (int index = 0; index < 1000; ++index) {
        hasher.Update(Span<const uint8>(chunk, sizeof(chunk)));
    }
    CHECK(HexEquals(hasher.Finish(),
                    "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

TEST(Sha256_SplitInputMatchesOneShotAndFinishResets) {
    uint8 data[300];
    for (usize index = 0; index < sizeof(data); ++index) {
        data[index] = static_cast<uint8>(index * 31u + 7u);
    }
    // 블록 경계(55·56·63·64·65)를 포함한 모든 길이와 분할 위치를 대조한다.
    for (usize length = 0; length <= sizeof(data); length += 1) {
        const Sha256Digest whole = Sha256::Hash(Span<const uint8>(data, length));
        for (usize split = 0; split <= length; split += length < 130 ? 1 : 37) {
            Sha256 hasher;
            hasher.Update(Span<const uint8>(data, split));
            hasher.Update(Span<const uint8>(data + split, length - split));
            if (!(hasher.Finish() == whole)) {
                CHECK(false);
                return;
            }
        }
    }
    Sha256 reused;
    reused.Update(Bytes("abc"));
    (void)reused.Finish();
    CHECK(HexEquals(reused.Finish(),
                    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
}

TEST(Sha256_HexRoundTripAndRejectsInvalidText) {
    const Sha256Digest digest = Sha256::Hash(Bytes("abc"));
    char hex[Sha256Digest::HexBytes + 1];
    digest.ToHex(hex);
    auto parsed = Sha256Digest::ParseHex(StringView(hex, Sha256Digest::HexBytes));
    REQUIRE(parsed);
    CHECK(parsed.Value() == digest);
    auto upper =
        Sha256Digest::ParseHex("BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD");
    REQUIRE(upper);
    CHECK(upper.Value() == digest);
    CHECK(Sha256Digest::ParseHex("abc").Error() == ErrorCode::InvalidArgument);
    CHECK(Sha256Digest::ParseHex("zz7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
              .Error() == ErrorCode::InvalidArgument);
}
namespace {

// 커널 대조 시험의 최대 블록 수와 최대 입력 길이다(CoreDataFoundations 6절 시험).
constexpr usize KernelMaxBlocks = 65;
constexpr usize KernelMaxBytes = KernelMaxBlocks * 64u;

constexpr uint32 InitialState[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                    0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};

// 스칼라 커널만으로 계산한 SHA-256이다(패딩 포함). 활성 커널과 대조하는 기준이다.
Sha256Digest ScalarHash(const uint8* data, usize length) noexcept {
    uint32 state[8];
    memcpy(state, InitialState, sizeof(state));
    const usize full = length / 64u;
    detail::Sha256BlocksScalar(state, data, full);
    uint8 tail[128] = {};
    const usize rest = length - full * 64u;
    memcpy(tail, data + full * 64u, rest);
    tail[rest] = 0x80u;
    const usize tailBlocks = rest + 9u <= 64u ? 1u : 2u;
    const uint64 bits = static_cast<uint64>(length) * 8u;
    for (uint32 index = 0; index < 8; ++index) {
        tail[tailBlocks * 64u - 1u - index] = static_cast<uint8>(bits >> (index * 8u));
    }

    detail::Sha256BlocksScalar(state, tail, tailBlocks);
    Sha256Digest digest;
    for (uint32 index = 0; index < 8; ++index) {
        digest.bytes[index * 4u] = static_cast<uint8>(state[index] >> 24);
        digest.bytes[index * 4u + 1u] = static_cast<uint8>(state[index] >> 16);
        digest.bytes[index * 4u + 2u] = static_cast<uint8>(state[index] >> 8);
        digest.bytes[index * 4u + 3u] = static_cast<uint8>(state[index]);
    }

    return digest;
}

// 결정적 의사 난수 byte다(xorshift32).
void FillPattern(uint8* data, usize bytes, uint32 seed) noexcept {
    uint32 value = seed;
    for (usize index = 0; index < bytes; ++index) {
        value ^= value << 13;
        value ^= value >> 17;
        value ^= value << 5;
        data[index] = static_cast<uint8>(value);
    }
}

// 스칼라로 압축한 뒤 state[0]의 한 bit를 뒤집는 결함 커널이다(자가 시험 거부 확인).
void BrokenBlocks(uint32* state, const uint8* blocks, usize count) noexcept {
    detail::Sha256BlocksScalar(state, blocks, count);
    state[0] ^= 1u;
}

} // namespace

TEST(Sha256_KernelSelectionFollowsCpuFeatures) {
    const platform::CpuFeatures none{};
    CHECK(detail::SelectSha256Kernel(none) == detail::Sha256Kernel::Scalar);
    platform::CpuFeatures shaOnly{};
    shaOnly.sha = true;
    CHECK(detail::SelectSha256Kernel(shaOnly) == detail::Sha256Kernel::Scalar);
    platform::CpuFeatures x64{};
    x64.sha = true;
    x64.sse42 = true;
    platform::CpuFeatures arm64{};
    arm64.neon = true;
    arm64.armSha2 = true;
#if ROCKS_ARCH_X64
    CHECK(detail::SelectSha256Kernel(x64) == detail::Sha256Kernel::X64Sha);
    CHECK(detail::SelectSha256Kernel(arm64) == detail::Sha256Kernel::Scalar);
    CHECK(detail::Sha256KernelBlocks(detail::Sha256Kernel::X64Sha) != nullptr);
    CHECK(detail::Sha256KernelBlocks(detail::Sha256Kernel::Arm64Sha2) == nullptr);
#elif ROCKS_ARCH_ARM64
    CHECK(detail::SelectSha256Kernel(arm64) == detail::Sha256Kernel::Arm64Sha2);
    CHECK(detail::SelectSha256Kernel(x64) == detail::Sha256Kernel::Scalar);
    CHECK(detail::Sha256KernelBlocks(detail::Sha256Kernel::Arm64Sha2) != nullptr);
    CHECK(detail::Sha256KernelBlocks(detail::Sha256Kernel::X64Sha) == nullptr);
#endif
    CHECK(detail::Sha256KernelBlocks(detail::Sha256Kernel::Scalar) == &detail::Sha256BlocksScalar);
    CHECK(strcmp(detail::Sha256KernelName(detail::Sha256Kernel::X64Sha), "X64Sha") == 0);
}

TEST(Sha256_SelfTestRejectsBrokenKernel) {
    CHECK(detail::CheckSha256Kernel(&detail::Sha256BlocksScalar));
    CHECK(!detail::CheckSha256Kernel(&BrokenBlocks));
    CHECK(!detail::CheckSha256Kernel(nullptr));
}

TEST(Sha256_ActiveKernelIsSelectedHardwareKernel) {
    // 하드웨어 커널이 바르면 자가 시험을 통과해 선택 결과가 그대로 활성 커널이다.
    const detail::Sha256Kernel selected = detail::SelectSha256Kernel(platform::QueryCpuFeatures());
    CHECK(detail::ActiveSha256Kernel() == selected);
    printf("  Sha256_ActiveKernelIsSelectedHardwareKernel: active kernel %s\n",
           detail::Sha256KernelName(detail::ActiveSha256Kernel()));
}

TEST(Sha256_HardwareKernelMatchesScalar) {
    const detail::Sha256Kernel selected = detail::SelectSha256Kernel(platform::QueryCpuFeatures());
    if (selected == detail::Sha256Kernel::Scalar) {
        printf("  Sha256_HardwareKernelMatchesScalar: skipped, CPU lacks SHA-NI+SSE4.2 (x64) or "
               "SHA2 (arm64)\n");
        return;
    }

    const detail::Sha256BlocksFunction blocks = detail::Sha256KernelBlocks(selected);
    REQUIRE(blocks != nullptr);
    CHECK(detail::CheckSha256Kernel(blocks));
    static uint8 data[KernelMaxBytes + 1];
    FillPattern(data, sizeof(data), 0x9e3779b9u);
    // 블록 수 1-65를 한 번에 넘기고, 정렬하지 않은 시작(+1)과 서로 다른 시작 상태로 대조한다.
    for (usize count = 1; count <= KernelMaxBlocks; ++count) {
        for (usize offset = 0; offset <= 1; ++offset) {
            uint32 expected[8];
            uint32 actual[8];
            for (uint32 index = 0; index < 8; ++index) {
                expected[index] = InitialState[index] ^ static_cast<uint32>(count * 0x01000193u);
                actual[index] = expected[index];
            }

            detail::Sha256BlocksScalar(expected, data + offset, count);
            blocks(actual, data + offset, count);
            if (memcmp(expected, actual, sizeof(expected)) != 0) {
                printf("  mismatch at %zu blocks, offset %zu\n", count, offset);
                CHECK(false);
                return;
            }
        }
    }
}

TEST(Sha256_PublicHashMatchesScalarReference) {
    // 공개 API는 활성 커널을 쓴다. 길이 0-4160 byte 모두 스칼라 기준과 같아야 한다.
    static uint8 data[KernelMaxBytes];
    FillPattern(data, sizeof(data), 0x2545f491u);
    for (usize length = 0; length <= KernelMaxBytes; ++length) {
        if (!(Sha256::Hash(Span<const uint8>(data, length)) == ScalarHash(data, length))) {
            printf("  mismatch at length %zu\n", length);
            CHECK(false);
            return;
        }
    }
}

} // namespace sha256_tests_file
