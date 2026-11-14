#pragma once

#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Platform/Fatal.h>

// libsodium 어댑터들이 함께 쓰는 내부 도우미다(Hash·Mac·Kdf 등).
namespace rocks::crypto::detail {

// 어댑터가 인자를 먼저 검사한 뒤 부르는 libsodium 함수의 반환값을 확인한다. 0이 아니면 어댑터가
// 막았어야 할 조건에 닿은 결함이므로 멈춘다(Docs/Design/Crypto.ko.md 5.1절 "misuse handler 도달
// 금지"와 같은 원칙).
inline void RequireSodium(int status, const char* message) noexcept {
    if (status != 0) {
        platform::Fatal(message);
    }
}

// 빈 Span의 Data()는 null일 수 있다. libsodium 일부 경로는 길이가 0이어도 포인터를 넘기거나
// memcpy에 쓰므로 유효한 주소를 준다(빈 입력은 읽히지 않는다).
inline const uint8* InputPointer(Span<const uint8> bytes) noexcept {
    static constexpr uint8 Empty = 0;
    return bytes.IsEmpty() ? &Empty : bytes.Data();
}

// 빈 출력에는 호출자의 scratch 1 byte를 준다(쓰이지 않는다).
inline uint8* OutputPointer(Span<uint8> bytes, uint8* scratch) noexcept {
    return bytes.IsEmpty() ? scratch : bytes.Data();
}

// 출력이 입력 조각이나 조각 배열 자체와 한 byte라도 겹치는가(7절 제자리 규칙).
inline bool OutputOverlaps(Span<const Span<const uint8>> pieces, Span<uint8> output) noexcept {
    if (Overlaps(pieces, output)) {
        return true;
    }

    for (Span<const uint8> piece : pieces) {
        if (Overlaps(piece, output)) {
            return true;
        }
    }

    return false;
}

} // namespace rocks::crypto::detail
