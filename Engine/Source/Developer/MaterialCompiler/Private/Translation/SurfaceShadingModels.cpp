#include "SurfaceShadingModels.h"

#include <Core/Containers/Array.h>
#include <Core/Memory/SystemAllocator.h>
#include <MaterialCompiler/MaterialIrBuilder.h>
#include <RenderData/MaterialEnums.h>

// shading model 집합이다(Docs/Design/MaterialSystem 6.11절 MS2d1).
namespace rocks::material::detail {

uint32 AllShadingModelBits() noexcept {
    const ShadingModel models[] = {ShadingModel::Unlit,
                                   ShadingModel::DefaultLit,
                                   ShadingModel::Subsurface,
                                   ShadingModel::ClearCoat,
                                   ShadingModel::SubsurfaceProfile,
                                   ShadingModel::TwoSidedFoliage,
                                   ShadingModel::Hair,
                                   ShadingModel::Cloth,
                                   ShadingModel::Eye,
                                   ShadingModel::SingleLayerWater,
                                   ShadingModel::ThinTranslucent};
    uint32 bits = 0;
    for (ShadingModel model : models) {
        bits |= 1u << static_cast<uint32>(model);
    }

    return bits;
}

Result<uint32> ReachableShadingModels(const IrBuilder& builder, IrValueId value) noexcept {
    uint32 bits = 0;
    SystemAllocator scratch;
    Array<IrValueId> pending(scratch);
    if (auto pushed = pending.TryPushBack(value); !pushed) {
        return Result<uint32>::Failure(pushed.Error());
    }

    while (!pending.IsEmpty()) {
        const IrValueId id = pending[pending.Size() - 1];
        (void)pending.TryPopBack();
        const IrValue& item = builder.Value(id);
        if (item.op == IrOp::Constant && item.immediate[0] < MaxShadingModelCount) {
            bits |= 1u << static_cast<uint32>(item.immediate[0]);
        } else if (item.op == IrOp::Select || item.op == IrOp::Branch) {
            for (uint8 operand = 1; operand < item.operandCount; ++operand) {
                if (auto pushed = pending.TryPushBack(item.operands[operand]); !pushed) {
                    return Result<uint32>::Failure(pushed.Error());
                }
            }
        } else {
            bits |= AllShadingModelBits();
        }
    }

    return Result<uint32>::Success(bits);
}

} // namespace rocks::material::detail
