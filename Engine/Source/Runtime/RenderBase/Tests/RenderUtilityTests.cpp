#include "RecordingDevice.h"
#include "RegistryShaderSet.h"

#include <Core/Memory/SystemAllocator.h>
#include <RenderBase/EngineShaderTypes.h>
#include <RenderBase/FullscreenPass.h>
#include <RenderBase/PipelineStatePresets.h>
#include <ShaderCore/ShaderTypeRegistry.h>
#include <TestSupport/Test.h>

// RenderBase 그리기 유틸리티 시험이다(Docs/Design/RenderBase 11.4절 1–4항): 상태 프리셋 값, 엔진
// 셰이더 타입 등록, 전체화면 파이프라인 요청, null 텍스처 바인딩 계수(기록 장치).
using namespace rocks;
using namespace rocks::render;
using rocks::render::testing::RecordingDevice;
using rocks::render::testing::RegistryShaderSet;

namespace render_utility_tests_file {

SystemAllocator& Allocator_() noexcept {
    static SystemAllocator allocator;
    return allocator;
}

ROCKS_SHADER_TYPE_NAME(LonePixelName, "RenderUtilityLonePixel");

constexpr shader::ShaderTypeDesc LonePixel =
    shader::MakeShaderTypeDesc(LonePixelName, "/Engine/Private/RenderUtilityLone.rsf", "Main",
                               shader::ShaderStagePixel, shader::ShaderTypeOwner::Engine);

bool SameBlend(const graphics::ColorTargetBlend& blend, graphics::BlendFactor sourceColor,
               graphics::BlendFactor destinationColor, graphics::BlendFactor sourceAlpha,
               graphics::BlendFactor destinationAlpha) noexcept {
    return blend.enable && blend.sourceColor == sourceColor &&
           blend.destinationColor == destinationColor && blend.sourceAlpha == sourceAlpha &&
           blend.destinationAlpha == destinationAlpha &&
           blend.colorOperation == graphics::BlendOperation::Add &&
           blend.alphaOperation == graphics::BlendOperation::Add &&
           blend.writeMask == graphics::ColorWriteMask::All;
}

// 프리셋은 11.4절 1항의 값이다(UE 정적 상태 템플릿과 같은 뜻).
TEST(RenderBase_PipelineStatePresets) {
    using graphics::BlendFactor;
    CHECK(!OpaqueBlend().enable && OpaqueBlend().writeMask == graphics::ColorWriteMask::All);
    CHECK(SameBlend(AlphaBlend(), BlendFactor::SourceAlpha, BlendFactor::InverseSourceAlpha,
                    BlendFactor::One, BlendFactor::InverseSourceAlpha));
    CHECK(SameBlend(PremultipliedAlphaBlend(), BlendFactor::One, BlendFactor::InverseSourceAlpha,
                    BlendFactor::One, BlendFactor::InverseSourceAlpha));
    CHECK(SameBlend(AdditiveBlend(), BlendFactor::One, BlendFactor::One, BlendFactor::One,
                    BlendFactor::One));
    graphics::PipelineDesc desc;
    SetBlend(desc, AdditiveBlend());
    bool all = true;
    for (const graphics::ColorTargetBlend& target : desc.blend) {
        all = all && SameBlend(target, BlendFactor::One, BlendFactor::One, BlendFactor::One,
                               BlendFactor::One);
    }

    CHECK(all);
    CHECK(NoCull().cullMode == graphics::CullMode::None &&
          BackFaceCull().cullMode == graphics::CullMode::Back &&
          FrontFaceCull().cullMode == graphics::CullMode::Front);
    CHECK(NoCull().depthBias == 0 && !BackFaceCull().depthClamp);
    CHECK(!DepthDisabled().depthTest && !DepthDisabled().depthWrite &&
          !DepthDisabled().stencilTest);
    CHECK(DepthRead().depthTest && !DepthRead().depthWrite &&
          DepthRead().depthCompare == graphics::CompareFunction::GreaterEqual);
    CHECK(DepthReadWrite().depthTest && DepthReadWrite().depthWrite &&
          DepthReadWrite().depthCompare == graphics::CompareFunction::GreaterEqual);
    // 프리셋 파이프라인은 장치 규칙을 통과한다.
    desc.depthFormat = graphics::PixelFormat::D32Float;
    desc.depthStencil = DepthReadWrite();
    desc.raster = BackFaceCull();
    CHECK(graphics::ValidatePipelineDesc(desc));
}

// 엔진 셰이더 타입은 등록부 규칙을 통과하고, 전체화면 요청은 집합의 FullscreenTriangleVertex와
// 불투명·컬 없음·깊이 끔 상태를 채운다.
TEST(RenderBase_FullscreenPipelineRequest) {
    RecordingDevice device(Allocator_());
    shader::ShaderTypeRegistry registry(Allocator_());
    REQUIRE(registry.Register(EngineShaderTypes()));
    REQUIRE(registry.Freeze());
    CHECK(registry.Find(FullscreenTriangleVertexType.hash) != nullptr &&
          registry.Find(CopyTexturePixelType.hash) != nullptr);
    CHECK(registry.Find(FullscreenTriangleVertexType.hash)->stage == shader::ShaderStageVertex &&
          registry.Find(CopyTexturePixelType.hash)->stage == shader::ShaderStagePixel);
    RegistryShaderSet shaders(Allocator_(), device, registry);
    REQUIRE(shaders.ready);
    auto vertex = shaders.set.FindEntry(FullscreenTriangleVertexType.hash);
    auto copy = shaders.set.FindEntry(CopyTexturePixelType.hash);
    REQUIRE(vertex && copy);
    auto request =
        FullscreenPipelineRequest(shaders.set, copy.Value(), graphics::PixelFormat::RGBA8Unorm);
    REQUIRE(request);
    const GraphicsPipelineRequest& value = request.Value();
    CHECK(value.set == &shaders.set && value.vertexEntry == vertex.Value() &&
          value.pixelEntry == copy.Value());
    CHECK(value.state.layout.attributes.Size() == 0 && value.state.layout.streams.Size() == 0);
    CHECK(value.state.colorTargetCount == 1 &&
          value.state.colorFormats[0] == graphics::PixelFormat::RGBA8Unorm);
    CHECK(!value.state.blend[0].enable && value.state.raster.cullMode == graphics::CullMode::None &&
          !value.state.depthStencil.depthTest &&
          value.state.depthFormat == graphics::PixelFormat::Unknown);
    CHECK(FullscreenPipelineRequest(shaders.set, shaders.set.EntryCount(),
                                    graphics::PixelFormat::RGBA8Unorm)
              .Error() == ErrorCode::InvalidArgument);

    // 요청으로 만든 파이프라인은 캐시에 담긴다(두 번째는 적중).
    PipelineCache cache(Allocator_(), device);
    REQUIRE(cache.FindOrCreate(value));
    REQUIRE(cache.FindOrCreate(value));
    CHECK(cache.Misses() == 1 && cache.Hits() == 1);
    cache.Clear();

    // FullscreenTriangleVertex가 없는 집합은 NotFound다.
    shader::ShaderTypeRegistry lone(Allocator_());
    const shader::ShaderTypeDesc* const loneTypes[] = {&LonePixel};
    REQUIRE(lone.Register(loneTypes));
    REQUIRE(lone.Freeze());
    RegistryShaderSet loneShaders(Allocator_(), device, lone);
    REQUIRE(loneShaders.ready);
    CHECK(
        FullscreenPipelineRequest(loneShaders.set, 0, graphics::PixelFormat::RGBA8Unorm).Error() ==
        ErrorCode::NotFound);
    shaders.set.Retire();
    loneShaders.set.Retire();
}

// 흰 텍스처로 대신 묶은 수(null·stale·ReadWrite view)를 센다. 유효한 텍스처는 세지 않는다.
TEST(RenderBase_NullTextureBindingsAreCounted) {
    RecordingDevice device(Allocator_());
    graphics::TextureDesc desc;
    desc.width = 1;
    desc.height = 1;
    desc.format = graphics::PixelFormat::RGBA8Unorm;
    const uint8 pixel[4] = {1, 2, 3, 4};
    auto texture = device.CreateTexture(desc, Span<const uint8>(pixel, sizeof(pixel)));
    REQUIRE(texture);
    graphics::PassDesc pass;
    auto recording = device.BeginPass(pass);
    REQUIRE(recording);
    graphics::CommandContext& commands = *recording.Value();
    CHECK(device.NullTextureBindings() == 0);
    commands.SetTexture(texture.Value());
    CHECK(device.NullTextureBindings() == 0);
    commands.SetTexture(graphics::TextureHandle{});
    CHECK(device.NullTextureBindings() == 1);
    REQUIRE(device.EndFrame());
    device.Retire(texture.Value());
    device.CompleteAll();
    device.ReclaimCompleted();
}

} // namespace render_utility_tests_file
