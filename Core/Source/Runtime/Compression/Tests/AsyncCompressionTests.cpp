#include "CompressionTestSupport.h"

#include <Compression/AsyncCompression.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Move.h>
#include <Core/ScopeExit.h>
#include <Platform/AsyncFile.h>
#include <Platform/Clock.h>
#include <Platform/File.h>
#include <Task/AsyncIoEvent.h>
#include <TestSupport/Test.h>

#include <stdio.h>

// 내부 경계(취소 probe)를 결정적으로 시험한다. 시험은 저장소 루트를 include 경로로 쓴다(PathTests와
// 같은 방식).
#include <Compression/Private/CompressionInternal.h>

namespace rocks::test {
namespace {

using namespace compression;
using Body = Function<void(WorkerTaskContext&)>;

// N번째 확인부터 취소를 보고하는 probe다. 확인 횟수도 센다.
struct CountdownProbe {
    uint32 remaining;
    uint32 checks = 0;

    static bool Check(void* raw) noexcept {
        auto& self = *static_cast<CountdownProbe*>(raw);
        ++self.checks;
        if (self.remaining == 0) {
            return true;
        }
        --self.remaining;
        return false;
    }
};

WorkerTaskState StateOf(const WorkerTaskHandle& handle) {
    return handle.Status().Value().state;
}

} // namespace

// 분할 루프의 취소 지점: 시작 확인 뒤에도 코덱 루프 안에서 취소를 관측하며(checks 1·2는 진행 중
// 취소), 취소되면 Append 출력 길이를 호출 전 길이(비어 있지 않은 base)로 되돌린다.
TEST(Compression_CancellationProbeStopsChunkedCodecs) {
    SystemAllocator allocator;
    {
        Array<uint8> original(allocator);
        Fill(original, (usize{1} << 20) * 5, Pattern::Text, 21);
        static constexpr CompressionFormat Cancellable[] = {
            CompressionFormat::Zstd, CompressionFormat::Deflate, CompressionFormat::Zlib,
            CompressionFormat::Gzip};
        static constexpr uint8 Base[] = {'k', 'e', 'e', 'p'};
        for (CompressionFormat format : Cancellable) {
            Array<uint8> packed(allocator);
            REQUIRE(TryCompressAppend(DefaultSettings(format), View(original), packed));
            for (uint32 checks = 0; checks < 3; ++checks) {
                CountdownProbe countdown{checks};
                detail::CompressionProbe probe{&CountdownProbe::Check, &countdown};

                Array<uint8> compressed(allocator);
                REQUIRE(compressed.TryAppend(Span<const uint8>(Base, 4)));
                auto compressedResult =
                    detail::CompressAppendOnce(format, DefaultSettings(format).Level(),
                                               View(original), compressed, allocator, probe);
                CHECK(compressedResult.Error() == ErrorCode::Cancelled);
                CHECK(Same(compressed, Span<const uint8>(Base, 4)));
                CHECK(countdown.checks == checks + 1);

                countdown = CountdownProbe{checks};
                Array<uint8> restored(allocator);
                REQUIRE(restored.TryAppend(Span<const uint8>(Base, 4)));
                auto restoredResult = detail::DecompressAppendOnce(
                    format, View(packed), original.Size(), restored, allocator, probe);
                CHECK(restoredResult.Error() == ErrorCode::Cancelled);
                CHECK(Same(restored, Span<const uint8>(Base, 4)));

                // 크기를 아는 Span 해제 경로도 같은 지점에서 멈춘다.
                countdown = CountdownProbe{checks};
                Array<uint8> fixed(allocator);
                REQUIRE(fixed.TryResize(original.Size()));
                auto fixedResult = detail::DecompressIntoOnce(
                    format, View(packed), Span<uint8>(fixed.Data(), fixed.Size()), allocator,
                    probe);
                CHECK(fixedResult.Error() == ErrorCode::Cancelled);
            }
        }

        // Lz4는 코덱 호출 전 한 번만 확인한다.
        Array<uint8> packed(allocator);
        REQUIRE(TryCompressAppend(DefaultSettings(CompressionFormat::Lz4), View(original), packed));
        CountdownProbe countdown{0};
        detail::CompressionProbe probe{&CountdownProbe::Check, &countdown};
        Array<uint8> restored(allocator);
        CHECK(detail::DecompressAppendOnce(CompressionFormat::Lz4, View(packed), original.Size(),
                                           restored, allocator, probe)
                  .Error() == ErrorCode::Cancelled);
        CHECK(restored.Size() == 0);
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
                // 앞의 5개는 분할 단위(1MiB)를 넘겨 동기·비동기의 스트림 경계가 같은지도 확인한다.
                const usize size = job < 5 ? (usize{3} << 20) + job : 20000 + job * 7919;
                Fill(slots[job].original, size, job % 3 == 0 ? Pattern::Random : Pattern::Text,
                     job + 1);
                REQUIRE(TrySubmitCompress(scheduler, compressTasks[job], DefaultSettings(format),
                                          View(slots[job].original), slots[job].packed));
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

// 진행 중인 앞 작업에 의존해 같은 output에 이어 붙인다. 제출은 output을 읽지 않으므로(데이터 경합
// 없음) 앞 작업이 output을 키우는 동안에도 제출할 수 있고, 뒤 작업은 앞 작업의 결과 뒤에 붙인다.
TEST(Compression_AsyncChainedAppendsShareOneOutput) {
    SystemAllocator allocator;
    {
        SystemAllocator schedulerAllocator;
        PausingAllocator pausing(allocator);
        WorkerScheduler scheduler(schedulerAllocator);
        REQUIRE(scheduler.Start({.workerCount = 2, .maxInFlight = 16}));
        {
            Array<uint8> first(allocator);
            Fill(first, 400000, Pattern::Text, 31);
            Array<uint8> second(allocator);
            Fill(second, 300000, Pattern::Random, 32);
            Array<uint8> output(pausing);
            CompressionTask head;
            CompressionTask tail;

            // 앞 작업을 body 안 첫 할당(코덱 문맥)에서 멈춰 둔 채 뒤 작업을 제출한다(1번째는 body
            // 저장소).
            pausing.PauseOnAllocation(2);
            REQUIRE(TrySubmitCompress(scheduler, head, DefaultSettings(CompressionFormat::Zstd),
                                      View(first), output));
            REQUIRE(pausing.WaitPaused());
            REQUIRE(TrySubmitCompress(scheduler, tail, DefaultSettings(CompressionFormat::Gzip),
                                      View(second), output,
                                      Span<const WorkerTaskHandle>(&head.Handle(), 1)));
            pausing.Resume();

            auto headBytes = head.TryWaitResult();
            auto tailBytes = tail.TryWaitResult();
            REQUIRE(headBytes && tailBytes);
            CHECK(output.Size() == headBytes.Value() + tailBytes.Value());

            Array<uint8> expected(allocator);
            REQUIRE(
                TryCompressAppend(DefaultSettings(CompressionFormat::Zstd), View(first), expected));
            REQUIRE(TryCompressAppend(DefaultSettings(CompressionFormat::Gzip), View(second),
                                      expected));
            CHECK(Same(output, View(expected)));
        }
        REQUIRE(scheduler.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 실행 중 취소: body가 코덱 문맥을 할당하는 지점에서 멈춘 동안 취소를 요청하면, 코덱 루프가 다음
// 확인에서 관측해 Cancelled(Failed 아님)로 끝나고 output은 호출 전 길이로 돌아간다. 후속 작업은
// DependencyFailed다.
TEST(Compression_AsyncRunningCancellation) {
    SystemAllocator allocator;
    {
        SystemAllocator schedulerAllocator;
        PausingAllocator pausing(allocator);
        WorkerScheduler scheduler(schedulerAllocator);
        REQUIRE(scheduler.Start({.workerCount = 2, .maxInFlight = 16}));
        {
            Array<uint8> original(allocator);
            Fill(original, (usize{1} << 20) * 4, Pattern::Text, 41);
            Array<uint8> output(pausing);
            REQUIRE(output.TryAppend(Span<const uint8>(original.Data(), 5)));
            Array<uint8> dependentOutput(allocator);
            CompressionTask running;
            CompressionTask dependent;

            pausing.PauseOnAllocation(2);
            REQUIRE(TrySubmitCompress(scheduler, running, DefaultSettings(CompressionFormat::Zlib),
                                      View(original), output));
            REQUIRE(TrySubmitCompress(scheduler, dependent, DefaultSettings(CompressionFormat::Lz4),
                                      View(original), dependentOutput,
                                      Span<const WorkerTaskHandle>(&running.Handle(), 1)));
            REQUIRE(pausing.WaitPaused());
            CHECK(StateOf(running.Handle()) == WorkerTaskState::Running);
            REQUIRE(running.Handle().RequestCancel());
            pausing.Resume();

            CHECK(running.TryWaitResult().Error() == ErrorCode::Cancelled);
            CHECK(StateOf(running.Handle()) == WorkerTaskState::Cancelled);
            CHECK(output.Size() == 5);
            CHECK(dependent.TryWaitResult().Error() == ErrorCode::DependencyFailed);
            CHECK(dependentOutput.Size() == 0);
        }
        REQUIRE(scheduler.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 늦은 취소: body가 성공한 뒤 capture 정리 중에 취소가 걸리면 handle은 Cancelled지만 결과는
// 성공이며 데이터도 유효하다. 이 handle에 의존한 후속 작업은 실행되지 않는다(DependencyFailed).
TEST(Compression_AsyncLateCancellationKeepsSuccessfulResult) {
    SystemAllocator allocator;
    {
        SystemAllocator schedulerAllocator;
        PausingAllocator pausing(allocator);
        WorkerScheduler scheduler(schedulerAllocator);
        REQUIRE(scheduler.Start({.workerCount = 2, .maxInFlight = 16}));
        {
            Array<uint8> original(allocator);
            Fill(original, 200000, Pattern::Text, 51);
            Array<uint8> output(pausing);
            Array<uint8> dependentOutput(allocator);
            CompressionTask late;
            CompressionTask dependent;

            // 1번째 할당은 제출 스레드의 body 저장소다. 그 해제(capture 정리, body 반환 뒤) 직전에
            // 멈춘다.
            pausing.PauseOnDeallocationOf(1);
            REQUIRE(TrySubmitCompress(scheduler, late, DefaultSettings(CompressionFormat::Zstd),
                                      View(original), output));
            REQUIRE(TrySubmitCompress(scheduler, dependent, DefaultSettings(CompressionFormat::Lz4),
                                      View(original), dependentOutput,
                                      Span<const WorkerTaskHandle>(&late.Handle(), 1)));
            REQUIRE(pausing.WaitPaused());
            REQUIRE(late.Handle().RequestCancel());
            pausing.Resume();

            auto bytes = late.TryWaitResult();
            CHECK(bytes && bytes.Value() == output.Size());
            CHECK(StateOf(late.Handle()) == WorkerTaskState::Cancelled);
            Array<uint8> expected(allocator);
            REQUIRE(TryCompressAppend(DefaultSettings(CompressionFormat::Zstd), View(original),
                                      expected));
            CHECK(Same(output, View(expected)));
            CHECK(dependent.TryWaitResult().Error() == ErrorCode::DependencyFailed);
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
            // 수명 규칙: source·output을 먼저, task를 나중에 선언한다(task가 먼저 소멸한다).
            Array<uint8> original(allocator);
            Fill(original, 50000, Pattern::Text, 9);
            Array<uint8> packed(allocator);
            REQUIRE(TryCompressAppend(DefaultSettings(CompressionFormat::Zstd), View(original),
                                      packed));
            Array<uint8> damaged(allocator);
            REQUIRE(damaged.TryAppend(View(packed)));
            damaged[damaged.Size() - 1] = static_cast<uint8>(damaged[damaged.Size() - 1] ^ 0x55);
            Array<uint8> output(allocator);
            REQUIRE(output.TryAppend(Span<const uint8>(packed.Data(), 3)));
            Array<uint8> failingOutput(allocator);
            Array<uint8> dependentOutput(allocator);
            Array<uint8> fixed(allocator);
            REQUIRE(fixed.TryResize(original.Size()));
            platform::ManualResetEvent release;

            CompressionTask idle;
            CompressionTask pending;
            CompressionTask failing;
            CompressionTask dependent;
            CompressionTask spanTask;
            CompressionTask rejected;

            // 제출 전에는 StaleHandle이다.
            CHECK(idle.TryGetResult().Error() == ErrorCode::StaleHandle);
            CHECK(idle.TryWaitResult(0).Error() == ErrorCode::StaleHandle);

            // 막힌 의존 작업 뒤에서 pending: Busy, 재제출 Busy, 취소하면 실행 없이 Cancelled이고
            // 출력은 그대로다.
            auto blockerBody = Body::TryCreate(allocator, [&release](WorkerTaskContext&) noexcept {
                (void)release.Wait(platform::InfiniteTimeout);
            });
            REQUIRE(blockerBody);
            auto blocker = scheduler.Submit(Move(blockerBody.Value()));
            REQUIRE(blocker);

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
            // release를 쓰는 blocker가 끝난 뒤에만 release를 파괴한다.
            CHECK(blocker.Value().Wait() && StateOf(blocker.Value()) == WorkerTaskState::Succeeded);

            // terminal 뒤에는 같은 객체로 다시 제출할 수 있다.
            REQUIRE(TrySubmitDecompress(scheduler, pending, CompressionFormat::Zstd, View(packed),
                                        original.Size(), output));
            auto again = pending.TryWaitResult();
            CHECK(again && again.Value() == original.Size());
            CHECK(Same(Span<const uint8>(output.Data() + 3, output.Size() - 3), View(original)));

            // 코덱 오류(체크섬 불일치)는 결과로 전달되고 이 작업에 의존한 후속 작업은 실행되지
            // 않는다.
            REQUIRE(TrySubmitDecompress(scheduler, failing, CompressionFormat::Zstd, View(damaged),
                                        original.Size(), failingOutput));
            REQUIRE(TrySubmitCompress(scheduler, dependent, DefaultSettings(CompressionFormat::Lz4),
                                      View(original), dependentOutput,
                                      Span<const WorkerTaskHandle>(&failing.Handle(), 1)));
            CHECK(failing.TryWaitResult().Error() == ErrorCode::InvalidArgument);
            CHECK(StateOf(failing.Handle()) == WorkerTaskState::Failed);
            CHECK(failingOutput.Size() == 0);
            CHECK(dependent.TryWaitResult().Error() == ErrorCode::DependencyFailed);
            CHECK(dependentOutput.Size() == 0);

            // 크기를 아는 Span 변형과 동기 검증 오류.
            REQUIRE(TrySubmitDecompress(scheduler, spanTask, CompressionFormat::Zstd, View(packed),
                                        Span<uint8>(fixed.Data(), fixed.Size()), allocator));
            auto fixedBytes = spanTask.TryWaitResult();
            CHECK(fixedBytes && fixedBytes.Value() == original.Size() &&
                  Same(fixed, View(original)));
            CHECK(TrySubmitDecompress(scheduler, rejected, CompressionFormat::Zstd,
                                      Span<const uint8>(), 10, output)
                      .Error() == ErrorCode::InvalidArgument);
            CHECK(rejected.TryGetResult().Error() == ErrorCode::StaleHandle);

            // source가 output 저장소를 가리키면 body가 InvalidArgument로 보고한다(제출은 output을
            // 읽지 않는다).
            CompressionTask overlapping;
            REQUIRE(TrySubmitCompress(scheduler, overlapping,
                                      DefaultSettings(CompressionFormat::Lz4),
                                      Span<const uint8>(output.Data(), 4), output));
            CHECK(overlapping.TryWaitResult().Error() == ErrorCode::InvalidArgument);
        }
        REQUIRE(scheduler.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 종료된 scheduler에는 제출할 수 없고(ShuttingDown) task와 output은 바뀌지 않는다.
TEST(Compression_AsyncSubmitAfterShutdownIsRejected) {
    SystemAllocator allocator;
    {
        WorkerScheduler scheduler(allocator);
        REQUIRE(scheduler.Start({.workerCount = 1}));
        REQUIRE(scheduler.Shutdown());
        Array<uint8> original(allocator);
        Fill(original, 1000, Pattern::Text);
        Array<uint8> output(allocator);
        CompressionTask task;
        CHECK(TrySubmitCompress(scheduler, task, DefaultSettings(CompressionFormat::Gzip),
                                View(original), output)
                  .Error() == ErrorCode::ShuttingDown);
        CHECK(task.TryGetResult().Error() == ErrorCode::StaleHandle);
        CHECK(output.Size() == 0);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// AsyncFile 완료 → 해제: 비동기 읽기 완료가 외부 이벤트(TrySubmitReadEvent)를 완료하고, 그 이벤트에
// 의존한 해제 작업이 읽은 버퍼를 푼다. 짧은 읽기(파일이 기대보다 짧음)는 이벤트 실패로 해제가
// DependencyFailed다. 반복마다 모든 객체를 바로 소멸해 완료 콜백과의 수명 경합이 없는지 본다(ASan).
TEST(Compression_AsyncDecompressAfterAsyncFileRead) {
    SystemAllocator allocator;
    {
        Array<uint8> original(allocator);
        Fill(original, 200000, Pattern::Text, 5);
        Array<uint8> packed(allocator);
        REQUIRE(
            TryCompressAppend(DefaultSettings(CompressionFormat::Zstd), View(original), packed));

        char path[160] = {};
        const int count =
            snprintf(path, sizeof(path), "rocks-async-decompress-%llu.tmp",
                     static_cast<unsigned long long>(platform::MonotonicNanoseconds()));
        REQUIRE(count > 0 && static_cast<usize>(count) < sizeof(path));
        const usize pathBytes = static_cast<usize>(count);
        {
            platform::File file;
            REQUIRE(file.Open(platform::PhysicalFileSystem(), path, pathBytes,
                              platform::FileOpenMode::CreateNew));
            auto written = file.WriteAll(packed.Data(), packed.Size());
            REQUIRE(written.status && written.bytes == packed.Size());
            REQUIRE(file.Close());
        }
        auto removeFile = MakeScopeExit(
            [&]() noexcept { CHECK(platform::PhysicalFileSystem().Delete(path, pathBytes)); });

        platform::AsyncIoContext io;
        REQUIRE(io.Start(platform::DefaultAsyncIoBackend()));
        WorkerScheduler scheduler(allocator);
        REQUIRE(scheduler.Start({.workerCount = 2, .maxInFlight = 16}));
        {
            platform::AsyncReadFile reader;
            REQUIRE(reader.Open(io, path, pathBytes));
            for (int round = 0; round < 40; ++round) {
                Array<uint8> buffer(allocator);
                REQUIRE(buffer.TryResize(packed.Size()));
                Array<uint8> output(allocator);
                auto event = scheduler.TryCreateEvent();
                REQUIRE(event);
                CompressionTask task;
                // 해제를 먼저 제출해도 이벤트가 완료될 때까지 실행되지 않는다(source 길이는 제출 때
                // 고정, 내용은 IO가 채운다).
                REQUIRE(TrySubmitDecompress(
                    scheduler, task, CompressionFormat::Zstd,
                    Span<const uint8>(buffer.Data(), buffer.Size()), original.Size(), output,
                    Span<const WorkerTaskHandle>(&event.Value().Handle(), 1)));
                platform::AsyncIoRequest request;
                REQUIRE(TrySubmitReadEvent(reader, request, 0, buffer.Data(), buffer.Size(),
                                           Move(event.Value()), allocator));
                auto bytes = task.TryWaitResult();
                REQUIRE(bytes);
                CHECK(bytes.Value() == original.Size());
                CHECK(Same(output, View(original)));
                REQUIRE(request.Wait(30000) == platform::WaitResult::Signaled);
            }

            // 파일보다 많이 기대하면 짧은 읽기라 이벤트가 실패하고 해제는 실행되지 않는다.
            Array<uint8> larger(allocator);
            REQUIRE(larger.TryResize(packed.Size() + 100));
            Array<uint8> unused(allocator);
            auto shortEvent = scheduler.TryCreateEvent();
            REQUIRE(shortEvent);
            CompressionTask shortTask;
            REQUIRE(TrySubmitDecompress(
                scheduler, shortTask, CompressionFormat::Zstd,
                Span<const uint8>(larger.Data(), larger.Size()), original.Size(), unused,
                Span<const WorkerTaskHandle>(&shortEvent.Value().Handle(), 1)));
            platform::AsyncIoRequest shortRequest;
            REQUIRE(TrySubmitReadEvent(reader, shortRequest, 0, larger.Data(), larger.Size(),
                                       Move(shortEvent.Value()), allocator));
            CHECK(shortTask.TryWaitResult().Error() == ErrorCode::DependencyFailed);
            CHECK(unused.Size() == 0);
            REQUIRE(shortRequest.Wait(30000) == platform::WaitResult::Signaled);
            REQUIRE(reader.Close());
        }
        REQUIRE(scheduler.Shutdown());
        REQUIRE(io.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

} // namespace rocks::test
