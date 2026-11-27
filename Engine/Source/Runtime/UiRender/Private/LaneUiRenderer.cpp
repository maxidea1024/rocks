#include <UiRender/LaneUiRenderer.h>
#include <UiRender/UiLaneRenderer.h>

namespace rocks::ui::detail {

// 창 표면·텍스처·화면 읽기 lane 작업 값이다. renderer는 렌더 lane 쪽 객체다(게임 주소가 아니다).
struct UiSurfaceCreateTask {
    UiLaneRenderer* renderer = nullptr;
    UiWindowSurfaceId id{};
    graphics::SwapChainDesc desc{};
};

struct UiSurfaceResizeTask {
    UiLaneRenderer* renderer = nullptr;
    UiWindowSurfaceId id{};
    uint32 width = 0;
    uint32 height = 0;
};

struct UiSurfaceDestroyTask {
    UiLaneRenderer* renderer = nullptr;
    UiWindowSurfaceId id{};
};

struct UiTextureCreateTask {
    render::RenderHandle handle{};
    graphics::TextureDesc desc{};
    render::RenderBlob pixels;
};

struct UiCaptureTask {
    UiLaneRenderer* renderer = nullptr;
    uint64 capture = 0;
    render::RenderHandle texture{};
    uint32 width = 0;
    uint32 height = 0;
};

} // namespace rocks::ui::detail

ROCKS_RENDER_TRANSFERABLE(rocks::ui::detail::UiSurfaceCreateTask);
ROCKS_RENDER_TRANSFERABLE(rocks::ui::detail::UiSurfaceResizeTask);
ROCKS_RENDER_TRANSFERABLE(rocks::ui::detail::UiSurfaceDestroyTask);
ROCKS_RENDER_TRANSFERABLE(rocks::ui::detail::UiTextureCreateTask);
ROCKS_RENDER_TRANSFERABLE(rocks::ui::detail::UiCaptureTask);

namespace rocks::ui {

namespace {

using detail::UiCaptureTask;
using detail::UiSurfaceCreateTask;
using detail::UiSurfaceDestroyTask;
using detail::UiSurfaceResizeTask;
using detail::UiTextureCreateTask;

graphics::PixelFormat ToPixelFormat(UiTextureFormat format) noexcept {
    switch (format) {
    case UiTextureFormat::Bgra8Srgb:
        return graphics::PixelFormat::BGRA8UnormSrgb;
    case UiTextureFormat::Rgba8:
        return graphics::PixelFormat::RGBA8Unorm;
    case UiTextureFormat::R8:
        return graphics::PixelFormat::R8Unorm;
    case UiTextureFormat::Rgba8Srgb:
    default:
        return graphics::PixelFormat::RGBA8UnormSrgb;
    }
}

usize PixelBytes(const UiTextureDesc& desc) noexcept {
    return static_cast<usize>(desc.width) * desc.height * UiTextureBytesPerPixel(desc.format);
}

void CreateSurfaceOnLane(render::RenderLaneContext& context, UiSurfaceCreateTask& task) noexcept {
    auto created = task.renderer->CreateWindowSurface(task.id, task.desc, context.Device());
    task.renderer->StoreSurfaceResult(task.id, created ? ErrorCode::None : created.Error());
}

void ResizeSurfaceOnLane(render::RenderLaneContext& context, UiSurfaceResizeTask& task) noexcept {
    // 실패는 다음 그리기가 그 창을 건너뛰게 한다(창 표면 크기가 창과 맞지 않음).
    (void)task.renderer->ResizeWindowSurface(task.id, task.width, task.height, context.Device());
}

void DestroySurfaceOnLane(render::RenderLaneContext& context, UiSurfaceDestroyTask& task) noexcept {
    task.renderer->DestroyWindowSurface(task.id, context.Device());
}

void CreateTextureOnLane(render::RenderLaneContext& context, UiTextureCreateTask& task) noexcept {
    auto created = context.Device().CreateTexture(task.desc, task.pixels.View());
    render::RenderNotice notice;
    notice.handle = task.handle;
    if (!created) {
        (void)context.Handles().MarkFailed(task.handle, created.Error());
        notice.kind = render::RenderNoticeKind::ResourceFailed;
        notice.error = created.Error();
    } else if (!context.Handles().BindTexture(task.handle, created.Value())) {
        // 그사이 Retire된 handle이다(만든 장치 자원을 바로 돌려준다).
        context.Device().Retire(created.Value());
        return;
    } else {
        notice.kind = render::RenderNoticeKind::ResourceReady;
    }

    context.Notify(notice);
}

void CaptureOnLane(render::RenderLaneContext& context, UiCaptureTask& task) noexcept {
    task.renderer->Capture(task.capture, task.texture, task.width, task.height, context);
}

void RenderUiFrame(render::RenderFrameContext& context, UiFramePacket& packet) noexcept {
    if (packet.Renderer()) {
        packet.Renderer()->Render(context, packet);
    }

    // slot이 반환되기 전(이 함수 안)에 arena 안의 장면 packet을 부순다(ReleaseScenes 주석).
    packet.ReleaseScenes();
}

template <class T>
Result<Span<const T>> CopySpan(FrameArena& arena, Span<const T> source) noexcept {
    auto copied = arena.TryNewArray<T>(source.Size());
    if (!copied) {
        return Result<Span<const T>>::Failure(copied.Error());
    }

    for (usize index = 0; index < source.Size(); ++index) {
        copied.Value()[index] = source[index];
    }

    return Result<Span<const T>>::Success(Span<const T>(copied.Value().Data(), source.Size()));
}

} // namespace

LaneUiRenderer::LaneUiRenderer(Allocator& allocator, render::RenderLane& lane,
                               UiLaneRenderer& laneRenderer) noexcept
    : UiRenderer(allocator), allocator_(allocator), lane_(lane), laneRenderer_(laneRenderer),
      textures_(allocator), windows_(allocator), captures_(allocator) {
}

LaneUiRenderer::~LaneUiRenderer() noexcept {
    for (usize index = 0; index < textures_.SlotCount(); ++index) {
        if (const TextureEntry* entry = textures_.FindAt(index)) {
            (void)lane_.Retire(entry->handle);
        }
    }
}

Result<UiWindowSurfaceId>
LaneUiRenderer::CreateWindowSurface(const UiWindowSurfaceDesc& desc) noexcept {
    if (desc.width == 0 || desc.height == 0 || !desc.nativeWindow) {
        return Result<UiWindowSurfaceId>::Failure(ErrorCode::InvalidArgument);
    }

    auto added = windows_.TryAdd({desc.width, desc.height});
    if (!added) {
        return Result<UiWindowSurfaceId>::Failure(added.Error());
    }

    const UiWindowSurfaceId id{added.Value()};
    UiSurfaceCreateTask task;
    task.renderer = &laneRenderer_;
    task.id = id;
    task.desc.surface = {desc.nativeWindow, desc.nativeInstance};
    task.desc.width = desc.width;
    task.desc.height = desc.height;
    task.desc.vsync = desc.vsync;
    if (auto reserved = laneRenderer_.ReserveSurfaceResult(id); !reserved) {
        (void)windows_.TryRemove(id.handle);
        return Result<UiWindowSurfaceId>::Failure(reserved.Error());
    }

    auto posted = lane_.PostSurfaceAndWait(Move(task), &CreateSurfaceOnLane);
    const ErrorCode result = laneRenderer_.TakeSurfaceResult(id);
    const ErrorCode error = !posted ? posted.Error() : result;
    if (error != ErrorCode::None) {
        // 기다림이 실패했어도(대기 상한) 작업이 나중에 돌아 스왑 체인을 만들 수 있다. 파괴 작업을
        // 뒤에 게시해 lane 쪽에 남지 않게 한다(모르는 id의 파괴는 아무것도 하지 않는다).
        if (!posted) {
            (void)lane_.PostSurface(UiSurfaceDestroyTask{&laneRenderer_, id},
                                    &DestroySurfaceOnLane);
        }

        (void)windows_.TryRemove(id.handle);
        return Result<UiWindowSurfaceId>::Failure(error);
    }

    return Result<UiWindowSurfaceId>::Success(id);
}

Result<void> LaneUiRenderer::ResizeWindowSurface(UiWindowSurfaceId surface, uint32 width,
                                                 uint32 height) noexcept {
    WindowEntry* entry = windows_.Find(surface.handle);
    if (!entry) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    if (width == 0 || height == 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (entry->width == width && entry->height == height) {
        return Result<void>::Success();
    }

    auto posted = lane_.PostSurface(UiSurfaceResizeTask{&laneRenderer_, surface, width, height},
                                    &ResizeSurfaceOnLane);
    if (!posted) {
        return posted;
    }

    entry->width = width;
    entry->height = height;
    return Result<void>::Success();
}

Result<void> LaneUiRenderer::DestroyWindowSurface(UiWindowSurfaceId surface) noexcept {
    if (!windows_.IsValid(surface.handle)) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    // 끝날 때까지 기다린다: 그 창의 프레임이 모두 처리된 뒤 스왑 체인을 놓으므로, 돌아온 뒤 native
    // 창을 부숴도 된다(UE OnWindowDestroyed와 같은 순서).
    auto posted = lane_.PostSurfaceAndWait(UiSurfaceDestroyTask{&laneRenderer_, surface},
                                           &DestroySurfaceOnLane);
    (void)windows_.TryRemove(surface.handle);
    return posted;
}

Result<render::RenderHandle>
LaneUiRenderer::CreateDeviceTexture(const UiTextureDesc& desc, Span<const uint8> pixels) noexcept {
    auto reserved = lane_.ReserveHandle(render::RenderResourceKind::Texture);
    if (!reserved) {
        return reserved;
    }

    UiTextureCreateTask task;
    task.handle = reserved.Value();
    task.desc.width = desc.width;
    task.desc.height = desc.height;
    task.desc.format = ToPixelFormat(desc.format);
    task.desc.usage = desc.usage == UiTextureUsage::RenderTarget
                          ? graphics::TextureUsage::RenderTarget
                          : graphics::TextureUsage::Sampled;
    if (desc.usage != UiTextureUsage::RenderTarget) {
        // 샘플 전용 텍스처는 초기 픽셀이 필요하다(비었으면 0으로 채운다).
        auto blob = pixels.IsEmpty() ? render::RenderBlob::TryAllocate(allocator_, PixelBytes(desc))
                                     : render::RenderBlob::TryCopy(allocator_, pixels);
        if (!blob) {
            (void)lane_.Retire(reserved.Value());
            return Result<render::RenderHandle>::Failure(blob.Error());
        }

        task.pixels = Move(blob.Value());
    }

    if (auto posted = lane_.PostResource(Move(task), &CreateTextureOnLane); !posted) {
        (void)lane_.Retire(reserved.Value());
        return Result<render::RenderHandle>::Failure(posted.Error());
    }

    return reserved;
}

Result<UiTextureId> LaneUiRenderer::CreateTexture(const UiTextureDesc& desc,
                                                  Span<const uint8> pixels) noexcept {
    if (desc.width == 0 || desc.height == 0 ||
        (!pixels.IsEmpty() && pixels.Size() != PixelBytes(desc)) ||
        (desc.usage == UiTextureUsage::RenderTarget &&
         (!pixels.IsEmpty() || desc.format != UiTextureFormat::Rgba8Srgb))) {
        return Result<UiTextureId>::Failure(ErrorCode::InvalidArgument);
    }

    auto handle = CreateDeviceTexture(desc, pixels);
    if (!handle) {
        return Result<UiTextureId>::Failure(handle.Error());
    }

    auto added = textures_.TryAdd({handle.Value(), desc});
    if (!added) {
        (void)lane_.Retire(handle.Value());
    }

    return added;
}

Result<void> LaneUiRenderer::UpdateTexture(UiTextureId texture, Span<const uint8> pixels) noexcept {
    const TextureEntry* entry = textures_.Find(texture);
    if (!entry) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    if (entry->desc.usage != UiTextureUsage::Dynamic || pixels.Size() != PixelBytes(entry->desc)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto blob = render::RenderBlob::TryCopy(allocator_, pixels);
    if (!blob) {
        return Result<void>::Failure(blob.Error());
    }

    return lane_.UpdateTexture(entry->handle, 0, Move(blob.Value()));
}

Result<void> LaneUiRenderer::ResizeRenderTarget(UiTextureId texture, uint32 width,
                                                uint32 height) noexcept {
    TextureEntry* entry = textures_.Find(texture);
    if (!entry) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    if (entry->desc.usage != UiTextureUsage::RenderTarget || width == 0 || height == 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (entry->desc.width == width && entry->desc.height == height) {
        return Result<void>::Success();
    }

    UiTextureDesc desc = entry->desc;
    desc.width = width;
    desc.height = height;
    auto handle = CreateDeviceTexture(desc, {});
    if (!handle) {
        return Result<void>::Failure(handle.Error());
    }

    // 이미 넘긴 프레임은 옛 handle로 기록되고, Retire는 그 프레임 뒤에 처리된다.
    (void)lane_.Retire(entry->handle);
    entry->handle = handle.Value();
    entry->desc = desc;
    return Result<void>::Success();
}

void LaneUiRenderer::ReleaseTexture(UiTextureId texture) noexcept {
    if (const TextureEntry* entry = textures_.Find(texture)) {
        (void)lane_.Retire(entry->handle);
        (void)textures_.TryRemove(texture);
    }
}

Result<UiTextureDesc> LaneUiRenderer::DescribeTexture(UiTextureId texture) const noexcept {
    const TextureEntry* entry = textures_.Find(texture);
    if (!entry) {
        return Result<UiTextureDesc>::Failure(ErrorCode::NotFound);
    }

    return Result<UiTextureDesc>::Success(entry->desc);
}

render::RenderHandle LaneUiRenderer::HandleOf(UiTextureId texture) const noexcept {
    const TextureEntry* entry = textures_.Find(texture);
    return entry ? entry->handle : render::RenderHandle{};
}

Result<UiFrameBatch> LaneUiRenderer::CopyBatch(FrameArena& arena, const DrawBatch& batch) noexcept {
    UiFrameBatch copy;
    auto vertices = CopySpan(arena, batch.Vertices());
    auto indices = CopySpan(arena, batch.Indices());
    auto commands = CopySpan(arena, batch.Commands());
    auto textures = arena.TryNewArray<UiFrameTexture>(batch.Commands().Size());
    if (!vertices || !indices || !commands || !textures) {
        return Result<UiFrameBatch>::Failure(!vertices   ? vertices.Error()
                                             : !indices  ? indices.Error()
                                             : !commands ? commands.Error()
                                                         : textures.Error());
    }

    for (usize index = 0; index < batch.Commands().Size(); ++index) {
        const UiTextureId resource = batch.Commands()[index].resource;
        if (const TextureEntry* entry = textures_.Find(resource)) {
            textures.Value()[index] = {entry->handle, entry->desc.sampling};
        }
    }

    copy.vertices = vertices.Value();
    copy.indices = indices.Value();
    copy.commands = commands.Value();
    copy.textures = Span<const UiFrameTexture>(textures.Value().Data(), textures.Value().Size());
    return Result<UiFrameBatch>::Success(copy);
}

Result<UiFramePacket> LaneUiRenderer::RecordWindows(FrameArena& arena,
                                                    UiDrawBuffer& buffer) noexcept {
    using ResultType = Result<UiFramePacket>;
    const Span<const UiDrawTarget> sources = buffer.Targets();
    auto targets = arena.TryNewArray<UiFrameTarget>(sources.Size());
    if (!targets) {
        return ResultType::Failure(targets.Error());
    }

    // packet을 먼저 만들어 두면 중간에 실패해도 이미 옮긴 장면 packet을 소멸자가 부순다.
    UiFramePacket packet(&laneRenderer_, targets.Value());
    for (usize index = 0; index < sources.Size(); ++index) {
        const UiDrawTarget& source = sources[index];
        UiFrameTarget& target = targets.Value()[index];
        if (!source.window.IsNull() && !windows_.IsValid(source.window.handle)) {
            return ResultType::Failure(ErrorCode::NotFound);
        }

        target.window = source.window;
        if (source.window.IsNull()) {
            const TextureEntry* entry = textures_.Find(source.renderTarget);
            if (!entry) {
                return ResultType::Failure(ErrorCode::NotFound);
            }

            // viewport·CopyPassTarget·화면 읽기가 모두 타깃 크기를 쓰므로 대상 크기가 같아야 한다.
            if (entry->desc.usage != UiTextureUsage::RenderTarget ||
                entry->desc.width != source.width || entry->desc.height != source.height) {
                return ResultType::Failure(ErrorCode::InvalidArgument);
            }

            target.renderTarget = entry->handle;
        }

        target.width = source.width;
        target.height = source.height;
        target.clear = source.clear;
        auto batch = CopyBatch(arena, *source.batch);
        auto passes = arena.TryNewArray<UiFramePass>(source.passCount);
        if (!batch || !passes) {
            return ResultType::Failure(!batch ? batch.Error() : passes.Error());
        }

        target.batch = batch.Value();
        target.passes = passes.Value();
        for (usize offset = 0; offset < source.passCount; ++offset) {
            const UiSurfacePass& from = buffer.Passes()[source.firstPass + offset];
            UiFramePass& to = passes.Value()[offset];
            to.kind = from.kind;
            // 모르는 타깃은 빈 handle로 남긴다(lane이 그 표면을 건너뛴다).
            to.target = HandleOf(from.target);
            to.width = from.width;
            to.height = from.height;
            to.clear = from.clear;
            if (from.kind == UiSurfacePassKind::Retained) {
                auto content = CopyBatch(arena, *from.content);
                if (!content) {
                    return ResultType::Failure(content.Error());
                }

                to.content = content.Value();
                continue;
            }

            auto scenes = arena.TryNewArray<UiSceneRecord>(from.packetCount);
            if (!scenes) {
                return ResultType::Failure(scenes.Error());
            }

            to.scenes = scenes.Value();
            for (usize scene = 0; scene < from.packetCount; ++scene) {
                UiScenePacket& source = buffer.Packets()[from.firstPacket + scene];
                auto storage = arena.TryAllocate(source.Size(), source.Alignment());
                if (!storage) {
                    return ResultType::Failure(storage.Error());
                }

                scenes.Value()[scene] = source.MoveTo(storage.Value());
            }
        }
    }

    return ResultType::Success(Move(packet));
}

Result<void> LaneUiRenderer::DrawWindows(UiDrawBuffer& buffer) noexcept {
    if (buffer.Targets().IsEmpty()) {
        return Result<void>::Success();
    }

    auto ticket = lane_.BeginFrame();
    if (!ticket) {
        return Result<void>::Failure(ticket.Error());
    }

    auto packet = RecordWindows(lane_.Arena(ticket.Value()), buffer);
    if (!packet) {
        lane_.AbandonFrame(Move(ticket.Value()));
        return Result<void>::Failure(packet.Error());
    }

    auto submitted = lane_.SubmitFrame(Move(ticket.Value()), Move(packet.Value()), &RenderUiFrame);
    if (!submitted) {
        if (ticket.Value().IsValid()) {
            lane_.AbandonFrame(Move(ticket.Value()));
        }

        return submitted;
    }

    ++framesSubmitted_;
    return Result<void>::Success();
}

Result<uint64> LaneUiRenderer::RequestCapture(UiTextureId target) noexcept {
    const TextureEntry* entry = textures_.Find(target);
    if (!entry) {
        return Result<uint64>::Failure(ErrorCode::NotFound);
    }

    if (entry->desc.usage != UiTextureUsage::RenderTarget) {
        return Result<uint64>::Failure(ErrorCode::InvalidArgument);
    }

    const uint64 capture = ++nextCapture_;
    if (auto tracked = captures_.TryPushBack(capture); !tracked) {
        return Result<uint64>::Failure(tracked.Error());
    }

    auto posted = lane_.PostResource(UiCaptureTask{&laneRenderer_, capture, entry->handle,
                                                   entry->desc.width, entry->desc.height},
                                     &CaptureOnLane);
    if (!posted) {
        (void)captures_.TryRemoveAt(captures_.Size() - 1);
        return Result<uint64>::Failure(posted.Error());
    }

    return Result<uint64>::Success(capture);
}

Result<void> LaneUiRenderer::WaitCapture(uint64) noexcept {
    // 읽기 작업은 lane 작업 순서대로 처리되므로 지금 게시한 표지에 닿으면 결과가 보관되어 있다.
    return Flush();
}

Result<void> LaneUiRenderer::Flush() noexcept {
    auto marker = lane_.PostMarker();
    if (!marker) {
        return Result<void>::Failure(marker.Error());
    }

    return lane_.WaitMarker(marker.Value());
}

Result<void> LaneUiRenderer::TakeCapture(uint64 capture, Array<uint8>& rgba) noexcept {
    for (usize index = 0; index < captures_.Size(); ++index) {
        if (captures_[index] != capture) {
            continue;
        }

        auto taken = laneRenderer_.TakeCapture(capture, rgba);
        if (taken || taken.Error() != ErrorCode::NotReady) {
            (void)captures_.TryRemoveAtSwap(index);
        }

        return taken;
    }

    return Result<void>::Failure(ErrorCode::NotFound);
}

usize LaneUiRenderer::LiveTextures() const noexcept {
    return textures_.Size();
}

usize LaneUiRenderer::LiveWindowSurfaces() const noexcept {
    return windows_.Size();
}

uint64 LaneUiRenderer::FramesSubmitted() const noexcept {
    return framesSubmitted_;
}

} // namespace rocks::ui
