#include <TraceAnalysis/TraceReport.h>

#include <Core/Memory/SystemAllocator.h>

namespace rocks {

namespace {

// 한 번에 결과를 모으는 JSON 쓰기다. 실패(메모리)는 처음 한 번만 기억하고 그 뒤 쓰기는 하지 않는다.
class JsonWriter {
public:
    explicit JsonWriter(Array<uint8>& out) noexcept : out_(out) {
    }

    bool Failed() const noexcept {
        return failed_;
    }

    void Raw(StringView text) noexcept {
        if (!failed_ && !out_.TryAppend(Span<const uint8>(
                            reinterpret_cast<const uint8*>(text.Data()), text.ByteCount()))) {
            failed_ = true;
        }
    }

    void Newline() noexcept {
        Raw("\n");
        for (uint32 level = 0; level < depth_; ++level) {
            Raw("  ");
        }
    }

    // 객체·배열의 시작과 끝이다. 원소 사이 쉼표는 Key·Element가 넣는다.
    void Open(char bracket) noexcept {
        const char text[2] = {bracket, '\0'};
        Raw(StringView(text, 1));
        ++depth_;
        first_[depth_] = true;
    }

    void Close(char bracket) noexcept {
        const bool empty = first_[depth_];
        --depth_;
        if (!empty) {
            Newline();
        }

        const char text[2] = {bracket, '\0'};
        Raw(StringView(text, 1));
    }

    void Element() noexcept {
        if (!first_[depth_]) {
            Raw(",");
        }

        first_[depth_] = false;
        Newline();
    }

    void Key(StringView key) noexcept {
        Element();
        String(key);
        Raw(": ");
    }

    void String(StringView text) noexcept {
        Raw("\"");
        constexpr char Hex[] = "0123456789abcdef";
        for (const char c : text) {
            const auto byte = static_cast<uint8>(c);
            if (c == '"' || c == '\\') {
                const char escaped[2] = {'\\', c};
                Raw(StringView(escaped, 2));
            } else if (byte < 0x20) {
                const char escaped[6] = {'\\', 'u', '0', '0', Hex[byte >> 4], Hex[byte & 15]};
                Raw(StringView(escaped, 6));
            } else {
                Raw(StringView(&c, 1));
            }
        }

        Raw("\"");
    }

    void Unsigned(uint64 value) noexcept {
        char digits[20];
        usize count = 0;
        do {
            digits[count++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value != 0);

        char text[20];
        for (usize index = 0; index < count; ++index) {
            text[index] = digits[count - 1 - index];
        }

        Raw(StringView(text, count));
    }

    void Signed(int64 value) noexcept {
        if (value < 0) {
            Raw("-");
            Unsigned(static_cast<uint64>(-(value + 1)) + 1);
        } else {
            Unsigned(static_cast<uint64>(value));
        }
    }

    void Hex64(uint64 value) noexcept {
        constexpr char Hex[] = "0123456789abcdef";
        char text[18] = {'0', 'x'};
        for (usize index = 0; index < 16; ++index) {
            text[2 + index] = Hex[(value >> (60 - index * 4)) & 15];
        }

        String(StringView(text, sizeof(text)));
    }

    void Bool(bool value) noexcept {
        Raw(value ? "true" : "false");
    }

    void KeyUnsigned(StringView key, uint64 value) noexcept {
        Key(key);
        Unsigned(value);
    }

    void KeyBool(StringView key, bool value) noexcept {
        Key(key);
        Bool(value);
    }

    void KeyString(StringView key, StringView value) noexcept {
        Key(key);
        String(value);
    }

private:
    static constexpr uint32 MaxDepth = 16;

    Array<uint8>& out_;
    uint32 depth_ = 0;
    bool first_[MaxDepth + 1] = {};
    bool failed_ = false;
};

// 이름 id의 표시 이름이다. 정의가 없으면 "<unknown:id>"를 buffer에 만든다.
StringView DisplayName(const TraceModel& model, uint32 id, char (&buffer)[32]) noexcept {
    const StringView name = model.ScopeName(id);
    if (!name.IsEmpty()) {
        return name;
    }

    constexpr StringView Prefix = "<unknown:";
    usize length = 0;
    for (const char c : Prefix) {
        buffer[length++] = c;
    }

    char digits[10];
    usize count = 0;
    do {
        digits[count++] = static_cast<char>('0' + id % 10);
        id /= 10;
    } while (id != 0);

    while (count != 0) {
        buffer[length++] = digits[--count];
    }

    buffer[length++] = '>';
    return StringView(buffer, length);
}

bool LessText(StringView left, StringView right) noexcept {
    const usize common =
        left.ByteCount() < right.ByteCount() ? left.ByteCount() : right.ByteCount();
    for (usize index = 0; index < common; ++index) {
        const auto a = static_cast<uint8>(left.Data()[index]);
        const auto b = static_cast<uint8>(right.Data()[index]);
        if (a != b) {
            return a < b;
        }
    }

    return left.ByteCount() < right.ByteCount();
}

// nearest-rank 백분위수다(정렬된 값, percent 1–100).
uint64 Percentile(const Array<uint64>& sorted, uint64 percent) noexcept {
    if (sorted.IsEmpty()) {
        return 0;
    }

    const uint64 rank = (percent * sorted.Size() + 99) / 100;
    return sorted[static_cast<usize>(rank == 0 ? 0 : rank - 1)];
}

struct ScopeGroup {
    uint32 nameId;
    StringView name;
};

void WriteThreadScopes(JsonWriter& json, const TraceModel& model, const TraceModelThread& thread,
                       Allocator& scratch, char (&buffer)[32], char (&other)[32]) noexcept {
    // 이름 id마다 묶는다. 표시 이름 순(같으면 id 순)으로 쓴다.
    Array<uint32> ids(scratch);
    for (const TraceModelScope& scope : thread.scopes) {
        if (!ids.Contains(scope.nameId) && !ids.TryPushBack(scope.nameId)) {
            return;
        }
    }

    ids.Sort([&](uint32 left, uint32 right) noexcept {
        const StringView a = DisplayName(model, left, buffer);
        const StringView b = DisplayName(model, right, other);
        if (LessText(a, b)) {
            return true;
        }

        return !LessText(b, a) && left < right;
    });

    json.Key("scopes");
    json.Open('[');
    Array<uint64> durations(scratch);
    const uint64 frequency = model.Decoder().Header().tickFrequency;
    for (const uint32 id : ids) {
        durations.Clear();
        uint64 unended = 0;
        uint64 total = 0;
        for (const TraceModelScope& scope : thread.scopes) {
            if (scope.nameId != id) {
                continue;
            }

            if (!scope.ended) {
                ++unended;
                continue;
            }

            const uint64 nanoseconds =
                TraceTicksToNanoseconds(scope.endTick - scope.beginTick, frequency);
            total += nanoseconds;
            if (!durations.TryPushBack(nanoseconds)) {
                return;
            }
        }

        durations.Sort();
        json.Element();
        json.Open('{');
        json.KeyString("name", DisplayName(model, id, buffer));
        json.KeyUnsigned("count", durations.Size());
        json.KeyUnsigned("unended", unended);
        json.KeyUnsigned("totalNs", total);
        json.KeyUnsigned("p50Ns", Percentile(durations, 50));
        json.KeyUnsigned("p95Ns", Percentile(durations, 95));
        json.KeyUnsigned("p99Ns", Percentile(durations, 99));
        json.Close('}');
    }

    json.Close(']');
}

void WriteTasks(JsonWriter& json, const TraceModel& model, Allocator& scratch) noexcept {
    uint64 executed = 0;
    uint64 failed = 0;
    uint64 cancelled = 0;
    for (const TraceModelTask& task : model.Tasks()) {
        executed += task.startedTick != 0 ? 1 : 0;
        failed += task.completed && !task.error.IsEmpty() ? 1 : 0;
        cancelled += task.cancelled ? 1 : 0;
    }

    json.Key("tasks");
    json.Open('{');
    json.KeyUnsigned("count", model.Tasks().Size());
    json.KeyUnsigned("executed", executed);
    json.KeyUnsigned("failed", failed);
    json.KeyUnsigned("cancelled", cancelled);
    json.KeyUnsigned("edges", model.Edges().Size());

    Array<uint64> path(scratch);
    const uint64 frequency = model.Decoder().Header().tickFrequency;
    uint64 executionTicks = 0;
    uint64 firstCreated = 0;
    uint64 lastCompleted = 0;
    if (model.CriticalPath(path)) {
        for (usize index = 0; index < path.Size(); ++index) {
            for (const TraceModelTask& task : model.Tasks()) {
                if (task.id != path[index]) {
                    continue;
                }

                if (task.startedTick != 0 && task.finishedTick >= task.startedTick) {
                    executionTicks += task.finishedTick - task.startedTick;
                }

                if (index == 0) {
                    firstCreated = task.createdTick;
                }

                if (index + 1 == path.Size()) {
                    lastCompleted = task.completedTick;
                }
            }
        }
    }

    json.Key("criticalPath");
    json.Open('{');
    json.Key("tasks");
    json.Open('[');
    for (const uint64 id : path) {
        json.Element();
        json.Unsigned(id);
    }

    json.Close(']');
    json.KeyUnsigned("executionNs", TraceTicksToNanoseconds(executionTicks, frequency));
    json.KeyUnsigned("spanNs",
                     firstCreated != 0 && lastCompleted >= firstCreated
                         ? TraceTicksToNanoseconds(lastCompleted - firstCreated, frequency)
                         : 0);
    json.Close('}');
    json.Close('}');
}

void WriteCounters(JsonWriter& json, const TraceModel& model, Allocator& scratch) noexcept {
    Array<const TraceModelCounter*> counters(scratch);
    for (const TraceModelCounter& counter : model.Counters()) {
        if (!counters.TryPushBack(&counter)) {
            return;
        }
    }

    counters.Sort([](const TraceModelCounter* left, const TraceModelCounter* right) noexcept {
        if (LessText(left->name, right->name)) {
            return true;
        }

        return !LessText(right->name, left->name) && left->id < right->id;
    });

    json.Key("counters");
    json.Open('[');
    for (const TraceModelCounter* counter : counters) {
        json.Element();
        json.Open('{');
        json.KeyString("name", counter->name);
        json.KeyUnsigned("kind", counter->kind);
        json.KeyUnsigned("unit", counter->unit);
        json.KeyUnsigned("samples", counter->samples);
        if (counter->kind == 0) {
            json.Key("last");
            json.Signed(static_cast<int64>(counter->lastBits));
        } else {
            json.Key("lastBits");
            json.Hex64(counter->lastBits);
        }

        json.Close('}');
    }

    json.Close(']');
}

} // namespace

Result<void> WriteTraceReportJson(const TraceModel& model, Array<uint8>& out) noexcept {
    SystemAllocator scratch;
    JsonWriter json(out);
    char buffer[32];
    char other[32];
    const TraceModelSummary& summary = model.Summary();
    const DecodedHeader& header = model.Decoder().Header();
    {
        json.Open('{');
        json.KeyString("format", "rocks-trace-report");
        json.KeyUnsigned("version", TraceReportVersion);

        json.Key("session");
        json.Open('{');
        json.Key("guidHigh");
        json.Hex64(header.sessionGuid.high);
        json.Key("guidLow");
        json.Hex64(header.sessionGuid.low);
        json.KeyUnsigned("tickFrequency", header.tickFrequency);
        json.Close('}');

        json.Key("summary");
        json.Open('{');
        json.KeyBool("truncated", summary.truncated);
        json.KeyBool("definitionsIncomplete", summary.definitionsIncomplete);
        json.KeyBool("attachedMidSession", summary.attachedMidSession);
        json.KeyUnsigned("attachOrderNumber", summary.attachOrderNumber);
        json.KeyUnsigned("lostRecords", summary.lostRecords);
        json.KeyUnsigned("lossRanges", summary.lossRanges);
        json.KeyUnsigned("definitionsDropped", summary.definitionsDropped);
        json.KeyUnsigned("missingScopeNames", summary.missingScopeNames);
        json.KeyUnsigned("truncatedStreams", summary.truncatedStreams);
        json.KeyUnsigned("undecodableStreams", summary.undecodableStreams);
        json.KeyUnsigned("orphanScopeEnds", summary.orphanScopeEnds);
        json.KeyUnsigned("unendedScopes", summary.unendedScopes);
        json.KeyUnsigned("orderGaps", summary.orderGaps);
        json.KeyUnsigned("marks", summary.marks);
        json.Key("channelLoss");
        json.Open('[');
        for (const TraceModelChannelLoss& loss : model.ChannelLosses()) {
            if (loss.lost == 0) {
                continue;
            }

            json.Element();
            json.Open('{');
            json.KeyUnsigned("channel", loss.channelId);
            json.KeyString("name", loss.name);
            json.KeyUnsigned("lost", loss.lost);
            json.Close('}');
        }

        json.Close(']');
        json.Close('}');

        json.Key("threads");
        json.Open('[');
        for (const TraceModelThread& thread : model.Threads()) {
            json.Element();
            json.Open('{');
            json.KeyUnsigned("id", thread.threadId);
            json.KeyString("name", thread.name);
            json.KeyString("group", thread.group);
            json.Key("sortOrder");
            json.Signed(thread.sortOrder);
            json.KeyUnsigned("lostRecords", thread.lostRecords);
            json.KeyUnsigned("orphanEnds", thread.orphanEnds);
            json.KeyBool("complete", thread.status == TraceDecodeStatus::Complete);
            WriteThreadScopes(json, model, thread, scratch, buffer, other);
            json.Close('}');
        }

        json.Close(']');
        WriteTasks(json, model, scratch);
        WriteCounters(json, model, scratch);
        json.Close('}');
        json.Raw("\n");
    }

    return json.Failed() ? Result<void>::Failure(ErrorCode::OutOfMemory) : Result<void>::Success();
}

} // namespace rocks
