#include <TestSupport/FileLink.h>
#include <Windows.h>

namespace rocks::test {
FileLinkResult CreateFileLink(const char* link, const char* target) noexcept {
    wchar_t nativeLink[512]{};
    wchar_t nativeTarget[512]{};
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, link, -1, nativeLink, 512) ||
        !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, target, -1, nativeTarget, 512)) {
        return FileLinkResult::Failed;
    }
    // Windows 10 이후 개발자 모드가 허용되면 관리자 권한 없이 생성한다.
    if (CreateSymbolicLinkW(nativeLink, nativeTarget, 2)) {
        return FileLinkResult::Created;
    }
    const DWORD error = GetLastError();
    if (error == ERROR_PRIVILEGE_NOT_HELD || error == ERROR_NOT_SUPPORTED ||
        error == ERROR_INVALID_PARAMETER || error == ERROR_ACCESS_DENIED) {
        return FileLinkResult::Unavailable;
    }
    return FileLinkResult::Failed;
}

FileLinkResult CreateDirectoryLink(const char* link, const char* target) noexcept {
    wchar_t nativeLink[512]{};
    wchar_t nativeTarget[512]{};
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, link, -1, nativeLink, 512) ||
        !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, target, -1, nativeTarget, 512)) {
        return FileLinkResult::Failed;
    }
    // SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
    if (CreateSymbolicLinkW(nativeLink, nativeTarget, 1 | 2)) {
        return FileLinkResult::Created;
    }
    const DWORD error = GetLastError();
    if (error == ERROR_PRIVILEGE_NOT_HELD || error == ERROR_NOT_SUPPORTED ||
        error == ERROR_INVALID_PARAMETER || error == ERROR_ACCESS_DENIED) {
        return FileLinkResult::Unavailable;
    }
    return FileLinkResult::Failed;
}
} // namespace rocks::test
