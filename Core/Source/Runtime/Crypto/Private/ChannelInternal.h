#pragma once

#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Core/Result.h>
#include <Crypto/Channel.h>

// 보안 채널 handshake의 모듈 내부 진입점이다(Docs/Design/SecureChannel.ko.md 3.1절). 구현은
// ChannelHandshake.cpp다.
namespace rocks::crypto::detail {

// ephemeral 비밀키를 값으로 받는 메시지 쓰기다(Crypto 11절 결정성: 바꿔 둘 수 있는 전역 hook 없이
// 값을 받는 함수). 공개 TryWriteMessage는 빈 ephemeralSecret을 넘겨 OS 난수 키를 키 저장소에 바로
// 만들고, 시험은 고정 키(Noise 공식 벡터의 init_ephemeral·resp_ephemeral)로 golden을 재현한다.
// ephemeralSecret이 비어 있지도 32 byte도 아니면 InvalidArgument(상태 불변)이고, 나머지 인자·오류는
// 공개 TryWriteMessage와 같다.
struct ROCKS_CRYPTO_API ChannelFactory {
    [[nodiscard]] static Result<usize> TryWriteMessage(NoiseHandshake& handshake,
                                                       Span<const uint8> ephemeralSecret,
                                                       Span<const uint8> payload,
                                                       Span<uint8> message) noexcept;
};

} // namespace rocks::crypto::detail
