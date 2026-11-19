#include "GraphCommandList.h"

namespace rocks::render::detail {

using graphics::BufferHandle;
using graphics::BufferViewHandle;
using graphics::PipelineHandle;
using graphics::ResourceTransition;
using graphics::TextureHandle;

void GraphCommandList::Fail(ErrorCode code) noexcept {
    if (error_ == ErrorCode::None) {
        error_ = code;
    }
}

void GraphCommandList::Push(const Command& command) noexcept {
    auto pushed = commands_.TryPushBack(arena_, command);
    if (!pushed) {
        Fail(pushed.Error());
    }
}

const void* GraphCommandList::Copy(const void* data, usize bytes, usize alignment) noexcept {
    if (bytes == 0) {
        return nullptr;
    }

    auto memory = arena_.TryAllocate(bytes, alignment);
    if (!memory) {
        Fail(memory.Error());
        return nullptr;
    }

    platform::MemoryCopy(memory.Value(), data, bytes);
    return memory.Value();
}

void GraphCommandList::SetPipeline(PipelineHandle pipeline) noexcept {
    Command command;
    command.op = Op::SetPipeline;
    command.first = pipeline.slot;
    Push(command);
}

void GraphCommandList::SetVertexBuffers(
    uint32 firstStream, Span<const graphics::VertexBufferBinding> buffers) noexcept {
    Command command;
    command.op = Op::SetVertexBuffers;
    command.a = firstStream;
    command.count = buffers.Size();
    command.data = Copy(buffers.Data(), buffers.Size() * sizeof(graphics::VertexBufferBinding),
                        alignof(graphics::VertexBufferBinding));
    Push(command);
}

void GraphCommandList::SetIndexBuffer(BufferHandle buffer, graphics::IndexFormat format,
                                      uint64 offset) noexcept {
    Command command;
    command.op = Op::SetIndexBuffer;
    command.first = buffer.slot;
    command.a = static_cast<uint32>(format);
    command.x = offset;
    Push(command);
}

void GraphCommandList::SetTexture(TextureHandle texture) noexcept {
    Command command;
    command.op = Op::SetTexture;
    command.first = texture.slot;
    Push(command);
}

void GraphCommandList::SetBufferView(uint32 slot, BufferViewHandle view) noexcept {
    Command command;
    command.op = Op::SetBufferView;
    command.a = slot;
    command.first = view.slot;
    Push(command);
}

Result<void> GraphCommandList::SetDrawConstants(Span<const uint8> bytes) noexcept {
    Command command;
    command.op = Op::SetDrawConstants;
    command.count = bytes.Size();
    command.data = Copy(bytes.Data(), bytes.Size(), alignof(uint32));
    Push(command);
    return Result<void>::Success();
}

void GraphCommandList::Draw(uint32 vertexCount, uint32 instanceCount, uint32 firstVertex) noexcept {
    Command command;
    command.op = Op::Draw;
    command.a = vertexCount;
    command.b = instanceCount;
    command.c = firstVertex;
    Push(command);
}

void GraphCommandList::DrawIndexed(uint32 indexCount, uint32 instanceCount,
                                   uint32 firstIndex) noexcept {
    Command command;
    command.op = Op::DrawIndexed;
    command.a = indexCount;
    command.b = instanceCount;
    command.c = firstIndex;
    Push(command);
}

void GraphCommandList::DrawIndirect(BufferHandle arguments, uint64 offset) noexcept {
    Command command;
    command.op = Op::DrawIndirect;
    command.first = arguments.slot;
    command.x = offset;
    Push(command);
}

void GraphCommandList::DrawIndexedIndirect(BufferHandle arguments, uint64 offset) noexcept {
    Command command;
    command.op = Op::DrawIndexedIndirect;
    command.first = arguments.slot;
    command.x = offset;
    Push(command);
}

void GraphCommandList::Dispatch(uint32 groupCountX, uint32 groupCountY,
                                uint32 groupCountZ) noexcept {
    Command command;
    command.op = Op::Dispatch;
    command.a = groupCountX;
    command.b = groupCountY;
    command.c = groupCountZ;
    Push(command);
}

void GraphCommandList::DispatchIndirect(BufferHandle arguments, uint64 offset) noexcept {
    Command command;
    command.op = Op::DispatchIndirect;
    command.first = arguments.slot;
    command.x = offset;
    Push(command);
}

void GraphCommandList::SetWritableBufferView(uint32 slot, BufferViewHandle view) noexcept {
    Command command;
    command.op = Op::SetWritableBufferView;
    command.a = slot;
    command.first = view.slot;
    Push(command);
}

void GraphCommandList::SetWritableTexture(uint32 slot, TextureHandle view) noexcept {
    Command command;
    command.op = Op::SetWritableTexture;
    command.a = slot;
    command.first = view.slot;
    Push(command);
}

void GraphCommandList::SetViewport(const graphics::Viewport& viewport) noexcept {
    Command command;
    command.op = Op::SetViewport;
    command.viewport = viewport;
    Push(command);
}

void GraphCommandList::SetScissor(const graphics::ScissorRect& scissor) noexcept {
    Command command;
    command.op = Op::SetScissor;
    command.scissor = scissor;
    Push(command);
}

void GraphCommandList::SetStencilReference(uint32 reference) noexcept {
    Command command;
    command.op = Op::SetStencilReference;
    command.a = reference;
    Push(command);
}

Result<void> GraphCommandList::CopyBuffer(BufferHandle destination, uint64 destinationOffset,
                                          BufferHandle source, uint64 sourceOffset,
                                          uint64 size) noexcept {
    Command command;
    command.op = Op::CopyBuffer;
    command.first = destination.slot;
    command.second = source.slot;
    command.x = destinationOffset;
    command.y = sourceOffset;
    command.z = size;
    Push(command);
    return Result<void>::Success();
}

Result<void> GraphCommandList::CopyTexture(TextureHandle destination,
                                           TextureHandle source) noexcept {
    Command command;
    command.op = Op::CopyTexture;
    command.first = destination.slot;
    command.second = source.slot;
    Push(command);
    return Result<void>::Success();
}

Result<void> GraphCommandList::ClearWritableBufferView(BufferViewHandle view,
                                                       uint32 value) noexcept {
    Command command;
    command.op = Op::ClearWritableBufferView;
    command.first = view.slot;
    command.a = value;
    Push(command);
    return Result<void>::Success();
}

Result<void> GraphCommandList::CopyPassTarget(TextureHandle destination) noexcept {
    Command command;
    command.op = Op::CopyPassTarget;
    command.first = destination.slot;
    Push(command);
    return Result<void>::Success();
}

void GraphCommandList::Transition(Span<const ResourceTransition> transitions) noexcept {
    Command command;
    command.op = Op::Transition;
    command.count = transitions.Size();
    command.data = Copy(transitions.Data(), transitions.Size() * sizeof(ResourceTransition),
                        alignof(ResourceTransition));
    Push(command);
}

graphics::SplitTransition
GraphCommandList::BeginTransitions(Span<const ResourceTransition> transitions) noexcept {
    // 분리 전이는 그래프가 소유 스레드에서 낸다(기록 목록에서는 쓸 수 없다).
    (void)transitions;
    Fail(ErrorCode::Unsupported);
    return {};
}

void GraphCommandList::EndTransitions(graphics::SplitTransition split,
                                      Span<const ResourceTransition> transitions) noexcept {
    (void)split;
    (void)transitions;
    Fail(ErrorCode::Unsupported);
}

Result<uint64> GraphCommandList::WriteTimestamp() noexcept {
    // GPU 구간은 그래프가 재생 위치에서 소유 스레드로 낸다.
    Fail(ErrorCode::Unsupported);
    return Result<uint64>::Failure(ErrorCode::Unsupported);
}

void GraphCommandList::PushMarker(StringView name) noexcept {
    Command command;
    command.op = Op::PushMarker;
    command.count = name.ByteCount();
    command.data = Copy(name.Data(), name.ByteCount(), alignof(char));
    Push(command);
}

void GraphCommandList::PopMarker() noexcept {
    Command command;
    command.op = Op::PopMarker;
    Push(command);
}

Result<void> GraphCommandList::Replay(graphics::CommandContext& target) const noexcept {
    ErrorCode first = ErrorCode::None;
    const auto keep = [&first](const Result<void>& result) noexcept {
        if (!result && first == ErrorCode::None) {
            first = result.Error();
        }
    };
    for (const Command& command : commands_) {
        switch (command.op) {
        case Op::SetPipeline:
            target.SetPipeline(PipelineHandle{command.first});
            break;
        case Op::SetVertexBuffers:
            target.SetVertexBuffers(
                command.a, Span<const graphics::VertexBufferBinding>(
                               static_cast<const graphics::VertexBufferBinding*>(command.data),
                               command.count));
            break;
        case Op::SetIndexBuffer:
            target.SetIndexBuffer(BufferHandle{command.first},
                                  static_cast<graphics::IndexFormat>(command.a), command.x);
            break;
        case Op::SetTexture:
            target.SetTexture(TextureHandle{command.first});
            break;
        case Op::SetBufferView:
            target.SetBufferView(command.a, BufferViewHandle{command.first});
            break;
        case Op::SetDrawConstants:
            keep(target.SetDrawConstants(
                Span<const uint8>(static_cast<const uint8*>(command.data), command.count)));
            break;
        case Op::Draw:
            target.Draw(command.a, command.b, command.c);
            break;
        case Op::DrawIndexed:
            target.DrawIndexed(command.a, command.b, command.c);
            break;
        case Op::DrawIndirect:
            target.DrawIndirect(BufferHandle{command.first}, command.x);
            break;
        case Op::DrawIndexedIndirect:
            target.DrawIndexedIndirect(BufferHandle{command.first}, command.x);
            break;
        case Op::Dispatch:
            target.Dispatch(command.a, command.b, command.c);
            break;
        case Op::DispatchIndirect:
            target.DispatchIndirect(BufferHandle{command.first}, command.x);
            break;
        case Op::SetWritableBufferView:
            target.SetWritableBufferView(command.a, BufferViewHandle{command.first});
            break;
        case Op::SetWritableTexture:
            target.SetWritableTexture(command.a, TextureHandle{command.first});
            break;
        case Op::SetViewport:
            target.SetViewport(command.viewport);
            break;
        case Op::SetScissor:
            target.SetScissor(command.scissor);
            break;
        case Op::SetStencilReference:
            target.SetStencilReference(command.a);
            break;
        case Op::CopyBuffer:
            keep(target.CopyBuffer(BufferHandle{command.first}, command.x,
                                   BufferHandle{command.second}, command.y, command.z));
            break;
        case Op::CopyTexture:
            keep(target.CopyTexture(TextureHandle{command.first}, TextureHandle{command.second}));
            break;
        case Op::ClearWritableBufferView:
            keep(target.ClearWritableBufferView(BufferViewHandle{command.first}, command.a));
            break;
        case Op::CopyPassTarget:
            keep(target.CopyPassTarget(TextureHandle{command.first}));
            break;
        case Op::Transition:
            target.Transition(Span<const ResourceTransition>(
                static_cast<const ResourceTransition*>(command.data), command.count));
            break;
        case Op::PushMarker:
            target.PushMarker(StringView(static_cast<const char*>(command.data), command.count));
            break;
        case Op::PopMarker:
            target.PopMarker();
            break;
        }
    }

    return first == ErrorCode::None ? Result<void>::Success() : Result<void>::Failure(first);
}

} // namespace rocks::render::detail
