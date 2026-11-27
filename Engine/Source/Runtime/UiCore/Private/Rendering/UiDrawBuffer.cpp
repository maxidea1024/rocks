#include <UiCore/Rendering/UiDrawBuffer.h>
#include <UiCore/Rendering/UiSceneExtract.h>

namespace rocks::ui {

UiDrawBuffer::UiDrawBuffer(Allocator& allocator) noexcept
    : allocator_(allocator), targets_(allocator), passes_(allocator), packets_(allocator) {
}

void UiDrawBuffer::Clear() noexcept {
    targets_.Clear();
    passes_.Clear();
    packets_.Clear();
    closedPasses_ = 0;
}

Result<void> UiDrawBuffer::AddRetainedPass(UiTextureId target, uint32 width, uint32 height,
                                           LinearColor clear, const DrawBatch& content) noexcept {
    if (target.IsNull() || width == 0 || height == 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    UiSurfacePass pass;
    pass.kind = UiSurfacePassKind::Retained;
    pass.target = target;
    pass.width = width;
    pass.height = height;
    pass.clear = clear;
    pass.content = &content;
    return passes_.TryPushBack(pass);
}

Result<UiSceneExtract> UiDrawBuffer::AddScenePass(UiTextureId target, uint32 width, uint32 height,
                                                  LinearColor clear) noexcept {
    if (target.IsNull() || width == 0 || height == 0) {
        return Result<UiSceneExtract>::Failure(ErrorCode::InvalidArgument);
    }

    UiSurfacePass pass;
    pass.kind = UiSurfacePassKind::Scene;
    pass.target = target;
    pass.width = width;
    pass.height = height;
    pass.clear = clear;
    pass.firstPacket = packets_.Size();
    if (auto added = passes_.TryPushBack(pass); !added) {
        return Result<UiSceneExtract>::Failure(added.Error());
    }

    return Result<UiSceneExtract>::Success(UiSceneExtract(*this, passes_.Size() - 1));
}

Result<void> UiDrawBuffer::AddWindow(UiWindowSurfaceId window, uint32 width, uint32 height,
                                     LinearColor clear, const DrawBatch& batch) noexcept {
    if (window.IsNull()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    UiDrawTarget target;
    target.window = window;
    target.width = width;
    target.height = height;
    target.clear = clear;
    return AddTarget(target, batch);
}

Result<void> UiDrawBuffer::AddOffscreen(UiTextureId renderTarget, uint32 width, uint32 height,
                                        LinearColor clear, const DrawBatch& batch) noexcept {
    if (renderTarget.IsNull()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    UiDrawTarget target;
    target.renderTarget = renderTarget;
    target.width = width;
    target.height = height;
    target.clear = clear;
    return AddTarget(target, batch);
}

Result<void> UiDrawBuffer::AddTarget(UiDrawTarget target, const DrawBatch& batch) noexcept {
    if (target.width == 0 || target.height == 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    target.batch = &batch;
    target.firstPass = closedPasses_;
    target.passCount = passes_.Size() - closedPasses_;
    if (auto added = targets_.TryPushBack(target); !added) {
        return added;
    }

    closedPasses_ = passes_.Size();
    return Result<void>::Success();
}

void UiDrawBuffer::DiscardPending() noexcept {
    if (passes_.Size() == closedPasses_) {
        return;
    }

    // 닫힌 대상의 packet은 첫 미닫힘 pass의 firstPacket 앞에 모두 있다.
    usize firstPacket = packets_.Size();
    for (usize index = closedPasses_; index < passes_.Size(); ++index) {
        if (passes_[index].kind == UiSurfacePassKind::Scene) {
            firstPacket = passes_[index].firstPacket;
            break;
        }
    }

    if (firstPacket < packets_.Size()) {
        (void)packets_.TryRemoveAt(firstPacket, packets_.Size() - firstPacket);
    }

    (void)passes_.TryRemoveAt(closedPasses_, passes_.Size() - closedPasses_);
}

Result<void> UiDrawBuffer::PostPacket(usize pass, UiScenePacket&& packet) noexcept {
    // 장면 packet은 pass마다 연속이어야 하므로 마지막으로 쌓은 장면 pass에만 게시한다.
    if (pass + 1 != passes_.Size() || pass < closedPasses_ ||
        passes_[pass].kind != UiSurfacePassKind::Scene) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (auto added = packets_.TryPushBack(Move(packet)); !added) {
        return added;
    }

    ++passes_[pass].packetCount;
    return Result<void>::Success();
}

usize UiDrawBuffer::PendingPassCount() const noexcept {
    return passes_.Size() - closedPasses_;
}

Span<const UiDrawTarget> UiDrawBuffer::Targets() const noexcept {
    return targets_.View();
}

Span<const UiSurfacePass> UiDrawBuffer::Passes() const noexcept {
    return passes_.View();
}

Span<UiScenePacket> UiDrawBuffer::Packets() noexcept {
    return packets_.View();
}

Allocator& UiDrawBuffer::GetAllocator() const noexcept {
    return allocator_;
}

} // namespace rocks::ui
