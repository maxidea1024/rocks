#include <TestSupport/Test.h>
#include <Core/Commandlet/Commandlet.h>
#include <Platform/Thread.h>

using namespace rocks;

namespace {

struct Capture {
    char output[4096]{};
    char error[4096]{};
    usize outputSize = 0;
    usize errorSize = 0;
    bool fail = false;
};

Result<void> CaptureWrite(void* context, CommandletStream stream, StringView text) noexcept {
    auto& capture = *static_cast<Capture*>(context);
    if (capture.fail) {
        return Result<void>::Failure(ErrorCode::DependencyFailed);
    }
    char* buffer = stream == CommandletStream::Output ? capture.output : capture.error;
    usize& size = stream == CommandletStream::Output ? capture.outputSize : capture.errorSize;
    if (text.ByteCount() > 4095 - size) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }
    for (usize i = 0; i < text.ByteCount(); ++i) {
        buffer[size++] = text.Data()[i];
    }
    buffer[size] = '\0';
    return Result<void>::Success();
}

struct CallbackState {
    int calls = 0;
    bool argsMatch = false;
    int32 code = 0;
    bool fail = false;
    CommandletRegistry* registry = nullptr;
    ErrorCode nestedRun = ErrorCode::None;
    ErrorCode nestedRegister = ErrorCode::None;
};

Result<int32> RunTest(void* context, Span<const StringView> args,
                      CommandletOutput& output) noexcept {
    auto& state = *static_cast<CallbackState*>(context);
    ++state.calls;
    state.argsMatch = args.Size() == 2 && args[0] == StringView("\xED\x95\x9C\xEA\xB8\x80") &&
                      args[1] == StringView("--raw");
    if (state.registry) {
        auto nested = state.registry->Run({}, output);
        state.nestedRun = nested ? ErrorCode::None : nested.Error();
        auto registration = state.registry->Register({"nested", "", &RunTest, context});
        state.nestedRegister = registration ? ErrorCode::None : registration.Error();
    }
    return state.fail ? Result<int32>::Failure(ErrorCode::DependencyFailed)
                      : Result<int32>::Success(state.code);
}

struct ThreadCheck {
    CommandletRegistry* registry;
    CommandletOutput output;
    ErrorCode registration = ErrorCode::None;
    ErrorCode run = ErrorCode::None;
    ErrorCode count = ErrorCode::None;
};

struct ReentrantOutput {
    CommandletRegistry* registry;
    ErrorCode nested = ErrorCode::None;
    bool countSucceeded = false;
};

Result<void> ReenterFromOutput(void* context, CommandletStream, StringView) noexcept {
    auto& state = *static_cast<ReentrantOutput*>(context);
    auto count = state.registry->Count();
    state.countSucceeded = count && count.Value() == 0;
    auto nested = state.registry->Run({}, {&ReenterFromOutput, context});
    state.nested = nested ? ErrorCode::None : nested.Error();
    return Result<void>::Success();
}

void CheckWrongThread(void* context) noexcept {
    auto& state = *static_cast<ThreadCheck*>(context);
    auto registration = state.registry->Register({"foreign", "", &RunTest, nullptr});
    auto run = state.registry->Run({}, state.output);
    auto count = state.registry->Count();
    state.registration = registration ? ErrorCode::None : registration.Error();
    state.run = run ? ErrorCode::None : run.Error();
    state.count = count ? ErrorCode::None : count.Error();
}

} // namespace

TEST(CommandletRegistrationCopiesAndRejectsInvalidDefinitions) {
    CommandletRegistry registry;
    Capture capture;
    CallbackState state;
    char name[] = "probe";
    char description[] = "original";
    REQUIRE(registry.Register({name, description, &RunTest, &state}));
    name[0] = 'x';
    description[0] = 'X';
    auto duplicate = registry.Register({"probe", "", &RunTest, &state});
    CHECK(!duplicate && duplicate.Error() == ErrorCode::AlreadyExists);
    StringView invalidNames[] = {"help", "list", "UPPER", "-name", "", "1name"};
    for (StringView invalid : invalidNames) {
        auto result = registry.Register({invalid, "", &RunTest, &state});
        CHECK(!result && result.Error() == ErrorCode::InvalidArgument);
    }
    auto invalidDescription =
        registry.Register({"valid", StringView("\xC0\xAF", 2), &RunTest, &state});
    CHECK(!invalidDescription && invalidDescription.Error() == ErrorCode::InvalidArgument);
    StringView list[] = {"--list"};
    auto listed = registry.Run({list, 1}, {&CaptureWrite, &capture});
    REQUIRE(listed);
    CHECK(listed.Value() == 0);
    CHECK(StringView(capture.output) == StringView("probe\toriginal\n"));
    auto count = registry.Count();
    REQUIRE(count);
    CHECK(count.Value() == 1);
}

TEST(CommandletArgumentsResultsAndFailureRecovery) {
    CommandletRegistry registry;
    Capture capture;
    CallbackState state;
    state.registry = &registry;
    state.code = 7;
    REQUIRE(registry.Register({"probe", "test", &RunTest, &state}));
    CommandletOutput output{&CaptureWrite, &capture};
    StringView arguments[] = {"probe", "\xED\x95\x9C\xEA\xB8\x80", "--raw"};
    auto run = registry.Run({arguments, 3}, output);
    REQUIRE(run);
    CHECK(run.Value() == 7 && state.argsMatch && state.calls == 1);
    CHECK(state.nestedRun == ErrorCode::Busy && state.nestedRegister == ErrorCode::Busy);
    StringView unknown[] = {"missing"};
    auto missing = registry.Run({unknown, 1}, output);
    REQUIRE(missing);
    CHECK(missing.Value() == 2 && state.calls == 1);
    auto empty = registry.Run({}, output);
    REQUIRE(empty);
    CHECK(empty.Value() == 2);
    StringView badHelp[] = {"--help", "extra"};
    auto help = registry.Run({badHelp, 2}, output);
    REQUIRE(help);
    CHECK(help.Value() == 2);
    capture.fail = true;
    auto outputFailure = registry.Run({unknown, 1}, output);
    CHECK(!outputFailure && outputFailure.Error() == ErrorCode::DependencyFailed);
    capture.fail = false;
    state.fail = true;
    auto callbackFailure = registry.Run({arguments, 3}, output);
    CHECK(!callbackFailure && callbackFailure.Error() == ErrorCode::DependencyFailed);
    state.fail = false;
    state.code = 126;
    auto badCode = registry.Run({arguments, 3}, output);
    CHECK(!badCode && badCode.Error() == ErrorCode::InvalidArgument);
    state.code = 125;
    auto recovered = registry.Run({arguments, 3}, output);
    REQUIRE(recovered);
    CHECK(recovered.Value() == 125);
    REQUIRE(registry.Register({"next", "", &RunTest, &state}));
}

TEST(CommandletRejectsInvalidArgumentsAndWrongThread) {
    CommandletRegistry registry;
    Capture capture;
    CommandletOutput output{&CaptureWrite, &capture};
    StringView invalidUtf8[] = {StringView("\xC0\xAF", 2)};
    auto invalid = registry.Run({invalidUtf8, 1}, output);
    CHECK(!invalid && invalid.Error() == ErrorCode::InvalidArgument);
    const char nul[] = {'a', '\0', 'b'};
    StringView embeddedNul[] = {StringView(nul, 3)};
    auto rejected = registry.Run({embeddedNul, 1}, output);
    CHECK(!rejected && rejected.Error() == ErrorCode::InvalidArgument);
    StringView many[CommandletRegistry::MaxArguments + 1]{};
    auto tooMany = registry.Run({many, CommandletRegistry::MaxArguments + 1}, output);
    CHECK(!tooMany && tooMany.Error() == ErrorCode::InvalidArgument);
    auto missingOutput = registry.Run({}, {});
    CHECK(!missingOutput && missingOutput.Error() == ErrorCode::InvalidArgument);

    // worker가 끝난 뒤에만 결과를 읽고 registry를 파괴한다.
    ThreadCheck state{&registry, output};
    platform::Thread worker;
    REQUIRE(worker.Start(&CheckWrongThread, &state) == platform::ThreadStatus::Success);
    CHECK(worker.Join() == platform::ThreadStatus::Success);
    CHECK(state.registration == ErrorCode::WrongThread);
    CHECK(state.run == ErrorCode::WrongThread);
    CHECK(state.count == ErrorCode::WrongThread);
}

TEST(CommandletCapacityFailureDoesNotChangeRegistry) {
    CommandletRegistry registry;
    CallbackState state;
    char name[] = "a00";
    for (usize index = 0; index < CommandletRegistry::Capacity; ++index) {
        name[1] = static_cast<char>('0' + index / 10);
        name[2] = static_cast<char>('0' + index % 10);
        REQUIRE(registry.Register({name, "", &RunTest, &state}));
    }
    auto full = registry.Register({"overflow", "", &RunTest, &state});
    CHECK(!full && full.Error() == ErrorCode::CapacityExceeded);
    auto count = registry.Count();
    REQUIRE(count);
    CHECK(count.Value() == CommandletRegistry::Capacity);
}

TEST(CommandletOutputReentryAndInputBoundaries) {
    CommandletRegistry registry;
    ReentrantOutput reentry{&registry};
    auto empty = registry.Run({}, {&ReenterFromOutput, &reentry});
    REQUIRE(empty);
    CHECK(empty.Value() == 2 && reentry.nested == ErrorCode::Busy && reentry.countSucceeded);

    CallbackState state;
    char name[65];
    for (usize i = 0; i < 64; ++i)
        name[i] = 'a';
    name[63] = '\0';
    char description[193];
    for (usize i = 0; i < 192; ++i)
        description[i] = 'b';
    description[191] = '\0';
    REQUIRE(registry.Register({name, description, &RunTest, &state}));
    name[63] = 'a';
    name[64] = '\0';
    auto longName = registry.Register({name, "", &RunTest, &state});
    CHECK(!longName && longName.Error() == ErrorCode::InvalidArgument);
    description[191] = 'b';
    description[192] = '\0';
    auto longDescription = registry.Register({"other", description, &RunTest, &state});
    CHECK(!longDescription && longDescription.Error() == ErrorCode::InvalidArgument);

    Capture capture;
    CommandletOutput output{&CaptureWrite, &capture};
    StringView args[CommandletRegistry::MaxArguments]{};
    args[0] = "unknown";
    auto maximumCount = registry.Run({args, CommandletRegistry::MaxArguments}, output);
    REQUIRE(maximumCount);
    CHECK(maximumCount.Value() == 2);
    char argument[CommandletRegistry::MaxArgumentBytes + 2];
    for (usize i = 0; i < CommandletRegistry::MaxArgumentBytes + 1; ++i)
        argument[i] = 'x';
    argument[CommandletRegistry::MaxArgumentBytes] = '\0';
    args[0] = argument;
    auto maximumBytes = registry.Run({args, 1}, output);
    REQUIRE(maximumBytes);
    CHECK(maximumBytes.Value() == 2);
    argument[CommandletRegistry::MaxArgumentBytes] = 'x';
    argument[CommandletRegistry::MaxArgumentBytes + 1] = '\0';
    args[0] = argument;
    auto oversized = registry.Run({args, 1}, output);
    CHECK(!oversized && oversized.Error() == ErrorCode::InvalidArgument);
}
