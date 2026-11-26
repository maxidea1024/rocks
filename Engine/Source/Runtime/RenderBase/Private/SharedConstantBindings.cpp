#include <RenderBase/SharedConstantBindings.h>

// 공유 상수 slot 값 묶음이다(Docs/Design/RenderBase 9.14절 3항).
namespace rocks::render {

Result<void> SharedConstantBindings::Bind(uint32 slot, const ConstantBlock* block) noexcept {
    const SharedConstantSlotDesc* desc = table_->At(slot);
    if (desc == nullptr) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    if (block != nullptr && block->Layout().structId != desc->layout->structId) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    blocks_[slot] = block;
    return Result<void>::Success();
}

Result<void> SharedConstantBindings::Bind(StringView name, const ConstantBlock* block) noexcept {
    auto slot = table_->FindByName(name);
    if (!slot) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    return Bind(slot.Value(), block);
}

const ConstantBlock* SharedConstantBindings::Bound(uint32 slot) const noexcept {
    return slot < MaxSharedConstantSlots ? blocks_[slot] : nullptr;
}

Result<void> SharedConstantBindings::Apply(graphics::CommandContext& commands,
                                           const SharedConstantSlotMap& map,
                                           const char** reason) const noexcept {
    for (const SharedConstantSlotUse& use : map.Uses()) {
        const ConstantBlock* block = Bound(use.slot);
        if (block == nullptr) {
            if (reason != nullptr) {
                *reason = "shared constant slot read by the pipeline is not bound";
            }

            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        if (auto applied = ApplyConstantBlock(commands, use.map, *block); !applied) {
            return applied;
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::render
