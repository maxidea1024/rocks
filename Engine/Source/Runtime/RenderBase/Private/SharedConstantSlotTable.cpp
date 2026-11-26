#include <RenderBase/SharedConstantSlotTable.h>

// 공유 상수 slot 표다(Docs/Design/RenderBase 9.14절 1항). 규칙은 공개 헤더 주석을 따른다.
namespace rocks::render {

namespace {

Result<uint32> RejectSlot(const char** reason, const char* why,
                          ErrorCode code = ErrorCode::InvalidArgument) noexcept {
    if (reason != nullptr) {
        *reason = why;
    }

    return Result<uint32>::Failure(code);
}

bool IsSlotName(StringView name) noexcept {
    if (name.IsEmpty() || name.ByteCount() > MaxSharedConstantSlotNameBytes) {
        return false;
    }

    for (usize index = 0; index < name.ByteCount(); ++index) {
        const char c = name.Data()[index];
        const bool letter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
        if (!letter && !(index > 0 && c >= '0' && c <= '9')) {
            return false;
        }
    }

    return true;
}

} // namespace

Result<uint32> SharedConstantSlotTable::Register(const SharedConstantSlotDesc& desc,
                                                 const char** reason) noexcept {
    if (IsFrozen()) {
        return RejectSlot(reason, "shared constant slots are registered before the shader host "
                                  "freezes",
                          ErrorCode::ScopeClosed);
    }

    if (!IsSlotName(desc.name)) {
        return RejectSlot(reason, "shared constant slot name must be an identifier of at most 64 "
                                  "bytes");
    }

    if (desc.layout == nullptr || desc.layout->rootConstants || desc.layout->structId == 0) {
        return RejectSlot(reason, "a shared constant slot needs a generated constant buffer "
                                  "layout (struct identity, not root constants)");
    }

    if (!shader::ValidateShaderParameterLayout(*desc.layout, reason)) {
        return Result<uint32>::Failure(ErrorCode::InvalidArgument);
    }

    if (desc.lifetime != ConstantBlockLifetime::PerDraw &&
        desc.lifetime != ConstantBlockLifetime::PerFrame &&
        desc.lifetime != ConstantBlockLifetime::Persistent) {
        return RejectSlot(reason, "shared constant slot lifetime is outside the vocabulary");
    }

    for (uint32 index = 0; index < count_; ++index) {
        if (slots_[index].name == desc.name ||
            slots_[index].layout->structId == desc.layout->structId) {
            return RejectSlot(reason, "shared constant slot names and structs must be unique",
                              ErrorCode::AlreadyExists);
        }
    }

    if (count_ == MaxSharedConstantSlots) {
        return RejectSlot(reason, "the shared constant slot table is full (8 slots)",
                          ErrorCode::CapacityExceeded);
    }

    slots_[count_] = desc;
    return Result<uint32>::Success(count_++);
}

void SharedConstantSlotTable::Freeze() noexcept {
    frozen_.Store<platform::MemoryOrder::Release>(true);
}

const SharedConstantSlotDesc* SharedConstantSlotTable::At(uint32 slot) const noexcept {
    return slot < count_ ? &slots_[slot] : nullptr;
}

Result<uint32> SharedConstantSlotTable::FindByStruct(uint64 structId) const noexcept {
    for (uint32 index = 0; structId != 0 && index < count_; ++index) {
        if (slots_[index].layout->structId == structId) {
            return Result<uint32>::Success(index);
        }
    }

    return Result<uint32>::Failure(ErrorCode::NotFound);
}

Result<uint32> SharedConstantSlotTable::FindByName(StringView name) const noexcept {
    for (uint32 index = 0; index < count_; ++index) {
        if (slots_[index].name == name) {
            return Result<uint32>::Success(index);
        }
    }

    return Result<uint32>::Failure(ErrorCode::NotFound);
}

} // namespace rocks::render
