#pragma once
#include <Platform/File.h>

namespace rocks::platform::detail {
// 경로로 전달할 UTF-8 scalar 및 NUL을 검사한다. 파일명 규칙이나 접근 권한은 검사하지 않는다.
bool ValidFilePath(const char* path, usize bytes) noexcept;

// ---- FileLock 플랫폼 hook ----
// 잠금 파일을 연다(없으면 만든다). 성공하면 handle을 소유한다.
FileStatus LockFileOpen(const char* path, usize bytes, uptr& handle) noexcept;
// 배타 잠금을 기다리지 않고 시도한다. 다른 쪽이 잡고 있으면 acquired = false로 성공한다.
FileStatus LockFileTry(uptr handle, bool& acquired) noexcept;
// 잠금을 풀고 닫는다.
void LockFileClose(uptr handle, bool held) noexcept;
// 잠근 handle이 아직 path의 파일인지 확인한다. 다른 쪽이 잠금 파일을 지우거나 바꿨으면 current가
// false이며 호출자는 다시 연다(Windows는 삭제 공유 없이 열어 늘 true다).
FileStatus LockFileIsCurrent(const char* path, usize bytes, uptr handle, bool& current) noexcept;
} // namespace rocks::platform::detail
