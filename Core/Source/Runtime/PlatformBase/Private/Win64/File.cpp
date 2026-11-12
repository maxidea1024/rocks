#include <Platform/File.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>
#include "../Common/FileInternal.h"
#include "FileType.h"
#include "WindowsFileCommon.h"
#include "WindowsPathGuard.h"
#include <Windows.h>
#include <new>

namespace rocks::platform {
namespace {

using detail::NativeError;
using detail::WidePath;

void ClearFileTypeError() noexcept {
    SetLastError(ERROR_SUCCESS);
}

uint32 ReadFileType(void* handle) noexcept {
    return static_cast<uint32>(GetFileType(handle));
}

uint32 ReadFileTypeError() noexcept {
    return static_cast<uint32>(GetLastError());
}

constexpr detail::FileTypeOperations fileTypeOps{ClearFileTypeError, ReadFileType,
                                                 ReadFileTypeError};
static_assert(FILE_TYPE_UNKNOWN == 0 && ERROR_SUCCESS == 0);

class WindowsFileHandle final : public FileHandle {
public:
    explicit WindowsFileHandle(HANDLE handle) noexcept : handle_(handle) {
    }
    FileTransfer Read(void* destination, usize bytes) noexcept override {
        if (bytes == 0) {
            return {{}, 0};
        }
        const DWORD amount = bytes > 0x7ffff000U ? 0x7ffff000U : static_cast<DWORD>(bytes);
        DWORD done = 0;
        if (!ReadFile(handle_, destination, amount, &done, nullptr)) {
            return {NativeError(GetLastError()), done};
        }
        return {{}, done};
    }
    FileTransfer Write(const void* source, usize bytes) noexcept override {
        if (bytes == 0) {
            return {{}, 0};
        }
        const DWORD amount = bytes > 0x7ffff000U ? 0x7ffff000U : static_cast<DWORD>(bytes);
        DWORD done = 0;
        if (!WriteFile(handle_, source, amount, &done, nullptr)) {
            return {NativeError(GetLastError()), done};
        }
        return {{}, done};
    }
    FileStatus Seek(int64 offset, FileSeekOrigin origin, uint64& position) noexcept override {
        DWORD method = FILE_BEGIN;
        if (origin == FileSeekOrigin::Current) {
            method = FILE_CURRENT;
        } else if (origin == FileSeekOrigin::End) {
            method = FILE_END;
        } else if (origin != FileSeekOrigin::Begin) {
            return {FileCode::InvalidArgument, 0};
        }
        LARGE_INTEGER distance{};
        LARGE_INTEGER result{};
        distance.QuadPart = offset;
        if (!SetFilePointerEx(handle_, distance, &result, method)) {
            return NativeError(GetLastError());
        }
        if (result.QuadPart < 0) {
            return {FileCode::SystemError, 0};
        }
        position = static_cast<uint64>(result.QuadPart);
        return {};
    }
    FileStatus Size(uint64& bytes) noexcept override {
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(handle_, &size)) {
            return NativeError(GetLastError());
        }
        if (size.QuadPart < 0) {
            return {FileCode::SystemError, 0};
        }
        bytes = static_cast<uint64>(size.QuadPart);
        return {};
    }
    FileStatus Close() noexcept override {
        HANDLE value = handle_;
        handle_ = INVALID_HANDLE_VALUE;
        if (!CloseHandle(value)) {
            return NativeError(GetLastError());
        }
        return {};
    }
    void Destroy() noexcept override {
        this->~WindowsFileHandle();
        AlignedFree(this);
    }

private:
    HANDLE handle_;
};

class WindowsFileSystem final : public FileSystem {
public:
    FileStatus Open(const char* path, usize bytes, FileOpenMode mode,
                    FileHandle*& out) noexcept override {
        if (out != nullptr) {
            return {FileCode::InvalidArgument, 0};
        }
        DWORD access = GENERIC_READ;
        DWORD disposition = OPEN_EXISTING;
        switch (mode) {
        case FileOpenMode::ReadExisting:
            break;
        case FileOpenMode::ReadWriteExisting:
            access |= GENERIC_WRITE;
            break;
        case FileOpenMode::CreateNew:
            access |= GENERIC_WRITE;
            disposition = CREATE_NEW;
            break;
        case FileOpenMode::CreateAlways:
            access |= GENERIC_WRITE;
            disposition = CREATE_ALWAYS;
            break;
        default:
            return {FileCode::InvalidArgument, 0};
        }
        WidePath native;
        auto valid = native.Initialize(path, bytes);
        if (!valid) {
            return valid;
        }
        void* storage = AlignedAllocate(sizeof(WindowsFileHandle), alignof(WindowsFileHandle));
        if (!storage) {
            return {FileCode::OutOfMemory, 0};
        }
        HANDLE handle = CreateFileW(native.Value(), access,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                    disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            const auto failure = NativeError(GetLastError());
            AlignedFree(storage);
            return failure;
        }
        // FILE_TYPE_UNKNOWN은 유효한 unknown과 OS 실패 모두를 나타낸다. 마지막 오류를
        // 초기화한 다음 즉시 캡처해야 이전 API의 오류를 잘못 전파하지 않는다.
        const auto fileType = detail::QueryFileType(handle, fileTypeOps);
        if (fileType.type != FILE_TYPE_DISK) {
            if (!CloseHandle(handle)) {
                Fatal("Windows file open rollback failed");
            }
            AlignedFree(storage);
            return fileType.error != ERROR_SUCCESS ? NativeError(fileType.error)
                                                   : FileStatus{FileCode::Unsupported, 0};
        }
        out = ::new (storage) WindowsFileHandle(handle);
        return {};
    }
    FileStatus Exists(const char* path, usize bytes, bool& exists) noexcept override {
        WidePath native;
        auto valid = native.Initialize(path, bytes);
        if (!valid) {
            return valid;
        }
        // reparse point 자체가 아니라 기본 CreateFile 의미로 연 대상의 유형을 확인한다.
        HANDLE handle = CreateFileW(native.Value(), FILE_READ_ATTRIBUTES,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
                exists = false;
                return {};
            }
            return NativeError(error);
        }
        const auto type = detail::QueryFileType(handle, fileTypeOps);
        FileStatus result{};
        bool found = false;
        if (type.error != ERROR_SUCCESS) {
            result = NativeError(type.error);
        } else if (type.type == FILE_TYPE_DISK) {
            BY_HANDLE_FILE_INFORMATION info{};
            if (!GetFileInformationByHandle(handle, &info)) {
                result = NativeError(GetLastError());
            } else {
                found = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
            }
        }
        if (!CloseHandle(handle)) {
            return NativeError(GetLastError());
        }
        if (result) {
            exists = found;
        }
        return result;
    }
    FileStatus Delete(const char* path, usize bytes) noexcept override {
        WidePath native;
        auto valid = native.Initialize(path, bytes);
        if (!valid) {
            return valid;
        }
        if (DeleteFileW(native.Value())) {
            return {};
        }
        const DWORD error = GetLastError();
        // 디렉터리 link는 RemoveDirectoryW로 link 자체를 지운다. 실제 디렉터리는 WrongType이다.
        const DWORD attributes = GetFileAttributesW(native.Value());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            // link(이름 대체 reparse point)만 지운다. 다른 reparse point가 붙은 실제 디렉터리는
            // WrongType이다.
            if (detail::PathIsLink(native.Value())) {
                if (!RemoveDirectoryW(native.Value())) {
                    return NativeError(GetLastError());
                }
                return {};
            }
            return {FileCode::WrongType, static_cast<uint32>(error)};
        }
        return NativeError(error);
    }

    FileStatus Stat(const char* path, usize bytes, FileInfo& info) noexcept override {
        WidePath native;
        auto valid = native.Initialize(path, bytes);
        if (!valid) {
            return valid;
        }
        // 경로 자체(link를 따라가지 않음)로 link 여부를 정한다.
        const bool isLink = detail::PathIsLink(native.Value());
        // 대상 정보는 link를 따라가 연 handle에서 읽는다. 디렉터리도 열 수 있게 backup semantics를
        // 쓴다.
        HANDLE handle = CreateFileW(native.Value(), FILE_READ_ATTRIBUTES,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            return NativeError(GetLastError());
        }
        BY_HANDLE_FILE_INFORMATION data{};
        FileStatus result{};
        FileInfo filled;
        if (!GetFileInformationByHandle(handle, &data)) {
            result = NativeError(GetLastError());
        } else {
            const bool directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (directory) {
                filled.type = FileType::Directory;
            } else {
                const auto type = detail::QueryFileType(handle, fileTypeOps);
                filled.type = type.type == FILE_TYPE_DISK ? FileType::File : FileType::Other;
                if (filled.type == FileType::File) {
                    filled.size =
                        (static_cast<uint64>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
                }
            }
            filled.isLink = isLink;
            filled.readOnly = (data.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0;
            filled.modifiedTicks = detail::FileTimeToTicks(data.ftLastWriteTime);
        }
        if (!CloseHandle(handle)) {
            return NativeError(GetLastError());
        }
        if (result) {
            info = filled;
        }
        return result;
    }

    FileStatus MakeDirectory(const char* path, usize bytes) noexcept override {
        WidePath native;
        auto valid = native.Initialize(path, bytes);
        if (!valid) {
            return valid;
        }
        if (!CreateDirectoryW(native.Value(), nullptr)) {
            return NativeError(GetLastError());
        }
        return {};
    }

    FileStatus DeleteDirectory(const char* path, usize bytes) noexcept override {
        WidePath native;
        auto valid = native.Initialize(path, bytes);
        if (!valid) {
            return valid;
        }
        if (!RemoveDirectoryW(native.Value())) {
            return NativeError(GetLastError());
        }
        return {};
    }

    FileStatus EnumerateDirectory(const char* path, usize bytes, DirectoryVisitor visitor,
                                  void* context) noexcept override {
        if (visitor == nullptr) {
            return {FileCode::InvalidArgument, 0};
        }
        WidePath native;
        auto valid = native.Initialize(path, bytes);
        if (!valid) {
            return valid;
        }
        const DWORD attributes = GetFileAttributesW(native.Value());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            return NativeError(GetLastError());
        }
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            return {FileCode::WrongType, 0};
        }
        // "<path>\*" 패턴을 만든다.
        usize units = 0;
        while (native.Value()[units] != L'\0') {
            ++units;
        }
        // "C:"(드라이브 상대)는 "C:*"로 그 드라이브의 현재 디렉터리를 연다.
        const bool driveRelative = units == 2 && native.Value()[1] == L':';
        const bool hasSeparator =
            driveRelative || (units != 0 && (native.Value()[units - 1] == L'\\' ||
                                             native.Value()[units - 1] == L'/'));
        const usize patternUnits = units + (hasSeparator ? 1 : 2);
        auto* pattern = static_cast<wchar_t*>(
            AlignedAllocate((patternUnits + 1) * sizeof(wchar_t), alignof(wchar_t)));
        if (pattern == nullptr) {
            return {FileCode::OutOfMemory, 0};
        }
        for (usize index = 0; index < units; ++index) {
            pattern[index] = native.Value()[index];
        }
        usize cursor = units;
        if (!hasSeparator) {
            pattern[cursor++] = L'\\';
        }
        pattern[cursor++] = L'*';
        pattern[cursor] = L'\0';
        WIN32_FIND_DATAW data{};
        HANDLE find = FindFirstFileExW(pattern, FindExInfoBasic, &data, FindExSearchNameMatch,
                                       nullptr, FIND_FIRST_EX_LARGE_FETCH);
        AlignedFree(pattern);
        if (find == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            return error == ERROR_FILE_NOT_FOUND ? FileStatus{} : NativeError(error);
        }
        FileStatus result{};
        for (;;) {
            const wchar_t* name = data.cFileName;
            const bool dot =
                name[0] == L'.' && (name[1] == L'\0' || (name[1] == L'.' && name[2] == L'\0'));
            if (!dot) {
                int nameUnits = 0;
                while (name[nameUnits] != L'\0') {
                    ++nameUnits;
                }
                char utf8[MAX_PATH * 3 + 1];
                DirectoryEntry entry;
                usize nameBytes = 0;
                if (!detail::WideToUtf8(name, nameUnits, utf8, sizeof(utf8), nameBytes)) {
                    result = {FileCode::SystemError, ERROR_NO_UNICODE_TRANSLATION};
                    break;
                }
                entry.name = utf8;
                entry.nameBytes = nameBytes;
                const bool directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                entry.info.type = directory ? FileType::Directory : FileType::File;
                entry.info.isLink = detail::IsLinkReparse(data.dwFileAttributes, data.dwReserved0);
                entry.info.readOnly = (data.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0;
                if (!directory && !entry.info.isLink) {
                    entry.info.size =
                        (static_cast<uint64>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
                }
                entry.info.modifiedTicks = detail::FileTimeToTicks(data.ftLastWriteTime);
                if (!visitor(entry, context)) {
                    break;
                }
            }
            if (!FindNextFileW(find, &data)) {
                const DWORD error = GetLastError();
                if (error != ERROR_NO_MORE_FILES) {
                    result = NativeError(error);
                }
                break;
            }
        }
        if (!FindClose(find)) {
            return NativeError(GetLastError());
        }
        return result;
    }

    FileStatus Move(const char* from, usize fromBytes, const char* to, usize toBytes,
                    bool replaceExisting) noexcept override {
        WidePath source;
        auto valid = source.Initialize(from, fromBytes);
        if (!valid) {
            return valid;
        }
        WidePath target;
        valid = target.Initialize(to, toBytes);
        if (!valid) {
            return valid;
        }
        // 플랫폼 간 결과를 맞춘다: 디렉터리는 기존 대상을 덮어쓰지 않고(AlreadyExists), 파일과
        // 디렉터리가 섞이면 WrongType이다. 대상이 원본과 같은 파일이면(대소문자만 바꾸는 이름
        // 변경 등) OS에 맡긴다.
        const DWORD sourceAttributes = GetFileAttributesW(source.Value());
        if (sourceAttributes == INVALID_FILE_ATTRIBUTES) {
            return NativeError(GetLastError());
        }
        const DWORD targetAttributes = GetFileAttributesW(target.Value());
        if (targetAttributes != INVALID_FILE_ATTRIBUTES &&
            !detail::SameFileObject(source.Value(), target.Value())) {
            // link는 POSIX lstat처럼 link 자체(디렉터리 아님)로 본다.
            const bool sourceDirectory = (sourceAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
                                         !detail::PathIsLink(source.Value());
            const bool targetDirectory = (targetAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
                                         !detail::PathIsLink(target.Value());
            if (sourceDirectory != targetDirectory) {
                return {FileCode::WrongType, 0};
            }
            if (sourceDirectory || !replaceExisting) {
                return {FileCode::AlreadyExists, ERROR_ALREADY_EXISTS};
            }
        }
        // MOVEFILE_COPY_ALLOWED를 쓰지 않으므로 다른 volume이면
        // ERROR_NOT_SAME_DEVICE(Unsupported)다.
        if (!MoveFileExW(source.Value(), target.Value(),
                         replaceExisting ? MOVEFILE_REPLACE_EXISTING : 0)) {
            return NativeError(GetLastError());
        }
        return {};
    }

    FileStatus Copy(const char* from, usize fromBytes, const char* to, usize toBytes,
                    bool replaceExisting) noexcept override {
        WidePath source;
        auto valid = source.Initialize(from, fromBytes);
        if (!valid) {
            return valid;
        }
        WidePath target;
        valid = target.Initialize(to, toBytes);
        if (!valid) {
            return valid;
        }
        const DWORD attributes = GetFileAttributesW(source.Value());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            return NativeError(GetLastError());
        }
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            return {FileCode::WrongType, 0};
        }
        // 같은 파일 객체(hard link 등)로의 복사는 원본을 지키기 위해 거부한다(POSIX와 같은 결과).
        if (replaceExisting && detail::SameFileObject(source.Value(), target.Value(), true)) {
            return {FileCode::InvalidArgument, 0};
        }
        // CopyFileW는 실패하면 만들던 대상 파일을 지운다.
        if (!CopyFileW(source.Value(), target.Value(), replaceExisting ? FALSE : TRUE)) {
            return NativeError(GetLastError());
        }
        return {};
    }
};
} // namespace

FileSystem& PhysicalFileSystem() noexcept {
    static WindowsFileSystem system;
    return system;
}
} // namespace rocks::platform
