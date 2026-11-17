#pragma once

#include <TraceAnalysis/TraceStreamDecoder.h>

#include <Core/Containers/Array.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/Atomics.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>
#include <Platform/File.h>
#include <Platform/Sync.h>
#include <Platform/Thread.h>
#include <TraceCapture/TraceSession.h>
#include <TraceCapture/TraceWrite.h>

#include <stdio.h>

// TR1 시험 공용 도구다. 시간·sleep에 기대지 않도록 순서는 이벤트(gate)로만 맞춘다. 대기의 10초
// 제한은 시험이 멈추지 않게 하는 안전장치이며 판정에 쓰지 않는다(넘으면 Fatal).
namespace rocks::tracetest {

inline constexpr uint32 GateTimeoutMs = 10000;

inline void WaitGate(platform::ManualResetEvent& gate) noexcept {
    if (gate.Wait(GateTimeoutMs) != platform::WaitResult::Signaled) {
        platform::Fatal("Trace test gate was not signaled");
    }
}

inline void JoinThread(platform::Thread& thread) noexcept {
    if (thread.WaitForCompletion(GateTimeoutMs) != platform::WaitResult::Signaled ||
        thread.Join() != platform::ThreadStatus::Success) {
        platform::Fatal("Trace test thread failed to terminate");
    }
}

// 출력 콜백이 받은 byte를 모은다. writer 스레드에서 불릴 수 있어 잠근다.
struct MemoryOutput {
    explicit MemoryOutput(Allocator& allocator) noexcept : bytes(allocator) {
    }

    static Result<void> Write(void* context, Span<const uint8> data) noexcept {
        auto& self = *static_cast<MemoryOutput*>(context);
        platform::LockGuard lock(self.mutex);
        return self.bytes.TryAppend(data);
    }

    platform::Mutex mutex;
    Array<uint8> bytes;
};

// 지정한 스레드가 지정한 크기를 처음 할당할 때 멈추는 할당기다. 기록 경로 한가운데(블록 풀의 새
// 페이지 확보)에 gate를 두어, 진행 중인 기록과 Stop의 순서를 결정적으로 만든다.
class GatedAllocator final : public Allocator {
public:
    void Arm(uint64 thread, usize bytes) noexcept {
        gatedBytes_ = bytes;
        platform::AtomicStore(&gatedThread_, thread);
        platform::AtomicStore(&armed_, 1u);
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        if (bytes == gatedBytes_ &&
            platform::AtomicLoad(&gatedThread_) == platform::CurrentThreadId() &&
            platform::AtomicExchange(&armed_, 0u) == 1u) {
            entered.Signal();
            WaitGate(release);
        }

        return inner_.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        return inner_.Deallocate(address);
    }

    platform::ManualResetEvent entered;
    platform::ManualResetEvent release;

private:
    SystemAllocator inner_;
    uint32 armed_ = 0;
    uint64 gatedThread_ = 0;
    usize gatedBytes_ = 0;
};

// 작업 디렉터리의 임시 trace 파일 경로다. 소멸 때 지운다.
struct TempTracePath {
    TempTracePath() noexcept {
        const int count = snprintf(path, sizeof(path), "rocks-trace-%llu-%llu.rtrace",
                                   static_cast<unsigned long long>(platform::MonotonicTicks()),
                                   static_cast<unsigned long long>(platform::CurrentThreadId()));
        if (count <= 0 || static_cast<usize>(count) >= sizeof(path)) {
            platform::Fatal("Trace test path does not fit");
        }

        length = static_cast<usize>(count);
    }

    ~TempTracePath() noexcept {
        auto removed = platform::PhysicalFileSystem().Delete(path, length);
        (void)removed;
    }

    TempTracePath(const TempTracePath&) = delete;
    TempTracePath& operator=(const TempTracePath&) = delete;

    bool ReadAll(Array<uint8>& out) const noexcept {
        platform::File file;
        if (!file.Open(path, platform::FileOpenMode::ReadExisting)) {
            return false;
        }

        uint64 size = 0;
        if (!file.Size(size) || !out.TryResize(static_cast<usize>(size))) {
            return false;
        }

        const platform::FileTransfer read = file.ReadExact(out.Data(), out.Size());
        return read.status && read.bytes == out.Size() && file.Close();
    }

    char path[160]{};
    usize length = 0;
};

// trace 스레드 id의 스트림 id다.
inline uint16 ThreadStream(uint32 traceThreadId) noexcept {
    return static_cast<uint16>(TraceFormat::FirstThreadStreamId + traceThreadId - 1);
}

// 정의 스트림의 ThreadInfo 기록에서 OS 스레드 id에 해당하는 trace 스레드 id를 찾는다. 없으면 0이다.
inline uint32 FindTraceThread(const TraceStreamDecoder& decoder, uint64 osThreadId) noexcept {
    SystemAllocator scratch;
    uint32 found = 0;
    {
        Array<DecodedRecord> records(scratch);
        if (decoder.DecodeRecords(TraceFormat::DefinitionStreamId, records)) {
            for (const DecodedRecord& record : records) {
                if (record.typeId == TraceFormat::ThreadInfoTypeId &&
                    ReadFixed<uint64>(record, TraceFormat::ThreadInfoOsIdOffset) == osThreadId) {
                    found = ReadFixed<uint32>(record, TraceFormat::ThreadInfoIdOffset);
                }
            }
        }
    }

    return found;
}

} // namespace rocks::tracetest
