#pragma once

#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <GraphicsTypes/CompareFunction.h>

// 텍스처 샘플러 설정이다. 그래픽 장치 계약(Graphics)과 에셋(AssetRuntime)·도구가 함께 쓰는 값
// 타입이며 장치 객체를 담지 않는다. 열거값은 에셋 파일에 저장되므로 정한 뒤에는 바꾸지 않고 추가만
// 한다(A0-G에서 UE ESamplerFilter 순서로 한 번 정리했다).
namespace rocks::graphics {

// 텍스처 샘플링 필터다(UE ESamplerFilter와 같은 다섯 가지·같은 순서, RHI/Public/RHIDefinitions.h).
enum class Filter : uint32 {
    // 확대·축소·mip 선택 모두 최근접(픽셀 아트)
    Point = 0,
    // 확대·축소는 선형, mip 선택은 최근접
    Bilinear = 1,
    // 확대·축소·mip 사이 모두 선형
    Trilinear = 2,
    // 이방성(SamplerDesc::maxAnisotropy 배율) + 최근접 mip. 하드웨어 대응이 없는 backend(D3D11,
    // AnisoFilterWithPointMipSupported가 없는 D3D12)는 UE와 같이 AnisotropicLinear로 만든다
    // (Graphics DeviceFeatures::anisotropicPointFilter가 거짓).
    AnisotropicPoint = 3,
    // 이방성(SamplerDesc::maxAnisotropy 배율) + 선형 mip
    AnisotropicLinear = 4
};

// 마지막 열거값이다(범위 검사용).
inline constexpr Filter MaxFilter = Filter::AnisotropicLinear;

// 이방성 필터인지다.
constexpr bool IsAnisotropicFilter(Filter filter) noexcept {
    return filter == Filter::AnisotropicPoint || filter == Filter::AnisotropicLinear;
}

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

// 텍스처를 읽는 방법이다. TextureDesc::sampler는 그 텍스처의 기본 샘플러이며, 바인딩 때 다른
// 샘플러를 고르는 것은 파라미터 바인딩(RenderBase GX6, 결정 R12)이 한다.
//
// 비교 샘플러(comparison = true, 그림자): 셰이더의 SamplerComparisonState(HLSL)·sampler*Shadow
// (GLSL ES)가 깊이 형식 텍스처를 표본할 때 "비교 값 compare 텍스처 깊이"의 결과(0 또는 1, 필터가
// 선형이면 이웃 결과의 가중 평균)를 돌려준다(RenderBase SX13). compare는 comparison이 참일 때만
// 쓴다. UE는 비교를 SCF_Never(끔)·SCF_Less 둘만 두지만 Rocks는 깊이 시험과 같은 CompareFunction
// 전체를 받는다(reverse-Z 그림자는 GreaterEqual 쪽을 쓴다).
//
// 사용 예(그림자 맵):
//   SamplerDesc shadow;
//   shadow.filter = Filter::Bilinear;
//   shadow.addressU = shadow.addressV = AddressMode::Clamp;
//   shadow.comparison = true;
//   shadow.compare = CompareFunction::LessEqual;
struct SamplerDesc {
    Filter filter = Filter::Trilinear;
    AddressMode addressU = AddressMode::Wrap;
    AddressMode addressV = AddressMode::Wrap;
    // 3D 텍스처의 깊이 축이다(UE AddressW). 2D·Cube·배열은 무시한다(cube는 면 경계를 이어 읽는다).
    AddressMode addressW = AddressMode::Wrap;
    // 이방성 필터의 최대 배율(1–16)이다. 장치 한계보다 크면 장치 한계로 줄인다. 다른 필터는
    // 무시한다.
    uint32 maxAnisotropy = 1;
    // Border 주소 모드에서만 쓴다.
    BorderColor borderColor = BorderColor::TransparentBlack;
    // 깊이 비교 샘플러다(위 설명).
    bool comparison = false;
    CompareFunction compare = CompareFunction::LessEqual;

    bool operator==(const SamplerDesc&) const noexcept = default;
};

// 열거값 범위와 maxAnisotropy(1–16)를 검사한다.
bool IsValidSamplerDesc(const SamplerDesc& desc) noexcept;

// border 색의 선형 RGBA 값이다(out[4]).
void BorderColorValue(BorderColor color, float (&out)[4]) noexcept;

// 진단·도구용 이름이다(열거자 이름: "Point"·"Bilinear"·"Trilinear"·"AnisotropicPoint"·
// "AnisotropicLinear", "Wrap"·"Clamp"·"Mirror"·"Border", "TransparentBlack"·"OpaqueBlack"·
// "OpaqueWhite"). Parse는 대소문자를 무시하며 모르는 이름은 InvalidArgument다.
const char* FilterName(Filter filter) noexcept;
const char* AddressModeName(AddressMode mode) noexcept;
const char* BorderColorName(BorderColor color) noexcept;
Result<Filter> ParseFilter(StringView name) noexcept;
Result<AddressMode> ParseAddressMode(StringView name) noexcept;
Result<BorderColor> ParseBorderColor(StringView name) noexcept;

} // namespace rocks::graphics
