#pragma once

#include <Core/Result.h>
#include <TextureTools/TextureBlockEncode.h>

// 블록 압축의 행 병렬 실행이다(Docs/Design/AssetSystem 12.17.9 2 — TC5a). 블록이 서로 독립이므로
// (slice, 블록 행) 단위를 batch로 나눠 순서 없이 압축해도 byte가 같다. 모든 batch는 결정적 부동소수
// 환경(platform::RunWithDeterministicFloatingPoint) 안에서 돈다 — FP 제어는 스레드마다라 worker의
// 설정이 결과를 바꾸지 않게.
namespace rocks::texture_block_detail {

// 블록 행 [begin, end)를 압축하는 본문이다. 행 번호는 slice · blocksY + 블록 행이다. 범위마다
// 겹치지 않는 출력을 쓴다.
using BlockRowBody = void (*)(void* context, uint64 begin, uint64 end) noexcept;

// BC1–BC5 최소 batch 블록 수다(UE TextureFormatDXT 2048블록 — TC5b에서 측정해 확정).
inline constexpr uint32 BcMinBatchBlocks = 2048;
// BC6H·BC7·EAC 최소 batch 블록 수다(1 — 블록 행 하나, UE ISPC와 같은 단위).
inline constexpr uint32 RowMinBatchBlocks = 1;

// rows개 블록 행을 실행한다. 행 하나는 blocksPerRow 블록이다. batch 하나는
// options.minBatchBlocks(0이면 defaultMinBatchBlocks) 이상의 블록이 되게 행을 묶는다.
// options.scheduler가 없으면 호출 스레드 혼자 실행한다(같은 scheduler의 worker 안이거나 helper
// 제출이 실패해도 ParallelFor 규칙대로 호출 스레드가 나머지를 실행한다 — 결과는 같다). 호출자가
// options.scratch를 확인했다(scheduler가 있으면 필수).
Result<void> RunBlockRows(const TextureBlockOptions& options, uint64 rows, uint64 blocksPerRow,
                          uint32 defaultMinBatchBlocks, void* context, BlockRowBody body) noexcept;

// options로 동시에 압축에 참여할 수 있는 스레드 수다(scheduler worker 수 + 1, scheduler가 없거나
// 시작 전·종료 뒤(새 제출을 받지 않음)이거나 그 worker 안에서 부르면 1). limit 이하로 자른다.
uint32 BlockParticipants(const TextureBlockOptions& options, uint32 limit) noexcept;

} // namespace rocks::texture_block_detail
