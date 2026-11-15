#pragma once

#include <Base/Types.h>
#include <Crypto/Aead.h>

// 시험이 순번 봉인기의 seq·블록 집계를 끝 근처로 옮기는 모듈 내부 접근점이다(2^64개를 봉인하지 않고
// 경계를 시험한다). 공개 API에는 순번을 이어 가는 길을 두지 않는다(Aead.h 계약, 같은 키로 이어 가면
// nonce가 다시 쓰일 수 있다). 이 헤더는 사실상 이어 가기 API이므로 Tests/ 소스만 include한다(정책
// 검사 test-access-include, 이름이 *TestAccess.h인 헤더 모두).
namespace rocks::crypto::detail {

struct SequenceTestAccess {
    template <typename Sequence>
    static void SetNextSequence(Sequence& sequence, uint64 next) noexcept {
        sequence.next_ = next;
    }

    static void SetUsedBlocks(hazmat::AeadSequenceSealer& sealer, uint64 blocks) noexcept {
        sealer.usedBlocks_ = blocks;
    }
};

} // namespace rocks::crypto::detail
