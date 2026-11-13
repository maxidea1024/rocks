#include "../Common/ProcessInternal.h"
#include "WindowsError.h"
#include "WindowsString.h"

#include <Platform/Atomics.h>
#include <Platform/Clock.h>
#include <Platform/Memory.h>
#include <Platform/SecureRandom.h>

#include <Windows.h>
#include <new>

namespace rocks::platform {

namespace {

// 부모 쪽 pipe 버퍼와 한 번에 읽는 크기다. 출력이 많은 도구도 pipe가 차기 전에 부모가 비운다.
constexpr DWORD PipeBufferBytes = 64u * 1024u;
constexpr DWORD ReadChunkBytes = 16u * 1024u;
constexpr DWORD WriteChunkBytes = 16u * 1024u;

// 입력 pipe가 가득 찼을 때 다시 쓰기를 시도하는 최대 간격이다(비차단 handle은 기다릴 이벤트가
// 없다).
constexpr DWORD InputRetryMilliseconds = 10;

// CreateProcessW 명령줄의 최대 길이(UTF-16 단위, NUL 포함)다.
constexpr usize MaxCommandLineUnits = 32767;

// Terminate가 job에 주는 종료 코드다.
constexpr UINT TerminateExitCode = 1;

// Close가 강제 종료한 자식의 끝남을 기다리는 상한이다. 종료 중 교착한 프로세스(ExitProcess의 DLL
// 분리에서 멈춘 경우 등)는 TerminateProcess로도 끝나지 않으므로 무한히 기다리지 않는다.
constexpr DWORD CloseWaitMilliseconds = 10'000;

FileStatus LastError() noexcept {
    return detail::NativeError(GetLastError());
}

// 크기가 늘어나는 byte 버퍼다(명령줄 조립용).
class ByteBuffer {
public:
    ByteBuffer() noexcept = default;
    ~ByteBuffer() noexcept {
        AlignedFree(data_);
    }

    ByteBuffer(const ByteBuffer&) = delete;
    ByteBuffer& operator=(const ByteBuffer&) = delete;

    bool Append(const char* text, usize bytes) noexcept {
        if (!Reserve(bytes)) {
            return false;
        }

        MemoryCopy(data_ + size_, text, bytes);
        size_ += bytes;
        return true;
    }

    bool Append(char value) noexcept {
        return Append(&value, 1);
    }

    bool AppendRepeated(char value, usize count) noexcept {
        if (!Reserve(count)) {
            return false;
        }

        MemorySet(data_ + size_, static_cast<uint8>(value), count);
        size_ += count;
        return true;
    }

    const char* Data() const noexcept {
        return data_;
    }

    usize Size() const noexcept {
        return size_;
    }

private:
    bool Reserve(usize extra) noexcept {
        if (extra <= capacity_ - size_) {
            return true;
        }

        if (extra > MaxSize - size_) {
            return false;
        }

        usize next = capacity_ == 0 ? 256u : capacity_;
        while (next - size_ < extra) {
            if (next > MaxSize / 2) {
                next = size_ + extra;
                break;
            }

            next *= 2;
        }

        auto* grown = static_cast<char*>(AlignedAllocate(next, 1));
        if (grown == nullptr) {
            return false;
        }

        MemoryCopy(grown, data_, size_);
        AlignedFree(data_);
        data_ = grown;
        capacity_ = next;
        return true;
    }

    char* data_ = nullptr;
    usize size_ = 0;
    usize capacity_ = 0;
};

// MSVC CRT·CommandLineToArgvW가 원래 인자로 되돌리도록 인자 하나를 인용해 붙인다. 공백·탭·줄바꿈·
// 따옴표가 없고 비어 있지 않으면 그대로 둔다. 따옴표 앞 역슬래시는 두 배로, 끝 역슬래시도 닫는
// 따옴표 앞이므로 두 배로 늘린다. 특수 문자는 모두 ASCII라 UTF-8 byte 단위로 처리해도 다중 byte
// 문자를 건드리지 않는다.
bool AppendQuotedArgument(ByteBuffer& line, const char* text, usize bytes) noexcept {
    bool needsQuotes = bytes == 0;
    for (usize i = 0; i < bytes && !needsQuotes; ++i) {
        const char value = text[i];
        needsQuotes =
            value == ' ' || value == '\t' || value == '\n' || value == '\v' || value == '"';
    }

    if (!needsQuotes) {
        return line.Append(text, bytes);
    }

    if (!line.Append('"')) {
        return false;
    }

    usize i = 0;
    for (;;) {
        usize backslashes = 0;
        while (i < bytes && text[i] == '\\') {
            ++backslashes;
            ++i;
        }

        if (i == bytes) {
            if (!line.AppendRepeated('\\', backslashes * 2u)) {
                return false;
            }

            break;
        }

        if (text[i] == '"') {
            if (!line.AppendRepeated('\\', backslashes * 2u + 1u) || !line.Append('"')) {
                return false;
            }
        } else if (!line.AppendRepeated('\\', backslashes) || !line.Append(text[i])) {
            return false;
        }

        ++i;
    }

    return line.Append('"');
}

// argv[0]은 CRT가 역슬래시를 해석하지 않고 따옴표로만 구분하므로 항상 따옴표로 감싼다(경로에 '"'는
// 없다). 나머지 인자는 AppendQuotedArgument 규칙이다.
FileStatus BuildCommandLine(const ProcessStartInfo& info,
                            detail::WideString& commandLine) noexcept {
    ByteBuffer line;
    bool appended =
        line.Append('"') && line.Append(info.executable, info.executableBytes) && line.Append('"');
    for (usize i = 0; appended && i < info.argumentCount; ++i) {
        appended = line.Append(' ') &&
                   AppendQuotedArgument(line, info.arguments[i].text, info.arguments[i].bytes);
    }

    if (!appended) {
        return {FileCode::OutOfMemory, 0};
    }

    auto status = commandLine.AssignUtf8(line.Data(), line.Size());
    if (!status) {
        return status;
    }

    if (commandLine.Units() + 1u > MaxCommandLineUnits) {
        return {FileCode::Overflow, 0};
    }

    return {};
}

void CloseHandleIfValid(HANDLE& handle) noexcept {
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
    }

    handle = nullptr;
}

// 자식에게 넘길 handle은 상속 가능해야 한다(실제로 넘기는 것은 handle 목록으로 제한한다).
SECURITY_ATTRIBUTES InheritableAttributes() noexcept {
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    return attributes;
}

FileStatus OpenNullDevice(HANDLE& child) noexcept {
    SECURITY_ATTRIBUTES attributes = InheritableAttributes();
    child = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        &attributes, OPEN_EXISTING, 0, nullptr);
    if (child == INVALID_HANDLE_VALUE) {
        child = nullptr;
        return LastError();
    }

    return {};
}

// 부모의 표준 handle을 상속 가능한 복제로 만든다. 부모에 해당 스트림이 없으면 NUL이다. GUI
// 프로세스는 실제로 물려받지 않은 STARTUPINFO의 handle 값을 갖고 있을 수 있으므로, 종류를 알 수
// 없는 handle(GetFileType 실패)과 복제할 수 없는 handle도 없는 것으로 본다.
FileStatus DuplicateStandardHandle(DWORD which, HANDLE& child) noexcept {
    const HANDLE source = GetStdHandle(which);
    if (source == nullptr || source == INVALID_HANDLE_VALUE) {
        return OpenNullDevice(child);
    }

    SetLastError(NO_ERROR);
    if (GetFileType(source) == FILE_TYPE_UNKNOWN && GetLastError() != NO_ERROR) {
        return OpenNullDevice(child);
    }

    if (!DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(), &child, 0, TRUE,
                         DUPLICATE_SAME_ACCESS)) {
        child = nullptr;
        if (GetLastError() == ERROR_INVALID_HANDLE) {
            return OpenNullDevice(child);
        }

        return LastError();
    }

    return {};
}

// 실행 파일이 batch(.bat·.cmd)인지다. CreateProcessW는 batch를 cmd.exe /c로 실행하고, cmd.exe는
// MSVC 규칙으로 인용한 인자 안의 &·|·%VAR%를 다시 해석한다(인자 주입, CVE-2024-24576 계열).
bool IsBatchFile(const char* path, usize bytes) noexcept {
    if (bytes < 4 || path[bytes - 4] != '.') {
        return false;
    }

    char extension[3];
    for (usize i = 0; i < 3; ++i) {
        const char value = path[bytes - 3 + i];
        extension[i] = value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
    }

    return (extension[0] == 'b' && extension[1] == 'a' && extension[2] == 't') ||
           (extension[0] == 'c' && extension[1] == 'm' && extension[2] == 'd');
}

void AppendDecimal(wchar_t* buffer, usize& length, uint64 value) noexcept {
    wchar_t digits[20];
    usize count = 0;
    do {
        digits[count++] = static_cast<wchar_t>(L'0' + value % 10u);
        value /= 10u;
    } while (value != 0);
    while (count > 0) {
        buffer[length++] = digits[--count];
    }
}

uint32 pipeCounter = 0;

// pipe 이름에 넣는 난수다. 이름을 미리 알 수 없어야 다른 로컬 프로세스가 먼저 접속해(인스턴스 1개)
// Start를 막지 못한다. 난수를 얻지 못하면 시각으로 대신한다(FIRST_PIPE_INSTANCE가 남은 충돌을
// 거부한다).
uint64 PipeNonce() noexcept {
    uint64 nonce = 0;
    if (!TryFillSecureRandom(&nonce, sizeof(nonce))) {
        nonce = MonotonicNanoseconds();
    }

    return nonce;
}

} // namespace

// 부모 쪽 pipe 끝 하나다. overlapped named pipe라 시간 제한 대기와 여러 pipe의 동시 대기가 된다.
// 읽기 쪽은 자기 버퍼로 읽는다(시간이 다해 반환해도 대기 중인 읽기가 호출자 버퍼를 쓰지 않게).
// 쓰기 쪽은 호출자 버퍼에서 바로 쓰고, 반환 전에 대기 중인 쓰기를 끝내거나 취소한다.
struct PipeEnd {
    HANDLE handle = nullptr;
    HANDLE event = nullptr;
    OVERLAPPED overlapped{};
    bool pending = false;
    bool endOfStream = false;
    uint8* buffer = nullptr;
    usize begin = 0;
    usize end = 0;

    bool IsOpen() const noexcept {
        return handle != nullptr;
    }

    // 대기 중인 I/O를 취소하고 끝날 때까지 기다린 뒤 닫는다. OVERLAPPED·버퍼를 해제하기 전에 커널이
    // 더는 쓰지 않음을 보장한다.
    void Close() noexcept {
        if (handle != nullptr && pending) {
            CancelIoEx(handle, &overlapped);
            DWORD transferred = 0;
            GetOverlappedResult(handle, &overlapped, &transferred, TRUE);
            pending = false;
        }

        CloseHandleIfValid(handle);
        CloseHandleIfValid(event);
        AlignedFree(buffer);
        buffer = nullptr;
        begin = 0;
        end = 0;
    }
};

struct Process::State {
    HANDLE process = nullptr;
    HANDLE job = nullptr;
    DWORD id = 0;
    bool exited = false;
    bool terminateRequested = false;
    ProcessExit exit{};
    PipeEnd input;
    PipeEnd output;
    PipeEnd error;
};

namespace {

// named pipe 하나를 만든다. 자식 쪽은 동기·상속 가능 handle이고 부모 쪽은 비상속이다.
// - 부모가 읽는 pipe(출력·오류)는 overlapped라 시간 제한 대기와 여러 pipe의 동시 대기가 된다.
// - 부모가 쓰는 pipe(입력)는 비차단(PIPE_NOWAIT) 동기 handle이다. overlapped 쓰기를 시간 초과로
//   취소하면 자식이 이미 읽어 간 양을 OS가 전송량에 반영하지 않으므로(취소한 쓰기는 0 byte로
//   보고된다) "실제로 쓴 양"을 알 수 없다. 비차단 쓰기는 pipe 버퍼에 들어간 만큼만 쓰고 그 수를
//   돌려준다.
// 이름은 프로세스 ID·순번·난수로 만들고 FILE_FLAG_FIRST_PIPE_INSTANCE로 다른 프로세스가 미리 만든
// 같은 이름을 거부한다(충돌하면 다음 이름으로 다시 시도한다). 원격 접속은 받지 않는다.
FileStatus CreatePipe(bool parentReads, PipeEnd& parent, HANDLE& child) noexcept {
    wchar_t name[96] = L"\\\\.\\pipe\\rocks-process-";
    const usize prefix = sizeof(L"\\\\.\\pipe\\rocks-process-") / sizeof(wchar_t) - 1u;
    const DWORD openMode =
        parentReads ? PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE
                    : PIPE_ACCESS_OUTBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE;
    const DWORD pipeMode = PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_REJECT_REMOTE_CLIENTS |
                           (parentReads ? PIPE_WAIT : PIPE_NOWAIT);
    for (int attempt = 0;; ++attempt) {
        usize length = prefix;
        AppendDecimal(name, length, GetCurrentProcessId());
        name[length++] = L'-';
        AppendDecimal(name, length, AtomicFetchAdd<MemoryOrder::Relaxed>(&pipeCounter, 1u));
        name[length++] = L'-';
        AppendDecimal(name, length, PipeNonce());
        name[length] = L'\0';
        parent.handle = CreateNamedPipeW(name, openMode, pipeMode, 1, PipeBufferBytes,
                                         PipeBufferBytes, 0, nullptr);
        if (parent.handle != INVALID_HANDLE_VALUE) {
            break;
        }

        parent.handle = nullptr;
        const DWORD error = GetLastError();
        if ((error != ERROR_ACCESS_DENIED && error != ERROR_PIPE_BUSY) || attempt >= 16) {
            return detail::NativeError(error);
        }
    }

    SECURITY_ATTRIBUTES attributes = InheritableAttributes();
    // 익명 pipe(CreatePipe)와 같은 권한이다. 일부 프로그램이 표준 handle의 속성을 조회·변경한다.
    const DWORD childAccess = parentReads ? (GENERIC_WRITE | FILE_READ_ATTRIBUTES)
                                          : (GENERIC_READ | FILE_WRITE_ATTRIBUTES);
    child = CreateFileW(name, childAccess, 0, &attributes, OPEN_EXISTING, 0, nullptr);
    if (child == INVALID_HANDLE_VALUE) {
        child = nullptr;
        const FileStatus status = LastError();
        parent.Close();
        return status;
    }

    if (parentReads) {
        parent.event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (parent.event == nullptr) {
            const FileStatus status = LastError();
            CloseHandleIfValid(child);
            parent.Close();
            return status;
        }

        parent.overlapped.hEvent = parent.event;
        parent.buffer = static_cast<uint8*>(AlignedAllocate(ReadChunkBytes, 16));
        if (parent.buffer == nullptr) {
            CloseHandleIfValid(child);
            parent.Close();
            return {FileCode::OutOfMemory, 0};
        }
    }

    return {};
}

// 스트림 하나의 자식 쪽 handle을 준비한다. Pipe면 parent에 부모 쪽 끝을 만든다.
FileStatus PrepareStream(ProcessStreamMode mode, DWORD standard, bool parentReads, PipeEnd& parent,
                         HANDLE& child) noexcept {
    switch (mode) {
    case ProcessStreamMode::Null:
        return OpenNullDevice(child);
    case ProcessStreamMode::Inherit:
        return DuplicateStandardHandle(standard, child);
    case ProcessStreamMode::Pipe:
        return CreatePipe(parentReads, parent, child);
    }

    return {FileCode::InvalidArgument, 0};
}

void RecordExit(Process::State& state) noexcept {
    DWORD code = 0;
    if (!GetExitCodeProcess(state.process, &code)) {
        code = static_cast<DWORD>(-1);
    }

    state.exit.kind =
        state.terminateRequested ? ProcessExitKind::Terminated : ProcessExitKind::Exited;
    state.exit.code = code;
    state.exited = true;
}

// 대기 중인 읽기가 끝났으면 결과를 버퍼 범위로 옮긴다. 데이터는 Success, 쓰는 쪽이 모두 닫혔으면
// EndOfFile, 0 byte 완료(쓰는 쪽의 0 byte 쓰기)는 NoProgress다.
FileStatus CompleteRead(PipeEnd& pipe) noexcept {
    DWORD transferred = 0;
    const BOOL done = GetOverlappedResult(pipe.handle, &pipe.overlapped, &transferred, FALSE);
    pipe.pending = false;
    if (!done) {
        const DWORD error = GetLastError();
        if (error == ERROR_BROKEN_PIPE) {
            pipe.endOfStream = true;
            return {FileCode::EndOfFile, 0};
        }

        if (error == ERROR_OPERATION_ABORTED) {
            return {FileCode::NoProgress, 0};
        }

        return detail::NativeError(error);
    }

    pipe.begin = 0;
    pipe.end = transferred;
    return transferred != 0 ? FileStatus{} : FileStatus{FileCode::NoProgress, 0};
}

// 버퍼가 비었으면 읽기를 건다. 바로 끝나면 결과를 반영하고, 대기하면 pending으로 둔다.
FileStatus StartRead(PipeEnd& pipe) noexcept {
    if (pipe.pending || pipe.endOfStream || pipe.begin != pipe.end) {
        return {};
    }

    pipe.begin = 0;
    pipe.end = 0;
    if (ReadFile(pipe.handle, pipe.buffer, ReadChunkBytes, nullptr, &pipe.overlapped)) {
        pipe.pending = true;
        return CompleteRead(pipe);
    }

    const DWORD error = GetLastError();
    if (error == ERROR_IO_PENDING) {
        pipe.pending = true;
        return {};
    }

    if (error == ERROR_BROKEN_PIPE) {
        pipe.endOfStream = true;
        return {FileCode::EndOfFile, 0};
    }

    return detail::NativeError(error);
}

DWORD RemainingMilliseconds(uint64 deadline) noexcept {
    const uint64 now = MonotonicNanoseconds();
    if (now >= deadline) {
        return 0;
    }

    const uint64 remaining = (deadline - now + 999'999u) / 1'000'000u;
    return remaining >= INFINITE ? INFINITE - 1u : static_cast<DWORD>(remaining);
}

uint64 DeadlineAfter(uint32 timeoutMilliseconds) noexcept {
    return MonotonicNanoseconds() + static_cast<uint64>(timeoutMilliseconds) * 1'000'000u;
}

// 비차단 입력 pipe에 들어가는 만큼 쓴다(written은 실제로 pipe 버퍼에 들어간 양이다). 버퍼가 차
// 있으면 0 byte 성공이다. 자식이 입력을 닫았으면 EndOfFile이다.
FileStatus WriteAvailable(PipeEnd& pipe, const uint8* data, usize bytes, usize& written) noexcept {
    const DWORD chunk = bytes < WriteChunkBytes ? static_cast<DWORD>(bytes) : WriteChunkBytes;
    DWORD transferred = 0;
    written = 0;
    if (!WriteFile(pipe.handle, data, chunk, &transferred, nullptr)) {
        const DWORD error = GetLastError();
        if (error == ERROR_NO_DATA || error == ERROR_BROKEN_PIPE ||
            error == ERROR_PIPE_NOT_CONNECTED) {
            return {FileCode::EndOfFile, error};
        }

        return detail::NativeError(error);
    }

    written = transferred;
    return {};
}

} // namespace

FileStatus Process::Start(const ProcessStartInfo& info) noexcept {
    if (state_ != nullptr) {
        return {FileCode::AlreadyOpen, 0};
    }

    auto status = detail::ValidateProcessStartInfo(info);
    if (!status) {
        return status;
    }

    for (usize i = 0; i < info.executableBytes; ++i) {
        if (info.executable[i] == '"') {
            return {FileCode::InvalidArgument, 0};
        }
    }

    if (IsBatchFile(info.executable, info.executableBytes)) {
        return {FileCode::Unsupported, 0};
    }

    detail::WideString application;
    detail::WideString commandLine;
    detail::WideString directory;
    status = application.AssignUtf8(info.executable, info.executableBytes);
    if (status) {
        status = BuildCommandLine(info, commandLine);
    }

    if (status && detail::HasWorkingDirectory(info)) {
        status = directory.AssignUtf8(info.workingDirectory, info.workingDirectoryBytes);
        if (status) {
            // CreateProcessW는 잘못된 작업 디렉터리를 ERROR_DIRECTORY 하나로 보고하므로 먼저 확인해
            // 없음(NotFound)과 디렉터리 아님(WrongType)을 구분한다.
            const DWORD attributes = GetFileAttributesW(directory.Value());
            if (attributes == INVALID_FILE_ATTRIBUTES) {
                status = LastError();
            } else if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                status = {FileCode::WrongType, 0};
            }
        }
    }

    if (!status) {
        return status;
    }

    void* memory = AlignedAllocate(sizeof(State), alignof(State));
    if (memory == nullptr) {
        return {FileCode::OutOfMemory, 0};
    }

    auto* state = new (memory) State;
    HANDLE childInput = nullptr;
    HANDLE childOutput = nullptr;
    HANDLE childError = nullptr;
    LPPROC_THREAD_ATTRIBUTE_LIST attributeList = nullptr;
    bool attributeListReady = false;
    PROCESS_INFORMATION created{};

    // 실패하면 만든 것을 모두 정리하고 시작 전 상태로 돌아간다.
    auto fail = [&](FileStatus failure) noexcept {
        if (created.hProcess != nullptr) {
            TerminateProcess(created.hProcess, TerminateExitCode);
            CloseHandleIfValid(created.hProcess);
        }

        CloseHandleIfValid(created.hThread);
        if (attributeListReady) {
            DeleteProcThreadAttributeList(attributeList);
        }

        AlignedFree(attributeList);
        CloseHandleIfValid(childInput);
        CloseHandleIfValid(childOutput);
        if (!info.mergeErrorIntoOutput) {
            CloseHandleIfValid(childError);
        }

        state->input.Close();
        state->output.Close();
        state->error.Close();
        CloseHandleIfValid(state->job);
        state->~State();
        AlignedFree(state);
        return failure;
    };

    status = PrepareStream(info.input, STD_INPUT_HANDLE, false, state->input, childInput);
    if (status) {
        status = PrepareStream(info.output, STD_OUTPUT_HANDLE, true, state->output, childOutput);
    }

    if (status) {
        if (info.mergeErrorIntoOutput) {
            childError = childOutput;
        } else {
            status = PrepareStream(info.error, STD_ERROR_HANDLE, true, state->error, childError);
        }
    }

    if (!status) {
        return fail(status);
    }

    // 자식이 물려받는 handle을 표준 스트림 셋으로 제한한다. 같은 handle을 두 번 넣으면 실패하므로
    // 합친 출력·오류는 하나만 넣는다.
    HANDLE inherited[3] = {childInput, childOutput, nullptr};
    DWORD inheritedCount = 2;
    if (childError != childOutput && childError != childInput) {
        inherited[inheritedCount++] = childError;
    }

    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    attributeList = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(AlignedAllocate(attributeBytes, 16));
    if (attributeList == nullptr) {
        return fail({FileCode::OutOfMemory, 0});
    }

    if (!InitializeProcThreadAttributeList(attributeList, 1, 0, &attributeBytes)) {
        return fail(LastError());
    }

    attributeListReady = true;
    if (!UpdateProcThreadAttribute(attributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
                                   inheritedCount * sizeof(HANDLE), nullptr, nullptr)) {
        return fail(LastError());
    }

    // 자손까지 한 번에 끝내고, 부모가 죽으면 함께 끝나며, 처리되지 않은 예외에 오류 대화상자를
    // 띄우지 않는 job이다. 탈출(BREAKAWAY_OK)은 허용하지 않는다.
    state->job = CreateJobObjectW(nullptr, nullptr);
    if (state->job == nullptr) {
        return fail(LastError());
    }

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    if (!SetInformationJobObject(state->job, JobObjectExtendedLimitInformation, &limits,
                                 sizeof(limits))) {
        return fail(LastError());
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = childInput;
    startup.StartupInfo.hStdOutput = childOutput;
    startup.StartupInfo.hStdError = childError;
    startup.lpAttributeList = attributeList;
    DWORD flags = CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT;
    if (info.priority == ProcessPriority::Low) {
        flags |= BELOW_NORMAL_PRIORITY_CLASS;
    }

    // 자식을 일시 정지 상태로 만든 뒤 job에 넣고 실행한다. 실행부터 시키면 job에 들어가기 전에 만든
    // 손자가 job 밖에 남는다.
    if (!CreateProcessW(application.Value(), commandLine.Value(), nullptr, nullptr, TRUE, flags,
                        nullptr, detail::HasWorkingDirectory(info) ? directory.Value() : nullptr,
                        &startup.StartupInfo, &created)) {
        return fail(LastError());
    }

    if (!AssignProcessToJobObject(state->job, created.hProcess)) {
        return fail(LastError());
    }

    if (ResumeThread(created.hThread) == static_cast<DWORD>(-1)) {
        return fail(LastError());
    }

    CloseHandleIfValid(created.hThread);
    DeleteProcThreadAttributeList(attributeList);
    AlignedFree(attributeList);
    CloseHandleIfValid(childInput);
    CloseHandleIfValid(childOutput);
    if (!info.mergeErrorIntoOutput) {
        CloseHandleIfValid(childError);
    }

    state->process = created.hProcess;
    state->id = created.dwProcessId;
    state_ = state;
    return {};
}

uint32 Process::Id() const noexcept {
    return state_ != nullptr ? state_->id : 0;
}

WaitResult Process::Wait(uint32 timeoutMilliseconds) noexcept {
    if (state_ == nullptr) {
        return WaitResult::Failed;
    }

    if (state_->exited) {
        return WaitResult::Signaled;
    }

    // InfiniteTimeout은 INFINITE와 같은 값이다.
    const DWORD result = WaitForSingleObject(state_->process, timeoutMilliseconds);
    if (result == WAIT_OBJECT_0) {
        RecordExit(*state_);
        return WaitResult::Signaled;
    }

    return result == WAIT_TIMEOUT ? WaitResult::TimedOut : WaitResult::Failed;
}

bool Process::GetExit(ProcessExit& exit) const noexcept {
    if (state_ == nullptr || !state_->exited) {
        return false;
    }

    exit = state_->exit;
    return true;
}

FileStatus Process::Terminate() noexcept {
    if (state_ == nullptr) {
        return {FileCode::NotOpen, 0};
    }

    if (state_->terminateRequested) {
        return {};
    }

    // 스스로 끝났지만 아직 관측하지 않은 자식은 먼저 Exited로 기록한다. 직접 자식이 이미 끝났으면
    // 종료 종류는 바꾸지 않고 남은 자손만 끝낸다.
    if (!state_->exited && WaitForSingleObject(state_->process, 0) == WAIT_OBJECT_0) {
        RecordExit(*state_);
    }

    if (!TerminateJobObject(state_->job, TerminateExitCode)) {
        const FileStatus failure = LastError();
        // job을 끝내지 못해도 직접 자식은 끝낸다. 그것도 실패하면 요청을 기록하지 않는다(다시
        // 시도할 수 있게).
        if (state_->exited || !TerminateProcess(state_->process, TerminateExitCode)) {
            return failure;
        }
    }

    if (!state_->exited) {
        state_->terminateRequested = true;
    }

    return {};
}

FileTransfer Process::WriteInput(const void* data, usize bytes,
                                 uint32 timeoutMilliseconds) noexcept {
    if (state_ == nullptr || !state_->input.IsOpen()) {
        return {{FileCode::NotOpen, 0}, 0};
    }

    if (bytes != 0 && data == nullptr) {
        return {{FileCode::InvalidArgument, 0}, 0};
    }

    const bool infinite = timeoutMilliseconds == InfiniteTimeout;
    const uint64 deadline = infinite ? 0 : DeadlineAfter(timeoutMilliseconds);
    const auto* source = static_cast<const uint8*>(data);
    usize written = 0;
    DWORD pause = 1;
    while (written < bytes) {
        usize transferred = 0;
        const auto status =
            WriteAvailable(state_->input, source + written, bytes - written, transferred);
        if (!status) {
            return {status, written};
        }

        written += transferred;
        if (transferred != 0) {
            pause = 1;
            continue;
        }

        // pipe 버퍼가 찼다. 비차단 handle은 기다릴 대상(이벤트)이 없으므로 짧게 쉬며 다시 시도한다.
        if (!infinite && RemainingMilliseconds(deadline) == 0) {
            return {{FileCode::NoProgress, 0}, written};
        }

        const DWORD remaining = infinite ? pause : RemainingMilliseconds(deadline);
        Sleep(pause < remaining ? pause : remaining);
        pause = pause * 2u > InputRetryMilliseconds ? InputRetryMilliseconds : pause * 2u;
    }

    return {{}, written};
}

FileStatus Process::CloseInput() noexcept {
    if (state_ == nullptr || !state_->input.IsOpen()) {
        return {FileCode::NotOpen, 0};
    }

    state_->input.Close();
    return {};
}

FileTransfer Process::Read(ProcessStream stream, void* buffer, usize capacity,
                           uint32 timeoutMilliseconds) noexcept {
    if (state_ == nullptr) {
        return {{FileCode::NotOpen, 0}, 0};
    }

    if (stream != ProcessStream::Output && stream != ProcessStream::Error) {
        return {{FileCode::InvalidArgument, 0}, 0};
    }

    PipeEnd& pipe = stream == ProcessStream::Output ? state_->output : state_->error;
    if (!pipe.IsOpen()) {
        return {{FileCode::NotOpen, 0}, 0};
    }

    if (capacity == 0 || buffer == nullptr) {
        return {{FileCode::InvalidArgument, 0}, 0};
    }

    // 데이터·EOF·시간 초과 중 하나가 될 때까지 읽기를 다시 건다. 쓰는 쪽의 0 byte 쓰기는 읽기를
    // 끝내지만 데이터가 아니므로 남은 시간 동안 계속 기다린다.
    const bool infinite = timeoutMilliseconds == InfiniteTimeout;
    const uint64 deadline = infinite ? 0 : DeadlineAfter(timeoutMilliseconds);
    const auto ignorable = [](const FileStatus& status) noexcept {
        return status || status.code == FileCode::EndOfFile || status.code == FileCode::NoProgress;
    };
    while (pipe.begin == pipe.end && !pipe.endOfStream) {
        auto status = StartRead(pipe);
        if (!ignorable(status)) {
            return {status, 0};
        }

        if (pipe.pending) {
            const DWORD waited = WaitForSingleObject(
                pipe.event, infinite ? INFINITE : RemainingMilliseconds(deadline));
            if (waited == WAIT_TIMEOUT) {
                return {{FileCode::NoProgress, 0}, 0};
            }

            if (waited != WAIT_OBJECT_0) {
                return {LastError(), 0};
            }

            status = CompleteRead(pipe);
            if (!ignorable(status)) {
                return {status, 0};
            }
        }

        if (pipe.begin == pipe.end && !pipe.endOfStream && !infinite &&
            RemainingMilliseconds(deadline) == 0) {
            return {{FileCode::NoProgress, 0}, 0};
        }
    }

    if (pipe.begin == pipe.end) {
        return {{pipe.endOfStream ? FileCode::EndOfFile : FileCode::NoProgress, 0}, 0};
    }

    const usize available = pipe.end - pipe.begin;
    const usize taken = available < capacity ? available : capacity;
    MemoryCopy(buffer, pipe.buffer + pipe.begin, taken);
    pipe.begin += taken;
    return {{}, taken};
}

void Process::Close() noexcept {
    State* state = state_;
    if (state == nullptr) {
        return;
    }

    state_ = nullptr;
    if (!state->exited) {
        state->terminateRequested = true;
        if (!TerminateJobObject(state->job, TerminateExitCode)) {
            TerminateProcess(state->process, TerminateExitCode);
        }

        WaitForSingleObject(state->process, CloseWaitMilliseconds);
    }

    state->input.Close();
    state->output.Close();
    state->error.Close();
    // job handle을 닫으면(KILL_ON_JOB_CLOSE) 직접 자식이 먼저 끝난 경우에도 남은 자손이 끝난다.
    CloseHandleIfValid(state->job);
    CloseHandleIfValid(state->process);
    state->~State();
    AlignedFree(state);
}

ProcessRunResult RunProcess(const ProcessStartInfo& info,
                            const ProcessRunOptions& options) noexcept {
    ProcessRunResult result;
    result.status = detail::ValidateProcessRunOptions(info, options);
    if (!result.status) {
        return result;
    }

    Process process;
    result.status = process.Start(info);
    if (!result.status) {
        return result;
    }

    Process::State& state = *process.state_;
    detail::RunSink sinks[2] = {{static_cast<uint8*>(options.output), options.outputCapacity},
                                {static_cast<uint8*>(options.error), options.errorCapacity}};
    PipeEnd* readers[2] = {&state.output, &state.error};
    const auto* input = static_cast<const uint8*>(options.input);
    usize inputLeft = options.inputBytes;
    const bool limited = options.timeoutMilliseconds != InfiniteTimeout;
    const uint64 deadline = limited ? DeadlineAfter(options.timeoutMilliseconds) : 0;
    bool timing = limited;
    // 직접 자식이 끝나거나 강제 종료한 뒤 남은 출력을 모으는 상한이다. 남은 자손을 끝내도 job 밖의
    // 프로세스가 pipe 끝을 물려받았거나(다른 스레드의 handle 목록 없는 CreateProcess) 끝나지 않는
    // 프로세스가 있으면 EOF가 오지 않는다.
    bool draining = false;
    uint64 drainDeadline = 0;
    const auto startDraining = [&]() noexcept {
        if (!draining) {
            draining = true;
            drainDeadline = DeadlineAfter(CloseWaitMilliseconds);
        }
    };

    for (;;) {
        // 0. 직접 자식의 종료와 기한은 pipe 완료보다 먼저 매번 확인한다. 출력이 쉬지 않고 들어와
        //    pipe 이벤트가 늘 켜져 있어도 시간 초과·종료를 놓치지 않게 한다.
        if (!state.exited && WaitForSingleObject(state.process, 0) == WAIT_OBJECT_0) {
            RecordExit(state);
            // pipe를 잡고 남은 자손을 끝내 나머지 출력이 EOF로 끝나게 한다.
            TerminateJobObject(state.job, TerminateExitCode);
            startDraining();
        }

        if (timing && RemainingMilliseconds(deadline) == 0) {
            // 시간이 다했다. 자식·자손을 끝내고 pipe가 닫힐 때까지 나머지를 모은다.
            result.timedOut = true;
            timing = false;
            const auto terminated = process.Terminate();
            if (!terminated) {
                result.status = terminated;
                break;
            }

            startDraining();
        }

        if (draining && RemainingMilliseconds(drainDeadline) == 0) {
            // 상한 안에 EOF가 오지 않은 스트림은 모으지 못한 출력이 있을 수 있다.
            for (int index = 0; index < 2; ++index) {
                if (readers[index]->IsOpen() && !readers[index]->endOfStream) {
                    sinks[index].truncated = true;
                }
            }

            break;
        }

        // 1. 읽기 쪽: 끝난 데이터는 버퍼로 옮기고 새 읽기를 건다. 바로 끝나는 읽기는 한 번에 몇
        //    번까지만 이어 간다(한 스트림이 다른 스트림·쓰기·종료 확인을 굶기지 않게).
        for (int index = 0; index < 2 && result.status; ++index) {
            PipeEnd& pipe = *readers[index];
            for (int burst = 0; burst < 4 && pipe.IsOpen() && !pipe.pending && !pipe.endOfStream;
                 ++burst) {
                if (pipe.begin != pipe.end) {
                    sinks[index].Append(pipe.buffer + pipe.begin, pipe.end - pipe.begin);
                    pipe.begin = pipe.end = 0;
                }

                const auto status = StartRead(pipe);
                if (!status && status.code != FileCode::EndOfFile &&
                    status.code != FileCode::NoProgress) {
                    result.status = status;
                }

                if (!result.status) {
                    break;
                }
            }
        }

        // 2. 쓰기 쪽: pipe 버퍼에 들어가는 만큼 쓰고, 다 썼으면 입력을 닫아 자식이 EOF를 보게 한다.
        //    버퍼가 차면 다음 반복에서 다시 시도한다(비차단 handle이라 기다릴 이벤트가 없다).
        PipeEnd& in = state.input;
        bool inputBlocked = false;
        for (int burst = 0; burst < 4 && result.status && in.IsOpen() && inputLeft != 0; ++burst) {
            usize transferred = 0;
            const auto status = WriteAvailable(in, input, inputLeft, transferred);
            if (status.code == FileCode::EndOfFile) {
                // 자식이 입력을 닫았다. 남은 입력은 버린다(실패가 아니다).
                inputLeft = 0;
                break;
            }

            if (!status) {
                result.status = status;
                break;
            }

            if (transferred == 0) {
                inputBlocked = true;
                break;
            }

            input += transferred;
            inputLeft -= transferred;
        }

        if (result.status && in.IsOpen() && inputLeft == 0) {
            in.Close();
        }

        inputBlocked = inputBlocked || (in.IsOpen() && inputLeft != 0);

        if (!result.status) {
            break;
        }

        // 3. 남은 일을 함께 기다린다. 아직 읽기를 걸지 못한 pipe(버퍼에 데이터가 남음)가 있으면
        //    기다리지 않고 다음 반복에서 마저 옮긴다. 종료·기한은 0단계가 매번 확인하므로 순서는
        //    공정성에만 영향을 준다.
        HANDLE handles[4];
        int kinds[4];
        DWORD count = 0;
        bool ready = false;
        for (int index = 0; index < 2; ++index) {
            const PipeEnd& pipe = *readers[index];
            if (pipe.pending) {
                handles[count] = pipe.event;
                kinds[count++] = index;
            } else if (pipe.IsOpen() && !pipe.endOfStream) {
                ready = true;
            }
        }

        if (!state.exited) {
            handles[count] = state.process;
            kinds[count++] = 3;
        }

        if (count == 0) {
            if (ready) {
                continue;
            }

            if (inputBlocked) {
                // 자식은 끝났는데 입력이 남았다(자손이 입력을 읽고 있다). 잠시 쉬고 다시 쓴다.
                Sleep(InputRetryMilliseconds);
                continue;
            }

            break;
        }

        DWORD waitMilliseconds = INFINITE;
        if (ready) {
            waitMilliseconds = 0;
        } else if (timing) {
            waitMilliseconds = RemainingMilliseconds(deadline);
        } else if (draining) {
            waitMilliseconds = RemainingMilliseconds(drainDeadline);
        }

        if (inputBlocked && waitMilliseconds > InputRetryMilliseconds) {
            waitMilliseconds = InputRetryMilliseconds;
        }

        const DWORD waited = WaitForMultipleObjects(count, handles, FALSE, waitMilliseconds);
        if (waited == WAIT_TIMEOUT) {
            // 기한·남은 출력 상한은 0단계가 처리한다.
            continue;
        }

        if (waited >= WAIT_OBJECT_0 + count) {
            result.status = LastError();
            break;
        }

        const int kind = kinds[waited - WAIT_OBJECT_0];
        if (kind < 2) {
            const auto status = CompleteRead(*readers[kind]);
            if (!status && status.code != FileCode::EndOfFile &&
                status.code != FileCode::NoProgress) {
                result.status = status;
                break;
            }
        } else {
            RecordExit(state);
            // 직접 자식이 끝났다. pipe를 잡고 남은 자손을 끝내 나머지 출력이 EOF로 끝나게 한다.
            TerminateJobObject(state.job, TerminateExitCode);
            startDraining();
        }
    }

    if (result.status) {
        for (int index = 0; index < 2; ++index) {
            PipeEnd& pipe = *readers[index];
            if (pipe.begin != pipe.end) {
                sinks[index].Append(pipe.buffer + pipe.begin, pipe.end - pipe.begin);
                pipe.begin = pipe.end = 0;
            }
        }

        // 남은 수집 상한에서 끝났으면 강제 종료한 자식의 끝남도 상한까지만 기다린다.
        const WaitResult waited =
            state.exited ? WaitResult::Signaled : process.Wait(CloseWaitMilliseconds);
        if (waited == WaitResult::TimedOut) {
            result.status = {FileCode::SystemError, WAIT_TIMEOUT};
        } else if (waited != WaitResult::Signaled) {
            result.status = LastError();
        }

        process.GetExit(result.exit);
    }

    result.outputBytes = sinks[0].bytes;
    result.outputTruncated = sinks[0].truncated;
    result.errorBytes = sinks[1].bytes;
    result.errorTruncated = sinks[1].truncated;
    process.Close();
    return result;
}

uint32 CurrentProcessId() noexcept {
    return GetCurrentProcessId();
}

bool IsProcessRunning(uint32 processId) noexcept {
    if (processId == 0) {
        return false;
    }

    const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, processId);
    if (process == nullptr) {
        // 있지만 열 권한이 없는 프로세스다.
        return GetLastError() == ERROR_ACCESS_DENIED;
    }

    const DWORD result = WaitForSingleObject(process, 0);
    CloseHandle(process);
    return result == WAIT_TIMEOUT;
}

FileStatus GetExecutablePath(char* buffer, usize capacity, usize& bytes) noexcept {
    if (buffer == nullptr && capacity != 0) {
        return {FileCode::InvalidArgument, 0};
    }

    detail::WideString path;
    auto status = path.AssignModuleFileName();
    if (!status) {
        return status;
    }

    return detail::WriteUtf8(path.Value(), path.Units(), buffer, capacity, bytes);
}

} // namespace rocks::platform
