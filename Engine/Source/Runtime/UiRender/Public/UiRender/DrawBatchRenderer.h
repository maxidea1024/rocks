#pragma once

#include <Graphics/Device.h>
#include <RenderBase/ShaderSet.h>
#include <UiCore/DrawBatch.h>

namespace rocks::ui {

// UI 그리기 파이프라인을 만든다(Docs/Design/UiToolkit 18.4절 — UiLaneRenderer가 렌더 lane에서
// 연다). 파이프라인은 DrawVertex의 position·uv·color와 16 byte 화면 변환 상수를 받는다. 셰이더는
// 엔진 셰이더 집합(UiShaderTypes 등록, 9.1절 UW1a)의 UI 셰이더이고, 반환한 파이프라인은 호출자가
// Retire한다. 집합이 Ready가 아니면 NotReady, UI 셰이더 타입이 없으면 NotFound다.
class ROCKS_UIRENDER_API DrawBatchRenderer {
public:
    // 단색 사각형용 파이프라인을 만든다.
    static Result<graphics::PipelineHandle>
    CreateSolidPipeline(graphics::Device& device, render::ShaderSet& shaders,
                        graphics::PixelFormat format) noexcept;
    // 직선 알파 텍스처(이미지·컬러 글리프·장면)를 정점 색에 곱한다.
    static Result<graphics::PipelineHandle>
    CreateImagePipeline(graphics::Device& device, render::ShaderSet& shaders,
                        graphics::PixelFormat format) noexcept;
    // 투명 바탕에 그린 하위 트리는 RGB에 알파가 이미 곱해져 있다.
    static Result<graphics::PipelineHandle>
    CreateRetainedPipeline(graphics::Device& device, render::ShaderSet& shaders,
                           graphics::PixelFormat format) noexcept;
    // R8 글리프의 R을 덮임률로 읽고 정점 색의 알파에 곱한다.
    static Result<graphics::PipelineHandle>
    CreateMaskPipeline(graphics::Device& device, render::ShaderSet& shaders,
                       graphics::PixelFormat format) noexcept;
    // 둥근 상자(채움 + 안쪽 외곽선, straight 알파)를 그린다(UW3a).
    static Result<graphics::PipelineHandle>
    CreateRoundedBoxPipeline(graphics::Device& device, render::ShaderSet& shaders,
                             graphics::PixelFormat format) noexcept;
    // 배경을 캡처한 텍스처에 화면 좌표 기준의 가우스 필터를 적용한다.
    static Result<graphics::PipelineHandle>
    CreateBlurPipeline(graphics::Device& device, render::ShaderSet& shaders,
                       graphics::PixelFormat format) noexcept;
};

} // namespace rocks::ui
