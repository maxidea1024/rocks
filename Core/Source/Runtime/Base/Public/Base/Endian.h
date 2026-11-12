#pragma once

#include <Base/Types.h>

// byte 순서(endianness) 변환과 고정 byte 순서 읽기·쓰기다. 파일·네트워크 형식처럼 플랫폼과 무관하게
// byte 순서가 정해진 데이터를 다룰 때 쓴다(구조체를 그대로 memcpy하면 플랫폼마다 결과가 달라진다).
//
// 사용 예:
//   uint8 header[8];
//   StoreLittleEndian<uint32>(header, magic);            // 저장 형식은 little-endian으로 정한다
//   StoreLittleEndian<uint32>(header + 4, version);
//   uint32 read = LoadLittleEndian<uint32>(header + 4);
//   uint16 port = LoadBigEndian<uint16>(packet + 2);      // 네트워크 byte 순서
//
// 지원 타입: uint16·uint32·uint64와 같은 폭의 부호 있는 정수(int16·int32·int64).
namespace rocks {

// 실행 중인 CPU가 little-endian인지다. 지원 대상(x64·ARM64)은 모두 little-endian이다.
inline constexpr bool IsLittleEndian = __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__;

static_assert(IsLittleEndian,
              "Rocks targets are little-endian; add byte-order support before porting");

// byte 순서를 뒤집는다. 예: ByteSwap<uint32>(0x11223344) == 0x44332211.
template <typename T>
constexpr T ByteSwap(T value) noexcept {
    static_assert(__is_integral(T) && (sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8));
    if constexpr (sizeof(T) == 2) {
        return static_cast<T>(__builtin_bswap16(static_cast<uint16>(value)));
    } else if constexpr (sizeof(T) == 4) {
        return static_cast<T>(__builtin_bswap32(static_cast<uint32>(value)));
    } else {
        return static_cast<T>(__builtin_bswap64(static_cast<uint64>(value)));
    }
}

// bytes에서 sizeof(T) byte를 little-endian으로 읽는다. 정렬되지 않은 주소도 된다.
template <typename T>
constexpr T LoadLittleEndian(const uint8* bytes) noexcept {
    static_assert(__is_integral(T) && (sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8));
    uint64 value = 0;
    for (usize index = 0; index < sizeof(T); ++index) {
        value |= static_cast<uint64>(bytes[index]) << (index * 8);
    }

    return static_cast<T>(value);
}

// bytes에서 sizeof(T) byte를 big-endian(네트워크 byte 순서)으로 읽는다.
template <typename T>
constexpr T LoadBigEndian(const uint8* bytes) noexcept {
    static_assert(__is_integral(T) && (sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8));
    uint64 value = 0;
    for (usize index = 0; index < sizeof(T); ++index) {
        value = (value << 8) | bytes[index];
    }

    return static_cast<T>(value);
}

// value를 bytes에 sizeof(T) byte little-endian으로 쓴다.
template <typename T>
constexpr void StoreLittleEndian(uint8* bytes, T value) noexcept {
    static_assert(__is_integral(T) && (sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8));
    const auto bits = static_cast<uint64>(value);
    for (usize index = 0; index < sizeof(T); ++index) {
        bytes[index] = static_cast<uint8>(bits >> (index * 8));
    }
}

// value를 bytes에 sizeof(T) byte big-endian으로 쓴다.
template <typename T>
constexpr void StoreBigEndian(uint8* bytes, T value) noexcept {
    static_assert(__is_integral(T) && (sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8));
    const auto bits = static_cast<uint64>(value);
    for (usize index = 0; index < sizeof(T); ++index) {
        bytes[index] = static_cast<uint8>(bits >> ((sizeof(T) - 1 - index) * 8));
    }
}

} // namespace rocks
