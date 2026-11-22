#pragma once

#include <Core/Result.h>
#include <Core/Text/StringView.h>

// 텍스처 view의 성분 대응(swizzle)이다(AssetSystem A0-G, ShaderSystem 5.2절 G1). 셰이더가 읽는 출력
// R·G·B·A마다 형식의 어느 성분(또는 상수 0·1)을 낼지 정한다. 쿠킹 데이터(TexturePlatformData)가
// 담고 렌더러가 view swizzle로 적용한다. backend 대응: D3D12 Shader4ComponentMapping, Vulkan
// VkComponentMapping, Metal MTLTextureSwizzleChannels, OpenGL ES 채널별 TEXTURE_SWIZZLE_R/G/B/A
// (TEXTURE_SWIZZLE_RGBA는 ES에 없다). D3D11은 view swizzle이 없어 항등이 아니면 Unsupported다.
// 열거값은 에셋 파일에 저장되므로 바꾸지 않고 추가만 한다.
namespace rocks::graphics {

// 출력 성분 하나의 출처다. Red–Alpha는 형식의 성분이다(형식에 없는 성분은 형식 규칙대로 G·B는 0,
// A는 1로 읽힌다). Zero·One은 상수다.
enum class SwizzleSource : uint8 {
    Red = 0,
    Green = 1,
    Blue = 2,
    Alpha = 3,
    Zero = 4,
    One = 5
};

// 마지막 열거값이다(범위 검사용).
inline constexpr SwizzleSource MaxSwizzleSource = SwizzleSource::One;

// 출력 R·G·B·A의 출처다. 기본값은 항등이다.
//
// 사용 예(회색 R8 텍스처를 (R, R, R, 1)로 읽기):
//   ChannelSwizzle gray{SwizzleSource::Red, SwizzleSource::Red, SwizzleSource::Red,
//                       SwizzleSource::One};
struct ChannelSwizzle {
    SwizzleSource red = SwizzleSource::Red;
    SwizzleSource green = SwizzleSource::Green;
    SwizzleSource blue = SwizzleSource::Blue;
    SwizzleSource alpha = SwizzleSource::Alpha;

    // 출력 성분 index(0 = R … 3 = A)의 출처다. 범위 밖 index는 Zero다.
    constexpr SwizzleSource Channel(uint32 index) const noexcept {
        return index == 0   ? red
               : index == 1 ? green
               : index == 2 ? blue
               : index == 3 ? alpha
                            : SwizzleSource::Zero;
    }

    constexpr bool IsIdentity() const noexcept {
        return red == SwizzleSource::Red && green == SwizzleSource::Green &&
               blue == SwizzleSource::Blue && alpha == SwizzleSource::Alpha;
    }

    bool operator==(const ChannelSwizzle&) const noexcept = default;
};

constexpr bool IsValidSwizzleSource(SwizzleSource source) noexcept {
    return static_cast<uint32>(source) <= static_cast<uint32>(MaxSwizzleSource);
}

// 네 출처가 모두 범위 안인지다.
constexpr bool IsValidChannelSwizzle(const ChannelSwizzle& swizzle) noexcept {
    return IsValidSwizzleSource(swizzle.red) && IsValidSwizzleSource(swizzle.green) &&
           IsValidSwizzleSource(swizzle.blue) && IsValidSwizzleSource(swizzle.alpha);
}

// outer를 inner 위에 합성한다: 결과의 출력 c는 outer[c]가 성분이면 inner[그 성분], 상수면 그
// 상수다. backend가 형식을 다른 저장 형식으로 흉내 낼 때(OpenGL ES A8 = R8 + (0, 0, 0, R)) 사용자
// swizzle(outer)을 흉내 대응(inner)과 합칠 때 쓴다.
//
// 사용 예: Compose({Alpha, Alpha, Alpha, One}, {Zero, Zero, Zero, Red}) = {Red, Red, Red, One}
constexpr ChannelSwizzle ComposeSwizzle(const ChannelSwizzle& outer,
                                        const ChannelSwizzle& inner) noexcept {
    auto pick = [&inner](SwizzleSource source) {
        return source == SwizzleSource::Zero || source == SwizzleSource::One
                   ? source
                   : inner.Channel(static_cast<uint32>(source));
    };
    return ChannelSwizzle{pick(outer.red), pick(outer.green), pick(outer.blue), pick(outer.alpha)};
}

// 진단·도구용 이름이다(열거자 이름, 범위 밖은 "Unknown"). Parse는 대소문자를 무시하며 모르는
// 이름은 InvalidArgument다.
ROCKS_GRAPHICSTYPES_API const char* SwizzleSourceName(SwizzleSource source) noexcept;
ROCKS_GRAPHICSTYPES_API Result<SwizzleSource> ParseSwizzleSource(StringView name) noexcept;

} // namespace rocks::graphics
