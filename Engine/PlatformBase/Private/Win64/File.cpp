#include <Platform/File.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>
#include "../Common/FileInternal.h"
#include "FileType.h"
#include "WindowsPathGuard.h"
#include <Windows.h>
#include <new>

namespace rocks::platform {
namespace {

FileStatus NativeError(DWORD error) noexcept {
    FileCode code = FileCode::SystemError;
    switch (error) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        code = FileCode::NotFound;
        break;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
        code = FileCode::AlreadyExists;
        break;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
        code = FileCode::AccessDenied;
        break;
    case ERROR_INVALID_NAME:
    case ERROR_INVALID_PARAMETER:
        code = FileCode::InvalidArgument;
        break;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
        code = FileCode::OutOfMemory;
        break;
    case ERROR_NOT_SUPPORTED:
        code = FileCode::Unsupported;
        break;
    }
    return {code, static_cast<uint32>(error)};
}

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

class WidePath {
public:
    ~WidePath() noexcept {
        AlignedFree(value_);
    }
    FileStatus Initialize(const char* path, usize bytes) noexcept {
        if (!detail::ValidFilePath(path, bytes)) {
            return {FileCode::InvalidArgument, 0};
        }
        // 일반 파일 인터페이스에서 device namespace나 레거시 DOS 장치를 열지 않는다.
        if (detail::IsWindowsDevicePath(path, bytes)) {
            return {FileCode::Unsupported, 0};
        }
        if (bytes > 0x7fffffffU) {
            return {FileCode::Overflow, 0};
        }
        const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path,
                                                 static_cast<int>(bytes), nullptr, 0);
        if (required == 0) {
            return NativeError(GetLastError());
        }
        const usize units = static_cast<usize>(required) + 1;
        if (units > MaxSize / sizeof(wchar_t)) {
            return {FileCode::Overflow, 0};
        }
        value_ = static_cast<wchar_t*>(AlignedAllocate(units * sizeof(wchar_t), alignof(wchar_t)));
        if (!value_) {
            return {FileCode::OutOfMemory, 0};
        }
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, static_cast<int>(bytes),
                                value_, required) != required) {
            return NativeError(GetLastError());
        }
        value_[required] = L'\0';
        return {};
    }
    const wchar_t* Value() const noexcept {
        return value_;
    }

private:
    wchar_t* value_ = nullptr;
};

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
        if (!DeleteFileW(native.Value())) {
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
