#include "RenderGraphInternal.h"

// 그래프 상수(Docs/Design/RenderBase 9.11절 8항, UE FRDGBuilder::CreateUniformBuffer 대응)다.
// 내용은 선언 때 프레임 arena에 복사하고, 장치 SingleFrame 상수 버퍼는 Deferred면 실행 시작(첫 기록
// 전, 소유 스레드)에, Immediate면 선언 때 만든다. 병렬 기록(RG3) pass 함수는 장치 생성을 하지
// 않으므로 pass 안에서 만들지 않는다. 상수 버퍼는 CPU 가시 메모리이고 바뀌지 않으므로 접근
// 선언·전이가 없다. 그래프 끝(FinishGraph)에 Retire한다(해제는 마지막 사용 fence 뒤).
namespace rocks::render {

using namespace detail;

namespace {

Result<void> CreateDeviceConstants(GraphState& state, ConstantRecord& record) noexcept {
    auto created = state.device->CreateConstantBuffer(graphics::ConstantBufferLifetime::SingleFrame,
                                                      Span<const uint8>(record.data, record.bytes));
    if (!created) {
        return Result<void>::Failure(RecordGraphError(state, created.Error(), false,
                                                      "graph constants '{}' could not be created",
                                                      record.name));
    }

    record.device = created.Value().slot;
    return Result<void>::Success();
}

} // namespace

namespace detail {

Result<void> CreateGraphConstants(GraphState& state) noexcept {
    for (ConstantRecord& record : state.constants) {
        if (record.device.IsNull()) {
            auto created = CreateDeviceConstants(state, record);
            if (!created) {
                return created;
            }
        }
    }

    return Result<void>::Success();
}

void RetireGraphConstants(GraphState& state) noexcept {
    for (ConstantRecord& record : state.constants) {
        if (!record.device.IsNull()) {
            state.device->Retire(graphics::ConstantBufferHandle{record.device});
            record.device = {};
        }
    }
}

} // namespace detail

Result<GraphConstantBuffer> RenderGraph::CreateConstants(Span<const uint8> bytes,
                                                         const char* name) noexcept {
    if (state_ == nullptr) {
        return Result<GraphConstantBuffer>::Failure(ErrorCode::OutOfMemory);
    }

    GraphState& state = *state_;
    if (state.executed) {
        return Result<GraphConstantBuffer>::Failure(ErrorCode::Busy);
    }

    if (!graphics::IsValidConstantBufferBytes(bytes.Size())) {
        return Result<GraphConstantBuffer>::Failure(RecordGraphError(
            state, ErrorCode::InvalidArgument, true,
            "graph constants '{}' must be 16-byte multiples of 16 to 65536 bytes", name));
    }

    auto memory = arena_.TryAllocate(bytes.Size(), alignof(uint64));
    if (!memory) {
        return Result<GraphConstantBuffer>::Failure(memory.Error());
    }

    ConstantRecord record;
    record.data = static_cast<uint8*>(memory.Value());
    record.bytes = static_cast<uint32>(bytes.Size());
    record.name = name;
    platform::MemoryCopy(record.data, bytes.Data(), bytes.Size());
    auto pushed = state.constants.TryPushBack(arena_, record);
    if (!pushed) {
        return Result<GraphConstantBuffer>::Failure(pushed.Error());
    }

    // 즉시 실행 모드는 pass를 선언하며 실행하므로 지금 만든다.
    if (state.mode == GraphMode::Immediate) {
        auto created = CreateDeviceConstants(state, state.constants[state.constants.Size() - 1]);
        if (!created) {
            return Result<GraphConstantBuffer>::Failure(created.Error());
        }
    }

    GraphConstantBuffer handle;
    handle.index = static_cast<uint32>(state.constants.Size() - 1);
    handle.graph = state.serial;
    return Result<GraphConstantBuffer>::Success(handle);
}

Result<GraphConstantBuffer> RenderGraph::CreateConstants(const ConstantBlock& block,
                                                         const char* name) noexcept {
    return CreateConstants(block.Bytes(), name);
}

graphics::ConstantBufferHandle
GraphPassContext::Constants(GraphConstantBuffer constants) const noexcept {
    const GraphState& state = *graph_.state_;
    if (constants.graph != state.serial || constants.index >= state.constants.Size()) {
        return graphics::ConstantBufferHandle{};
    }

    return graphics::ConstantBufferHandle{state.constants[constants.index].device};
}

Result<void> GraphPassContext::SetConstants(const ShaderBindingMap& map,
                                            GraphConstantBuffer constants) const noexcept {
    const graphics::ConstantBufferHandle buffer = Constants(constants);
    if (buffer.IsNull()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return ApplyConstantBuffer(commands_, map, buffer);
}

} // namespace rocks::render
