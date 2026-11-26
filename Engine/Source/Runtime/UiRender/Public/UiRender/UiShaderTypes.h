#pragma once

#include <Core/Containers/Span.h>
#include <Core/Result.h>
#include <Graphics/Device.h>
#include <RenderBase/ShaderSet.h>
#include <ShaderCore/ShaderType.h>

// UiRender가 등록하는 엔진 셰이더 타입이다(Docs/Design/UiToolkit 9.1절 UW1a, 엔진 셰이더 A19).
// 원문은 Engine/Shaders/Private/Ui에 있다. 공개 헤더에는 이름·해시와 텍스처 이름만 두고 desc는
// cpp에 둔다(ShaderSystem 12.2.1절 SSD2 — RenderBase EngineShaderTypes와 같다). 엔진 호스트는
// UiRender 모듈 수명 객체가 호스트 등록부에 넣고, 등록부를 직접 만드는 호스트(UiDemo·시험)는
// UiShaderTypes()를 RenderBase EngineShaderTypes()와 함께 등록한다.
//
// 사용 예:
//   auto pipeline = ui::CreateUiPipeline(device, shaders, ui::UiSolidVertexType,
//                                        ui::UiSolidPixelType, desc);
namespace rocks::ui {

// UiElements.rsf: 단색·텍스처·mask·보관 표면 합성.
ROCKS_SHADER_TYPE_NAME(UiSolidVertexType, "UiSolidVertex");
ROCKS_SHADER_TYPE_NAME(UiSolidPixelType, "UiSolidPixel");
ROCKS_SHADER_TYPE_NAME(UiImageVertexType, "UiImageVertex");
ROCKS_SHADER_TYPE_NAME(UiImagePixelType, "UiImagePixel");
ROCKS_SHADER_TYPE_NAME(UiMaskPixelType, "UiMaskPixel");
ROCKS_SHADER_TYPE_NAME(UiRetainedPixelType, "UiRetainedPixel");

// UiBackdropBlur.rsf: 배경 흐림.
ROCKS_SHADER_TYPE_NAME(UiBlurVertexType, "UiBlurVertex");
ROCKS_SHADER_TYPE_NAME(UiBlurPixelType, "UiBlurPixel");

// UiRoundedBox.rsf: 둥근 상자(채움 + 안쪽 외곽선 — UW3a).
ROCKS_SHADER_TYPE_NAME(UiRoundedBoxVertexType, "UiRoundedBoxVertex");
ROCKS_SHADER_TYPE_NAME(UiRoundedBoxPixelType, "UiRoundedBoxPixel");

// UiWorldPlane.rsf: 월드 공간 위젯 평면(색, 색 + 속도).
ROCKS_SHADER_TYPE_NAME(UiWorldPlaneVertexType, "UiWorldPlaneVertex");
ROCKS_SHADER_TYPE_NAME(UiWorldPlanePixelType, "UiWorldPlanePixel");
ROCKS_SHADER_TYPE_NAME(UiWorldPlaneVelocityPixelType, "UiWorldPlaneVelocityPixel");

// 셰이더의 텍스처 이름이다. slot은 셰이더 바인딩 layout에서 이 이름으로 찾는다(RenderBase 9.13절 —
// UiPixelTextureSlot).
inline constexpr char UiTextureName[] = "UiTexture";
inline constexpr char UiBackdropName[] = "UiBackdrop";
inline constexpr char UiSurfaceName[] = "UiSurface";

// UiRender가 등록하는 셰이더 타입 목록이다(정적 수명).
ROCKS_UIRENDER_API Span<const shader::ShaderTypeDesc* const> UiShaderTypes() noexcept;

// pixel 셰이더 타입의 텍스처 name이 묶이는 slot(backend 위치 — D3D t register, Vulkan binding,
// GLES 텍스처 유닛)이다. CommandContext::SetTexture(ShaderStage::Pixel, slot, …)에 준다. 실패:
// 집합이 Ready가 아님 NotReady, 타입·이름이 없음 NotFound.
ROCKS_UIRENDER_API Result<uint32> UiPixelTextureSlot(render::ShaderSet& shaders,
                                                     const shader::ShaderTypeName& pixel,
                                                     StringView name) noexcept;

// shaders에서 두 타입의 장치 셰이더를 찾아 desc로 파이프라인을 만든다(desc의 셰이더 handle은
// 덮어쓴다). 실패: 집합이 Ready가 아님 NotReady, 타입이 없음 NotFound, 장치 셰이더·파이프라인 생성
// 오류.
ROCKS_UIRENDER_API Result<graphics::PipelineHandle>
CreateUiPipeline(graphics::Device& device, render::ShaderSet& shaders,
                 const shader::ShaderTypeName& vertex, const shader::ShaderTypeName& pixel,
                 graphics::PipelineDesc desc) noexcept;

} // namespace rocks::ui
