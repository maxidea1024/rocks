#include "ScreenRenderer.h"

#include <Core/Diagnostics/Log.h>
#include <Core/Math/Float16.h>
#include <Core/Move.h>
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

// 절차 생성 표시 텍스처의 색이다(RGBA8Unorm).
constexpr uint8 Red[4] = {255, 0, 0, 255};
constexpr uint8 Green[4] = {0, 255, 0, 255};
constexpr uint8 Blue[4] = {0, 0, 255, 255};
constexpr uint8 White[4] = {255, 255, 255, 255};
constexpr uint8 Cyan[4] = {0, 255, 255, 255};
constexpr uint8 Magenta[4] = {255, 0, 255, 255};
constexpr uint8 Yellow[4] = {255, 255, 0, 255};
// cube 면 순서(+X, -X, +Y, -Y, +Z, -Z)의 색이다.
constexpr const uint8* CubeFaceColors[6] = {Red, Cyan, Green, Magenta, Blue, Yellow};
// 2D 표시 텍스처 크기(mip 0)다.
constexpr uint32 Inspector2DSize = 4;
constexpr usize RgbaBytes = 4;

// view마다 셰이더 타입·텍스처 이름·표시 방식이다(None 칸은 쓰지 않는다).
struct InspectorKind {
    shader::ShaderTypeName type;
    const char* texture;
    render::TextureInspectorDisplay display;
};

constexpr InspectorKind InspectorKinds[InspectorViewCount] = {
    {render::TextureInspector2DPixelType, render::TextureInspector2DSourceName,
     render::TextureInspectorDisplay::Ldr},
    {render::TextureInspector2DPixelType, render::TextureInspector2DSourceName,
     render::TextureInspectorDisplay::Ldr},
    {render::TextureInspector2DPixelType, render::TextureInspector2DSourceName,
     render::TextureInspectorDisplay::Hdr},
    {render::TextureInspector2DArrayPixelType, render::TextureInspector2DArraySourceName,
     render::TextureInspectorDisplay::Ldr},
    {render::TextureInspectorCubePixelType, render::TextureInspectorCubeSourceName,
     render::TextureInspectorDisplay::Ldr},
    {render::TextureInspectorCubeArrayPixelType, render::TextureInspectorCubeArraySourceName,
     render::TextureInspectorDisplay::Ldr},
    {render::TextureInspectorVolumePixelType, render::TextureInspectorVolumeSourceName,
     render::TextureInspectorDisplay::Ldr}};

void AppendColor(Array<uint8>& out, const uint8* color, usize count) noexcept {
    for (usize index = 0; index < count; ++index) {
        (void)out.TryAppend(Span<const uint8>(color, RgbaBytes));
    }
}

// 점 표본·clamp 샘플러의 표시 텍스처 desc다.
graphics::TextureDesc InspectorDesc(graphics::TextureDimension dimension, uint32 size) noexcept {
    graphics::TextureDesc desc;
    desc.dimension = dimension;
    desc.width = size;
    desc.height = size;
    desc.format = graphics::PixelFormat::RGBA8Unorm;
    desc.sampler.filter = graphics::Filter::Point;
    desc.sampler.addressU = graphics::AddressMode::Clamp;
    desc.sampler.addressV = graphics::AddressMode::Clamp;
    desc.sampler.addressW = graphics::AddressMode::Clamp;
    return desc;
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
    : allocator_(allocator), host_(host), resources_(allocator),
      captured_{Array<uint8>(allocator), Array<uint8>(allocator), Array<uint8>(allocator),
                Array<uint8>(allocator), Array<uint8>(allocator), Array<uint8>(allocator),
                Array<uint8>(allocator), Array<uint8>(allocator)} {
}

Result<void> ScreenRenderer::CreateInspectorTextures(graphics::Device& device) noexcept {
    Array<uint8> bytes(allocator_);
    auto create = [&](InspectorView view, const graphics::TextureDesc& desc) noexcept {
        auto created = device.CreateTexture(desc, bytes.View());
        bytes.Clear();
        if (!created) {
            Log::Error(LogSandboxScreen, "An inspector texture could not be created: {Error}",
                       ErrorCodeName(created.Error()));
            return Result<void>::Failure(created.Error());
        }

        inspectorTextures_[static_cast<uint32>(view)] = created.Value();
        inspectorCreated_ |= 1u << static_cast<uint32>(view);
        return Result<void>::Success();
    };

    // 2D: mip 0 바둑판(빨강·초록), mip 1 파랑, mip 2 흰색.
    graphics::TextureDesc plane = InspectorDesc(graphics::TextureDimension::Texture2D,
                                                Inspector2DSize);
    plane.mipLevels = InspectorMips[static_cast<uint32>(InspectorView::Texture2D)];
    for (uint32 y = 0; y < Inspector2DSize; ++y) {
        for (uint32 x = 0; x < Inspector2DSize; ++x) {
            AppendColor(bytes, (x + y) % 2 == 0 ? Red : Green, 1);
        }
    }

    AppendColor(bytes, Blue, (Inspector2DSize / 2) * (Inspector2DSize / 2));
    AppendColor(bytes, White, 1);
    if (auto made = create(InspectorView::Texture2D, plane); !made) {
        return made;
    }

    // HDR 2D: RGBA16Float 1×1.
    graphics::TextureDesc hdr = InspectorDesc(graphics::TextureDimension::Texture2D, 1);
    hdr.format = graphics::PixelFormat::RGBA16Float;
    const Float16 half[4] = {Float16::FromFloat(InspectorHdrColor[0]),
                             Float16::FromFloat(InspectorHdrColor[1]),
                             Float16::FromFloat(InspectorHdrColor[2]), Float16::FromFloat(1.0f)};
    (void)bytes.TryAppend(Span<const uint8>(reinterpret_cast<const uint8*>(half), sizeof(half)));
    if (auto made = create(InspectorView::Hdr2D, hdr); !made) {
        return made;
    }

    graphics::TextureDesc layers = InspectorDesc(graphics::TextureDimension::Texture2DArray, 1);
    layers.arraySize = InspectorLayers[static_cast<uint32>(InspectorView::Texture2DArray)];
    AppendColor(bytes, Red, 1);
    AppendColor(bytes, Green, 1);
    AppendColor(bytes, Blue, 1);
    if (auto made = create(InspectorView::Texture2DArray, layers); !made) {
        return made;
    }

    graphics::TextureDesc cube = InspectorDesc(graphics::TextureDimension::TextureCube, 1);
    for (const uint8* color : CubeFaceColors) {
        AppendColor(bytes, color, 1);
    }

    if (auto made = create(InspectorView::Cube, cube); !made) {
        return made;
    }

    if (device.Caps().features.textureCubeArray) {
        graphics::TextureDesc cubes =
            InspectorDesc(graphics::TextureDimension::TextureCubeArray, 1);
        cubes.arraySize = InspectorLayers[static_cast<uint32>(InspectorView::CubeArray)];
        for (const uint8* color : CubeFaceColors) {
            AppendColor(bytes, color, 1);
        }

        AppendColor(bytes, White, 6);
        if (auto made = create(InspectorView::CubeArray, cubes); !made) {
            return made;
        }
    }

    graphics::TextureDesc volume = InspectorDesc(graphics::TextureDimension::Texture3D, 1);
    volume.depth = InspectorLayers[static_cast<uint32>(InspectorView::Volume)];
    AppendColor(bytes, Red, 1);
    AppendColor(bytes, Green, 1);
    AppendColor(bytes, Blue, 1);
    AppendColor(bytes, White, 1);
    return create(InspectorView::Volume, volume);
}

void ScreenRenderer::RetireInspectorTextures(graphics::Device& device) noexcept {
    for (graphics::TextureHandle& texture : inspectorTextures_) {
        if (!texture.IsNull()) {
            device.Retire(texture);
            texture = {};
        }
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
        Log::Error(LogSandboxScreen, "The swap chain could not be created ({Width}x{Height}): {Error}",
                   width, height, ErrorCodeName(created.Error()));
        return Result<void>::Failure(created.Error());
    }

    swapChain_ = created.Value();
    surface_ = surface;
    width_ = width;
    height_ = height;
    return Result<void>::Success();
}

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
        request.state.bindings.texture = render::BuiltinMeshAlbedoName;
    }

    auto tonemap = set->FindEntry(render::TonemapPixelType.hash);
    if (!tonemap) {
        return Result<void>::Failure(tonemap.Error());
    }

    auto request = render::FullscreenPipelineRequest(*set, tonemap.Value(), OutputFormat);
    if (!request) {
        return Result<void>::Failure(request.Error());
    }

    tonemapRequest_ = request.Value();
    tonemapRequest_.state.bindings.texture = render::TonemapSceneColorName;
    tonemapRequest_.state.drawConstantBytes = sizeof(render::TonemapConstants);
    for (uint32 view = 1; view < InspectorViewCount; ++view) {
        auto entry = set->FindEntry(InspectorKinds[view].type.hash);
        if (!entry) {
            return Result<void>::Failure(entry.Error());
        }

        auto inspector = render::FullscreenPipelineRequest(*set, entry.Value(), OutputFormat);
        if (!inspector) {
            return Result<void>::Failure(inspector.Error());
        }

        inspectorRequests_[view] = inspector.Value();
        inspectorRequests_[view].state.bindings.texture = InspectorKinds[view].texture;
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
    if (auto prepared = PrepareRequests(); !prepared) {
        return prepared;
    }

    return CreateInspectorTextures(device);
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

    RetireInspectorTextures(device);
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
    if (!capture.IsNull() && slot < MaxCaptureSlots) {
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
    const uint32 view = static_cast<uint32>(packet.inspector);
    const graphics::TextureHandle texture = inspectorTextures_[view];
    render::TextureInspectorConstants constants;
    constants.exposure[0] = packet.tonemap.exposure[0];
    constants.selection[0] = static_cast<float>(packet.inspectorMip);
    constants.selection[1] = static_cast<float>(packet.inspectorLayer);
    constants.selection[2] = static_cast<float>(InspectorLayers[view]);
    constants.selection[3] = static_cast<float>(InspectorKinds[view].display);
    auto bind = [texture, constants](render::GraphPassContext& pass) noexcept {
        pass.Commands().SetTexture(texture);
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

    const uint32 view = static_cast<uint32>(packet.inspector);
    if (packet.inspector != InspectorView::None && view < InspectorViewCount &&
        !renderer.inspectorTextures_[view].IsNull()) {
        renderer.RenderInspector(context, packet);
        return;
    }

    render::BuiltinResources& builtins = renderer.resources_.Builtins();
    // 메시 파이프라인은 소유 스레드(이 자리)에서 만든다(그래프 기록 함수는 장치 생성을 하지 않는다).
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
            auto texture = builtins.ResolveTexture(context.Handles(), item.texture,
                                                   fallback.Value());
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
    (void)graph.AddPass(BasePassName, render::GraphPassKind::Raster, render::GraphPassFlags::None,
                        base, [items](render::GraphPassContext& pass) noexcept {
                            graphics::CommandContext& commands = pass.Commands();
                            for (const ResolvedItem& item : items) {
                                commands.SetPipeline(item.pipeline);
                                (void)commands.SetDrawConstants(
                                    Bytes(&item.constants, sizeof(item.constants)));
                                commands.SetTexture(item.texture);
                                const graphics::VertexBufferBinding vertices[] = {
                                    {item.mesh->vertices, 0}};
                                commands.SetVertexBuffers(0, vertices);
                                commands.SetIndexBuffer(item.mesh->indices,
                                                        item.mesh->indexFormat);
                                commands.DrawIndexed(item.mesh->indexCount);
                            }
                        });
    const render::TonemapConstants tonemap = packet.tonemap;
    auto bindTonemap = [sceneColor, tonemap](render::GraphPassContext& pass) noexcept {
        pass.Commands().SetTexture(pass.Texture(sceneColor));
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
            (void)graph.AddFullscreenPass(TonemapCaptureName, render::GraphPassFlags::None,
                                          capture, *renderer.cache_, renderer.tonemapRequest_,
                                          bindTonemap);
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
        Log::Error(LogSandboxScreen, "The swap chain could not be resized to {Width}x{Height}: {Error}",
                   value.width, value.height, ErrorCodeName(resized.Error()));
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
