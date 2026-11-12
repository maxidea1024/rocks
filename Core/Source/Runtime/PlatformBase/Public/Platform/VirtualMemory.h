#pragma once

#include <Base/Types.h>
#include <Platform/Sync.h>

namespace rocks::platform {

enum class VirtualMemoryCode {
    Success,
    InvalidArgument,
    AlreadyReserved,
    NotReserved,
    Poisoned,
    UnsupportedAddress,
    OsFailure
};

// nativeError는 OS 실패 시 즉시 얻은 GetLastError 또는 errno다. 성공·인자 오류는 0이다.
struct [[nodiscard]] VirtualMemoryResult {
    VirtualMemoryCode code;
    uint32 nativeError;

    explicit operator bool() const noexcept {
        return code == VirtualMemoryCode::Success;
    }
};

struct VirtualMemoryInfo {
    usize pageSize;
    usize reserveGranularity;
};

// 실패 시 info는 갱신하지 않는다. reserveGranularity는 예약 주소의 정렬 단위다.
[[nodiscard]] VirtualMemoryResult QueryVirtualMemoryInfo(VirtualMemoryInfo& info) noexcept;

// OS 주소 공간의 단일 예약을 소유한다. 일반 Allocator와 달리 개별 할당은 관리하지 않는다.
// Reserve 크기와 Commit/Decommit 범위는 페이지 크기의 양수 배수여야 한다.
// Commit은 접근 권한을 부여하며, macOS는 Windows와 같은 commit charge를 보장하지 않는다.
// Decommit은 접근을 막고 내용에 대한 권리를 버린다. 물리 메모리의 즉시 반환이나
// 다시 Commit한 내용의 0 초기화는 보장하지 않는다. 데이터는 실행 권한이 없다.
// 메서드는 직렬화되지만 반환된 주소에 대한 읽기·쓰기는 보호하지 않는다. 호출자는
// Decommit/Release 및 소멸 전에 모든 주소 사용과 다른 메서드 호출을 끝내야 한다.
// Reserve 실패는 예약을 남기지 않으며 다시 시도할 수 있다. Commit/Decommit 실패 시
// 영역은 Poisoned가 되며 Release만 가능하다. Release 실패는
// 소유권을 유지해 재시도할 수 있다. 소멸 시 해제가 실패하면 누수를 숨기지 않고 Fatal이다.
class VirtualMemoryRegion final {
public:
    VirtualMemoryRegion() noexcept = default;
    ~VirtualMemoryRegion() noexcept;

    VirtualMemoryRegion(const VirtualMemoryRegion&) = delete;
    VirtualMemoryRegion& operator=(const VirtualMemoryRegion&) = delete;
    VirtualMemoryRegion(VirtualMemoryRegion&&) = delete;
    VirtualMemoryRegion& operator=(VirtualMemoryRegion&&) = delete;

    [[nodiscard]] VirtualMemoryResult Reserve(usize bytes) noexcept;
    [[nodiscard]] VirtualMemoryResult Commit(usize offset, usize bytes) noexcept;
    [[nodiscard]] VirtualMemoryResult Decommit(usize offset, usize bytes) noexcept;
    // 비어 있는 영역에 대한 Release는 성공이다.
    [[nodiscard]] VirtualMemoryResult Release() noexcept;

    // 이 주소는 소유하지 않는 관찰값이며 Release 이후에는 무효다.
    void* Address() noexcept;
    usize Size() noexcept;
    bool IsPoisoned() noexcept;

private:
    [[nodiscard]] VirtualMemoryResult Change(usize offset, usize bytes, bool commit) noexcept;

    Mutex mutex_;
    void* address_ = nullptr;
    usize size_ = 0;
    usize pageSize_ = 0;
    bool poisoned_ = false;
};

} // namespace rocks::platform
