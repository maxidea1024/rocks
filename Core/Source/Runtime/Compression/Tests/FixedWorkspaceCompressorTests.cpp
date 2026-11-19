#include "CompressionTestSupport.h"
#include <Compression/FixedWorkspaceCompressor.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Move.h>
#include <TestSupport/Test.h>

using namespace rocks;
namespace fixed_workspace_compressor_tests_file {

using namespace rocks::test::compression;

namespace {

constexpr usize MaxBatchBytes = 64 * 1024;
constexpr int32 TestLevel = 3;
constexpr uint32 Batches = 8;

} // namespace

// 만든 뒤에는 압축마다 할당하지 않고(CountingAllocator는 우리 할당기만 본다. zstd 내부 malloc이
// 없다는 근거는 정적 문맥이 크기를 다시 잡지 않고 오류를 낸다는 것이다 — 여러 크기의 묶음이 모두
// 성공하므로 고정 작업 공간으로 충분하다), 각 묶음은 독립 frame이라 이어 붙인 결과가 한 zstd
// 스트림으로 풀린다. 크기 상한을 넘는 입력·모자란 출력은 CapacityExceeded다.
TEST(FixedWorkspaceCompressorCompressesWithoutAllocating) {
    SystemAllocator system;
    CountingAllocator counting(system);
    auto settings = CompressionSettings::TryMake(CompressionFormat::Zstd, TestLevel);
    REQUIRE(settings);
    auto made = FixedWorkspaceCompressor::TryCreate(settings.Value(), MaxBatchBytes, counting);
    REQUIRE(made);
    FixedWorkspaceCompressor compressor = Move(made.Value());
    CHECK(compressor.WorkspaceBytes() != 0 && compressor.MaxSourceBytes() == MaxBatchBytes);

    Array<uint8> expected(system);
    Array<uint8> packed(system);
    Array<uint8> batch(system);
    Array<uint8> out(system);
    REQUIRE(out.TryResize(FixedWorkspaceCompressor::Bound(MaxBatchBytes)));
    const uint64 before = counting.Allocations();
    for (uint32 index = 0; index < Batches; ++index) {
        // 크기가 묶음마다 달라도(원본 크기 기록) 작업 공간을 다시 잡지 않는다.
        Fill(batch, MaxBatchBytes / (index + 1), index % 2 == 0 ? Pattern::Text : Pattern::Random,
             index + 1);
        auto written = compressor.TryCompress(batch.View(), out.View());
        REQUIRE(written);
        REQUIRE(expected.TryAppend(batch.View()));
        REQUIRE(packed.TryAppend(Span<const uint8>(out.Data(), written.Value())));
    }

    CHECK(counting.Allocations() == before);

    Array<uint8> restored(system);
    REQUIRE(TryDecompressAppend(CompressionFormat::Zstd, packed.View(), expected.Size(), restored));
    REQUIRE(restored.Size() == expected.Size());
    bool same = true;
    for (usize index = 0; index < expected.Size(); ++index) {
        same = same && restored[index] == expected[index];
    }

    CHECK(same);

    Array<uint8> large(system);
    Fill(large, MaxBatchBytes + 1, Pattern::Text);
    CHECK(compressor.TryCompress(large.View(), out.View()).Error() == ErrorCode::CapacityExceeded);
    uint8 tiny[4];
    CHECK(compressor.TryCompress(batch.View(), Span<uint8>(tiny)).Error() ==
          ErrorCode::CapacityExceeded);
    // 실패 뒤에도 다음 호출은 성공한다.
    CHECK(compressor.TryCompress(batch.View(), out.View()));

    FixedWorkspaceCompressor moved = Move(compressor);
    CHECK(compressor.TryCompress(batch.View(), out.View()).Error() == ErrorCode::StaleHandle);
    CHECK(moved.TryCompress(batch.View(), out.View()));
}

TEST(FixedWorkspaceCompressorRejectsOtherFormats) {
    SystemAllocator system;
    auto lz4 = CompressionSettings::TryDefault(CompressionFormat::Lz4);
    REQUIRE(lz4);
    CHECK(FixedWorkspaceCompressor::TryCreate(lz4.Value(), MaxBatchBytes, system).Error() ==
          ErrorCode::Unsupported);
    auto zstd = CompressionSettings::TryDefault(CompressionFormat::Zstd);
    REQUIRE(zstd);
    CHECK(FixedWorkspaceCompressor::TryCreate(zstd.Value(), 0, system).Error() ==
          ErrorCode::InvalidArgument);
}
} // namespace fixed_workspace_compressor_tests_file
