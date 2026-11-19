#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/SlotArray.h>
#include <Graphics/Device.h>
#include <RenderBase/RenderHandle.h>

namespace rocks::render {

// 그래프 자원 풀의 기본 예산(byte)과 축출 전 유지 프레임 수다. UE 렌더 타깃 풀은 3프레임 이상 안 쓴
// 것을 풀 크기가 하한을 넘을 때 오래된 순으로 해제한다(RC/Private/RenderTargetPool.cpp:402–436).
inline constexpr uint64 DefaultGraphPoolBudgetBytes = uint64{256} << 20;
inline constexpr uint32 DefaultGraphPoolRetainFrames = 3;

struct GraphResourcePoolConfig {
    // 풀이 가진 자원(쓰는 중·export·반납 대기 포함) byte 합이 이 값을 넘을 때만 축출한다.
    uint64 budgetBytes = DefaultGraphPoolBudgetBytes;
    // 마지막으로 쓴 뒤 이 프레임 수가 지나야 축출 후보다(경과 프레임만으로는 축출하지 않는다:
    // 마지막 사용 fence 완료도 필요하다, 계획 8.2절).
    uint32 retainFrames = DefaultGraphPoolRetainFrames;
};

struct GraphPoolStats {
    // 풀이 가진 자원 수(아래 상태 모두 포함)와 그 가운데 그래프가 쓰는 중인 수, export 소유자가
    // 가진 수, Retire 뒤 마지막 사용 fence를 기다리는 수다.
    usize resources = 0;
    usize inUse = 0;
    usize exported = 0;
    usize returning = 0;
    // 풀이 가진 자원의 byte 합(추정)이다.
    uint64 bytes = 0;
    // 누적 계수: 장치 생성, 쓰지 않던 자원을 다시 준 수, 축출, export, export 자원이 반납되어 다시
    // 쓸 수 있게 된 수다.
    uint64 created = 0;
    uint64 reused = 0;
    uint64 evicted = 0;
    uint64 exports = 0;
    uint64 returned = 0;
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
//   ... 프레임마다 RenderGraph(desc{.pool = &pool, ...})가 Acquire·Release·Export·Evict를 부른다
//   ...
//   ... export한 history handle은 소유자가 lane.Retire로 놓는다(풀 반납) ...
//   device.WaitIdle(); pool.Clear();                         // 종료(OnLaneStop)
//
// 자원 상태: 쓰지 않음 → (Acquire) 쓰는 중 → (Release) 쓰지 않음, 또는 쓰는 중 → (Export) export →
// (handle Retire) 반납 대기 → (마지막 사용 fence 완료) 쓰지 않음.
//
// 규칙:
// - 키는 desc다: 텍스처는 크기·형식(PixelFormat)·용도·mip 수와 샘플러, 버퍼는 크기·용도·stride.
//   샘플러는 지금 장치가 텍스처 desc의 샘플러로 묶으므로 키에 넣는다(결정 R12의 바인딩 때 샘플러
//   지정이 들어오면 뺀다). desc가 넓어지면(GX2: 차원·array·샘플 수) 키 비교 함수 하나만 고친다.
// - 풀 byte 합은 PixelFormatInfo 기준 mip 사슬 크기(TextureDataBytes, 장치 정렬 제외)의 추정이다.
// - 같은 그래프 안의 재사용(수명이 겹치지 않는 같은 desc 자원)은 그래프가 실행 전에 정한다(RG2,
//   그래프 쪽 계획). 풀은 그래프가 끝날 때 Release한 자원을 다음 그래프에 준다. 단일 graphics
//   queue에서는 GPU가 제출 순서대로 실행하므로 이전 프레임의 GPU 완료를 기다리지 않고 다시 써도
//   된다.
// - export(결정 R15): Export는 쓰는 중인 자원을 렌더 handle에 잇고 그 handle의 Retire 대행자가
//   된다(RenderRetireSink). 자원은 풀 소속 그대로이고 byte 합에 든다. 그 handle의 Retire(렌더 lane
//   Retire, lease가 남았으면 마지막 lease가 풀린 뒤)는 장치 해제가 아니라 반납이다: 마지막 사용
//   fence가 GPU에서 끝난 뒤에야 쓰지 않음으로 돌아가 같은 desc의 다음 Acquire가 다시 쓴다. 그래서
//   history를 매 프레임 만들고 놓아도 정상 상태에서는 장치 생성 없이 2–3개가 돈다.
// - 축출(Evict): 쓰지 않음이고, 마지막 사용 fence가 GPU에서 끝났고, retainFrames 동안 쓰이지 않은
//   자원을, 풀 byte 합이 예산을 넘는 동안 오래된 순으로 장치 Retire한다. fence가 끝나기 전에는
//   축출하지 않는다. 축출 규칙은 반납 뒤부터 적용된다.
//
// 스레드: 장치 소유 스레드(렌더 lane) 전용이다(OnRenderRetire도 렌더 lane이 부른다). 수명: 장치보다
// 먼저 파괴한다. 파괴는 export 중인 자원의 소유를 그 handle에 넘기고(DetachRetireSink, 그 뒤
// handle의 Retire는 장치 Retire다) 나머지를 모두 장치 Retire로 넘긴다(장치가 마지막 사용 fence 뒤에
// 해제한다). export에 쓴 RenderHandleTable은 풀보다 오래 살아야 한다.
class GraphResourcePool final : public RenderRetireSink {
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

    // 쓰지 않는 같은 키의 자원을 주거나 새로 만든다. 반납 대기 가운데 fence가 끝난 것을 먼저
    // 거둔다(CollectReturned).
    // - RenderTarget 텍스처: pixels는 비어 있어야 한다(내용은 GPU가 채운다).
    // - Sampled 텍스처: pixels는 mip 0부터 빈틈 없는 초기 데이터(TextureDataBytes 크기, TextureDesc
    // 배치)이며, 새로 만들면
    //   생성 초기 데이터로, 다시 주면 mip마다 장치 UpdateTexture로 그 내용을 채운다(그래프 안
    //   업로드).
    // - 버퍼: 새로 만들면 0으로 채운다. 다시 준 버퍼의 내용은 이전 사용의 것이다.
    // 실패: 잘못된 desc·pixels 크기 InvalidArgument, 깊이 텍스처 Unsupported(GX2 텍스처 단계), 장치
    // 생성·갱신 실패는 그 오류다.
    Result<Acquired> AcquireTexture(const graphics::TextureDesc& desc,
                                    Span<const uint8> pixels = {}) noexcept;
    Result<Acquired> AcquireBuffer(const graphics::BufferDesc& desc) noexcept;

    // 그래프가 다 쓴 자원을 돌려준다. lastUse는 그 자원을 마지막으로 쓴 제출의 완료 지점이고
    // frame은 그 프레임 번호다.
    void Release(GraphPoolEntry entry, graphics::GpuFencePoint lastUse, uint64 frame) noexcept;

    // 쓰는 중인 자원을 Pending 렌더 handle에 잇고 export 상태로 둔다(결정 R15). 이 풀이 그 handle의
    // Retire 대행자가 된다. 실패: 모르는·쓰는 중이 아닌 항목 StaleHandle, handle 잇기 실패는 그
    // 오류다(항목은 쓰는 중 그대로이므로 호출자가 Release한다).
    Result<void> Export(GraphPoolEntry entry, RenderHandleTable& handles,
                        RenderHandle handle) noexcept;

    // 반납 대기 가운데 마지막 사용 fence가 GPU에서 끝난 것을 쓰지 않음으로 돌리고 그 수를
    // 돌려준다. Acquire·Evict가 먼저 부른다.
    usize CollectReturned() noexcept;

    // 위 축출 규칙으로 자원을 장치 Retire하고 그 수를 돌려준다. frame은 지금 프레임 번호다.
    usize Evict(uint64 frame) noexcept;

    // 쓰지 않는·반납 대기 자원을 모두 장치 Retire한다(종료·장치 재생성). 쓰는 중·export 자원은
    // 남긴다.
    void Clear() noexcept;

    GraphPoolStats Stats() const noexcept;

    const GraphResourcePoolConfig& Config() const noexcept {
        return config_;
    }

    // 두 desc가 같은 풀 키인지다(그래프가 같은 그래프 안 재사용을 정할 때도 쓴다).
    static bool SameTextureKey(const graphics::TextureDesc& left,
                               const graphics::TextureDesc& right) noexcept;
    static bool SameBufferKey(const graphics::BufferDesc& left,
                              const graphics::BufferDesc& right) noexcept;

    // export한 handle의 Retire다(RenderRetireSink, ReleaseRetiredResource가 렌더 lane에서 부른다).
    // 그 자원을 반납 대기로 두고 lastUse가 끝나면 쓰지 않음으로 돌린다. 이 풀이 export하지 않은
    // handle이면 장치 Retire로 넘긴다(새지 않게).
    void OnRenderRetire(RenderHandle handle, RenderResourceKind kind, SlotHandle device,
                        graphics::GpuFencePoint lastUse) noexcept override;

private:
    // 풀 키다. 텍스처는 desc 전체(샘플러 포함), 버퍼는 크기·용도·stride다.
    struct Key {
        bool texture = true;
        graphics::TextureDesc textureDesc;
        graphics::BufferDesc bufferDesc;
    };

    enum class EntryState : uint8 {
        Free,
        InUse,
        Exported,
        Returning
    };

    struct Entry {
        Key key;
        SlotHandle device;
        uint64 bytes = 0;
        uint64 lastFrame = 0;
        graphics::GpuFencePoint lastUse;
        EntryState state = EntryState::Free;
        // export 상태의 handle과 그 표다.
        RenderHandleTable* handles = nullptr;
        RenderHandle handle;
    };

    static bool SameKey(const Key& left, const Key& right) noexcept;
    Result<Acquired> Acquire(const Key& key, Span<const uint8> pixels) noexcept;
    Result<void> FillTexture(const Entry& entry, Span<const uint8> pixels) noexcept;
    void RetireEntry(const Entry& entry) noexcept;

    Allocator& allocator_;
    graphics::Device& device_;
    GraphResourcePoolConfig config_;
    SlotArray<Entry> entries_;
    uint64 bytes_ = 0;
    // 마지막으로 본 프레임 번호다(반납 대기 자원의 나이).
    uint64 frame_ = 0;
    uint64 created_ = 0;
    uint64 reused_ = 0;
    uint64 evicted_ = 0;
    uint64 exports_ = 0;
    uint64 returned_ = 0;
};

} // namespace rocks::render
