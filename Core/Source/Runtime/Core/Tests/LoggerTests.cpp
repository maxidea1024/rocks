#include <TestSupport/Test.h>
#include <Core/Diagnostics/Logger.h>
#include <Platform/Fatal.h>
#include <Platform/Thread.h>

using namespace rocks;

namespace {
struct CaptureSink final : RuntimeLogSink {
    uint32 count = 0;
    uint32 lastCategory = 0;
    LogLevel lastLevel = LogLevel::Trace;
    uint64 lastThread = 0;
    char bytes[4]{};
    usize byteCount = 0;
    bool fail = false;
    Logger* reenter = nullptr;
    ErrorCode nestedWrite = ErrorCode::None;
    ErrorCode nestedRemove = ErrorCode::None;
    ErrorCode nestedAdd = ErrorCode::None;
    ErrorCode nestedFilter = ErrorCode::None;
    uint32* order = nullptr;
    uint32* orderCount = nullptr;
    uint32 id = 0;

    Result<void> Write(const LogMessage& message) noexcept override {
        ++count;
        if (order != nullptr && orderCount != nullptr) {
            order[(*orderCount)++] = id;
        }
        lastCategory = message.category;
        lastLevel = message.level;
        lastThread = message.thread;
        byteCount = message.message.ByteCount();
        for (usize index = 0; index < byteCount && index < sizeof(bytes); ++index) {
            bytes[index] = message.message.Data()[index];
        }
        if (reenter != nullptr) {
            nestedWrite = reenter->Write(LogLevel::Info, 0, "nested").Error();
            nestedRemove = reenter->Remove(*this).Error();
            nestedAdd = reenter->Add(*this).Error();
            nestedFilter = reenter->SetFilter(*this, {}).Error();
        }
        return fail ? Result<void>::Failure(ErrorCode::OutOfMemory) : Result<void>::Success();
    }
};

struct TextCapture {
    char bytes[128]{};
    usize count = 0;
    uint32 writes = 0;
    uint32 flushes = 0;
    uint32 failWrite = 0;
    bool failFlush = false;

    static Result<void> Write(void* raw, StringView view) noexcept {
        auto& capture = *static_cast<TextCapture*>(raw);
        ++capture.writes;
        if (capture.writes == capture.failWrite ||
            view.ByteCount() > sizeof(capture.bytes) - capture.count) {
            return Result<void>::Failure(ErrorCode::DependencyFailed);
        }
        for (usize index = 0; index < view.ByteCount(); ++index) {
            capture.bytes[capture.count++] = view.Data()[index];
        }
        return Result<void>::Success();
    }

    static Result<void> Flush(void* raw) noexcept {
        auto& capture = *static_cast<TextCapture*>(raw);
        ++capture.flushes;
        return capture.failFlush ? Result<void>::Failure(ErrorCode::DependencyFailed)
                                 : Result<void>::Success();
    }
};

void Join(platform::Thread& thread) noexcept {
    if (thread.WaitForCompletion(10000) != platform::WaitResult::Signaled ||
        thread.Join() != platform::ThreadStatus::Success) {
        platform::Fatal("Logger test worker did not finish");
    }
}

struct WriterContext {
    Logger* logger;
    uint32 failures = 0;
};

void WriteMany(void* raw) noexcept {
    auto& context = *static_cast<WriterContext*>(raw);
    for (uint32 index = 0; index < 400; ++index) {
        if (!context.logger->Write(LogLevel::Info, 42, "worker").IsOk()) {
            ++context.failures;
        }
    }
}

struct BlockingSink final : RuntimeLogSink {
    platform::ManualResetEvent entered;
    platform::ManualResetEvent release;
    Result<void> Write(const LogMessage&) noexcept override {
        entered.Signal();
        if (release.Wait(10000) != platform::WaitResult::Signaled) {
            platform::Fatal("Logger test callback did not release");
        }
        return Result<void>::Success();
    }
};

struct RemovalContext {
    RemovalContext(Logger& owner, BlockingSink& target) noexcept : logger(&owner), sink(&target) {
    }

    Logger* logger;
    BlockingSink* sink;
    platform::ManualResetEvent started;
    platform::ManualResetEvent finished;
    bool removed = false;
};

void WriteBlocking(void* raw) noexcept {
    auto& context = *static_cast<RemovalContext*>(raw);
    CHECK(context.logger->Write(LogLevel::Info, 0, "blocking").IsOk());
}

void RemoveBlocking(void* raw) noexcept {
    auto& context = *static_cast<RemovalContext*>(raw);
    context.started.Signal();
    context.removed = context.logger->Remove(*context.sink).IsOk();
    context.finished.Signal();
}
} // namespace

TEST(RuntimeLoggerFiltersFanoutAndOwnedBytes) {
    Logger logger;
    CaptureSink first;
    CaptureSink second;
    REQUIRE(logger.Add(first).IsOk());
    REQUIRE(logger.Add(second, {LogLevel::Warning, true, 7}).IsOk());
    CHECK(logger.Add(first).Error() == ErrorCode::AlreadyExists);
    CHECK(logger.Write(LogLevel::Info, 7, "info").IsOk());
    CHECK(first.count == 1 && second.count == 0);
    const char embedded[] = {'A', '\0', 'B'};
    REQUIRE(logger.Write(LogLevel::Warning, 7, StringView(embedded, sizeof(embedded))).IsOk());
    CHECK(first.count == 2 && second.count == 1 && second.byteCount == 3);
    CHECK(second.bytes[0] == 'A' && second.bytes[1] == '\0' && second.bytes[2] == 'B');
    CHECK(second.lastCategory == 7 && second.lastLevel == LogLevel::Warning &&
          second.lastThread != 0);
    CHECK(logger.Write(LogLevel::Error, 8, "other").IsOk());
    CHECK(second.count == 1);
    REQUIRE(logger.SetFilter(second, {LogLevel::Error, false, 0}).IsOk());
    CHECK(logger.Write(LogLevel::Warning, 7, "filtered").IsOk());
    CHECK(logger.Write(LogLevel::Error, 8, "accepted").IsOk());
    CHECK(second.count == 2);
    CHECK(logger.Remove(second).IsOk());
    CHECK(logger.Remove(second).Error() == ErrorCode::NotFound);
    CHECK(logger.SetFilter(second, {}).Error() == ErrorCode::NotFound);
    CHECK(logger.Remove(first).IsOk());
    CHECK(logger.Write(LogLevel::Info, 0, "no sinks").IsOk());
}

TEST(RuntimeLoggerRejectsInvalidInputAndReportsSinkFailure) {
    Logger logger;
    CaptureSink failing;
    CaptureSink succeeding;
    CaptureSink invalidFilter;
    failing.fail = true;
    uint32 order[2]{};
    uint32 orderCount = 0;
    failing.order = succeeding.order = order;
    failing.orderCount = succeeding.orderCount = &orderCount;
    failing.id = 1;
    succeeding.id = 2;
    CHECK(logger.Add(invalidFilter, {static_cast<LogLevel>(255), false, 0}).Error() ==
          ErrorCode::InvalidArgument);
    REQUIRE(logger.Add(failing).IsOk());
    REQUIRE(logger.Add(succeeding).IsOk());
    const char invalid[] = {static_cast<char>(0xff)};
    CHECK(logger.Write(LogLevel::Info, 0, StringView(invalid, 1)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(logger.Write(static_cast<LogLevel>(255), 0, "invalid").Error() ==
          ErrorCode::InvalidArgument);
    CHECK(logger.SetFilter(succeeding, {static_cast<LogLevel>(255), false, 0}).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(failing.count == 0 && succeeding.count == 0);
    CHECK(logger.Write(LogLevel::Error, 0, "failure").Error() == ErrorCode::DependencyFailed);
    CHECK(failing.count == 1 && succeeding.count == 1);
    CHECK(orderCount == 2 && order[0] == 1 && order[1] == 2);
    CHECK(logger.Remove(failing).IsOk());
    CHECK(logger.Remove(succeeding).IsOk());
}

TEST(RuntimeLoggerCapacityAndReentry) {
    Logger logger;
    Logger another;
    CaptureSink sinks[Logger::MaxSinks + 1];
    for (usize index = 0; index < Logger::MaxSinks; ++index) {
        REQUIRE(logger.Add(sinks[index]).IsOk());
    }
    CHECK(logger.Add(sinks[Logger::MaxSinks]).Error() == ErrorCode::CapacityExceeded);
    sinks[0].reenter = &logger;
    CHECK(logger.Write(LogLevel::Info, 0, "outer").IsOk());
    CHECK(sinks[0].nestedWrite == ErrorCode::Busy && sinks[0].nestedRemove == ErrorCode::Busy &&
          sinks[0].nestedAdd == ErrorCode::Busy && sinks[0].nestedFilter == ErrorCode::Busy);
    sinks[0].reenter = &another;
    CHECK(logger.Write(LogLevel::Info, 0, "other logger").IsOk());
    CHECK(sinks[0].nestedWrite == ErrorCode::Busy && sinks[0].nestedRemove == ErrorCode::Busy &&
          sinks[0].nestedAdd == ErrorCode::Busy && sinks[0].nestedFilter == ErrorCode::Busy);
    for (usize index = 0; index < Logger::MaxSinks; ++index) {
        CHECK(logger.Remove(sinks[index]).IsOk());
    }
    CHECK(logger.Add(sinks[Logger::MaxSinks]).IsOk());
    CHECK(logger.Remove(sinks[Logger::MaxSinks]).IsOk());
}

TEST(RuntimeLoggerTextOutputAndFailures) {
    TextCapture output;
    TextLogSink sink({&output, TextCapture::Write, TextCapture::Flush});
    Logger logger;
    REQUIRE(logger.Add(sink).IsOk());
    const char message[] = {static_cast<char>(0xed), static_cast<char>(0x95),
                            static_cast<char>(0x9c), '\0', 'X'};
    REQUIRE(logger.Write(LogLevel::Warning, 27, StringView(message, sizeof(message))).IsOk());
    const char expected[] = {'[',
                             'W',
                             'a',
                             'r',
                             'n',
                             'i',
                             'n',
                             'g',
                             ']',
                             '[',
                             '2',
                             '7',
                             ']',
                             ' ',
                             static_cast<char>(0xed),
                             static_cast<char>(0x95),
                             static_cast<char>(0x9c),
                             '\0',
                             'X',
                             '\n'};
    REQUIRE(output.count == sizeof(expected));
    for (usize index = 0; index < sizeof(expected); ++index) {
        CHECK(output.bytes[index] == expected[index]);
    }
    CHECK(output.writes == 3 && output.flushes == 1);

    output = {};
    output.failWrite = 2;
    CHECK(logger.Write(LogLevel::Info, 0, "x").Error() == ErrorCode::DependencyFailed);
    CHECK(output.writes == 2 && output.flushes == 0);
    output = {};
    output.failFlush = true;
    CHECK(logger.Write(LogLevel::Info, 0, "x").Error() == ErrorCode::DependencyFailed);
    CHECK(output.writes == 3 && output.flushes == 1);
    CHECK(logger.Remove(sink).IsOk());
}

// 외부 프로세스가 stderr 원시 byte를 캡처하여 한글과 NUL까지 확인할 수 있는 전용 probe다.
TEST(RuntimeLoggerStderrOutputProbe) {
    Logger logger;
    StderrLogSink stderrSink;
    REQUIRE(logger.Add(stderrSink).IsOk());
    const char message[] = {static_cast<char>(0xed), static_cast<char>(0x95),
                            static_cast<char>(0x9c), '\0', 'X'};
    CHECK(logger.Write(LogLevel::Warning, 27, StringView(message, sizeof(message))).IsOk());
    CHECK(logger.Remove(stderrSink).IsOk());
}

TEST(RuntimeLoggerConcurrentWritesAndRemovalWaitsForCallback) {
    Logger logger;
    CaptureSink capture;
    REQUIRE(logger.Add(capture).IsOk());
    platform::Thread workers[4];
    WriterContext contexts[4] = {{&logger}, {&logger}, {&logger}, {&logger}};
    for (usize index = 0; index < 4; ++index) {
        if (workers[index].Start(WriteMany, &contexts[index]) != platform::ThreadStatus::Success) {
            platform::Fatal("Logger test worker start failed");
        }
    }
    for (usize index = 0; index < 4; ++index) {
        Join(workers[index]);
        CHECK(contexts[index].failures == 0);
    }
    CHECK(capture.count == 1600);
    REQUIRE(logger.Remove(capture).IsOk());

    BlockingSink blocking;
    RemovalContext context(logger, blocking);
    REQUIRE(logger.Add(blocking).IsOk());
    platform::Thread writer;
    platform::Thread remover;
    if (writer.Start(WriteBlocking, &context) != platform::ThreadStatus::Success ||
        blocking.entered.Wait(10000) != platform::WaitResult::Signaled ||
        remover.Start(RemoveBlocking, &context) != platform::ThreadStatus::Success ||
        context.started.Wait(10000) != platform::WaitResult::Signaled) {
        platform::Fatal("Logger test setup failed");
    }
    CHECK(context.finished.Wait(20) == platform::WaitResult::TimedOut);
    blocking.release.Signal();
    Join(writer);
    Join(remover);
    CHECK(context.removed && context.finished.Wait(0) == platform::WaitResult::Signaled);
    CHECK(logger.Write(LogLevel::Info, 0, "after removal").IsOk());
}
