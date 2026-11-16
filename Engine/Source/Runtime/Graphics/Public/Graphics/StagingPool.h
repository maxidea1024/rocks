#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/SlotArray.h>
#include <Graphics/DeviceTypes.h>
#include <Platform/Sync.h>

namespace rocks::graphics {

// backend가 staging 메모리 하나를 만들고 없애는 경계다(D3D12 upload heap 버퍼, OpenGL CPU 메모리).
// data는 CPU가 쓸 수 있는 고정 주소이며 IO 목적지로 바로 쓸 수 있다.
struct StagingMemory {
    void* resource = nullptr;
    uint8* data = nullptr;
    uint64 size = 0;
};

class StagingSource {
public:
    virtual Result<StagingMemory> CreateStaging(uint64 bytes) noexcept = 0;
    virtual void DestroyStaging(const StagingMemory& memory) noexcept = 0;

protected:
    ~StagingSource() = default;
};

// 스트리밍 staging pool의 공용 장부다(RenderBase 4.4절 GX8, TextureStreaming 5.4·6절). 프레임
// 업로드 ring과 따로 두며, 받은 staging은 그것을 읽는 업로드의 fence가 끝난 뒤에만 다시 쓴다.
//
// 사용 예(backend):
//   auto staging = pool.Acquire(bytes);              // 스레드 안전
//   ... IO가 staging.Value().data에 쓴다 ...
//   pool.MarkPending(staging.Value().handle);        // 업로드를 기록했다(아직 제출 전)
//   pool.Release(staging.Value().handle);            // 호출자는 더 쓰지 않는다
//   pool.AssignPending(signaledFence);               // 업로드 묶음 제출 뒤
//   pool.Reclaim(completedFence);                    // fence 완료 뒤 재사용 목록으로
//
// 계약:
// - 받은 handle은 Release까지 그 메모리를 가리킨다. Release 뒤에는 stale이다.
// - 제출 전 업로드가 쓰는(MarkPending) staging은 AssignPending이 fence를 줄 때까지, fence를 받은
// 뒤에는
//   그 fence가 끝날 때까지 다시 내주지 않는다. Release 순서와 무관하다.
// - 재사용은 크기가 요청 이상이고 두 배 이하인 빈 staging을 고른다. 없으면 새로 만든다. 만든 전체
// 크기는
//   budgetBytes를 넘지 않는다(넘으면 빈 staging부터 지우고, 그래도 넘으면 CapacityExceeded).
// - 모든 함수가 스레드 안전이다(자체 잠금). StagingSource 호출은 잠금 안에서 한다.
class StagingPool {
public:
    StagingPool(Allocator& allocator, StagingSource& source, uint64 budgetBytes) noexcept;
    ~StagingPool() noexcept;

    StagingPool(const StagingPool&) = delete;
    StagingPool& operator=(const StagingPool&) = delete;

    Result<StagingBuffer> Acquire(uint64 bytes) noexcept;

    // 받은 staging의 메모리다. 실패: 모르는·Release한 handle StaleHandle.
    Result<StagingMemory> Find(StagingHandle handle) const noexcept;

    // 아직 제출하지 않은 업로드가 이 staging을 읽는다고 표시한다.
    Result<void> MarkPending(StagingHandle handle) noexcept;

    // 제출 전 표시(MarkPending)된 staging에 fence 값을 준다.
    void AssignPending(uint64 fence) noexcept;

    // 호출자가 더 쓰지 않는다. 마지막 업로드의 fence가 끝나면 재사용 목록으로 간다.
    void Release(StagingHandle handle) noexcept;

    // completed 이하 fence의 Release된 staging을 재사용 목록으로 옮긴다.
    void Reclaim(uint64 completed) noexcept;

    // 모든 staging을 없앤다(장치 소멸, GPU idle 뒤).
    void Clear() noexcept;

    StagingStats Stats() const noexcept;

private:
    enum class State : uint8 {
        Acquired,
        Released,
        Free
    };

    struct Entry {
        StagingMemory memory;
        uint64 fence = 0;
        State state = State::Acquired;
        bool pending = false;
    };

    void TrimFreeLocked(uint64 needed) noexcept;

    StagingSource& source_;
    const uint64 budgetBytes_;
    mutable platform::Mutex gate_;
    SlotArray<Entry> entries_;
    uint64 createdBytes_ = 0;
    uint64 reused_ = 0;
    uint64 created_ = 0;
};

} // namespace rocks::graphics
