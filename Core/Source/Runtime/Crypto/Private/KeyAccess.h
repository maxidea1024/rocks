#pragma once

#include <Base/Types.h>
#include <Core/Containers/Span.h>

// 키 타입(MacKey 등)의 비밀 byte를 모듈 내부 구현만 읽게 하는 접근점이다. 키 타입은 이 구조체를
// friend로 두고, 공개 API에는 명시적 내보내기(TryExport)만 남긴다(Docs/Design/Crypto.ko.md 7절
// "비밀은 view로만 노출").
namespace rocks::crypto::detail {

struct KeyAccess {
    // 키 byte view다. 수명은 key 객체와 같다.
    template <typename Key>
    static Span<const uint8> Bytes(const Key& key) noexcept {
        return key.key_.View();
    }
};

} // namespace rocks::crypto::detail
