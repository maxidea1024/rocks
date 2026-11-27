#include "TextureBlockParallel.h"

#include <Platform/FloatingPointEnvironment.h>
#include <Task/ParallelFor.h>
#include <Task/WorkerScheduler.h>

namespace rocks::texture_block_detail {

namespace {

// 유니티 빌드에서 같은 묶음의 이름과 부딪히지 않게 이름 있는 namespace에 둔다.
namespace block_parallel {

struct Rows {
    void* context;
    BlockRowBody body;
    uint64 rowsPerBatch;
    uint64 rows;
};

struct Range {
    const Rows* rows;
    uint64 begin;
    uint64 end;
};

void RunRange(void* opaque) noexcept {
    const Range& range = *static_cast<const Range*>(opaque);
    range.rows->body(range.rows->context, range.begin, range.end);
}

// batch [first, last)의 행을 결정적 FP 환경 안에서 실행한다.
void RunBatches(const Rows& rows, usize first, usize last) noexcept {
    for (usize batch = first; batch < last; ++batch) {
        const uint64 begin = uint64{batch} * rows.rowsPerBatch;
        const uint64 end =
            begin + rows.rowsPerBatch < rows.rows ? begin + rows.rowsPerBatch : rows.rows;
        Range range{&rows, begin, end};
        platform::RunWithDeterministicFloatingPoint(&RunRange, &range);
    }
}

} // namespace block_parallel

} // namespace

Result<void> RunBlockRows(const TextureBlockOptions& options, uint64 rows, uint64 blocksPerRow,
                          uint32 defaultMinBatchBlocks, void* context, BlockRowBody body) noexcept {
    using namespace block_parallel;
    if (rows == 0) {
        return Result<void>::Success();
    }

    const uint64 minBlocks =
        options.minBatchBlocks != 0 ? options.minBatchBlocks : defaultMinBatchBlocks;
    const uint64 perRow = blocksPerRow == 0 ? 1 : blocksPerRow;
    uint64 rowsPerBatch = (minBlocks + perRow - 1) / perRow;
    rowsPerBatch = rowsPerBatch == 0 ? 1 : rowsPerBatch;
    const Rows plan{context, body, rowsPerBatch, rows};
    const uint64 batches = (rows + rowsPerBatch - 1) / rowsPerBatch;
    if (options.scheduler == nullptr || batches == 1) {
        RunBatches(plan, 0, static_cast<usize>(batches));
        return Result<void>::Success();
    }

    return ParallelFor(*options.scheduler, *options.scratch, static_cast<usize>(batches),
                       [&plan](usize begin, usize end) noexcept { RunBatches(plan, begin, end); });
}

uint32 BlockParticipants(const TextureBlockOptions& options, uint32 limit) noexcept {
    if (options.scheduler == nullptr || options.scheduler->IsCurrentWorker()) {
        return 1;
    }

    const WorkerSchedulerStats stats = options.scheduler->Stats();
    if (!stats.accepting) {
        return 1;
    }

    const uint32 count = stats.workerCount + 1;
    return count < limit ? count : limit;
}

} // namespace rocks::texture_block_detail
