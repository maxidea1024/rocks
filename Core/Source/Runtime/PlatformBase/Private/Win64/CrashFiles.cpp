#include "CrashFiles.h"

#include <Base/Compiler.h>
#include <Platform/Atomics.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>
#include <Platform/Win64/WideString.h>

namespace rocks::platform::detail {

namespace {

// 경로 칸의 UTF-16 단위 상한이다(뿌리 경로, 그 뒤에 폴더·파일 이름이 붙는다).
constexpr usize MaxRootUnits = 1024;
constexpr usize FolderUnits = CrashFolderUnits;
static_assert(FolderUnits > MaxRootUnits + CrashFolderNameBytes, "folder slot holds root and name");
constexpr usize FileUnits = FolderUnits + 32;
// UTF-16 단위 하나가 UTF-8로 최대 3 byte다(짝 surrogate는 4 byte/2 단위).
constexpr usize FolderTextBytes = FolderUnits * 3;
constexpr usize ModuleJsonBytes = CrashModuleJsonBytes;
constexpr usize ModuleJsonReserve = 64;
constexpr usize CommentMessageBytes = 384;
constexpr usize ErrorTextBytes = 16;
// PE 머리가 들어 있는 첫 페이지 크기다(이미지 크기를 읽는 범위).
constexpr usize HeaderPageBytes = 4096;
constexpr uint32 TimeStampDigits = 8;
constexpr uint32 BitsPerByte = 8;
constexpr wchar_t DefaultRoot[] = L"Saved\\Crashes";
constexpr wchar_t FallbackSubdirectory[] = L"Rocks\\Crashes\\";
constexpr wchar_t MetadataFileName[] = L"CrashInfo.json";
constexpr wchar_t MetadataTemporaryName[] = L"CrashInfo.json.tmp";
constexpr const wchar_t* DumpFileName = CrashDumpFileName;
constexpr const wchar_t* DetailDumpFileName = CrashDetailDumpFileName;
constexpr const char* DumpFileText = CrashDumpFileText;
constexpr const char* DetailDumpFileText = CrashDetailDumpFileText;

using MiniDumpWriteDumpFunction = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                                PMINIDUMP_EXCEPTION_INFORMATION,
                                                PMINIDUMP_USER_STREAM_INFORMATION,
                                                PMINIDUMP_CALLBACK_INFORMATION);

constexpr auto StandardDumpType = static_cast<MINIDUMP_TYPE>(
    MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithUnloadedModules);
constexpr auto DetailedDumpType =
    static_cast<MINIDUMP_TYPE>(StandardDumpType | MiniDumpWithDataSegs | MiniDumpWithHandleData |
                               MiniDumpWithProcessThreadData | MiniDumpWithFullMemoryInfo);
constexpr auto FullDumpType = static_cast<MINIDUMP_TYPE>(DetailedDumpType | MiniDumpWithFullMemory);

// 설치 때 정한다(설치·SetCrashOutputDirectory 때만 쓴다). 주 폴더는 칸 두 개를 번갈아 쓴다. 바꿀 때
// 쓰지 않는 칸을 채운 뒤 원자 index로 바꿔 끼우므로, 겹친 크래시가 쓰는 도중의 경로를 읽지 않는다.
constinit wchar_t primaryFolders[2][FolderUnits] = {};
constinit char primaryFolderTexts[2][FolderTextBytes] = {};
constinit uint32 primaryFolderIndex = 0;
constinit wchar_t fallbackFolder[FolderUnits] = {};
constinit char fallbackFolderText[FolderTextBytes] = {};
constinit char moduleJson[ModuleJsonBytes] = {};
constinit bool hasModuleJson = false;
constinit HMODULE dbghelp = nullptr;
constinit MiniDumpWriteDumpFunction writeMiniDump = nullptr;

// 쓰는 스레드(부모 크래시 처리 스레드 또는 도우미)만 쓴다.
constinit wchar_t filePath[FileUnits] = {};
constinit wchar_t temporaryPath[FileUnits] = {};
constinit wchar_t directoryPath[FolderUnits] = {};
constinit uint64 dumpDeadline = 0;
constinit bool dumpCancelled = false;
constinit bool dumpFromSnapshot = false;
constinit const CrashMemoryRange* memoryRanges = nullptr;
constinit usize memoryRangeCount = 0;
constinit usize memoryRangeIndex = 0;
// 프로세스 안 흐름의 기록 사본·범위·설명이다.
constinit CrashRecordSnapshot inProcessRecord = {};
constinit CrashMemoryRange inProcessRanges[CrashRecordRangeCount] = {};
constinit char inProcessComment[CrashCommentBytes] = {};

usize WideLengthOf(const wchar_t* text) noexcept {
    usize length = 0;
    while (text[length] != L'\0') {
        ++length;
    }

    return length;
}

// destination(capacity 단위, NUL 포함)의 length 뒤에 source를 붙인다. 넘치면 false다.
bool AppendWide(wchar_t* destination, usize capacity, usize& length,
                const wchar_t* source) noexcept {
    for (usize index = 0; source[index] != L'\0'; ++index) {
        if (length + 1 >= capacity) {
            return false;
        }

        destination[length++] = source[index];
    }

    destination[length] = L'\0';
    return true;
}

bool AppendAscii(wchar_t* destination, usize capacity, usize& length, const char* source) noexcept {
    for (usize index = 0; source[index] != '\0'; ++index) {
        if (length + 1 >= capacity) {
            return false;
        }

        destination[length++] = static_cast<wchar_t>(static_cast<uint8>(source[index]));
    }

    destination[length] = L'\0';
    return true;
}

bool EndsWithSeparator(const wchar_t* text, usize length) noexcept {
    return length != 0 && (text[length - 1] == L'\\' || text[length - 1] == L'/');
}

void ToUtf8(const wchar_t* text, char* buffer, usize capacity) noexcept {
    (void)windows::ConvertWideToUtf8(text, WideLengthOf(text), buffer, capacity,
                                     windows::TextFit::Truncate);
}

// root(UTF-16, 상대 가능)를 절대 경로로 굳히고 크래시 폴더 이름을 붙인다.
bool BuildFolder(const wchar_t* root, wchar_t* folder, char* folderText) noexcept {
    wchar_t absolute[MaxRootUnits];
    const DWORD units = GetFullPathNameW(root, MaxRootUnits, absolute, nullptr);
    if (units == 0 || units >= MaxRootUnits) {
        return false;
    }

    usize length = 0;
    folder[0] = L'\0';
    if (!AppendWide(folder, FolderUnits, length, absolute) ||
        (!EndsWithSeparator(folder, length) && !AppendWide(folder, FolderUnits, length, L"\\")) ||
        !AppendAscii(folder, FolderUnits, length, CrashFolderName())) {
        folder[0] = L'\0';
        return false;
    }

    ToUtf8(folder, folderText, FolderTextBytes);
    return true;
}

bool BuildPrimaryFolder(const char* directory) noexcept {
    wchar_t root[MaxRootUnits];
    if (directory == nullptr) {
        usize length = 0;
        root[0] = L'\0';
        (void)AppendWide(root, MaxRootUnits, length, DefaultRoot);
    } else {
        usize bytes = 0;
        while (directory[bytes] != '\0') {
            ++bytes;
        }

        if (!windows::ConvertUtf8ToWide(directory, bytes, root, MaxRootUnits,
                                        windows::TextFit::Exact)) {
            return false;
        }
    }

    wchar_t folder[FolderUnits];
    char folderText[FolderTextBytes];
    if (!BuildFolder(root, folder, folderText)) {
        return false;
    }

    const uint32 next = AtomicLoad<MemoryOrder::Relaxed>(&primaryFolderIndex) ^ 1u;
    usize length = 0;
    primaryFolders[next][0] = L'\0';
    (void)AppendWide(primaryFolders[next], FolderUnits, length, folder);
    for (usize index = 0; index < FolderTextBytes; ++index) {
        primaryFolderTexts[next][index] = folderText[index];
        if (folderText[index] == '\0') {
            break;
        }
    }

    AtomicStore<MemoryOrder::Release>(&primaryFolderIndex, next);
    return true;
}

void BuildFallbackFolder() noexcept {
    wchar_t root[MaxRootUnits];
    fallbackFolder[0] = L'\0';
    fallbackFolderText[0] = '\0';
    const DWORD units = GetTempPathW(MaxRootUnits, root);
    if (units == 0 || units >= MaxRootUnits) {
        return;
    }

    usize length = units;
    if ((!EndsWithSeparator(root, length) && !AppendWide(root, MaxRootUnits, length, L"\\")) ||
        !AppendWide(root, MaxRootUnits, length, FallbackSubdirectory)) {
        return;
    }

    (void)BuildFolder(root, fallbackFolder, fallbackFolderText);
}

// 주 모듈(실행 파일) 메모리 이미지의 PE 머리에서 CodeView(RSDS) 레코드를 읽는다. 디버거의 심볼 일치
// 검사가 쓰는 값(PDB GUID·age)과 실행 파일 저장소 키(TimeDateStamp·SizeOfImage)다. 이미지 범위를
// 벗어나는 머리·디렉터리가 있으면 module을 null로 둔다(ParsePeCodeView가 모든 범위를 확인한다).
void CaptureModuleIdentity() noexcept {
    hasModuleJson = false;
    const auto* base = reinterpret_cast<const uint8*>(GetModuleHandleW(nullptr));
    // 머리는 첫 페이지 안에 있다. 그 안에서 이미지 크기를 읽은 뒤 이미지 전체 범위로 다시 읽는다.
    const usize imageBytes = PeImageSize(base, HeaderPageBytes);
    PeCodeView codeView;
    if (imageBytes == 0 || !ParsePeCodeView(base, imageBytes, codeView)) {
        return;
    }

    CrashJsonWriter json(moduleJson, sizeof(moduleJson), ModuleJsonReserve);
    // 설치 때 한 번이다. 실행 파일 이름은 공통 기록에서 얻는다.
    CaptureCrashRecord(inProcessRecord);
    json.BeginObject();
    json.Key("name");
    json.String(inProcessRecord.hasExecutable ? inProcessRecord.executable : nullptr);
    json.Key("pdbName");
    json.String(codeView.hasCodeView ? codeView.pdbName : nullptr, codeView.pdbNameBytes,
                NumericLimits<usize>::Max());
    json.Key("pdbGuid");
    json.Constant(codeView.hasCodeView ? codeView.guid : nullptr);
    json.Key("pdbAge");
    json.Unsigned(codeView.age);
    json.Key("imageTimeDateStamp");
    json.Hex(codeView.timeDateStamp, TimeStampDigits);
    json.Key("imageSize");
    json.Unsigned(codeView.sizeOfImage);
    json.EndObject();
    hasModuleJson = !json.Truncated();
}

void LoadDbgHelp() noexcept {
    // 크래시 뒤에는 loader lock 때문에 로드하지 않는다. System32에서만 찾는다(DLL 바꿔치기 방지).
    dbghelp = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    writeMiniDump = dbghelp != nullptr
                        ? reinterpret_cast<MiniDumpWriteDumpFunction>(
                              reinterpret_cast<void*>(GetProcAddress(dbghelp, "MiniDumpWriteDump")))
                        : nullptr;
}

// path의 각 상위 폴더를 만든 뒤 path 자체를 만든다. path가 폴더로 있으면 성공이다. 중간 단계 오류는
// 무시하고(이미 있음·드라이브 뿌리) 마지막 결과만 본다.
// source 폴더를 상위부터 만든다. 경로는 쓰는 스레드의 칸(directoryPath)에 복사해 다룬다(공유 경로
// 칸을 잠시도 바꾸지 않는다).
DWORD CreateDirectoryTree(const wchar_t* source) noexcept {
    usize length = 0;
    directoryPath[0] = L'\0';
    if (!AppendWide(directoryPath, FolderUnits, length, source)) {
        return ERROR_FILENAME_EXCED_RANGE;
    }

    wchar_t* path = directoryPath;
    for (usize index = 1; path[index] != L'\0'; ++index) {
        if ((path[index] == L'\\' || path[index] == L'/') && path[index - 1] != L':' &&
            path[index - 1] != L'\\') {
            const wchar_t saved = path[index];
            path[index] = L'\0';
            (void)CreateDirectoryW(path, nullptr);
            path[index] = saved;
        }
    }

    if (!CreateDirectoryW(path, nullptr)) {
        const DWORD error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS) {
            return error;
        }
    }

    const DWORD attributes = GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return GetLastError();
    }

    return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ? ERROR_SUCCESS : ERROR_DIRECTORY;
}

bool BuildFilePath(wchar_t* destination, const wchar_t* folder, const wchar_t* name) noexcept {
    usize length = 0;
    destination[0] = L'\0';
    return AppendWide(destination, FileUnits, length, folder) &&
           AppendWide(destination, FileUnits, length, L"\\") &&
           AppendWide(destination, FileUnits, length, name);
}

DWORD WriteWholeFile(const wchar_t* path, const char* bytes, usize count) noexcept {
    const HANDLE file =
        CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return GetLastError();
    }

    DWORD error = ERROR_SUCCESS;
    usize written = 0;
    while (written < count) {
        DWORD chunk = 0;
        const usize remaining = count - written;
        const DWORD request = remaining > NumericLimits<DWORD>::Max()
                                  ? NumericLimits<DWORD>::Max()
                                  : static_cast<DWORD>(remaining);
        if (!WriteFile(file, bytes + written, request, &chunk, nullptr) || chunk == 0) {
            error = GetLastError();
            break;
        }

        written += chunk;
    }

    CloseHandle(file);
    return error;
}

BOOL CALLBACK DumpCallback(PVOID, PMINIDUMP_CALLBACK_INPUT input,
                           PMINIDUMP_CALLBACK_OUTPUT output) noexcept {
    if (input == nullptr || output == nullptr) {
        return TRUE;
    }

    switch (input->CallbackType) {
    case IsProcessSnapshotCallback:
        // 대상이 PssCaptureSnapshot 핸들임을 알린다(도우미 덤프). 프로세스 안 덤프는 S_OK다.
        output->Status = dumpFromSnapshot ? S_FALSE : S_OK;
        return TRUE;
    case CancelCallback:
        // 마감을 넘으면 취소한다(취소된 덤프는 열리지 않으므로 호출자가 지운다).
        output->Cancel = MonotonicNanoseconds() >= dumpDeadline ? TRUE : FALSE;
        output->CheckCancel = TRUE;
        dumpCancelled = dumpCancelled || output->Cancel != FALSE;
        return TRUE;
    case MemoryCallback:
        // 정적 크래시 기록 범위를 덤프에 넣는다(디버거가 기호로 바로 본다). 다 넣으면 FALSE다.
        if (memoryRangeIndex >= memoryRangeCount) {
            return FALSE;
        }

        output->MemoryBase = reinterpret_cast<ULONG64>(memoryRanges[memoryRangeIndex].address);
        output->MemorySize = static_cast<ULONG>(memoryRanges[memoryRangeIndex].bytes);
        ++memoryRangeIndex;
        return TRUE;
    default:
        return TRUE;
    }
}

void WriteText(const char* text) noexcept {
    usize length = 0;
    while (text[length] != '\0') {
        ++length;
    }

    WriteDiagnostic(text, length);
}

} // namespace

bool LoadCrashDumpWriter() noexcept {
    if (writeMiniDump == nullptr) {
        LoadDbgHelp();
    }

    return writeMiniDump != nullptr;
}

const char* CrashModuleJson() noexcept {
    return hasModuleJson ? moduleJson : nullptr;
}

const wchar_t* CrashPrimaryFolder() noexcept {
    return primaryFolders[AtomicLoad<MemoryOrder::Acquire>(&primaryFolderIndex)];
}

const wchar_t* CrashFallbackFolder() noexcept {
    return fallbackFolder;
}

const char* CrashPrimaryFolderText() noexcept {
    return primaryFolderTexts[AtomicLoad<MemoryOrder::Acquire>(&primaryFolderIndex)];
}

const char* CrashFallbackFolderText() noexcept {
    return fallbackFolderText;
}

// 폴더 경로와 Win32 오류를 한 줄씩 남긴다(저장 실패 때 최소 비상 진단).
void WriteCrashFolderFailure(const char* primaryText, DWORD primaryError, const char* fallbackText,
                             DWORD fallbackError) noexcept {
    char number[ErrorTextBytes];
    WriteDiagnosticLine("Rocks crash report directory unavailable; no crash files were written");
    WriteText("  primary: ");
    WriteText(primaryText);
    WriteText(" (error ");
    (void)FormatCodeMessage(number, sizeof(number), "", primaryError, false);
    WriteText(number);
    WriteDiagnosticLine(")");
    WriteText("  fallback: ");
    WriteText(fallbackText);
    WriteText(" (error ");
    (void)FormatCodeMessage(number, sizeof(number), "", fallbackError, false);
    WriteText(number);
    WriteDiagnosticLine(")");
}

DWORD WriteCrashMetadataFile(const wchar_t* folder, const char* bytes, usize count,
                             bool final) noexcept {
    if (!BuildFilePath(filePath, folder, MetadataFileName)) {
        return ERROR_FILENAME_EXCED_RANGE;
    }

    if (!final) {
        return WriteWholeFile(filePath, bytes, count);
    }

    // 최종본은 임시 파일에 쓴 뒤 바꿔 넣는다. 임시 파일을 쓰지 못하면 예비본을 그대로 둔다(덮어써
    // 둘 다 잃지 않는다). 바꿔 넣기만 실패하면 직접 쓰고 임시 파일을 지운다.
    if (!BuildFilePath(temporaryPath, folder, MetadataTemporaryName)) {
        return ERROR_FILENAME_EXCED_RANGE;
    }

    const DWORD error = WriteWholeFile(temporaryPath, bytes, count);
    if (error != ERROR_SUCCESS) {
        (void)DeleteFileW(temporaryPath);
        return error;
    }

    if (MoveFileExW(temporaryPath, filePath, MOVEFILE_REPLACE_EXISTING)) {
        return ERROR_SUCCESS;
    }

    const DWORD direct = WriteWholeFile(filePath, bytes, count);
    (void)DeleteFileW(temporaryPath);
    return direct;
}

void OpenCrashReportFolder(CrashReportFolder& report, const char* preliminary,
                           usize bytes) noexcept {
    report.current = nullptr;
    report.fallbackUsed = false;
    report.wroteFile = false;
    report.primaryError =
        report.primary[0] != L'\0' ? CreateDirectoryTree(report.primary) : ERROR_PATH_NOT_FOUND;
    if (report.primaryError == ERROR_SUCCESS) {
        report.primaryError = WriteCrashMetadataFile(report.primary, preliminary, bytes, false);
        if (report.primaryError == ERROR_SUCCESS) {
            report.current = report.primary;
            report.wroteFile = true;
            return;
        }
    }

    (void)MoveCrashReportToFallback(report, report.primaryError, preliminary, bytes);
}

bool MoveCrashReportToFallback(CrashReportFolder& report, DWORD primaryError,
                               const char* preliminary, usize bytes) noexcept {
    if (report.fallbackUsed) {
        return false;
    }

    // 폴더를 만들었더라도 파일을 만들 수 없으면(권한·같은 이름의 폴더 등) 임시 폴더로 한 번 옮긴다.
    // primary에 이미 쓴 예비 CrashInfo.json은 지운다(미완 예비본이 실제 보고 위치를 가리지 않는다).
    if (report.wroteFile && report.current == report.primary &&
        BuildFilePath(filePath, report.primary, MetadataFileName)) {
        (void)DeleteFileW(filePath);
    }

    report.primaryError = primaryError;
    report.fallbackUsed = true;
    report.fallbackError =
        report.fallback[0] != L'\0' ? CreateDirectoryTree(report.fallback) : ERROR_PATH_NOT_FOUND;
    if (report.fallbackError == ERROR_SUCCESS) {
        report.fallbackError = WriteCrashMetadataFile(report.fallback, preliminary, bytes, false);
    }

    if (report.fallbackError != ERROR_SUCCESS) {
        report.current = nullptr;
        report.wroteFile = false;
        return false;
    }

    report.current = report.fallback;
    report.wroteFile = true;
    return true;
}

CrashDumpFile OpenCrashDumpFile(const wchar_t* folder, const wchar_t* name) noexcept {
    CrashDumpFile file;
    if (!BuildFilePath(filePath, folder, name)) {
        file.error = ERROR_FILENAME_EXCED_RANGE;
        return file;
    }

    file.handle = CreateFileW(filePath, GENERIC_READ | GENERIC_WRITE | DELETE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file.handle == INVALID_HANDLE_VALUE) {
        file.error = GetLastError();
        return file;
    }

    // 다 쓰기 전에는 삭제 예약 상태로 둔다. 쓰는 프로세스가 도중에 끝나도(크래시한 스레드의 총
    // 예산이 끝남) 열 수 없는 덤프가 남지 않는다. 다 쓰면 예약을 푼다.
    FILE_DISPOSITION_INFO disposition{};
    disposition.DeleteFile = TRUE;
    file.pendingDelete = SetFileInformationByHandle(file.handle, FileDispositionInfo, &disposition,
                                                    sizeof(disposition)) != FALSE;
    return file;
}

void CloseCrashDumpFile(CrashDumpFile& file) noexcept {
    if (file.handle != INVALID_HANDLE_VALUE) {
        CloseHandle(file.handle);
        file.handle = INVALID_HANDLE_VALUE;
    }
}

CrashDumpWrite WriteCrashDumpFile(const wchar_t* folder, const wchar_t* name, MINIDUMP_TYPE type,
                                  const CrashDumpSource& source, const char* record,
                                  usize recordBytes, const char* comment,
                                  uint64 deadlineNanoseconds) noexcept {
    if (writeMiniDump == nullptr) {
        return {"Failed", ERROR_MOD_NOT_FOUND, false};
    }

    CrashDumpFile file = OpenCrashDumpFile(folder, name);
    if (file.handle == INVALID_HANDLE_VALUE) {
        return {"Failed", file.error, true};
    }

    return WriteOpenedCrashDumpFile(file, folder, name, type, source, record, recordBytes, comment,
                                    deadlineNanoseconds);
}

CrashDumpWrite WriteOpenedCrashDumpFile(CrashDumpFile& opened, const wchar_t* folder,
                                        const wchar_t* name, MINIDUMP_TYPE type,
                                        const CrashDumpSource& source, const char* record,
                                        usize recordBytes, const char* comment,
                                        uint64 deadlineNanoseconds) noexcept {
    if (writeMiniDump == nullptr) {
        CloseCrashDumpFile(opened);
        return {"Failed", ERROR_MOD_NOT_FOUND, false};
    }

    const HANDLE file = opened.handle;
    const bool pendingDelete = opened.pendingDelete;
    FILE_DISPOSITION_INFO disposition{};
    MINIDUMP_EXCEPTION_INFORMATION exception{};
    // 덤프의 예외 스레드는 실패한 스레드다(쓰는 스레드가 아니다). 문맥은 부모의 정적 사본이다.
    exception.ThreadId = source.failingThreadId;
    exception.ExceptionPointers = source.pointers;
    exception.ClientPointers = source.clientPointers ? TRUE : FALSE;
    MINIDUMP_USER_STREAM streams[2] = {};
    streams[0].Type = CrashRecordStreamType;
    streams[0].BufferSize = static_cast<ULONG>(recordBytes);
    streams[0].Buffer = const_cast<char*>(record);
    usize commentBytes = 0;
    while (comment[commentBytes] != '\0') {
        ++commentBytes;
    }

    streams[1].Type = CommentStreamA;
    streams[1].BufferSize = static_cast<ULONG>(commentBytes + 1);
    streams[1].Buffer = const_cast<char*>(comment);
    MINIDUMP_USER_STREAM_INFORMATION userStreams{};
    userStreams.UserStreamCount = 2;
    userStreams.UserStreamArray = streams;
    MINIDUMP_CALLBACK_INFORMATION callback{};
    callback.CallbackRoutine = &DumpCallback;
    callback.CallbackParam = nullptr;
    memoryRanges = source.ranges;
    memoryRangeCount = source.ranges != nullptr ? source.rangeCount : 0;
    memoryRangeIndex = 0;
    dumpFromSnapshot = source.snapshot;
    dumpCancelled = false;
    dumpDeadline = deadlineNanoseconds;
    const BOOL written =
        writeMiniDump(source.process, source.processId, file, type,
                      source.pointers != nullptr ? &exception : nullptr, &userStreams, &callback);
    const DWORD error = written ? ERROR_SUCCESS : GetLastError();
    bool kept = false;
    if (written) {
        disposition.DeleteFile = FALSE;
        kept = !pendingDelete || SetFileInformationByHandle(file, FileDispositionInfo, &disposition,
                                                            sizeof(disposition)) != FALSE;
    }

    CloseCrashDumpFile(opened);
    if (written && kept) {
        return {"Written", ERROR_SUCCESS, false};
    }

    // 취소·실패한 덤프는 열 수 없다(시제품 확인). 삭제 예약이 없었으면 직접 지운다.
    if (!pendingDelete && BuildFilePath(filePath, folder, name)) {
        (void)DeleteFileW(filePath);
    }

    return {dumpCancelled ? "Cancelled" : "Failed", written ? GetLastError() : error, false};
}

MINIDUMP_TYPE StandardCrashDumpType() noexcept {
    return StandardDumpType;
}

MINIDUMP_TYPE DetailCrashDumpType(CrashDumpDetail detail) noexcept {
    return detail == CrashDumpDetail::Full ? FullDumpType : DetailedDumpType;
}

const char* CrashDumpDetailName(CrashDumpDetail detail) noexcept {
    switch (detail) {
    case CrashDumpDetail::Standard:
        return "Standard";
    case CrashDumpDetail::Detailed:
        return "Detailed";
    case CrashDumpDetail::Full:
        return "Full";
    }

    return "Standard";
}

usize FormatCrashRecordStream(char* buffer, usize capacity, const CrashRecordSnapshot& record,
                              const CrashMetadataInput& input, usize& jsonOffset) noexcept {
    usize prefix = 0;
    for (; CrashRecordStreamName[prefix] != '\0' && prefix + 1 < capacity; ++prefix) {
        buffer[prefix] = CrashRecordStreamName[prefix];
    }

    buffer[prefix++] = '\n';
    jsonOffset = prefix;
    return prefix + FormatCrashMetadata(buffer + prefix, capacity - prefix, record, input);
}

void FormatCrashComment(char* buffer, usize capacity, const CrashRecordSnapshot& record) noexcept {
    usize length = 0;
    const auto put = [&](const char* text, usize limit) noexcept {
        for (usize index = 0; index < limit && text[index] != '\0' && length + 1 < capacity;
             ++index) {
            buffer[length++] = text[index];
        }
    };
    put("Rocks crash ", capacity);
    put(record.crashId, capacity);
    put(" ", 1);
    put(record.program, capacity);
    put(": ", 2);
    put(record.message,
        record.messageBytes < CommentMessageBytes ? record.messageBytes : CommentMessageBytes);
    buffer[length] = '\0';
}

bool PrepareCrashFiles(const char* outputDirectory, bool writeDumps) noexcept {
    AtomicStore<MemoryOrder::Relaxed>(&primaryFolderIndex, 0u);
    primaryFolders[0][0] = L'\0';
    primaryFolderTexts[0][0] = '\0';
    if (!BuildPrimaryFolder(outputDirectory)) {
        return false;
    }

    BuildFallbackFolder();
    CaptureModuleIdentity();
    if (writeDumps) {
        LoadDbgHelp();
    }

    return true;
}

bool ResolveCrashOutputDirectory(const char* directory) noexcept {
    return IsValidCrashDirectoryText(directory) && BuildPrimaryFolder(directory);
}

void ReleaseCrashFiles() noexcept {
    writeMiniDump = nullptr;
    if (dbghelp != nullptr) {
        FreeLibrary(dbghelp);
        dbghelp = nullptr;
    }
}

bool CanWriteInProcessDump() noexcept {
    return writeMiniDump != nullptr;
}

void WriteCrashFiles(EXCEPTION_POINTERS* pointers, const CrashInProcessReport& request,
                     const CrashHelperOutcome& helper) noexcept {
    CrashMetadataInput input;
    input.hasCrashUtc = TryUtcTicks(input.crashUtcTicks);
    input.helper = helper;
    // 처리 스레드가 디스크에 직접 쓴다(heap·loader lock에 막힐 수 있다). 결과에 늘 남긴다.
    input.limitations[input.limitationCount++] = "in-process";
    if (request.skippedReason != nullptr) {
        input.limitations[input.limitationCount++] = request.skippedReason;
    }

    CaptureCrashRecord(inProcessRecord);
    const auto detail = static_cast<CrashDumpDetail>(inProcessRecord.dumpDetail);
    // 덤프 마감은 크래시한 스레드의 마감에서 메타데이터 몫과 여유를 뺀 시각을 넘지 않는다.
    const uint64 now = MonotonicNanoseconds();
    uint64 deadline = now + uint64{request.dumpTimeoutMilliseconds} * NanosecondsPerMillisecond;
    const uint64 reserved =
        uint64{request.metadataMilliseconds + ReportMarginMilliseconds} * NanosecondsPerMillisecond;
    if (request.ownerDeadlineNanoseconds != 0) {
        const uint64 latest = request.ownerDeadlineNanoseconds > reserved
                                  ? request.ownerDeadlineNanoseconds - reserved
                                  : 0;
        deadline = deadline < latest ? deadline : latest;
    }

    const bool canDump = request.writeDump && writeMiniDump != nullptr;
    const bool dumping = canDump && deadline > now;
    if (dumping) {
        input.dump = {"Pending", "InProcess", "Standard", DumpFileText, 0, nullptr};
    } else if (canDump) {
        input.dump = {"Skipped",  "InProcess",
                      "Standard", nullptr,
                      0,          "no time left before the crashing thread ends the process"};
    } else if (request.writeDump) {
        input.dump = {"Failed",
                      nullptr,
                      nullptr,
                      nullptr,
                      static_cast<uint32>(ERROR_MOD_NOT_FOUND),
                      "dbghelp.dll unavailable"};
    } else {
        input.dump = {"Unsupported", nullptr, nullptr, nullptr, 0, request.skippedReason};
    }

    // 예비본: 덤프가 끝나지 못해도 기록이 남는다. 같은 내용을 덤프 사용자 stream에도 넣는다.
    char* buffer = CrashMetadataBuffer();
    usize jsonOffset = 0;
    const usize recordBytes =
        FormatCrashRecordStream(buffer, CrashMetadataBytes, inProcessRecord, input, jsonOffset);
    CrashReportFolder report;
    report.primary = CrashPrimaryFolder();
    report.fallback = fallbackFolder;
    OpenCrashReportFolder(report, buffer + jsonOffset, recordBytes - jsonOffset);
    if (report.current == nullptr) {
        WriteCrashFolderFailure(CrashPrimaryFolderText(), report.primaryError, fallbackFolderText,
                                report.fallbackError);
        return;
    }

    if (dumping) {
        CrashRecordRanges(inProcessRanges);
        FormatCrashComment(inProcessComment, sizeof(inProcessComment), inProcessRecord);
        CrashDumpSource source;
        source.process = GetCurrentProcess();
        source.processId = GetCurrentProcessId();
        source.failingThreadId = static_cast<DWORD>(inProcessRecord.failingThreadId);
        source.pointers = pointers;
        source.ranges = inProcessRanges;
        source.rangeCount = CrashRecordRangeCount;
        // 기본·추가 덤프가 같은 마감을 나눠 쓴다.
        CrashDumpWrite standard =
            WriteCrashDumpFile(report.current, DumpFileName, StandardDumpType, source, buffer,
                               recordBytes, inProcessComment, deadline);
        if (standard.fileError &&
            MoveCrashReportToFallback(report, standard.error, buffer + jsonOffset,
                                      recordBytes - jsonOffset)) {
            standard = WriteCrashDumpFile(report.current, DumpFileName, StandardDumpType, source,
                                          buffer, recordBytes, inProcessComment, deadline);
        }

        input.dump.status = standard.status;
        input.dump.error = standard.error;
        input.dump.file = standard.error == ERROR_SUCCESS ? DumpFileText : nullptr;
        if (detail != CrashDumpDetail::Standard) {
            input.detailDump = {"Skipped", "InProcess", CrashDumpDetailName(detail),
                                nullptr,   0,           "standard dump was not written"};
            if (standard.error == ERROR_SUCCESS && report.current != nullptr) {
                const CrashDumpWrite extra = WriteCrashDumpFile(
                    report.current, DetailDumpFileName, DetailCrashDumpType(detail), source, buffer,
                    recordBytes, inProcessComment, deadline);
                input.detailDump.status = extra.status;
                input.detailDump.error = extra.error;
                input.detailDump.file = extra.error == ERROR_SUCCESS ? DetailDumpFileText : nullptr;
                input.detailDump.reason = nullptr;
            }
        }
    }

    if (report.current == nullptr) {
        WriteCrashFolderFailure(CrashPrimaryFolderText(), report.primaryError, fallbackFolderText,
                                report.fallbackError);
        return;
    }

    input.final = true;
    input.fallbackDirectory = report.fallbackUsed;
    const usize finalBytes =
        FormatCrashMetadata(buffer, CrashMetadataBytes, inProcessRecord, input);
    (void)WriteCrashMetadataFile(report.current, buffer, finalBytes, true);
    // 예비본이 이미 이 폴더에 있다(wroteFile). 최종본 쓰기가 실패해도 예비본은 남는다.
    WriteText("Rocks crash report: ");
    WriteDiagnosticLine(report.fallbackUsed ? fallbackFolderText : CrashPrimaryFolderText());
}

const char* CrashPlatformName() noexcept {
    return "Windows";
}

bool ProcessStartUtcTicks(int64& ticks) noexcept {
    // FILETIME(1601-01-01부터 100ns)을 0001-01-01 기준 tick으로 옮긴다.
    constexpr int64 FileTimeEpochTicks = 504911232000000000;
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
        return TryUtcTicks(ticks);
    }

    const uint64 value =
        (uint64{creation.dwHighDateTime} << (sizeof(DWORD) * BitsPerByte)) | creation.dwLowDateTime;
    ticks = static_cast<int64>(value) + FileTimeEpochTicks;
    return true;
}

} // namespace rocks::platform::detail
