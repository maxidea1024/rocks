#pragma once

#include <Core/Result.h>
#include <Core/Text/StringView.h>

// 텍스처 샘플러 설정이다. 그래픽 장치 계약(Graphics)과 에셋(AssetRuntime)·도구가 함께 쓰는 값
// 타입이며 장치 객체를 담지 않는다. 열거값은 에셋 파일에 저장되므로 바꾸지 않고 추가만 한다.
namespace rocks::graphics {

// 텍스처 샘플링 필터다(UE TextureFilter·RHI SamplerFilter 대응).
enum class Filter : uint32 {
    // 확대·축소·mip 선택 모두 최근접(픽셀 아트)
    Nearest = 0,
    // 확대·축소는 선형, mip 선택은 최근접
    Bilinear = 1,
    // 확대·축소·mip 사이 모두 선형
    Trilinear = 2,
    // 이방성(SamplerDesc::maxAnisotropy 배율) + 선형 mip
    Anisotropic = 3
};

// 텍스처 좌표가 [0, 1]을 벗어날 때(그리고 가장자리 texel 바깥 절반을 선형 필터가 읽을 때)의
// 처리다(UE TextureAddress·RHI SamplerAddressMode 대응).
enum class AddressMode : uint32 {
    // 반대편 가장자리로 이어진다(타일링)
    Wrap = 0,
    // 가장자리 texel을 늘린다
    Clamp = 1,
    // 거울처럼 뒤집어 이어진다
    Mirror = 2,
    // SamplerDesc::borderColor로 채운다. 투명 테두리와 함께 쓰면 가장자리 색이 번지지 않는다.
    Border = 3
};

// Border 주소 모드의 색이다. Vulkan·D3D12 static sampler와 같은 고정 세 가지만 둔다(이식성).
enum class BorderColor : uint32 {
    TransparentBlack = 0,
    OpaqueBlack = 1,
    OpaqueWhite = 2
};

// 텍스처를 읽는 방법이다. 텍스처마다 하나를 정한다(TextureDesc::sampler).
struct SamplerDesc {
    Filter filter = Filter::Trilinear;
    AddressMode addressU = AddressMode::Wrap;
    AddressMode addressV = AddressMode::Wrap;
    // Anisotropic의 최대 배율(1–16)이다. 장치 한계보다 크면 장치 한계로 줄인다. 다른 필터는
    // 무시한다.
    uint32 maxAnisotropy = 1;
    // Border 주소 모드에서만 쓴다.
    BorderColor borderColor = BorderColor::TransparentBlack;

    bool operator==(const SamplerDesc&) const noexcept = default;
};

// 열거값 범위와 maxAnisotropy(1–16)를 검사한다.
bool IsValidSamplerDesc(const SamplerDesc& desc) noexcept;

// border 색의 선형 RGBA 값이다(out[4]).
void BorderColorValue(BorderColor color, float (&out)[4]) noexcept;

// 진단·도구용 이름이다("Nearest"·"Bilinear"·"Trilinear"·"Anisotropic", "Wrap"·"Clamp"·"Mirror"·
// "Border", "TransparentBlack"·"OpaqueBlack"·"OpaqueWhite"). Parse는 대소문자를 무시하며 모르는
// 이름은 InvalidArgument다.
const char* FilterName(Filter filter) noexcept;
const char* AddressModeName(AddressMode mode) noexcept;
const char* BorderColorName(BorderColor color) noexcept;
Result<Filter> ParseFilter(StringView name) noexcept;
Result<AddressMode> ParseAddressMode(StringView name) noexcept;
Result<BorderColor> ParseBorderColor(StringView name) noexcept;

} // namespace rocks::graphics
