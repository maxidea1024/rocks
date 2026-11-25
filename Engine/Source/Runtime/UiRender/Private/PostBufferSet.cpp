#include <UiRender/PostBufferSet.h>

namespace rocks::ui {

PostBufferSet::PostBufferSet(graphics::Device& device, SlotArray<BrushTexture>& textures) noexcept
    : device_(device), textures_(textures) {
}
PostBufferSet::~PostBufferSet() noexcept {
    for (const auto& target : targets_) {
        if (!target.brush.IsNull()) {
            (void)textures_.TryRemove(target.brush);
            device_.Retire(target.texture);
        }
    }
}
SlotHandle PostBufferSet::Get(uint32 channel) const noexcept {
    return channel < PostBufferChannelCount ? targets_[channel].brush : SlotHandle{};
}
Result<SlotHandle> PostBufferSet::Prepare(uint32 channel, uint32 width, uint32 height,
                                          graphics::PixelFormat format,
                                          graphics::PipelineHandle pipeline) noexcept {
    if (channel >= PostBufferChannelCount || !width || !height || pipeline.IsNull()) {
        return Result<SlotHandle>::Failure(ErrorCode::InvalidArgument);
    }
    auto& target = targets_[channel];
    if (!target.brush.IsNull() && width == target.width && height == target.height &&
        target.format == format) {
        return Result<SlotHandle>::Success(target.brush);
    }
    graphics::TextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.format = format;
    desc.usage = graphics::TextureUsage::RenderTarget;
    auto created = device_.CreateTexture(desc, {});
    if (!created) {
        return Result<SlotHandle>::Failure(created.Error());
    }
    const BrushTexture value{created.Value(), pipeline};
    if (target.brush.IsNull()) {
        auto slot = textures_.TryAdd(value);
        if (!slot) {
            device_.Retire(created.Value());
            return Result<SlotHandle>::Failure(slot.Error());
        }
        target.brush = slot.Value();
    } else {
        auto* brush = textures_.Find(target.brush);
        if (!brush) {
            device_.Retire(created.Value());
            return Result<SlotHandle>::Failure(ErrorCode::StaleHandle);
        }
        *brush = value;
        device_.Retire(target.texture);
    }
    target.texture = created.Value();
    target.width = width;
    target.height = height;
    target.format = format;
    return Result<SlotHandle>::Success(target.brush);
}

} // namespace rocks::ui
