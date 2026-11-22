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
[[nodiscard]] ROCKS_PLATFORMBASE_API VirtualMemoryResult
QueryVirtualMemoryInfo(VirtualMemoryInfo& info) noexcept;

// 기기의 물리 메모리 총량(byte)이다(UE FPlatformMemory::GetConstants().TotalPhysical 대응). 설정
// 계층의 기기 프로필 메모리 등급 판정 입력이다(Docs/Design/ConfigSystem.ko.md 6.2절). Windows는
// GlobalMemoryStatusEx의 ullTotalPhys, macOS는 sysctl hw.memsize, 그 밖의 POSIX(Android)는
// sysconf(_SC_PHYS_PAGES) × 페이지 크기다. 실패 시 totalBytes는 갱신하지 않는다(OsFailure).
//
// 사용 예:
//   uint64 bytes = 0;
//   if (platform::QueryPhysicalMemory(bytes)) { /* bytes / (1024 * 1024) MiB */ }
[[nodiscard]] ROCKS_PLATFORMBASE_API VirtualMemoryResult
QueryPhysicalMemory(uint64& totalBytes) noexcept;

// 예약한 범위의 접근 방식이다(VirtualMemoryRegion::Reserve의 인자).
enum class VirtualMemoryAccess : uint8 {
    // 기본. Commit하지 않은 범위와 Decommit한 범위는 접근하면 fault다(guard 페이지로 쓸 수 있다).
    Guarded,
    // 접근을 열어 둔 예약이다. 큰 주소 범위를 작은 조각으로 commit·decommit하는 할당자가 쓴다
    // (UE 5.8 FPlatformVirtualMemoryBlock 대응 — Unix·Android·Apple이 예약을 읽기·쓰기 매핑 하나로
    // 잡는다).
    // - POSIX: 예약이 읽기·쓰기 매핑 하나다(물리 메모리는 처음 쓸 때 붙는다). Commit은 보호를
    //   바꾸지 않고 Decommit은 페이지만 OS에 돌려준다. 조각마다 보호가 달라져 매핑(VMA)이
    //   쪼개지는 일이 없다 — Guarded는 commit한 조각과 아닌 조각이 섞이는 만큼 매핑이 늘고,
    //   Linux·Android에는 프로세스당 매핑 수 한도(vm.max_map_count, 기본 65530)가 있다.
    //   대신 Commit하지 않았거나 Decommit한 범위의 접근이 fault라는 보장이 없다(읽으면 0이나 옛
    //   내용, 쓰면 페이지가 다시 붙는다). 접근 차단에 기대는 코드(guard 페이지)는 Guarded를 쓴다.
    //   메모리 초과 약정을 끈 시스템(Linux vm.overcommit_memory=2)에서는 예약 크기가 곧 약정이라 큰
    //   예약이 거부될 수 있다(Reserve가 OsFailure — 호출자가 크기를 줄여 다시 시도한다).
    // - Windows: Guarded와 같다(commit charge가 실제라 Commit·Decommit이 접근을 열고 막는다).
    Open
};

// OS 주소 공간의 단일 예약을 소유한다. 일반 Allocator와 달리 개별 할당은 관리하지 않는다.
// Reserve 크기와 Commit/Decommit 범위는 페이지 크기의 양수 배수여야 한다.
// Commit은 접근 권한을 부여하며, macOS는 Windows와 같은 commit charge를 보장하지 않는다.
// Decommit은 내용에 대한 권리를 버리고(Guarded는 접근도 막는다) 물리 메모리를 OS에 돌려준다. 물리
// 메모리의 즉시 반환이나 다시 Commit한 내용의 0 초기화는 보장하지 않는다. 데이터는 실행 권한이
// 없다. 메서드는 직렬화되지만 반환된 주소에 대한 읽기·쓰기는 보호하지 않는다. 호출자는
// Decommit/Release 및 소멸 전에 모든 주소 사용과 다른 메서드 호출을 끝내야 한다.
//
// 실패:
// - Reserve 실패는 예약을 남기지 않으며 다시 시도할 수 있다.
// - Commit 실패(OS의 commit 한도 등 — 일시적일 수 있다)는 영역을 닫지 않는다. 그 범위는 commit되지
//   않은 것으로 다룬다(일부 페이지만 바뀌었을 수 있으니 접근하지 않는다). 같은 범위를 다시
//   Commit하거나 Decommit할 수 있고 다른 범위는 영향이 없다.
// - Decommit 실패도 영역을 닫지 않는다. Open 예약과 Windows에서는 그 범위가 그대로다(접근과 내용이
//   유지된다 — 계속 쓰거나 나중에 다시 Decommit한다). POSIX의 Guarded 예약에서는 일부 페이지의
//   접근이 막혔을 수 있으므로 commit되지 않은 것으로 다루고, 쓰려면 다시 Commit한다.
// - Release 실패는 소유권을 유지하고 영역을 Poisoned로 만든다(Commit·Decommit은 Poisoned를 돌려주고
//   Release만 다시 시도할 수 있다). 소멸 시 해제가 실패하면 누수를 숨기지 않고 Fatal이다.
//
// 사용 예:
//   VirtualMemoryRegion region;
//   if (!region.Reserve(bytes, VirtualMemoryAccess::Open)) {
//       // 크기를 줄여 다시 시도한다.
//   }
//   if (!region.Commit(offset, slabBytes)) {
//       // 이 조각은 쓰지 않는다. 나중에 다시 시도할 수 있다.
//   }
//   ...
//   (void)region.Decommit(offset, slabBytes);
class ROCKS_PLATFORMBASE_API VirtualMemoryRegion final {
public:
    VirtualMemoryRegion() noexcept = default;
    ~VirtualMemoryRegion() noexcept;

    VirtualMemoryRegion(const VirtualMemoryRegion&) = delete;
    VirtualMemoryRegion& operator=(const VirtualMemoryRegion&) = delete;
    VirtualMemoryRegion(VirtualMemoryRegion&&) = delete;
    VirtualMemoryRegion& operator=(VirtualMemoryRegion&&) = delete;

    // access는 예약한 범위의 접근 방식이다(위 VirtualMemoryAccess). 예약이 살아 있는 동안 바뀌지
    // 않는다.
    [[nodiscard]] VirtualMemoryResult
    Reserve(usize bytes, VirtualMemoryAccess access = VirtualMemoryAccess::Guarded) noexcept;
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
    VirtualMemoryAccess access_ = VirtualMemoryAccess::Guarded;
    bool poisoned_ = false;
};

} // namespace rocks::platform
