#include "RenderGraphInternal.h"

#include <RenderGraph/RenderGraph.h>

// 그래프 버퍼 view와 compute pass 도우미다(Docs/Design/RenderBase 4.4절 "GX3·GX4 compute 연결 설계"
// (3)·(4)). 규칙은 공개 헤더 주석을 따른다.
namespace rocks::render {

using namespace detail;

namespace {

// 간접 인자 offset의 정렬(byte)이다(import 인자 버퍼는 크기·용도를 모르므로 이것만 본다).
constexpr uint64 ComputeIndirectOffsetAlignment = 4;

const char* ComputeTraceName(TraceScopeName& name) noexcept {
    return name.Name() != nullptr ? name.Name() : "";
}

} // namespace

Result<GraphBufferView>
RenderGraph::CreateBufferView(GraphBuffer buffer, const graphics::BufferViewDesc& desc) noexcept {
    if (state_ == nullptr) {
        return Result<GraphBufferView>::Failure(ErrorCode::OutOfMemory);
    }

    GraphState& state = *state_;
    if (state.executed) {
        return Result<GraphBufferView>::Failure(ErrorCode::Busy);
    }

    if (buffer.graph != state.serial || buffer.index >= state.resources.Size() ||
        state.resources[buffer.index].kind != GraphResourceKind::Buffer) {
        return Result<GraphBufferView>::Failure(
            RecordGraphError(state, ErrorCode::InvalidArgument, true,
                             "a buffer view refers to a buffer from another graph"));
    }

    // 만든 버퍼는 장치 규칙(ResolveBufferView)으로 바로 본다. import 버퍼는 desc를 모르므로 장치
    // view 생성이 확인한다.
    const ResourceRecord& resource = state.resources[buffer.index];
    if (!resource.imported && !graphics::ResolveBufferView(resource.bufferDesc, desc)) {
        return Result<GraphBufferView>::Failure(RecordGraphError(
            state, ErrorCode::InvalidArgument, true,
            "buffer view of '{}' is invalid (offset {}, {} elements, {} access)", resource.name,
            desc.offset, desc.elementCount,
            desc.access == graphics::ViewAccess::ReadWrite ? "read-write" : "read"));
    }

    ViewRecord view;
    view.kind = GraphResourceKind::Buffer;
    view.resource = buffer.index;
    view.bufferDesc = desc;
    auto pushed = state.views.TryPushBack(arena_, view);
    if (!pushed) {
        return Result<GraphBufferView>::Failure(pushed.Error());
    }

    GraphBufferView handle;
    handle.index = static_cast<uint32>(state.views.Size() - 1);
    handle.graph = state.serial;
    return Result<GraphBufferView>::Success(handle);
}

Result<void> RenderGraph::CheckComputeDispatch(TraceScopeName& name, GraphPassKind kind,
                                               const GraphComputeDispatch& dispatch,
                                               GraphPassAccesses& accesses) noexcept {
    if (state_ == nullptr) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    GraphState& state = *state_;
    if (state.executed) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    const char* passName = ComputeTraceName(name);
    if (kind != GraphPassKind::Compute && kind != GraphPassKind::AsyncCompute) {
        return Result<void>::Failure(
            RecordGraphError(state, ErrorCode::InvalidArgument, true,
                             "compute pass '{}' must be a Compute or AsyncCompute pass", passName));
    }

    if (dispatch.pipeline.IsNull()) {
        return Result<void>::Failure(RecordGraphError(state, ErrorCode::InvalidArgument, true,
                                                      "compute pass '{}' has no pipeline",
                                                      passName));
    }

    const GraphBuffer arguments = dispatch.indirectArguments;
    if (arguments.IsNull()) {
        const ComputeGroupCount groups = dispatch.groups;
        if (state.device != nullptr && !ValidateGroupCount(state.device->Caps().limits, groups)) {
            return Result<void>::Failure(RecordGraphError(
                state, ErrorCode::InvalidArgument, true,
                "compute pass '{}' dispatches {} x {} x {} groups, over the device limit", passName,
                groups.x, groups.y, groups.z));
        }

        return Result<void>::Success();
    }

    if (arguments.graph != state.serial || arguments.index >= state.resources.Size() ||
        state.resources[arguments.index].kind != GraphResourceKind::Buffer) {
        return Result<void>::Failure(RecordGraphError(
            state, ErrorCode::InvalidArgument, true,
            "compute pass '{}' reads indirect arguments from another graph", passName));
    }

    const ResourceRecord& resource = state.resources[arguments.index];
    const bool valid =
        resource.imported
            ? dispatch.indirectOffset % ComputeIndirectOffsetAlignment == 0
            : ValidateDispatchArguments(resource.bufferDesc, dispatch.indirectOffset).IsOk();
    if (!valid) {
        return Result<void>::Failure(RecordGraphError(
            state, ErrorCode::InvalidArgument, true,
            "compute pass '{}' has invalid indirect arguments in '{}' at offset {}", passName,
            resource.name, dispatch.indirectOffset));
    }

    // 인자 버퍼는 이 pass의 IndirectArgs 접근이다(이미 같은 선언이 있으면 더하지 않는다).
    for (const GraphResourceAccess& access : accesses.Accesses()) {
        if (access.kind == GraphResourceKind::Buffer && access.view == GraphIndexNone &&
            access.resource == arguments.index && access.graph == arguments.graph &&
            access.access == graphics::ResourceAccess::IndirectArgs) {
            return Result<void>::Success();
        }
    }

    accesses.Buffer(arguments, graphics::ResourceAccess::IndirectArgs);
    if (accesses.Error() != ErrorCode::None) {
        return Result<void>::Failure(accesses.Error());
    }

    return Result<void>::Success();
}

ErrorCode RenderGraph::RecordComputePipelineFailure(TraceScopeName& name, ErrorCode code) noexcept {
    if (state_ == nullptr) {
        return code;
    }

    return RecordGraphError(*state_, code, false,
                            "compute pass '{}' could not get its pipeline from the cache ({})",
                            ComputeTraceName(name), ErrorCodeName(code));
}

} // namespace rocks::render
