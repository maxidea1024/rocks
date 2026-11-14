#pragma once

#include <Base/Types.h>
#include <Core/Result.h>
#include <Crypto/Kdf.h>
#include <Crypto/Secret.h>

// 키 타입별 파생(TryDeriveMacKey·TryDeriveKdfKey·TryDeriveAeadKey 등)이 함께 쓰는 내부 도우미다
// (Docs/Design/Crypto.ko.md 10.2절).
namespace rocks::crypto::detail {

// 타입 파생의 BLAKE2b personal 뒤 8 byte다. crypto_kdf(TryDeriveSecret)와 같은 키·salt(LE64(id) ‖
// 0⁸)를 쓰고 이 자리에 키 타입·알고리즘을 넣는다. crypto_kdf는 이 자리가 0이라 서로 겹치지 않는다.
// 이 byte는 파생 결과를 정하므로 바꾸지 않는다(바꾸면 저장된 키 계층이 모두 달라진다). 새 키 타입은
// 아래 목록에 겹치지 않는 tag를 더한다.
struct KdfTypeTag {
    uint8 bytes[8];
};

inline constexpr KdfTypeTag KdfKeyTypeTag = {{'R', 'k', 'K', 'd', 'f', 'K', 'e', 'y'}};

// algorithm은 MacAlgorithm 열거값이다.
constexpr KdfTypeTag MacKeyTypeTag(uint8 algorithm) noexcept {
    return KdfTypeTag{{'R', 'k', 'M', 'a', 'c', 'K', 0x00, algorithm}};
}

// algorithm은 AeadAlgorithm 열거값이다.
constexpr KdfTypeTag AeadKeyTypeTag(uint8 algorithm) noexcept {
    return KdfTypeTag{{'R', 'k', 'A', 'e', 'a', 'd', 0x00, algorithm}};
}

// 순번 봉인기(AeadSequenceSealer·Opener::TryDerive)의 키와 iv다. algorithm은 AeadAlgorithm
// 열거값이다.
constexpr KdfTypeTag AeadSequenceKeyTypeTag(uint8 algorithm) noexcept {
    return KdfTypeTag{{'R', 'k', 'S', 'e', 'q', 'K', 0x00, algorithm}};
}

constexpr KdfTypeTag AeadSequenceIvTypeTag(uint8 algorithm) noexcept {
    return KdfTypeTag{{'R', 'k', 'S', 'e', 'q', 'I', 0x00, algorithm}};
}

// master·subkeyId·context·tag에서 bytes 길이의 비밀을 뽑는다. bytes는 호출자가 키 타입 길이로
// 맞춘다(16–64). 빈 master는 StaleHandle, 키 저장소가 찼으면 OutOfMemory다.
[[nodiscard]] Result<SecretBytes> TryDeriveTypedSecret(const KdfKey& master, uint64 subkeyId,
                                                       KdfContext context, KdfTypeTag tag,
                                                       usize bytes) noexcept;

} // namespace rocks::crypto::detail
