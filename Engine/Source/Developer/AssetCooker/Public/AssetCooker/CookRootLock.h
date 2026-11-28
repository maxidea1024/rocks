#pragma once

#include <Core/Hash/Sha256.h>
#include <Core/Memory/Allocator.h>
#include <Core/Text/StringView.h>
#include <Platform/File.h>

// 쿠킹 루트 쓰기 잠금이다(Docs/Design/AssetSystem 10.3절 7항, 10.3.1절 A4f4 1번). 같은 쿠킹 루트
// (`<Saved>/Cooked/<Profile>`)를 두 쿠커가 동시에 쓰지 못하게 한다 — 프로젝트 쿠킹과 엔진 전용
// 쿠킹이 모두 쥔다. 잠금 파일은 쿠킹 루트 밖의 `<Saved>/Locks/Cooked-<Profile>.lock`이고 Locks가
// 없으면 만든다. 에셋 DB 잠금과 같은 PlatformBase FileLock(프로세스 사이 권고 잠금) 위에 있다. 잠금
// 순서는 에셋 DB 잠금 → 쿠킹 루트 잠금이다.
//
// 사용 예:
//   CookRootLock cookLock;
//   if (auto held = cookLock.Acquire(allocator, layout.SavedDirectory(), "Windows", waitMs); !held)
//   {
//       // Busy: 같은 쿠킹 루트를 다른 쿠커가 쓰는 중
//   }
//
// 스레드: 한 객체를 여러 스레드가 동시에 쓰지 않는다.
namespace rocks {

// Saved 아래 잠금 디렉터리와 잠금 파일 이름 앞뒤다.
inline constexpr StringView CookLockDirectoryName = "Locks";
inline constexpr StringView CookLockFilePrefix = "Cooked-";
inline constexpr StringView CookLockFileSuffix = ".lock";

class ROCKS_ASSETCOOKER_API CookRootLock final {
public:
    CookRootLock() noexcept = default;
    ~CookRootLock() noexcept = default;

    CookRootLock(const CookRootLock&) = delete;
    CookRootLock& operator=(const CookRootLock&) = delete;

    // `<savedDirectory>/Cooked/<profileId>`의 잠금을 잡는다. 다른 쪽이 잡고 있으면
    // waitMilliseconds까지 다시 시도한다(0이면 한 번 — `--wait`와 같은 뜻). 오류: 못 잡으면 Busy,
    // 이 객체가 이미 잡고 있으면 AlreadyExists, 빈 인자 InvalidArgument, 경로가 너무 길면
    // CapacityExceeded, Locks를 만들 수 없거나 잠금 파일 오류는 그 파일 오류(FileErrorCode), 경로
    // 조립의 할당 실패. allocator는 경로 조립에만 쓴다(반환 전에 돌려준다).
    Result<void> Acquire(Allocator& allocator, StringView savedDirectory, StringView profileId,
                         uint32 waitMilliseconds) noexcept;

    // 잠금을 푼다. 잡고 있지 않으면 아무것도 하지 않는다.
    void Release() noexcept {
        lock_.Release();
    }

    bool IsHeld() const noexcept {
        return lock_.IsHeld();
    }

    // 그 쿠킹 루트(같은 철자)의 잠금을 잡고 있는지다(게시기가 다른 루트의 잠금을 받지 않게). 잡을
    // 때의 쿠킹 루트를 SHA-256으로 기억한다(할당 없이).
    bool IsHeldFor(StringView savedDirectory, StringView profileId) const noexcept;

private:
    platform::FileLock lock_;
    Sha256Digest root_{};
};

} // namespace rocks
