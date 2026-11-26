#include <RenderBase/SharedConstantSlotMap.h>

// 공유 상수 slot의 파이프라인 지도다(Docs/Design/RenderBase 9.14절 2항).
namespace rocks::render {

Result<SharedConstantSlotMap>
SharedConstantSlotMap::Build(const SharedConstantSlotTable& table,
                             Span<const shader::ShaderBindingLayout* const> stages,
                             const char** reason) noexcept {
    if (!table.IsFrozen()) {
        if (reason != nullptr) {
            *reason = "the shared constant slot table is not frozen";
        }

        return Result<SharedConstantSlotMap>::Failure(ErrorCode::InvalidArgument);
    }

    SharedConstantSlotMap map;
    for (uint32 slot = 0; slot < table.Count(); ++slot) {
        const SharedConstantSlotDesc& desc = *table.At(slot);
        bool read = false;
        for (const shader::ShaderBindingLayout* stage : stages) {
            if (stage == nullptr) {
                continue;
            }

            for (const shader::ShaderBindingResource& resource : stage->resources) {
                read = read || (resource.kind == shader::ShaderResourceKind::ConstantBuffer &&
                                resource.structId == desc.layout->structId);
            }
        }

        if (!read) {
            continue;
        }

        // 자리·크기·멤버 대조는 바인딩 지도 규칙 그대로다(구조체 identity로 찾는다 — SX8).
        auto built = ShaderBindingMap::Build(*desc.layout, stages, reason);
        if (!built) {
            return Result<SharedConstantSlotMap>::Failure(built.Error());
        }

        map.uses_[map.count_++] = SharedConstantSlotUse{slot, built.Value()};
    }

    return Result<SharedConstantSlotMap>::Success(map);
}

bool SharedConstantSlotMap::Reads(uint32 slot) const noexcept {
    for (usize index = 0; index < count_; ++index) {
        if (uses_[index].slot == slot) {
            return true;
        }
    }

    return false;
}

} // namespace rocks::render
