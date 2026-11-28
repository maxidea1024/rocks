#include <Base/NumericLimits.h>

#include "IrResultQueries.h"

namespace rocks::material::detail {

namespace {

// float 비트에서 부호를 뺀 크기 비트다(+0·-0을 같이 0으로 본다).
constexpr uint32 FloatMagnitudeMask = static_cast<uint32>(NumericLimits<int32>::Max());

} // namespace

const IrResult* FindIrResult(const IrModule& module, IrEntry entry, StringView slot) noexcept {
    for (const IrResult& result : module.Results()) {
        if (result.entry == entry && result.slot == slot) {
            return &result;
        }
    }

    return nullptr;
}

bool IsZeroIrResult(const IrModule& module, const IrResult* result) noexcept {
    if (result == nullptr) {
        return true;
    }

    const IrValue& value = module.Values()[result->value];
    if (value.op != IrOp::Constant || value.type.scalar != IrScalar::Float) {
        return false;
    }

    for (uint8 index = 0; index < value.type.components; ++index) {
        if ((static_cast<uint32>(value.immediate[index]) & FloatMagnitudeMask) != 0) {
            return false;
        }
    }

    return true;
}

} // namespace rocks::material::detail
