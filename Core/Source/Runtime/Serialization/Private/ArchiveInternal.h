#pragma once

#include <Base/Types.h>

namespace rocks::archive_detail {

// wire 폭을 먼저 고정한 뒤 byte를 조립한다. host endian/alignment/구조체 padding에 의존하지 않는다.
inline void Store32(uint8* output, uint32 value) noexcept {
    for (uint32 index = 0; index < 4; ++index) {
        output[index] = static_cast<uint8>(value >> (index * 8));
    }
}

inline void Store64(uint8* output, uint64 value) noexcept {
    for (uint32 index = 0; index < 8; ++index) {
        output[index] = static_cast<uint8>(value >> (index * 8));
    }
}

inline uint32 Load32(const uint8* input) noexcept {
    uint32 result = 0;
    for (uint32 index = 0; index < 4; ++index) {
        result |= static_cast<uint32>(input[index]) << (index * 8);
    }
    return result;
}

inline uint64 Load64(const uint8* input) noexcept {
    uint64 result = 0;
    for (uint32 index = 0; index < 8; ++index) {
        result |= static_cast<uint64>(input[index]) << (index * 8);
    }
    return result;
}

} // namespace rocks::archive_detail
