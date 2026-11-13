#include <TestSupport/ProcessProbe.h>

// UTF-8·UTF-16 변환은 PlatformBase Win64 backend의 것을 그대로 쓴다(시험 도구에 따로 두지 않는다).
#include <Core/Source/Runtime/PlatformBase/Private/Win64/WindowsString.h>

#include <Windows.h>
#include <shellapi.h>

namespace rocks::test {

bool ProcessSpawnExpected() noexcept {
    return true;
}

usize ReadUtf8Arguments(int, char**, char* buffer, usize capacity, const char** arguments,
                        usize maxArguments) noexcept {
    int count = 0;
    LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &count);
    if (wide == nullptr || count < 0 || static_cast<usize>(count) > maxArguments) {
        LocalFree(wide);
        return 0;
    }

    usize used = 0;
    for (int i = 0; i < count; ++i) {
        usize written = 0;
        if (!platform::detail::WideToUtf8(wide[i], lstrlenW(wide[i]), buffer + used,
                                          capacity - used, written)) {
            LocalFree(wide);
            return 0;
        }

        arguments[i] = buffer + used;
        used += written + 1u;
    }

    LocalFree(wide);
    return static_cast<usize>(count);
}

bool WriteStandardStream(int stream, const void* data, usize bytes) noexcept {
    const HANDLE handle = GetStdHandle(stream == 2 ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    const auto* source = static_cast<const uint8*>(data);
    while (bytes != 0) {
        const DWORD chunk = bytes > 65536u ? 65536u : static_cast<DWORD>(bytes);
        DWORD written = 0;
        if (!WriteFile(handle, source, chunk, &written, nullptr) || written == 0) {
            return false;
        }

        source += written;
        bytes -= written;
    }

    return true;
}

int64 ReadStandardInput(void* buffer, usize capacity) noexcept {
    const HANDLE handle = GetStdHandle(STD_INPUT_HANDLE);
    const DWORD chunk = capacity > 65536u ? 65536u : static_cast<DWORD>(capacity);
    DWORD read = 0;
    if (!ReadFile(handle, buffer, chunk, &read, nullptr)) {
        return GetLastError() == ERROR_BROKEN_PIPE ? 0 : -1;
    }

    return read;
}

uint32 SpawnSleepingDescendant() noexcept {
    platform::detail::WideString path;
    if (!path.AssignModuleFileName()) {
        return 0;
    }

    static const wchar_t suffix[] = L"\" --process-probe sleep 60000";
    constexpr usize suffixUnits = sizeof(suffix) / sizeof(wchar_t) - 1u;
    const usize units = 1u + path.Units() + suffixUnits;
    auto* commandLine = static_cast<wchar_t*>(
        platform::AlignedAllocate((units + 1u) * sizeof(wchar_t), alignof(wchar_t)));
    if (commandLine == nullptr) {
        return 0;
    }

    commandLine[0] = L'"';
    platform::MemoryCopy(commandLine + 1, path.Value(), path.Units() * sizeof(wchar_t));
    platform::MemoryCopy(commandLine + 1 + path.Units(), suffix, suffixUnits * sizeof(wchar_t));
    commandLine[units] = L'\0';
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION created{};
    // job을 벗어나지 않고(CREATE_BREAKAWAY_FROM_JOB 없음) 상속 가능한 표준 handle을 물려준다.
    // 손자가 표준 출력 pipe를 잡고 있게 된다.
    const BOOL started = CreateProcessW(path.Value(), commandLine, nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &created);
    platform::AlignedFree(commandLine);
    if (!started) {
        return 0;
    }

    CloseHandle(created.hThread);
    CloseHandle(created.hProcess);
    return created.dwProcessId;
}

ProcessWatch::ProcessWatch(uint32 processId) noexcept : processId_(processId) {
    // 살아 있는 동안 handle을 열어 두면 그 PID는 재사용되지 않는다.
    handle_ = reinterpret_cast<uptr>(OpenProcess(SYNCHRONIZE, FALSE, processId));
}

ProcessWatch::~ProcessWatch() noexcept {
    if (handle_ != 0) {
        CloseHandle(reinterpret_cast<HANDLE>(handle_));
    }
}

bool ProcessWatch::WaitGone(uint32 milliseconds) noexcept {
    if (handle_ == 0) {
        // 열 수 없었다. 이미 끝났거나 확인할 권한이 없다.
        return GetLastError() != ERROR_ACCESS_DENIED || processId_ == 0;
    }

    return WaitForSingleObject(reinterpret_cast<HANDLE>(handle_), milliseconds) == WAIT_OBJECT_0;
}

bool MakeHardLink(const char* existing, const char* link) noexcept {
    platform::detail::WideString from;
    platform::detail::WideString to;
    usize existingBytes = 0;
    while (existing[existingBytes] != '\0') {
        ++existingBytes;
    }

    usize linkBytes = 0;
    while (link[linkBytes] != '\0') {
        ++linkBytes;
    }

    return from.AssignUtf8(existing, existingBytes) && to.AssignUtf8(link, linkBytes) &&
           CreateHardLinkW(to.Value(), from.Value(), nullptr) != 0;
}

int32 CurrentPriorityRank() noexcept {
    switch (GetPriorityClass(GetCurrentProcess())) {
    case IDLE_PRIORITY_CLASS:
        return 19;
    case BELOW_NORMAL_PRIORITY_CLASS:
        return 5;
    case ABOVE_NORMAL_PRIORITY_CLASS:
        return -5;
    case HIGH_PRIORITY_CLASS:
        return -10;
    case REALTIME_PRIORITY_CLASS:
        return -20;
    default:
        return 0;
    }
}

bool OpenInheritableFile(const char* path, uptr& handle, uint64& identity) noexcept {
    platform::detail::WideString wide;
    usize bytes = 0;
    while (path[bytes] != '\0') {
        ++bytes;
    }

    if (!wide.AssignUtf8(path, bytes)) {
        return false;
    }

    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    const HANDLE file = CreateFileW(wide.Value(), GENERIC_READ | GENERIC_WRITE, 0, &attributes,
                                    CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(file, &information)) {
        CloseHandle(file);
        return false;
    }

    handle = reinterpret_cast<uptr>(file);
    identity = (static_cast<uint64>(information.nFileIndexHigh) << 32) | information.nFileIndexLow;
    return true;
}

void CloseInheritableFile(uptr handle) noexcept {
    CloseHandle(reinterpret_cast<HANDLE>(handle));
}

bool HandleRefersToFile(uptr handle, uint64 identity) noexcept {
    DWORD flags = 0;
    const auto native = reinterpret_cast<HANDLE>(handle);
    if (!GetHandleInformation(native, &flags) || GetFileType(native) != FILE_TYPE_DISK) {
        return false;
    }

    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(native, &information)) {
        return false;
    }

    return ((static_cast<uint64>(information.nFileIndexHigh) << 32) | information.nFileIndexLow) ==
           identity;
}

bool RaiseTerminateSignal() noexcept {
    return false;
}

} // namespace rocks::test
