#pragma once

#include <Base/Types.h>
#include <Crypto/Channel.h>

// 시험이 datagram 봉인기의 counter와 열기의 인증 실패 수를 끝 근처로 옮기는 모듈 내부 접근점이다
// (2^63개를 봉인하거나 2^36번 위조하지 않고 경계를 시험한다). 공개 API에는 counter를 지정하는 길을
// 두지 않는다(같은 키로 counter를 되돌리면 nonce가 다시 쓰인다). 이 헤더는 사실상 counter 지정
// API이므로 Tests/ 소스만 include한다(정책 검사 test-access-include).
namespace rocks::crypto::detail {

struct ChannelTestAccess {
    static void SetNextCounter(DatagramSealer& sealer, uint64 next) noexcept {
        sealer.next_ = next;
    }

    static void SetAuthenticationFailures(DatagramOpener& opener, uint64 failures) noexcept {
        opener.failures_ = failures;
    }
};

} // namespace rocks::crypto::detail
