#pragma once

#include "RenderGraphInternal.h"

// 그래프 병렬 기록의 기록 목록이다(Docs/Design/RenderBase.ko.md 8절 "RG3 병렬 기록 설계"). pass
// 기록 함수가 worker에서 이 목록에 명령을 쌓고, 소유 스레드가 선언 순서로 장치 CommandContext에
// 재생한다(UE FRHICommandList 기록 → RHI 번역 대응).
namespace rocks::render::detail {

// CommandContext 호출을 프레임 arena의 명령 열로 쌓는다.
//
// 계약(설계 "기록 목록의 한계"):
// - 값을 돌려주는 호출(SetDrawConstants·CopyBuffer·CopyTexture·ClearWritableBufferView·
//   CopyPassTarget)은 기록 때 성공을 돌려주고, 재생 때 실패하면 Replay가 첫 실패를 돌려준다.
// - WriteTimestamp는 Unsupported, BeginTransitions는 무효 짝을 돌려주고 EndTransitions는 무시한다
//   (둘 다 기록 오류로 남긴다). GPU 구간·분리 전이는 그래프가 소유 스레드에서 낸다.
// - Span 인자는 기록 때 arena에 복사한다(호출 뒤 원본을 바꿔도 된다).
//
// 스레드: 한 목록은 한 스레드가 기록하고(arena 할당은 스레드 안전), 기록이 끝난 뒤(ParallelFor
// join) 소유 스레드가 재생한다. 수명: 그래프의 프레임 arena 안이다(자명 소멸).
class GraphCommandList final : public graphics::CommandContext {
public:
    explicit GraphCommandList(FrameArena& arena) noexcept : arena_(arena) {
    }

    // 쌓은 명령을 순서대로 target에 낸다. 값을 돌려주는 호출의 첫 실패를 돌려준다.
    Result<void> Replay(graphics::CommandContext& target) const noexcept;

    // 기록 중 생긴 오류다(arena 할당 실패, 기록 목록에서 쓸 수 없는 호출). 없으면 None이다.
    ErrorCode RecordError() const noexcept {
        return error_;
    }

    usize CommandCount() const noexcept {
        return commands_.Size();
    }

    void SetPipeline(graphics::PipelineHandle pipeline) noexcept override;
    void SetVertexBuffers(uint32 firstStream,
                          Span<const graphics::VertexBufferBinding> buffers) noexcept override;
    void SetIndexBuffer(graphics::BufferHandle buffer, graphics::IndexFormat format,
                        uint64 offset) noexcept override;
    void SetTexture(graphics::TextureHandle texture) noexcept override;
    void SetBufferView(uint32 slot, graphics::BufferViewHandle view) noexcept override;
    Result<void> SetDrawConstants(Span<const uint8> bytes) noexcept override;
    void Draw(uint32 vertexCount, uint32 instanceCount, uint32 firstVertex) noexcept override;
    void DrawIndexed(uint32 indexCount, uint32 instanceCount, uint32 firstIndex) noexcept override;
    void DrawIndirect(graphics::BufferHandle arguments, uint64 offset) noexcept override;
    void DrawIndexedIndirect(graphics::BufferHandle arguments, uint64 offset) noexcept override;
    void Dispatch(uint32 groupCountX, uint32 groupCountY, uint32 groupCountZ) noexcept override;
    void DispatchIndirect(graphics::BufferHandle arguments, uint64 offset) noexcept override;
    void SetWritableBufferView(uint32 slot, graphics::BufferViewHandle view) noexcept override;
    void SetWritableTexture(uint32 slot, graphics::TextureHandle view) noexcept override;
    void SetViewport(const graphics::Viewport& viewport) noexcept override;
    void SetScissor(const graphics::ScissorRect& scissor) noexcept override;
    void SetStencilReference(uint32 reference) noexcept override;
    Result<void> CopyBuffer(graphics::BufferHandle destination, uint64 destinationOffset,
                            graphics::BufferHandle source, uint64 sourceOffset,
                            uint64 size) noexcept override;
    Result<void> CopyTexture(graphics::TextureHandle destination,
                             graphics::TextureHandle source) noexcept override;
    Result<void> ClearWritableBufferView(graphics::BufferViewHandle view,
                                         uint32 value) noexcept override;
    Result<void> CopyPassTarget(graphics::TextureHandle destination) noexcept override;
    void Transition(Span<const graphics::ResourceTransition> transitions) noexcept override;
    graphics::SplitTransition
    BeginTransitions(Span<const graphics::ResourceTransition> transitions) noexcept override;
    void EndTransitions(graphics::SplitTransition split,
                        Span<const graphics::ResourceTransition> transitions) noexcept override;
    Result<uint64> WriteTimestamp() noexcept override;
    void PushMarker(StringView name) noexcept override;
    void PopMarker() noexcept override;

private:
    enum class Op : uint8 {
        SetPipeline,
        SetVertexBuffers,
        SetIndexBuffer,
        SetTexture,
        SetBufferView,
        SetDrawConstants,
        Draw,
        DrawIndexed,
        DrawIndirect,
        DrawIndexedIndirect,
        Dispatch,
        DispatchIndirect,
        SetWritableBufferView,
        SetWritableTexture,
        SetViewport,
        SetScissor,
        SetStencilReference,
        CopyBuffer,
        CopyTexture,
        ClearWritableBufferView,
        CopyPassTarget,
        Transition,
        PushMarker,
        PopMarker
    };

    // 명령 하나다. 인자는 종류마다 아래 칸을 나눠 쓴다(handle 둘, 정수 셋, 64 bit 셋, 복사한 byte
    // 열, viewport·scissor).
    struct Command {
        Op op = Op::Draw;
        SlotHandle first;
        SlotHandle second;
        uint32 a = 0;
        uint32 b = 0;
        uint32 c = 0;
        uint64 x = 0;
        uint64 y = 0;
        uint64 z = 0;
        const void* data = nullptr;
        usize count = 0;
        graphics::Viewport viewport;
        graphics::ScissorRect scissor;
    };

    void Push(const Command& command) noexcept;
    // bytes byte를 arena에 복사한다(실패하면 null, 오류를 남긴다).
    const void* Copy(const void* data, usize bytes, usize alignment) noexcept;
    void Fail(ErrorCode code) noexcept;

    FrameArena& arena_;
    ArenaArray<Command> commands_;
    ErrorCode error_ = ErrorCode::None;
};

} // namespace rocks::render::detail
