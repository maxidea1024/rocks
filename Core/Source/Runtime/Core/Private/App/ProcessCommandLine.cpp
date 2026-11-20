#include "CommandLineStorage.h"

#include <Base/Compiler.h>
#include <Core/Assert.h>
#include <Core/FileError.h>
#include <Platform/Atomics.h>
#include <Platform/Process.h>
#include <Platform/ProcessEntry.h>

namespace rocks {

namespace detail {

namespace {

template <typename Character>
inline constexpr bool IsWideCharacter = false;

template <>
inline constexpr bool IsWideCharacter<wchar_t> = true;

ErrorCode EntryArgumentError(platform::FileStatus status) noexcept {
    return status.code == platform::FileCode::Overflow ? ErrorCode::CapacityExceeded
                                                       : FileErrorCode(status);
}

} // namespace

void CommandLineStorage::Clear() noexcept {
    argumentCount_ = 0;
    executableBytes_ = 0;
    executable_[0] = '\0';
}

template <typename Character>
Result<void> CommandLineStorage::FillFrom(int argc, Character** argv) noexcept {
    Clear();
    // 진입점 형태는 플랫폼마다 하나다(설계 D7). 인자가 없어도 맞지 않는 판은 받지 않는다.
    if constexpr (IsWideCharacter<Character> != IsWindows) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    if (argc < 1 || argv == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const usize count = static_cast<usize>(argc) - 1;
    if (count > ProcessCommandLine::MaxArguments) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    usize used = 0;
    for (usize index = 0; index < count; ++index) {
        const Character* argument = argv[index + 1];
        if (argument == nullptr) {
            Clear();
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        usize bytes = 0;
        char* destination = text_ + used;
        const platform::FileStatus converted = platform::ConvertEntryArgument(
            argument, destination, ProcessCommandLine::MaxTotalBytes - used, bytes);
        if (!converted) {
            Clear();
            return Result<void>::Failure(EntryArgumentError(converted));
        }

        const StringView view(destination, bytes);
        if (!view.ValidateUtf8()) {
            Clear();
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        arguments_[index] = view;
        used += bytes + 1;
    }

    usize pathBytes = 0;
    const platform::FileStatus path =
        platform::GetExecutablePath(executable_, sizeof(executable_), pathBytes);
    if (!path) {
        Clear();
        return Result<void>::Failure(EntryArgumentError(path));
    }

    argumentCount_ = count;
    executableBytes_ = pathBytes;
    return Result<void>::Success();
}

Result<void> CommandLineStorage::Fill(int argc, wchar_t** argv) noexcept {
    return FillFrom(argc, argv);
}

Result<void> CommandLineStorage::Fill(int argc, char** argv) noexcept {
    return FillFrom(argc, argv);
}

} // namespace detail

// ProcessCommandLine의 private 칸을 채우는 통로다(이 파일 안에서만 쓴다).
class ProcessCommandLineAccess {
public:
    static void Publish(ProcessCommandLine& commandLine,
                        const detail::CommandLineStorage& storage) noexcept {
        commandLine.executablePath_ = storage.ExecutablePath();
        commandLine.arguments_ = storage.Arguments();
    }
};

namespace {

// 캡처 상태다. Empty → Capturing(한 스레드만) → Captured, 실패하면 Empty로 돌아간다.
enum CaptureState : uint32 {
    Empty = 0,
    Capturing = 1,
    Captured = 2,
};

constinit uint32 captureState = Empty;
detail::CommandLineStorage storage;
ProcessCommandLine captured;

template <typename Character>
Result<void> CaptureFrom(int argc, Character** argv) noexcept {
    uint32 expected = Empty;
    if (!platform::AtomicCompareExchangeStrong<platform::MemoryOrder::Acquire,
                                               platform::MemoryOrder::Relaxed>(
            &captureState, expected, uint32{Capturing})) {
        return Result<void>::Failure(ErrorCode::AlreadyExists);
    }

    const Result<void> filled = storage.Fill(argc, argv);
    if (!filled) {
        platform::AtomicStore<platform::MemoryOrder::Release>(&captureState, uint32{Empty});
        return Result<void>::Failure(filled.Error());
    }

    ProcessCommandLineAccess::Publish(captured, storage);
    platform::AtomicStore<platform::MemoryOrder::Release>(&captureState, uint32{Captured});
    return Result<void>::Success();
}

} // namespace

Result<void> ProcessCommandLine::Capture(int argc, wchar_t** argv) noexcept {
    return CaptureFrom(argc, argv);
}

Result<void> ProcessCommandLine::Capture(int argc, char** argv) noexcept {
    return CaptureFrom(argc, argv);
}

const ProcessCommandLine& ProcessCommandLine::Get() noexcept {
    ROCKS_ASSERT(IsCaptured());
    return captured;
}

bool ProcessCommandLine::IsCaptured() noexcept {
    return platform::AtomicLoad<platform::MemoryOrder::Acquire>(&captureState) == Captured;
}

} // namespace rocks
