#pragma once

#include <Base/Compiler.h>
#include <Base/NumericLimits.h>
#include <Base/Types.h>
#include <Core/Diagnostics/LogCapture.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>

namespace rocks {

// 큰 로그 값(긴 문자열·Formatter 결과·런타임 템플릿)을 담을 고정 크기 블록 풀이다
// (Docs/Design/LoggingSystem.ko.md 8.3절). LogBlockProvider를 구현한다.
//
// 구조: lock-free 스택(Treiber). 머리는 블록 index(하위 32 bit)와 태그(상위 32 bit)를 64-bit 원자
// 하나에 묶는다. 성공한 CAS마다 태그를 올려 ABA(같은 index가 빠졌다 돌아온 사이의 오판)를 막는다.
// 다음 index 배열 next[]도 원자 변수다(빌리는 쪽이 읽는 동안 다른 스레드가 쓸 수 있으므로).
//
// 메모리는 Initialize에서 한 번 할당하고, 그 뒤 빌리기·돌려주기는 할당하지 않는다.
// TryBorrow·Release· Resolve는 여러 스레드에서 동시에 불러도 되고 기다리지 않는다.
// Initialize·소멸은 다른 호출이 없을 때만 한다.
//
// 사용 예:
//   LogBlockPool pool;
//   if (!pool.Initialize(allocator, 64, 4096)) { ... }
//   LogBlock block;
//   if (pool.TryBorrow(block)) { ...; pool.Release(block.index); }
class ROCKS_CORE_API LogBlockPool final : public LogBlockProvider {
public:
    LogBlockPool() noexcept = default;
    LogBlockPool(const LogBlockPool&) = delete;
    LogBlockPool& operator=(const LogBlockPool&) = delete;
    ~LogBlockPool() noexcept override;

    // blockCount개의 blockBytes byte 블록을 확보한다. blockCount는 1 이상 NoLogBlock 미만,
    // blockBytes는 MinimumLogRenderBytes 이상이어야 한다(아니면 InvalidArgument). 이미 초기화했으면
    // InvalidArgument, 크기 넘침은 Overflow, 그 밖은 allocator 오류다.
    Result<void> Initialize(Allocator& allocator, usize blockCount, usize blockBytes) noexcept;

    // 메모리를 돌려준다. 빌려 간 블록이 없고 다른 호출이 없을 때만 부른다.
    void Shutdown() noexcept;

    usize BlockBytes() const noexcept override {
        return blockBytes_;
    }

    usize BlockCount() const noexcept {
        return blockCount_;
    }

    // 지금 빌릴 수 있는 블록 수의 근사값이다(통계·시험용).
    usize ApproximateAvailable() const noexcept;

    bool TryBorrow(LogBlock& block) noexcept override;
    Span<const uint8> Resolve(uint32 index) const noexcept override;
    void Release(uint32 index) noexcept override;

private:
    static constexpr uint64 IndexMask = NumericLimits<uint32>::Max();
    // head의 위쪽 절반은 ABA를 막는 태그다(아래 절반은 블록 색인).
    static constexpr uint32 TagShift = 32;
    static_assert(IndexMask == (uint64{1} << TagShift) - 1,
                  "the head tag must start right above the block index bits");

    // 머리를 다른 데이터와 다른 캐시 라인에 둔다(빌리기·돌려주기 CAS 경합 줄).
    struct alignas(CacheLineSize) Head {
        uint64 value = 0;
    };

    Allocator* allocator_ = nullptr;
    uint8* storage_ = nullptr;
    uint32* next_ = nullptr;
    usize blockCount_ = 0;
    usize blockBytes_ = 0;
    Head head_;
    uint64 available_ = 0;
};

} // namespace rocks
