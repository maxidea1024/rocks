#include <TestSupport/Test.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Diagnostics/LogCapture.h>
#include <Core/Diagnostics/LogContext.h>
#include <Core/Diagnostics/LogLayout.h>
#include <Core/Diagnostics/LogSystem.h>
#include <Core/Diagnostics/RuntimeLogSink.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/Json.h>
#include <Core/Text/StringBuilder.h>
#include <Core/Time/DateTime.h>
#include <Platform/Atomics.h>
#include <Platform/Memory.h>

using namespace rocks;

namespace rocks::test {
uint64 OperatorNewCalls() noexcept;
} // namespace rocks::test

namespace {

constexpr LogCategory LogLayoutTests{"Tests.Layout"};

// 레이아웃 출력을 모은다.
struct Collected {
    char bytes[8192]{};
    usize count = 0;

    static void Append(void* raw, StringView text) noexcept {
        auto& self = *static_cast<Collected*>(raw);
        for (usize index = 0; index < text.ByteCount() && self.count < sizeof(self.bytes);
             ++index) {
            self.bytes[self.count++] = text.Data()[index];
        }
    }

    StringView View() const noexcept {
        return StringView(bytes, count);
    }
};

// sink 출력(LogTextOutput)을 모은다.
struct TextOutput {
    char bytes[65536]{};
    usize count = 0;
    uint32 writes = 0;
    uint32 flushes = 0;

    static Result<void> Write(void* raw, StringView view) noexcept {
        auto& self = *static_cast<TextOutput*>(raw);
        ++self.writes;
        if (view.ByteCount() > sizeof(self.bytes) - self.count) {
            return Result<void>::Failure(ErrorCode::DependencyFailed);
        }

        platform::MemoryCopy(self.bytes + self.count, view.Data(), view.ByteCount());
        self.count += view.ByteCount();
        return Result<void>::Success();
    }

    static Result<void> Flush(void* raw) noexcept {
        ++static_cast<TextOutput*>(raw)->flushes;
        return Result<void>::Success();
    }

    LogTextOutput Output() noexcept {
        return {this, Write, Flush};
    }

    StringView View() const noexcept {
        return StringView(bytes, count);
    }
};

class CountingAllocator final : public Allocator {
public:
    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        (void)platform::AtomicFetchAdd<platform::MemoryOrder::Relaxed>(&allocations, uint64{1});
        return inner.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        return inner.Deallocate(address);
    }

    uint64 Allocations() const noexcept {
        return platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&allocations);
    }

    SystemAllocator inner;
    uint64 allocations = 0;
};

// 2026-11-17T09:30:00.2505Z(밀리초 아래 0.5 ms가 있다)의 tick이다.
int64 FixedUtcTicks() noexcept {
    constexpr int64 FractionTicks = 2505000;
    return DateTime::TryFromDate(2026, 11, 17, 9, 30, 0).Value().Ticks() + FractionTicks;
}

// 모든 칸을 채운 고정 기록이다(시계에 기대지 않는다).
struct FixedEvent {
    LogProperty properties[5];
    LogProperty context[2];
    LogEvent event;

    FixedEvent() noexcept {
        properties[0].name = "Count";
        properties[0].text = "3";
        properties[0].kind = LogPropertyKind::Signed;
        properties[0].signedValue = -3;
        properties[1].name = "Ratio";
        properties[1].text = "0.5";
        properties[1].kind = LogPropertyKind::Float;
        properties[1].floatValue = 0.5;
        properties[2].name = "Ready";
        properties[2].text = "true";
        properties[2].kind = LogPropertyKind::Bool;
        properties[2].boolValue = true;
        properties[3].name = "Extra";
        properties[3].text = "nan";
        properties[3].kind = LogPropertyKind::String;
        properties[4].name = "Size";
        properties[4].text = "18446744073709551615";
        properties[4].kind = LogPropertyKind::Unsigned;
        properties[4].unsignedValue = NumericLimits<uint64>::Max();
        context[0].name = "World";
        context[0].text = "Main";
        context[1].name = "Stage";
        context[1].text = "BeginPlay";
        event.level = LogLevel::Warning;
        event.category = "Engine.Core";
        event.messageTemplate = "{Count} items {Ratio} {Ready} {Size}";
        event.eventId = 0xabc;
        event.message = "3 items 0.5 true 18446744073709551615";
        event.properties = Span<const LogProperty>(properties);
        event.context = Span<const LogProperty>(context);
        event.location.file = "Engine/Private/Engine.cpp";
        event.location.function = "Initialize";
        event.location.line = 118;
        event.threadId = 42;
        event.threadName = "Main";
        event.frame = 7;
        event.monotonicNanoseconds = 13345678901;
        event.utcTicks = FixedUtcTicks();
    }
};

LogLayoutOptions FixedOptions() noexcept {
    LogLayoutOptions options;
    options.startMonotonicNanoseconds = 1000000000;
    return options;
}

StringView Render(StringView pattern, const LogEvent& event, Collected& out,
                  const LogLayoutOptions& options = FixedOptions()) noexcept {
    out.count = 0;
    auto layout = LogLayout::Parse(pattern, options);
    if (!layout) {
        return "<parse failed>";
    }

    layout.Value().Render(event, {&out, Collected::Append});
    return out.View();
}

} // namespace

TEST(LogLayoutRendersEveryToken) {
    FixedEvent fixed;
    Collected out;
    CHECK(Render("{Timestamp}", fixed.event, out) == "2026-11-17T09:30:00.2505Z");
    CHECK(Render("{Timestamp:ms}", fixed.event, out) == "2026-11-17T09:30:00.250Z");
    CHECK(Render("{Elapsed}", fixed.event, out) == "12.345");
    CHECK(Render("{Level}", fixed.event, out) == "Warning");
    CHECK(Render("{Level:u3}", fixed.event, out) == "WRN");
    CHECK(Render("{Category}", fixed.event, out) == "Engine.Core");
    CHECK(Render("{EventId}", fixed.event, out) == "0000000000000abc");
    CHECK(Render("{Thread}", fixed.event, out) == "Main");
    CHECK(Render("{Frame}", fixed.event, out) == "7");
    CHECK(Render("{Message}", fixed.event, out) == "3 items 0.5 true 18446744073709551615");
    CHECK(Render("{Template}", fixed.event, out) == "{Count} items {Ratio} {Ready} {Size}");
    // 템플릿 자리에 쓰이지 않은 속성만 나온다.
    CHECK(Render("{Properties}", fixed.event, out) == "Extra=nan");
    CHECK(Render("{Context}", fixed.event, out) == "World=Main Stage=BeginPlay");
    CHECK(Render("{Location}", fixed.event, out) == "Engine/Private/Engine.cpp:118 Initialize");
    CHECK(Render("{NewLine}", fixed.event, out) == "\n");
    CHECK(Render("{{{Level:u3}}}", fixed.event, out) == "{WRN}");

    const char* const u3[] = {"TRC", "DBG", "INF", "WRN", "ERR", "FTL"};
    for (usize index = 0; index < LogLevelCount; ++index) {
        fixed.event.level = static_cast<LogLevel>(index);
        CHECK(Render("{Level:u3}", fixed.event, out) == u3[index]);
    }

    // 스레드 이름이 없으면 id, {Location}은 최소 레벨 아래면 빈 값이다.
    fixed.event.threadName = StringView();
    CHECK(Render("{Thread}", fixed.event, out) == "42");
    fixed.event.level = LogLevel::Info;
    CHECK(Render("{Location}", fixed.event, out).IsEmpty());
    LogLayoutOptions options = FixedOptions();
    options.locationMinimum = LogLevel::Info;
    CHECK(Render("{Location}", fixed.event, out, options) ==
          "Engine/Private/Engine.cpp:118 Initialize");
}

// 빈 값 생략 규칙: 대괄호 짝, 앞 공백, 붙은 토큰 사이 공백.
TEST(LogLayoutOmitsEmptyTokensWithSeparators) {
    FixedEvent fixed;
    Collected out;
    CHECK(Render(DefaultConsoleLogLayout, fixed.event, out) ==
          "[2026-11-17T09:30:00.250Z][Warning][Engine.Core] 3 items 0.5 true 18446744073709551615 "
          "World=Main Stage=BeginPlay Engine/Private/Engine.cpp:118 Initialize\n");

    // UTC 없음 → "[{Timestamp:ms}]" 통째로, 문맥 없음·Info(위치 없음) → 붙은 토큰이 사라진다.
    fixed.event.utcTicks = 0;
    fixed.event.context = Span<const LogProperty>();
    fixed.event.level = LogLevel::Info;
    CHECK(Render(DefaultConsoleLogLayout, fixed.event, out) ==
          "[Info][Engine.Core] 3 items 0.5 true 18446744073709551615\n");

    // 문맥이 비어도 위치는 메시지에 붙는다(빈 토큰을 건너뛰어도 붙은 것으로 본다).
    fixed.event.level = LogLevel::Error;
    CHECK(Render("{Message}{Context}{Location}", fixed.event, out) ==
          "3 items 0.5 true 18446744073709551615 Engine/Private/Engine.cpp:118 Initialize");

    // 앞 공백 하나와 대괄호 짝.
    fixed.event.eventId = 0;
    CHECK(Render("a {EventId}|", fixed.event, out) == "a|");
    CHECK(Render("x[{EventId}]y", fixed.event, out) == "xy");
    CHECK(Render("x[{EventId}y", fixed.event, out) == "x[y");
    // 리터럴이 있으면 공백을 넣지 않는다.
    CHECK(Render("{Level}:{Category}", fixed.event, out) == "Error:Engine.Core");
    CHECK(Render("{Level}{Category}", fixed.event, out) == "Error Engine.Core");
}

// 문법 검사는 상수 평가로 쓸 수 있다(설정 검증의 기본값 검사). 이유는 영어 리터럴이다.
static_assert(CheckLogLayoutSyntax(DefaultConsoleLogLayout).IsEmpty());
static_assert(CheckLogLayoutSyntax("[{Level:u3}] {Message}{NewLine}").IsEmpty());
static_assert(CheckLogLayoutSyntax("").IsEmpty());
static_assert(CheckLogLayoutSyntax("{Unknown}") == "unknown layout token");
static_assert(CheckLogLayoutSyntax("{Level:u4}") == "unsupported format for this layout token");
static_assert(CheckLogLayoutSyntax("{0}") == "positional holes are not layout tokens");
static_assert(!CheckLogLayoutSyntax("{Level").IsEmpty());

// Parse의 오류 출력은 위치(토큰의 '{')와 이유를 준다.
TEST(LogLayoutParseReportsErrorOffset) {
    LogLayoutParseError error;
    CHECK(LogLayout::Parse("[{Level}] {Mesage}", {}, &error).Error() == ErrorCode::InvalidArgument);
    CHECK(error.offset == 10);
    CHECK(error.code == ErrorCode::InvalidArgument);
    CHECK(error.reason == "unknown layout token");
    CHECK(LogLayout::Parse("ab {Timestamp:s}", {}, &error).Error() == ErrorCode::InvalidArgument);
    CHECK(error.offset == 3);
    CHECK(error.reason == "unsupported format for this layout token");
    CHECK(LogLayout::Parse("abc}", {}, &error).Error() == ErrorCode::InvalidArgument);
    CHECK(error.offset == 3);
}

TEST(LogLayoutRejectsBadPatterns) {
    const char* const invalid[] = {"{Unknown}", "{Level:u4}", "{Timestamp:xx}", "{Message:x}",
                                   "{}",        "{0}",        "{Level",         "Level}"};
    for (const char* pattern : invalid) {
        CHECK(LogLayout::Parse(pattern).Error() == ErrorCode::InvalidArgument);
    }

    char tooMany[33 * 7 + 1]{};
    for (usize index = 0; index < 33; ++index) {
        platform::MemoryCopy(tooMany + index * 7, "{Level}", 7);
    }

    CHECK(LogLayout::Parse(tooMany).Error() == ErrorCode::CapacityExceeded);
    char tooLong[MaxLogLayoutTextBytes + 2]{};
    for (usize index = 0; index + 1 < sizeof(tooLong); ++index) {
        tooLong[index] = 'a';
    }

    CHECK(LogLayout::Parse(tooLong).Error() == ErrorCode::CapacityExceeded);
    CHECK(LogLayout::Parse(DefaultConsoleLogLayout).IsOk());
    CHECK(LogLayout::DefaultConsole().TokenCount() == 7);
}

TEST(DateTimeMillisecondFormatIsFixedWidth) {
    const DateTime whole = DateTime::TryFromDate(2026, 11, 12, 9, 30, 0).Value();
    char storage[64];
    FixedStringBuilder text{Span<char>(storage)};
    REQUIRE(text.TryAppendFormat("{:ms}|{}", whole, whole).IsOk());
    CHECK(text.View() == "2026-11-12T09:30:00.000Z|2026-11-12T09:30:00Z");
    const DateTime quarter = DateTime::TryFromTicks(whole.Ticks() + 2500000).Value();
    text.Clear();
    REQUIRE(text.TryAppendFormat("{:ms}|{}", quarter, quarter).IsOk());
    CHECK(text.View() == "2026-11-12T09:30:00.250Z|2026-11-12T09:30:00.25Z");
    text.Clear();
    CHECK(text.TryAppendFormat("{:xx}", quarter).Error() == ErrorCode::InvalidArgument);
}

// compact·고정 frame·중복 키 확인 끔: 한 줄 출력, 고정 깊이, 할당 없음.
TEST(JsonWriterCompactFixedFramesWithoutAllocation) {
    const uint64 attempts = detail::FixedStringBuilderAllocationAttempts();
    const uint64 news = test::OperatorNewCalls();
    char storage[512];
    FixedStringBuilder text{Span<char>(storage)};
    JsonWriterOptions options;
    options.compact = true;
    options.fixedFrames = true;
    options.detectDuplicateKeys = false;
    {
        JsonWriter json(text, options);
        REQUIRE(json.BeginObject().IsOk());
        REQUIRE(json.Key("a").IsOk() && json.Int64(1).IsOk());
        REQUIRE(json.Key("b").IsOk() && json.BeginArray().IsOk());
        REQUIRE(json.Bool(true).IsOk() && json.Null().IsOk() && json.Double(0.5).IsOk());
        REQUIRE(json.EndArray().IsOk());
        // 중복 키 확인을 끄면 호출자가 책임진다(그대로 쓴다).
        REQUIRE(json.Key("a").IsOk() && json.String("x").IsOk());
        REQUIRE(json.EndObject().IsOk() && json.Finish().IsOk());
    }

    CHECK(text.View() == "{\"a\":1,\"b\":[true,null,0.5],\"a\":\"x\"}\n");
    CHECK(detail::FixedStringBuilderAllocationAttempts() == attempts);
    CHECK(test::OperatorNewCalls() == news);

    // 고정 frame은 FixedFrameCapacity 깊이까지다.
    text.Clear();
    JsonWriter deep(text, options);
    for (uint32 depth = 0; depth < JsonWriter::FixedFrameCapacity; ++depth) {
        REQUIRE(deep.BeginArray().IsOk());
    }

    CHECK(deep.BeginArray().Error() == ErrorCode::CapacityExceeded);

    // 기본 옵션은 그대로(들여쓰기·중복 키 거부).
    SystemAllocator allocator;
    StringBuilder<> pretty(allocator);
    JsonWriter normal(pretty);
    REQUIRE(normal.BeginObject().IsOk() && normal.Key("a").IsOk() && normal.Int64(1).IsOk());
    CHECK(normal.Key("a").Error() == ErrorCode::InvalidArgument);
}

// JSON lines: 한 줄 객체를 Core JsonDocument로 다시 읽어 필드와 타입을 확인한다.
TEST(JsonLinesLogSinkRoundTripsTypedFields) {
    FixedEvent fixed;
    fixed.event.flags = LogEventFlags::MessageTruncated | LogEventFlags::ContextTruncated;
    TextOutput output;
    JsonLinesLogSink sink(output.Output());
    REQUIRE(sink.Write(fixed.event).IsOk());
    CHECK(output.writes == 1 && output.flushes == 0);
    REQUIRE(sink.Flush().IsOk());
    CHECK(output.flushes == 1);
    const StringView line = output.View();
    REQUIRE(line.EndsWith("}\n", StringComparison::CaseSensitive));
    // 한 줄이다(끝 줄바꿈 하나뿐).
    usize newlines = 0;
    for (char character : line) {
        newlines += character == '\n' ? 1 : 0;
    }

    CHECK(newlines == 1);

    SystemAllocator allocator;
    JsonDocument document(allocator);
    REQUIRE(document.Parse(line).Succeeded());
    const JsonValue root = document.Root();
    CHECK(root.Find("t").GetString() == "2026-11-17T09:30:00.250Z");
    CHECK(root.Find("mono").TryGetUInt64().Value() == 13345678901u);
    CHECK(root.Find("lvl").GetString() == "Warning");
    CHECK(root.Find("cat").GetString() == "Engine.Core");
    CHECK(root.Find("event").GetString() == "0000000000000abc");
    CHECK(root.Find("tpl").GetString() == "{Count} items {Ratio} {Ready} {Size}");
    CHECK(root.Find("msg").GetString() == "3 items 0.5 true 18446744073709551615");
    const JsonValue props = root.Find("props");
    CHECK(props.Find("Count").Type() == JsonType::Integer &&
          props.Find("Count").TryGetInt64().Value() == -3);
    CHECK(props.Find("Ratio").Type() == JsonType::Double &&
          props.Find("Ratio").TryGetDouble().Value() == 0.5);
    CHECK(props.Find("Ready").Type() == JsonType::Bool && props.Find("Ready").GetBool());
    CHECK(props.Find("Extra").IsString() && props.Find("Extra").GetString() == "nan");
    CHECK(props.Find("Size").TryGetUInt64().Value() == NumericLimits<uint64>::Max());
    CHECK(root.Find("ctx").Find("World").GetString() == "Main");
    CHECK(root.Find("ctx").Find("Stage").GetString() == "BeginPlay");
    CHECK(root.Find("src").Find("file").GetString() == "Engine/Private/Engine.cpp");
    CHECK(root.Find("src").Find("line").TryGetInt64().Value() == 118);
    CHECK(root.Find("src").Find("func").GetString() == "Initialize");
    CHECK(root.Find("thread").GetString() == "Main");
    CHECK(root.Find("frame").TryGetInt64().Value() == 7);
    const JsonValue flags = root.Find("flags");
    REQUIRE(flags.Size() == 2);
    CHECK(flags.At(0).GetString() == "MessageTruncated" &&
          flags.At(1).GetString() == "ContextTruncated");

    // UTC·위치가 없으면 null이다.
    output.count = 0;
    fixed.event.utcTicks = 0;
    fixed.event.location = SourceLocation();
    REQUIRE(sink.Write(fixed.event).IsOk());
    REQUIRE(document.Parse(output.View()).Succeeded());
    CHECK(document.Root().Find("t").IsNull() && document.Root().Find("src").IsNull());
}

// 버퍼를 넘는 기록도 늘 유효한 JSON 한 줄(대체 줄)이다.
TEST(JsonLinesLogSinkFallsBackForOversizedRecords) {
    FixedEvent fixed;
    static char huge[JsonLinesLogSink::BufferBytes * 2];
    for (char& character : huge) {
        character = 'h';
    }

    fixed.event.message = StringView(huge, sizeof(huge));
    TextOutput output;
    JsonLinesLogSink sink(output.Output());
    REQUIRE(sink.Write(fixed.event).IsOk());
    SystemAllocator allocator;
    JsonDocument document(allocator);
    REQUIRE(document.Parse(output.View()).Succeeded());
    CHECK(document.Root().Find("lvl").GetString() == "Warning");
    CHECK(document.Root().Find("msg").GetString().ByteCount() < sizeof(huge));
    CHECK(document.Root().Find("flags").At(0).GetString() == "MessageTruncated");
}

namespace {

// 캡처·렌더로 문맥을 얻는다.
struct ContextRender {
    char message[256]{};
    char texts[64]{};
    LogProperty properties[MaxLogArguments];
    LogProperty context[MaxLogContextDepth];
    LogEvent event;

    bool From(Span<const uint8> capture) noexcept {
        const LogRenderTarget target{Span<char>(message), Span<char>(texts),
                                     Span<LogProperty>(properties), Span<LogProperty>(context)};
        return RenderLogCapture(capture, nullptr, target, event).IsOk();
    }
};

} // namespace

TEST(LogContextScopeIsCapturedNestedAndCopied) {
    uint8 slot[LogSlotBytes];
    usize used = 0;
    {
        char world[] = "Main";
        LogContextScope worldScope("World", StringView(world, 4));
        {
            LogContextScope stage("Stage", "BeginPlay");
            const auto captured =
                CaptureLog({Span<uint8>(slot)}, LogLevel::Info, LogLayoutTests, "inner");
            REQUIRE(captured.IsOk());
            used = captured.Value();
        }

        // 캡처 뒤 원본 값을 바꿔도 기록은 복사본을 쓴다.
        world[0] = 'X';
    }

    ContextRender rendered;
    REQUIRE(rendered.From(Span<const uint8>(slot, used)));
    REQUIRE(rendered.event.context.Size() == 2);
    CHECK(rendered.event.context[0].name == "World" && rendered.event.context[0].text == "Main");
    CHECK(rendered.event.context[1].name == "Stage" &&
          rendered.event.context[1].text == "BeginPlay");
    CHECK(!HasAnyLogEventFlags(rendered.event.flags, LogEventFlags::ContextTruncated));

    // 범위가 끝나면 문맥이 없다.
    const auto outside = CaptureLog({Span<uint8>(slot)}, LogLevel::Info, LogLayoutTests, "outer");
    REQUIRE(outside.IsOk());
    ContextRender plain;
    REQUIRE(plain.From(Span<const uint8>(slot, outside.Value())));
    CHECK(plain.event.context.Size() == 0);
}

// 깊이를 넘으면 안쪽 범위를 버리고 ContextTruncated를 세운다.
TEST(LogContextScopeOverflowDropsInnermost) {
    uint8 slot[LogSlotBytes];
    LogContextScope s0("C0", "0");
    LogContextScope s1("C1", "1");
    LogContextScope s2("C2", "2");
    LogContextScope s3("C3", "3");
    LogContextScope s4("C4", "4");
    LogContextScope s5("C5", "5");
    LogContextScope s6("C6", "6");
    LogContextScope s7("C7", "7");
    usize used = 0;
    {
        LogContextScope s8("C8", "8");
        const auto captured = CaptureLog({Span<uint8>(slot)}, LogLevel::Info, LogLayoutTests, "x");
        REQUIRE(captured.IsOk());
        used = captured.Value();
    }

    ContextRender rendered;
    REQUIRE(rendered.From(Span<const uint8>(slot, used)));
    REQUIRE(rendered.event.context.Size() == MaxLogContextDepth);
    CHECK(rendered.event.context[7].name == "C7");
    CHECK(HasAnyLogEventFlags(rendered.event.flags, LogEventFlags::ContextTruncated));

    // 넘친 범위가 닫히면 다시 잘림 없음이다.
    const auto after = CaptureLog({Span<uint8>(slot)}, LogLevel::Info, LogLayoutTests, "y");
    REQUIRE(after.IsOk());
    ContextRender full;
    REQUIRE(full.From(Span<const uint8>(slot, after.Value())));
    CHECK(full.event.context.Size() == MaxLogContextDepth);
    CHECK(!HasAnyLogEventFlags(full.event.flags, LogEventFlags::ContextTruncated));
}

namespace {

// 같은 기록 열을 남긴다(Sync와 Manual 비교용, 시각·위치 없는 레이아웃).
void EmitWithContext() noexcept {
    LogContextScope world("World", "Main");
    Log::Info(LogLayoutTests, "spawned {Count}", 3);
    {
        LogContextScope stage("Stage", "BeginPlay");
        Log::Warning(LogLayoutTests, "slow frame {Ms:.1f}", 40.25);
    }

    Log::Error(LogLayoutTests, "plain");
}

constexpr StringView CompareLayout = "[{Level:u3}][{Category}] {Message}{Context}{NewLine}";

// 앞선 시험이 설치 없이 남긴 버림 수를 비운다(설치 때 알림 줄이 섞이지 않게).
void ResetUninstalledDrops() noexcept {
    SystemAllocator allocator;
    LogSystem reset(allocator, LogSystemConfig::ForTool());
    (void)reset.Start();
    LogInstallScope installed(reset);
}

} // namespace

TEST(TextLogSinkSyncAndManualProduceIdenticalLines) {
    ResetUninstalledDrops();
    const auto layout = LogLayout::Parse(CompareLayout);
    REQUIRE(layout.IsOk());
    SystemAllocator allocator;
    TextOutput syncOutput;
    TextOutput manualOutput;
    {
        TextLogSink sink(syncOutput.Output(), layout.Value());
        LogSystem logs(allocator, LogSystemConfig::ForTool());
        REQUIRE(logs.AddSink(sink).IsOk());
        REQUIRE(logs.Start().IsOk());
        LogInstallScope installed(logs);
        EmitWithContext();
        // Sync는 기록마다 flush한다.
        CHECK(syncOutput.flushes == 3);
    }

    {
        LogSystemConfig config;
        config.mode = LogMode::Manual;
        TextLogSink sink(manualOutput.Output(), layout.Value());
        LogSystem logs(allocator, config);
        REQUIRE(logs.AddSink(sink).IsOk());
        REQUIRE(logs.Start().IsOk());
        LogInstallScope installed(logs);
        EmitWithContext();
        CHECK(logs.Pump() == 3);
        // Manual·Async는 묶음 끝에 한 번 flush한다.
        CHECK(manualOutput.flushes == 1);
    }

    CHECK(syncOutput.View() == manualOutput.View());
    CHECK(syncOutput.View() == "[INF][Tests.Layout] spawned 3 World=Main\n"
                               "[WRN][Tests.Layout] slow frame 40.2 World=Main Stage=BeginPlay\n"
                               "[ERR][Tests.Layout] plain World=Main\n");
}

// sink 경로(레이아웃·JSON 렌더, LogSystem 출력 쪽)는 할당하지 않는다: 세는 allocator(Start 뒤
// 0회)와 이 스레드의 operator new·거절 allocator 카운터.
TEST(LogSinksDoNotAllocate) {
    ResetUninstalledDrops();
    const uint64 attempts = detail::FixedStringBuilderAllocationAttempts();
    const uint64 news = test::OperatorNewCalls();
    FixedEvent fixed;
    {
        TextOutput text;
        TextOutput json;
        TextLogSink textSink(text.Output());
        JsonLinesLogSink jsonSink(json.Output());
        for (uint32 round = 0; round < 100; ++round) {
            REQUIRE(textSink.Write(fixed.event).IsOk());
            REQUIRE(jsonSink.Write(fixed.event).IsOk());
        }

        CHECK(textSink.Flush().IsOk() && jsonSink.Flush().IsOk());
    }

    CHECK(detail::FixedStringBuilderAllocationAttempts() == attempts);
    CHECK(test::OperatorNewCalls() == news);

    CountingAllocator allocator;
    LogSystemConfig config;
    config.mode = LogMode::Manual;
    TextOutput text;
    TextOutput json;
    TextLogSink textSink(text.Output());
    JsonLinesLogSink jsonSink(json.Output());
    LogSystem logs(allocator, config);
    REQUIRE(logs.AddSink(textSink).IsOk());
    REQUIRE(logs.AddSink(jsonSink).IsOk());
    REQUIRE(logs.Start().IsOk());
    const uint64 afterStart = allocator.Allocations();
    {
        LogInstallScope installed(logs);
        LogContextScope world("World", "Main");
        for (uint32 index = 0; index < 50; ++index) {
            Log::Warning(LogLayoutTests, "record {Index} {Ratio} {Ready}", index, 0.25, true);
        }

        CHECK(logs.Pump() == 50);
    }

    CHECK(allocator.Allocations() == afterStart);
    CHECK(test::OperatorNewCalls() == news);
    CHECK(detail::FixedStringBuilderAllocationAttempts() == attempts);
    CHECK(text.View().Contains("World=Main", StringComparison::CaseSensitive));
}
