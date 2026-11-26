#pragma once

#include <ShaderCore/ShaderParameterTypes.h>

// 셰이더 파라미터 구조체 자원 멤버의 HLSL 자원 서술자다(Docs/Design/ShaderSystem 16.2절 SSD59).
//
// 서술자는 값이 없는 태그 타입이다. 자원 멤버의 C++ 타입은 "값을 담는 틀"(handle을 가진 모듈 —
// RenderBase DeviceTextureParameter·RenderGraph GraphTextureParameter 등)에 이 서술자를 템플릿
// 인자로 준 것이고, MetaCompiler 셰이더 파라미터 모드가 서술자에서 HLSL 선언(모양·원소·짝 샘플러·
// image 형식)을 만든다. 서술자만으로는 멤버가 되지 않는다(틀이 없으면 RMC6007).
//
// 원소(Element)는 float·int32·uint32과 ShaderCore 정렬 벡터(ShaderFloat2–ShaderUint4)다. 구조화
// 버퍼 원소는 4 byte 스칼라·2·4성분·ShaderFloat4x4만이다(3성분은 C++ 16 byte와 HLSL stride 12가
// 다르다). 짝(Sampler)은 원소가 float인 SRV 텍스처에만 낸다: ShaderFilterSampler는
// SamplerState <이름>Sampler, ShaderComparisonSampler는 SamplerComparisonState
// <이름>Sampler다(7.2절 9항). 정수 원소 텍스처와 MS 텍스처는 짝이 없다(Load 전용). UAV 텍스처·typed
// 버퍼의 형식(Format)은 ShaderImage* 태그이고 ROCKS_IMAGE_FORMAT 철자가 된다(7.2절 1항). 형식은
// 원소의 기본 타입·성분 수와 맞아야 한다.
//
// 사용 예(RenderGraph 틀과 함께):
//   ROCKS_SHADER_PARAMETERS()
//   struct alignas(16) BlurParameters {
//       shader::ShaderFloat4 texelSize;                                              // 상수
//       render::GraphTextureParameter<shader::ShaderTexture2D<>> source;             //
//       Texture2D<float4> source; SamplerState sourceSampler; render::GraphTextureUavParameter<
//           shader::ShaderRWTexture2D<shader::ShaderFloat4, shader::ShaderImageRgba16f>> output;
//   };
namespace rocks::shader {

// 짝 샘플러 종류 태그다.
struct ShaderFilterSampler {};
struct ShaderComparisonSampler {};

// UAV image 형식 태그다(DXC [[vk::image_format]] 철자).
struct ShaderImageRgba8 {};
struct ShaderImageRgba8Snorm {};
struct ShaderImageRgba16f {};
struct ShaderImageRgba32f {};
struct ShaderImageR11g11b10f {};
struct ShaderImageR16f {};
struct ShaderImageRg16f {};
struct ShaderImageR32f {};
struct ShaderImageRg32f {};
struct ShaderImageR32ui {};
struct ShaderImageR32i {};
struct ShaderImageRgba32ui {};

// SRV 텍스처 서술자다(HLSL Texture1D … TextureCubeArray).
template <class Element = ShaderFloat4, class Sampler = ShaderFilterSampler>
struct ShaderTexture1D {};

template <class Element = ShaderFloat4, class Sampler = ShaderFilterSampler>
struct ShaderTexture1DArray {};

template <class Element = ShaderFloat4, class Sampler = ShaderFilterSampler>
struct ShaderTexture2D {};

template <class Element = ShaderFloat4, class Sampler = ShaderFilterSampler>
struct ShaderTexture2DArray {};

template <class Element = ShaderFloat4, class Sampler = ShaderFilterSampler>
struct ShaderTexture3D {};

template <class Element = ShaderFloat4, class Sampler = ShaderFilterSampler>
struct ShaderTextureCube {};

template <class Element = ShaderFloat4, class Sampler = ShaderFilterSampler>
struct ShaderTextureCubeArray {};

// MS 텍스처 서술자다(HLSL Texture2DMS·Texture2DMSArray — 짝 없음).
template <class Element = ShaderFloat4>
struct ShaderTexture2DMS {};

template <class Element = ShaderFloat4>
struct ShaderTexture2DMSArray {};

// 버퍼 SRV 서술자다(HLSL Buffer<T>·StructuredBuffer<T>·ByteAddressBuffer).
template <class Element>
struct ShaderBuffer {};

template <class Element>
struct ShaderStructuredBuffer {};

struct ShaderByteAddressBuffer {};

// UAV 서술자다(HLSL RWTexture2D·RWTexture2DArray·RWTexture3D·RWBuffer — 형식 지정,
// RWStructuredBuffer·RWByteAddressBuffer).
template <class Element, class Format>
struct ShaderRWTexture2D {};

template <class Element, class Format>
struct ShaderRWTexture2DArray {};

template <class Element, class Format>
struct ShaderRWTexture3D {};

template <class Element, class Format>
struct ShaderRWBuffer {};

template <class Element>
struct ShaderRWStructuredBuffer {};

struct ShaderRWByteAddressBuffer {};

} // namespace rocks::shader
