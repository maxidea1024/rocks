// AES 경량 계층의 ARM64 커널(ARMv8 Crypto Extensions AESE·AESMC,
// PMULL)이다(Docs/Design/Crypto.ko.md 5.2·5.3절). 커널 표 계약은 AesKernels.h에 있다.
//
// 출처: libsodium 1.0.22-stable(ISC)에서 파생했다. 원본 SHA-256은 같은 디렉터리의 Derivation.json이
// 고정한다(정책 검사 derived-source). 함수마다 대응하는 원본 함수 이름을 주석에 적었다.
//   src/libsodium/crypto_aead/aes256gcm/armcrypto/aead_aes256gcm_armcrypto.c
//     AES-256 키 확장(0 라운드 키 AESE로 AESKEYGENASSIST를 흉내 낸다), 블록 암호화, 병렬 CTR,
//     carry-less 곱셈 GHASH, H 거듭제곱 사전 계산
//   src/libsodium/crypto_ipcrypt/ipcrypt_armcrypto.c
//     AES-128 키 확장
// 원본에서 바꾼 점:
// - 라운드 수를 템플릿 인자(10·14)로 두어 AES-128과 AES-256이 같은 코드를 쓴다.
// - 카운터 증가를 64 bit lane 덧셈(vaddq_u64)에서 폭을 고르는 덧셈(32 bit lane = GCM inc32, 64 bit
//   lane, 128 bit 올림)으로 바꿨다. 원본은 메시지 블록 수 한도(2^32 - 2) 안에서만 inc32와 같고,
//   경량 계층은 카운터를 이어 받는 멀티파트에서도 inc32여야 하며 CTR hazmat은 폭을 고른다.
// - CTR과 GHASH를 한 함수에서 엮던 aes_gcm_encrypt_generic·aes_gcm_decrypt_generic을 버퍼 단위
//   커널(CtrXor·GhashUpdate)로 나눴다. GCM 조립은 AesModes.cpp가 한다.
// - 원본의 __ARM_FEATURE_CRYPTO·__ARM_FEATURE_AES 정의(예약 식별자), 파일 전체의 #pragma clang
//   attribute("neon,crypto,aes,fuse-aes"), PREFETCH, __vectorcall, _MSC_VER 분기를 뺐다. target
//   속성은 ROCKS_TARGET_ARM64_AES("neon,aes,fuse-aes", SHA2를 켜는 crypto 제외)로 함수마다
//   건다(5.3절).
// - 64 bit 단위 load·store(vld1q_u64)를 byte 단위(vld1q_u8)로 바꿨다. 결과 byte는 같고 uint64_t와
//   Rocks uint64의 타입 차이(Android LP64)를 피한다.
// - CBC-MAC 누적(CbcMacUpdate)과 부분 블록 keystream 소거는 원본에 없는 새 코드다.
//
// libsodium 고지(ISC License):
//   Copyright (c) 2013-2026 Frank Denis <j at pureftpd dot org>
//
//   Permission to use, copy, modify, and/or distribute this software for any purpose with or
//   without fee is hereby granted, provided that the above copyright notice and this permission
//   notice appear in all copies.
//
//   THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS
//   SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
//   AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
//   WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
//   NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
//   PERFORMANCE OF THIS SOFTWARE.

#include "AesKernels.h"

#include <Base/Compiler.h>

#if ROCKS_ARCH_ARM64

#include <Platform/Fatal.h>
#include <Platform/Memory.h>

#include <sodium.h>

#include <arm_neon.h>

namespace rocks::crypto::detail {
namespace {

using BlockVec = uint64x2_t;

// 원본 PARALLEL_BLOCKS: CTR이 한 번에 겹쳐 돌리는 블록 수다.
constexpr usize ParallelBlocks = 6;

// 원본 PC_COUNT: GHASH가 reduce 한 번에 묶는 최대 블록 수이자 미리 계산하는 H 거듭제곱 수다.
constexpr usize PowerCount = 2 * ParallelBlocks;

static_assert(PowerCount <= AesGhashPowerCapacity && PowerCount % 2 == 0);

// 원본 AES_KEYGEN의 aes_keygen_shuffle: ShiftRows를 되돌리며 AESKEYGENASSIST의 단어 배치(SubWord,
// RotWord(SubWord))로 옮기는 TBL 색인이다. 색인은 상수라 비밀에 따라 주소가 달라지지 않는다.
constexpr uint8 KeygenShuffle[16] = {4, 1, 14, 11, 1, 14, 11, 4, 12, 9, 6, 3, 9, 6, 3, 12};

// 원본 I256: reduce 전 256 bit carry-less 곱의 세 조각이다.
struct I256 {
    BlockVec hi;
    BlockVec lo;
    BlockVec mid;
};

// 원본 LOAD128·STORE128(정렬 요구 없음).
ROCKS_TARGET_ARM64_AES BlockVec Load(const uint8* bytes) noexcept {
    return vreinterpretq_u64_u8(vld1q_u8(bytes));
}

ROCKS_TARGET_ARM64_AES void Store(uint8* bytes, BlockVec value) noexcept {
    vst1q_u8(bytes, vreinterpretq_u8_u64(value));
}

// 원본 XOR128.
ROCKS_TARGET_ARM64_AES BlockVec Xor(BlockVec a, BlockVec b) noexcept {
    return veorq_u64(a, b);
}

// 원본 SET64x2(a, b): 위 64 bit가 high, 아래 64 bit가 low다.
ROCKS_TARGET_ARM64_AES BlockVec Set64x2(uint64 high, uint64 low) noexcept {
    return vsetq_lane_u64(high, vmovq_n_u64(low), 1);
}

// 원본 REV128_: byte 순서를 뒤집는다.
ROCKS_TARGET_ARM64_AES BlockVec Reverse(BlockVec value) noexcept {
    const uint8x16_t swapped = vrev64q_u8(vreinterpretq_u8_u64(value));
    return vreinterpretq_u64_u8(vextq_u8(swapped, swapped, 8));
}

// 원본 BYTESHL128·BYTESHR128: 128 bit를 Bytes byte만큼 민다(0을 채운다). EXT의 즉시값이라 템플릿
// 인자다.
template <int Bytes>
ROCKS_TARGET_ARM64_AES BlockVec ShiftLeftBytes(BlockVec value) noexcept {
    return vreinterpretq_u64_u8(vextq_u8(vdupq_n_u8(0), vreinterpretq_u8_u64(value), 16 - Bytes));
}

template <int Bytes>
ROCKS_TARGET_ARM64_AES BlockVec ShiftRightBytes(BlockVec value) noexcept {
    return vreinterpretq_u64_u8(vextq_u8(vreinterpretq_u8_u64(value), vdupq_n_u8(0), Bytes));
}

// 원본 SHL128(a, 1): 128 bit 전체를 왼쪽으로 1 bit 민다.
ROCKS_TARGET_ARM64_AES BlockVec ShiftLeftOne(BlockVec value) noexcept {
    return vorrq_u64(vshlq_n_u64(value, 1), vshrq_n_u64(ShiftLeftBytes<8>(value), 63));
}

// 원본 SWAP64HALVES.
ROCKS_TARGET_ARM64_AES BlockVec SwapHalves(BlockVec value) noexcept {
    const uint32x4_t words = vreinterpretq_u32_u64(value);
    return vreinterpretq_u64_u32(vextq_u32(words, words, 2));
}

// 원본 SHUFFLE32x4_3333·SHUFFLE32x4_2222: 32 bit 단어 하나를 네 칸에 복사한다.
ROCKS_TARGET_ARM64_AES BlockVec BroadcastWord3(BlockVec value) noexcept {
    return vreinterpretq_u64_u32(vdupq_laneq_u32(vreinterpretq_u32_u64(value), 3));
}

ROCKS_TARGET_ARM64_AES BlockVec BroadcastWord2(BlockVec value) noexcept {
    return vreinterpretq_u64_u32(vdupq_laneq_u32(vreinterpretq_u32_u64(value), 2));
}

// 원본 CLMULLO128·CLMULHI128·CLMULLOHI128·CLMULHILO128: 64 bit 반쪽끼리의 carry-less 곱(PMULL·
// PMULL2)이다.
ROCKS_TARGET_ARM64_AES BlockVec MultiplyLowLow(BlockVec a, BlockVec b) noexcept {
    return vreinterpretq_u64_p128(vmull_p64(vgetq_lane_p64(vreinterpretq_p64_u64(a), 0),
                                            vgetq_lane_p64(vreinterpretq_p64_u64(b), 0)));
}

ROCKS_TARGET_ARM64_AES BlockVec MultiplyHighHigh(BlockVec a, BlockVec b) noexcept {
    return vreinterpretq_u64_p128(
        vmull_high_p64(vreinterpretq_p64_u64(a), vreinterpretq_p64_u64(b)));
}

ROCKS_TARGET_ARM64_AES BlockVec MultiplyLowHigh(BlockVec a, BlockVec b) noexcept {
    return vreinterpretq_u64_p128(vmull_p64(vgetq_lane_p64(vreinterpretq_p64_u64(a), 0),
                                            vgetq_lane_p64(vreinterpretq_p64_u64(b), 1)));
}

ROCKS_TARGET_ARM64_AES BlockVec MultiplyHighLow(BlockVec a, BlockVec b) noexcept {
    return vreinterpretq_u64_p128(vmull_p64(vgetq_lane_p64(vreinterpretq_p64_u64(a), 1),
                                            vgetq_lane_p64(vreinterpretq_p64_u64(b), 0)));
}

// byte 역순 카운터(lane 0이 블록 끝 byte 쪽)에 1을 더한다. 원본 ADD64x2(counter, ONE128) 자리다.
// - Bits32: 32 bit lane 0만 더한다(GCM inc32).
// - Bits64: 64 bit lane 0만 더한다.
// - Bits128: 64 bit lane 0을 더하고, 그 결과가 0이면(올림) lane 1에 1을 더한다. 올림은 비교
//   mask(CMEQ)를 위 lane으로 옮겨 빼서 분기 없이 만든다.
template <AesCounterWidth Width>
ROCKS_TARGET_ARM64_AES BlockVec Increment(BlockVec counter) noexcept {
    if constexpr (Width == AesCounterWidth::Bits32) {
        const uint32x4_t one = vsetq_lane_u32(1, vdupq_n_u32(0), 0);
        return vreinterpretq_u64_u32(vaddq_u32(vreinterpretq_u32_u64(counter), one));
    } else if constexpr (Width == AesCounterWidth::Bits64) {
        return vaddq_u64(counter, Set64x2(0, 1));
    } else {
        const BlockVec added = vaddq_u64(counter, Set64x2(0, 1));
        const BlockVec wrapped = vceqq_u64(added, vmovq_n_u64(0));
        return vsubq_u64(added, ShiftLeftBytes<8>(wrapped));
    }
}

ROCKS_TARGET_ARM64_AES BlockVec RoundKey(const AesSchedule& schedule, usize index) noexcept {
    return Load(schedule.roundKeys[index].bytes);
}

ROCKS_TARGET_ARM64_AES void StoreRoundKey(AesSchedule& schedule, usize index,
                                          BlockVec value) noexcept {
    Store(schedule.roundKeys[index].bytes, value);
}

ROCKS_TARGET_ARM64_AES BlockVec Power(const AesSchedule& schedule, usize index) noexcept {
    return Load(schedule.hPowers[index].bytes);
}

ROCKS_TARGET_ARM64_AES void StorePower(AesSchedule& schedule, usize index,
                                       BlockVec value) noexcept {
    Store(schedule.hPowers[index].bytes, value);
}

// ---- AES 라운드 ----

// 원본 AES_XENCRYPT: AESE(AddRoundKey·SubBytes·ShiftRows) 뒤 AESMC(MixColumns).
ROCKS_TARGET_ARM64_AES BlockVec EncryptRound(BlockVec block, BlockVec key) noexcept {
    return vreinterpretq_u64_u8(
        vaesmcq_u8(vaeseq_u8(vreinterpretq_u8_u64(key), vreinterpretq_u8_u64(block))));
}

// 원본 AES_XENCRYPTLAST: MixColumns 없는 마지막 AESE.
ROCKS_TARGET_ARM64_AES BlockVec EncryptLastRound(BlockVec block, BlockVec key) noexcept {
    return vreinterpretq_u64_u8(vaeseq_u8(vreinterpretq_u8_u64(key), vreinterpretq_u8_u64(block)));
}

// ---- 키 확장 ----

// 원본 AES_KEYGEN: x86 AESKEYGENASSIST(block, rcon)과 같은 값을 0 라운드 키 AESE(SubBytes·
// ShiftRows)와 TBL 재배치로 만든다.
ROCKS_TARGET_ARM64_AES BlockVec KeygenAssist(BlockVec block, uint32 rcon) noexcept {
    const uint8x16_t substituted = vaeseq_u8(vreinterpretq_u8_u64(block), vdupq_n_u8(0));
    const uint8x16_t arranged = vqtbl1q_u8(substituted, vld1q_u8(KeygenShuffle));
    const uint64 rconWords = uint64{rcon} << 32;
    return Xor(vreinterpretq_u64_u8(arranged), Set64x2(rconWords, rconWords));
}

// 원본 EXPAND_KEY의 뒤 세 줄: 앞 라운드 키의 32 bit 단어를 누적 XOR하고 word를 더한다.
ROCKS_TARGET_ARM64_AES BlockVec ChainKeyWords(BlockVec previous, BlockVec word) noexcept {
    BlockVec chained = Xor(previous, ShiftLeftBytes<4>(previous));
    chained = Xor(chained, ShiftLeftBytes<8>(chained));
    return Xor(chained, word);
}

// 원본 expand256 EXPAND_KEY_1·ipcrypt_armcrypto.c expand_key EXPAND_KEY: RotWord(SubWord(last의
// 마지막 단어)) xor Rcon을 더한다.
ROCKS_TARGET_ARM64_AES BlockVec NextRotatedKey(BlockVec previous, BlockVec last,
                                               uint32 rcon) noexcept {
    return ChainKeyWords(previous, BroadcastWord3(KeygenAssist(last, rcon)));
}

// 원본 expand256 EXPAND_KEY_2: SubWord(last의 마지막 단어)를 더한다(회전·Rcon 없음).
ROCKS_TARGET_ARM64_AES BlockVec NextSubWordKey(BlockVec previous, BlockVec last,
                                               uint32 rcon) noexcept {
    return ChainKeyWords(previous, BroadcastWord2(KeygenAssist(last, rcon)));
}

// 원본 ipcrypt_armcrypto.c expand_key: AES-128 라운드 키 11개.
ROCKS_TARGET_ARM64_AES void ExpandKey128(const uint8* key, AesSchedule& schedule) noexcept {
    static constexpr uint32 Rcons[10] = {0x01, 0x02, 0x04, 0x08, 0x10,
                                         0x20, 0x40, 0x80, 0x1b, 0x36};
    BlockVec t = Load(key);
    StoreRoundKey(schedule, 0, t);
    for (usize i = 0; i < 10; ++i) {
        t = NextRotatedKey(t, t, Rcons[i]);
        StoreRoundKey(schedule, i + 1, t);
    }
}

// 원본 expand256: AES-256 라운드 키 15개. t1은 짝수 번째, t2는 홀수 번째 라운드 키다.
ROCKS_TARGET_ARM64_AES void ExpandKey256(const uint8* key, AesSchedule& schedule) noexcept {
    BlockVec t1 = Load(key);
    BlockVec t2 = Load(key + 16);
    StoreRoundKey(schedule, 0, t1);
    StoreRoundKey(schedule, 1, t2);
    uint32 rcon = 0x01;
    for (usize i = 2; i < 14; i += 2) {
        t1 = NextRotatedKey(t1, t2, rcon);
        StoreRoundKey(schedule, i, t1);
        t2 = NextSubWordKey(t2, t1, rcon);
        StoreRoundKey(schedule, i + 1, t2);
        rcon <<= 1;
    }

    t1 = NextRotatedKey(t1, t2, rcon);
    StoreRoundKey(schedule, 14, t1);
}

// ---- 블록 암호화 ----

// 원본 encrypt: 블록 하나를 Rounds 라운드로 암호화한다.
template <usize Rounds>
ROCKS_TARGET_ARM64_AES BlockVec EncryptVector(const AesSchedule& schedule,
                                              BlockVec block) noexcept {
    BlockVec state = EncryptRound(block, RoundKey(schedule, 0));
    for (usize round = 1; round < Rounds - 1; ++round) {
        state = EncryptRound(state, RoundKey(schedule, round));
    }

    state = EncryptLastRound(state, RoundKey(schedule, Rounds - 1));
    return Xor(state, RoundKey(schedule, Rounds));
}

// 원본 encrypt_xor_block: output = input xor E_K(counter). output은 input과 같아도 된다.
template <usize Rounds>
ROCKS_TARGET_ARM64_AES void EncryptXorBlock(const AesSchedule& schedule, uint8* output,
                                            const uint8* input, BlockVec counter) noexcept {
    Store(output, Xor(EncryptVector<Rounds>(schedule, counter), Load(input)));
}

// 원본 encrypt_xor_wide: ParallelBlocks개 블록을 라운드마다 겹쳐 암호화하고 input에 XOR한다. 모든
// 블록을 읽은 뒤 쓰므로 output은 input과 같아도 된다.
template <usize Rounds>
ROCKS_TARGET_ARM64_AES void EncryptXorWide(const AesSchedule& schedule, uint8* output,
                                           const uint8* input,
                                           const BlockVec (&counters)[ParallelBlocks]) noexcept {
    BlockVec states[ParallelBlocks];
    const BlockVec first = RoundKey(schedule, 0);
    for (usize j = 0; j < ParallelBlocks; ++j) {
        states[j] = EncryptRound(counters[j], first);
    }

    for (usize round = 1; round < Rounds - 1; ++round) {
        const BlockVec key = RoundKey(schedule, round);
        for (usize j = 0; j < ParallelBlocks; ++j) {
            states[j] = EncryptRound(states[j], key);
        }
    }

    const BlockVec penultimate = RoundKey(schedule, Rounds - 1);
    const BlockVec last = RoundKey(schedule, Rounds);
    for (usize j = 0; j < ParallelBlocks; ++j) {
        states[j] = EncryptLastRound(states[j], penultimate);
        states[j] = Xor(states[j], Xor(last, Load(input + AesBlockBytes * j)));
    }

    for (usize j = 0; j < ParallelBlocks; ++j) {
        Store(output + AesBlockBytes * j, states[j]);
    }
}

// 원본 incr_counters(n = ParallelBlocks): 카운터 블록들(원래 byte 순서)을 만들고 다음 카운터를
// 돌려준다. counter는 byte 역순으로 들고 다닌다.
template <AesCounterWidth Width>
ROCKS_TARGET_ARM64_AES BlockVec IncrementCounters(BlockVec (&counters)[ParallelBlocks],
                                                  BlockVec counter) noexcept {
    for (usize j = 0; j < ParallelBlocks; ++j) {
        counters[j] = Reverse(counter);
        counter = Increment<Width>(counter);
    }

    return counter;
}

// CtrXor 본체다. 원본 aes_gcm_encrypt_generic의 CTR 부분(병렬 블록 → 블록 하나씩 → 마지막 부분
// 블록의 last_blocks 처리)을 따른다.
template <usize Rounds, AesCounterWidth Width>
ROCKS_TARGET_ARM64_AES void CtrXorRounds(const AesSchedule& schedule, AesBlock& counterBlock,
                                         const uint8* input, uint8* output, usize bytes) noexcept {
    BlockVec counter = Reverse(Load(counterBlock.bytes));
    BlockVec counters[ParallelBlocks];
    usize offset = 0;
    while (bytes - offset >= ParallelBlocks * AesBlockBytes) {
        counter = IncrementCounters<Width>(counters, counter);
        EncryptXorWide<Rounds>(schedule, output + offset, input + offset, counters);
        offset += ParallelBlocks * AesBlockBytes;
    }

    while (bytes - offset >= AesBlockBytes) {
        EncryptXorBlock<Rounds>(schedule, output + offset, input + offset, Reverse(counter));
        counter = Increment<Width>(counter);
        offset += AesBlockBytes;
    }

    if (offset < bytes) {
        // 부분 블록은 스택 블록에 옮겨 암호화한다(입력 끝 너머를 읽지 않는다). 쓰지 않은
        // keystream이 남으므로 지운다.
        const usize left = bytes - offset;
        alignas(16) uint8 last[AesBlockBytes] = {};
        platform::MemoryCopy(last, input + offset, left);
        EncryptXorBlock<Rounds>(schedule, last, last, Reverse(counter));
        platform::MemoryCopy(output + offset, last, left);
        counter = Increment<Width>(counter);
        sodium_memzero(last, sizeof(last));
    }

    Store(counterBlock.bytes, Reverse(counter));
}

// CBC-MAC 본체다(원본에 없는 새 코드, encrypt와 같은 라운드).
template <usize Rounds>
ROCKS_TARGET_ARM64_AES void CbcMacRounds(const AesSchedule& schedule, AesBlock& state,
                                         const uint8* data, usize blocks) noexcept {
    BlockVec chained = Load(state.bytes);
    for (usize i = 0; i < blocks; ++i) {
        chained = EncryptVector<Rounds>(schedule, Xor(chained, Load(data + AesBlockBytes * i)));
    }

    Store(state.bytes, chained);
}

// ---- GHASH ----

// 원본 clsq128: 제곱(가운데 항이 없다).
ROCKS_TARGET_ARM64_AES I256 Square(BlockVec x) noexcept {
    return I256{MultiplyHighHigh(x, x), MultiplyLowLow(x, x), vmovq_n_u64(0)};
}

// 원본 clmul128(USE_KARATSUBA_MULTIPLICATION 미정의 경로): 교과서식 64 bit 곱 4회다.
ROCKS_TARGET_ARM64_AES I256 Multiply(BlockVec x, BlockVec y) noexcept {
    const BlockVec hi = MultiplyHighHigh(x, y);
    const BlockVec lo = MultiplyLowLow(x, y);
    const BlockVec mid = Xor(MultiplyHighLow(x, y), MultiplyLowHigh(x, y));
    return I256{hi, lo, mid};
}

// 원본 gcm_reduce: 가운데 항을 합치고 GCM 다항식으로 줄인다.
ROCKS_TARGET_ARM64_AES BlockVec Reduce(const I256& x) noexcept {
    const BlockVec hi = Xor(x.hi, ShiftRightBytes<8>(x.mid));
    const BlockVec lo = Xor(x.lo, ShiftLeftBytes<8>(x.mid));
    const BlockVec p64 = Set64x2(0, 0xc200000000000000);
    const BlockVec a = MultiplyLowLow(lo, p64);
    const BlockVec b = Xor(SwapHalves(lo), a);
    const BlockVec c = MultiplyLowLow(b, p64);
    const BlockVec d = Xor(SwapHalves(b), c);
    return Xor(d, hi);
}

// 원본 precomp: H 거듭제곱 hPowers[i] = H^(i + 1)을 from부터 to 앞까지 채운다.
ROCKS_TARGET_ARM64_AES void PrecomputeRange(AesSchedule& schedule, usize from, usize to) noexcept {
    const BlockVec h = Power(schedule, 0);
    for (usize i = from & ~usize{1}; i < to; i += 2) {
        StorePower(schedule, i, Reduce(Multiply(Power(schedule, i - 1), h)));
        StorePower(schedule, i + 1, Reduce(Square(Power(schedule, i / 2))));
    }
}

// 원본 precomp_for_block_count(block_count = PC_COUNT): ghashKey(H, GCM byte 순서)를 byte 역순으로
// 바꾸고 x를 곱해(한 bit 왼쪽, 올림은 분기 없는 mask로 줄인다) 거듭제곱을 채운다.
ROCKS_TARGET_ARM64_AES void PrecomputePowers(AesSchedule& schedule,
                                             const uint8* ghashKey) noexcept {
    const BlockVec h0 = Reverse(Load(ghashKey));
    BlockVec carry = Set64x2(0xc200000000000000, 1);
    BlockVec mask = vsubq_u64(vmovq_n_u64(0), vshrq_n_u64(h0, 63));
    mask = BroadcastWord3(mask);
    carry = vandq_u64(carry, mask);
    const BlockVec h = Xor(ShiftLeftOne(h0), carry);
    StorePower(schedule, 0, h);
    StorePower(schedule, 1, Reduce(Square(h)));
    PrecomputeRange(schedule, 2, PowerCount);
}

// 원본 gh_update0: 누적값에 첫 블록을 더해 power를 곱한다.
ROCKS_TARGET_ARM64_AES I256 MultiplyFirst(BlockVec accumulator, const uint8* block,
                                          BlockVec power) noexcept {
    return Multiply(Xor(accumulator, Reverse(Load(block))), power);
}

// 원본 gh_update: 블록 · power를 reduce 전 합에 더한다.
ROCKS_TARGET_ARM64_AES void MultiplyAdd(I256& sum, const uint8* block, BlockVec power) noexcept {
    const I256 product = Multiply(Reverse(Load(block)), power);
    sum.hi = Xor(sum.hi, product.hi);
    sum.lo = Xor(sum.lo, product.lo);
    sum.mid = Xor(sum.mid, product.mid);
}

// 원본 gh_ad_blocks의 묶음 하나: Count개 블록을 H^Count..H^1과 곱해 reduce 한 번으로 흡수한다.
template <usize Count>
ROCKS_TARGET_ARM64_AES BlockVec AbsorbGroup(const AesSchedule& schedule, BlockVec accumulator,
                                            const uint8* data) noexcept {
    I256 sum = MultiplyFirst(accumulator, data, Power(schedule, Count - 1));
    for (usize j = 1; j < Count; ++j) {
        MultiplyAdd(sum, data + AesBlockBytes * j, Power(schedule, Count - 1 - j));
    }

    return Reduce(sum);
}

// 원본 gh_ad_blocks: bytes(16의 배수)를 PowerCount·PowerCount / 2·4·2·1개 묶음 순서로 흡수한다.
ROCKS_TARGET_ARM64_AES BlockVec AbsorbBlocks(const AesSchedule& schedule, BlockVec accumulator,
                                             const uint8* data, usize bytes) noexcept {
    usize i = 0;
    for (; i + PowerCount * AesBlockBytes <= bytes; i += PowerCount * AesBlockBytes) {
        accumulator = AbsorbGroup<PowerCount>(schedule, accumulator, data + i);
    }

    for (; i + PowerCount / 2 * AesBlockBytes <= bytes; i += PowerCount / 2 * AesBlockBytes) {
        accumulator = AbsorbGroup<PowerCount / 2>(schedule, accumulator, data + i);
    }

    for (; i + 4 * AesBlockBytes <= bytes; i += 4 * AesBlockBytes) {
        accumulator = AbsorbGroup<4>(schedule, accumulator, data + i);
    }

    for (; i + 2 * AesBlockBytes <= bytes; i += 2 * AesBlockBytes) {
        accumulator = AbsorbGroup<2>(schedule, accumulator, data + i);
    }

    if (i < bytes) {
        accumulator = AbsorbGroup<1>(schedule, accumulator, data + i);
    }

    return accumulator;
}

// ---- 커널 표 함수 ----

[[noreturn]] void FailBadRounds() noexcept {
    platform::Fatal("Crypto: AES key schedule has an invalid round count");
}

// 원본 crypto_aead_aes256gcm_beforenm: 라운드 키를 만들고 H = E_K(0^128)의 거듭제곱을 채운다.
ROCKS_TARGET_ARM64_AES void ExpandKey(const uint8* key, usize keyBytes,
                                      AesSchedule& schedule) noexcept {
    platform::MemoryZero(&schedule, sizeof(schedule));
    alignas(16) uint8 h[AesBlockBytes] = {};
    if (keyBytes == 16) {
        ExpandKey128(key, schedule);
        schedule.rounds = 10;
        Store(h, EncryptVector<10>(schedule, vmovq_n_u64(0)));
    } else if (keyBytes == 32) {
        ExpandKey256(key, schedule);
        schedule.rounds = 14;
        Store(h, EncryptVector<14>(schedule, vmovq_n_u64(0)));
    } else {
        platform::Fatal("Crypto: AES key expansion got a key that is not 16 or 32 bytes");
    }

    PrecomputePowers(schedule, h);
    sodium_memzero(h, sizeof(h));
}

// 라운드 수를 고른 CTR이다. 폭은 버퍼마다 한 번 고른다(블록마다 분기하지 않는다).
template <usize Rounds>
ROCKS_TARGET_ARM64_AES void CtrXorWidth(const AesSchedule& schedule, AesCounterWidth width,
                                        AesBlock& counter, const uint8* input, uint8* output,
                                        usize bytes) noexcept {
    switch (width) {
    case AesCounterWidth::Bits32:
        CtrXorRounds<Rounds, AesCounterWidth::Bits32>(schedule, counter, input, output, bytes);
        return;
    case AesCounterWidth::Bits64:
        CtrXorRounds<Rounds, AesCounterWidth::Bits64>(schedule, counter, input, output, bytes);
        return;
    case AesCounterWidth::Bits128:
        CtrXorRounds<Rounds, AesCounterWidth::Bits128>(schedule, counter, input, output, bytes);
        return;
    }

    platform::Fatal("Crypto: AES CTR got an invalid counter width");
}

ROCKS_TARGET_ARM64_AES void CtrXor(const AesSchedule& schedule, AesCounterWidth width,
                                   AesBlock& counter, const uint8* input, uint8* output,
                                   usize bytes) noexcept {
    switch (schedule.rounds) {
    case 10:
        CtrXorWidth<10>(schedule, width, counter, input, output, bytes);
        return;
    case 14:
        CtrXorWidth<14>(schedule, width, counter, input, output, bytes);
        return;
    }

    FailBadRounds();
}

// GHASH 누적이다. 원본 gh_ad_blocks에 aes_gcm_encrypt_generic의 부분 블록 0 채우기(pad)를 더했다.
// state는 GCM byte 순서로 두고, 원본의 누적값(GHash.acc)은 그 byte 역순이다.
ROCKS_TARGET_ARM64_AES void GhashUpdate(const AesSchedule& schedule, AesBlock& state,
                                        const uint8* data, usize bytes) noexcept {
    const usize whole = bytes & ~(AesBlockBytes - 1);
    BlockVec accumulator = Reverse(Load(state.bytes));
    accumulator = AbsorbBlocks(schedule, accumulator, data, whole);
    if (whole < bytes) {
        alignas(16) uint8 pad[AesBlockBytes] = {};
        platform::MemoryCopy(pad, data + whole, bytes - whole);
        accumulator = AbsorbBlocks(schedule, accumulator, pad, AesBlockBytes);
        sodium_memzero(pad, sizeof(pad));
    }

    Store(state.bytes, Reverse(accumulator));
}

ROCKS_TARGET_ARM64_AES void CbcMacUpdate(const AesSchedule& schedule, AesBlock& state,
                                         const uint8* data, usize blocks) noexcept {
    switch (schedule.rounds) {
    case 10:
        CbcMacRounds<10>(schedule, state, data, blocks);
        return;
    case 14:
        CbcMacRounds<14>(schedule, state, data, blocks);
        return;
    }

    FailBadRounds();
}

ROCKS_TARGET_ARM64_AES void EncryptBlock(const AesSchedule& schedule, const AesBlock& input,
                                         AesBlock& output) noexcept {
    switch (schedule.rounds) {
    case 10:
        Store(output.bytes, EncryptVector<10>(schedule, Load(input.bytes)));
        return;
    case 14:
        Store(output.bytes, EncryptVector<14>(schedule, Load(input.bytes)));
        return;
    }

    FailBadRounds();
}

} // namespace

const AesKernels& GetNativeAesKernels() noexcept {
    static constexpr AesKernels Kernels = {
        .expandKey = &ExpandKey,
        .ctrXor = &CtrXor,
        .ghashUpdate = &GhashUpdate,
        .cbcMacUpdate = &CbcMacUpdate,
        .encryptBlock = &EncryptBlock,
        .name = StringView("rocks-aes/arm64"),
    };
    return Kernels;
}

} // namespace rocks::crypto::detail

#endif
