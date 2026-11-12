#pragma once

#include <Core/Text/StringView.h>

// 픽셀 형식과 mip 크기 계산이다. 그래픽 장치 계약(Graphics)과 에셋(AssetRuntime)·도구가 함께 쓰는
// 값 타입이며 장치 객체를 담지 않는다.
namespace rocks::graphics {

// 텍스처·렌더 타깃 픽셀 형식이다. 모두 픽셀당 4바이트, 채널 순서는 이름 그대로다(RGBA8은 R가 첫
// 바이트). Srgb 형식은 저장값이 sRGB 인코딩이며 샘플링은 선형으로 디코딩하고 렌더 타깃 쓰기는
// 인코딩한다. 값은 에셋 파일(텍스처)에 저장되므로 바꾸지 않고 추가만 한다.
enum class Format : uint32 {
    Unknown = 0,
    RGBA8Unorm = 1,
    RGBA8UnormSrgb = 2
};

// 픽셀당 바이트 수다. Unknown은 0이다.
uint32 BytesPerPixel(Format format) noexcept;

// "RGBA8UnormSrgb" 같은 이름이다. 범위 밖이면 "Unknown"이다.
const char* FormatName(Format format) noexcept;

// mip level의 크기다(max(1, size >> level)).
constexpr uint32 MipExtent(uint32 size, uint32 level) noexcept {
    const uint32 extent = level < 32 ? size >> level : 0u;
    return extent == 0 ? 1u : extent;
}

// width × height의 전체 mip 수(1×1까지)다.
constexpr uint32 FullMipCount(uint32 width, uint32 height) noexcept {
    uint32 largest = width > height ? width : height;
    uint32 count = 1;
    while (largest > 1) {
        largest >>= 1;
        ++count;
    }
    return count;
}

// 앞 mipLevels개 mip을 빈틈 없이 이은 픽셀 byte 수다(텍스처 초기 데이터 배치). 크기·mip 수·형식이
// 잘못됐으면 0이다.
uint64 MipChainBytes(uint32 width, uint32 height, uint32 mipLevels, Format format) noexcept;

} // namespace rocks::graphics
