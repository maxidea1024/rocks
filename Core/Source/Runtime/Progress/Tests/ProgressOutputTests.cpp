#include <Core/Containers/Array.h>
#include <Core/Guid.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Move.h>
#include <Core/Text/Json.h>
#include <Core/Text/String.h>
#include <Core/Text/StringBuilder.h>
#include <Localization/Text.h>
#include <Progress/ConsoleProgressReporter.h>
#include <Progress/ParentControlListener.h>
#include <Progress/ProgressCounter.h>
#include <Progress/ProgressHub.h>
#include <Progress/ProgressJsonlWriter.h>
#include <Progress/ProgressPumpThread.h>
#include <TestSupport/Test.h>
#include <TestSupport/TestGate.h>
#include <stdio.h>

using namespace rocks;
namespace progress_output_tests_file {

// 받는 쪽 시험(Docs/Design/ProgressReporting 14장 15·16·17, 10.2절 제어 줄). 시계는 주입하고 시험이
// 직접 넘긴다. 줄은 모아서 문자열 그대로 단언한다.
struct FakeClock {
    uint64 now = 0;
};

uint64 ReadFakeClock(void* context) noexcept {
    return static_cast<FakeClock*>(context)->now;
}

constexpr uint64 Second = 1'000'000'000;

struct Line {
    explicit Line(Allocator& allocator) noexcept : text(allocator) {
    }

    Line(Line&&) noexcept = default;
    Line& operator=(Line&&) noexcept = default;

    String text;
    bool warning = false;
};

struct Lines {
    explicit Lines(Allocator& allocator) noexcept : allocator(allocator), items(allocator) {
    }

    Allocator& allocator;
    Array<Line> items;

    static void Console(void* context, bool warning, StringView line) noexcept {
        auto& self = *static_cast<Lines*>(context);
        if (auto added = self.items.TryEmplaceBack(self.allocator)) {
            (void)added.Value()->text.TryAssign(line);
            added.Value()->warning = warning;
        }
    }

    static void Json(void* context, StringView line) noexcept {
        Console(context, false, line);
    }

    bool Has(StringView text, bool warning) const noexcept {
        for (const Line& line : items) {
            if (line.text.View() == text && line.warning == warning) {
                return true;
            }
        }

        return false;
    }

    // 실패를 읽기 쉽게 모은 줄을 찍는다(항상 false — CHECK(... || lines.Dump())).
    bool Dump() const noexcept {
        for (const Line& line : items) {
            printf("  line%s: %.*s\n", line.warning ? "(warning)" : "",
                   static_cast<int>(line.text.View().ByteCount()), line.text.View().Data());
        }

        return false;
    }

    bool Has(StringView text) const noexcept {
        for (const Line& line : items) {
            if (line.text.View() == text) {
                return true;
            }
        }

        return false;
    }
};

struct Hub {
    SystemAllocator allocator;
    FakeClock clock;
    bool ready = false;

    explicit Hub(uint32 eventCapacity = DefaultProgressEventLogCapacity) noexcept {
        ProgressHubSettings settings;
        settings.clock = ReadFakeClock;
        settings.clockContext = &clock;
        settings.eventLogCapacity = eventCapacity;
        ready = static_cast<bool>(ProgressHub::Get().Initialize(allocator, settings));
    }

    ~Hub() noexcept {
        ProgressHub::Get().Shutdown();
    }

    Text MakeText(StringView source) noexcept {
        auto text = Text::Invariant(allocator, source);
        return text ? Move(text.Value()) : Text();
    }

    ProgressTaskHandle Begin(StringView title,
                             ProgressPresentation presentation = ProgressPresentation::Background,
                             ProgressCancel cancel = ProgressCancel::NotCancellable,
                             ProgressTaskId parent = NoProgressTask) noexcept {
        const Text text = MakeText(title);
        ProgressTaskDesc desc;
        desc.title = &text;
        desc.presentation = presentation;
        desc.cancel = cancel;
        desc.parent = parent;
        auto task = ProgressHub::Get().Begin(desc);
        return task ? Move(task.Value()) : ProgressTaskHandle();
    }
};

// 시험 15: 시작·끝 줄은 Pump 때 바로(Hidden 포함), 진행 줄은 2 s마다(직전·정확히), 최상위·Hidden이
// 아닌 작업만. 실패·취소·Abandoned 끝은 Warning이다.
TEST(Progress_ConsoleReporterLinesAndInterval) {
    Hub hub;
    REQUIRE(hub.ready);
    Lines lines(hub.allocator);
    ConsoleProgressReporterDesc desc;
    desc.write = &Lines::Console;
    desc.context = &lines;
    ConsoleProgressReporter console(hub.allocator, desc);
    REQUIRE(console.Pump(0));
    CHECK(lines.items.IsEmpty());

    ProgressTaskHandle cook = hub.Begin("Cooking packages");
    ProgressTaskHandle hidden = hub.Begin("Hidden work", ProgressPresentation::Hidden);
    ProgressTaskHandle child = hub.Begin("Child", ProgressPresentation::Background,
                                         ProgressCancel::NotCancellable, cook.Id());
    auto counter = ProgressCounter::ForTask(cook, 5000);
    REQUIRE(counter);
    counter.Value().Add(1234);
    CHECK(cook.SetMessage(hub.MakeText("Maps/Arena")));
    hub.clock.now = 2 * Second - 1;
    REQUIRE(console.Pump(hub.clock.now));
    CHECK(lines.Has("Cooking packages: started", false));
    CHECK(lines.Has("Hidden work: started", false));
    CHECK(lines.items.Size() == 3);

    hub.clock.now = 2 * Second;
    REQUIRE(console.Pump(hub.clock.now));
    CHECK(lines.Has("Cooking packages: 1,234 / 5,000 (24%) - Maps/Arena", false) || lines.Dump());
    CHECK(lines.items.Size() == 4);

    CHECK(child.Finish(ProgressOutcome::Cancelled));
    CHECK(hidden.Finish(ProgressOutcome::Failed, hub.MakeText("Disk full")));
    CHECK(cook.Finish(ProgressOutcome::Succeeded));
    ProgressTaskHandle dropped = hub.Begin("Dropped");
    REQUIRE(console.Pump(hub.clock.now));
    CHECK(lines.Has("Child: cancelled", true));
    CHECK(lines.Has("Hidden work: failed (Disk full)", true));
    CHECK(lines.Has("Cooking packages: done", false));
    CHECK(dropped.Finish(ProgressOutcome::Abandoned));
    REQUIRE(console.Pump(hub.clock.now));
    CHECK(lines.Has("Dropped: abandoned", true));
}

// 시험 15: 비율·불확정 모양의 진행 줄.
TEST(Progress_ConsoleReporterFractionAndIndeterminate) {
    Hub hub;
    REQUIRE(hub.ready);
    Lines lines(hub.allocator);
    ConsoleProgressReporterDesc desc;
    desc.write = &Lines::Console;
    desc.context = &lines;
    ConsoleProgressReporter console(hub.allocator, desc);
    REQUIRE(console.Pump(0));
    ProgressTaskHandle loading = hub.Begin("Loading");
    ProgressTaskHandle mesh = hub.Begin("Building mesh");
    REQUIRE(mesh.SetExternalProgress(ProgressShape::Fraction, 0.415, 0, 0));
    hub.clock.now = 2 * Second;
    REQUIRE(console.Pump(hub.clock.now));
    CHECK(lines.Has("Loading: in progress", false));
    CHECK(lines.Has("Building mesh: 41%", false));
    CHECK(loading.Finish(ProgressOutcome::Succeeded));
    CHECK(mesh.Finish(ProgressOutcome::Succeeded));
}

bool terminationFlag = false;

bool ReadTermination(void* context) noexcept {
    (void)context;
    return terminationFlag;
}

uint32 pumpCalls = 0;

void CountPump(void* context, uint64 now) noexcept {
    (void)context;
    (void)now;
    ++pumpCalls;
}

// 시험 15: 펌프 주인은 매 Tick에 펌프를 부르고, 종료 요청을 처음 보았을 때만 취소 가능한 최상위
// 작업을 취소한다. 스레드는 Stop에서 마지막 Tick 뒤 회수된다.
TEST(Progress_PumpThreadCancelsOnTerminationOnce) {
    Hub hub;
    REQUIRE(hub.ready);
    ProgressTaskHandle task =
        hub.Begin("Cook", ProgressPresentation::Background, ProgressCancel::UserCancellable);
    terminationFlag = false;
    pumpCalls = 0;
    ProgressPumpThreadDesc desc;
    desc.pump = &CountPump;
    desc.terminationRequested = &ReadTermination;
    ProgressPumpThread pump;
    pump.Configure(desc);
    pump.Tick(0);
    CHECK(pumpCalls == 1);
    CHECK(!task.Token().IsCancellationRequested());
    terminationFlag = true;
    pump.Tick(1);
    pump.Tick(2);
    CHECK(pump.CancelRequests() == 1);
    CHECK(task.Token().IsCancellationRequested());
    CHECK(pumpCalls == 3);

    ProgressPumpThread threaded;
    CHECK(threaded.Start(hub.allocator, ProgressPumpThreadDesc{}).Error() ==
          ErrorCode::InvalidArgument);
    REQUIRE(threaded.Start(hub.allocator, desc));
    CHECK(threaded.Start(hub.allocator, desc).Error() == ErrorCode::AlreadyExists);
    const uint32 before = pumpCalls;
    threaded.Stop();
    CHECK(pumpCalls > before);
    CHECK(task.Finish(ProgressOutcome::Cancelled));
}

// 줄이 JSON으로 읽히고 그 문자열 칸 값이다.
bool JsonField(Allocator& allocator, StringView line, StringView key, String& out) noexcept {
    JsonDocument document(allocator);
    if (!document.Parse(line).Succeeded()) {
        return false;
    }

    const JsonValue value = document.Root().Find(key);
    return value.IsValid() && value.IsString() && out.TryAssign(value.GetString());
}

// 시험 16·17: 각 사건·모양의 golden, 변화 없는 작업은 건너뜀, 문구 원문·지역화 id(Localized만),
// Format 문구는 원문만, 긴 문구는 UTF-8 경계에서 자름, lost 줄.
TEST(Progress_JsonlWriterGoldenLines) {
    Hub hub(4);
    REQUIRE(hub.ready);
    Lines lines(hub.allocator);
    ProgressJsonlWriterDesc desc;
    desc.write = &Lines::Json;
    desc.context = &lines;
    ProgressJsonlWriter jsonl(hub.allocator, desc);
    ProgressTaskHandle cook = hub.Begin("Cooking packages", ProgressPresentation::Background,
                                        ProgressCancel::UserCancellable);
    auto counter = ProgressCounter::ForTask(cook, 4);
    REQUIRE(counter);
    counter.Value().Add(1);
    REQUIRE(jsonl.Flush(0));
    StringBuilder<256> expected(hub.allocator);
    REQUIRE(expected.TryAppendFormat(
        "{{\"v\":1,\"e\":\"begin\",\"id\":{},\"parent\":0,\"title\":\"Cooking packages\","
        "\"titleId\":\"\",\"titleRev\":0,\"pres\":\"background\",\"cancel\":true,"
        "\"weight\":1.0,\"selfWeight\":1.0}}",
        cook.Id()));
    CHECK(lines.Has(expected.View()));
    expected.Clear();
    REQUIRE(expected.TryAppendFormat(
        "{{\"v\":1,\"e\":\"progress\",\"id\":{},\"shape\":\"count\",\"frac\":0.25,\"done\":1,"
        "\"total\":4,\"msg\":\"\",\"msgId\":\"\",\"msgRev\":0,\"self\":0.25,"
        "\"cancelRequested\":false}}",
        cook.Id()));
    CHECK(lines.Has(expected.View()));
    const usize afterFirst = lines.items.Size();
    REQUIRE(jsonl.Flush(Second));
    CHECK(lines.items.Size() == afterFirst);

    // Localized 문구는 지역화 id·판을, Format 문구는 원문만 보낸다.
    constexpr TextId MessageId{0x0123456789abcdef, 0x0fedcba987654321};
    auto localized = Text::Localized(hub.allocator, MessageId, 3, "Maps/Arena");
    REQUIRE(localized);
    CHECK(cook.SetMessage(localized.Value()));
    REQUIRE(jsonl.Flush(2 * Second));
    StringBuilder<64> guid(hub.allocator);
    REQUIRE(guid.TryAppendFormat("{}", MessageId));
    expected.Clear();
    REQUIRE(expected.TryAppendFormat(
        "{{\"v\":1,\"e\":\"progress\",\"id\":{},\"shape\":\"count\",\"frac\":0.25,\"done\":1,"
        "\"total\":4,\"msg\":\"Maps/Arena\",\"msgId\":\"{}\",\"msgRev\":3,\"self\":0.25,"
        "\"cancelRequested\":false}}",
        cook.Id(), guid.View()));
    CHECK(lines.Has(expected.View()));

    const Text pattern = hub.MakeText("Hello {Name}");
    const Text name = hub.MakeText("World");
    const TextArgument arguments[] = {{"Name", &name}};
    auto formatted = Text::Format(hub.allocator, pattern, Span<const TextArgument>(arguments));
    REQUIRE(formatted);
    ProgressTaskDesc formattedDesc;
    formattedDesc.title = &formatted.Value();
    formattedDesc.presentation = ProgressPresentation::Hidden;
    auto hello = ProgressHub::Get().Begin(formattedDesc);
    REQUIRE(hello);
    REQUIRE(jsonl.Pump(2 * Second));
    expected.Clear();
    REQUIRE(expected.TryAppendFormat(
        "{{\"v\":1,\"e\":\"begin\",\"id\":{},\"parent\":0,\"title\":\"Hello World\","
        "\"titleId\":\"\",\"titleRev\":0,\"pres\":\"hidden\",\"cancel\":false,"
        "\"weight\":1.0,\"selfWeight\":1.0}}",
        hello.Value().Id()));
    CHECK(lines.Has(expected.View()));

    // 긴 문구(가 3 byte × 4000)는 MaxProgressJsonlTextBytes까지 code point 경계에서 자른다.
    String longText(hub.allocator);
    for (uint32 index = 0; index < 4000; ++index) {
        REQUIRE(longText.TryAppend("\xEA\xB0\x80"));
    }

    CHECK(hello.Value().SetMessage(hub.MakeText(longText.View())));
    REQUIRE(jsonl.Flush(3 * Second));
    String message(hub.allocator);
    REQUIRE(JsonField(hub.allocator, lines.items.Last().text.View(), "msg", message));
    CHECK(message.View().ByteCount() <= MaxProgressJsonlTextBytes);
    CHECK(message.View().ByteCount() % 3 == 0);
    CHECK(lines.items.Last().text.View().ByteCount() <= MaxProgressJsonlLineBytes);

    CHECK(cook.Finish(ProgressOutcome::Failed, hub.MakeText("Disk full")));
    REQUIRE(jsonl.Pump(3 * Second));
    expected.Clear();
    REQUIRE(expected.TryAppendFormat(
        "{{\"v\":1,\"e\":\"end\",\"id\":{},\"outcome\":\"failed\",\"detail\":\"Disk full\","
        "\"detailId\":\"\",\"detailRev\":0}}",
        cook.Id()));
    CHECK(lines.Has(expected.View()));

    // 고리(용량 4)가 넘치면 lost 줄이다.
    for (uint32 index = 0; index < 4; ++index) {
        ProgressTaskHandle burst = hub.Begin("Burst");
        CHECK(burst.Finish(ProgressOutcome::Succeeded));
    }

    REQUIRE(jsonl.Pump(3 * Second));
    CHECK(lines.Has("{\"v\":1,\"e\":\"lost\",\"count\":4}"));
    CHECK(hello.Value().Finish(ProgressOutcome::Succeeded));
}

// 시험 16: writer보다 먼저 시작한 작업도 begin 줄이 progress 줄보다 먼저 나온다.
TEST(Progress_JsonlWriterBeginsTasksStartedEarlier) {
    Hub hub;
    REQUIRE(hub.ready);
    ProgressTaskHandle early = hub.Begin("Early");
    REQUIRE(early.SetExternalProgress(ProgressShape::Fraction, 0.5, 0, 0));
    Lines lines(hub.allocator);
    ProgressJsonlWriterDesc desc;
    desc.write = &Lines::Json;
    desc.context = &lines;
    ProgressJsonlWriter jsonl(hub.allocator, desc);
    REQUIRE(jsonl.Pump(0));
    REQUIRE(lines.items.Size() == 2);
    String event(hub.allocator);
    REQUIRE(JsonField(hub.allocator, lines.items[0].text.View(), "e", event));
    CHECK(event.View() == StringView("begin"));
    REQUIRE(JsonField(hub.allocator, lines.items[1].text.View(), "e", event));
    CHECK(event.View() == StringView("progress"));
    // 간격 전 Pump는 progress 줄을 쓰지 않는다.
    REQUIRE(early.SetExternalProgress(ProgressShape::Fraction, 0.75, 0, 0));
    REQUIRE(jsonl.Pump(DefaultJsonlProgressIntervalNanoseconds - 1));
    CHECK(lines.items.Size() == 2);
    REQUIRE(jsonl.Pump(DefaultJsonlProgressIntervalNanoseconds));
    CHECK(lines.items.Size() == 3);
    CHECK(early.Finish(ProgressOutcome::Succeeded));
}

// 부모 제어 줄을 차례로 주는 가짜 입력이다(다 주면 EOF 또는 NoProgress를 되풀이).
struct ScriptedInput {
    const char* text = "";
    usize offset = 0;
    bool endWithEof = true;
    // 입력 끝(EOF)을 돌려준 뒤 연다 — 시험은 이것을 기다린 뒤 Stop한다(스레드가 Stop 전에 다 읽게).
    test::TestGate drained;
};

platform::FileTransfer ReadScripted(void* context, void* buffer, usize capacity,
                                    uint32 timeoutMilliseconds) noexcept {
    (void)timeoutMilliseconds;
    auto& input = *static_cast<ScriptedInput*>(context);
    platform::FileTransfer result;
    const usize total = StringView(input.text).ByteCount();
    if (input.offset == total) {
        result.status.code =
            input.endWithEof ? platform::FileCode::EndOfFile : platform::FileCode::NoProgress;
        input.drained.Open();
        return result;
    }

    // 한 번에 3 byte씩 준다(줄이 여러 읽기에 걸쳐도 맞게 나뉘는지).
    constexpr usize Piece = 3;
    usize bytes = total - input.offset;
    bytes = bytes < Piece ? bytes : Piece;
    bytes = bytes < capacity ? bytes : capacity;
    for (usize index = 0; index < bytes; ++index) {
        static_cast<char*>(buffer)[index] = input.text[input.offset + index];
    }

    input.offset += bytes;
    result.bytes = bytes;
    return result;
}

// 10.2절·시험 20: "cancel" 줄은 최상위 작업을 취소하고, 모르는 줄은 Warning 한 번 뒤 무시하고,
// EOF는 다시 취소한 뒤 읽기를 멈춘다(CRLF도 같은 줄).
TEST(Progress_ParentControlListenerHandlesLines) {
    Hub hub;
    REQUIRE(hub.ready);
    ProgressTaskHandle task =
        hub.Begin("Child work", ProgressPresentation::Background, ProgressCancel::UserCancellable);
    ScriptedInput input;
    input.text = "hello\nwhat\r\ncancel\r\n";
    ParentControlListenerDesc desc;
    desc.read = &ReadScripted;
    desc.context = &input;
    ParentControlListener listener;
    REQUIRE(listener.Start(hub.allocator, desc));
    CHECK(listener.Start(hub.allocator, desc).Error() == ErrorCode::AlreadyExists);
    // EOF에서 스레드 본문이 끝난다 — 다 읽은 뒤 Stop이 회수한다.
    input.drained.Wait();
    listener.Stop();
    CHECK(listener.CancelRequests() == 2);
    CHECK(listener.UnknownLines() == 2);
    CHECK(task.Token().IsCancellationRequested());
    CHECK(task.Finish(ProgressOutcome::Cancelled));
}

// 7.4절: 전체 취소를 받은 뒤 시작하는 취소 가능한 최상위 작업도 취소된 채 시작한다(취소가 명령의
// 첫 Begin보다 먼저 와도 사라지지 않음). 취소 불가·하위 작업은 그대로이고 다시 켜면 지워진다.
TEST(Progress_CancelAllReachesLaterTopLevelTasks) {
    {
        Hub hub;
        REQUIRE(hub.ready);
        ProgressHub::Get().RequestCancelAllTopLevel();
        ProgressTaskHandle later =
            hub.Begin("Later", ProgressPresentation::Background, ProgressCancel::UserCancellable);
        ProgressTaskHandle fixed = hub.Begin("Fixed");
        CHECK(later.Token().IsCancellationRequested());
        CHECK(!fixed.Token().IsCancellationRequested());
        CHECK(later.Finish(ProgressOutcome::Cancelled));
        CHECK(fixed.Finish(ProgressOutcome::Succeeded));
    }

    Hub again;
    REQUIRE(again.ready);
    ProgressTaskHandle fresh =
        again.Begin("Fresh", ProgressPresentation::Background, ProgressCancel::UserCancellable);
    CHECK(!fresh.Token().IsCancellationRequested());
    CHECK(fresh.Finish(ProgressOutcome::Succeeded));
}

// 9.3절: 사건이 유실되면(lost) begin을 쓴 작업 가운데 스냅샷에서 사라진 것에 abandoned end를
// 보충한다(end 사건을 잃어도 받는 쪽 하위 작업이 남지 않게).
TEST(Progress_JsonlWriterFillsLostEnds) {
    Hub hub(4);
    REQUIRE(hub.ready);
    Lines lines(hub.allocator);
    ProgressJsonlWriterDesc desc;
    desc.write = &Lines::Json;
    desc.context = &lines;
    ProgressJsonlWriter jsonl(hub.allocator, desc);
    ProgressTaskHandle gone = hub.Begin("Gone");
    REQUIRE(jsonl.Pump(0));
    CHECK(gone.Finish(ProgressOutcome::Succeeded));
    for (uint32 index = 0; index < 4; ++index) {
        ProgressTaskHandle burst = hub.Begin("Burst");
        CHECK(burst.Finish(ProgressOutcome::Succeeded));
    }

    REQUIRE(jsonl.Pump(0));
    StringBuilder<256> expected(hub.allocator);
    REQUIRE(expected.TryAppendFormat(
        "{{\"v\":1,\"e\":\"end\",\"id\":{},\"outcome\":\"abandoned\",\"detail\":\"\","
        "\"detailId\":\"\",\"detailRev\":0}}",
        gone.Id()));
    CHECK(lines.Has(expected.View()) || lines.Dump());
}

} // namespace progress_output_tests_file
