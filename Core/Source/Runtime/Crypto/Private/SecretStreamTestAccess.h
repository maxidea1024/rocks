#pragma once

#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Crypto/SecretStream.h>
#include <Platform/Memory.h>

#include "SecretStreamInternal.h"

// 시험이 secretstream 상태(하위 키·카운터·inonce)를 넣고 읽는 모듈 내부 접근점이다. Wycheproof
// secretstream 벡터는 "이 상태에서 push하면 이 암호문과 이 상태가 된다"는 형태라(카운터 2^32 경계의
// 자동 rekey) 공개 API로는 그 상태에 닿을 수 없다. 이 헤더는 사실상 상태 주입 API이므로 Tests/
// 소스만 include한다(정책 검사 test-access-include).
namespace rocks::crypto::detail {

struct SecretStreamTestAccess {
    // state 전체(SecretStreamStateBytes)를 덮는다. 채움 byte는 0이다. 빈 객체면 false다.
    template <typename Stream>
    static bool SetState(Stream& stream, Span<const uint8> key, Span<const uint8> counter,
                         Span<const uint8> inonce) noexcept {
        if (stream.state_ == nullptr || key.Size() != SecretStreamStateKeyBytes ||
            counter.Size() != SecretStreamStateCounterBytes ||
            inonce.Size() != SecretStreamStateInonceBytes) {
            return false;
        }

        uint8* state = static_cast<uint8*>(stream.state_);
        platform::MemoryZero(state, SecretStreamStateBytes);
        platform::MemoryCopy(state + SecretStreamStateKeyOffset, key.Data(), key.Size());
        platform::MemoryCopy(state + SecretStreamStateCounterOffset, counter.Data(),
                             counter.Size());
        platform::MemoryCopy(state + SecretStreamStateInonceOffset, inonce.Data(), inonce.Size());
        return true;
    }

    // state의 앞 44 byte(하위 키 ‖ 카운터 ‖ inonce)를 out에 복사한다. 빈 객체면 false다.
    template <typename Stream>
    static bool GetState(const Stream& stream, Span<uint8> out) noexcept {
        constexpr usize Bytes = SecretStreamStateInonceOffset + SecretStreamStateInonceBytes;
        if (stream.state_ == nullptr || out.Size() != Bytes) {
            return false;
        }

        platform::MemoryCopy(out.Data(), stream.state_, Bytes);
        return true;
    }
};

} // namespace rocks::crypto::detail
