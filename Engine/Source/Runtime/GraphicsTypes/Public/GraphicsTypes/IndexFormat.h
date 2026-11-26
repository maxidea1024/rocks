#pragma once

#include <Core/Result.h>

// index 형식이다(Docs/Design/RenderBase 10.4절, 결정 R20 — 고정 번호표). 메시 쿠킹 데이터가 번호를
// 직렬화하므로 장치 계약(Graphics)이 아니라 GraphicsTypes에 둔다(Docs/Design/MeshAnimationAssets
// 5.5.1절 8).
namespace rocks::graphics {

// index 형식이다. 열거자 값이 고정 번호표다(1–2). 번호는 한 번 정하면 재사용하지 않고 직렬화는
// IndexFormatNumber·IndexFormatFromNumber를 쓴다(enum 순서 값을 쓰지 않는다).
//
// 사용 예:
//   const uint32 bytes = IndexFormatSize(IndexFormat::UInt16);           // 2
//   auto loaded = IndexFormatFromNumber(lod.indexFormat);                // 쿠킹 데이터 읽기
enum class IndexFormat : uint8 {
    UInt16 = 1,
    UInt32 = 2
};

// 형식 수와 가장 큰 번호다(번호는 1부터 빈틈 없이 쓴다).
inline constexpr uint32 IndexFormatCount = 2;

// 번호표 안의 값인지다.
constexpr bool IsValidIndexFormat(IndexFormat format) noexcept {
    return format == IndexFormat::UInt16 || format == IndexFormat::UInt32;
}

// index 하나의 byte 크기다(2·4). 모르는 값은 0이다.
constexpr uint32 IndexFormatSize(IndexFormat format) noexcept {
    switch (format) {
    case IndexFormat::UInt16:
        return 2u;
    case IndexFormat::UInt32:
        return 4u;
    }

    return 0u;
}

// 고정 번호다(바이너리 직렬화).
constexpr uint32 IndexFormatNumber(IndexFormat format) noexcept {
    return static_cast<uint32>(format);
}

// 번호의 형식이다. 실패: 번호표 밖 InvalidArgument.
ROCKS_GRAPHICSTYPES_API Result<IndexFormat> IndexFormatFromNumber(uint32 number) noexcept;

} // namespace rocks::graphics
