#include "../Common/DirectoryWatchInternal.h"
#include "WindowsFileCommon.h"

#include <Platform/Memory.h>

#include <new>

// Windows backend다. 디렉터리 핸들(FILE_FLAG_OVERLAPPED)에 ReadDirectoryChangesExW(확장 정보로 항목
// 종류를 얻는다, Windows 10 1709+)를 걸고, 없으면 ReadDirectoryChangesW를 쓴다. 완료 여부는
// Poll에서 GetOverlappedResult로 기다리지 않고 확인한다. 커널은 다음 요청 사이의 변경을 첫 요청
// 버퍼 크기만큼 쌓아 두며 넘치면 0 byte 완료(또는 ERROR_NOTIFY_ENUM_DIR)로 알린다 → Overflow.
//
// 루트 삭제는 핸들의 DeletePending, 루트 이동은 열 때의 최종 경로와 현재 최종 경로 비교로 감지한다.
namespace rocks::platform::detail {
namespace {

// 네트워크 공유의 ReadDirectoryChanges 버퍼 상한이 64 KiB다.
constexpr DWORD BufferBytes = 64u * 1024u;
constexpr DWORD NotifyFilter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                               FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE |
                               FILE_NOTIFY_CHANGE_CREATION | FILE_NOTIFY_CHANGE_ATTRIBUTES;
// NTFS 경로 최대 32767 UTF-16 단위 → UTF-8 최대 3배.
constexpr usize NameScratchBytes = 32768u * 3u + 4u;
constexpr DWORD FinalPathUnits = 32768u;

using ReadChangesExFn = BOOL(WINAPI*)(HANDLE, LPVOID, DWORD, BOOL, DWORD, LPDWORD, LPOVERLAPPED,
                                      LPOVERLAPPED_COMPLETION_ROUTINE,
                                      READ_DIRECTORY_NOTIFY_INFORMATION_CLASS);

} // namespace

struct WatchBackend {
    HANDLE directory = INVALID_HANDLE_VALUE;
    OVERLAPPED overlapped{};
    ReadChangesExFn readEx = nullptr;
    bool pending = false;
    bool rootRemoved = false;
    // 열 때의 최종 경로(이동 감지용)다.
    wchar_t* finalPath = nullptr;
    wchar_t* finalScratch = nullptr;
    wchar_t* longName = nullptr;
    char* names = nullptr;
    char* renameOld = nullptr;
    usize renameOldBytes = 0;
    DirectoryEntryKind renameEntry = DirectoryEntryKind::Unknown;
    bool renamePending = false;
    alignas(8) uint8 buffer[BufferBytes];
};

namespace {

void FreeBackend(WatchBackend* backend) noexcept {
    if (backend == nullptr) {
        return;
    }
    if (backend->overlapped.hEvent != nullptr) {
        CloseHandle(backend->overlapped.hEvent);
    }
    if (backend->directory != INVALID_HANDLE_VALUE) {
        CloseHandle(backend->directory);
    }
    AlignedFree(backend->finalPath);
    AlignedFree(backend->finalScratch);
    AlignedFree(backend->longName);
    AlignedFree(backend->names);
    AlignedFree(backend->renameOld);
    backend->~WatchBackend();
    AlignedFree(backend);
}

// 감시 요청을 다시 건다. 확장 API가 이 볼륨에서 지원되지 않으면 일반 API로 내려간다.
FileStatus Issue(WatchBackend& backend) noexcept {
    ResetEvent(backend.overlapped.hEvent);
    if (backend.readEx != nullptr) {
        if (backend.readEx(backend.directory, backend.buffer, BufferBytes, TRUE, NotifyFilter,
                           nullptr, &backend.overlapped, nullptr,
                           ReadDirectoryNotifyExtendedInformation)) {
            backend.pending = true;
            return {};
        }
        const DWORD error = GetLastError();
        if (error != ERROR_INVALID_FUNCTION && error != ERROR_INVALID_PARAMETER &&
            error != ERROR_NOT_SUPPORTED) {
            return NativeError(error);
        }
        backend.readEx = nullptr;
    }
    if (!ReadDirectoryChangesW(backend.directory, backend.buffer, BufferBytes, TRUE, NotifyFilter,
                               nullptr, &backend.overlapped, nullptr)) {
        return NativeError(GetLastError());
    }
    backend.pending = true;
    return {};
}

// 루트가 지워졌거나(삭제 대기 포함) 다른 곳으로 옮겨졌는지다.
bool RootGone(WatchBackend& backend) noexcept {
    FILE_STANDARD_INFO standard{};
    if (!GetFileInformationByHandleEx(backend.directory, FileStandardInfo, &standard,
                                      sizeof(standard))) {
        return true;
    }
    if (standard.DeletePending) {
        return true;
    }
    const DWORD units = GetFinalPathNameByHandleW(backend.directory, backend.finalScratch,
                                                  FinalPathUnits, FILE_NAME_NORMALIZED);
    if (units == 0 || units >= FinalPathUnits) {
        return true;
    }
    return CompareStringOrdinal(backend.finalScratch, static_cast<int>(units), backend.finalPath,
                                -1, TRUE) != CSTR_EQUAL;
}

// 8.3 짧은 이름(구성 요소에 '~')이 섞인 상대 경로를 긴 이름으로 바꾼다. 항목이 이미 없어 바꿀 수
// 없으면 false다.
bool ExpandShortName(WatchBackend& backend, const wchar_t* name, DWORD units, wchar_t* out,
                     DWORD& outUnits) noexcept {
    // finalPath는 "\\?\C:\..." 형식이다. 루트 + '\' + 상대 경로를 만든다.
    const DWORD rootUnits = static_cast<DWORD>(lstrlenW(backend.finalPath));
    if (rootUnits + 1u + units + 1u > FinalPathUnits) {
        return false;
    }
    platform::MemoryCopy(backend.finalScratch, backend.finalPath, rootUnits * sizeof(wchar_t));
    backend.finalScratch[rootUnits] = L'\\';
    platform::MemoryCopy(backend.finalScratch + rootUnits + 1u, name, units * sizeof(wchar_t));
    backend.finalScratch[rootUnits + 1u + units] = L'\0';
    const DWORD longUnits = GetLongPathNameW(backend.finalScratch, out, FinalPathUnits);
    if (longUnits == 0 || longUnits >= FinalPathUnits || longUnits <= rootUnits + 1u) {
        return false;
    }
    // 앞의 루트 부분을 떼어 상대 경로만 남긴다.
    outUnits = longUnits - rootUnits - 1u;
    platform::MemoryMove(out, out + rootUnits + 1u, outUnits * sizeof(wchar_t));
    out[outUnits] = L'\0';
    return true;
}

// 구성 요소가 OS가 만든 8.3 짧은 이름 모양인지다: 이름 부분 1–8자가 "~숫자"로 끝나고 확장자가
// 0–3자다(예: "PROGRA~1", "LONGDI~1.TXT"). "~$doc.docx"·"foo~"·"~WRL0001.tmp" 같은 실제 이름은
// 해당하지 않는다.
bool IsShortNameShape(const wchar_t* text, DWORD units) noexcept {
    DWORD dot = units;
    for (DWORD index = 0; index < units; ++index) {
        if (text[index] == L'.') {
            dot = index;
        }
    }
    const DWORD baseUnits = dot;
    const DWORD extensionUnits = dot < units ? units - dot - 1u : 0u;
    if (baseUnits == 0 || baseUnits > 8u || extensionUnits > 3u) {
        return false;
    }
    DWORD digits = 0;
    while (digits < baseUnits && text[baseUnits - 1u - digits] >= L'0' &&
           text[baseUnits - 1u - digits] <= L'9') {
        ++digits;
    }
    return digits > 0 && digits < baseUnits && text[baseUnits - 1u - digits] == L'~';
}

// 상대 경로에 8.3 모양 구성 요소가 있는지다. leafOnly는 마지막 구성 요소만 그런지다.
bool HasShortComponent(const wchar_t* path, DWORD units, bool& leafOnly) noexcept {
    bool any = false;
    bool parent = false;
    DWORD start = 0;
    for (DWORD index = 0; index <= units; ++index) {
        if (index == units || path[index] == L'\\') {
            if (IsShortNameShape(path + start, index - start)) {
                any = true;
                parent = parent || index != units;
            }
            start = index + 1u;
        }
    }
    leafOnly = any && !parent;
    return any;
}

enum class NameOutcome : uint8 {
    // names에 이벤트 경로가 있다.
    Converted,
    // 긴 이름을 알 수 없다. names에 다시 검색할 부모 경로가 있다.
    ParentOverflow,
    // UTF-16이 잘못됐다.
    Invalid
};

// UTF-16 상대 경로를 '/' 구분 UTF-8로 바꿔 names 버퍼에 쓴다. 8.3 짧은 이름은 긴 이름으로 바꾼다.
// 항목이 이미 없어 바꿀 수 없으면 부모만 바꿔 원래 이벤트로 알리고, 부모도 바꿀 수 없으면 부모
// 재검색(ParentOverflow)을 요청한다.
NameOutcome ConvertName(WatchBackend& backend, const wchar_t* name, DWORD nameBytes,
                        usize& bytes) noexcept {
    DWORD units = nameBytes / sizeof(wchar_t);
    bool leafOnly = false;
    bool parentOverflow = false;
    if (HasShortComponent(name, units, leafOnly)) {
        DWORD longUnits = 0;
        if (ExpandShortName(backend, name, units, backend.longName, longUnits)) {
            name = backend.longName;
            units = longUnits;
        } else {
            DWORD parent = units;
            while (parent > 0 && name[parent - 1u] != L'\\') {
                --parent;
            }
            const DWORD parentUnits = parent > 0 ? parent - 1u : 0u;
            DWORD longParent = 0;
            if (leafOnly) {
                // 리프만 짧은 이름이고 이미 사라졌다. 이름 그대로 알린다(소비자가 다시 Stat한다).
            } else if (parentUnits != 0 &&
                       ExpandShortName(backend, name, parentUnits, backend.longName, longParent) &&
                       longParent + 1u + (units - parent) < FinalPathUnits) {
                backend.longName[longParent] = L'\\';
                MemoryCopy(backend.longName + longParent + 1u, name + parent,
                           (units - parent) * sizeof(wchar_t));
                units = longParent + 1u + (units - parent);
                backend.longName[units] = L'\0';
                name = backend.longName;
            } else {
                units = parentUnits;
                parentOverflow = true;
            }
        }
    }
    if (!WideToUtf8(name, static_cast<int>(units), backend.names, NameScratchBytes, bytes)) {
        return NameOutcome::Invalid;
    }
    for (usize index = 0; index < bytes; ++index) {
        if (backend.names[index] == '\\') {
            backend.names[index] = '/';
        }
    }
    return parentOverflow ? NameOutcome::ParentOverflow : NameOutcome::Converted;
}

void FlushRename(WatchBackend& backend, WatchEventQueue& out) noexcept {
    if (backend.renamePending) {
        out.Push(DirectoryChangeKind::Removed, backend.renameEntry, backend.renameOld,
                 backend.renameOldBytes);
        backend.renamePending = false;
    }
}

void Parse(WatchBackend& backend, DWORD bytes, bool extended, WatchEventQueue& out) noexcept {
    usize offset = 0;
    for (;;) {
        DWORD action = 0;
        DWORD attributes = 0;
        const wchar_t* name = nullptr;
        DWORD nameBytes = 0;
        DWORD next = 0;
        if (extended) {
            if (offset + sizeof(FILE_NOTIFY_EXTENDED_INFORMATION) > bytes) {
                break;
            }
            const auto* info =
                reinterpret_cast<const FILE_NOTIFY_EXTENDED_INFORMATION*>(backend.buffer + offset);
            action = info->Action;
            attributes = info->FileAttributes;
            name = info->FileName;
            nameBytes = info->FileNameLength;
            next = info->NextEntryOffset;
            if (reinterpret_cast<const uint8*>(name) + nameBytes > backend.buffer + bytes) {
                break;
            }
        } else {
            if (offset + sizeof(FILE_NOTIFY_INFORMATION) > bytes) {
                break;
            }
            const auto* info =
                reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(backend.buffer + offset);
            action = info->Action;
            name = info->FileName;
            nameBytes = info->FileNameLength;
            next = info->NextEntryOffset;
            if (reinterpret_cast<const uint8*>(name) + nameBytes > backend.buffer + bytes) {
                break;
            }
        }
        DirectoryEntryKind entry = DirectoryEntryKind::Unknown;
        if (extended && action != FILE_ACTION_REMOVED && action != FILE_ACTION_RENAMED_OLD_NAME) {
            entry = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ? DirectoryEntryKind::Directory
                                                                 : DirectoryEntryKind::File;
        }
        usize converted = 0;
        const NameOutcome outcome = ConvertName(backend, name, nameBytes, converted);
        if (outcome == NameOutcome::Invalid) {
            // 변환할 수 없는 이름(잘못된 UTF-16)은 경로를 알릴 수 없으므로 재검색을 요청한다.
            out.Push(DirectoryChangeKind::Overflow, DirectoryEntryKind::Unknown, "", 0);
        } else if (outcome == NameOutcome::ParentOverflow) {
            // 긴 이름을 알 수 없는 짧은 이름이다. 부모 디렉터리를 다시 검색하게 한다.
            out.Push(DirectoryChangeKind::Overflow, DirectoryEntryKind::Directory, backend.names,
                     converted);
        } else if (action == FILE_ACTION_RENAMED_OLD_NAME) {
            FlushRename(backend, out);
            void* storage = AlignedAllocate(converted + 1u, 1);
            if (storage == nullptr) {
                out.Push(DirectoryChangeKind::Overflow, DirectoryEntryKind::Unknown, "", 0);
            } else {
                AlignedFree(backend.renameOld);
                backend.renameOld = static_cast<char*>(storage);
                platform::MemoryCopy(backend.renameOld, backend.names, converted + 1u);
                backend.renameOldBytes = converted;
                backend.renameEntry = entry;
                backend.renamePending = true;
            }
        } else if (action == FILE_ACTION_RENAMED_NEW_NAME) {
            if (backend.renamePending) {
                out.Push(DirectoryChangeKind::Renamed, entry, backend.names, converted,
                         backend.renameOld, backend.renameOldBytes);
                backend.renamePending = false;
            } else {
                out.Push(DirectoryChangeKind::Added, entry, backend.names, converted);
            }
        } else {
            FlushRename(backend, out);
            switch (action) {
            case FILE_ACTION_ADDED:
                out.Push(DirectoryChangeKind::Added, entry, backend.names, converted);
                break;
            case FILE_ACTION_REMOVED:
                out.Push(DirectoryChangeKind::Removed, entry, backend.names, converted);
                break;
            case FILE_ACTION_MODIFIED:
                // 디렉터리의 MODIFIED는 자식 변경에 따른 시각 갱신이라 따로 알리지 않는다.
                if (entry != DirectoryEntryKind::Directory) {
                    out.Push(DirectoryChangeKind::Modified, entry, backend.names, converted);
                }
                break;
            default:
                break;
            }
        }
        if (next == 0) {
            break;
        }
        offset += next;
    }
    // 짝 없는 이전 이름은 버퍼 경계를 넘지 않는다고 보고 Removed로 보낸다(계약상 허용).
    FlushRename(backend, out);
}

} // namespace

FileStatus WatchOpen(const char* path, usize pathBytes, WatchBackend*& backend) noexcept {
    WidePath wide;
    if (const FileStatus converted = wide.Initialize(path, pathBytes); !converted) {
        return converted;
    }
    const DWORD attributes = GetFileAttributesW(wide.Value());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return NativeError(GetLastError());
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return {FileCode::WrongType, 0};
    }
    void* memory = AlignedAllocate(sizeof(WatchBackend), alignof(WatchBackend));
    if (memory == nullptr) {
        return {FileCode::OutOfMemory, 0};
    }
    auto* created = ::new (memory) WatchBackend();
    created->finalPath =
        static_cast<wchar_t*>(AlignedAllocate(FinalPathUnits * sizeof(wchar_t), alignof(wchar_t)));
    created->finalScratch =
        static_cast<wchar_t*>(AlignedAllocate(FinalPathUnits * sizeof(wchar_t), alignof(wchar_t)));
    created->longName =
        static_cast<wchar_t*>(AlignedAllocate(FinalPathUnits * sizeof(wchar_t), alignof(wchar_t)));
    created->names = static_cast<char*>(AlignedAllocate(NameScratchBytes, 1));
    if (created->finalPath == nullptr || created->finalScratch == nullptr ||
        created->longName == nullptr || created->names == nullptr) {
        FreeBackend(created);
        return {FileCode::OutOfMemory, 0};
    }
    created->directory = CreateFileW(
        wide.Value(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (created->directory == INVALID_HANDLE_VALUE) {
        const FileStatus failed = NativeError(GetLastError());
        FreeBackend(created);
        return failed;
    }
    const DWORD units = GetFinalPathNameByHandleW(created->directory, created->finalPath,
                                                  FinalPathUnits, FILE_NAME_NORMALIZED);
    if (units == 0 || units >= FinalPathUnits) {
        const FileStatus failed =
            units == 0 ? NativeError(GetLastError()) : FileStatus{FileCode::Overflow, 0};
        FreeBackend(created);
        return failed;
    }
    created->overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (created->overlapped.hEvent == nullptr) {
        const FileStatus failed = NativeError(GetLastError());
        FreeBackend(created);
        return failed;
    }
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    created->readEx = kernel != nullptr ? reinterpret_cast<ReadChangesExFn>(reinterpret_cast<void*>(
                                              GetProcAddress(kernel, "ReadDirectoryChangesExW")))
                                        : nullptr;
    if (const FileStatus issued = Issue(*created); !issued) {
        FreeBackend(created);
        return issued;
    }
    backend = created;
    return {};
}

FileStatus WatchCollect(WatchBackend& backend, WatchEventQueue& out) noexcept {
    // 완료된 요청을 모두 비운다(한 번에 너무 오래 붙잡지 않도록 상한을 둔다).
    for (int round = 0; round < 64 && !backend.rootRemoved; ++round) {
        if (!backend.pending) {
            if (const FileStatus issued = Issue(backend); !issued) {
                if (RootGone(backend)) {
                    out.Push(DirectoryChangeKind::RootRemoved, DirectoryEntryKind::Directory, "",
                             0);
                    backend.rootRemoved = true;
                    return {};
                }
                // 감시가 끊긴 동안의 변경을 알 수 없다. 다음 Poll에서 다시 건다.
                out.Push(DirectoryChangeKind::Overflow, DirectoryEntryKind::Unknown, "", 0);
                return issued;
            }
        }
        DWORD bytes = 0;
        const bool extended = backend.readEx != nullptr;
        if (!GetOverlappedResult(backend.directory, &backend.overlapped, &bytes, FALSE)) {
            const DWORD error = GetLastError();
            if (error == ERROR_IO_INCOMPLETE) {
                break;
            }
            backend.pending = false;
            if (RootGone(backend)) {
                out.Push(DirectoryChangeKind::RootRemoved, DirectoryEntryKind::Directory, "", 0);
                backend.rootRemoved = true;
                return {};
            }
            // ERROR_NOTIFY_ENUM_DIR(커널 버퍼 넘침) 등: 놓친 변경이 있다.
            out.Push(DirectoryChangeKind::Overflow, DirectoryEntryKind::Unknown, "", 0);
            continue;
        }
        backend.pending = false;
        if (bytes == 0) {
            if (RootGone(backend)) {
                out.Push(DirectoryChangeKind::RootRemoved, DirectoryEntryKind::Directory, "", 0);
                backend.rootRemoved = true;
                return {};
            }
            out.Push(DirectoryChangeKind::Overflow, DirectoryEntryKind::Unknown, "", 0);
            continue;
        }
        Parse(backend, bytes, extended, out);
    }
    if (!backend.rootRemoved && RootGone(backend)) {
        out.Push(DirectoryChangeKind::RootRemoved, DirectoryEntryKind::Directory, "", 0);
        backend.rootRemoved = true;
    }
    return {};
}

bool WatchWait(WatchBackend& backend, uint32 timeoutMilliseconds) noexcept {
    if (backend.rootRemoved) {
        return false;
    }
    if (!backend.pending) {
        if (!Issue(backend)) {
            return true; // Poll이 오류·루트 삭제를 보고하게 한다.
        }
    }
    // 루트 이름 변경은 대기 중인 요청을 완료시키지 않는다. 짧은 조각으로 나눠 루트 상태를 확인한다.
    uint32 remaining = timeoutMilliseconds;
    for (;;) {
        const uint32 slice = remaining < 250u ? remaining : 250u;
        if (WaitForSingleObject(backend.overlapped.hEvent, slice) == WAIT_OBJECT_0) {
            return true;
        }
        if (RootGone(backend)) {
            return true; // Poll이 RootRemoved를 보고한다.
        }
        if (remaining <= slice) {
            return false;
        }
        remaining -= slice;
    }
}

void WatchClose(WatchBackend* backend) noexcept {
    if (backend == nullptr) {
        return;
    }
    if (backend->pending) {
        // 커널이 버퍼를 더 쓰지 않을 때까지 기다린 뒤 해제한다.
        CancelIoEx(backend->directory, &backend->overlapped);
        DWORD bytes = 0;
        GetOverlappedResult(backend->directory, &backend->overlapped, &bytes, TRUE);
        backend->pending = false;
    }
    FreeBackend(backend);
}

} // namespace rocks::platform::detail
