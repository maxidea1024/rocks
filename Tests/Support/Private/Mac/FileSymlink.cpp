#include <Tests/Support/FileLink.h>
#include <errno.h>
#include <unistd.h>

namespace rocks::test {
FileLinkResult CreateFileLink(const char* link, const char* target) noexcept {
    if (symlink(target, link) == 0) {
        return FileLinkResult::Created;
    }
    if (errno == EPERM || errno == EACCES || errno == ENOTSUP) {
        return FileLinkResult::Unavailable;
    }
    return FileLinkResult::Failed;
}
} // namespace rocks::test
