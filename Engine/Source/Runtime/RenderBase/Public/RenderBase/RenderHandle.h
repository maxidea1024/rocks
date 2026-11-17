#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/SlotArray.h>
#include <Graphics/DeviceTypes.h>
#include <Platform/Sync.h>
#include <RenderBase/RenderTransfer.h>

namespace rocks::render {

// 게임 lane이 예약받는 렌더 자원 handle이다(Docs/Design/RenderBase.ko.md 5.4절 handle 예약, 6.2절).
// 장치 handle이 아니라 RenderBase 표의 slot+generation이며, 렌더 lane이 실제 장치 자원을 만들어 이
// handle에 잇는다. 게임 lane은 생성 요청 때 바로 이 handle을 받아 프레임 packet에 담는다.
struct RenderHandle {
    SlotHandle slot;

    bool IsNull() const noexcept {
        return slot.IsNull();
    }

    bool operator==(const RenderHandle&) const noexcept = default;
};

// 렌더 handle이 가리키는 장치 자원 종류다.
enum class RenderResourceKind : uint8 {
    Buffer,
    Texture,
    Pipeline
};

// 렌더 handle 상태다(6.2절 장치 자원 소유자 상태).
enum class RenderResourceState : uint8 {
    // 예약됐고 렌더 lane이 아직 장치 자원을 잇지 않았다.
    Pending,
    // 장치 자원이 이어졌다.
    Ready,
    // 생성이 실패했다(완료 통지로 알린다).
    Failed,
    // Retire 요청을 받았다. 새 사용은 막히고 GPU 완료 뒤 표에서 빠진다.
    Retired
};

// 렌더 handle의 사용 lease다(계획 11.2절 RenderResourceLease,
// Docs/Design/RenderBase.ko.md 6.2·7.4절). 렌더 그래프 import처럼 장치 자원을 기록에 쓰겠다고
// 승인받은 동안 잡는다. lease가 남은 handle을 Retire하면 새 lease는 막히고, 장치 Retire는 마지막
// lease가 풀린 뒤로 미뤄진다(그 사이 기록이 stale handle을 쓰지 않는다). GPU 사용 뒤 해제는 장치
// 내부 last-use 집계(GX7)가 맡는다.
struct RenderResourceLease {
    RenderHandle handle;
    RenderResourceKind kind = RenderResourceKind::Buffer;
    // lease를 얻을 때의 장치 handle이다(slot+generation).
    SlotHandle device;

    bool IsValid() const noexcept {
        return !handle.IsNull();
    }
};

// RenderHandle → 장치 handle 표다(5.4절 "RenderBase 표가 RenderHandle → 장치 handle을 잇는다").
// 장치 내부 slot 표는 장치 소유 스레드(렌더 lane)만 만진다. 이 표는 게임 lane의 예약과 렌더 lane의
// 연결·조회가 함께 쓰므로 잠금으로 보호한다.
//
// 사용 예:
//   // 게임 lane
//   auto texture = handles.Reserve(RenderResourceKind::Texture);
//   // 렌더 lane 작업
//   auto created = context.Device().CreateTexture(desc, pixels);
//   if (created) {
//       handles.BindTexture(texture.Value(), created.Value());
//   } else {
//       handles.MarkFailed(texture.Value(), created.Error());
//   }
//   auto bound = handles.Texture(texture.Value());   // Ready면 장치 handle
//
// 스레드: 모든 함수가 스레드 안전이다. 수명: 표는 렌더 lane과 그 모든 작업보다 오래 살아야 한다.
class RenderHandleTable {
public:
    explicit RenderHandleTable(Allocator& allocator) noexcept;

    RenderHandleTable(const RenderHandleTable&) = delete;
    RenderHandleTable& operator=(const RenderHandleTable&) = delete;

    // 새 handle을 Pending으로 예약한다. 실패: 할당 실패는 그 오류다.
    Result<RenderHandle> Reserve(RenderResourceKind kind) noexcept;

    // Pending handle에 장치 자원을 잇고 Ready로 바꾼다. 실패: 모르는 handle StaleHandle, 종류가
    // 다르면 TypeMismatch, Pending이 아니면 InvalidArgument(Retired면 ShuttingDown — 호출자가 만든
    // 장치 자원을 바로 Retire한다).
    Result<void> BindBuffer(RenderHandle handle, graphics::BufferHandle buffer) noexcept;
    Result<void> BindTexture(RenderHandle handle, graphics::TextureHandle texture) noexcept;
    Result<void> BindPipeline(RenderHandle handle, graphics::PipelineHandle pipeline) noexcept;

    // Pending handle을 Failed로 바꾼다(error는 진단용으로 남는다).
    Result<void> MarkFailed(RenderHandle handle, ErrorCode error) noexcept;

    // Ready handle의 장치 자원이다. 실패: 모르는 handle StaleHandle, 종류가 다르면 TypeMismatch,
    // Pending·Failed·Retired는 NotFound(그리기는 대체 자원을 쓴다 — RB2).
    Result<graphics::BufferHandle> Buffer(RenderHandle handle) const noexcept;
    Result<graphics::TextureHandle> Texture(RenderHandle handle) const noexcept;
    Result<graphics::PipelineHandle> Pipeline(RenderHandle handle) const noexcept;

    Result<RenderResourceState> State(RenderHandle handle) const noexcept;

    // Ready handle의 lease를 얻는다(7.4절 import). 렌더 lane에서 기록에 쓰기 전에 얻고, 그 기록을
    // 마친 뒤(그래프 실행 끝) ReleaseLease로 놓는다. 실패: 모르는 handle과 Retired handle은
    // StaleHandle(Retire 뒤에는 새 lease가 없다), 종류가 다르면 TypeMismatch, Pending·Failed는
    // NotFound다.
    //
    // 사용 예:
    //   auto lease = handles.AcquireLease(texture, RenderResourceKind::Texture);
    //   if (!lease) { ... 내장 대체 자원을 쓰거나 pass를 넣지 않는다 ... }
    //   ... graphics::TextureHandle{lease.Value().device}로 기록 ...
    //   handles.ReleaseLease(lease.Value());
    Result<RenderResourceLease> AcquireLease(RenderHandle handle, RenderResourceKind kind) noexcept;

    // lease를 놓는다. 마지막 lease이고 그 사이 Retire되었으면 장치 Retire를 기다리는 목록에 넣는다
    // (TakeReleasedRetire). 이미 표에서 빠진 handle은 무시한다.
    void ReleaseLease(const RenderResourceLease& lease) noexcept;

    // handle의 lease 수다(진단·시험). 모르는 handle은 0이다.
    uint32 LeaseCount(RenderHandle handle) const noexcept;

    // Retired로 바꾸고 이어진 장치 자원(없으면 null slot)과 종류를 돌려준다. lease가 남아 있으면
    // deferred가 true이고 device는 null이다: 장치 Retire는 마지막 lease가 풀린 뒤
    // TakeReleasedRetire가 넘긴다. 실패: 모르는 handle StaleHandle, 이미 Retired면 InvalidArgument.
    struct RetiredResource {
        RenderResourceKind kind = RenderResourceKind::Buffer;
        // Ready였을 때만 null이 아니다(deferred면 null).
        SlotHandle device;
        // lease가 남아 장치 Retire를 미뤘다.
        bool deferred = false;
    };

    Result<RetiredResource> BeginRetire(RenderHandle handle) noexcept;

    // 미룬 Retire 가운데 마지막 lease가 풀린 것 하나를 꺼낸다. 있으면 true이고 handle·retired(장치
    // handle 포함)를 채운다. 렌더 lane이 lane 작업 끝에 불러 장치 Retire를 이어 간다.
    bool TakeReleasedRetire(RenderHandle& handle, RetiredResource& retired) noexcept;

    // Retired handle을 표에서 뺀다(GPU 완료 뒤). 그 뒤 handle은 stale이다.
    Result<void> Remove(RenderHandle handle) noexcept;

    // 표에 남은 handle 수다(종료 closure 확인: 0이어야 한다).
    usize Count() const noexcept;

private:
    struct Entry {
        RenderResourceKind kind = RenderResourceKind::Buffer;
        RenderResourceState state = RenderResourceState::Pending;
        ErrorCode error = ErrorCode::None;
        SlotHandle device;
        uint32 leases = 0;
        // Retire 때 lease가 남아 장치 Retire를 미뤘다(device는 아직 이어져 있다).
        bool retireDeferred = false;
    };

    Result<void> Bind(RenderHandle handle, RenderResourceKind kind, SlotHandle device) noexcept;
    Result<SlotHandle> Lookup(RenderHandle handle, RenderResourceKind kind) const noexcept;

    mutable platform::Mutex gate_;
    SlotArray<Entry> entries_;
    // 마지막 lease가 풀려 장치 Retire를 이어 갈 handle이다.
    Array<RenderHandle> releasedRetires_;
};

} // namespace rocks::render

ROCKS_RENDER_TRANSFERABLE(rocks::render::RenderHandle);
