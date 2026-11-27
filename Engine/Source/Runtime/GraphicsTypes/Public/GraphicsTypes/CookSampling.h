#pragma once

#include <Core/Containers/Span.h>
#include <GraphicsTypes/GraphicsBackend.h>
#include <GraphicsTypes/GraphicsPlatform.h>
#include <GraphicsTypes/PixelFormat.h>

// 쿠킹 보장 샘플링 표다(Docs/Design/PixelFormat 7.1절). 엔진이 (backend, 플랫폼)에서 받아 주는
// 모든 장치가 형식을 2D 텍스처로 만들고 샘플(선형 필터 칸이면 선형 필터까지)하는지를 장치 없이
// 답한다. 텍스처 쿠커가 프로필 graphicsBackends마다 고른 형식을 검사하고(AssetSystem 12.17절
// TD12) 보장 비압축 대체를 고를 때 쓰고, backend는 장치 생성 때 같은 표를 장치 능력과 대조한다
// (FindMissingCookSampling — 보장 칸이 없는 장치는 Unsupported로 받지 않는다).
//
// 보장 근거는 두 등급이다(CookSamplingBasis): 엔진이 장치 생성 때 요구하는 최소 사양 수준의
// 보장(D3D FL 11_0, Vulkan 1.1 필수 형식 표, OpenGL ES 3.2 core, Metal Apple7)과, 사양 밖이지만
// 엔진이 장치에 요구하는 것(데스크톱 Vulkan BC 등)이다.
//
// 사용 예(쿠커 — Windows 프로필):
//   const Backend backends[] = {Backend::D3D12, Backend::D3D11, Backend::Vulkan};
//   const Span<const Backend> list(backends, 3);
//   const PixelFormatCaps caps =
//       CookSamplingGuarantee(list, GraphicsPlatform::Windows, PixelFormat::BC7UnormSrgb);
//   if (!HasPixelFormatCaps(caps, CookSamplingCaps)) {
//       // 경고 PlatformFormatFallback 뒤 다음 대체, 마지막은 보장 비압축 형식이다.
//       const PixelFormat fallback = CookSamplingFallback(list, GraphicsPlatform::Windows,
//                                                         PixelFormat::BC7UnormSrgb);
//   }
namespace rocks::graphics {

// 표가 다루는 능력 비트다(샘플 + 선형 필터). 표의 값은 늘 이 비트의 부분집합이다.
inline constexpr PixelFormatCaps CookSamplingCaps =
    PixelFormatCaps::Sampled | PixelFormatCaps::Filterable;

// 보장 칸의 근거다.
enum class CookSamplingBasis : uint8 {
    // 보장 없음
    None,
    // 엔진이 요구하는 최소 API 수준(feature level·core 판)의 사양 보장
    ApiMinimum,
    // 사양 밖이지만 엔진이 그 플랫폼의 장치에 요구하는 능력(장치 생성이 대조해 없으면 거부)
    EngineRequirement
};

// (backend, platform)이 쿠킹 대상 조합인지다. 대상은 D3D12·Windows, D3D11·Windows,
// Vulkan·Windows, Vulkan·Android, OpenGL·Android, Metal·Mac 여섯이다(OpenGL·Windows 미리보기는
// 개발 도구라 대상이 아니다). 대상이 아닌 조합은 모든 형식의 보장이 None이다.
ROCKS_GRAPHICSTYPES_API bool IsCookSamplingTarget(Backend backend,
                                                  GraphicsPlatform platform) noexcept;

// format의 보장 능력이다(CookSamplingCaps의 부분집합: None, Sampled, Sampled|Filterable). 대상이
// 아닌 조합, Unknown·번호표 밖 형식, 범위 밖 enum은 None이다.
ROCKS_GRAPHICSTYPES_API PixelFormatCaps CookSamplingGuarantee(Backend backend,
                                                              GraphicsPlatform platform,
                                                              PixelFormat format) noexcept;

// backends 모두의 보장 교집합이다(프로필 graphicsBackends 전체 검사). 목록이 비었거나 대상이
// 아닌 조합이 끼면 None이다. backends는 호출 동안만 빌린다.
ROCKS_GRAPHICSTYPES_API PixelFormatCaps CookSamplingGuarantee(Span<const Backend> backends,
                                                              GraphicsPlatform platform,
                                                              PixelFormat format) noexcept;

// format 보장 칸의 근거다. 선형 필터가 엔진 요구인 칸(데스크톱 Vulkan 32F 등)은 샘플이 API
// 최소여도 EngineRequirement다. 보장이 None이면 None이다.
ROCKS_GRAPHICSTYPES_API CookSamplingBasis CookSamplingGuaranteeBasis(Backend backend,
                                                                     GraphicsPlatform platform,
                                                                     PixelFormat format) noexcept;

// format을 대신할 보장 비압축 형식이다(TD12의 마지막 대체). 결과는 backends 모두에서 required가
// 보장인 비압축 색 형식이다.
// - format이 비압축 색 형식이고 이미 모두에서 required가 보장이면 format 그대로다.
// - 아니면 format의 대체 계열 후보를 앞에서부터 본다: 8비트 선형 BGRA8Unorm → RGBA8Unorm, 8비트
//   sRGB BGRA8UnormSrgb → RGBA8UnormSrgb, 고정밀(snorm·16비트·float·BC6H·ASTC HDR 등)
//   RGBA16Float. 채널 수는 줄이지 않는다.
// - Unknown: 대체 계열이 없는 형식(정수·깊이·비디오·Unknown), backends가 비었거나 대상이 아닌
//   조합이 낌, required에 Sampled가 없거나 CookSamplingCaps 밖 비트가 있음.
// 쿠킹 대상 조합만 든 목록이면 대체 계열이 있는 형식은 늘 답이 있다(각 계열의 마지막 후보는 모든
// 대상에서 샘플 + 선형 필터 보장이다).
//
// 사용 예:
//   const Backend android[] = {Backend::OpenGL};
//   CookSamplingFallback(Span<const Backend>(android, 1), GraphicsPlatform::Android,
//                        PixelFormat::BC7UnormSrgb);  // RGBA8UnormSrgb(ES에 BGRA8 보장 없음)
ROCKS_GRAPHICSTYPES_API PixelFormat
CookSamplingFallback(Span<const Backend> backends, GraphicsPlatform platform, PixelFormat format,
                     PixelFormatCaps required = CookSamplingCaps) noexcept;

// 장치 능력 배열(색인 = PixelFormatNumber, 보통 MaxPixelFormatNumber + 1칸)에서 보장 칸의 능력이
// 빠진 첫 형식(번호 순)이다. 모두 갖췄거나 대상이 아닌 조합이면 Unknown이다. 배열보다 큰 번호의
// 능력은 None으로 본다. backend 장치 생성이 형식 능력을 질의한 직후 부른다(Docs/Design/PixelFormat
// 7.1절 "장치 생성 강제").
//
// 사용 예(backend):
//   const PixelFormat missing = FindMissingCookSampling(
//       Backend::Vulkan, HostGraphicsPlatform(),
//       Span<const PixelFormatCaps>(formatCaps_, MaxPixelFormatNumber + 1));
//   if (missing != PixelFormat::Unknown) { /* Unsupported, 형식 이름을 진단에 */ }
ROCKS_GRAPHICSTYPES_API PixelFormat FindMissingCookSampling(
    Backend backend, GraphicsPlatform platform, Span<const PixelFormatCaps> capsByNumber) noexcept;

// FindMissingCookSampling이 찾은 missing 때문에 장치 생성을 거부할 때의 진단 문장(영어)을 out에
// NUL로 끝나게 쓴다(넘치면 자른다). 네 backend가 같은 문장을 DeviceCreateReport·LastError에 쓴다.
// out이 비었으면 아무것도 하지 않는다.
//
// 사용 예:
//   char detail[256];
//   WriteCookSamplingRejection(Span<char>(detail), Backend::Vulkan, HostGraphicsPlatform(),
//                              missing);
ROCKS_GRAPHICSTYPES_API void WriteCookSamplingRejection(Span<char> out, Backend backend,
                                                        GraphicsPlatform platform,
                                                        PixelFormat missing) noexcept;

} // namespace rocks::graphics
