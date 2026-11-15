#pragma once

#include <Base/Types.h>
#include <Crypto/SegmentStream.h>

#include "SegmentStreamInternal.h"

// 시험이 순차 세그먼트 암·복호기의 위치(세그먼트 번호·누적 크기)를 끝 근처로 옮기는 모듈 내부
// 접근점이다. 세그먼트 2^32개·AES 2^40 byte 경계를 실제 데이터 없이
// 시험한다(Docs/Design/Crypto.ko.md 12절 "최대 길이 경계"). 공개 API에는 위치를 옮기는 길이
// 없다(같은 키 파생으로 위치를 되돌리면 nonce가 다시 쓰인다). Tests/ 소스만 include한다(정책 검사
// test-access-include).
namespace rocks::crypto::detail {

struct SegmentStreamTestAccess {
    // 다음에 봉인할 세그먼트 번호와 지금까지 소비한 평문 합을 바꾼다. 버퍼에 평문이 없을
    // 때만(헤더를 내보낸 직후 등) 되고, 빈 객체나 평문이 남아 있으면 false다.
    static bool SetEncryptorPosition(SegmentStreamEncryptor& encryptor, uint64 index,
                                     uint64 plaintextBytes) noexcept {
        auto* state = static_cast<SegmentEncryptorState*>(encryptor.state_);
        if (state == nullptr || state->plaintextFill != 0) {
            return false;
        }

        state->index = index;
        state->plaintextBytes = plaintextBytes;
        return true;
    }

    // 다음에 열 세그먼트 번호와 연 세그먼트 암호문 합을 바꾼다. 헤더를 처리했고 버퍼가 비었을 때만
    // 되고, 아니면 false다.
    static bool SetDecryptorPosition(SegmentStreamDecryptor& decryptor, uint64 index,
                                     uint64 bodyBytes) noexcept {
        auto* state = static_cast<SegmentDecryptorState*>(decryptor.state_);
        if (state == nullptr || state->buffer == nullptr || state->bufferFill != 0 ||
            state->pendingBegin != state->pendingEnd) {
            return false;
        }

        state->index = index;
        state->bodyBytes = bodyBytes;
        return true;
    }
};

} // namespace rocks::crypto::detail
