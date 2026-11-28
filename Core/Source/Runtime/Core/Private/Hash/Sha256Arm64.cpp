#include "Sha256Kernels.h"

#include <Base/Compiler.h>

// ARMv8 SHA2 SHA-256 커널이다(Docs/Design/CoreDataFoundations.ko.md 6절). Sha256Kernel::Arm64Sha2
// 조건(armSha2)을 채운 CPU에서만 커널 표를 거쳐 부른다.
#if ROCKS_ARCH_ARM64

#include <arm_neon.h>

namespace rocks::detail {

ROCKS_TARGET_ARM64_SHA2 void Sha256BlocksArm64Sha2(uint32* state, const uint8* blocks,
                                                   usize count) noexcept {
    uint32x4_t abcd = vld1q_u32(state);
    uint32x4_t efgh = vld1q_u32(state + 4);
    for (usize block = 0; block < count; ++block) {
        const uint8* data = blocks + block * 64u;
        const uint32x4_t savedAbcd = abcd;
        const uint32x4_t savedEfgh = efgh;
        // words[r & 3]는 묶음 r의 메시지 word 넷이다(앞 넷은 입력, 그 뒤는 일정 계산).
        uint32x4_t words[4];
        for (int group = 0; group < 4; ++group) {
            words[group] = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(data + group * 16)));
        }

        for (int group = 0; group < Sha256MessageGroups; ++group) {
            if (group >= 4) {
                // W[r] = SU1(SU0(W[r-4], W[r-3]), W[r-2], W[r-1])
                words[group & 3] =
                    vsha256su1q_u32(vsha256su0q_u32(words[group & 3], words[(group + 1) & 3]),
                                    words[(group + 2) & 3], words[(group + 3) & 3]);
            }

            const uint32x4_t message =
                vaddq_u32(words[group & 3], vld1q_u32(Sha256RoundConstants + group * 4));
            const uint32x4_t previous = abcd;
            abcd = vsha256hq_u32(abcd, efgh, message);
            efgh = vsha256h2q_u32(efgh, previous, message);
        }

        abcd = vaddq_u32(abcd, savedAbcd);
        efgh = vaddq_u32(efgh, savedEfgh);
    }

    vst1q_u32(state, abcd);
    vst1q_u32(state + 4, efgh);
}

} // namespace rocks::detail

#endif
