#pragma once
namespace rocks::test {
enum class FileLinkResult {
    Created,
    Unavailable,
    Failed
};
FileLinkResult CreateFileLink(const char* link, const char* target) noexcept;
// 디렉터리를 가리키는 link를 만든다(Windows는 디렉터리 symbolic link, POSIX는 symlink).
FileLinkResult CreateDirectoryLink(const char* link, const char* target) noexcept;
} // namespace rocks::test
