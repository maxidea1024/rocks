#include <UiRender/WorldWidgetPlane.h>
#include <RenderBase/PipelineStatePresets.h>
#include <UiRender/UiShaderTypes.h>

namespace rocks::ui {

namespace {

// 평면은 삼각형 띠 정점 넷이다.
constexpr uint32 PlaneStripVertexCount = 4;

// 평면 그리기 상수(이번·지난 프레임 원점·u·v — UiWorldPlane.rsf UiPlaneConstants)의 Vec4 수다.
constexpr uint32 PlaneConstantVectors = 6;

// 속도를 쓰면 색·속도 두 타깃이다.
constexpr uint32 VelocityTargetCount = 2;

} // namespace

Result<WorldWidgetClipPlane> WorldWidgetClipPlane::Project(Vec3d tl, Vec3d tr, Vec3d bl,
                                                           const Mat4d& viewProjection) noexcept {
    auto point = [&](Vec3d world) noexcept {
        auto clip = viewProjection.TransformVector4({world.x, world.y, world.z, 1});
        return Vec4{static_cast<float>(clip.x), static_cast<float>(clip.y),
                    static_cast<float>(clip.z), static_cast<float>(clip.w)};
    };
    WorldWidgetClipPlane plane{point(tl), point(tr), point(bl)};
    if (!plane.topLeft.IsFinite() || !plane.topRight.IsFinite() || !plane.bottomLeft.IsFinite()) {
        return Result<WorldWidgetClipPlane>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<WorldWidgetClipPlane>::Success(plane);
}

WorldWidgetPlane::WorldWidgetPlane(graphics::Device& device, render::ShaderSet& shaders) noexcept
    : device_(device), shaders_(shaders) {
}

WorldWidgetPlane::~WorldWidgetPlane() noexcept {
    if (!quad_.IsNull()) {
        device_.Retire(quad_);
    }

    if (!pipeline_.IsNull()) {
        device_.Retire(pipeline_);
    }
}

Result<void> WorldWidgetPlane::Initialize(graphics::PixelFormat color, graphics::PixelFormat depth,
                                          graphics::PixelFormat velocity, bool writeDepth,
                                          uint32 sampleCount) noexcept {
    const graphics::VertexAttribute attributes[] = {{0, graphics::VertexFormat::Float2, 0, 0}};
    const graphics::VertexStream streams[] = {{sizeof(Vec2), graphics::VertexStepRate::Vertex}};
    graphics::PipelineDesc desc;
    desc.layout = {attributes, streams};
    desc.topology = graphics::PrimitiveTopology::TriangleStrip;
    desc.colorFormats[0] = color;
    desc.blend[0] = render::PremultipliedAlphaBlend();
    desc.depthFormat = depth;
    desc.depthStencil.depthTest = depth != graphics::PixelFormat::Unknown;
    desc.depthStencil.depthWrite = writeDepth;
    desc.drawConstantBytes = sizeof(Vec4) * PlaneConstantVectors;
    desc.sampleCount = sampleCount;
    const bool withVelocity = velocity != graphics::PixelFormat::Unknown;
    if (withVelocity) {
        desc.colorTargetCount = VelocityTargetCount;
        desc.colorFormats[1] = velocity;
    }

    const shader::ShaderTypeName& pixel =
        withVelocity ? UiWorldPlaneVelocityPixelType : UiWorldPlanePixelType;
    auto slot = UiPixelTextureSlot(shaders_, pixel, UiSurfaceName);
    if (!slot) {
        return Result<void>::Failure(slot.Error());
    }

    auto pipeline = CreateUiPipeline(device_, shaders_, UiWorldPlaneVertexType, pixel, desc);
    if (!pipeline) {
        return Result<void>::Failure(pipeline.Error());
    }

    surfaceSlot_ = slot.Value();

    if (quad_.IsNull()) {
        constexpr Vec2 quad[] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
        graphics::BufferDesc buffer;
        buffer.size = sizeof(quad);
        buffer.usage = graphics::BufferUsage::Vertex;
        const Span<const uint8> bytes{reinterpret_cast<const uint8*>(quad), sizeof(quad)};
        auto made = device_.CreateBuffer(buffer, bytes);
        if (!made) {
            device_.Retire(pipeline.Value());
            return Result<void>::Failure(made.Error());
        }

        quad_ = made.Value();
    }

    if (!pipeline_.IsNull()) {
        device_.Retire(pipeline_);
    }

    pipeline_ = pipeline.Value();
    return Result<void>::Success();
}

Result<void> WorldWidgetPlane::Draw(graphics::CommandContext& commands,
                                    graphics::TextureHandle texture,
                                    const WorldWidgetClipPlane& current,
                                    const WorldWidgetClipPlane& previous) noexcept {
    if (pipeline_.IsNull() || texture.IsNull()) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }

    const Vec4 values[] = {current.topLeft,
                           current.topRight - current.topLeft,
                           current.bottomLeft - current.topLeft,
                           previous.topLeft,
                           previous.topRight - previous.topLeft,
                           previous.bottomLeft - previous.topLeft};
    for (const auto value : values) {
        if (!value.IsFinite()) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    commands.SetPipeline(pipeline_);
    auto set = commands.SetDrawConstants(graphics::DrawConstantBytes(values));
    if (!set) {
        return set;
    }

    const graphics::VertexBufferBinding binding[] = {{quad_, 0}};
    commands.SetVertexBuffers(0, binding);
    commands.SetTexture(graphics::ShaderStage::Pixel, surfaceSlot_, texture);
    commands.Draw(PlaneStripVertexCount);
    return Result<void>::Success();
}

} // namespace rocks::ui
