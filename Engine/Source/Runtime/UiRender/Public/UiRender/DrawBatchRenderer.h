#pragma once

#include <Graphics/Device.h>
#include <RenderBase/ShaderSet.h>
#include <UiCore/DrawBatch.h>

namespace rocks::ui {

class DrawBatchBuffers;

// Render lane이 관리하는 브러시 자원이다. 배치 소비가 끝날 때까지 등록과 텍스처를 유지한다.
struct BrushTexture {
    graphics::TextureHandle texture{};
    graphics::PipelineHandle pipeline{};
};

// DrawBatch를 현재 Graphics 패스에 기록한다. Render owner lane에서 호출한다.
// pipeline은 DrawVertex의 position·uv·color와 16 byte 화면 변환 상수를 받도록 생성해야 한다.
// pipeline과 패스의 색상 형식·블렌드 설정은 호출자가 맞춘다.
class ROCKS_UIRENDER_API DrawBatchRenderer {
public:
    // 준비된 배치를 오프스크린에 그려 위에서 아래 순서의 RGBA 픽셀을 읽는다.
    static Result<void> ReadPixels(graphics::Device& device, graphics::PipelineHandle pipeline,
                                   const DrawBatchBuffers& buffers,
                                   const SlotArray<BrushTexture>& textures, uint32 width,
                                   uint32 height, Array<uint8>& rgba) noexcept;
    // 파이프라인은 shaders(엔진 셰이더 집합 — UiShaderTypes 등록, UiToolkit 9.1절 UW1a)의 UI
    // 셰이더로 만든다. 반환한 파이프라인은 호출자가 Retire한다. 집합이 Ready가 아니면 NotReady, UI
    // 셰이더 타입이 없으면 NotFound다.
    //
    // 단색 사각형용 파이프라인을 만든다.
    static Result<graphics::PipelineHandle>
    CreateSolidPipeline(graphics::Device& device, render::ShaderSet& shaders,
                        graphics::PixelFormat format) noexcept;

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

    // 배경을 캡처한 텍스처에 화면 좌표 기준의 가우스 필터를 적용한다.
    static Result<graphics::PipelineHandle>
    CreateBlurPipeline(graphics::Device& device, render::ShaderSet& shaders,
                       graphics::PixelFormat format) noexcept;

    static Result<void>
    Draw(graphics::Device& device, render::ShaderSet& shaders, graphics::CommandContext& commands,
         graphics::PipelineHandle pipeline, const DrawBatch& batch, Vec2 viewportSize,
         const SlotArray<BrushTexture>* textures = nullptr,
         graphics::PixelFormat targetFormat = graphics::PixelFormat::RGBA8UnormSrgb) noexcept;
    static Result<void> Draw(graphics::CommandContext& commands, graphics::PipelineHandle pipeline,
                             const DrawBatchBuffers& buffers, Vec2 viewportSize,
                             const SlotArray<BrushTexture>* textures = nullptr) noexcept;
};

} // namespace rocks::ui
