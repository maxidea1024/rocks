#include "CompressionTestSupport.h"

#include <Compression/AsyncCompression.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Move.h>
#include <TestSupport/Test.h>

// 내부 경계(취소 probe)를 결정적으로 시험한다. 시험은 저장소 루트를 include 경로로 쓴다(PathTests와
// 같은 방식).
#include <Core/Source/Runtime/Compression/Private/CompressionInternal.h>

namespace rocks::test {
namespace {

using namespace compression;
using Body = Function<void(WorkerTaskContext&)>;

// N번째 확인부터 취소를 보고하는 probe다.
struct CountdownProbe {
    uint32 remaining;

    static bool Check(void* raw) noexcept {
        auto& self = *static_cast<CountdownProbe*>(raw);
        if (self.remaining == 0) {
            return true;
        }
        --self.remaining;
        return false;
    }
};

bool IsTerminal(const WorkerTaskHandle& handle) {
    auto status = handle.Status();
    return status && (status.Value().state == WorkerTaskState::Succeeded ||
                      status.Value().state == WorkerTaskState::Failed ||
                      status.Value().state == WorkerTaskState::Cancelled);
}

} // namespace

// 분할 루프의 취소 지점: 입력·출력 1MiB마다 확인하며, 취소되면 Append 출력 길이를 되돌린다.
TEST(Compression_CancellationProbeStopsChunkedCodecs) {
    SystemAllocator allocator;
    {
        Array<uint8> original(allocator);
        Fill(original, (usize{1} << 20) * 5, Pattern::Text, 21);
        static constexpr CompressionFormat Cancellable[] = {
            CompressionFormat::Zstd, CompressionFormat::Deflate, CompressionFormat::Zlib,
            CompressionFormat::Gzip};
        for (CompressionFormat format : Cancellable) {
            Array<uint8> packed(allocator);
            REQUIRE(TryCompressAppend(DefaultSettings(format), View(original), packed));
            for (uint32 checks = 0; checks < 3; ++checks) {
                CountdownProbe countdown{checks};
                detail::CompressionProbe probe{&CountdownProbe::Check, &countdown};

                Array<uint8> compressed(allocator);
                auto compressedResult = detail::CompressAppendOnce(
                    DefaultSettings(format), View(original), compressed, allocator, probe);
                CHECK(compressedResult.Error() == ErrorCode::Cancelled);
                CHECK(compressed.Size() == 0);

                countdown.remaining = checks;
                Array<uint8> restored(allocator);
                auto restoredResult = detail::DecompressAppendOnce(
                    format, View(packed), original.Size(), restored, allocator, probe);
                CHECK(restoredResult.Error() == ErrorCode::Cancelled);
                CHECK(restored.Size() == 0);
            }
        }
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(Compression_AsyncMatchesSyncAndChainsDependencies) {
    SystemAllocator allocator;
    {
        WorkerScheduler scheduler(allocator);
        REQUIRE(scheduler.Start({.workerCount = 8, .maxInFlight = 256}));
        {
            constexpr uint32 Jobs = 40;
            // 작업이 output 주소를 보관하므로 바깥 배열은 미리 확보해 재배치되지 않게 한다.
            struct Slot {
                explicit Slot(Allocator& allocator) noexcept
                    : original(allocator), packed(allocator), restored(allocator) {
                }

                Array<uint8> original;
                Array<uint8> packed;
                Array<uint8> restored;
            };
            Array<Slot> slots(allocator);
            REQUIRE(slots.TryReserve(Jobs));
            for (uint32 job = 0; job < Jobs; ++job) {
                REQUIRE(slots.TryPushBack(Slot(allocator)));
            }
            CompressionTask compressTasks[Jobs];
            CompressionTask decompressTasks[Jobs];

            for (uint32 job = 0; job < Jobs; ++job) {
                const CompressionFormat format = AllFormats[job % 5];
                Fill(slots[job].original, 20000 + job * 7919,
                     job % 3 == 0 ? Pattern::Random : Pattern::Text, job + 1);
                REQUIRE(TrySubmitCompress(scheduler, compressTasks[job], DefaultSettings(format),
                                          View(slots[job].original), slots[job].packed));
                // 압축 결과가 해제 입력이 된다. source Span은 제출 시점에 고정되므로 압축 작업이
                // 같은 output에 쓰는 대신 해제는 압축 완료 뒤에 따로 제출한다.
            }
            for (uint32 job = 0; job < Jobs; ++job) {
                auto bytes = compressTasks[job].TryWaitResult();
                REQUIRE(bytes);
                CHECK(bytes.Value() == slots[job].packed.Size());

                // 동기 API와 byte 단위로 같다.
                Array<uint8> expected(allocator);
                REQUIRE(TryCompressAppend(DefaultSettings(AllFormats[job % 5]),
                                          View(slots[job].original), expected));
                CHECK(Same(slots[job].packed, View(expected)));

                REQUIRE(TrySubmitDecompress(
                    scheduler, decompressTasks[job], AllFormats[job % 5], View(slots[job].packed),
                    slots[job].original.Size(), slots[job].restored,
                    Span<const WorkerTaskHandle>(&compressTasks[job].Handle(), 1)));
            }
            for (uint32 job = 0; job < Jobs; ++job) {
                auto bytes = decompressTasks[job].TryWaitResult();
                CHECK(bytes && bytes.Value() == slots[job].original.Size());
                CHECK(Same(slots[job].restored, View(slots[job].original)));
            }
        }
        REQUIRE(scheduler.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(Compression_AsyncResultStatesCancellationAndFailures) {
    SystemAllocator allocator;
    {
        WorkerScheduler scheduler(allocator);
        REQUIRE(scheduler.Start({.workerCount = 2, .maxInFlight = 64}));
        {
            Array<uint8> original(allocator);
            Fill(original, 50000, Pattern::Text, 9);
            Array<uint8> packed(allocator);
            REQUIRE(TryCompressAppend(DefaultSettings(CompressionFormat::Zstd), View(original),
                                      packed));

            // 제출 전에는 StaleHandle이다.
            CompressionTask idle;
            CHECK(idle.TryGetResult().Error() == ErrorCode::StaleHandle);
            CHECK(idle.TryWaitResult(0).Error() == ErrorCode::StaleHandle);

            // 막힌 의존 작업 뒤에서 pending: Busy, 재제출 Busy, 취소하면 실행 없이 Cancelled이고
            // 출력은 그대로다.
            platform::ManualResetEvent release;
            auto blockerBody = Body::TryCreate(allocator, [&release](WorkerTaskContext&) noexcept {
                (void)release.Wait(platform::InfiniteTimeout);
            });
            REQUIRE(blockerBody);
            auto blocker = scheduler.Submit(Move(blockerBody.Value()));
            REQUIRE(blocker);

            CompressionTask pending;
            Array<uint8> output(allocator);
            REQUIRE(output.TryAppend(Span<const uint8>(packed.Data(), 3)));
            REQUIRE(TrySubmitDecompress(scheduler, pending, CompressionFormat::Zstd, View(packed),
                                        original.Size(), output,
                                        Span<const WorkerTaskHandle>(&blocker.Value(), 1)));
            CHECK(pending.TryGetResult().Error() == ErrorCode::Busy);
            CHECK(pending.TryWaitResult(0).Error() == ErrorCode::Busy);
            CHECK(TrySubmitDecompress(scheduler, pending, CompressionFormat::Zstd, View(packed),
                                      original.Size(), output)
                      .Error() == ErrorCode::Busy);
            REQUIRE(pending.Handle().RequestCancel());
            release.Signal();
            CHECK(pending.TryWaitResult().Error() == ErrorCode::Cancelled);
            CHECK(output.Size() == 3);

            // terminal 뒤에는 같은 객체로 다시 제출할 수 있다.
            REQUIRE(TrySubmitDecompress(scheduler, pending, CompressionFormat::Zstd, View(packed),
                                        original.Size(), output));
            auto again = pending.TryWaitResult();
            CHECK(again && again.Value() == original.Size());
            CHECK(Same(Span<const uint8>(output.Data() + 3, output.Size() - 3), View(original)));

            // 코덱 오류는 결과로 전달되고 이 작업에 의존한 후속 작업은 실행되지 않는다.
            Array<uint8> damaged(allocator);
            REQUIRE(damaged.TryAppend(View(packed)));
            damaged[damaged.Size() - 1] =
                static_cast<uint8>(damaged[damaged.Size() - 1] ^ 0x55); // checksum
            CompressionTask failing;
            Array<uint8> failingOutput(allocator);
            REQUIRE(TrySubmitDecompress(scheduler, failing, CompressionFormat::Zstd, View(damaged),
                                        original.Size(), failingOutput));
            CompressionTask dependent;
            Array<uint8> dependentOutput(allocator);
            REQUIRE(TrySubmitCompress(scheduler, dependent, DefaultSettings(CompressionFormat::Lz4),
                                      View(original), dependentOutput,
                                      Span<const WorkerTaskHandle>(&failing.Handle(), 1)));
            CHECK(failing.TryWaitResult().Error() == ErrorCode::InvalidArgument);
            CHECK(failingOutput.Size() == 0);
            CHECK(dependent.TryWaitResult().Error() == ErrorCode::DependencyFailed);
            CHECK(dependentOutput.Size() == 0);

            // 크기를 아는 Span 변형과 동기 검증 오류.
            Array<uint8> fixed(allocator);
            REQUIRE(fixed.TryResize(original.Size()));
            CompressionTask spanTask;
            REQUIRE(TrySubmitDecompress(scheduler, spanTask, CompressionFormat::Zstd, View(packed),
                                        Span<uint8>(fixed.Data(), fixed.Size()), allocator));
            auto fixedBytes = spanTask.TryWaitResult();
            CHECK(fixedBytes && fixedBytes.Value() == original.Size() &&
                  Same(fixed, View(original)));
            CompressionTask rejected;
            CHECK(TrySubmitDecompress(scheduler, rejected, CompressionFormat::Zstd,
                                      Span<const uint8>(), 10, output)
                      .Error() == ErrorCode::InvalidArgument);
            CHECK(rejected.TryGetResult().Error() == ErrorCode::StaleHandle);
            CHECK(IsTerminal(pending.Handle()));
        }
        REQUIRE(scheduler.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 종료된 scheduler에는 제출할 수 없고 task는 바뀌지 않는다.
TEST(Compression_AsyncSubmitAfterShutdownIsRejected) {
    SystemAllocator allocator;
    {
        WorkerScheduler scheduler(allocator);
        REQUIRE(scheduler.Start({.workerCount = 1}));
        REQUIRE(scheduler.Shutdown());
        {
            Array<uint8> original(allocator);
            Fill(original, 1000, Pattern::Text);
            Array<uint8> output(allocator);
            CompressionTask task;
            CHECK(!TrySubmitCompress(scheduler, task, DefaultSettings(CompressionFormat::Gzip),
                                     View(original), output));
            CHECK(task.TryGetResult().Error() == ErrorCode::StaleHandle);
            CHECK(output.Size() == 0);
        }
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

} // namespace rocks::test
