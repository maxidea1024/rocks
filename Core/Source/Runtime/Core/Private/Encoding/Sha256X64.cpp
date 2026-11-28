#include "Sha256Kernels.h"

#include <Base/Compiler.h>

// x64 SHA-NI SHA-256 커널이다(Docs/Design/CoreDataFoundations.ko.md 6절). Intel "SHA Extensions"
// 백서의 상태 배치(ABEF·CDGH)를 따른다. Sha256Kernel::X64Sha 조건(sha && sse42)을 채운 CPU에서만
// 커널 표를 거쳐 부른다.
#if ROCKS_ARCH_X64

#include <immintrin.h>

namespace rocks::detail {

namespace {

// 블록 안 word 묶음을 big-endian으로 바꾸는 PSHUFB 표다(32 bit word마다 byte 뒤집기).
ROCKS_TARGET_X64_SHA __m128i ByteSwapMask() noexcept {
    return _mm_set_epi64x(0x0c0d0e0f08090a0bll, 0x0405060700010203ll);
}

} // namespace

ROCKS_TARGET_X64_SHA void Sha256BlocksX64Sha(uint32* state, const uint8* blocks,
                                             usize count) noexcept {
    if (count == 0) {
        return;
    }

    const __m128i mask = ByteSwapMask();
    // state[0..7] = A..H를 SHA256RNDS2가 쓰는 ABEF·CDGH 배치로 바꾼다.
    const __m128i low =
        _mm_shuffle_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(state)), 0xB1);
    const __m128i high =
        _mm_shuffle_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(state + 4)), 0x1B);
    __m128i abef = _mm_alignr_epi8(low, high, 8);
    __m128i cdgh = _mm_blend_epi16(high, low, 0xF0);
    for (usize block = 0; block < count; ++block) {
        const uint8* data = blocks + block * 64u;
        const __m128i savedAbef = abef;
        const __m128i savedCdgh = cdgh;
        // words[r & 3]는 묶음 r의 메시지 word 넷이다(앞 넷은 입력, 그 뒤는 일정 계산).
        __m128i words[4];
        for (int group = 0; group < 4; ++group) {
            words[group] = _mm_shuffle_epi8(
                _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + group * 16)), mask);
        }

        for (int group = 0; group < Sha256MessageGroups; ++group) {
            __m128i& current = words[group & 3];
            if (group >= 4) {
                // W[r] = MSG2(MSG1(W[r-4], W[r-3]) + ALIGNR(W[r-1], W[r-2], 4), W[r-1])
                __m128i next = _mm_sha256msg1_epu32(words[group & 3], words[(group + 1) & 3]);
                next = _mm_add_epi32(
                    next, _mm_alignr_epi8(words[(group + 3) & 3], words[(group + 2) & 3], 4));
                current = _mm_sha256msg2_epu32(next, words[(group + 3) & 3]);
            }

            __m128i message =
                _mm_add_epi32(current, _mm_loadu_si128(reinterpret_cast<const __m128i*>(
                                           Sha256RoundConstants + group * 4)));
            cdgh = _mm_sha256rnds2_epu32(cdgh, abef, message);
            message = _mm_shuffle_epi32(message, 0x0E);
            abef = _mm_sha256rnds2_epu32(abef, cdgh, message);
        }

        abef = _mm_add_epi32(abef, savedAbef);
        cdgh = _mm_add_epi32(cdgh, savedCdgh);
    }

    // ABEF·CDGH를 A..H 순서로 되돌린다.
    const __m128i feba = _mm_shuffle_epi32(abef, 0x1B);
    cdgh = _mm_shuffle_epi32(cdgh, 0xB1);
    const __m128i first = _mm_blend_epi16(feba, cdgh, 0xF0);
    const __m128i second = _mm_alignr_epi8(cdgh, feba, 8);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(state), first);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(state + 4), second);
}

} // namespace rocks::detail

#endif
