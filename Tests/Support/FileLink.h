#pragma once
namespace rocks::test {
enum class FileLinkResult {
    Created,
    Unavailable,
    Failed
};
FileLinkResult CreateFileLink(const char* link, const char* target) noexcept;
} // namespace rocks::test
