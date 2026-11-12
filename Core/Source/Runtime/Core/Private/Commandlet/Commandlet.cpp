#include <Core/Commandlet/Commandlet.h>
#include <Platform/Clock.h>

namespace rocks {
namespace {

bool IsValidText(StringView text, usize limit) noexcept {
    if (text.ByteCount() > limit || !text.ValidateUtf8()) {
        return false;
    }
    for (usize index = 0; index < text.ByteCount(); ++index) {
        if (text.Data()[index] == '\0') {
            return false;
        }
    }
    return true;
}

bool IsValidName(StringView name) noexcept {
    if (name.IsEmpty() || name.ByteCount() > CommandletRegistry::NameCapacity) {
        return false;
    }
    for (usize index = 0; index < name.ByteCount(); ++index) {
        const char value = name.Data()[index];
        if (!((value >= 'a' && value <= 'z') || (index != 0 && value >= '0' && value <= '9') ||
              (index != 0 && value == '-'))) {
            return false;
        }
    }
    return true;
}

void CopyText(char* destination, StringView source) noexcept {
    for (usize index = 0; index < source.ByteCount(); ++index) {
        destination[index] = source.Data()[index];
    }
    destination[source.ByteCount()] = '\0';
}

Result<int32> Emit(CommandletOutput& output, CommandletStream stream, StringView text,
                   int32 code) noexcept {
    auto written = output.write(output.context, stream, text);
    return written ? Result<int32>::Success(code) : Result<int32>::Failure(written.Error());
}

struct RunningGuard {
    bool& running;
    ~RunningGuard() noexcept {
        running = false;
    }
};

} // namespace

CommandletRegistry::CommandletRegistry() noexcept : owner_(platform::CurrentThreadId()) {
}

Result<void> CommandletRegistry::Register(const CommandletDefinition& definition) noexcept {
    if (platform::CurrentThreadId() != owner_) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }
    if (running_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }
    if (!IsValidName(definition.name) || definition.name == StringView("help") ||
        definition.name == StringView("list") ||
        !IsValidText(definition.description, DescriptionCapacity) ||
        definition.execute == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    for (usize index = 0; index < count_; ++index) {
        if (StringView(entries_[index].name, entries_[index].nameBytes) == definition.name) {
            return Result<void>::Failure(ErrorCode::AlreadyExists);
        }
    }
    if (count_ == Capacity) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }
    Entry& entry = entries_[count_];
    CopyText(entry.name, definition.name);
    CopyText(entry.description, definition.description);
    entry.nameBytes = definition.name.ByteCount();
    entry.descriptionBytes = definition.description.ByteCount();
    entry.execute = definition.execute;
    entry.context = definition.context;
    ++count_;
    return Result<void>::Success();
}

Result<usize> CommandletRegistry::Count() const noexcept {
    if (platform::CurrentThreadId() != owner_) {
        return Result<usize>::Failure(ErrorCode::WrongThread);
    }
    return Result<usize>::Success(count_);
}

Result<int32> CommandletRegistry::Run(Span<const StringView> args,
                                      CommandletOutput output) noexcept {
    if (platform::CurrentThreadId() != owner_) {
        return Result<int32>::Failure(ErrorCode::WrongThread);
    }
    if (running_) {
        return Result<int32>::Failure(ErrorCode::Busy);
    }
    if (!output.write || args.Size() > MaxArguments) {
        return Result<int32>::Failure(ErrorCode::InvalidArgument);
    }
    for (usize index = 0; index < args.Size(); ++index) {
        if (!IsValidText(args[index], MaxArgumentBytes)) {
            return Result<int32>::Failure(ErrorCode::InvalidArgument);
        }
    }
    // 출력 callback도 재진입할 수 있으므로 도움말과 오류 경로도 실행 상태로 묶는다.
    running_ = true;
    RunningGuard guard{running_};
    if (args.Size() == 0) {
        return Emit(output, CommandletStream::Error,
                    "Usage: rocks-commandlet --help | --list | <command> [args...]\n", 2);
    }
    if (args[0] == StringView("--help") || args[0] == StringView("--list")) {
        if (args.Size() != 1) {
            return Emit(output, CommandletStream::Error, "Help/list accepts no arguments.\n", 2);
        }
        if (args[0] == StringView("--help")) {
            auto result =
                output.write(output.context, CommandletStream::Output,
                             "Usage: rocks-commandlet --help | --list | <command> [args...]\n");
            if (!result) {
                return Result<int32>::Failure(result.Error());
            }
        }
        for (usize index = 0; index < count_; ++index) {
            const Entry& entry = entries_[index];
            auto result = output.write(output.context, CommandletStream::Output,
                                       StringView(entry.name, entry.nameBytes));
            if (!result)
                return Result<int32>::Failure(result.Error());
            result = output.write(output.context, CommandletStream::Output, "\t");
            if (!result)
                return Result<int32>::Failure(result.Error());
            result = output.write(output.context, CommandletStream::Output,
                                  StringView(entry.description, entry.descriptionBytes));
            if (!result)
                return Result<int32>::Failure(result.Error());
            result = output.write(output.context, CommandletStream::Output, "\n");
            if (!result)
                return Result<int32>::Failure(result.Error());
        }
        return Result<int32>::Success(0);
    }
    for (usize index = 0; index < count_; ++index) {
        Entry& entry = entries_[index];
        if (StringView(entry.name, entry.nameBytes) != args[0])
            continue;
        Span<const StringView> commandArgs(args.Data() + 1, args.Size() - 1);
        auto result = entry.execute(entry.context, commandArgs, output);
        if (!result)
            return result;
        if (result.Value() < 0 || result.Value() > 125) {
            return Result<int32>::Failure(ErrorCode::InvalidArgument);
        }
        return result;
    }
    return Emit(output, CommandletStream::Error, "Unknown command. Use --list.\n", 2);
}

} // namespace rocks
