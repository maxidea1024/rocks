#include "WindowsFileCommon.h"
#include "WindowsPlatformPath.h"

#include <Platform/Path.h>

#include <KnownFolders.h>
#include <ShlObj.h>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")

namespace rocks::platform {
namespace {

// UTF-16 경로 units개를 UTF-8로 buffer에 쓴다. 끝 구분자는 루트가 아니면 지운다.
FileStatus WriteDirectory(const wchar_t* text, usize units, char* buffer, usize capacity,
                          usize& bytes) noexcept {
    while (units > 3 && (text[units - 1] == L'\\' || text[units - 1] == L'/')) {
        --units;
    }
    if (units > 0x7fffffffu) {
        return {FileCode::Overflow, 0};
    }
    usize written = 0;
    if (!detail::WideToUtf8(text, static_cast<int>(units), buffer, capacity, written)) {
        if (written != 0) {
            bytes = written;
            return {FileCode::Overflow, 0};
        }
        return {FileCode::SystemError, ERROR_NO_UNICODE_TRANSLATION};
    }
    bytes = written;
    return {};
}

FileStatus KnownFolder(const KNOWNFOLDERID& id, char* buffer, usize capacity,
                       usize& bytes) noexcept {
    PWSTR path = nullptr;
    const HRESULT result = SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &path);
    if (FAILED(result)) {
        CoTaskMemFree(path);
        return {FileCode::SystemError, static_cast<uint32>(result)};
    }
    usize units = 0;
    while (path[units] != L'\0') {
        ++units;
    }
    auto status = WriteDirectory(path, units, buffer, capacity, bytes);
    CoTaskMemFree(path);
    return status;
}

} // namespace

PathStyle HostPathStyle() noexcept {
    return WindowsPlatformPath::HostStyle();
}

char NativePathSeparator() noexcept {
    return WindowsPlatformPath::NativeSeparator();
}

FileStatus GetStandardDirectory(StandardDirectory kind, char* buffer, usize capacity,
                                usize& bytes) noexcept {
    if (buffer == nullptr && capacity != 0) {
        return {FileCode::InvalidArgument, 0};
    }
    switch (kind) {
    case StandardDirectory::Executable: {
        // 긴 경로를 위해 버퍼를 키워 가며 실행 파일 경로를 읽고 파일 이름을 뗀다.
        for (DWORD size = MAX_PATH; size <= 32768; size *= 2) {
            auto* path =
                static_cast<wchar_t*>(AlignedAllocate(size * sizeof(wchar_t), alignof(wchar_t)));
            if (path == nullptr) {
                return {FileCode::OutOfMemory, 0};
            }
            const DWORD length = GetModuleFileNameW(nullptr, path, size);
            if (length == 0) {
                const DWORD error = GetLastError();
                AlignedFree(path);
                return detail::NativeError(error);
            }
            if (length < size) {
                usize units = length;
                while (units > 0 && path[units - 1] != L'\\' && path[units - 1] != L'/') {
                    --units;
                }
                auto status = WriteDirectory(path, units, buffer, capacity, bytes);
                AlignedFree(path);
                return status;
            }
            AlignedFree(path);
        }
        return {FileCode::Overflow, 0};
    }
    case StandardDirectory::Working: {
        const DWORD required = GetCurrentDirectoryW(0, nullptr);
        if (required == 0) {
            return detail::NativeError(GetLastError());
        }
        auto* path =
            static_cast<wchar_t*>(AlignedAllocate(required * sizeof(wchar_t), alignof(wchar_t)));
        if (path == nullptr) {
            return {FileCode::OutOfMemory, 0};
        }
        const DWORD length = GetCurrentDirectoryW(required, path);
        if (length == 0 || length >= required) {
            const DWORD error = GetLastError();
            AlignedFree(path);
            return length == 0 ? detail::NativeError(error) : FileStatus{FileCode::Busy, 0};
        }
        auto status = WriteDirectory(path, length, buffer, capacity, bytes);
        AlignedFree(path);
        return status;
    }
    case StandardDirectory::Temporary: {
        wchar_t path[MAX_PATH + 2];
        const DWORD length = GetTempPathW(MAX_PATH + 1, path);
        if (length == 0 || length > MAX_PATH + 1) {
            return detail::NativeError(GetLastError());
        }
        return WriteDirectory(path, length, buffer, capacity, bytes);
    }
    case StandardDirectory::Home:
        return KnownFolder(FOLDERID_Profile, buffer, capacity, bytes);
    case StandardDirectory::UserData:
        return KnownFolder(FOLDERID_RoamingAppData, buffer, capacity, bytes);
    case StandardDirectory::UserCache:
        return KnownFolder(FOLDERID_LocalAppData, buffer, capacity, bytes);
    }
    return {FileCode::InvalidArgument, 0};
}

} // namespace rocks::platform
