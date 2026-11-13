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

    return detail::WriteUtf8(text, units, buffer, capacity, bytes);
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

FileStatus RegisterApplicationDirectories(const char*, const char*) noexcept {
    return {FileCode::Unsupported, 0};
}

FileStatus GetStandardDirectory(StandardDirectory kind, char* buffer, usize capacity,
                                usize& bytes) noexcept {
    if (buffer == nullptr && capacity != 0) {
        return {FileCode::InvalidArgument, 0};
    }

    switch (kind) {
    case StandardDirectory::Executable: {
        // 실행 파일 경로를 읽고 파일 이름을 뗀다.
        detail::WideString path;
        auto status = path.AssignModuleFileName();
        if (!status) {
            return status;
        }

        usize units = path.Units();
        while (units > 0 && path.Value()[units - 1] != L'\\' && path.Value()[units - 1] != L'/') {
            --units;
        }

        return WriteDirectory(path.Value(), units, buffer, capacity, bytes);
    }
    case StandardDirectory::Working: {
        // 다른 스레드가 작업 디렉터리를 바꾸면 길이 조회와 읽기 사이에 커질 수 있으므로 다시
        // 시도한다.
        for (int attempt = 0; attempt < 8; ++attempt) {
            const DWORD required = GetCurrentDirectoryW(0, nullptr);
            if (required == 0) {
                return detail::NativeError(GetLastError());
            }

            auto* path = static_cast<wchar_t*>(
                AlignedAllocate(required * sizeof(wchar_t), alignof(wchar_t)));
            if (path == nullptr) {
                return {FileCode::OutOfMemory, 0};
            }

            const DWORD length = GetCurrentDirectoryW(required, path);
            if (length == 0) {
                const DWORD error = GetLastError();
                AlignedFree(path);
                return detail::NativeError(error);
            }

            if (length < required) {
                auto status = WriteDirectory(path, length, buffer, capacity, bytes);
                AlignedFree(path);
                return status;
            }

            AlignedFree(path);
        }

        return {FileCode::Busy, 0};
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
