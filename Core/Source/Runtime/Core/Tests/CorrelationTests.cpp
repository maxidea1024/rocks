#include <TestSupport/Test.h>
#include <Core/Diagnostics/Correlation.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Diagnostics/LogCapture.h>
#include <Core/Diagnostics/LogLayout.h>
#include <Core/Diagnostics/LogSystem.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/Fatal.h>
#include <Platform/Thread.h>

using namespace rocks;
namespace correlation_tests_file {

namespace {

constexpr LogCategory LogCorrelationTests{"Tests.Correlation"};

constexpr uint32 ThreadTimeoutMilliseconds = 30000;

struct ThreadProbe {
    bool sawCorrelation = true;
    usize depth = MaxCorrelationDepth;
};

void ProbeOtherThread(void* raw) noexcept {
    auto& probe = *static_cast<ThreadProbe*>(raw);
    CorrelationIds ids;
    probe.sawCorrelation = CurrentCorrelation(ids);
    probe.depth = CorrelationDepth();
}

// 관찰자 호출을 순서대로 적는다.
struct ObserverLog {
    char events[64]{};
    usize count = 0;
};

ObserverLog observerLog;

void OnTestPush(CorrelationEntry& entry, bool restored) noexcept {
    observerLog.events[observerLog.count++] = restored ? 'R' : 'P';
    // 확장 칸은 상위 계층이 쓴다. 사본·복원 때 그대로 옮겨지는지 본다.
    entry.ext[0] = entry.ext[0] == 0 ? entry.ids.request : entry.ext[0];
}

void OnTestPop(const CorrelationEntry& entry) noexcept {
    observerLog.events[observerLog.count++] = entry.ext[0] != 0 ? 'o' : 'x';
}

constexpr CorrelationObserver TestObserver{&OnTestPush, &OnTestPop};

// 다른 관찰자(trace)가 걸려 있으면 잠시 바꿨다가 되돌린다.
struct ObserverSwap {
    ObserverSwap() noexcept : previous(detail::CurrentCorrelationObserver()) {
        REQUIRE(SetCorrelationObserver(nullptr));
        REQUIRE(SetCorrelationObserver(&TestObserver));
    }

    ~ObserverSwap() noexcept {
        REQUIRE(SetCorrelationObserver(nullptr));
        if (previous != nullptr) {
            REQUIRE(SetCorrelationObserver(previous));
        }
    }

    const CorrelationObserver* previous;
};

struct Rendered {
    char message[256]{};
    char propertyText[64]{};
    LogProperty properties[MaxLogArguments];
    LogEvent event;

    Result<void> From(Span<const uint8> capture) noexcept {
        LogRenderTarget target{
            Span<char>(message), Span<char>(propertyText), Span<LogProperty>(properties), {}};
        return RenderLogCapture(capture, nullptr, target, event);
    }
};

void AppendText(void* raw, StringView text) noexcept {
    (void)static_cast<FixedStringBuilder*>(raw)->TryAppend(text);
}

} // namespace

// 스택은 중첩되고, 깊이 상한을 넘은 쌓기는 세기만 하며, 다른 스레드에는 보이지 않는다.
TEST(CorrelationScopeNestsOverflowsAndStaysOnItsThread) {
    const usize base = CorrelationDepth();
    CorrelationIds ids;
    {
        CorrelationScope outer(CorrelationIds{1, 0, 0, 0, 0});
        {
            CorrelationScope inner(CorrelationIds{0, 2, 0, 0, 0});
            REQUIRE(CurrentCorrelation(ids));
            CHECK(ids.task == 2 && ids.session == 0);
            CHECK(CorrelationDepth() == base + 2);

            ThreadProbe probe;
            platform::Thread thread;
            REQUIRE(thread.Start(&ProbeOtherThread, &probe) == platform::ThreadStatus::Success);
            if (thread.WaitForCompletion(ThreadTimeoutMilliseconds) !=
                    platform::WaitResult::Signaled ||
                thread.Join() != platform::ThreadStatus::Success) {
                platform::Fatal("correlation probe thread did not finish");
            }

            CHECK(!probe.sawCorrelation && probe.depth == 0);
        }

        REQUIRE(CurrentCorrelation(ids));
        CHECK(ids.session == 1 && ids.task == 0);
    }

    CHECK(CorrelationDepth() == base);

    // 상한까지 쌓고 하나 더: 넘친 쌓기는 스택에 들어가지 않는다(내림도 무동작).
    alignas(
        CorrelationScope) unsigned char storage[MaxCorrelationDepth + 1][sizeof(CorrelationScope)];
    CorrelationScope* scopes[MaxCorrelationDepth + 1]{};
    const usize room = MaxCorrelationDepth - base;
    for (usize index = 0; index <= room; ++index) {
        scopes[index] = ConstructAt<CorrelationScope>(
            storage[index], CorrelationIds{0, 0, 0, 0, static_cast<uint64>(index + 1)});
    }

    CHECK(CorrelationDepth() == MaxCorrelationDepth);
    REQUIRE(CurrentCorrelation(ids));
    CHECK(ids.request == room);
    for (usize index = room + 1; index-- > 0;) {
        DestroyAt(scopes[index]);
    }

    CHECK(CorrelationDepth() == base);
}

// 사본은 값과 확장 칸을 옮기고, 관찰자는 쌓기·복원·내리기마다 불린다.
TEST(CorrelationTokenCarriesExtensionsAndObserverSeesEveryChange) {
    observerLog = {};
    ObserverSwap swap;
    CorrelationToken token;
    {
        CorrelationScope request(CorrelationIds{0, 0, 0, 0, 9});
        token = CaptureCorrelation();
    }

    REQUIRE(token.valid);
    CHECK(token.entry.ids.request == 9 && token.entry.ext[0] == 9);
    {
        CorrelationScope restored(token);
        CorrelationIds ids;
        REQUIRE(CurrentCorrelation(ids));
        CHECK(ids.request == 9);
    }

    {
        CorrelationScope empty(CorrelationToken{});
        CHECK(CorrelationDepth() == 0);
    }

    CHECK(StringView(observerLog.events, observerLog.count) == "PoRo");
}

// 로그 기록은 캡처 때 맨 위 상관 id를 복사한다. 범위가 끝난 뒤 렌더해도 같은 값이다. 비어 있으면
// 0 byte이고 레이아웃 토큰은 비며, JSON 키도 쓰지 않는다.
TEST(LogCaptureCopiesTheCurrentCorrelation) {
    uint8 slot[LogSlotBytes]{};
    usize bytes = 0;
    usize emptyBytes = 0;
    {
        const auto empty =
            CaptureLog({Span<uint8>(slot)}, LogLevel::Info, LogCorrelationTests, "plain");
        REQUIRE(empty.IsOk());
        emptyBytes = empty.Value();
        CorrelationScope request(CorrelationIds{3, 1842, 0, 0, 0});
        const auto captured = CaptureLog({Span<uint8>(slot)}, LogLevel::Info, LogCorrelationTests,
                                         "with correlation");
        REQUIRE(captured.IsOk());
        bytes = captured.Value();
    }

    CHECK(bytes == emptyBytes + sizeof(CorrelationIds));
    Rendered rendered;
    REQUIRE(rendered.From(Span<const uint8>(slot, bytes)).IsOk());
    CHECK(rendered.event.hasCorrelation);
    CHECK(rendered.event.correlation.session == 3 && rendered.event.correlation.task == 1842);

    auto layout = LogLayout::Parse("{Message}{Correlation}{NewLine}");
    REQUIRE(layout);
    char storage[128];
    FixedStringBuilder line{Span<char>(storage)};
    layout.Value().Render(rendered.event, {&line, &AppendText});
    CHECK(line.View() == "with correlation session=3 task=1842\n");

    const auto plain =
        CaptureLog({Span<uint8>(slot)}, LogLevel::Info, LogCorrelationTests, "plain");
    REQUIRE(plain.IsOk());
    Rendered without;
    REQUIRE(without.From(Span<const uint8>(slot, plain.Value())).IsOk());
    CHECK(!without.event.hasCorrelation);
    FixedStringBuilder second{Span<char>(storage)};
    layout.Value().Render(without.event, {&second, &AppendText});
    CHECK(second.View() == "plain\n");
}

namespace {

// 모든 할당을 센다.
class CountingCorrelationAllocator final : public Allocator {
public:
    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        ++allocations;
        return inner.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        return inner.Deallocate(address);
    }

    SystemAllocator inner;
    uint64 allocations = 0;
};

class CountingSink final : public RuntimeLogSink {
public:
    Result<void> Write(const LogEvent& event) noexcept override {
        correlated += event.hasCorrelation && event.correlation.request == 5 ? 1u : 0u;
        return Result<void>::Success();
    }

    Result<void> Flush() noexcept override {
        return Result<void>::Success();
    }

    uint32 correlated = 0;
};

} // namespace

// 상관 id 쌓기·사본·복원과 상관 id가 실린 기록의 캡처·렌더는 Start 뒤 할당하지 않는다.
TEST(CorrelationCaptureDoesNotAllocate) {
    CountingCorrelationAllocator allocator;
    LogSystemConfig config;
    config.mode = LogMode::Manual;
    config.globalMinimum = LogLevel::Trace;
    CountingSink sink;
    LogSystem logs(allocator, config);
    REQUIRE(logs.AddSink(sink));
    REQUIRE(logs.Start());
    const uint64 before = allocator.allocations;
    {
        LogInstallScope installed(logs);
        CorrelationScope request(CorrelationIds{0, 0, 0, 0, 5});
        const CorrelationToken token = CaptureCorrelation();
        {
            CorrelationScope restored(token);
            Log::Info(LogCorrelationTests, "correlated {Value}", 1);
        }

        // 앞 시험이 설치 없이 남긴 기록 수 알림이 하나 더 있을 수 있다.
        CHECK(logs.Pump() != 0);
    }

    CHECK(allocator.allocations == before);
    CHECK(sink.correlated == 1);
    logs.Stop();
}
} // namespace correlation_tests_file
