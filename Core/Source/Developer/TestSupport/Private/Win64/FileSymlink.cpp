#include <TestSupport/FileLink.h>

#include <Platform/Win64/WideString.h>

#include <Windows.h>
#include <string.h>

namespace rocks::test {

namespace {

// 링크·대상 경로의 UTF-16 버퍼 크기(NUL 포함)다. 시험 임시 디렉터리 경로만 받으므로 긴 경로
// 접두사 없이 이 안에 들어간다.
constexpr usize MaxLinkPathUnits = 512;

bool ToWidePath(const char* path, wchar_t (&wide)[MaxLinkPathUnits]) noexcept {
    return static_cast<bool>(platform::windows::ConvertUtf8ToWide(
        path, strlen(path), wide, MaxLinkPathUnits, platform::windows::TextFit::Exact));
}

} // namespace

FileLinkResult CreateFileLink(const char* link, const char* target) noexcept {
    wchar_t nativeLink[MaxLinkPathUnits]{};
    wchar_t nativeTarget[MaxLinkPathUnits]{};
    if (!ToWidePath(link, nativeLink) || !ToWidePath(target, nativeTarget)) {
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
    wchar_t nativeLink[MaxLinkPathUnits]{};
    wchar_t nativeTarget[MaxLinkPathUnits]{};
    if (!ToWidePath(link, nativeLink) || !ToWidePath(target, nativeTarget)) {
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
