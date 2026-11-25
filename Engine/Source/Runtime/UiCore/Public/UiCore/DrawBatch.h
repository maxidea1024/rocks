#pragma once

#include <Base/NumericLimits.h>
#include <Core/Algorithms.h>
#include <Core/Math/MathUtility.h>
#include <UiCore/PaintList.h>

namespace rocks::ui {

class CacheRoot;

// UI 사각형의 GPU 입력이다. 색상은 선형이며 alpha를 미리 곱하지 않는다.
struct DrawVertex {
    Vec2 position;
    Vec2 uv;
    LinearColor color;
};

struct DrawVertexRange {
    uint32 first;
    uint32 count;
};

// 같은 clip 상태로 연속 기록된 인덱스 범위다.
struct DrawCommand {
    uint32 firstIndex = 0;
    uint32 indexCount = 0;
    bool hasClip = false;
    Box2<float> clip;
    SlotHandle resource{};
    PaintOperation operation = PaintOperation::Draw;
    Vec4 cornerRadius{};
};

// CPU 그리기 목록을 정점과 인덱스로 변환한다. Graphics 자원이나 GPU 수명은 소유하지 않는다.
class ROCKS_UICORE_API DrawBatch {
public:
    explicit DrawBatch(Allocator& allocator) noexcept;

    // layer 오름차순으로 기록하고 같은 layer는 원래 순서를 유지한다. 실패하면 이전 결과를 보존한다.
    Result<void> Build(const PaintList& list) noexcept;
    // 같은 캐시의 바로 다음 판이면 변경 범위에 대응하는 정점만 교체한다.
    Result<void> Update(const CacheRoot& cache) noexcept;

    Span<const DrawVertex> Vertices() const noexcept;

    Span<const uint32> Indices() const noexcept;

    Span<const DrawCommand> Commands() const noexcept;
    uint64 Revision() const noexcept;
    uint64 TopologyRevision() const noexcept;
    Span<const DrawVertexRange> ChangedVertices() const noexcept;

private:
    struct PaintOrder {
        usize index;
        int32 layer;
    };

    Array<DrawVertex> vertices_;
    Array<uint32> indices_;
    Array<DrawCommand> commands_;
    Array<DrawVertex> scratchVertices_;
    Array<uint32> scratchIndices_;
    Array<DrawCommand> scratchCommands_;
    Array<PaintOrder> scratchOrder_;
    Array<uint32> firstVertex_, scratchFirstVertex_;
    const CacheRoot* cache_ = nullptr;
    uint64 revision_ = 0;
    uint64 batchRevision_ = 0, topologyRevision_ = 0;
    Array<DrawVertexRange> changedVertices_, scratchRanges_;
};

} // namespace rocks::ui
