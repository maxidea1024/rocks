#include <UiRender/UiShaderTypes.h>

// UiRender 셰이더 타입 desc다(Docs/Design/UiToolkit 9.1절 UW1a). 원문 경로는 Engine/Shaders 아래다.
namespace rocks::ui {

namespace {

constexpr char ElementsPath[] = "/Engine/Private/Ui/UiElements.rsf";
constexpr char BlurPath[] = "/Engine/Private/Ui/UiBackdropBlur.rsf";
constexpr char PlanePath[] = "/Engine/Private/Ui/UiWorldPlane.rsf";
constexpr char RoundedBoxPath[] = "/Engine/Private/Ui/UiRoundedBox.rsf";

constexpr shader::ShaderTypeDesc Vertex(shader::ShaderTypeName name, StringView path) noexcept {
    return shader::MakeShaderTypeDesc(name, path, name.name, shader::ShaderStageVertex,
                                      shader::ShaderTypeOwner::Engine);
}

constexpr shader::ShaderTypeDesc Pixel(shader::ShaderTypeName name, StringView path) noexcept {
    return shader::MakeShaderTypeDesc(name, path, name.name, shader::ShaderStagePixel,
                                      shader::ShaderTypeOwner::Engine);
}

constexpr shader::ShaderTypeDesc SolidVertexDesc = Vertex(UiSolidVertexType, ElementsPath);
constexpr shader::ShaderTypeDesc SolidPixelDesc = Pixel(UiSolidPixelType, ElementsPath);
constexpr shader::ShaderTypeDesc ImageVertexDesc = Vertex(UiImageVertexType, ElementsPath);
constexpr shader::ShaderTypeDesc ImagePixelDesc = Pixel(UiImagePixelType, ElementsPath);
constexpr shader::ShaderTypeDesc MaskPixelDesc = Pixel(UiMaskPixelType, ElementsPath);
constexpr shader::ShaderTypeDesc RetainedPixelDesc = Pixel(UiRetainedPixelType, ElementsPath);
constexpr shader::ShaderTypeDesc BlurVertexDesc = Vertex(UiBlurVertexType, BlurPath);
constexpr shader::ShaderTypeDesc BlurPixelDesc = Pixel(UiBlurPixelType, BlurPath);
constexpr shader::ShaderTypeDesc RoundedBoxVertexDesc =
    Vertex(UiRoundedBoxVertexType, RoundedBoxPath);
constexpr shader::ShaderTypeDesc RoundedBoxPixelDesc = Pixel(UiRoundedBoxPixelType, RoundedBoxPath);
constexpr shader::ShaderTypeDesc PlaneVertexDesc = Vertex(UiWorldPlaneVertexType, PlanePath);
constexpr shader::ShaderTypeDesc PlanePixelDesc = Pixel(UiWorldPlanePixelType, PlanePath);
constexpr shader::ShaderTypeDesc PlaneVelocityPixelDesc =
    Pixel(UiWorldPlaneVelocityPixelType, PlanePath);

constexpr const shader::ShaderTypeDesc* Types[] = {
    &SolidVertexDesc,    &SolidPixelDesc,    &ImageVertexDesc,        &ImagePixelDesc,
    &MaskPixelDesc,      &RetainedPixelDesc, &BlurVertexDesc,         &BlurPixelDesc,
    &PlaneVertexDesc,    &PlanePixelDesc,    &PlaneVelocityPixelDesc, &RoundedBoxVertexDesc,
    &RoundedBoxPixelDesc};

Result<graphics::ShaderHandle> DeviceShader(render::ShaderSet& shaders,
                                            const shader::ShaderTypeName& type) noexcept {
    auto entry = shaders.FindEntry(type.hash);
    if (!entry) {
        return Result<graphics::ShaderHandle>::Failure(entry.Error());
    }

    return shaders.DeviceShader(entry.Value());
}

} // namespace

Span<const shader::ShaderTypeDesc* const> UiShaderTypes() noexcept {
    return Span<const shader::ShaderTypeDesc* const>(Types);
}

Result<graphics::PipelineHandle> CreateUiPipeline(graphics::Device& device,
                                                  render::ShaderSet& shaders,
                                                  const shader::ShaderTypeName& vertex,
                                                  const shader::ShaderTypeName& pixel,
                                                  graphics::PipelineDesc desc) noexcept {
    auto vs = DeviceShader(shaders, vertex);
    if (!vs) {
        return Result<graphics::PipelineHandle>::Failure(vs.Error());
    }

    auto ps = DeviceShader(shaders, pixel);
    if (!ps) {
        return Result<graphics::PipelineHandle>::Failure(ps.Error());
    }

    desc.vertexShader = vs.Value();
    desc.pixelShader = ps.Value();
    return device.CreatePipeline(desc);
}

} // namespace rocks::ui
