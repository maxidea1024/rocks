#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/SlotArray.h>
#include <Graphics/Device.h>

namespace rocks::render {

// 그래프 자원 풀의 기본 예산(byte)과 축출 전 유지 프레임 수다. UE 렌더 타깃 풀은 3프레임 이상 안 쓴
// 것을 풀 크기가 하한을 넘을 때 오래된 순으로 해제한다(RC/Private/RenderTargetPool.cpp:402–436).
inline constexpr uint64 DefaultGraphPoolBudgetBytes = uint64{256} << 20;
inline constexpr uint32 DefaultGraphPoolRetainFrames = 3;

struct GraphResourcePoolConfig {
    // 풀이 가진 자원(쓰는 중 포함) byte 합이 이 값을 넘을 때만 축출한다.
    uint64 budgetBytes = DefaultGraphPoolBudgetBytes;
    // 마지막으로 쓴 뒤 이 프레임 수가 지나야 축출 후보다(경과 프레임만으로는 축출하지 않는다:
    // 마지막 사용 fence 완료도 필요하다, 계획 8.2절).
    uint32 retainFrames = DefaultGraphPoolRetainFrames;
};

struct GraphPoolStats {
    // 풀이 가진 자원 수와 그 가운데 그래프가 쓰는 중인 수다.
    usize resources = 0;
    usize inUse = 0;
    // 풀이 가진 자원의 byte 합(추정)이다.
    uint64 bytes = 0;
    uint64 created = 0;
    uint64 reused = 0;
    uint64 evicted = 0;
    // 내보내(export) 소유가 풀 밖으로 넘어간 수다.
    uint64 detached = 0;
};

// 풀 자원 하나를 가리키는 표다(그래프가 쓰는 동안만 유효).
struct GraphPoolEntry {
    SlotHandle slot;

    bool IsNull() const noexcept {
        return slot.IsNull();
    }
};

// 그래프 자원 풀이다(UE FRenderTargetPool·FRDGBufferPool 대응, 7.4절). 같은 desc의 장치 자원을
// 프레임 사이에 다시 쓴다. 렌더러(렌더 lane 쪽 처리기)가 하나를 가지고 매 프레임 그래프에 넘긴다.
//
// 사용 예(렌더 lane):
//   GraphResourcePool pool(allocator, device);              // 렌더러 수명
//   ... 프레임마다 RenderGraph(desc{.pool = &pool, ...})가 Acquire·Release·Evict를 부른다 ...
//   device.WaitIdle(); pool.Clear();                         // 종료(OnLaneStop)
//
// 규칙:
// - 키는 desc다: 텍스처는 크기·형식·용도·mip 수와 샘플러, 버퍼는 크기·용도. 샘플러는 지금 장치가
//   텍스처 desc의 샘플러로 묶으므로 키에 넣는다(결정 R12의 바인딩 때 샘플러 지정이 들어오면 뺀다).
//   desc가 넓어지면(GX2·픽셀 형식) 키 계산 함수 하나만 고친다.
// - 같은 그래프 안에서는 재사용하지 않는다(RG1). 그래프가 끝날 때 Release한 자원을 다음 그래프가
//   쓴다. 단일 graphics queue에서는 GPU가 제출 순서대로 실행하므로 이전 프레임의 GPU 완료를
//   기다리지 않고 다시 써도 된다.
// - 축출(Evict): 쓰지 않는 중이고, 마지막 사용 fence가 GPU에서 끝났고, retainFrames 동안 쓰이지
//   않은 자원을, 풀 byte 합이 예산을 넘는 동안 오래된 순으로 장치 Retire한다. fence가 끝나기 전에는
//   축출하지 않는다.
// - export: Detach로 자원 하나를 풀에서 빼 소유를 호출자에게 넘긴다(그래프가 RenderHandle에
// 잇는다).
//
// 스레드: 장치 소유 스레드(렌더 lane) 전용이다. 수명: 장치보다 먼저 파괴한다. 파괴(또는 Clear)는
// 남은 자원을 모두 장치 Retire로 넘긴다(장치가 마지막 사용 fence 뒤에 해제한다).
class GraphResourcePool {
public:
    GraphResourcePool(Allocator& allocator, graphics::Device& device,
                      const GraphResourcePoolConfig& config = {}) noexcept;
    ~GraphResourcePool() noexcept;

    GraphResourcePool(const GraphResourcePool&) = delete;
    GraphResourcePool& operator=(const GraphResourcePool&) = delete;

    struct Acquired {
        GraphPoolEntry entry;
        // 장치 handle의 slot이다(텍스처면 graphics::TextureHandle, 버퍼면 graphics::BufferHandle).
        SlotHandle device;
        bool reused = false;
    };

    // 쓰지 않는 같은 키의 자원을 주거나 새로 만든다. 텍스처는 RenderTarget 용도여야 한다(초기 픽셀
    // 없이 만든다). 버퍼는 0으로 채워 만든다. 실패: 잘못된 desc InvalidArgument, 장치 생성 실패는
    // 그 오류다.
    Result<Acquired> AcquireTexture(const graphics::TextureDesc& desc) noexcept;
    Result<Acquired> AcquireBuffer(const graphics::BufferDesc& desc) noexcept;

    // 그래프가 다 쓴 자원을 돌려준다. lastUse는 그 자원을 마지막으로 쓴 제출의 완료 지점이고
    // frame은 그 프레임 번호다.
    void Release(GraphPoolEntry entry, graphics::GpuFencePoint lastUse, uint64 frame) noexcept;

    // 쓰는 중인 자원을 풀에서 빼고 장치 handle slot을 돌려준다(export). 이후 수명은 받은 쪽이
    // 정한다. 실패: 모르는 항목 StaleHandle.
    Result<SlotHandle> Detach(GraphPoolEntry entry) noexcept;

    // 위 축출 규칙으로 자원을 장치 Retire하고 그 수를 돌려준다. frame은 지금 프레임 번호다.
    usize Evict(uint64 frame) noexcept;

    // 쓰지 않는 자원을 모두 장치 Retire한다(종료·장치 재생성). 쓰는 중인 자원은 남긴다.
    void Clear() noexcept;

    GraphPoolStats Stats() const noexcept;

    const GraphResourcePoolConfig& Config() const noexcept {
        return config_;
    }

private:
    // 풀 키다. 텍스처는 desc 전체(샘플러 포함), 버퍼는 크기·용도다.
    struct Key {
        bool texture = true;
        graphics::TextureDesc textureDesc;
        graphics::BufferDesc bufferDesc;
    };

    struct Entry {
        Key key;
        SlotHandle device;
        uint64 bytes = 0;
        uint64 lastFrame = 0;
        graphics::GpuFencePoint lastUse;
        bool inUse = false;
    };

    static bool SameKey(const Key& left, const Key& right) noexcept;
    Result<Acquired> Acquire(const Key& key) noexcept;
    void RetireEntry(const Entry& entry) noexcept;

    Allocator& allocator_;
    graphics::Device& device_;
    GraphResourcePoolConfig config_;
    SlotArray<Entry> entries_;
    uint64 bytes_ = 0;
    uint64 created_ = 0;
    uint64 reused_ = 0;
    uint64 evicted_ = 0;
    uint64 detached_ = 0;
};

} // namespace rocks::render
