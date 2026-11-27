#include "ScreenRenderer.h"

#include <Core/Diagnostics/Log.h>
#include <Core/Move.h>
#include <Graphics/ShaderBindings.h>
#include <RenderBase/FullscreenPass.h>
#include <RenderBase/PipelineStatePresets.h>
#include <RenderGraph/RenderGraph.h>

namespace sandbox::screen {

namespace {

inline constexpr LogCategory LogSandboxScreen{"Sandbox.Screen"};

// 장면 색·깊이 형식이다(18.5절 1항).
constexpr graphics::PixelFormat SceneColorFormat = graphics::PixelFormat::RGBA16Float;
constexpr graphics::PixelFormat SceneDepthFormat = graphics::PixelFormat::D32Float;
// 스왑체인·capture 그리기 view 형식이다(스왑체인 계약 — 그리기 view sRGB).
constexpr graphics::PixelFormat OutputFormat = graphics::PixelFormat::RGBA8UnormSrgb;
constexpr usize OutputBytesPerPixel = 4;

// 메시 파이프라인 번호다.
constexpr uint32 BackFaceMesh = 0;
constexpr uint32 FrontFaceMesh = 1;

TraceScopeName BasePassName("Sandbox.BasePass");
TraceScopeName TonemapName("Sandbox.Tonemap");
TraceScopeName TonemapCaptureName("Sandbox.TonemapCapture");
TraceScopeName InspectorName("Sandbox.TextureInspector");
TraceScopeName InspectorCaptureName("Sandbox.TextureInspectorCapture");

// 차원·성분 계열마다 셰이더 타입·텍스처 이름·대체 텍스처 모양이다(InspectorKindCount 칸).
struct InspectorKind {
    graphics::TextureDimension dimension;
    InspectorComponent component;
    shader::ShaderTypeName type;
    const char* texture;
    shader::ShaderResourceShape shape;
};

constexpr InspectorKind InspectorKinds[InspectorKindCount] = {
    {graphics::TextureDimension::Texture2D, InspectorComponent::Float,
     render::TextureInspector2DPixelType, render::TextureInspector2DSourceName,
     shader::ShaderResourceShape::Texture2D},
    {graphics::TextureDimension::Texture2DArray, InspectorComponent::Float,
     render::TextureInspector2DArrayPixelType, render::TextureInspector2DArraySourceName,
     shader::ShaderResourceShape::Texture2DArray},
    {graphics::TextureDimension::TextureCube, InspectorComponent::Float,
     render::TextureInspectorCubePixelType, render::TextureInspectorCubeSourceName,
     shader::ShaderResourceShape::TextureCube},
    {graphics::TextureDimension::TextureCubeArray, InspectorComponent::Float,
     render::TextureInspectorCubeArrayPixelType, render::TextureInspectorCubeArraySourceName,
     shader::ShaderResourceShape::TextureCubeArray},
    {graphics::TextureDimension::Texture3D, InspectorComponent::Float,
     render::TextureInspectorVolumePixelType, render::TextureInspectorVolumeSourceName,
     shader::ShaderResourceShape::Texture3D},
    {graphics::TextureDimension::Texture2D, InspectorComponent::Uint,
     render::TextureInspector2DUintPixelType, render::TextureInspector2DUintSourceName,
     shader::ShaderResourceShape::Texture2D},
    {graphics::TextureDimension::Texture2DArray, InspectorComponent::Uint,
     render::TextureInspector2DArrayUintPixelType, render::TextureInspector2DArrayUintSourceName,
     shader::ShaderResourceShape::Texture2DArray},
    {graphics::TextureDimension::Texture3D, InspectorComponent::Uint,
     render::TextureInspectorVolumeUintPixelType, render::TextureInspectorVolumeUintSourceName,
     shader::ShaderResourceShape::Texture3D},
    {graphics::TextureDimension::Texture2D, InspectorComponent::Int,
     render::TextureInspector2DIntPixelType, render::TextureInspector2DIntSourceName,
     shader::ShaderResourceShape::Texture2D},
    {graphics::TextureDimension::Texture2DArray, InspectorComponent::Int,
     render::TextureInspector2DArrayIntPixelType, render::TextureInspector2DArrayIntSourceName,
     shader::ShaderResourceShape::Texture2DArray},
    {graphics::TextureDimension::Texture3D, InspectorComponent::Int,
     render::TextureInspectorVolumeIntPixelType, render::TextureInspectorVolumeIntSourceName,
     shader::ShaderResourceShape::Texture3D}};

// 2D float 칸이다(CPU Availability·대체 없음의 검정 표시).
constexpr uint32 InspectorKind2D = 0;
constexpr uint32 NoInspectorKind = InspectorKindCount;

uint32 FindInspectorKind(graphics::TextureDimension dimension,
                         InspectorComponent component) noexcept {
    for (uint32 index = 0; index < InspectorKindCount; ++index) {
        if (InspectorKinds[index].dimension == dimension &&
            InspectorKinds[index].component == component) {
            return index;
        }
    }

    return NoInspectorKind;
}

shader::ShaderComponentType ShaderComponentOf(InspectorComponent component) noexcept {
    switch (component) {
    case InspectorComponent::Float:
        return shader::ShaderComponentType::Float;
    case InspectorComponent::Uint:
        return shader::ShaderComponentType::Uint;
    case InspectorComponent::Int:
        return shader::ShaderComponentType::Int;
    }

    return shader::ShaderComponentType::Float;
}

// lane이 장치 handle로 푼 그리기 항목이다(그 프레임 arena에 둔다).
struct ResolvedItem {
    render::BuiltinMeshConstants constants;
    graphics::PipelineHandle pipeline;
    graphics::TextureHandle texture;
    const render::BuiltinMeshBuffers* mesh = nullptr;
};

Span<const uint8> Bytes(const void* data, usize size) noexcept {
    return Span<const uint8>(static_cast<const uint8*>(data), size);
}

} // namespace

ScreenRenderer::ScreenRenderer(Allocator& allocator, const render::EngineShaderHost& host) noexcept
    : allocator_(allocator), host_(host), resources_(allocator), captured_(allocator) {
    // capture 자리는 미리 만든다(lane이 자리를 늘리지 않는다 — 할당 실패면 자리가 적을 뿐이다).
    if (captured_.TryReserve(MaxCaptureSlots)) {
        for (uint32 slot = 0; slot < MaxCaptureSlots; ++slot) {
            (void)captured_.TryPushBack(Array<uint8>(allocator));
        }
    }
}

uint32 InspectorLayerCount(const InspectorTexture& texture, uint32 mip) noexcept {
    switch (texture.dimension) {
    case graphics::TextureDimension::Texture2DArray:
    case graphics::TextureDimension::TextureCubeArray:
        return texture.layers;
    case graphics::TextureDimension::Texture3D:
        return graphics::MipExtent(texture.depth, mip);
    default:
        return 1;
    }
}

ScreenRenderer::~ScreenRenderer() noexcept = default;

void ScreenRenderer::SetInitialSurface(graphics::NativeSurface surface, uint32 width,
                                       uint32 height) noexcept {
    surface_ = surface;
    width_ = width;
    height_ = height;
}

Result<void> ScreenRenderer::CreateSwapChain(graphics::Device& device,
                                             graphics::NativeSurface surface, uint32 width,
                                             uint32 height) noexcept {
    graphics::SwapChainDesc desc;
    desc.surface = surface;
    desc.width = width;
    desc.height = height;
    auto created = device.CreateSwapChain(desc);
    if (!created) {
        Log::Error(LogSandboxScreen,
                   "The swap chain could not be created ({Width}x{Height}): {Error}", width, height,
                   ErrorCodeName(created.Error()));
        return Result<void>::Failure(created.Error());
    }

    swapChain_ = created.Value();
    surface_ = surface;
    width_ = width;
    height_ = height;
    return Result<void>::Success();
}

namespace {

// layout의 PS 텍스처 slot이다(없으면 SetTexture가 무시하는 범위 밖 slot).
uint32 TextureSlotOf(const shader::ShaderBindingLayout* layout, StringView name) noexcept {
    auto slot = layout != nullptr ? graphics::FindTextureSlot(*layout, name)
                                  : Result<uint32>::Failure(ErrorCode::NotFound);
    return slot ? slot.Value() : graphics::MaxResourceSlots;
}

} // namespace

Result<void> ScreenRenderer::PrepareRequests() noexcept {
    render::ShaderSet* set = resources_.Shaders();
    const render::BuiltinShaderEntries& entries = resources_.Builtins().ShaderEntries();
    for (uint32 index = 0; index < 2; ++index) {
        render::GraphicsPipelineRequest& request = meshRequests_[index];
        request.set = set;
        request.vertexEntry = entries.meshVertex;
        request.pixelEntry = entries.meshPixel;
        request.state.layout = render::BuiltinVertexLayout();
        request.state.colorFormats[0] = SceneColorFormat;
        request.state.depthFormat = SceneDepthFormat;
        request.state.raster =
            index == BackFaceMesh ? render::BackFaceCull() : render::FrontFaceCull();
        request.state.depthStencil = render::DepthReadWrite();
        request.state.drawConstantBytes = sizeof(render::BuiltinMeshConstants);
    }

    meshTextureSlot_ =
        TextureSlotOf(set->BindingLayout(entries.meshPixel), render::BuiltinMeshAlbedoName);

    auto tonemap = set->FindEntry(render::TonemapPixelType.hash);
    if (!tonemap) {
        return Result<void>::Failure(tonemap.Error());
    }

    auto request = render::FullscreenPipelineRequest(*set, tonemap.Value(), OutputFormat);
    if (!request) {
        return Result<void>::Failure(request.Error());
    }

    tonemapRequest_ = request.Value();
    tonemapTextureSlot_ =
        TextureSlotOf(set->BindingLayout(tonemap.Value()), render::TonemapSceneColorName);
    tonemapRequest_.state.drawConstantBytes = sizeof(render::TonemapConstants);
    for (uint32 view = 0; view < InspectorKindCount; ++view) {
        auto entry = set->FindEntry(InspectorKinds[view].type.hash);
        if (!entry) {
            return Result<void>::Failure(entry.Error());
        }

        auto inspector = render::FullscreenPipelineRequest(*set, entry.Value(), OutputFormat);
        if (!inspector) {
            return Result<void>::Failure(inspector.Error());
        }

        inspectorRequests_[view] = inspector.Value();
        inspectorTextureSlots_[view] =
            TextureSlotOf(set->BindingLayout(entry.Value()), InspectorKinds[view].texture);
        inspectorRequests_[view].state.drawConstantBytes =
            sizeof(render::TextureInspectorConstants);
    }

    return Result<void>::Success();
}

Result<void> ScreenRenderer::OnLaneStart(render::RenderLaneContext& context) noexcept {
    graphics::Device& device = context.Device();
    // 시작 순서(5.5절): 스왑체인 → 엔진 셰이더 집합·내장 자원 팩 → 렌더러 자원.
    if (surface_.window != nullptr && width_ != 0 && height_ != 0) {
        if (auto created = CreateSwapChain(device, surface_, width_, height_); !created) {
            return created;
        }
    }

    if (auto opened = resources_.Open(device, host_); !opened) {
        return opened;
    }

    opened_ = true;
    auto cache = UniquePtr<render::PipelineCache>::TryMake(allocator_, allocator_, device);
    if (!cache) {
        return Result<void>::Failure(cache.Error());
    }

    cache_ = Move(cache.Value());
    auto pool = UniquePtr<render::GraphResourcePool>::TryMake(allocator_, allocator_, device);
    if (!pool) {
        return Result<void>::Failure(pool.Error());
    }

    pool_ = Move(pool.Value());
    return PrepareRequests();
}

void ScreenRenderer::OnLaneStop(render::RenderLaneContext& context) noexcept {
    graphics::Device& device = context.Device();
    // 종료 4·5단계(5.5절): 파이프라인 캐시 비우기 → 그래프 풀 → 엔진 렌더 자원 → 스왑체인.
    if (cache_) {
        cache_->Clear();
        cache_.Reset();
    }

    if (pool_) {
        pool_->Clear();
        pool_.Reset();
    }

    resources_.Retire(device);
    if (!swapChain_.IsNull()) {
        device.Retire(swapChain_);
        swapChain_ = {};
    }
}

// 그래프를 실행하고 capture를 읽는다(그래프 실패는 lane 실패로 남긴다).
bool ScreenRenderer::FinishFrame(render::RenderFrameContext& context, render::RenderGraph& graph,
                                 graphics::TextureHandle capture, uint32 slot) noexcept {
    if (auto executed = graph.Execute(); !executed) {
        ++stats_.graphFailures;
        Log::Error(LogSandboxScreen, "The screen frame graph failed: {Error} ({Detail})",
                   ErrorCodeName(executed.Error()), graph.LastError());
        context.ReportFailure(executed.Error());
        return false;
    }

    ++stats_.framesRendered;
    if (!capture.IsNull() && slot < captured_.Size()) {
        Array<uint8>& pixels = captured_[slot];
        const usize bytes = static_cast<usize>(width_) * height_ * OutputBytesPerPixel;
        if (pixels.TryResize(bytes) && context.Device().ReadPixels(capture, pixels.View())) {
            captureWidth_ = width_;
            captureHeight_ = height_;
            ++stats_.captures;
        }
    }

    return true;
}

// 텍스처 표시 view를 그린다(장면 대신 — 18.5절 6항).
void ScreenRenderer::RenderInspector(render::RenderFrameContext& context,
                                     const ScreenPacket& packet) noexcept {
    const InspectorTexture& target = packet.inspector;
    render::BuiltinResources& builtins = resources_.Builtins();
    const graphics::TextureHandle black = builtins.Texture(render::BuiltinTexture::Black2D);
    uint32 kind = FindInspectorKind(target.dimension, target.component);
    graphics::TextureHandle texture;
    bool fallback = false;
    if (target.cpuPlaceholder || kind == NoInspectorKind) {
        // CPU Availability(6.5절)와 표시 엔트리가 없는 조합(정수 cube 계열)은 검정 2D다.
        kind = InspectorKind2D;
        texture = black;
        fallback = !target.cpuPlaceholder;
    } else {
        auto typed = builtins.FallbackTexture(InspectorKinds[kind].shape,
                                              ShaderComponentOf(target.component));
        auto ready = context.Handles().Texture(target.handle);
        if (ready) {
            texture = ready.Value();
        } else if (typed) {
            // Pending·Failed는 typed 대체다(6.2절 — 계수는 ResolveTexture가 올린다).
            auto resolved =
                builtins.ResolveTexture(context.Handles(), target.handle, typed.Value());
            texture = resolved ? resolved.Value() : typed.Value();
            fallback = true;
        } else {
            // 맞는 typed 대체가 없는 조합(정수 2D 배열·volume)은 검정 2D다.
            kind = InspectorKind2D;
            texture = black;
            fallback = true;
        }
    }

    if (fallback) {
        ++stats_.inspectorFallbacks;
    }

    const uint32 mip = texture == black ? 0u : packet.inspectorMip;
    render::TextureInspectorConstants constants;
    constants.exposure[0] = packet.tonemap.exposure[0];
    constants.selection[0] = static_cast<float>(mip);
    constants.selection[1] = static_cast<float>(packet.inspectorLayer);
    constants.selection[2] = static_cast<float>(graphics::MipExtent(target.depth, mip));
    constants.selection[3] = static_cast<float>(
        texture == black ? render::TextureInspectorDisplay::Ldr : target.display);
    const graphics::ChannelSwizzle swizzle =
        texture == black ? graphics::ChannelSwizzle{} : target.swizzle;
    for (uint32 channel = 0; channel < 4; ++channel) {
        constants.swizzle[channel] = static_cast<float>(swizzle.Channel(channel));
    }

    constants.integer[0] = target.integerScale;
    constants.integer[1] = static_cast<float>(graphics::MipExtent(target.width, mip));
    constants.integer[2] = static_cast<float>(graphics::MipExtent(target.height, mip));
    const uint32 view = kind;
    const uint32 slot = inspectorTextureSlots_[view];
    auto bind = [texture, constants, slot](render::GraphPassContext& pass) noexcept {
        pass.Commands().SetTexture(graphics::ShaderStage::Pixel, slot, texture);
        (void)pass.Commands().SetDrawConstants(Bytes(&constants, sizeof(constants)));
    };
    render::RenderGraph graph(render::MakeRenderGraphDesc(context, *pool_));
    render::GraphPassAccesses present(graph);
    present.SwapChainTarget(swapChain_, packet.clear);
    (void)graph.AddFullscreenPass(InspectorName, render::GraphPassFlags::None, present, *cache_,
                                  inspectorRequests_[view], bind);
    graphics::TextureHandle captureTexture;
    if (!packet.capture.IsNull()) {
        auto target = graph.ImportTexture(packet.capture, "Capture");
        auto device = context.Handles().Texture(packet.capture);
        if (target && device) {
            captureTexture = device.Value();
            render::GraphPassAccesses capture(graph);
            capture.RenderTarget(target.Value(), render::GraphLoadAction::DontCare);
            (void)graph.AddFullscreenPass(InspectorCaptureName, render::GraphPassFlags::None,
                                          capture, *cache_, inspectorRequests_[view], bind);
        }
    }

    if (FinishFrame(context, graph, captureTexture, packet.captureSlot)) {
        ++stats_.inspectorFrames;
    }
}

void RenderScreen(render::RenderFrameContext& context, ScreenPacket& packet) noexcept {
    auto& renderer = *static_cast<ScreenRenderer*>(context.Processor());
    if (renderer.swapChain_.IsNull() || !renderer.resources_.IsReady()) {
        ++renderer.stats_.framesSkipped;
        return;
    }

    // 감시 재컴파일의 셰이더 교체(ShaderSystem 16.3절 SSD76): 프레임 경계에서 새 집합으로 바꾸면
    // 옛 집합의 파이프라인은 캐시에서 회수되었으므로 요청(집합·항목)을 다시 만든다.
    if (renderer.resources_.ApplyShaderUpdates(renderer.cache_.Get())) {
        if (auto prepared = renderer.PrepareRequests(); !prepared) {
            ++renderer.stats_.graphFailures;
            Log::Error(LogSandboxScreen,
                       "The screen pipelines could not be prepared after a shader swap: {Error}",
                       ErrorCodeName(prepared.Error()));
            context.ReportFailure(prepared.Error());
            return;
        }
    }

    if (packet.inspecting) {
        renderer.RenderInspector(context, packet);
        return;
    }

    render::BuiltinResources& builtins = renderer.resources_.Builtins();
    // 메시 파이프라인은 소유 스레드(이 자리)에서 만든다(그래프 기록 함수는 장치 생성을 하지
    // 않는다).
    graphics::PipelineHandle pipelines[2];
    for (uint32 index = 0; index < 2; ++index) {
        auto pipeline = renderer.cache_->FindOrCreate(renderer.meshRequests_[index]);
        if (!pipeline) {
            ++renderer.stats_.graphFailures;
            context.ReportFailure(pipeline.Error());
            return;
        }

        pipelines[index] = pipeline.Value();
    }

    auto fallback = builtins.FallbackTexture(shader::ShaderResourceShape::Texture2D,
                                             shader::ShaderComponentType::Float);
    auto resolved = context.Arena().TryNewArray<ResolvedItem>(packet.items.Size());
    if (!fallback || !resolved) {
        ++renderer.stats_.graphFailures;
        return;
    }

    for (usize index = 0; index < packet.items.Size(); ++index) {
        const ScreenItem& item = packet.items[index];
        ResolvedItem& out = resolved.Value()[index];
        out.constants = item.constants;
        out.pipeline = pipelines[item.reverseCull ? FrontFaceMesh : BackFaceMesh];
        out.mesh = &builtins.Mesh(item.mesh);
        out.texture = builtins.Texture(item.builtin);
        if (!item.texture.IsNull()) {
            auto texture =
                builtins.ResolveTexture(context.Handles(), item.texture, fallback.Value());
            out.texture = texture ? texture.Value() : fallback.Value();
        }
    }

    const Span<const ResolvedItem> items(resolved.Value().Data(), resolved.Value().Size());
    render::RenderGraph graph(render::MakeRenderGraphDesc(context, *renderer.pool_));
    graphics::TextureDesc colorDesc;
    colorDesc.width = renderer.width_;
    colorDesc.height = renderer.height_;
    colorDesc.format = SceneColorFormat;
    colorDesc.usage = graphics::TextureUsage::RenderTarget;
    colorDesc.sampler.filter = graphics::Filter::Point;
    colorDesc.sampler.addressU = graphics::AddressMode::Clamp;
    colorDesc.sampler.addressV = graphics::AddressMode::Clamp;
    graphics::TextureDesc depthDesc;
    depthDesc.width = renderer.width_;
    depthDesc.height = renderer.height_;
    depthDesc.format = SceneDepthFormat;
    depthDesc.usage = graphics::TextureUsage::DepthStencil;
    auto color = graph.CreateTexture(colorDesc, "SceneColor");
    auto depth = graph.CreateTexture(depthDesc, "SceneDepth");
    if (!color || !depth) {
        ++renderer.stats_.graphFailures;
        return;
    }

    const render::GraphTexture sceneColor = color.Value();
    render::GraphDepthTarget depthTarget;
    depthTarget.texture = depth.Value();
    render::GraphPassAccesses base(graph);
    base.RenderTarget(sceneColor, render::GraphLoadAction::Clear, packet.clear)
        .DepthTarget(depthTarget);
    (void)graph.AddPass(
        BasePassName, render::GraphPassKind::Raster, render::GraphPassFlags::None, base,
        [items, meshSlot = renderer.meshTextureSlot_](render::GraphPassContext& pass) noexcept {
            graphics::CommandContext& commands = pass.Commands();
            for (const ResolvedItem& item : items) {
                commands.SetPipeline(item.pipeline);
                (void)commands.SetDrawConstants(Bytes(&item.constants, sizeof(item.constants)));
                commands.SetTexture(graphics::ShaderStage::Pixel, meshSlot, item.texture);
                const graphics::VertexBufferBinding vertices[] = {{item.mesh->vertices, 0}};
                commands.SetVertexBuffers(0, vertices);
                commands.SetIndexBuffer(item.mesh->indices, item.mesh->indexFormat);
                commands.DrawIndexed(item.mesh->indexCount);
            }
        });
    const render::TonemapConstants tonemap = packet.tonemap;
    auto bindTonemap = [sceneColor, tonemap, slot = renderer.tonemapTextureSlot_](
                           render::GraphPassContext& pass) noexcept {
        pass.Commands().SetTexture(graphics::ShaderStage::Pixel, slot, pass.Texture(sceneColor));
        (void)pass.Commands().SetDrawConstants(Bytes(&tonemap, sizeof(tonemap)));
    };
    render::GraphPassAccesses present(graph);
    present.Texture(sceneColor, graphics::ResourceAccess::SrvGraphics)
        .SwapChainTarget(renderer.swapChain_, packet.clear);
    (void)graph.AddFullscreenPass(TonemapName, render::GraphPassFlags::None, present,
                                  *renderer.cache_, renderer.tonemapRequest_, bindTonemap);
    graphics::TextureHandle captureTexture;
    if (!packet.capture.IsNull()) {
        auto target = graph.ImportTexture(packet.capture, "Capture");
        auto device = context.Handles().Texture(packet.capture);
        if (target && device) {
            captureTexture = device.Value();
            render::GraphPassAccesses capture(graph);
            capture.Texture(sceneColor, graphics::ResourceAccess::SrvGraphics)
                .RenderTarget(target.Value(), render::GraphLoadAction::DontCare);
            (void)graph.AddFullscreenPass(TonemapCaptureName, render::GraphPassFlags::None, capture,
                                          *renderer.cache_, renderer.tonemapRequest_, bindTonemap);
        }
    }

    (void)renderer.FinishFrame(context, graph, captureTexture, packet.captureSlot);
}

void CreateSurface(render::RenderLaneContext& context, SurfaceCreate& value) noexcept {
    auto& renderer = *static_cast<ScreenRenderer*>(context.Processor());
    if (!renderer.swapChain_.IsNull() || value.width == 0 || value.height == 0) {
        return;
    }

    (void)renderer.CreateSwapChain(context.Device(), value.surface, value.width, value.height);
}

void ResizeSurface(render::RenderLaneContext& context, SurfaceResize& value) noexcept {
    auto& renderer = *static_cast<ScreenRenderer*>(context.Processor());
    if (renderer.swapChain_.IsNull() || value.width == 0 || value.height == 0 ||
        (value.width == renderer.width_ && value.height == renderer.height_)) {
        return;
    }

    auto resized = context.Device().ResizeSwapChain(renderer.swapChain_, value.width, value.height);
    if (!resized) {
        Log::Error(LogSandboxScreen,
                   "The swap chain could not be resized to {Width}x{Height}: {Error}", value.width,
                   value.height, ErrorCodeName(resized.Error()));
        context.ReportFailure(resized.Error());
        return;
    }

    renderer.width_ = value.width;
    renderer.height_ = value.height;
    ++renderer.stats_.resizes;
}

void ReleaseSurface(render::RenderLaneContext& context, SurfaceRelease&) noexcept {
    auto& renderer = *static_cast<ScreenRenderer*>(context.Processor());
    if (renderer.swapChain_.IsNull()) {
        return;
    }

    context.Device().Retire(renderer.swapChain_);
    renderer.swapChain_ = {};
    // 표면을 돌려주기 전에 그 표면을 쓰는 GPU 일과 회수를 끝낸다(5.3절 예외 1).
    (void)context.Device().WaitIdle();
}

void CreateCapture(render::RenderLaneContext& context, CaptureCreate& value) noexcept {
    graphics::TextureDesc desc;
    desc.width = value.width;
    desc.height = value.height;
    desc.format = OutputFormat;
    desc.usage = graphics::TextureUsage::RenderTarget;
    auto created = context.Device().CreateTexture(desc, {});
    render::RenderNotice notice;
    notice.handle = value.handle;
    if (!created) {
        (void)context.Handles().MarkFailed(value.handle, created.Error());
        notice.kind = render::RenderNoticeKind::ResourceFailed;
        notice.error = created.Error();
    } else if (!context.Handles().BindTexture(value.handle, created.Value())) {
        context.Device().Retire(created.Value());
        return;
    } else {
        notice.kind = render::RenderNoticeKind::ResourceReady;
    }

    context.Notify(notice);
}

} // namespace sandbox::screen
