#include <Core/Algorithms.h>
#include <Core/Diagnostics/Correlation.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Diagnostics/LogSystem.h>
#include <Core/Function.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/Clock.h>
#include <Task/WorkerScheduler.h>
#include <TraceCapture/TraceTask.h>

#include <stdio.h>

// 로그 생산자 비용 측정 도구다(Docs/Design/LoggingSystem.ko.md 8.6절 목표치: 비활성 호출은 분기
// 하나 수준, 활성 호출(내장 타입 인자) p99 1 µs 미만). 목표치는 보장이 아니라 측정해 보고하는
// 값이다.
//
// 재는 것(호출 스레드가 Log::*에 머문 시간, MonotonicTicks로 한 건씩):
// - compiled out: Release에서 컴파일 타임 하한 아래인 Log::Debug(int, double)
// - runtime disabled: 전역 최소 Warning에서 Log::Info(int, double)
// - async enabled: Async 모드에서 Log::Info(int, double). 출력 스레드는 빈 sink로 보낸다.
// - sync enabled: Sync 모드에서 같은 호출(렌더·sink까지 호출 스레드가 한다. 참고값)
// 큐가 차서 버리는 경우가 섞이지 않도록 묶음마다 Flush한다(Flush 시간은 재지 않는다).
// - 상관 id 전파(14.1.4절): 빈 사본 복원(TraceTaskScope·CorrelationScope 인라인 빈 경로), 사본 +
//   복원, 그리고 실제 WorkerScheduler Submit → 실행 → Wait 왕복을 스택이 빈 경우와 상관 id가 있는
//   경우로 나눠 잰다(왕복 차이가 task 하나의 전파 비용이다. trace 세션은 없다).
//
// 사용: LogBench [건수]. Release로 빌드해 같은 기기에서 비교한다.
//
// Release 문자열 제외 확인(LG5): Release 링크 결과 LogBench.exe에 Debug 템플릿 "compiled out
// {Index} {Ratio}"는 없고 Info 템플릿 "enabled {Index} {Ratio}"는 있다(.obj는 ThinLTO
// bitcode라 링크 결과를 본다). Windows clang-cl 21.1.6·lld-link 한 구성에서 확인한 사실이며
// 일반 보장이 아니다(최적화가 참조 없는 리터럴을 버리는 데 기댄다). self 시험
// Log.ReleaseBinaryExcludesCompiledOutTemplates가 빌드된 Release LogBench를 이 기준으로 검사한다.
namespace {

using namespace rocks;

constexpr LogCategory LogBench{"Bench"};
constexpr uint32 DefaultSamples = 200000;
// 건수는 BatchSize의 배수로 받는다(묶음 평균).
constexpr uint32 BatchSize = 1000;
constexpr uint32 MaxSamples = 2000000;
// task 왕복 측정 건수는 기록 측정 건수를 이 값으로 나눈 것이다.
constexpr uint32 TaskSampleDivisor = 10;

uint64 samples[MaxSamples];

class NullSink final : public RuntimeLogSink {
public:
    Result<void> Write(const LogEvent&) noexcept override {
        return Result<void>::Success();
    }
};

double ToNanoseconds(uint64 ticks) {
    return static_cast<double>(ticks) * 1e9 /
           static_cast<double>(platform::MonotonicTickFrequency());
}

// 한 건씩 잰 값(시계 해상도로 양자화된다)의 분위수와, 묶음 전체 시간을 건수로 나눈 평균이다.
void Report(const char* name, uint32 count, uint64 batchTicks) {
    algo::Sort(Span<uint64>(samples, count));
    const uint64 p50 = samples[count / 2];
    const uint64 p99 = samples[static_cast<usize>(count) * 99 / 100];
    const uint64 maximum = samples[count - 1];
    printf("%-16s mean %7.1f ns  p50 %7.1f ns  p99 %7.1f ns  max %10.1f ns  (%u calls)\n", name,
           ToNanoseconds(batchTicks) / count, ToNanoseconds(p50), ToNanoseconds(p99),
           ToNanoseconds(maximum), count);
}

// 한 건씩 재고(samples), 같은 호출을 묶음 단위로 한 번 더 재어 묶음 시간의 합을 돌려준다.
template <typename Call>
uint64 Measure(LogSystem& logs, uint32 count, Call&& call) {
    for (uint32 index = 0; index < count; ++index) {
        const uint64 start = platform::MonotonicTicks();
        call(index);
        samples[index] = platform::MonotonicTicks() - start;
        if ((index + 1) % BatchSize == 0) {
            (void)logs.Flush();
        }
    }

    uint64 total = 0;
    for (uint32 batch = 0; batch < count / BatchSize; ++batch) {
        const uint64 start = platform::MonotonicTicks();
        for (uint32 index = 0; index < BatchSize; ++index) {
            call(batch * BatchSize + index);
        }

        total += platform::MonotonicTicks() - start;
        (void)logs.Flush();
    }

    return total * count / (count / BatchSize * BatchSize);
}

void Run(LogMode mode, const char* enabledName, uint32 count) {
    SystemAllocator allocator;
    LogSystemConfig config;
    config.mode = mode;
    config.globalMinimum = LogLevel::Info;
    LogSystem logs(allocator, config);
    NullSink sink;
    if (!logs.AddSink(sink) || !logs.Start()) {
        printf("Log system did not start\n");
        return;
    }

    LogInstallScope installed(logs);
    if (mode == LogMode::Async) {
        // Release에서 Debug는 컴파일 타임 하한 아래라 호출 자체가 사라진다(비교 기준).
        const uint64 compiledOut = Measure(logs, count, [](uint32 index) noexcept {
            Log::Debug(LogBench, "compiled out {Index} {Ratio}", index, 0.5);
        });
        Report("compiled out", count, compiledOut);

        // 런타임 비활성: 전역 최소를 Warning으로 올리고 Info를 부른다(빠른 거부 하한 검사).
        (void)logs.SetGlobalMinimum(LogLevel::Warning);
        const uint64 disabled = Measure(logs, count, [](uint32 index) noexcept {
            Log::Info(LogBench, "disabled {Index} {Ratio}", index, 0.5);
        });
        Report("runtime disabled", count, disabled);
        (void)logs.SetGlobalMinimum(LogLevel::Info);
    }

    const uint64 enabled = Measure(logs, count, [](uint32 index) noexcept {
        Log::Info(LogBench, "enabled {Index} {Ratio}", index, 0.5);
    });
    Report(enabledName, count, enabled);
    {
        // 상관 id가 있을 때의 활성 기록(머리 뒤 40 B 복사)과, task·lane 전파 한 번(사본 + 복원)의
        // 비용이다(LoggingSystem.ko.md 14.1.4 측정 항목). 스택이 비면 전파는 분기 하나다.
        const uint64 emptyPropagation = Measure(logs, count, [](uint32) noexcept {
            if (CorrelationDepth() != 0) {
                const CorrelationToken token = CaptureCorrelation();
                CorrelationScope restored(token);
            }
        });
        Report("propagate empty", count, emptyPropagation);
        // scheduler가 task마다 거치는 실행 구간(기록하지 않는 task, 빈 사본)이다.
        const uint64 emptyTaskScope = Measure(logs, count, [](uint32) noexcept {
            const TraceTaskInfo info = TraceCreateTask();
            TraceTaskScope traced(info);
        });
        Report("task scope empty", count, emptyTaskScope);
        CorrelationScope request(CorrelationIds{0, 0, 0, 0, 42});
        const uint64 correlated = Measure(logs, count, [](uint32 index) noexcept {
            Log::Info(LogBench, "correlated {Index} {Ratio}", index, 0.5);
        });
        Report("with correlation", count, correlated);
        const uint64 propagation = Measure(logs, count, [](uint32) noexcept {
            if (CorrelationDepth() != 0) {
                const CorrelationToken token = CaptureCorrelation();
                CorrelationScope restored(token);
            }
        });
        Report("propagate", count, propagation);
    }

    const LogSystemStats stats = logs.Stats();
    printf("%-16s written %llu, dropped Info %llu\n", "",
           static_cast<unsigned long long>(stats.written),
           static_cast<unsigned long long>(stats.dropped[static_cast<usize>(LogLevel::Info)]));
}

// 실제 WorkerScheduler 경로(Submit → worker 실행 → Wait 왕복)를 한 건씩 잰다. worker 하나라 task가
// 차례로 돈다. 스택이 빈 경우와 상관 id가 있는 경우의 차이가 task 하나의 전파 비용이다(사본 +
// 복원·내림). 왕복은 깨우기를 포함하므로 차이보다 훨씬 크고 흔들린다(참고값).
void RunTaskRoundTrip(const char* name, uint32 count) {
    SystemAllocator allocator;
    WorkerScheduler scheduler(allocator);
    WorkerSchedulerConfig config;
    config.workerCount = 1;
    if (!scheduler.Start(config)) {
        printf("Worker scheduler did not start\n");
        return;
    }

    uint64 total = 0;
    uint32 measured = 0;
    for (uint32 index = 0; index < count; ++index) {
        const uint64 start = platform::MonotonicTicks();
        auto body = Function<void(WorkerTaskContext&)>::TryCreate(
            allocator, [](WorkerTaskContext&) noexcept {});
        if (!body) {
            break;
        }

        auto handle = scheduler.Submit(Move(body.Value()));
        if (!handle || !handle.Value().Wait()) {
            break;
        }

        samples[measured] = platform::MonotonicTicks() - start;
        total += samples[measured];
        ++measured;
    }

    (void)scheduler.Shutdown();
    if (measured != count) {
        printf("%-16s stopped after %u of %u tasks\n", name, measured, count);
        return;
    }

    Report(name, count, total);
}

void RunTaskPropagation(uint32 count) {
    RunTaskRoundTrip("task empty", count);
    CorrelationScope request(CorrelationIds{0, 0, 0, 0, 42});
    RunTaskRoundTrip("task correlated", count);
}

} // namespace

int main(int argc, char** argv) {
    uint32 count = DefaultSamples;
    if (argc == 2) {
        uint64 value = 0;
        bool digits = argv[1][0] != '\0';
        for (const char* digit = argv[1]; *digit != '\0' && digits; ++digit) {
            digits = *digit >= '0' && *digit <= '9' && value <= MaxSamples;
            value = value * 10 + static_cast<uint64>(*digit - '0');
        }

        // 묶음 평균이 건수를 BatchSize로 나누므로 BatchSize보다 작으면 0으로 나누게 된다.
        if (!digits || value < BatchSize || value > MaxSamples) {
            printf("usage: LogBench [samples (%u-%u)]\n", BatchSize, MaxSamples);
            return 2;
        }

        count = static_cast<uint32>(value);
    }

    Run(LogMode::Async, "async enabled", count);
    Run(LogMode::Sync, "sync enabled", count);
    // 왕복은 기록 한 건보다 훨씬 느려 건수를 줄인다.
    RunTaskPropagation(count / TaskSampleDivisor);
    return 0;
}
