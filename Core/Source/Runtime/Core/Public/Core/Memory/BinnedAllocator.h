#pragma once

#include <Base/Compiler.h>
#include <Core/Memory/Allocator.h>

namespace rocks {

namespace detail {
struct BinnedAllocatorTestAccess;
} // namespace detail

// BinnedAllocator 생성 설정이다(기본값이면 플랫폼 기본 예약 크기).
//
// 사용 예:
//   BinnedAllocatorConfig config;
//   config.reserveBytes = usize{256} << 20;   // 도구처럼 작게 쓰는 인스턴스
//   BinnedAllocator memory(backing, config);
struct BinnedAllocatorConfig {
    // 작은 블록용으로 예약할 주소 범위(byte)다. slab 크기(BinnedAllocator::SlabBytes) 단위로
    // 올린다. 0이면 플랫폼 기본값(BinnedAllocator::DefaultReserveBytes)이다. 예약은 주소만 잡고
    // 메모리를 commit하지 않는다. OS가 예약을 거부하면 절반씩 줄여 다시 시도한다(BinnedAllocator
    // 주석).
    usize reserveBytes = 0;
    // 비어서 돌려받은 slab을 commit한 채 보관하는 한도(개수)다. 넘는 slab은 decommit한다. 0이면
    // 비는 즉시 decommit한다.
    uint32 retainedEmptySlabs = 64;
};

// 주소 범위와 느린 경로의 사용량이다(할당 수·byte는 Stats가 낸다). 한 시점의 snapshot이다.
struct BinnedAllocatorUsage {
    // 실제로 예약한 주소 범위(byte)다. 설정보다 작으면 OS가 예약을 거부해 줄인 것이고, 0이면
    // 예약하지 못해 모든 요청이 상위 allocator로 간다.
    usize reservedBytes = 0;
    // commit한 slab(쓰는 slab과 보관 중인 빈 slab)의 byte다.
    usize committedBytes = 0;
    // 보관 중인 빈 slab 수다.
    uint32 retainedSlabs = 0;
    // 살아 있는 큰 할당 수다(상위 allocator가 준 블록).
    uint64 largeAllocations = 0;
    // 작은 요청인데 상위 allocator로 간 누적 횟수다(예약 범위가 없거나 다 찼거나 slab을 commit하지
    // 못했을 때). 0이 아니면 예약 크기를 늘릴지 본다.
    uint64 fallbackAllocations = 0;
};

// 범용 할당자다(Docs/Design/GarbageCollectionScale.ko.md 18절 — UE 5.8 FMallocBinned3 대응).
// 작은 요청(MaxSmallBytes 이하, 정렬 MaxSmallAlignment 이하)은 인스턴스가 예약한 주소 범위 안의
// 64KB slab에서 크기 등급별 블록으로 내주고, 그보다 큰 요청은 상위 allocator(backing)로 넘긴다.
//
// 사용 예:
//   SystemAllocator system;
//   BinnedAllocator memory(system);
//   auto block = memory.Allocate(48, 16);
//   if (block) { /* 사용 */ (void)memory.Deallocate(block.Value()); }
//
// 계약:
// - 0 byte는 성공한 nullptr, 0이거나 2의 거듭제곱이 아닌 정렬은 InvalidArgument다.
// - 작은 요청의 블록은 16 byte 이상으로 정렬된다(큰 경로로 가도 같다). 정렬 32·64는 블록 크기가 그
//   배수인 등급으로 올린다. 큰 요청은 요청한 정렬 그대로 상위 allocator에 넘긴다.
// - Deallocate: 이 인스턴스가 내주지 않은 주소, 이미 반환한 주소, 블록 가운데 주소는 NotFound이고
//   상태를 바꾸지 않는다. 예약 범위 밖 주소·쓰지 않는 slab의 주소·블록 경계가 아닌 주소는 그 주소를
//   읽지 않고 판정한다. 그 밖(쓰는 slab의 블록 경계)은 slab 머리와 블록 안 반환 표지를 읽는다.
//   한계: 반환 표지는 값 일치다 — 살아 있는 블록의 둘째 word(offset 8)에 사용자가 그 블록의 표지
//   값(인스턴스마다 무작위, 최상위 비트가 선 64비트 값)을 써 두면 정상 반환이 NotFound가 된다.
//   주소가 재사용된 뒤의 옛 포인터는 새 포인터와 구분하지 못한다(Allocator.h).
// - Stats의 liveBytes는 요청 크기가 아니라 내준 블록 크기의 합이다(반환 때 요청 크기를 알 수 없다
//   — 큰 할당은 요청 크기 그대로다). Allocator.h의 "요청 크기"와 다르다.
// - OS가 주소 범위 예약을 거부해도 생성은 실패하지 않는다: 크기를 절반씩 줄여 MinReserveBytes까지
//   (설정이 그보다 작으면 설정값 한 번만) 다시 시도하고, 끝내 못 하면 모든 요청이 상위
//   allocator로 간다(느리지만 맞다). 실제 크기는 ReservedBytes()로 본다. 예약 범위가 다 찼거나
//   slab commit이 실패한 작은 요청도 상위 allocator로 간다(Usage().fallbackAllocations).
// - 상위 allocator: 이 객체와 모든 할당보다 오래 살고, 어느 스레드에서나 부를 수 있어야 한다. 상위
//   호출은 이 allocator의 잠금 밖에서 한다. 성공과 함께 null·정렬이 틀린 주소·주소 넘침·예약
//   범위와 겹치는 주소·이미 내준 주소를 돌려주면 Fatal이다. 상위가 큰 블록의 반환을 거부하면 그
//   오류를 돌려주고 할당은 살아 있는 채로 남는다.
// - 스레드: 모든 메서드를 여러 스레드에서 동시에 부를 수 있다. 파괴는 모든 호출이 끝난 뒤에 하고,
//   반환하지 않은 할당이 남아 있으면 Fatal이다.
class BinnedAllocator final : public Allocator {
public:
    // slab 하나의 크기(byte)다. 예약 크기의 단위다.
    static constexpr usize SlabBytes = usize{64} << 10;
    // 작은 블록 경로가 받는 가장 큰 요청(byte)과 정렬이다. 넘으면 상위 allocator로 간다.
    static constexpr usize MaxSmallBytes = 8192;
    static constexpr usize MaxSmallAlignment = 64;
    // 예약 크기의 플랫폼 기본값이다(Windows·Mac 16 GiB, Android 4 GiB — 39비트 주소 기기).
    static constexpr usize DefaultReserveBytes = IsAndroid ? usize{4} << 30 : usize{16} << 30;
    // 예약이 거부될 때 줄여 가며 다시 시도하는 하한이다.
    static constexpr usize MinReserveBytes = usize{64} << 20;

    // backing은 큰 할당의 상위 allocator다. 생성은 실패하지 않는다(위 계약).
    explicit BinnedAllocator(Allocator& backing, const BinnedAllocatorConfig& config = {}) noexcept;
    // 반환하지 않은 할당이 남아 있으면 Fatal이다. 예약한 주소 범위와 내부 저장소를 모두 푼다.
    ~BinnedAllocator() noexcept override;

    BinnedAllocator(const BinnedAllocator&) = delete;
    BinnedAllocator& operator=(const BinnedAllocator&) = delete;
    BinnedAllocator(BinnedAllocator&&) = delete;
    BinnedAllocator& operator=(BinnedAllocator&&) = delete;

    // 실패: 잘못된 정렬 InvalidArgument, 큰 경로의 상위 allocator 오류(그대로 전달), 큰 할당
    // 원장을 늘리지 못하면 OutOfMemory. 실패는 통계를 바꾸지 않는다. tag는 상위 allocator로만 간다.
    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override;
    // nullptr는 성공한 무동작이다. 미소유·중복·중간 주소는 NotFound다(위 계약).
    Result<void> Deallocate(void* address) noexcept override;

    // 보관 중인 빈 slab을 모두 decommit한다. 살아 있는 할당은 건드리지 않는다. 메모리 압박 때나
    // 큰 작업이 끝난 뒤에 부른다.
    void Trim() noexcept;

    // 살아 있는 할당 수·byte와 누적 할당 수다(작은 블록 + 큰 할당). 다른 스레드가 쓰는 중이면 그
    // 순간의 근사다.
    AllocationStats Stats() const noexcept;
    // 주소 범위와 느린 경로의 사용량이다.
    BinnedAllocatorUsage Usage() const noexcept;
    // 실제로 예약한 주소 범위(byte)다. 0이면 예약하지 못했다.
    usize ReservedBytes() const noexcept;

private:
    friend struct detail::BinnedAllocatorTestAccess;
    // 내부 상태다(등급별 잠금·slab 목록·주소 범위·큰 할당 원장). 타입은 구현 파일에만 둔다.
    struct State;
    // 시험 통로(detail::BinnedAllocatorTestAccess)가 부르는 실패 주입·내부 조회 진입점이다. 선언은
    // Core/Private/Memory/BinnedAllocatorTestHooks.h, 구현은 내부 상태를 아는 구현 파일에 있다.
    // 제품 코드는 부르지 않는다.
    struct TestHooks;

    State* state_ = nullptr;
};

} // namespace rocks
