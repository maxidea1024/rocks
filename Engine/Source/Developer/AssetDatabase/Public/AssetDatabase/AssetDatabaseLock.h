#pragma once

#include <AssetDatabase/ProjectLayout.h>
#include <Core/Encoding/Sha256.h>
#include <Platform/File.h>

// 프로젝트 쓰기 잠금이다(Docs/Design/AssetSystem.ko.md 8절, 7.1절 4 — A2a). 이 잠금을 쥔 프로세스만
// 에셋·별칭 표·색인 캐시·쿠킹 출력을 바꾼다. 쓰기 API는 잠금 객체를 인자로 받아 잠금 없는 쓰기를
// 타입으로 막는다. 잠금 파일은 `<Intermediate>/AssetDatabase.lock`이고 Intermediate가 없으면
// 만든다. PlatformBase FileLock(Windows LockFileEx + 삭제 공유 없는 열기, POSIX flock + 잠근 뒤
// dev/ino 재확인) 위에 있다 — 프로세스 사이의 권고 잠금이다.
//
// 사용 예:
//   AssetDatabaseLock lock;
//   if (auto held = lock.Acquire(layout, waitMilliseconds); !held) {
//       // Busy: 다른 프로세스가 쓰는 중(13절 --wait로 기다릴 수 있다)
//   }
//   SaveSomething(lock, ...);   // 소멸자가 놓는다
//
// 스레드: 한 객체를 여러 스레드가 동시에 쓰지 않는다.
namespace rocks {

// 잠금 파일 이름이다(Intermediate 아래).
inline constexpr StringView AssetDatabaseLockFileName = "AssetDatabase.lock";

class AssetDatabaseLock final {
public:
    AssetDatabaseLock() noexcept = default;
    ~AssetDatabaseLock() noexcept = default;

    AssetDatabaseLock(const AssetDatabaseLock&) = delete;
    AssetDatabaseLock& operator=(const AssetDatabaseLock&) = delete;

    // layout의 잠금을 잡는다. 다른 쪽이 잡고 있으면 waitMilliseconds까지 다시 시도한다(0이면 한
    // 번). 오류: 못 잡으면 Busy, 이 객체가 이미 잡고 있으면 AlreadyExists, Intermediate를 만들 수
    // 없거나 잠금 파일 오류는 그 파일 오류(FileErrorCode), 경로가 너무 길면 CapacityExceeded,
    // Initialize 전 배치는 InvalidArgument.
    Result<void> Acquire(const ProjectLayout& layout, uint32 waitMilliseconds) noexcept;

    // 잠금을 푼다. 잡고 있지 않으면 아무것도 하지 않는다.
    void Release() noexcept {
        lock_.Release();
    }

    bool IsHeld() const noexcept {
        return lock_.IsHeld();
    }

    // layout(같은 루트 철자)의 잠금을 잡고 있는지다(저장 같은 쓰기 API가 다른 프로젝트의 잠금을
    // 받지 않게 — 7.1절 15 A2c2b). 잡을 때의 루트를 SHA-256으로 기억한다(할당 없이).
    bool IsHeldFor(const ProjectLayout& layout) const noexcept;

private:
    platform::FileLock lock_;
    // 잡은 프로젝트 루트의 SHA-256이다.
    Sha256Digest root_{};
};

} // namespace rocks
