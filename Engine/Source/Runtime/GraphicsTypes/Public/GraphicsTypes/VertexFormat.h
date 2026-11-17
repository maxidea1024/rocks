#pragma once

#include <Core/Result.h>
#include <Core/Text/StringView.h>

// 정점 원소 형식이다(Docs/Design/RenderBase.ko.md 10.4절 GX3a, UE 5.8 EVertexElementType 20종
// 대응). 메시 에셋이 직렬화하므로 장치 계약(Graphics)이 아니라 GraphicsTypes에 둔다.
namespace rocks::graphics {

// 정점 원소 형식이다. 열거자 값이 고정 번호표다(1–20). 번호는 한 번 정하면 재사용하지 않고
// 직렬화(바이너리)는 VertexFormatNumber, 텍스트·JSON은 VertexFormatName을 쓴다.
//
// | 번호 | 형식 | byte | 셰이더 타입 | D3D12 | Vulkan | Metal | OpenGL |
// |---|---|---|---|---|---|---|---|
// | 1 | Float1 | 4 | float | R32_FLOAT | R32_SFLOAT | Float | GL_FLOAT×1 |
// | 2 | Float2 | 8 | float2 | R32G32_FLOAT | R32G32_SFLOAT | Float2 | GL_FLOAT×2 |
// | 3 | Float3 | 12 | float3 | R32G32B32_FLOAT | R32G32B32_SFLOAT | Float3 | GL_FLOAT×3 |
// | 4 | Float4 | 16 | float4 | R32G32B32A32_FLOAT | R32G32B32A32_SFLOAT | Float4 | GL_FLOAT×4 |
// | 5 | Byte4Snorm | 4 | float4 | R8G8B8A8_SNORM | R8G8B8A8_SNORM | Char4Normalized | GL_BYTE×4
// 정규화 | | 6 | UByte4 | 4 | uint4 | R8G8B8A8_UINT | R8G8B8A8_UINT | UChar4 | GL_UNSIGNED_BYTE×4
// 정수 | | 7 | UByte4Unorm | 4 | float4 | R8G8B8A8_UNORM | R8G8B8A8_UNORM | UChar4Normalized |
// GL_UNSIGNED_BYTE×4 정규화 | | 8 | Bgra8Unorm | 4 | float4 | B8G8R8A8_UNORM | B8G8R8A8_UNORM |
// UChar4Normalized_BGRA | 미지원 | | 9 | Short2 | 4 | int2 | R16G16_SINT | R16G16_SINT | Short2 |
// GL_SHORT×2 정수 | | 10 | Short4 | 8 | int4 | R16G16B16A16_SINT | R16G16B16A16_SINT | Short4 |
// GL_SHORT×4 정수 | | 11 | Short2Snorm | 4 | float2 | R16G16_SNORM | R16G16_SNORM |
// Short2Normalized | GL_SHORT×2 정규화 | | 12 | Short4Snorm | 8 | float4 | R16G16B16A16_SNORM |
// R16G16B16A16_SNORM | Short4Normalized | GL_SHORT×4 정규화 | | 13 | Half2 | 4 | float2 |
// R16G16_FLOAT | R16G16_SFLOAT | Half2 | GL_HALF_FLOAT×2 | | 14 | Half4 | 8 | float4 |
// R16G16B16A16_FLOAT | R16G16B16A16_SFLOAT | Half4 | GL_HALF_FLOAT×4 | | 15 | UShort2 | 4 | uint2 |
// R16G16_UINT | R16G16_UINT | UShort2 | GL_UNSIGNED_SHORT×2 정수 | | 16 | UShort4 | 8 | uint4 |
// R16G16B16A16_UINT | R16G16B16A16_UINT | UShort4 | GL_UNSIGNED_SHORT×4 정수 | | 17 | UShort2Unorm
// | 4 | float2 | R16G16_UNORM | R16G16_UNORM | UShort2Normalized | GL_UNSIGNED_SHORT×2 정규화 | |
// 18 | UShort4Unorm | 8 | float4 | R16G16B16A16_UNORM | R16G16B16A16_UNORM | UShort4Normalized |
// GL_UNSIGNED_SHORT×4 정규화 | | 19 | Rgb10A2Unorm | 4 | float4 | R10G10B10A2_UNORM |
// A2B10G10R10_UNORM_PACK32 | UInt1010102Normalized | GL_UNSIGNED_INT_2_10_10_10_REV×4 정규화 | | 20
// | UInt1 | 4 | uint | R32_UINT | R32_UINT | UInt | GL_UNSIGNED_INT×1 정수 |
//
// - Vulkan·Metal 열은 backend가 아직 없어 대응표로만 둔다(VulkanGraphics·MetalGraphics가 생길 때
// 쓴다).
// - 정규화 규칙은 그래픽 API 표준이다: snorm은 max(c / (2^(b−1) − 1), −1), unorm은 c / (2^b − 1).
//   32-bit 정수 형식은 정규화하지 않는다. CPU 쪽 변환은 Core/Math/VertexPacking.h다.
// - Bgra8Unorm은 OpenGL backend(ES 3.2 공통 API)에 BGRA 정점 배열이 없어 미지원이다. 그 형식이 든
//   파이프라인 생성은 Unsupported다. 메시 쿠커는 GL 형식 프로필에서 색을 UByte4Unorm으로 쓴다.
//
// 사용 예:
//   const uint32 bytes = VertexFormatSize(VertexFormat::Half4);              // 8
//   auto parsed = ParseVertexFormatName("Byte4Snorm");                       // 텍스트 직렬화
//   auto loaded = VertexFormatFromNumber(archive.ReadU32());                 // 바이너리 직렬화
enum class VertexFormat : uint8 {
    Float1 = 1,
    Float2 = 2,
    Float3 = 3,
    Float4 = 4,
    Byte4Snorm = 5,
    UByte4 = 6,
    UByte4Unorm = 7,
    Bgra8Unorm = 8,
    Short2 = 9,
    Short4 = 10,
    Short2Snorm = 11,
    Short4Snorm = 12,
    Half2 = 13,
    Half4 = 14,
    UShort2 = 15,
    UShort4 = 16,
    UShort2Unorm = 17,
    UShort4Unorm = 18,
    Rgb10A2Unorm = 19,
    UInt1 = 20
};

// 형식 수와 가장 큰 번호다(번호는 1부터 빈틈 없이 쓴다).
inline constexpr uint32 VertexFormatCount = 20;

// 셰이더가 그 형식을 받는 VS 입력 변수의 기본 타입 계열이다. 정규화·float 형식은 Float, 정규화 없는
// 정수 형식은 Int·Uint다.
enum class VertexShaderType : uint8 {
    Float,
    Int,
    Uint
};

// 형식의 byte 크기다. 모르는 값은 0이다.
uint32 VertexFormatSize(VertexFormat format) noexcept;

// 형식의 성분 수(1–4)다. 모르는 값은 0이다.
uint32 VertexFormatComponents(VertexFormat format) noexcept;

// 형식의 셰이더 타입 계열이다. 모르는 값은 Float다(IsValidVertexFormat으로 먼저 확인한다).
VertexShaderType VertexFormatShaderType(VertexFormat format) noexcept;

// 정규화 정수 형식(snorm·unorm·10:10:10:2)인지다.
bool IsNormalizedVertexFormat(VertexFormat format) noexcept;

// 번호표 안의 값인지다.
bool IsValidVertexFormat(VertexFormat format) noexcept;

// 고정 번호다(바이너리 직렬화).
constexpr uint32 VertexFormatNumber(VertexFormat format) noexcept {
    return static_cast<uint32>(format);
}

// 번호의 형식이다. 실패: 번호표 밖 InvalidArgument.
Result<VertexFormat> VertexFormatFromNumber(uint32 number) noexcept;

// "Byte4Snorm" 같은 안정 이름이다(텍스트·JSON 직렬화). 모르는 값은 "Unknown"이다.
const char* VertexFormatName(VertexFormat format) noexcept;

// 안정 이름의 형식이다(대소문자 구분). 실패: 모르는 이름 NotFound.
Result<VertexFormat> ParseVertexFormatName(StringView name) noexcept;

// 셰이더 타입 계열 이름("float"·"int"·"uint")이다(진단 문자열).
const char* VertexShaderTypeName(VertexShaderType type) noexcept;

} // namespace rocks::graphics
