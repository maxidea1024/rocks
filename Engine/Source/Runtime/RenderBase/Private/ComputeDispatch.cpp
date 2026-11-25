#include <RenderBase/ComputeDispatch.h>

#include <Graphics/PipelineState.h>

// compute dispatch 도우미 규칙이다(Docs/Design/RenderBase 4.4절 "GX3·GX4 compute 연결 설계" (2)).
namespace rocks::render {

namespace {

// 간접 인자 offset의 정렬(byte)이다(D3D12 ExecuteIndirect·Vulkan·GL 모두 4 byte).
constexpr uint64 IndirectArgumentAlignment = 4;

} // namespace

Result<void> ValidateGroupCount(const graphics::DeviceLimits& limits,
                                ComputeGroupCount groups) noexcept {
    if (groups.IsEmpty()) {
        return Result<void>::Success();
    }

    const uint32 counts[3] = {groups.x, groups.y, groups.z};
    for (uint32 axis = 0; axis < 3; ++axis) {
        if (counts[axis] > limits.computeGroupCount[axis]) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    return Result<void>::Success();
}

Result<void> ValidateDispatchArguments(const graphics::BufferDesc& buffer, uint64 offset) noexcept {
    constexpr uint64 bytes = sizeof(graphics::DispatchIndirectArguments);
    if (!graphics::HasAnyBufferUsage(buffer.usage, graphics::BufferUsage::Indirect) ||
        offset % IndirectArgumentAlignment != 0 || offset > buffer.size ||
        buffer.size - offset < bytes) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<void>::Success();
}

} // namespace rocks::render
