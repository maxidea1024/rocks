#include <UiRender/UiLaneRenderer.h>
#include <Base/InitializerList.h>
#include <Core/Diagnostics/Log.h>
#include <RenderBase/EngineShaderHost.h>
#include <RenderBase/ShaderSet.h>
#include <RenderGraph/GraphPassAccesses.h>
#include <TraceCapture/TraceCpuScope.h>
#include <UiRender/DrawBatchRenderer.h>
#include <UiRender/UiScenePassContext.h>
#include <UiRender/UiShaderTypes.h>

namespace rocks::ui {

namespace {

constexpr LogCategory LogUiLane{"UiRender"};

// 배경 흐림 캡처 텍스처 cache 크기다(대상 크기마다 하나 — 창·보관 표면 크기 수만큼 생긴다).
constexpr usize MaxBackdropTextures = 8;

TraceScopeName UiSurfacePassName("UiSurface");
TraceScopeName UiTargetPassName("UiTarget");
TraceScopeName UiSceneClearName("UiSceneClear");
TraceScopeName UiScenePassName("UiSceneBatch");

bool IsBackdropOperation(PaintOperation operation) noexcept {
    return operation == PaintOperation::BlurBackdrop || operation == PaintOperation::BlurToBuffer;
}

graphics::ClearColor ToClear(LinearColor color) noexcept {
    return {color.r, color.g, color.b, color.a};
}

// 정점 레이아웃에는 float가 들어가므로 GPU에는 원래 비트 표현을 그대로 전달한다.
Span<const uint8> VertexBytes(Span<const DrawVertex> vertices) noexcept {
    static_assert(IsTriviallyCopyableV<DrawVertex>);
    return {reinterpret_cast<const uint8*>(vertices.Data()), vertices.ByteCount()};
}

struct ScreenTransform {
    float scaleX;
    float scaleY;
    float offsetX;
    float offsetY;
};

struct BlurConstants {
    ScreenTransform screen;
    Vec4 radii;
};

struct RoundedBoxConstants {
    ScreenTransform screen;
    Vec4 params;
    Vec4 radii;
    LinearColor outline;
};

} // namespace

struct UiLaneRenderer::Pipelines {
    graphics::PipelineHandle solid{};
    graphics::PipelineHandle image{};
    graphics::PipelineHandle mask{};
    graphics::PipelineHandle retained{};
    graphics::PipelineHandle rounded{};
    graphics::PipelineHandle blur{};
    uint32 textureSlot = graphics::MaxResourceSlots;
    uint32 backdropSlot = graphics::MaxResourceSlots;

    struct Backdrop {
        graphics::TextureHandle texture{};
        uint32 width = 0;
        uint32 height = 0;
        // 마지막으로 쓴 그래프 세대다(같은 그래프가 쓴 칸은 교체하지 않는다).
        uint64 generation = 0;
    };

    Backdrop backdrops[MaxBackdropTextures]{};
    usize nextBackdrop = 0;

    graphics::PipelineHandle ForSampling(UiTextureSampling sampling) const noexcept {
        switch (sampling) {
        case UiTextureSampling::Mask:
            return mask;
        case UiTextureSampling::Premultiplied:
            return retained;
        case UiTextureSampling::Image:
        default:
            return image;
        }
    }

    void Retire(graphics::Device& device) noexcept {
        for (graphics::PipelineHandle* pipeline :
             {&solid, &image, &mask, &retained, &rounded, &blur}) {
            if (!pipeline->IsNull()) {
                device.Retire(*pipeline);
                *pipeline = {};
            }
        }

        for (Backdrop& backdrop : backdrops) {
            if (!backdrop.texture.IsNull()) {
                device.Retire(backdrop.texture);
                backdrop = {};
            }
        }
    }
};

UiLaneRenderer::UiLaneRenderer(Allocator& allocator) noexcept
    : allocator_(allocator), windows_(allocator), results_(allocator), captures_(allocator) {
}

UiLaneRenderer::~UiLaneRenderer() noexcept = default;

bool UiLaneRenderer::IsOpen() const noexcept {
    return pipelines_.Get() != nullptr;
}

Result<void> UiLaneRenderer::Open(graphics::Device& device,
                                  render::EngineRenderResources& resources,
                                  graphics::PixelFormat format) noexcept {
    render::ShaderSet* shaders = resources.Shaders();
    if (!shaders) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }

    auto opened = Open(device, *shaders, format);
    if (opened) {
        resources_ = &resources;
    }

    return opened;
}

namespace {

// 파이프라인 handle 한 쌍(지금 것, 새 것)이다.
struct Swap {
    graphics::PipelineHandle* old;
    graphics::PipelineHandle* fresh;
};

} // namespace

Result<void> UiLaneRenderer::ReloadShaders(graphics::Device& device,
                                           render::ShaderSet& shaders) noexcept {
    if (!IsOpen()) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }

    if (&shaders == shaders_) {
        return Result<void>::Success();
    }

    // 새 파이프라인이 새 집합의 셰이더를 붙잡으므로 lease를 먼저 잡는다(Retire된 집합·Ready 아님은
    // NotReady — ShaderSetSlot 계약).
    if (!shaders.TryAcquireLease()) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }

    auto made = UniquePtr<Pipelines>::TryMake(allocator_);
    if (!made) {
        shaders.ReleaseLease();
        return Result<void>::Failure(made.Error());
    }

    auto built = BuildPipelines(device, shaders, format_, *made.Value());
    if (!built) {
        shaders.ReleaseLease();
        return built;
    }

    // 새 파이프라인이 다 만들어진 뒤에만 옛 것을 놓는다(배경 텍스처는 그대로 둔다).
    Pipelines& current = *pipelines_;
    Pipelines& next = *made.Value();
    for (auto [old, fresh] :
         {Swap{&current.solid, &next.solid}, Swap{&current.image, &next.image},
          Swap{&current.mask, &next.mask}, Swap{&current.retained, &next.retained},
          Swap{&current.rounded, &next.rounded}, Swap{&current.blur, &next.blur}}) {
        if (!old->IsNull()) {
            device.Retire(*old);
        }

        *old = *fresh;
        *fresh = {};
    }

    current.textureSlot = next.textureSlot;
    current.backdropSlot = next.backdropSlot;
    // 옛 파이프라인을 Retire한 뒤에 옛 집합의 lease를 놓는다(그 뒤 CollectRetired가 파괴한다).
    shaders_->ReleaseLease();
    shaders_ = &shaders;
    return Result<void>::Success();
}

Result<void> UiLaneRenderer::Open(graphics::Device& device, render::ShaderSet& shaders,
                                  graphics::PixelFormat format) noexcept {
    if (IsOpen()) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    // 파이프라인이 집합의 셰이더를 붙잡는 동안 집합이 파괴되지 않게 lease를 잡는다(ShaderSetSlot
    // 계약 — 교체 뒤에도 다시 만들 때까지 옛 파이프라인을 쓴다).
    if (!shaders.TryAcquireLease()) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }

    auto made = UniquePtr<Pipelines>::TryMake(allocator_);
    if (!made) {
        shaders.ReleaseLease();
        return Result<void>::Failure(made.Error());
    }

    auto built = BuildPipelines(device, shaders, format, *made.Value());
    if (!built) {
        shaders.ReleaseLease();
        return built;
    }

    auto pool = UniquePtr<render::GraphResourcePool>::TryMake(allocator_, allocator_, device);
    if (!pool) {
        made.Value()->Retire(device);
        shaders.ReleaseLease();
        return Result<void>::Failure(pool.Error());
    }

    shaders_ = &shaders;
    failedShaders_ = nullptr;
    resources_ = nullptr;
    format_ = format;
    pool_ = Move(pool.Value());
    pipelines_ = Move(made.Value());
    return Result<void>::Success();
}

Result<void> UiLaneRenderer::BuildPipelines(graphics::Device& device, render::ShaderSet& shaders,
                                            graphics::PixelFormat format,
                                            Pipelines& pipelines) noexcept {
    const auto fail = [&](ErrorCode error) noexcept {
        pipelines.Retire(device);
        return Result<void>::Failure(error);
    };
    struct Creation {
        graphics::PipelineHandle* handle;
        Result<graphics::PipelineHandle> (*create)(graphics::Device&, render::ShaderSet&,
                                                   graphics::PixelFormat) noexcept;
    };
    const Creation creations[] = {
        {&pipelines.solid, &DrawBatchRenderer::CreateSolidPipeline},
        {&pipelines.image, &DrawBatchRenderer::CreateImagePipeline},
        {&pipelines.mask, &DrawBatchRenderer::CreateMaskPipeline},
        {&pipelines.retained, &DrawBatchRenderer::CreateRetainedPipeline},
        {&pipelines.rounded, &DrawBatchRenderer::CreateRoundedBoxPipeline},
        {&pipelines.blur, &DrawBatchRenderer::CreateBlurPipeline}};
    for (const Creation& creation : creations) {
        auto created = creation.create(device, shaders, format);
        if (!created) {
            return fail(created.Error());
        }

        *creation.handle = created.Value();
    }

    // 이미지·마스크·보관 셰이더는 텍스처 하나(UiTexture)를 같은 slot에 선언해야 한다(그리기마다
    // 파이프라인이 바뀌어도 slot 하나로 묶는다 — RenderBase 9.13절).
    auto image = UiPixelTextureSlot(shaders, UiImagePixelType, UiTextureName);
    auto mask = UiPixelTextureSlot(shaders, UiMaskPixelType, UiTextureName);
    auto retained = UiPixelTextureSlot(shaders, UiRetainedPixelType, UiTextureName);
    auto blur = UiPixelTextureSlot(shaders, UiBlurPixelType, UiBackdropName);
    if (!image || !mask || !retained || !blur) {
        return fail(!image      ? image.Error()
                    : !mask     ? mask.Error()
                    : !retained ? retained.Error()
                                : blur.Error());
    }

    if (mask.Value() != image.Value() || retained.Value() != image.Value()) {
        return fail(ErrorCode::InvalidArgument);
    }

    pipelines.textureSlot = image.Value();
    pipelines.backdropSlot = blur.Value();
    return Result<void>::Success();
}

void UiLaneRenderer::Close(graphics::Device& device) noexcept {
    for (const WindowSurface& window : windows_) {
        if (!window.swapChain.IsNull()) {
            device.Retire(window.swapChain);
        }
    }

    windows_.Clear();
    if (pool_) {
        pool_->Clear();
        pool_.Reset();
    }

    if (pipelines_) {
        pipelines_->Retire(device);
        pipelines_.Reset();
    }

    if (shaders_) {
        shaders_->ReleaseLease();
    }

    shaders_ = nullptr;
    failedShaders_ = nullptr;
    resources_ = nullptr;
}

const UiLaneRenderer::Stats& UiLaneRenderer::Statistics() const noexcept {
    return stats_;
}

UiLaneRenderer::WindowSurface* UiLaneRenderer::FindWindow(UiWindowSurfaceId id) noexcept {
    for (WindowSurface& window : windows_) {
        if (window.id == id) {
            return &window;
        }
    }

    return nullptr;
}

Result<void> UiLaneRenderer::CreateWindowSurface(UiWindowSurfaceId id,
                                                 const graphics::SwapChainDesc& desc,
                                                 graphics::Device& device) noexcept {
    if (id.IsNull() || FindWindow(id)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (auto reserved = windows_.TryReserve(windows_.Size() + 1); !reserved) {
        return reserved;
    }

    auto created = device.CreateSwapChain(desc);
    if (!created) {
        return Result<void>::Failure(created.Error());
    }

    return windows_.TryPushBack({id, created.Value(), desc.width, desc.height});
}

Result<void> UiLaneRenderer::ResizeWindowSurface(UiWindowSurfaceId id, uint32 width, uint32 height,
                                                 graphics::Device& device) noexcept {
    WindowSurface* window = FindWindow(id);
    if (!window) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    if (window->width == width && window->height == height) {
        return Result<void>::Success();
    }

    if (auto resized = device.ResizeSwapChain(window->swapChain, width, height); !resized) {
        return resized;
    }

    window->width = width;
    window->height = height;
    return Result<void>::Success();
}

void UiLaneRenderer::DestroyWindowSurface(UiWindowSurfaceId id, graphics::Device& device) noexcept {
    for (usize index = 0; index < windows_.Size(); ++index) {
        if (windows_[index].id == id) {
            device.Retire(windows_[index].swapChain);
            (void)windows_.TryRemoveAtSwap(index);
            return;
        }
    }
}

graphics::SwapChainHandle UiLaneRenderer::WindowSwapChain(UiWindowSurfaceId id) const noexcept {
    for (const WindowSurface& window : windows_) {
        if (window.id == id) {
            return window.swapChain;
        }
    }

    return {};
}

Result<void> UiLaneRenderer::ReserveSurfaceResult(UiWindowSurfaceId id) noexcept {
    platform::LockGuard lock(resultGate_);
    // 작업이 아직 돌지 않았다는 표지(NotReady)로 칸을 먼저 둔다. 작업은 이 칸을 덮어쓰기만 하므로
    // 결과가 할당 실패로 사라져 실패가 성공으로 보이는 일이 없다.
    return results_.TryPushBack({id, ErrorCode::NotReady});
}

ErrorCode UiLaneRenderer::TakeSurfaceResult(UiWindowSurfaceId id) noexcept {
    platform::LockGuard lock(resultGate_);
    for (usize index = 0; index < results_.Size(); ++index) {
        if (results_[index].id == id) {
            const ErrorCode error = results_[index].error;
            (void)results_.TryRemoveAtSwap(index);
            return error;
        }
    }

    return ErrorCode::None;
}

void UiLaneRenderer::StoreSurfaceResult(UiWindowSurfaceId id, ErrorCode error) noexcept {
    platform::LockGuard lock(resultGate_);
    for (SurfaceResult& result : results_) {
        if (result.id == id) {
            result.error = error;
            return;
        }
    }

    (void)results_.TryPushBack({id, error});
}

void UiLaneRenderer::Capture(uint64 capture, render::RenderHandle texture, uint32 width,
                             uint32 height, render::RenderLaneContext& context) noexcept {
    CaptureResult result{capture, ErrorCode::None, Array<uint8>(allocator_)};
    auto device = context.Handles().Texture(texture);
    if (!device) {
        result.error = device.Error();
    } else if (auto sized = result.rgba.TryResize(static_cast<usize>(width) * height *
                                                  UiTextureBytesPerPixel(UiTextureFormat::Rgba8));
               !sized) {
        result.error = sized.Error();
    } else if (auto read = context.Device().ReadPixels(device.Value(), result.rgba.View()); !read) {
        result.error = read.Error();
    }

    StoreCapture(capture, Move(result.rgba), result.error);
}

void UiLaneRenderer::StoreCapture(uint64 capture, Array<uint8>&& rgba, ErrorCode error) noexcept {
    CaptureResult result{capture, error, Move(rgba)};
    platform::LockGuard lock(resultGate_);
    (void)captures_.TryPushBack(Move(result));
}

render::ShaderSet* UiLaneRenderer::Shaders() const noexcept {
    return shaders_;
}

Allocator& UiLaneRenderer::GetAllocator() const noexcept {
    return allocator_;
}

Result<void> UiLaneRenderer::TakeCapture(uint64 capture, Array<uint8>& rgba) noexcept {
    platform::LockGuard lock(resultGate_);
    for (usize index = 0; index < captures_.Size(); ++index) {
        if (captures_[index].id != capture) {
            continue;
        }

        const ErrorCode error = captures_[index].error;
        if (error == ErrorCode::None) {
            rgba = Move(captures_[index].rgba);
        }

        (void)captures_.TryRemoveAtSwap(index);
        return error == ErrorCode::None ? Result<void>::Success() : Result<void>::Failure(error);
    }

    return Result<void>::Failure(ErrorCode::NotReady);
}

Result<graphics::TextureHandle>
UiLaneRenderer::PrepareBackdrop(graphics::Device& device, uint32 width, uint32 height) noexcept {
    Pipelines& pipelines = *pipelines_;
    for (auto& backdrop : pipelines.backdrops) {
        if (!backdrop.texture.IsNull() && backdrop.width == width && backdrop.height == height) {
            backdrop.generation = graphGeneration_;
            return Result<graphics::TextureHandle>::Success(backdrop.texture);
        }
    }

    // 지금 그래프가 쓰지 않는 칸 중 다음 칸을 바꾼다. 모두 지금 그래프가 쓰면(크기가 다른 흐림
    // 대상이 MaxBackdropTextures를 넘음) 그 pass를 실패로 둔다 — 기록 중인 pass의 텍스처를 Retire할
    // 수 없다.
    usize victim = MaxBackdropTextures;
    for (usize step = 0; step < MaxBackdropTextures; ++step) {
        const usize index = (pipelines.nextBackdrop + step) % MaxBackdropTextures;
        if (pipelines.backdrops[index].texture.IsNull() ||
            pipelines.backdrops[index].generation != graphGeneration_) {
            victim = index;
            break;
        }
    }

    if (victim == MaxBackdropTextures) {
        return Result<graphics::TextureHandle>::Failure(ErrorCode::CapacityExceeded);
    }

    graphics::TextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.format = format_;
    desc.usage = graphics::TextureUsage::RenderTarget;
    auto created = device.CreateTexture(desc, {});
    if (!created) {
        return Result<graphics::TextureHandle>::Failure(created.Error());
    }

    // 바꾸는 칸의 옛 텍스처는 장치 Retire가 마지막 사용(앞 그래프) 뒤에 회수한다.
    auto& slot = pipelines.backdrops[victim];
    pipelines.nextBackdrop = (victim + 1) % MaxBackdropTextures;
    if (!slot.texture.IsNull()) {
        device.Retire(slot.texture);
    }

    slot = {created.Value(), width, height, graphGeneration_};
    return Result<graphics::TextureHandle>::Success(created.Value());
}

Result<void> UiLaneRenderer::AddBatchPass(render::RenderGraph& graph,
                                          render::RenderLaneContext& context,
                                          const UiFrameBatch& batch, uint32 width, uint32 height,
                                          render::GraphPassAccesses& accesses,
                                          Span<const render::RenderHandle> producedTargets,
                                          Span<const render::GraphTexture> producedTextures,
                                          TraceScopeName& name) noexcept {
    graphics::Device& device = context.Device();
    const bool draws = !batch.indices.IsEmpty() && !batch.vertices.IsEmpty();
    render::GraphBuffer vertices{};
    render::GraphBuffer indices{};
    if (draws) {
        graphics::BufferDesc desc;
        desc.usage = graphics::BufferUsage::Vertex;
        desc.size = batch.vertices.ByteCount();
        auto vertexBuffer = graph.CreateBuffer(desc, "UiVertices");
        desc.usage = graphics::BufferUsage::Index;
        desc.size = batch.indices.ByteCount();
        auto indexBuffer = graph.CreateBuffer(desc, "UiIndices");
        if (!vertexBuffer || !indexBuffer) {
            return Result<void>::Failure(!vertexBuffer ? vertexBuffer.Error()
                                                       : indexBuffer.Error());
        }

        vertices = vertexBuffer.Value();
        indices = indexBuffer.Value();
        if (auto uploaded = graph.UploadBuffer(vertices, VertexBytes(batch.vertices)); !uploaded) {
            return uploaded;
        }

        if (auto uploaded = graph.UploadBuffer(indices, AsBytes(batch.indices)); !uploaded) {
            return uploaded;
        }

        accesses.Buffer(vertices, graphics::ResourceAccess::VertexOrIndex)
            .Buffer(indices, graphics::ResourceAccess::VertexOrIndex);
    }

    // 같은 그래프의 표면 pass가 그린 타깃을 읽으면 읽기 접근을 선언한다(전이). 그 밖의 텍스처
    // (정적 이미지·post buffer)는 그래프 밖 자원이다(18.4절 pass 안 캡처 규칙).
    bool backdrop = false;
    for (usize command = 0; command < batch.commands.Size(); ++command) {
        backdrop |= IsBackdropOperation(batch.commands[command].operation);
        const render::RenderHandle handle = command < batch.textures.Size()
                                                ? batch.textures[command].handle
                                                : render::RenderHandle{};
        for (usize produced = 0; produced < producedTargets.Size(); ++produced) {
            if (!handle.IsNull() && producedTargets[produced] == handle) {
                accesses.Texture(producedTextures[produced], graphics::ResourceAccess::SrvGraphics);
            }
        }
    }

    graphics::TextureHandle backdropTexture{};
    if (backdrop) {
        auto prepared = PrepareBackdrop(device, width, height);
        if (!prepared) {
            return Result<void>::Failure(prepared.Error());
        }

        backdropTexture = prepared.Value();
    }

    if (accesses.Error() != ErrorCode::None) {
        return Result<void>::Failure(accesses.Error());
    }

    // 명령이 읽는 텍스처의 장치 handle은 기록 전에 푼다(병렬 기록 함수는 handle 표를 보지 않는다).
    // Ready가 아닌 텍스처(생성 중·실패)를 읽는 명령은 건너뛴다.
    const Pipelines& pipelines = *pipelines_;
    const ScreenTransform transform{2.0f / static_cast<float>(width),
                                    -2.0f / static_cast<float>(height), -1.0f, 1.0f};
    const Vec2 size{static_cast<float>(width), static_cast<float>(height)};
    render::RenderHandleTable& handles = context.Handles();
    Span<graphics::TextureHandle> resolved;
    if (!batch.commands.IsEmpty()) {
        auto allocated = graph.Arena().TryNewArray<graphics::TextureHandle>(batch.commands.Size());
        if (!allocated) {
            return Result<void>::Failure(allocated.Error());
        }

        resolved = allocated.Value();
        for (usize command = 0; command < batch.commands.Size(); ++command) {
            const render::RenderHandle handle = command < batch.textures.Size()
                                                    ? batch.textures[command].handle
                                                    : render::RenderHandle{};
            if (!handle.IsNull()) {
                auto texture = handles.Texture(handle);
                if (texture) {
                    resolved[command] = texture.Value();
                } else {
                    ++stats_.commandsSkipped;
                }
            }
        }
    }

    ++stats_.passesRecorded;
    auto added = graph.AddPass(
        name, render::GraphPassKind::Raster, render::GraphPassFlags::HasSideEffects, accesses,
        [batch, vertices, indices, resolved, backdropTexture, transform, size, draws,
         &pipelines](render::GraphPassContext& pass) noexcept {
            if (!draws) {
                return;
            }

            graphics::CommandContext& commands = pass.Commands();
            commands.SetViewport({0, 0, size.x, size.y, 0, 1});
            commands.SetPipeline(pipelines.solid);
            (void)commands.SetDrawConstants(graphics::DrawConstantBytes(transform));
            const graphics::VertexBufferBinding binding[] = {{pass.Buffer(vertices), 0}};
            commands.SetVertexBuffers(0, binding);
            commands.SetIndexBuffer(pass.Buffer(indices), graphics::IndexFormat::UInt32);
            graphics::PipelineHandle current = pipelines.solid;
            for (usize index = 0; index < batch.commands.Size(); ++index) {
                const DrawCommand& command = batch.commands[index];
                // 텍스처 id가 있는 명령은 텍스처로 그린다. 모르는·해제된 id(빈 handle)나 아직
                // 준비되지 않은 텍스처는 그 명령을 건너뛴다(단색으로 그리지 않는다 — 18.4절).
                const bool textured = !command.resource.IsNull();
                const graphics::TextureHandle texture = resolved[index];
                // 텍스처가 준비되지 않은 명령은 그리지 않는다(빈 텍스처를 묶지 않는다).
                if (textured && texture.IsNull()) {
                    continue;
                }

                float left = 0;
                float top = 0;
                float right = size.x;
                float bottom = size.y;
                if (command.hasClip) {
                    left = math::Clamp(math::Floor(command.clip.min.x), 0.0f, size.x);
                    top = math::Clamp(math::Floor(command.clip.min.y), 0.0f, size.y);
                    right = math::Clamp(math::Ceil(command.clip.max.x), 0.0f, size.x);
                    bottom = math::Clamp(math::Ceil(command.clip.max.y), 0.0f, size.y);
                }

                if (right <= left || bottom <= top) {
                    continue;
                }

                commands.SetScissor({static_cast<uint32>(left), static_cast<uint32>(top),
                                     static_cast<uint32>(right - left),
                                     static_cast<uint32>(bottom - top)});
                if (command.operation == PaintOperation::CopyBackdrop ||
                    IsBackdropOperation(command.operation)) {
                    if ((command.operation == PaintOperation::CopyBackdrop ||
                         command.operation == PaintOperation::BlurToBuffer) &&
                        texture.IsNull()) {
                        continue;
                    }

                    // 복사 대상 텍스처를 읽기 slot에서 내린다(흰 텍스처를 묶는다).
                    commands.SetTexture(graphics::ShaderStage::Pixel, pipelines.backdropSlot, {});
                    auto copied = commands.CopyPassTarget(
                        command.operation == PaintOperation::CopyBackdrop ? texture
                                                                          : backdropTexture);
                    if (!copied || command.operation == PaintOperation::CopyBackdrop) {
                        continue;
                    }
                }

                const graphics::PipelineHandle next =
                    IsBackdropOperation(command.operation) ? pipelines.blur
                    : command.roundedBox                   ? pipelines.rounded
                    : textured ? pipelines.ForSampling(batch.textures[index].sampling)
                               : pipelines.solid;
                if (current != next) {
                    commands.SetPipeline(next);
                    current = next;
                }

                if (command.roundedBox) {
                    const RoundedBoxConstants rounded{
                        transform,
                        {0.0f, command.outlineWidth, command.roundedSize.x, command.roundedSize.y},
                        command.cornerRadius,
                        command.outlineColor};
                    (void)commands.SetDrawConstants(graphics::DrawConstantBytes(rounded));
                } else if (command.operation == PaintOperation::Draw) {
                    // 다른 크기의 상수(흐림·둥근 상자) 뒤에는 화면 변환만 다시 넣는다.
                    (void)commands.SetDrawConstants(graphics::DrawConstantBytes(transform));
                }

                if (IsBackdropOperation(command.operation)) {
                    const BlurConstants blur{transform, command.cornerRadius};
                    (void)commands.SetDrawConstants(graphics::DrawConstantBytes(blur));
                    commands.SetTexture(graphics::ShaderStage::Pixel, pipelines.backdropSlot,
                                        backdropTexture);
                } else if (textured) {
                    commands.SetTexture(graphics::ShaderStage::Pixel, pipelines.textureSlot,
                                        texture);
                }

                commands.DrawIndexed(command.indexCount, 1, command.firstIndex);
                if (command.operation == PaintOperation::BlurToBuffer) {
                    // 같은 pass에서 필터 결과를 복사한 뒤 바꾼 픽셀만 원본으로 되돌린다. 뒤에
                    // 그리는 위젯과 다음 후처리 항목은 처음 화면에서 출발한다.
                    (void)commands.CopyPassTarget(texture);
                    const BlurConstants restore{transform, {-1, -1, -1, -1}};
                    (void)commands.SetDrawConstants(graphics::DrawConstantBytes(restore));
                    commands.DrawIndexed(command.indexCount, 1, command.firstIndex);
                }
            }
        });
    if (!added) {
        return Result<void>::Failure(added.Error());
    }

    return Result<void>::Success();
}

Result<void> UiLaneRenderer::AddPasses(render::RenderGraph& graph,
                                       render::RenderLaneContext& context, UiFrameTarget& target,
                                       render::GraphLoadAction windowLoad) noexcept {
    ++graphGeneration_;
    if (!IsOpen()) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }

    if (target.width == 0 || target.height == 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 이 대상의 표면 pass가 그린 타깃(같은 그래프에 한 번만 import)이다.
    auto producedTargets = graph.Arena().TryNewArray<render::RenderHandle>(target.passes.Size());
    auto producedTextures = graph.Arena().TryNewArray<render::GraphTexture>(target.passes.Size());
    if (!producedTargets || !producedTextures) {
        return Result<void>::Failure(!producedTargets ? producedTargets.Error()
                                                      : producedTextures.Error());
    }

    usize producedCount = 0;
    for (UiFramePass& pass : target.passes) {
        render::GraphTexture texture{};
        bool imported = false;
        for (usize index = 0; index < producedCount; ++index) {
            if (producedTargets.Value()[index] == pass.target) {
                texture = producedTextures.Value()[index];
                imported = true;
            }
        }

        if (!imported) {
            auto made = graph.ImportTexture(pass.target, "UiSurface");
            if (!made) {
                // 타깃이 아직 준비되지 않았다(생성 중·실패). 그 표면만 건너뛰고 읽는 명령도
                // 건너뛴다.
                continue;
            }

            texture = made.Value();
            producedTargets.Value()[producedCount] = pass.target;
            producedTextures.Value()[producedCount] = texture;
            ++producedCount;
        }

        const auto earlier = producedTargets.Value().Slice(0, producedCount - (imported ? 0 : 1));
        const auto earlierTextures =
            producedTextures.Value().Slice(0, producedCount - (imported ? 0 : 1));
        if (pass.kind == UiSurfacePassKind::Retained) {
            render::GraphPassAccesses accesses(graph);
            accesses.RenderTarget(texture, render::GraphLoadAction::Clear, ToClear(pass.clear));
            if (auto added = AddBatchPass(graph, context, pass.content, pass.width, pass.height,
                                          accesses, earlier, earlierTextures, UiSurfacePassName);
                !added) {
                return added;
            }

            continue;
        }

        // 장면 기록은 이번 프레임의 현재 집합으로 자기 파이프라인을 만든다(UI 재빌드에 실패해 옛
        // 집합을 붙잡고 있어도 Retire된 집합으로 새 파이프라인을 만들지 않게).
        render::ShaderSet* current = resources_ ? resources_->Shaders() : nullptr;
        UiScenePassContext scene(graph, texture, pass.width, pass.height, pass.clear, context,
                                 *this, current ? *current : *shaders_);
        for (const UiSceneRecord& record : pass.scenes) {
            record.Record(scene);
        }

        if (!scene.TargetWritten()) {
            render::GraphPassAccesses accesses(graph);
            accesses.RenderTarget(texture, render::GraphLoadAction::Clear, ToClear(pass.clear));
            auto added = graph.AddPass(UiSceneClearName, render::GraphPassKind::Raster,
                                       render::GraphPassFlags::HasSideEffects, accesses,
                                       [](render::GraphPassContext&) noexcept {});
            if (!added) {
                return Result<void>::Failure(added.Error());
            }
        }
    }

    render::GraphPassAccesses accesses(graph);
    if (!target.window.IsNull()) {
        WindowSurface* window = FindWindow(target.window);
        if (!window) {
            return Result<void>::Failure(ErrorCode::NotFound);
        }

        // 게임 lane의 크기 변경 작업이 실패했거나 아직 반영되지 않았으면 여기서 맞춘다(viewport·
        // CopyPassTarget이 스왑 체인 크기와 같아야 한다). 그래도 실패하면 이 대상만 실패한다.
        // Load(앞 pass가 그린 백버퍼에 이어 그림)면 앞 pass가 이미 옛 크기로 그렸으므로 여기서
        // 바꾸지 않고 거부한다 — 호출자가 장면 pass를 선언하기 전에 크기를 맞춘다.
        if (window->width != target.width || window->height != target.height) {
            if (windowLoad == render::GraphLoadAction::Load) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }

            if (auto resized = ResizeWindowSurface(target.window, target.width, target.height,
                                                   context.Device());
                !resized) {
                return resized;
            }
        }

        accesses.SwapChainTarget(window->swapChain, windowLoad, ToClear(target.clear));
    } else {
        auto made = graph.ImportTexture(target.renderTarget, "UiOffscreen");
        if (!made) {
            return Result<void>::Failure(made.Error());
        }

        accesses.RenderTarget(made.Value(), render::GraphLoadAction::Clear, ToClear(target.clear));
    }

    return AddBatchPass(graph, context, target.batch, target.width, target.height, accesses,
                        producedTargets.Value().Slice(0, producedCount),
                        producedTextures.Value().Slice(0, producedCount), UiTargetPassName);
}

Result<void> UiLaneRenderer::AddBatchToScene(UiScenePassContext& scene,
                                             const UiFrameBatch& batch) noexcept {
    if (!IsOpen()) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }

    render::GraphPassAccesses accesses(scene.Graph());
    accesses.RenderTarget(scene.Target(), scene.TargetLoad(), ToClear(scene.Clear()));
    auto added = AddBatchPass(scene.Graph(), scene.Lane(), batch, scene.Width(), scene.Height(),
                              accesses, {}, {}, UiScenePassName);
    if (added) {
        scene.MarkTargetWritten();
    }

    return added;
}

void UiLaneRenderer::Render(render::RenderFrameContext& context, UiFramePacket& packet) noexcept {
    if (!IsOpen()) {
        context.ReportFailure(ErrorCode::NotReady);
        return;
    }

    // 감시 재컴파일의 셰이더 교체를 프레임 경계에서 받는다(ShaderSystem 16.3절 SSD76). UI
    // 파이프라인은 캐시를 거치지 않고 장치에 직접 만들므로 캐시는 넘기지 않는다.
    // 다시 만들기에 실패하면 옛 파이프라인(lease로 붙잡은 옛 집합)으로 계속 그리고, 같은 집합은
    // 다음 교체까지 다시 시도하지 않는다(계획 14.1절 "hot reload 실패 시 이전 검증 버전 유지").
    if (resources_) {
        if (resources_->ApplyShaderUpdates(nullptr)) {
            failedShaders_ = nullptr;
        }

        render::ShaderSet* current = resources_->Shaders();
        if (current && current != shaders_ && current != failedShaders_) {
            if (auto reloaded = ReloadShaders(context.Device(), *current); !reloaded) {
                failedShaders_ = current;
                Log::Error(LogUiLane,
                           "The UI pipelines could not be rebuilt after a shader swap; the "
                           "previous shaders stay in use: {Error}",
                           ErrorCodeName(reloaded.Error()));
            }
        }
    }

    for (UiFrameTarget& target : packet.Targets()) {
        // 대상마다 그래프 하나다: 한 대상의 실패가 다른 대상(창)을 멈추지 않는다(18.2절).
        render::RenderGraph graph(render::MakeRenderGraphDesc(context, *pool_));
        auto executed = AddPasses(graph, context, target);
        if (executed) {
            executed = graph.Execute();
        }

        if (!executed) {
            ++stats_.targetsFailed;
            Log::Warning(LogUiLane, "A UI draw target failed: {Error} ({Detail})",
                         ErrorCodeName(executed.Error()), graph.LastError());
            continue;
        }

        ++stats_.graphsExecuted;
    }
}

} // namespace rocks::ui
