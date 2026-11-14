#pragma once

#include <Base/Types.h>
#include <Crypto/Aead.h>

// 시험이 순번 봉인기의 seq·블록 집계를 끝 근처로 옮기는 모듈 내부 접근점이다(2^64개를 봉인하지 않고
// 경계를 시험한다). 공개 API에는 순번을 이어 가는 길을 두지 않는다(Aead.h 계약, 같은 키로 이어 가면
// nonce가 다시 쓰일 수 있다).
namespace rocks::crypto::detail {

struct SequenceAccess {
    template <typename Sequence>
    static void SetNextSequence(Sequence& sequence, uint64 next) noexcept {
        sequence.next_ = next;
    }

    static void SetUsedBlocks(hazmat::AeadSequenceSealer& sealer, uint64 blocks) noexcept {
        sealer.usedBlocks_ = blocks;
    }
};

} // namespace rocks::crypto::detail
