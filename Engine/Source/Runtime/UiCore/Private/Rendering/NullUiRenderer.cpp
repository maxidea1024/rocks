#include <UiCore/Rendering/NullUiRenderer.h>

namespace rocks::ui {

namespace {

// 캡처 결과는 RGBA8이다.
constexpr usize NullCaptureBytesPerPixel = 4;

bool PixelsMatch(const UiTextureDesc& desc, Span<const uint8> pixels) noexcept {
    const uint64 expected =
        static_cast<uint64>(desc.width) * desc.height * UiTextureBytesPerPixel(desc.format);
    return pixels.Size() == expected;
}

} // namespace

NullUiRenderer::NullUiRenderer(Allocator& allocator) noexcept
    : UiRenderer(allocator), textures_(allocator), windows_(allocator), captures_(allocator) {
}

Result<UiWindowSurfaceId>
NullUiRenderer::CreateWindowSurface(const UiWindowSurfaceDesc& desc) noexcept {
    if (desc.width == 0 || desc.height == 0) {
        return Result<UiWindowSurfaceId>::Failure(ErrorCode::InvalidArgument);
    }

    auto added = windows_.TryAdd(desc);
    if (!added) {
        return Result<UiWindowSurfaceId>::Failure(added.Error());
    }

    return Result<UiWindowSurfaceId>::Success(UiWindowSurfaceId{added.Value()});
}

Result<void> NullUiRenderer::ResizeWindowSurface(UiWindowSurfaceId surface, uint32 width,
                                                 uint32 height) noexcept {
    auto* found = windows_.Find(surface.handle);
    if (!found) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    if (width == 0 || height == 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    found->width = width;
    found->height = height;
    return Result<void>::Success();
}

Result<void> NullUiRenderer::DestroyWindowSurface(UiWindowSurfaceId surface) noexcept {
    return windows_.TryRemove(surface.handle);
}

Result<UiTextureId> NullUiRenderer::CreateTexture(const UiTextureDesc& desc,
                                                  Span<const uint8> pixels) noexcept {
    if (desc.width == 0 || desc.height == 0 || (!pixels.IsEmpty() && !PixelsMatch(desc, pixels)) ||
        (desc.usage == UiTextureUsage::RenderTarget &&
         (!pixels.IsEmpty() || desc.format != UiTextureFormat::Rgba8Srgb))) {
        return Result<UiTextureId>::Failure(ErrorCode::InvalidArgument);
    }

    return textures_.TryAdd(desc);
}

Result<void> NullUiRenderer::UpdateTexture(UiTextureId texture, Span<const uint8> pixels) noexcept {
    const auto* found = textures_.Find(texture);
    if (!found) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    if (found->usage != UiTextureUsage::Dynamic || !PixelsMatch(*found, pixels)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    ++stats_.textureUpdates;
    return Result<void>::Success();
}

Result<void> NullUiRenderer::ResizeRenderTarget(UiTextureId texture, uint32 width,
                                                uint32 height) noexcept {
    auto* found = textures_.Find(texture);
    if (!found) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    if (found->usage != UiTextureUsage::RenderTarget || width == 0 || height == 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    found->width = width;
    found->height = height;
    return Result<void>::Success();
}

void NullUiRenderer::ReleaseTexture(UiTextureId texture) noexcept {
    (void)textures_.TryRemove(texture);
}

Result<UiTextureDesc> NullUiRenderer::DescribeTexture(UiTextureId texture) const noexcept {
    const auto* found = textures_.Find(texture);
    if (!found) {
        return Result<UiTextureDesc>::Failure(ErrorCode::NotFound);
    }

    return Result<UiTextureDesc>::Success(*found);
}

Result<void> NullUiRenderer::DrawWindows(UiDrawBuffer& buffer) noexcept {
    // 실제 렌더러와 같은 입력 검사를 한다: 모르는 대상은 NotFound다.
    for (const UiDrawTarget& target : buffer.Targets()) {
        if (!target.window.IsNull() && !windows_.IsValid(target.window.handle)) {
            return Result<void>::Failure(ErrorCode::NotFound);
        }

        if (!target.renderTarget.IsNull()) {
            const UiTextureDesc* desc = textures_.Find(target.renderTarget);
            if (!desc) {
                return Result<void>::Failure(ErrorCode::NotFound);
            }

            if (desc->usage != UiTextureUsage::RenderTarget || desc->width != target.width ||
                desc->height != target.height) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }
        }
    }

    // 모르는 표면 pass 대상은 그 pass만 건너뛴다(실제 렌더러가 import하지 못한 타깃을 건너뛰는 것과
    // 같다).
    for (const UiSurfacePass& pass : buffer.Passes()) {
        if (!textures_.IsValid(pass.target)) {
            ++stats_.passesSkipped;
        }
    }

    ++stats_.drawCalls;
    stats_.targetsDrawn += buffer.Targets().Size();
    stats_.passesDrawn += buffer.Passes().Size();
    stats_.packetsDrawn += buffer.Packets().Size();
    // 장면 packet은 기록하지 않고 게임 lane에서 부순다.
    for (UiScenePacket& packet : buffer.Packets()) {
        packet = UiScenePacket{};
    }

    return Result<void>::Success();
}

Result<uint64> NullUiRenderer::RequestCapture(UiTextureId target) noexcept {
    const auto* found = textures_.Find(target);
    if (!found || found->usage != UiTextureUsage::RenderTarget) {
        return Result<uint64>::Failure(found ? ErrorCode::InvalidArgument : ErrorCode::NotFound);
    }

    const Capture capture{++nextCapture_, found->width, found->height};
    if (auto added = captures_.TryPushBack(capture); !added) {
        return Result<uint64>::Failure(added.Error());
    }

    return Result<uint64>::Success(capture.id);
}

Result<void> NullUiRenderer::WaitCapture(uint64 capture) noexcept {
    for (const Capture& pending : captures_) {
        if (pending.id == capture) {
            return Result<void>::Success();
        }
    }

    return Result<void>::Failure(ErrorCode::NotFound);
}

Result<void> NullUiRenderer::TakeCapture(uint64 capture, Array<uint8>& rgba) noexcept {
    for (usize index = 0; index < captures_.Size(); ++index) {
        if (captures_[index].id != capture) {
            continue;
        }

        const usize bytes = static_cast<usize>(captures_[index].width) * captures_[index].height *
                            NullCaptureBytesPerPixel;
        if (auto resized = rgba.TryResize(bytes); !resized) {
            return resized;
        }

        for (uint8& value : rgba) {
            value = 0;
        }

        return captures_.TryRemoveAt(index);
    }

    return Result<void>::Failure(ErrorCode::NotFound);
}

Result<void> NullUiRenderer::Flush() noexcept {
    return Result<void>::Success();
}

usize NullUiRenderer::LiveTextures() const noexcept {
    return textures_.Size();
}

usize NullUiRenderer::LiveWindowSurfaces() const noexcept {
    return windows_.Size();
}

const NullUiRenderer::Stats& NullUiRenderer::Statistics() const noexcept {
    return stats_;
}

} // namespace rocks::ui
