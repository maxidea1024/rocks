#pragma once

#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Core/Move.h>

// 키 타입(MacKey·KdfKey 등)의 비밀 byte와 생성자를 모듈 내부 구현만 쓰게 하는 접근점이다. 키 타입은
// 이 구조체를 friend로 두고, 공개 API에는 생성 함수와 hazmat 내보내기만
// 남긴다(Docs/Design/Crypto.ko.md 7절 "비밀은 view로만 노출", 10.4절 "평문 내보내기는 hazmat").
namespace rocks::crypto::detail {

struct KeyAccess {
    // 키 byte view다. 수명은 key 객체와 같다.
    template <typename Key>
    static Span<const uint8> Bytes(const Key& key) noexcept {
        return key.key_.View();
    }

    // 파생 결과 같은 SecretBytes를 복사 없이 넘겨 키를 만든다(키 타입의 private 생성자).
    template <typename Key, typename... Args>
    static Key Make(Args&&... args) noexcept {
        return Key(Forward<Args>(args)...);
    }
};

} // namespace rocks::crypto::detail
