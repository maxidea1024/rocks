#include <MaterialSchema/VertexInputUsage.h>

// 사용 대상 집합 도우미다(Docs/Design/MaterialSystem 4.6.1절).
namespace rocks::material {

bool ContainsVertexInput(const VertexInputUsage& usage, Name name) noexcept {
    for (const Name& entry : usage.vertexInputs) {
        if (entry == name) {
            return true;
        }
    }

    return false;
}

Result<void> AddVertexInput(VertexInputUsage& usage, Name name) noexcept {
    if (name.IsNone()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (ContainsVertexInput(usage, name)) {
        return Result<void>::Success();
    }

    return usage.vertexInputs.TryPushBack(name);
}

} // namespace rocks::material
