#include <Base/Compiler.h>
#include <Base/Endian.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/SystemAllocator.h>
#include <Crypto/Aead.h>
#include <Crypto/AesModes.h>
#include <Crypto/Bytes.h>
#include <Crypto/Crypto.h>
#include <Crypto/Private/AeadInternal.h>
#include <Crypto/Private/Aes/AesKernels.h>
#include <Crypto/Private/Aes/AesModesInternal.h>
#include <Crypto/Private/SecretArena.h>
#include <Platform/Cpu.h>
#include <Platform/Memory.h>
#include <TestSupport/Test.h>

#include <stdio.h>
#include <string.h>

using namespace rocks;
namespace aes_layer_tests_file {

using namespace rocks::crypto;

// AES 경량 계층(Docs/Design/Crypto.ko.md 5.2·5.3·12절)의 시험이다.
// - 시험 전용 스칼라 참조(FIPS-197 블록 암호화·키 확장, SP 800-38D 알고리즘 1의 bit 단위 GHASH,
//   32·64·128 bit 카운터 CTR, CBC-MAC)를 둔다. 테이블 S-box라 상수 시간이 아니며 출하하지 않는다.
//   스칼라 참조 자체는 FIPS-197 부록 C.1·C.3과 McGrew–Viega GCM 명세 시험 사례로 고정한다.
// - 스칼라 참조로 만든 커널 표(ScalarKernels)는 HW가 없어도 AesModes.cpp(GCM 조립·자가 시험)를
//   시험하게 한다. HW 커널(GetNativeAesKernels)은 스칼라 참조·libsodium AES-256-GCM과 대조한다.
// - HW 조건(x64 AES-NI·PCLMULQDQ·AVX, arm64 AES·PMULL)이 없으면 HW 부분은 건너뛰고 그렇다고
//   출력한다.
// 알려진 답의 출처:
// - GCM: McGrew–Viega "The Galois/Counter Mode of Operation (GCM)" 시험 사례 1–4(AES-128)·13–16
//   (AES-256), 96 bit IV. Python cryptography 50.0.2(OpenSSL)와 pycryptodome 4.4.0이 같은 값을
//   냄을 확인했다(2026-11-15).
// - 블록: FIPS-197 부록 C.1(AES-128)·C.3(AES-256), 부록 A.1·A.3 마지막 라운드 키.
namespace {

using crypto::detail::AesBlock;
using crypto::detail::AesBlockBytes;
using crypto::detail::AesCounterWidth;
using crypto::detail::AesKernels;
using crypto::detail::AesSchedule;

// GCM inc32 카운터(블록 끝 32 bit)의 최댓값이다. 다음 증가에서 0으로 돈다.
constexpr uint32 Counter32Max = ~uint32{0};

// ---- 공통 도우미 ----

// 시험 난수(SplitMix64)다. 씨앗은 test::CurrentSeed()에서 받아 실패를 재현할 수 있다.
struct TestRandom {
    uint64 state;

    uint64 Next() noexcept {
        state += 0x9e3779b97f4a7c15ull;
        uint64 z = state;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }

    usize Below(usize bound) noexcept {
        return static_cast<usize>(Next() % bound);
    }

    void Fill(Span<uint8> bytes) noexcept {
        for (uint8& byte : bytes) {
            byte = static_cast<uint8>(Next());
        }
    }
};

struct HexBytes {
    uint8 bytes[128];
    usize size = 0;

    explicit HexBytes(const char* hex) noexcept {
        auto decoded = TryFromHex(StringView(hex), Span<uint8>(bytes));
        size = decoded ? decoded.Value() : MaxSize;
    }

    bool IsValid() const noexcept {
        return size != MaxSize;
    }

    Span<const uint8> View() const noexcept {
        return Span<const uint8>(bytes, size);
    }
};

bool Same(Span<const uint8> left, Span<const uint8> right) noexcept {
    return left.Size() == right.Size() &&
           (left.IsEmpty() || platform::MemoryEquals(left.Data(), right.Data(), left.Size()));
}

bool AllZero(Span<const uint8> bytes) noexcept {
    for (uint8 byte : bytes) {
        if (byte != 0) {
            return false;
        }
    }

    return true;
}

Span<const uint8> BlockView(const AesBlock& block) noexcept {
    return Span<const uint8>(block.bytes);
}

// HW 커널 표다. HW 조건이 없으면 건너뛴다고 출력하고 nullptr다.
const AesKernels* NativeKernelsOrSkip(const char* test) noexcept {
    if (!crypto::detail::HasAesInstructions(platform::QueryCpuFeatures())) {
        printf("  %s: skipped HW kernels, CPU lacks AES-NI/PCLMULQDQ/AVX (x64) or AES/PMULL "
               "(arm64)\n",
               test);
        return nullptr;
    }

    return &crypto::detail::GetNativeAesKernels();
}

// ---- 스칼라 AES(FIPS-197) ----

constexpr uint8 Xtime(uint8 value) noexcept {
    return static_cast<uint8>((value << 1) ^ ((value & 0x80) != 0 ? 0x1b : 0x00));
}

constexpr uint8 GfMultiply(uint8 a, uint8 b) noexcept {
    uint8 product = 0;
    while (b != 0) {
        if ((b & 1) != 0) {
            product = static_cast<uint8>(product ^ a);
        }

        a = Xtime(a);
        b = static_cast<uint8>(b >> 1);
    }

    return product;
}

constexpr uint8 RotateLeft(uint8 value) noexcept {
    return static_cast<uint8>((value << 1) | (value >> 7));
}

// S-box를 정의대로 계산한다: GF(2^8) 곱셈 역원(x^254, 0은 0) 뒤 아핀 변환(FIPS-197 5.1.1).
struct SBoxTable {
    uint8 values[256] = {};

    constexpr SBoxTable() noexcept {
        for (usize x = 0; x < 256; ++x) {
            uint8 inverse = 0;
            if (x != 0) {
                inverse = 1;
                uint8 base = static_cast<uint8>(x);
                for (uint32 exponent = 254; exponent != 0; exponent >>= 1) {
                    if ((exponent & 1) != 0) {
                        inverse = GfMultiply(inverse, base);
                    }

                    base = GfMultiply(base, base);
                }
            }

            uint8 rotated = inverse;
            uint8 result = inverse;
            for (int i = 0; i < 4; ++i) {
                rotated = RotateLeft(rotated);
                result = static_cast<uint8>(result ^ rotated);
            }

            values[x] = static_cast<uint8>(result ^ 0x63);
        }
    }
};

constexpr SBoxTable SBox;

// FIPS-197 그림 7의 값 몇 개다.
static_assert(SBox.values[0x00] == 0x63 && SBox.values[0x01] == 0x7c && SBox.values[0x53] == 0xed &&
              SBox.values[0xff] == 0x16);

// FIPS-197 5.2 키 확장. schedule의 라운드 키 배치(byte 순서 그대로)와 rounds를 쓴다.
void ScalarExpandRoundKeys(const uint8* key, usize keyBytes, AesSchedule& schedule) noexcept {
    const usize nk = keyBytes / 4;
    const usize rounds = nk + 6;
    const usize words = 4 * (rounds + 1);
    uint8 w[4 * (crypto::detail::AesMaxRounds + 1)][4] = {};
    for (usize i = 0; i < nk; ++i) {
        for (usize b = 0; b < 4; ++b) {
            w[i][b] = key[4 * i + b];
        }
    }

    uint8 rcon = 1;
    for (usize i = nk; i < words; ++i) {
        uint8 temp[4] = {w[i - 1][0], w[i - 1][1], w[i - 1][2], w[i - 1][3]};
        if (i % nk == 0) {
            const uint8 first = temp[0];
            temp[0] = static_cast<uint8>(SBox.values[temp[1]] ^ rcon);
            temp[1] = SBox.values[temp[2]];
            temp[2] = SBox.values[temp[3]];
            temp[3] = SBox.values[first];
            rcon = Xtime(rcon);
        } else if (nk > 6 && i % nk == 4) {
            for (usize b = 0; b < 4; ++b) {
                temp[b] = SBox.values[temp[b]];
            }
        }

        for (usize b = 0; b < 4; ++b) {
            w[i][b] = static_cast<uint8>(w[i - nk][b] ^ temp[b]);
        }
    }

    platform::MemoryZero(&schedule, sizeof(schedule));
    for (usize round = 0; round <= rounds; ++round) {
        for (usize column = 0; column < 4; ++column) {
            for (usize b = 0; b < 4; ++b) {
                schedule.roundKeys[round].bytes[4 * column + b] = w[4 * round + column][b];
            }
        }
    }

    schedule.rounds = static_cast<uint32>(rounds);
}

// FIPS-197 5.1 암호화. 상태는 열 우선(state[r + 4c])이다. output은 input과 같아도 된다.
void ScalarEncrypt(const AesSchedule& schedule, const uint8* input, uint8* output) noexcept {
    uint8 state[16];
    for (usize i = 0; i < 16; ++i) {
        state[i] = static_cast<uint8>(input[i] ^ schedule.roundKeys[0].bytes[i]);
    }

    for (usize round = 1; round <= schedule.rounds; ++round) {
        uint8 shifted[16];
        for (usize column = 0; column < 4; ++column) {
            for (usize row = 0; row < 4; ++row) {
                shifted[row + 4 * column] = SBox.values[state[row + 4 * ((column + row) % 4)]];
            }
        }

        for (usize column = 0; column < 4; ++column) {
            const uint8* a = shifted + 4 * column;
            uint8* b = state + 4 * column;
            if (round == schedule.rounds) {
                for (usize row = 0; row < 4; ++row) {
                    b[row] = a[row];
                }
            } else {
                b[0] = static_cast<uint8>(GfMultiply(a[0], 2) ^ GfMultiply(a[1], 3) ^ a[2] ^ a[3]);
                b[1] = static_cast<uint8>(a[0] ^ GfMultiply(a[1], 2) ^ GfMultiply(a[2], 3) ^ a[3]);
                b[2] = static_cast<uint8>(a[0] ^ a[1] ^ GfMultiply(a[2], 2) ^ GfMultiply(a[3], 3));
                b[3] = static_cast<uint8>(GfMultiply(a[0], 3) ^ a[1] ^ a[2] ^ GfMultiply(a[3], 2));
            }
        }

        for (usize i = 0; i < 16; ++i) {
            state[i] = static_cast<uint8>(state[i] ^ schedule.roundKeys[round].bytes[i]);
        }
    }

    platform::MemoryCopy(output, state, 16);
}

// ---- 스칼라 GHASH(SP 800-38D 알고리즘 1) ----

struct Gf128 {
    uint64 hi;
    uint64 lo;
};

Gf128 LoadGf(const uint8* bytes) noexcept {
    return Gf128{LoadBigEndian<uint64>(bytes), LoadBigEndian<uint64>(bytes + 8)};
}

void StoreGf(uint8* bytes, Gf128 value) noexcept {
    StoreBigEndian<uint64>(bytes, value.hi);
    StoreBigEndian<uint64>(bytes + 8, value.lo);
}

// x · y. bit 0이 가장 높은 bit인 GCM 표현이다.
Gf128 GfMultiply128(Gf128 x, Gf128 y) noexcept {
    Gf128 z{0, 0};
    Gf128 v = y;
    for (usize i = 0; i < 128; ++i) {
        const uint64 bit = i < 64 ? (x.hi >> (63 - i)) & 1 : (x.lo >> (127 - i)) & 1;
        if (bit != 0) {
            z.hi ^= v.hi;
            z.lo ^= v.lo;
        }

        const uint64 lowest = v.lo & 1;
        v.lo = (v.lo >> 1) | (v.hi << 63);
        v.hi >>= 1;
        if (lowest != 0) {
            v.hi ^= 0xe100000000000000ull;
        }
    }

    return z;
}

// ---- 스칼라 커널 표 ----
// 커널 표 계약(AesKernels.h)을 스칼라로 구현한다. hPowers[0]에 H를 GCM byte 순서로 둔다(이 표만의
// 표현이라 HW 커널 스케줄과 섞으면 GHASH가 틀린다. 자가 시험 거부 시험이 이 점을 쓴다).

void ScalarExpandKey(const uint8* key, usize keyBytes, AesSchedule& schedule) noexcept {
    ScalarExpandRoundKeys(key, keyBytes, schedule);
    const uint8 zero[16] = {};
    ScalarEncrypt(schedule, zero, schedule.hPowers[0].bytes);
}

// 카운터 블록 끝 width bit를 big-endian 정수로 보고 1을 더한다(byte 단위 올림, 폭 안에서만).
void ScalarIncrement(AesBlock& counter, AesCounterWidth width) noexcept {
    const usize bytes = width == AesCounterWidth::Bits32   ? 4
                        : width == AesCounterWidth::Bits64 ? 8
                                                           : 16;
    for (usize i = 0; i < bytes; ++i) {
        uint8& byte = counter.bytes[AesBlockBytes - 1 - i];
        byte = static_cast<uint8>(byte + 1);
        if (byte != 0) {
            break;
        }
    }
}

void ScalarCtrXor(const AesSchedule& schedule, AesCounterWidth width, AesBlock& counter,
                  const uint8* input, uint8* output, usize bytes) noexcept {
    for (usize offset = 0; offset < bytes; offset += AesBlockBytes) {
        uint8 keystream[16];
        ScalarEncrypt(schedule, counter.bytes, keystream);
        const usize count = bytes - offset < AesBlockBytes ? bytes - offset : AesBlockBytes;
        for (usize i = 0; i < count; ++i) {
            output[offset + i] = static_cast<uint8>(input[offset + i] ^ keystream[i]);
        }

        ScalarIncrement(counter, width);
    }
}

void ScalarGhashUpdate(const AesSchedule& schedule, AesBlock& state, const uint8* data,
                       usize bytes) noexcept {
    const Gf128 h = LoadGf(schedule.hPowers[0].bytes);
    Gf128 x = LoadGf(state.bytes);
    for (usize offset = 0; offset < bytes; offset += AesBlockBytes) {
        uint8 block[16] = {};
        const usize count = bytes - offset < AesBlockBytes ? bytes - offset : AesBlockBytes;
        platform::MemoryCopy(block, data + offset, count);
        const Gf128 b = LoadGf(block);
        x.hi ^= b.hi;
        x.lo ^= b.lo;
        x = GfMultiply128(x, h);
    }

    StoreGf(state.bytes, x);
}

void ScalarCbcMacUpdate(const AesSchedule& schedule, AesBlock& state, const uint8* data,
                        usize blocks) noexcept {
    for (usize i = 0; i < blocks; ++i) {
        for (usize j = 0; j < AesBlockBytes; ++j) {
            state.bytes[j] = static_cast<uint8>(state.bytes[j] ^ data[AesBlockBytes * i + j]);
        }

        ScalarEncrypt(schedule, state.bytes, state.bytes);
    }
}

void ScalarEncryptBlock(const AesSchedule& schedule, const AesBlock& input,
                        AesBlock& output) noexcept {
    ScalarEncrypt(schedule, input.bytes, output.bytes);
}

constexpr AesKernels ScalarKernels = {
    .expandKey = &ScalarExpandKey,
    .ctrXor = &ScalarCtrXor,
    .ghashUpdate = &ScalarGhashUpdate,
    .cbcMacUpdate = &ScalarCbcMacUpdate,
    .encryptBlock = &ScalarEncryptBlock,
    .name = StringView("scalar-reference"),
};

// 스칼라 GCM 봉인(SP 800-38D 7.1, 96 bit IV)이다. AesModes.cpp와 따로 짠 기준이다.
void ScalarGcmSeal(Span<const uint8> key, Span<const uint8> nonce, Span<const uint8> associatedData,
                   Span<const uint8> plaintext, Span<uint8> ciphertext, uint8* tag) noexcept {
    AesSchedule schedule;
    ScalarExpandKey(key.Data(), key.Size(), schedule);
    AesBlock counter;
    platform::MemoryCopy(counter.bytes, nonce.Data(), 12);
    StoreBigEndian<uint32>(counter.bytes + 12, 2);
    ScalarCtrXor(schedule, AesCounterWidth::Bits32, counter, plaintext.Data(), ciphertext.Data(),
                 plaintext.Size());

    AesBlock state = {};
    ScalarGhashUpdate(schedule, state, associatedData.Data(), associatedData.Size());
    ScalarGhashUpdate(schedule, state, ciphertext.Data(), ciphertext.Size());
    AesBlock lengths;
    StoreBigEndian<uint64>(lengths.bytes, static_cast<uint64>(associatedData.Size()) * 8);
    StoreBigEndian<uint64>(lengths.bytes + 8, static_cast<uint64>(plaintext.Size()) * 8);
    ScalarGhashUpdate(schedule, state, lengths.bytes, AesBlockBytes);

    AesBlock j0;
    platform::MemoryCopy(j0.bytes, nonce.Data(), 12);
    StoreBigEndian<uint32>(j0.bytes + 12, 1);
    AesBlock mask;
    ScalarEncrypt(schedule, j0.bytes, mask.bytes);
    for (usize i = 0; i < 16; ++i) {
        tag[i] = static_cast<uint8>(mask.bytes[i] ^ state.bytes[i]);
    }
}

// ---- 일부러 망가뜨린 커널(자가 시험 거부 시험) ----

void FlippedEncryptBlock(const AesSchedule& schedule, const AesBlock& input,
                         AesBlock& output) noexcept {
    ScalarEncrypt(schedule, input.bytes, output.bytes);
    output.bytes[7] ^= 0x10;
}

void IgnoringGhashUpdate(const AesSchedule&, AesBlock&, const uint8*, usize) noexcept {
}

void IgnoringCbcMacUpdate(const AesSchedule&, AesBlock&, const uint8*, usize) noexcept {
}

// 카운터를 inc32가 아니라 앞 byte까지 올림하는 CTR이다(블록마다 끝 byte 대신 byte 11을 올린다).
void WrongCounterCtrXor(const AesSchedule& schedule, AesCounterWidth, AesBlock& counter,
                        const uint8* input, uint8* output, usize bytes) noexcept {
    for (usize offset = 0; offset < bytes; offset += AesBlockBytes) {
        uint8 keystream[16];
        ScalarEncrypt(schedule, counter.bytes, keystream);
        const usize count = bytes - offset < AesBlockBytes ? bytes - offset : AesBlockBytes;
        for (usize i = 0; i < count; ++i) {
            output[offset + i] = static_cast<uint8>(input[offset + i] ^ keystream[i]);
        }

        ++counter.bytes[11];
    }
}

// ---- GCM 알려진 답 ----

struct GcmCase {
    const char* name;
    const char* key;
    const char* nonce;
    const char* associatedData;
    const char* plaintext;
    const char* ciphertext;
    const char* tag;
};

constexpr const char* McgrewKey128 = "feffe9928665731c6d6a8f9467308308";
constexpr const char* McgrewKey256 =
    "feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308";
constexpr const char* McgrewNonce = "cafebabefacedbaddecaf888";
constexpr const char* McgrewAd = "feedfacedeadbeeffeedfacedeadbeefabaddad2";
constexpr const char* McgrewPlaintext64 =
    "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
    "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b391aafd255";
constexpr const char* McgrewPlaintext60 =
    "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
    "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39";

const GcmCase GcmCases[] = {
    {"case 1", "00000000000000000000000000000000", "000000000000000000000000", "", "", "",
     "58e2fccefa7e3061367f1d57a4e7455a"},
    {"case 2", "00000000000000000000000000000000", "000000000000000000000000", "",
     "00000000000000000000000000000000", "0388dace60b6a392f328c2b971b2fe78",
     "ab6e47d42cec13bdf53a67b21257bddf"},
    {"case 3", McgrewKey128, McgrewNonce, "", McgrewPlaintext64,
     "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e"
     "21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091473f5985",
     "4d5c2af327cd64a62cf35abd2ba6fab4"},
    {"case 4", McgrewKey128, McgrewNonce, McgrewAd, McgrewPlaintext60,
     "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e"
     "21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091",
     "5bc94fbc3221a5db94fae95ae7121a47"},
    {"case 13", "0000000000000000000000000000000000000000000000000000000000000000",
     "000000000000000000000000", "", "", "", "530f8afbc74536b9a963b4f1c4cb738b"},
    {"case 14", "0000000000000000000000000000000000000000000000000000000000000000",
     "000000000000000000000000", "", "00000000000000000000000000000000",
     "cea7403d4d606b6e074ec5d3baf39d18", "d0d1c8a799996bf0265b98b5d48ab919"},
    {"case 15", McgrewKey256, McgrewNonce, "", McgrewPlaintext64,
     "522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa"
     "8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662898015ad",
     "b094dac5d93471bdec1a502270e3cc6c"},
    {"case 16", McgrewKey256, McgrewNonce, McgrewAd, McgrewPlaintext60,
     "522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa"
     "8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662",
     "76fc6ece0f4e1768cddf8853bb2d551b"},
};

// kernels로 사례 하나를 봉인·열기(바깥·제자리)·tag 변조 거부까지 확인한다.
void CheckGcmCase(const AesKernels& kernels, const GcmCase& item) noexcept {
    const HexBytes key(item.key);
    const HexBytes nonce(item.nonce);
    const HexBytes associatedData(item.associatedData);
    const HexBytes plaintext(item.plaintext);
    const HexBytes ciphertext(item.ciphertext);
    const HexBytes tag(item.tag);
    REQUIRE(key.IsValid() && nonce.IsValid() && associatedData.IsValid() && plaintext.IsValid() &&
            ciphertext.IsValid() && tag.IsValid());
    REQUIRE(plaintext.size == ciphertext.size && plaintext.size <= 64);

    auto schedule = crypto::detail::TryCreateAesSchedule(kernels, key.View());
    REQUIRE(schedule);
    const AesSchedule& expanded = *schedule.Value();
    uint8 sealed[64];
    uint8 sealedTag[16];
    const Span<uint8> sealedView(sealed, plaintext.size);
    crypto::detail::AesGcmSealDetached(kernels, expanded, nonce.View(), associatedData.View(),
                                       plaintext.View(), sealedView, Span<uint8>(sealedTag));
    if (!Same(sealedView, ciphertext.View()) || !Same(Span<const uint8>(sealedTag), tag.View())) {
        printf("  %s (%.*s) GCM mismatch\n", item.name, static_cast<int>(kernels.name.ByteCount()),
               kernels.name.Data());
        CHECK(false);
    }

    uint8 opened[64];
    const Span<uint8> openedView(opened, plaintext.size);
    CHECK(crypto::detail::AesGcmOpenDetached(kernels, expanded, nonce.View(), associatedData.View(),
                                             ciphertext.View(), tag.View(), openedView));
    CHECK(Same(openedView, plaintext.View()));

    // 제자리 열기다.
    platform::MemoryCopy(sealed, ciphertext.bytes, ciphertext.size);
    CHECK(crypto::detail::AesGcmOpenDetached(kernels, expanded, nonce.View(), associatedData.View(),
                                             sealedView, tag.View(), sealedView));
    CHECK(Same(sealedView, plaintext.View()));

    // tag bit 하나를 뒤집으면 거부하고 출력 전체를 지운다.
    uint8 badTag[16];
    platform::MemoryCopy(badTag, tag.bytes, 16);
    badTag[15] ^= 0x01;
    platform::MemorySet(opened, 0xa5, sizeof(opened));
    CHECK(!crypto::detail::AesGcmOpenDetached(kernels, expanded, nonce.View(),
                                              associatedData.View(), ciphertext.View(),
                                              Span<const uint8>(badTag), openedView));
    CHECK(AllZero(openedView));
    crypto::detail::DestroyAesSchedule(schedule.Value());
}

// 키를 kernels와 스칼라 참조로 확장한 두 스케줄의 라운드 키가 같은가.
bool SameRoundKeys(const AesSchedule& left, const AesSchedule& right) noexcept {
    if (left.rounds != right.rounds) {
        return false;
    }

    for (usize round = 0; round <= left.rounds; ++round) {
        if (!Same(BlockView(left.roundKeys[round]), BlockView(right.roundKeys[round]))) {
            return false;
        }
    }

    return true;
}

} // namespace

// 스칼라 참조 자체: FIPS-197 부록 C.1·C.3 블록, 부록 A.1·A.3 마지막 라운드 키, McGrew–Viega GCM.
TEST(CryptoAesScalarReference) {
    struct BlockCase {
        const char* key;
        const char* lastRoundKey;
        const char* ciphertext;
    };

    const BlockCase blocks[] = {
        {"000102030405060708090a0b0c0d0e0f", nullptr, "69c4e0d86a7b0430d8cdb78070b4c55a"},
        {"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", nullptr,
         "8ea2b7ca516745bfeafc49904b496089"},
        {"2b7e151628aed2a6abf7158809cf4f3c", "d014f9a8c9ee2589e13f0cc8b6630ca6", nullptr},
        {"603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4",
         "fe4890d1e6188d0b046df344706c631e", nullptr},
    };
    const HexBytes plaintext("00112233445566778899aabbccddeeff");
    REQUIRE(plaintext.IsValid());
    for (const BlockCase& item : blocks) {
        const HexBytes key(item.key);
        REQUIRE(key.IsValid());
        AesSchedule schedule;
        ScalarExpandKey(key.bytes, key.size, schedule);
        CHECK(schedule.rounds == (key.size == 16 ? 10u : 14u));
        if (item.lastRoundKey != nullptr) {
            const HexBytes expected(item.lastRoundKey);
            CHECK(Same(BlockView(schedule.roundKeys[schedule.rounds]), expected.View()));
        }

        if (item.ciphertext != nullptr) {
            const HexBytes expected(item.ciphertext);
            uint8 output[16];
            ScalarEncrypt(schedule, plaintext.bytes, output);
            CHECK(Same(Span<const uint8>(output), expected.View()));
        }
    }

    // 스칼라 GCM이 명세 사례와 같아야 아래 무작위 대조의 기준이 된다.
    for (const GcmCase& item : GcmCases) {
        const HexBytes key(item.key);
        const HexBytes nonce(item.nonce);
        const HexBytes associatedData(item.associatedData);
        const HexBytes message(item.plaintext);
        const HexBytes ciphertext(item.ciphertext);
        const HexBytes tag(item.tag);
        uint8 sealed[64];
        uint8 sealedTag[16];
        REQUIRE(message.size <= sizeof(sealed));
        const Span<uint8> sealedView(sealed, message.size);
        ScalarGcmSeal(key.View(), nonce.View(), associatedData.View(), message.View(), sealedView,
                      sealedTag);
        CHECK(Same(sealedView, ciphertext.View()));
        CHECK(Same(Span<const uint8>(sealedTag), tag.View()));
    }
}

// HW 판정과 커널 표 선택은 값을 받으므로 HW 없는 경로를 어느 기기에서나 확인한다(12절 경로 범위).
TEST(CryptoAesKernelSelection) {
    const platform::CpuFeatures none{};
    CHECK(!crypto::detail::HasAesInstructions(none));
    CHECK(!crypto::detail::SelectAesKernels(none).IsComplete());
    CHECK(!crypto::detail::HasAesHardware(none, CryptoRuntimeInfo{}, true));

    platform::CpuFeatures all{};
    all.aes = true;
    all.pclmulqdq = true;
    all.avx = true;
    all.armAes = true;
    all.armPmull = true;
    CryptoRuntimeInfo sodium{};
    sodium.sodiumAesni = true;
    sodium.sodiumPclmul = true;
    sodium.sodiumAvx = true;
    sodium.sodiumArmCrypto = true;
    CHECK(crypto::detail::HasAesInstructions(all));
    // 표를 고르기만 하고 부르지 않으므로 HW가 없는 기기에서도 안전하다.
    const AesKernels selected = crypto::detail::SelectAesKernels(all);
    CHECK(selected.IsComplete());
    CHECK(selected.name == crypto::detail::GetNativeAesKernels().name);
    CHECK(selected.name == (IsX64 ? StringView("rocks-aes/x64") : StringView("rocks-aes/arm64")));
    CHECK(crypto::detail::HasAesHardware(all, sodium, true));
    CHECK(!crypto::detail::HasAesHardware(all, sodium, false));
    CHECK(!crypto::detail::HasAesHardware(all, CryptoRuntimeInfo{}, true));

    // 조건 하나만 빠져도 AES 전체가 없다(AES-NI는 있고 AVX가 없는 x64, PMULL이 없는 arm64).
    platform::CpuFeatures partial = all;
    if constexpr (IsX64) {
        partial.avx = false;
    } else {
        partial.armPmull = false;
    }

    CHECK(!crypto::detail::HasAesInstructions(partial));
    CHECK(!crypto::detail::SelectAesKernels(partial).IsComplete());
    CHECK(!crypto::detail::HasAesHardware(partial, sodium, true));

    // 이 기기의 결정: 표는 HW 조건과 같고(자가 시험 통과), libsodium 경로는 값을 받는 판정과 같다.
    const platform::CpuFeatures& cpu = platform::QueryCpuFeatures();
    const AesKernels& kernels = crypto::detail::GetAesKernels();
    CHECK(kernels.IsComplete() == crypto::detail::HasAesInstructions(cpu));
    if (kernels.IsComplete()) {
        CHECK(kernels.name == crypto::detail::GetNativeAesKernels().name);
    } else {
        printf("  CryptoAesKernelSelection: this CPU has no AES lightweight layer (Unsupported "
               "path)\n");
    }

    auto info = TryGetAeadInfo(AeadAlgorithm::Aes256Gcm);
    REQUIRE(info);
    if (info.Value().available) {
        CHECK(crypto::detail::HasAesHardware(cpu, GetCryptoRuntimeInfo(), true));
    }

    // 공개 AES 계열은 한 조건(libsodium HW 판정과 경량 계층 표)으로 함께 쓰거나 함께 없다.
    const bool aes = crypto::detail::IsAesAvailable();
    CHECK(aes == (crypto::detail::HasAesHardware(cpu, GetCryptoRuntimeInfo(), true) &&
                  kernels.IsComplete()));
    const AeadAlgorithm aesAlgorithms[] = {AeadAlgorithm::Aes256Gcm, AeadAlgorithm::Aes128Gcm,
                                           AeadAlgorithm::Aegis256, AeadAlgorithm::Aegis128L};
    for (AeadAlgorithm algorithm : aesAlgorithms) {
        CHECK(TryGetAeadInfo(algorithm).Value().available == aes);
    }

    if (aes) {
        CHECK(TryGetAeadInfo(AeadAlgorithm::Aes128Gcm).Value().implementation == kernels.name);
    }
}

// 자가 시험: 올바른 표는 받고, 하나라도 틀리거나 빠진 표는 거부한다(빈 표 → 모든 AES 호출
// Unsupported). 스칼라 표는 HW 없이 돈다.
TEST(CryptoAesSelfTest) {
    const AesKernels accepted = crypto::detail::CheckAesKernels(ScalarKernels);
    CHECK(accepted.IsComplete());
    CHECK(accepted.name == ScalarKernels.name);

    AesKernels broken = ScalarKernels;
    broken.encryptBlock = &FlippedEncryptBlock;
    CHECK(!crypto::detail::CheckAesKernels(broken).IsComplete());

    broken = ScalarKernels;
    broken.ghashUpdate = &IgnoringGhashUpdate;
    CHECK(!crypto::detail::CheckAesKernels(broken).IsComplete());

    broken = ScalarKernels;
    broken.cbcMacUpdate = &IgnoringCbcMacUpdate;
    CHECK(!crypto::detail::CheckAesKernels(broken).IsComplete());

    broken = ScalarKernels;
    broken.ctrXor = &WrongCounterCtrXor;
    CHECK(!crypto::detail::CheckAesKernels(broken).IsComplete());

    broken = ScalarKernels;
    broken.encryptBlock = nullptr;
    CHECK(!crypto::detail::CheckAesKernels(broken).IsComplete());
    CHECK(!crypto::detail::CheckAesKernels(AesKernels{}).IsComplete());

    // 거부된 표로는 스케줄을 만들 수 없다(Unsupported).
    const uint8 key[16] = {};
    CHECK(crypto::detail::TryCreateAesSchedule(AesKernels{}, Span<const uint8>(key)).Error() ==
          ErrorCode::Unsupported);

    const AesKernels* native = NativeKernelsOrSkip("CryptoAesSelfTest");
    if (native == nullptr) {
        return;
    }

    CHECK(crypto::detail::CheckAesKernels(*native).IsComplete());
    // HW 커널 표에 스칼라 GHASH를 섞으면 H 표현이 달라 자가 시험이 거부한다.
    AesKernels mixed = *native;
    mixed.ghashUpdate = &ScalarGhashUpdate;
    CHECK(!crypto::detail::CheckAesKernels(mixed).IsComplete());
}

// 키 스케줄은 키 저장소 슬롯에 있고, 인자 오류는 슬롯을 남기지 않는다.
TEST(CryptoAesScheduleLifetime) {
    const uint8 key16[16] = {0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
                             0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c};
    const uint8 key24[24] = {};
    CHECK(crypto::detail::TryCreateAesSchedule(ScalarKernels, Span<const uint8>(key24)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(crypto::detail::TryCreateAesSchedule(ScalarKernels, Span<const uint8>()).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(crypto::detail::TryCreateAesSchedule(AesKernels{}, Span<const uint8>(key24)).Error() ==
          ErrorCode::Unsupported);

    auto schedule = crypto::detail::TryCreateAesSchedule(ScalarKernels, Span<const uint8>(key16));
    REQUIRE(schedule);
    AesSchedule* created = schedule.Value();
    CHECK(created->rounds == 10);
    CHECK(Same(BlockView(created->roundKeys[0]), Span<const uint8>(key16)));
    CHECK(reinterpret_cast<uptr>(created) % alignof(AesSchedule) == 0);

    bool inArena = false;
    const uint8* begin = nullptr;
    usize bytes = 0;
    for (usize index = 0; crypto::detail::TryGetSecretArenaRange(index, begin, bytes); ++index) {
        const uptr address = reinterpret_cast<uptr>(created);
        if (address >= reinterpret_cast<uptr>(begin) &&
            address + sizeof(AesSchedule) <= reinterpret_cast<uptr>(begin) + bytes) {
            inArena = true;
        }
    }

    CHECK(inArena);
    crypto::detail::DestroyAesSchedule(created);
    crypto::detail::DestroyAesSchedule(nullptr);
}

// HW 커널을 스칼라 참조와 대조한다: 키 확장, 블록, CTR(정렬·제자리·inc32 2^32 wrap·이어 부르기),
// GHASH(부분 블록·이어 부르기), CBC-MAC.
TEST(CryptoAesKernelsMatchScalar) {
    const AesKernels* native = NativeKernelsOrSkip("CryptoAesKernelsMatchScalar");
    if (native == nullptr) {
        return;
    }

    TestRandom random{test::CurrentSeed() ^ 0xae5c7a1a2ull};
    SystemAllocator allocator;
    constexpr usize MaxBytes = 2048;
    Array<uint8> input(allocator);
    Array<uint8> expected(allocator);
    Array<uint8> actual(allocator);
    REQUIRE(input.TryResize(MaxBytes + 32));
    REQUIRE(expected.TryResize(MaxBytes + 32));
    REQUIRE(actual.TryResize(MaxBytes + 32));

    const usize specialLengths[] = {0,   1,   15,  16,  17,  31,  32,  33,   95,   96,  97,
                                    111, 112, 113, 127, 128, 129, 191, 192,  193,  223, 224,
                                    225, 239, 240, 241, 255, 256, 257, 1000, 2047, 2048};
    usize failures = 0;
    for (usize iteration = 0; iteration < 160; ++iteration) {
        const usize keyBytes = iteration % 2 == 0 ? 16 : 32;
        uint8 key[32];
        random.Fill(Span<uint8>(key));
        AesSchedule hardware;
        AesSchedule scalar;
        native->expandKey(key, keyBytes, hardware);
        ScalarExpandKey(key, keyBytes, scalar);
        CHECK(SameRoundKeys(hardware, scalar));

        // 블록 하나(입력과 출력이 같은 블록인 경우 포함).
        AesBlock block;
        random.Fill(Span<uint8>(block.bytes));
        AesBlock hardwareBlock;
        AesBlock scalarBlock;
        native->encryptBlock(hardware, block, hardwareBlock);
        ScalarEncryptBlock(scalar, block, scalarBlock);
        CHECK(Same(BlockView(hardwareBlock), BlockView(scalarBlock)));
        native->encryptBlock(hardware, block, block);
        CHECK(Same(BlockView(block), BlockView(scalarBlock)));

        // CTR. 카운터 끝 32 bit는 작은 값·무작위·2^32 바로 아래(wrap)를 고루 쓴다.
        const usize length = iteration < sizeof(specialLengths) / sizeof(specialLengths[0])
                                 ? specialLengths[iteration]
                                 : random.Below(MaxBytes + 1);
        const usize inputOffset = random.Below(16);
        const usize outputOffset = random.Below(16);
        AesBlock counter;
        random.Fill(Span<uint8>(counter.bytes));
        uint32 low = static_cast<uint32>(random.Next());
        if (iteration % 3 == 0) {
            low = Counter32Max - static_cast<uint32>(random.Below(24));
        } else if (iteration % 3 == 1) {
            low = static_cast<uint32>(random.Below(4));
        }

        StoreBigEndian<uint32>(counter.bytes + 12, low);
        const Span<uint8> source = input.View().Mid(inputOffset, length);
        random.Fill(source);
        AesBlock hardwareCounter = counter;
        AesBlock scalarCounter = counter;
        native->ctrXor(hardware, AesCounterWidth::Bits32, hardwareCounter, source.Data(),
                       actual.View().Mid(outputOffset, length).Data(), length);
        ScalarCtrXor(scalar, AesCounterWidth::Bits32, scalarCounter, source.Data(), expected.Data(),
                     length);
        const bool ctrMatched =
            Same(actual.View().Mid(outputOffset, length), expected.View().Left(length)) &&
            Same(BlockView(hardwareCounter), BlockView(scalarCounter));
        CHECK(ctrMatched);
        if (!ctrMatched && ++failures <= 4) {
            printf("  CTR mismatch: key %u bytes, length %u, low32 0x%08x\n",
                   static_cast<unsigned>(keyBytes), static_cast<unsigned>(length), low);
        }

        // 제자리 CTR과 블록 경계에서 나눠 이어 부르기도 한 번에 부른 것과 같다.
        platform::MemoryCopy(actual.Data() + outputOffset, source.Data(), length);
        const usize split = (random.Below(length + 1) / AesBlockBytes) * AesBlockBytes;
        AesBlock chainedCounter = counter;
        uint8* inPlace = actual.Data() + outputOffset;
        native->ctrXor(hardware, AesCounterWidth::Bits32, chainedCounter, inPlace, inPlace, split);
        native->ctrXor(hardware, AesCounterWidth::Bits32, chainedCounter, inPlace + split,
                       inPlace + split, length - split);
        CHECK(Same(Span<const uint8>(inPlace, length), expected.View().Left(length)));
        CHECK(Same(BlockView(chainedCounter), BlockView(scalarCounter)));

        // GHASH: 시작 상태 무작위, 앞 조각은 16의 배수, 뒤 조각은 부분 블록 가능.
        AesBlock state;
        random.Fill(Span<uint8>(state.bytes));
        AesBlock hardwareState = state;
        AesBlock scalarState = state;
        native->ghashUpdate(hardware, hardwareState, source.Data(), split);
        native->ghashUpdate(hardware, hardwareState, source.Data() + split, length - split);
        ScalarGhashUpdate(scalar, scalarState, source.Data(), length);
        CHECK(Same(BlockView(hardwareState), BlockView(scalarState)));

        // CBC-MAC.
        const usize blocks = length / AesBlockBytes;
        AesBlock hardwareMac = state;
        AesBlock scalarMac = state;
        native->cbcMacUpdate(hardware, hardwareMac, source.Data(), blocks);
        ScalarCbcMacUpdate(scalar, scalarMac, source.Data(), blocks);
        CHECK(Same(BlockView(hardwareMac), BlockView(scalarMac)));
    }
}

// McGrew–Viega 사례 1–4·13–16. 스칼라 표(AesModes.cpp 조립 확인, HW 무관)와 HW 표로 돌린다.
TEST(CryptoAesGcmKnownAnswers) {
    for (const GcmCase& item : GcmCases) {
        CheckGcmCase(ScalarKernels, item);
    }

    const AesKernels* native = NativeKernelsOrSkip("CryptoAesGcmKnownAnswers");
    if (native == nullptr) {
        return;
    }

    for (const GcmCase& item : GcmCases) {
        CheckGcmCase(*native, item);
    }
}

// 같은 기기의 libsodium AES-256-GCM(원본)과 경량 계층 AES-256-GCM이 byte 단위로 같아야 한다(12절
// 차등). 길이 0–64 KiB(병렬 폭·GHASH 묶음·GCM 조각 경계 포함), AD 0–1 KiB, 비정렬, 제자리, 변조
// 거부.
TEST(CryptoAesGcmMatchesLibsodium) {
    const AesKernels* native = NativeKernelsOrSkip("CryptoAesGcmMatchesLibsodium");
    if (native == nullptr) {
        return;
    }

    auto info = TryGetAeadInfo(AeadAlgorithm::Aes256Gcm);
    REQUIRE(info);
    if (!info.Value().available) {
        printf("  CryptoAesGcmMatchesLibsodium: skipped, libsodium AES-256-GCM is unavailable\n");
        return;
    }

    TestRandom random{test::CurrentSeed() ^ 0x5eed6c3ull};
    SystemAllocator allocator;
    constexpr usize MaxMessage = 64 * 1024;
    constexpr usize MaxAssociated = 1024;
    Array<uint8> plaintext(allocator);
    Array<uint8> reference(allocator);
    Array<uint8> sealed(allocator);
    Array<uint8> opened(allocator);
    Array<uint8> associated(allocator);
    REQUIRE(plaintext.TryResize(MaxMessage + 16));
    REQUIRE(reference.TryResize(MaxMessage));
    REQUIRE(sealed.TryResize(MaxMessage + 16));
    REQUIRE(opened.TryResize(MaxMessage + 16));
    REQUIRE(associated.TryResize(MaxAssociated + 16));

    const usize specialLengths[] = {0,   1,    15,   16,   17,   95,   96,    97,        111, 112,
                                    113, 127,  128,  129,  191,  192,  193,   223,       224, 225,
                                    245, 4031, 4032, 4033, 8064, 8065, 65535, MaxMessage};
    const usize specialAssociated[] = {0, 1, 15, 16, 17, 223, 224, 225, MaxAssociated};
    constexpr usize SpecialCount = sizeof(specialLengths) / sizeof(specialLengths[0]);
    for (usize iteration = 0; iteration < SpecialCount + 24; ++iteration) {
        const usize length =
            iteration < SpecialCount ? specialLengths[iteration] : random.Below(MaxMessage + 1);
        const usize associatedLength =
            iteration % 2 == 0
                ? specialAssociated[iteration / 2 %
                                    (sizeof(specialAssociated) / sizeof(specialAssociated[0]))]
                : random.Below(MaxAssociated + 1);
        uint8 keyBytes[32];
        uint8 nonce[12];
        random.Fill(Span<uint8>(keyBytes));
        random.Fill(Span<uint8>(nonce));
        auto key = AeadKey::TryImport(AeadAlgorithm::Aes256Gcm, Span<const uint8>(keyBytes));
        REQUIRE(key);
        auto schedule = crypto::detail::TryCreateAesSchedule(*native, Span<const uint8>(keyBytes));
        REQUIRE(schedule);
        const AesSchedule& expanded = *schedule.Value();

        const Span<uint8> message = plaintext.View().Mid(random.Below(16), length);
        const Span<uint8> ad = associated.View().Mid(random.Below(16), associatedLength);
        random.Fill(message);
        random.Fill(ad);
        uint8 referenceTag[16];
        REQUIRE(hazmat::TrySealDetached(key.Value(), Span<const uint8>(nonce), ad, message,
                                        reference.View().Left(length), Span<uint8>(referenceTag)));

        // 비정렬 바깥 출력.
        const Span<uint8> ciphertext = sealed.View().Mid(random.Below(16), length);
        uint8 tag[16];
        crypto::detail::AesGcmSealDetached(*native, expanded, Span<const uint8>(nonce), ad, message,
                                           ciphertext, Span<uint8>(tag));
        const bool matched = Same(ciphertext, reference.View().Left(length)) &&
                             Same(Span<const uint8>(tag), Span<const uint8>(referenceTag));
        CHECK(matched);
        if (!matched) {
            printf("  AES-256-GCM differs from libsodium: message %u bytes, AD %u bytes\n",
                   static_cast<unsigned>(length), static_cast<unsigned>(associatedLength));
        }

        // 제자리 봉인.
        const Span<uint8> inPlace = opened.View().Mid(random.Below(16), length);
        platform::MemoryCopy(inPlace.Data(), message.Data(), length);
        uint8 inPlaceTag[16];
        crypto::detail::AesGcmSealDetached(*native, expanded, Span<const uint8>(nonce), ad, inPlace,
                                           inPlace, Span<uint8>(inPlaceTag));
        CHECK(Same(inPlace, reference.View().Left(length)));
        CHECK(Same(Span<const uint8>(inPlaceTag), Span<const uint8>(referenceTag)));

        // 제자리 열기와 바깥 열기.
        CHECK(crypto::detail::AesGcmOpenDetached(*native, expanded, Span<const uint8>(nonce), ad,
                                                 inPlace, Span<const uint8>(inPlaceTag), inPlace));
        CHECK(Same(inPlace, message));
        const Span<uint8> output = opened.View().Mid(random.Below(16), length);
        CHECK(crypto::detail::AesGcmOpenDetached(*native, expanded, Span<const uint8>(nonce), ad,
                                                 ciphertext, Span<const uint8>(tag), output));
        CHECK(Same(output, message));

        // tag·암호문·AD 변조는 거부하고 출력 전체를 0으로 지운다.
        uint8 badTag[16];
        platform::MemoryCopy(badTag, tag, 16);
        badTag[random.Below(16)] ^= static_cast<uint8>(1u << random.Below(8));
        platform::MemorySet(output.Data(), 0xa5, length);
        CHECK(!crypto::detail::AesGcmOpenDetached(*native, expanded, Span<const uint8>(nonce), ad,
                                                  ciphertext, Span<const uint8>(badTag), output));
        CHECK(AllZero(output));
        if (length != 0) {
            const usize at = random.Below(length);
            ciphertext[at] ^= 0x80;
            CHECK(!crypto::detail::AesGcmOpenDetached(*native, expanded, Span<const uint8>(nonce),
                                                      ad, ciphertext, Span<const uint8>(tag),
                                                      output));
            CHECK(AllZero(output));
            ciphertext[at] ^= 0x80;
        }

        if (associatedLength != 0) {
            const usize at = random.Below(associatedLength);
            ad[at] ^= 0x01;
            CHECK(!crypto::detail::AesGcmOpenDetached(*native, expanded, Span<const uint8>(nonce),
                                                      ad, ciphertext, Span<const uint8>(tag),
                                                      output));
            ad[at] ^= 0x01;
        }

        crypto::detail::DestroyAesSchedule(schedule.Value());
    }
}

// libsodium에 없는 AES-128-GCM은 스칼라 GCM과 무작위 입력으로 대조한다.
TEST(CryptoAes128GcmMatchesScalar) {
    const AesKernels* native = NativeKernelsOrSkip("CryptoAes128GcmMatchesScalar");
    if (native == nullptr) {
        return;
    }

    TestRandom random{test::CurrentSeed() ^ 0xa128ull};
    SystemAllocator allocator;
    constexpr usize MaxMessage = 8192;
    Array<uint8> plaintext(allocator);
    Array<uint8> reference(allocator);
    Array<uint8> sealed(allocator);
    Array<uint8> associated(allocator);
    REQUIRE(plaintext.TryResize(MaxMessage + 16));
    REQUIRE(reference.TryResize(MaxMessage));
    REQUIRE(sealed.TryResize(MaxMessage + 16));
    REQUIRE(associated.TryResize(256 + 16));

    const usize specialLengths[] = {0, 1, 16, 17, 112, 113, 224, 225, 4031, 4032, 4033, MaxMessage};
    constexpr usize SpecialCount = sizeof(specialLengths) / sizeof(specialLengths[0]);
    for (usize iteration = 0; iteration < SpecialCount + 12; ++iteration) {
        const usize length =
            iteration < SpecialCount ? specialLengths[iteration] : random.Below(MaxMessage + 1);
        const usize associatedLength = random.Below(257);
        uint8 key[16];
        uint8 nonce[12];
        random.Fill(Span<uint8>(key));
        random.Fill(Span<uint8>(nonce));
        auto schedule = crypto::detail::TryCreateAesSchedule(*native, Span<const uint8>(key));
        REQUIRE(schedule);
        const Span<uint8> message = plaintext.View().Mid(random.Below(16), length);
        const Span<uint8> ad = associated.View().Mid(random.Below(16), associatedLength);
        random.Fill(message);
        random.Fill(ad);
        uint8 referenceTag[16];
        ScalarGcmSeal(Span<const uint8>(key), Span<const uint8>(nonce), ad, message,
                      reference.View().Left(length), referenceTag);
        const Span<uint8> ciphertext = sealed.View().Mid(random.Below(16), length);
        uint8 tag[16];
        crypto::detail::AesGcmSealDetached(*native, *schedule.Value(), Span<const uint8>(nonce), ad,
                                           message, ciphertext, Span<uint8>(tag));
        CHECK(Same(ciphertext, reference.View().Left(length)));
        CHECK(Same(Span<const uint8>(tag), Span<const uint8>(referenceTag)));
        CHECK(crypto::detail::AesGcmOpenDetached(*native, *schedule.Value(),
                                                 Span<const uint8>(nonce), ad, ciphertext,
                                                 Span<const uint8>(tag), ciphertext));
        CHECK(Same(ciphertext, message));
        crypto::detail::DestroyAesSchedule(schedule.Value());
    }
}

// ---- CTR 카운터 폭(32·64·128 bit) ----

namespace {

usize WidthBytes(AesCounterWidth width) noexcept {
    return width == AesCounterWidth::Bits32 ? 4 : width == AesCounterWidth::Bits64 ? 8 : 16;
}

hazmat::AesCtrCounterWidth PublicWidth(AesCounterWidth width) noexcept {
    return width == AesCounterWidth::Bits32   ? hazmat::AesCtrCounterWidth::Bits32
           : width == AesCounterWidth::Bits64 ? hazmat::AesCtrCounterWidth::Bits64
                                              : hazmat::AesCtrCounterWidth::Bits128;
}

// 끝 onesBytes byte를 big-endian 정수로 보면 2^(8 × onesBytes) - remaining인 카운터 블록이다(앞
// byte는 무작위, 바로 앞 byte는 올림이 거기서 멈추게 0xff가 아니다). remaining은 1 이상 2^32
// 미만이다.
AesBlock CounterBelow(TestRandom& random, usize onesBytes, uint64 remaining) noexcept {
    AesBlock counter;
    random.Fill(Span<uint8>(counter.bytes));
    for (usize i = 0; i < onesBytes; ++i) {
        counter.bytes[AesBlockBytes - 1 - i] = 0xff;
    }

    if (onesBytes < AesBlockBytes) {
        counter.bytes[AesBlockBytes - 1 - onesBytes] &= 0xfe;
    }

    const uint64 low = LoadBigEndian<uint64>(counter.bytes + 8);
    StoreBigEndian<uint64>(counter.bytes + 8, low - (remaining - 1));
    return counter;
}

} // namespace

// HW 커널의 32·64·128 bit 카운터 증가를 스칼라 참조(byte 단위 big-endian 올림)와 대조한다. 시작
// 카운터를 2^32 - k, 2^64 - k, 2^128 - k에 두어 병렬 경로·블록 경로·부분 블록이 올림과 폭 안의
// wrap을 지나게 한다(커널은 wrap을 막지 않는다. 막는 것은 모드 계층이다).
TEST(CryptoAesCtrKernelWidths) {
    const AesKernels* native = NativeKernelsOrSkip("CryptoAesCtrKernelWidths");
    if (native == nullptr) {
        return;
    }

    TestRandom random{test::CurrentSeed() ^ 0xc7a3ull};
    uint8 input[64 * AesBlockBytes];
    uint8 actual[64 * AesBlockBytes];
    uint8 expected[64 * AesBlockBytes];
    const AesCounterWidth widths[] = {AesCounterWidth::Bits32, AesCounterWidth::Bits64,
                                      AesCounterWidth::Bits128};
    const usize ones[] = {4, 8, 16};
    const uint64 remainders[] = {1, 2, 3, 6, 7, 8, 13, 14, 15, 20};
    usize failures = 0;
    for (usize iteration = 0; iteration < 2; ++iteration) {
        const usize keyBytes = iteration == 0 ? 16 : 32;
        uint8 key[32];
        random.Fill(Span<uint8>(key));
        AesSchedule hardware;
        AesSchedule scalar;
        native->expandKey(key, keyBytes, hardware);
        ScalarExpandKey(key, keyBytes, scalar);
        for (AesCounterWidth width : widths) {
            for (usize onesBytes : ones) {
                if (onesBytes > WidthBytes(width)) {
                    continue;
                }

                for (uint64 remaining : remainders) {
                    const AesBlock counter = CounterBelow(random, onesBytes, remaining);
                    const usize length = static_cast<usize>(remaining) * AesBlockBytes - 15 +
                                         random.Below(16 * 3 + 15);
                    random.Fill(Span<uint8>(input).Left(length));
                    AesBlock hardwareCounter = counter;
                    AesBlock scalarCounter = counter;
                    native->ctrXor(hardware, width, hardwareCounter, input, actual, length);
                    ScalarCtrXor(scalar, width, scalarCounter, input, expected, length);
                    const bool matched = Same(Span<const uint8>(actual).Left(length),
                                              Span<const uint8>(expected).Left(length)) &&
                                         Same(BlockView(hardwareCounter), BlockView(scalarCounter));
                    CHECK(matched);
                    if (!matched && ++failures <= 4) {
                        printf("  CTR width mismatch: width %u bytes, ones %u, remaining %u, "
                               "length %u\n",
                               static_cast<unsigned>(WidthBytes(width)),
                               static_cast<unsigned>(onesBytes), static_cast<unsigned>(remaining),
                               static_cast<unsigned>(length));
                    }
                }
            }
        }

        platform::MemoryZero(&hardware, sizeof(hardware));
    }
}

// 공개 CTR(AesCtr·TryAesCtrXor)의 용량 검사다. 폭 안에서 남은 카운터 값만큼은 처리하고, 한 byte라도
// 넘으면 쓰기 전에 CapacityExceeded(출력 그대로)다. 결과는 스칼라 참조와 같다.
TEST(CryptoAesCtrCapacity) {
    uint8 keyBytes[32];
    TestRandom random{test::CurrentSeed() ^ 0xca9aull};
    random.Fill(Span<uint8>(keyBytes));
    auto key = hazmat::AesKey::TryImport(Span<const uint8>(keyBytes).Left(16));
    if (!crypto::detail::IsAesAvailable()) {
        CHECK(key.Error() == ErrorCode::Unsupported);
        printf("  CryptoAesCtrCapacity: AES is unavailable on this CPU (Unsupported path)\n");
        return;
    }

    REQUIRE(key);
    AesSchedule scalar;
    ScalarExpandKey(keyBytes, 16, scalar);
    uint8 input[32 * AesBlockBytes + 64];
    uint8 output[32 * AesBlockBytes + 64];
    uint8 expected[32 * AesBlockBytes + 64];
    random.Fill(Span<uint8>(input));
    const AesCounterWidth widths[] = {AesCounterWidth::Bits32, AesCounterWidth::Bits64,
                                      AesCounterWidth::Bits128};
    const uint64 remainders[] = {1, 2, 7, 8, 15};
    for (AesCounterWidth width : widths) {
        const hazmat::AesCtrCounterWidth publicWidth = PublicWidth(width);
        for (uint64 remaining : remainders) {
            const usize capacity = static_cast<usize>(remaining) * AesBlockBytes;
            const AesBlock counter = CounterBelow(random, WidthBytes(width), remaining);
            const Span<const uint8> counterView = BlockView(counter);
            AesBlock scalarCounter = counter;
            ScalarCtrXor(scalar, width, scalarCounter, input, expected, capacity);

            // 일회성: 남은 만큼은 되고, 1 byte 더 길면 쓰기 전에 거부한다.
            memset(output, 0xa5, sizeof(output));
            CHECK(hazmat::TryAesCtrXor(key.Value(), counterView,
                                       Span<const uint8>(input).Left(capacity + 1),
                                       Span<uint8>(output).Left(capacity + 1), publicWidth)
                      .Error() == ErrorCode::CapacityExceeded);
            CHECK(output[0] == 0xa5 && output[capacity] == 0xa5);
            REQUIRE(hazmat::TryAesCtrXor(key.Value(), counterView,
                                         Span<const uint8>(input).Left(capacity),
                                         Span<uint8>(output).Left(capacity), publicWidth));
            CHECK(Same(Span<const uint8>(output).Left(capacity),
                       Span<const uint8>(expected).Left(capacity)));

            // 스트림: 실패한 호출은 상태를 바꾸지 않고, 마지막 블록의 남은 keystream은 소진 뒤에도
            // 쓴다. 그 뒤 1 byte는 거부한다.
            auto ctr = hazmat::AesCtr::TryCreate(key.Value(), counterView, publicWidth);
            REQUIRE(ctr);
            memset(output, 0xa5, sizeof(output));
            CHECK(ctr.Value()
                      .TryProcess(Span<const uint8>(input).Left(capacity + 1),
                                  Span<uint8>(output).Left(capacity + 1))
                      .Error() == ErrorCode::CapacityExceeded);
            CHECK(output[0] == 0xa5);
            const usize first = capacity - 5;
            REQUIRE(ctr.Value().TryProcess(Span<const uint8>(input).Left(first),
                                           Span<uint8>(output).Left(first)));
            REQUIRE(ctr.Value().TryProcess(Span<const uint8>(input).Mid(first, 5),
                                           Span<uint8>(output).Mid(first, 5)));
            REQUIRE(ctr.Value().TryProcess(Span<const uint8>(), Span<uint8>()));
            CHECK(ctr.Value()
                      .TryProcess(Span<const uint8>(input).Mid(capacity, 1),
                                  Span<uint8>(output).Mid(capacity, 1))
                      .Error() == ErrorCode::CapacityExceeded);
            CHECK(output[capacity] == 0xa5);
            CHECK(Same(Span<const uint8>(output).Left(capacity),
                       Span<const uint8>(expected).Left(capacity)));
        }

        // 폭보다 좁은 자리(2^32 - k, 2^64 - k)의 올림은 용량과 무관하게 넘어간다.
        for (usize onesBytes = 4; onesBytes < WidthBytes(width); onesBytes *= 2) {
            const AesBlock counter = CounterBelow(random, onesBytes, 3);
            const usize length = 8 * AesBlockBytes + 9;
            AesBlock scalarCounter = counter;
            ScalarCtrXor(scalar, width, scalarCounter, input, expected, length);
            auto ctr = hazmat::AesCtr::TryCreate(key.Value(), BlockView(counter), publicWidth);
            REQUIRE(ctr);
            REQUIRE(ctr.Value().TryProcess(Span<const uint8>(input).Left(33),
                                           Span<uint8>(output).Left(33)));
            REQUIRE(ctr.Value().TryProcess(Span<const uint8>(input).Mid(33, length - 33),
                                           Span<uint8>(output).Mid(33, length - 33)));
            CHECK(Same(Span<const uint8>(output).Left(length),
                       Span<const uint8>(expected).Left(length)));
        }
    }
}
} // namespace aes_layer_tests_file
