#include <TestSupport/Test.h>
#include <TraceCapture/Tests/TraceCaptureTestSupport.h>
#include <Core/Memory/SystemAllocator.h>
#include <Task/TaskScheduler.h>
#include <Task/WorkerScheduler.h>
#include <TraceCapture/TraceContext.h>
#include <TraceCapture/TraceCpuScope.h>
#include <TraceCapture/TraceTask.h>

// Task 모듈의 trace 연동 시험이다(Docs/Design/TraceSystem.ko.md 7절, TR3). 고정 task 그래프를
// 실행하고 기록에서 의존 간선·실행 집합·완료 상태를 복원해 직렬 oracle과 비교한다. 순서는 gate로만
// 맞춘다.
using namespace rocks;
using namespace rocks::tracetest;

namespace {

using Format = TraceStandardFormat;

// 고정 그래프: 0 A, 1 B←A, 2 C←A(실패), 3 D←B,C, 4 E, 5 F←B,E, 6 G←F, 7 H←D.
constexpr usize NodeCount = 8;
constexpr usize MaxEdges = 2;
struct Node {
    usize dependencies[MaxEdges];
    usize dependencyCount;
    bool fails;
};

constexpr Node Graph[NodeCount] = {
    {{0, 0}, 0, false}, {{0, 0}, 1, false}, {{0, 0}, 1, true},  {{1, 2}, 2, false},
    {{0, 0}, 0, false}, {{1, 4}, 2, false}, {{5, 0}, 1, false}, {{3, 0}, 1, false},
};

// 직렬 oracle: 위상 순서(색인 순서가 위상 순서다)로 돌며 의존이 모두 성공한 노드만 실행한다.
struct Oracle {
    bool executed[NodeCount] = {};
    bool succeeded[NodeCount] = {};

    Oracle() noexcept {
        for (usize node = 0; node < NodeCount; ++node) {
            bool ready = true;
            for (usize index = 0; index < Graph[node].dependencyCount; ++index) {
                ready = ready && succeeded[Graph[node].dependencies[index]];
            }

            executed[node] = ready;
            succeeded[node] = ready && !Graph[node].fails;
        }
    }
};

struct GraphRun {
    platform::ManualResetEvent release;
    uint32 executed[NodeCount] = {};
    uint64 request[NodeCount] = {};
};

// task 시작 사건 순서로 노드 번호를 매기기 위해, 노드 i는 i번째로 만든 task다(한 스레드에서 차례로
// 제출한다).
void SubmitGraph(Allocator& allocator, WorkerScheduler& scheduler, GraphRun& run,
                 WorkerTaskHandle (&handles)[NodeCount]) {
    for (usize node = 0; node < NodeCount; ++node) {
        WorkerTaskHandle dependencies[MaxEdges];
        for (usize index = 0; index < Graph[node].dependencyCount; ++index) {
            auto clone = handles[Graph[node].dependencies[index]].Clone();
            if (!clone) {
                platform::Fatal("Task trace test failed to clone a handle");
            }

            dependencies[index] = Move(clone.Value());
        }

        auto body = Function<void(WorkerTaskContext&)>::TryCreate(
            allocator, [&run, node](WorkerTaskContext& context) noexcept {
                // A는 모든 task가 제출될 때까지 gate에서 기다린다. 그래서 뒤 task들은 의존이 끝나기
                // 전에 기다리는 상태가 되고, Ready는 의존 완료 경로에서 나온다.
                if (node == 0) {
                    WaitGate(run.release);
                }

                ++run.executed[node];
                TraceContextValues values;
                if (TraceCurrentContext(values)) {
                    run.request[node] = values.request;
                }

                if (Graph[node].fails) {
                    context.Fail(ErrorCode::InvalidArgument);
                }
            });
        if (!body) {
            platform::Fatal("Task trace test failed to create a body");
        }

        auto handle = scheduler.Submit(
            Move(body.Value()),
            Span<const WorkerTaskHandle>(dependencies, Graph[node].dependencyCount));
        if (!handle) {
            platform::Fatal("Task trace test failed to submit a task");
        }

        handles[node] = Move(handle.Value());
    }
}

struct TaskEvent {
    uint64 order;
    uint64 task;
    uint64 related;
    uint8 kind;
    StringView error;
};

} // namespace

namespace {

// 스레드 스트림 전체에서 Trace.Task 사건을 모은다.
bool CollectTaskEvents(const TraceStreamDecoder& decoder, Array<TaskEvent>& events) noexcept {
    const DecodedSchema* schema = decoder.FindSchema(StringView(Format::TaskType));
    if (schema == nullptr) {
        return false;
    }

    SystemAllocator scratch;
    bool decoded = true;
    for (const DecodedStream& stream : decoder.Streams()) {
        if (stream.id < TraceFormat::FirstThreadStreamId) {
            continue;
        }

        Array<DecodedRecord> records(scratch);
        decoded = decoded && decoder.DecodeRecords(stream.id, records);
        for (const DecodedRecord& record : records) {
            if (record.typeId != schema->typeId) {
                continue;
            }

            TaskEvent event;
            event.order = record.order;
            event.task = ReadFixed<uint64>(record, Format::TaskIdOffset);
            event.related = ReadFixed<uint64>(record, Format::TaskRelatedOffset);
            event.kind = ReadFixedByte(record, Format::TaskKindOffset);
            event.error = StringView(reinterpret_cast<const char*>(record.variable[0].Data()),
                                     record.variable[0].Size());
            decoded = decoded && record.hasOrder && events.TryPushBack(event);
        }
    }

    return decoded;
}

// kind 사건 가운데 task의 것을 찾는다. 없으면 null.
const TaskEvent* FindEvent(const Array<TaskEvent>& events, uint64 task, uint8 kind) noexcept {
    for (const TaskEvent& event : events) {
        if (event.task == task && event.kind == kind) {
            return &event;
        }
    }

    return nullptr;
}

usize CountEvents(const Array<TaskEvent>& events, uint64 task, uint8 kind) noexcept {
    usize count = 0;
    for (const TaskEvent& event : events) {
        count += event.task == task && event.kind == kind ? 1 : 0;
    }

    return count;
}

// 한 그래프를 실행한다. traced면 task·cpu·context 채널을 켠다. 실행 결과는 run에 남는다.
void RunGraph(Allocator& allocator, TraceSession& session, GraphRun& run, bool traced) noexcept {
    if (traced) {
        CHECK(session.SetChannelEnabled(TraceTaskChannel, true));
        CHECK(session.SetChannelEnabled(TraceCpuChannel, true));
        CHECK(session.SetChannelEnabled(TraceContextChannel, true));
    }

    WorkerScheduler scheduler(allocator);
    WorkerSchedulerConfig config;
    config.workerCount = 2;
    if (!scheduler.Start(config)) {
        platform::Fatal("Task trace test failed to start the scheduler");
    }

    {
        TraceContextScope request(TraceContextValues{0, 0, 0, 0, 42});
        WorkerTaskHandle handles[NodeCount];
        SubmitGraph(allocator, scheduler, run, handles);
        run.release.Signal();
        // 모든 노드를 기다린다(main은 task 밖에서 기다린다).
        for (WorkerTaskHandle& handle : handles) {
            auto waited = handle.Wait(GateTimeoutMs);
            if (!waited || waited.Value() != platform::WaitResult::Signaled) {
                platform::Fatal("Task trace test graph did not finish");
            }
        }

        const WorkerTaskState expected[NodeCount] = {
            WorkerTaskState::Succeeded, WorkerTaskState::Succeeded, WorkerTaskState::Failed,
            WorkerTaskState::Failed,    WorkerTaskState::Succeeded, WorkerTaskState::Succeeded,
            WorkerTaskState::Succeeded, WorkerTaskState::Failed};
        for (usize node = 0; node < NodeCount; ++node) {
            auto status = handles[node].Status();
            CHECK(status && status.Value().state == expected[node]);
        }
    }

    CHECK(scheduler.Shutdown());
}

// 만듦 사건의 순서 번호 순으로 task id를 늘어놓는다(노드 i는 i번째로 만든 task다).
bool OrderCreated(const Array<TaskEvent>& events, uint64 (&ids)[NodeCount]) noexcept {
    uint64 previous = 0;
    for (usize node = 0; node < NodeCount; ++node) {
        const TaskEvent* next = nullptr;
        for (const TaskEvent& event : events) {
            if (event.kind == Format::TaskCreated && (node == 0 || event.order > previous) &&
                (next == nullptr || event.order < next->order)) {
                next = &event;
            }
        }

        if (next == nullptr) {
            return false;
        }

        ids[node] = next->task;
        previous = next->order;
    }

    return true;
}

uint32 FindScopeNameId(const TraceStreamDecoder& decoder, StringView name) noexcept {
    const DecodedSchema* schema = decoder.FindSchema(StringView(Format::ScopeNameType));
    if (schema == nullptr) {
        return 0;
    }

    SystemAllocator scratch;
    uint32 id = 0;
    {
        Array<DecodedRecord> definitions(scratch);
        if (decoder.DecodeRecords(TraceFormat::DefinitionStreamId, definitions)) {
            for (const DecodedRecord& record : definitions) {
                if (record.typeId == schema->typeId &&
                    StringView(reinterpret_cast<const char*>(record.variable[0].Data()),
                               record.variable[0].Size()) == name) {
                    id = ReadFixed<uint32>(record, Format::ScopeNameIdOffset);
                }
            }
        }
    }

    return id;
}

// 모든 스레드에서 이름 id가 nameId인 CPU 구간 시작 수다.
usize CountScopes(const TraceStreamDecoder& decoder, uint32 nameId) noexcept {
    const DecodedSchema* schema = decoder.FindSchema(StringView(Format::CpuScopeBatchType));
    if (schema == nullptr) {
        return 0;
    }

    SystemAllocator scratch;
    usize count = 0;
    for (const DecodedStream& stream : decoder.Streams()) {
        if (stream.id < TraceFormat::FirstThreadStreamId) {
            continue;
        }

        Array<DecodedRecord> records(scratch);
        Array<DecodedScopeEvent> events(scratch);
        if (!decoder.DecodeRecords(stream.id, records)) {
            return 0;
        }

        for (const DecodedRecord& record : records) {
            if (record.typeId == schema->typeId && !DecodeScopeBatch(record.variable[0], events)) {
                return 0;
            }
        }

        for (const DecodedScopeEvent& event : events) {
            count += event.begin && event.nameId == nameId ? 1 : 0;
        }
    }

    return count;
}

} // namespace

// 고정 task 그래프의 기록에서 의존 간선·실행 집합·완료 상태·실행 가능 시점을 복원해 직렬 oracle과
// 맞춘다. 모든 사건은 순서 번호를 받고, 간선 앞 task의 완료가 뒤 task의 시작보다 앞선다. 실행
// 구간은 CPU 구간 "Task.Run"으로 감싸고, 만들 때의 문맥이 실행 스레드에서 복원된다.
TEST(TaskTraceRecordsGraphMatchingSerialOracle) {
    SystemAllocator allocator;
    {
        MemoryOutput output(allocator);
        TraceSession session;
        TraceSessionSettings settings;
        settings.allocator = &allocator;
        settings.writerMode = TraceWriterMode::Manual;
        REQUIRE(session.Start(settings));
        REQUIRE(session.AttachCallbackOutput(&MemoryOutput::Write, &output));
        GraphRun run;
        RunGraph(allocator, session, run, true);
        session.Stop();

        const Oracle oracle;
        usize executed = 0;
        for (usize node = 0; node < NodeCount; ++node) {
            CHECK(run.executed[node] == (oracle.executed[node] ? 1u : 0u));
            if (oracle.executed[node]) {
                CHECK(run.request[node] == 42);
                ++executed;
            }
        }

        TraceStreamDecoder decoder(allocator);
        REQUIRE(decoder.Decode(output.bytes.View()));
        Array<TaskEvent> events(allocator);
        REQUIRE(CollectTaskEvents(decoder, events));
        uint64 ids[NodeCount] = {};
        REQUIRE(OrderCreated(events, ids));

        for (usize node = 0; node < NodeCount; ++node) {
            const uint64 task = ids[node];
            // 의존 간선(앞 → 뒤)이 그래프와 같다.
            usize edges = 0;
            for (const TaskEvent& event : events) {
                if (event.kind == Format::TaskDependencyAdded && event.task == task) {
                    bool known = false;
                    for (usize index = 0; index < Graph[node].dependencyCount; ++index) {
                        known = known || event.related == ids[Graph[node].dependencies[index]];
                    }

                    CHECK(known);
                    ++edges;
                }
            }

            CHECK(edges == Graph[node].dependencyCount);

            // 실행 집합과 완료 상태가 oracle과 같다.
            const TaskEvent* started = FindEvent(events, task, Format::TaskStarted);
            const TaskEvent* finished = FindEvent(events, task, Format::TaskFinished);
            const TaskEvent* completed = FindEvent(events, task, Format::TaskCompleted);
            CHECK((started != nullptr) == oracle.executed[node]);
            CHECK((finished != nullptr) == oracle.executed[node]);
            CHECK(CountEvents(events, task, Format::TaskReady) ==
                  (oracle.executed[node] ? 1u : 0u));
            REQUIRE(completed != nullptr);
            CHECK(completed->error.IsEmpty() == oracle.succeeded[node]);
            if (!oracle.executed[node]) {
                CHECK(completed->error == StringView("DependencyFailed"));
            } else if (Graph[node].fails) {
                CHECK(completed->error == StringView("InvalidArgument"));
            }

            if (started != nullptr) {
                const TaskEvent* ready = FindEvent(events, task, Format::TaskReady);
                REQUIRE(ready != nullptr);
                CHECK(ready->order < started->order);
                CHECK(started->order < finished->order);
                CHECK(finished->order < completed->order);
                for (usize index = 0; index < Graph[node].dependencyCount; ++index) {
                    const TaskEvent* before = FindEvent(
                        events, ids[Graph[node].dependencies[index]], Format::TaskCompleted);
                    REQUIRE(before != nullptr);
                    CHECK(before->order < started->order);
                }
            }

            // main이 기다린 task마다 기다림 시작이 하나 있고, 기다리는 쪽은 task 밖(0)이다.
            usize waits = 0;
            for (const TaskEvent& event : events) {
                if (event.kind == Format::TaskWaitBegin && event.related == task) {
                    CHECK(event.task == 0);
                    ++waits;
                }
            }

            CHECK(waits == 1);
        }

        // 실행 구간 "Task.Run"은 실행한 task 수만큼이다.
        const uint32 runName = FindScopeNameId(decoder, StringView(Format::TaskRunScopeName));
        REQUIRE(runName != 0);
        CHECK(CountScopes(decoder, runName) == executed);
    }
}

// task 채널이 꺼져 있으면 같은 그래프가 같은 결과로 돌고 task 기록은 없다. InlineExecutor도 켠
// 채널에서 만듦부터 완료까지 한 스레드에서 차례로 남긴다.
TEST(TaskTraceChannelOffKeepsBehaviorAndInlineExecutorIsTraced) {
    SystemAllocator allocator;
    {
        MemoryOutput output(allocator);
        TraceSession session;
        TraceSessionSettings settings;
        settings.allocator = &allocator;
        settings.writerMode = TraceWriterMode::Manual;
        REQUIRE(session.Start(settings));
        REQUIRE(session.AttachCallbackOutput(&MemoryOutput::Write, &output));
        GraphRun run;
        RunGraph(allocator, session, run, false);
        const Oracle oracle;
        for (usize node = 0; node < NodeCount; ++node) {
            CHECK(run.executed[node] == (oracle.executed[node] ? 1u : 0u));
        }

        REQUIRE(session.SetChannelEnabled(TraceTaskChannel, true));
        {
            InlineExecutor executor(allocator);
            TaskDesc description;
            auto body = Function<void(TaskContext&)>::TryCreate(
                allocator, [](TaskContext& context) noexcept { context.Fail(ErrorCode::Busy); });
            REQUIRE(body);
            description.body = Move(body.Value());
            auto handle = executor.Submit(Move(description));
            REQUIRE(handle);
        }

        session.Stop();

        TraceStreamDecoder decoder(allocator);
        REQUIRE(decoder.Decode(output.bytes.View()));
        Array<TaskEvent> events(allocator);
        REQUIRE(CollectTaskEvents(decoder, events));
        // 그래프는 채널이 꺼진 채 돌았으므로 InlineExecutor의 task 하나만 있다.
        REQUIRE(events.Size() == 5);
        const uint8 kinds[] = {Format::TaskCreated, Format::TaskReady, Format::TaskStarted,
                               Format::TaskFinished, Format::TaskCompleted};
        for (usize index = 0; index < events.Size(); ++index) {
            CHECK(events[index].kind == kinds[index]);
            CHECK(events[index].task == events[0].task);
            CHECK(index == 0 || events[index].order > events[index - 1].order);
        }

        CHECK(events[4].error == StringView("Busy"));
    }
}
