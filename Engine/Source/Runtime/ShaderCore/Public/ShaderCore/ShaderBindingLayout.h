#pragma once

#include <Base/Types.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <ShaderCore/ShaderReadStatus.h>

// 셰이더 바인딩 layout v3(셰이더 Record 값 1 BindingLayout)이다(Docs/Design/ShaderSystem 7.2절 3항,
// 12.2.1절 SSD26, UE FShaderParameterMap·FVulkanShaderHeader 대응).
//
// 워커가 형식별 리플렉션을 이 공통 모양으로 바꿔 값 1로 내고(SH2b·SH2c), 장치가 파이프라인 layout을
// 만들 때 읽는다(SH2e). 단위는 (종류, 단계 안 순번)이다.
//
// byte 형식(모두 little-endian, 이 순서):
//   u32 형식 버전(= ShaderBindingLayoutVersion), u32 단계(ShaderStageBits 한 비트),
//   u32 루트 상수 byte 수(0 = 없음), u32 정점 입력 attribute 마스크(ATTRIBUTEn — VS만),
//   u32 int attribute 마스크·u32 uint attribute 마스크(attribute 마스크의 부분, 서로 겹치지 않음 —
//   나머지는 float, v3),
//   u32 render target 마스크(SV_Target0–7 — PS만), u32 단계 사이 마스크(TEXCOORD0–15 — VS 출력·PS
//   입력, v2), u32 자원 수, 자원 표. 자원 하나: u8 종류, u16 순번, u8 이름 byte 수, 이름 byte, u32
//   상수 버퍼 byte 수(상수 버퍼만, 아니면 0), u8 샘플러 출처(SRV 텍스처만, 아니면 None), u8 공유
//   샘플러 표시(샘플러만, 0·1), u8 모양(ShaderResourceShape, v3), u8 원소 타입(ShaderComponentType
//   — typed 자원만, v3), u32 구조화 원소 byte(구조화 버퍼만, v3), u16 멤버 수(상수 버퍼만),
//   멤버마다 (u8 이름 byte 수, 이름 byte, u32 오프셋, u32 byte 수).
// 자원 표는 (종류, 순번) 오름차순이고 같은 (종류, 순번)은 하나다. 멤버는 오프셋 오름차순이고 겹치지
// 않으며 상수 버퍼 안에 있다. 이름은 64 byte 이하 식별자다.
//
// 사용 예(읽기 — 이름은 bytes를 가리키므로 bytes가 layout보다 오래 살아야 한다):
//   ShaderBindingLayout layout(allocator);
//   if (const ShaderReadStatus read = DecodeShaderBindingLayout(bytes, layout); !read) { ... }
//   for (const ShaderBindingResource& resource : layout.resources) { ... }
namespace rocks::shader {

inline constexpr uint32 ShaderBindingLayoutVersion = 3;
// 단계 하나의 최대 자원 수다(형식 상한 합보다 넉넉하다).
inline constexpr usize MaxShaderBindingResources = 1024;
// 상수 버퍼 하나의 최대 멤버 수다(ShaderParameterLayout과 같다).
inline constexpr usize MaxShaderBindingMembers = 256;
// 자원·멤버 이름의 최대 byte 수(NUL 제외)다.
inline constexpr usize MaxShaderBindingNameBytes = 64;
// render target 수 상한(SV_Target0–7)이다.
inline constexpr uint32 MaxShaderRenderTargets = 8;
// 정점 입력 attribute 수 상한(ATTRIBUTE0–15)이다.
inline constexpr uint32 MaxShaderVertexAttributes = 16;
// 단계 사이 변수 수 상한(TEXCOORD0–15)이다.
inline constexpr uint32 MaxShaderInterStageVariables = 16;

// 자원 종류다(7.2절 3항 — Vulkan descriptor 종류 기준). 값은 byte 형식에 들어가므로 바꾸지 않는다.
// 정렬 순서도 이 값이다.
enum class ShaderResourceKind : uint8 {
    ConstantBuffer = 0,
    // SRV 텍스처(typed buffer SRV 포함)다.
    SampledTexture = 1,
    // 구조화·raw 버퍼 SRV다.
    StorageBufferRead = 2,
    // 구조화·raw 버퍼 UAV다.
    StorageBufferReadWrite = 3,
    // storage image(UAV 텍스처·typed buffer UAV)다.
    StorageImage = 4,
    Sampler = 5
};

inline constexpr uint32 ShaderResourceKindCount = 6;

// SRV 텍스처의 샘플러 출처다(7.2절 9항). None은 Load·GetDimensions만 쓰는 텍스처다.
enum class ShaderSamplerSource : uint8 {
    None = 0,
    Own = 1,
    SharedWrap = 2,
    SharedClamp = 3
};

inline constexpr uint32 ShaderSamplerSourceCount = 4;

// 자원 모양이다(v3, Docs/Design/ShaderSystem 16.1절 SH2e2 메모). 장치가 descriptor 종류(Vulkan
// texel buffer·image)를 고르고 묶는 view의 종류를 검사한다(UE FVulkanShaderHeader의 binding 종류와
// 같은 정보). 상수 버퍼·샘플러는 None이다. 값은 byte 형식에 들어가므로 바꾸지 않는다.
enum class ShaderResourceShape : uint8 {
    None = 0,
    Texture1D = 1,
    Texture1DArray = 2,
    Texture2D = 3,
    Texture2DArray = 4,
    Texture2DMultisample = 5,
    Texture2DMultisampleArray = 6,
    Texture3D = 7,
    TextureCube = 8,
    TextureCubeArray = 9,
    // typed buffer(Buffer<T>·RWBuffer<T>)
    TypedBuffer = 10,
    // StructuredBuffer<T>·RWStructuredBuffer<T>
    StructuredBuffer = 11,
    // ByteAddressBuffer·RWByteAddressBuffer
    RawBuffer = 12
};

inline constexpr uint32 ShaderResourceShapeCount = 13;

// typed 자원(텍스처·typed buffer·storage image)과 정점 입력의 원소 기본 타입이다(v3). unorm·snorm은
// Float다.
enum class ShaderComponentType : uint8 {
    None = 0,
    Float = 1,
    Int = 2,
    Uint = 3
};

inline constexpr uint32 ShaderComponentTypeCount = 4;

// 상수 버퍼 멤버 하나다(5항 오프셋 검사의 입력).
struct ShaderBindingMember {
    StringView name;
    uint32 offset = 0;
    uint32 bytes = 0;
};

// 자원 하나다. 멤버는 layout.members[firstMember, firstMember + memberCount)다.
struct ShaderBindingResource {
    ShaderResourceKind kind = ShaderResourceKind::ConstantBuffer;
    uint16 slot = 0;
    StringView name;
    uint32 constantBufferBytes = 0;
    ShaderSamplerSource samplerSource = ShaderSamplerSource::None;
    bool sharedSampler = false;
    // 모양·원소 타입·구조화 원소 byte다(v3). 종류마다 허용 값: SRV 텍스처는 텍스처
    // 모양·TypedBuffer, storage image는 1D·1DArray·2D·2DArray·3D·TypedBuffer, 구조화 버퍼는
    // StructuredBuffer(원소 byte > 0, 4의 배수)·RawBuffer(0)이고 원소 타입은 텍스처·typed 모양만
    // None이 아니다.
    ShaderResourceShape shape = ShaderResourceShape::None;
    ShaderComponentType component = ShaderComponentType::None;
    uint32 structureStride = 0;
    uint32 firstMember = 0;
    uint32 memberCount = 0;
};

// 바인딩 layout 하나다(값 1의 메모리 모양). 이름은 원본 byte(읽기) 또는 호출자 문자열(쓰기)을
// 가리킨다.
struct ROCKS_SHADERCORE_API ShaderBindingLayout {
    explicit ShaderBindingLayout(Allocator& allocator) noexcept
        : resources(allocator), members(allocator) {
    }

    uint32 stage = 0;
    uint32 rootConstantBytes = 0;
    uint32 attributeMask = 0;
    // attribute 가운데 int·uint 입력이다(v3, 나머지는 float).
    uint32 attributeIntMask = 0;
    uint32 attributeUintMask = 0;
    uint32 renderTargetMask = 0;
    // 단계 사이 변수(TEXCOORDn) 마스크다(VS 출력·PS 입력, 16.1절 SSD37).
    uint32 interStageMask = 0;
    Array<ShaderBindingResource> resources;
    Array<ShaderBindingMember> members;

    // 자원의 멤버다(범위가 잘못되었으면 빈 Span).
    Span<const ShaderBindingMember> MembersOf(const ShaderBindingResource& resource) const noexcept;

    // (kind, slot)의 자원이다(이진 탐색). 없으면 null이다.
    const ShaderBindingResource* Find(ShaderResourceKind kind, uint16 slot) const noexcept;
};

// layout을 위 byte 형식으로 out 끝에 붙인다. 쓰기 전에 읽기와 같은 규칙으로 검사한다. 오류:
// 규칙 위반 InvalidArgument(out은 바뀌지 않는다), 할당 실패.
ROCKS_SHADERCORE_API Result<void> EncodeShaderBindingLayout(const ShaderBindingLayout& layout,
                                                            Array<uint8>& out) noexcept;

// bytes(값 1 전체)를 읽어 out을 채운다(out의 기존 내용은 지운다). 버전이 다르면 UnsupportedVersion,
// 규칙 위반·남는 byte·할당 실패는 CorruptPackage다(할당 실패는 reason이 따로다).
ROCKS_SHADERCORE_API ShaderReadStatus DecodeShaderBindingLayout(Span<const uint8> bytes,
                                                                ShaderBindingLayout& out) noexcept;

// VS·PS 쌍이 이어지는가(7.2절 7항, 16.1절 SSD37). PS 입력 TEXCOORD 집합이 VS 출력 집합의
// 앞부분(PS가 읽는 가장 큰 n까지 VS 출력과 같다)이어야 한다 — D3D register가 선언 순서로 정해지기
// 때문이다. 단계가 VS·PS가 아니거나 맞지 않으면 false와 reason(영어 정적 문구)이다.
ROCKS_SHADERCORE_API bool CheckShaderStageLink(const ShaderBindingLayout& vertex,
                                               const ShaderBindingLayout& pixel,
                                               const char** reason = nullptr) noexcept;

// 종류 이름("ConstantBuffer" 등, 정적 수명)이다. 범위 밖이면 "unknown".
ROCKS_SHADERCORE_API const char* ShaderResourceKindName(ShaderResourceKind kind) noexcept;

// "Texture2D" 같은 이름이다. 범위 밖이면 "unknown"이다.
ROCKS_SHADERCORE_API const char* ShaderResourceShapeName(ShaderResourceShape shape) noexcept;

// attribute location의 원소 타입이다(마스크에 없으면 None).
ROCKS_SHADERCORE_API ShaderComponentType ShaderAttributeComponent(const ShaderBindingLayout& layout,
                                                                  uint32 location) noexcept;

} // namespace rocks::shader
