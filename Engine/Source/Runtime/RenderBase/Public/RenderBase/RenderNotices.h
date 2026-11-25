#pragma once

#include <Core/Containers/Array.h>
#include <Graphics/DeviceTypes.h>
#include <Platform/Sync.h>
#include <RenderBase/RenderHandle.h>

namespace rocks::render {

// 렌더 lane이 게임 lane에 돌려주는 결과 종류다(Docs/Design/RenderBase 5.2절 완료 통지 큐).
enum class RenderNoticeKind : uint8 {
    // 프레임 하나의 lane 처리와 GPU 실행이 끝났다(frame 번호, 실패면 error).
    FrameCompleted,
    // 렌더 handle의 장치 자원이 Ready가 되었다.
    ResourceReady,
    // 렌더 handle의 생성이 실패했다(error).
    ResourceFailed,
    // Retire한 렌더 handle의 장치 자원이 GPU 완료 뒤 해제되었다. handle은 이제 stale이다.
    ResourceRetired,
    // 장치가 손실되었다. 이후 프레임 slot 확보는 실패한다.
    DeviceLost,
    // 렌더 lane 작업이 치명 오류를 보고했다(error).
    LaneFailed,
    // 비동기 readback이 끝났다(frame 칸이 readback 번호, handle이 대상 텍스처). 결과는
    // RenderLane::TakeReadback으로 가져간다. 실패면 error다.
    ReadbackReady,
    // GPU가 끝낸 프레임의 timestamp 결과 하나다(GX9). frame 칸이 CommandContext::WriteTimestamp가
    // 돌려준 번호, value가 GPU tick(DeviceInfo::timestampFrequency)이다. 구간이 끊겨 값이
    // 무효면 error가 Cancelled이고 value는 0이다. 같은 프레임의 FrameCompleted보다 먼저 온다.
    GpuTimestamp
};

// 완료 통지 하나다. 종류에 따라 frame·handle·value를 쓴다(ReadbackReady는 frame에 readback 번호,
// GpuTimestamp는 frame에 timestamp 번호와 value에 tick).
struct RenderNotice {
    RenderNoticeKind kind = RenderNoticeKind::FrameCompleted;
    ErrorCode error = ErrorCode::None;
    uint64 frame = 0;
    RenderHandle handle;
    uint64 value = 0;
    // GpuTimestamp를 쓴 기록의 queue다(query 번호는 queue마다 따로다, RenderBase 8절 RG4).
    graphics::QueueKind queue = graphics::QueueKind::Graphics;
};

// 완료 통지 큐다. 렌더 lane(과 그 fence 통지)이 넣고 게임 lane이 프레임 시작 때 비운다. 셰이더
// 공급 완료는 그 단계(RB3)가 종류를 더한다. 게임 쪽 CPU 데이터를 GPU
// 완료 뒤까지 붙잡는 경우(ObjectRuntime BackendLease)의 해제는 게임 쪽 통합 계층이 이 통지를 받아
// 한다(RenderBase는 ObjectRuntime을 모른다).
//
// 사용 예(게임 lane):
//   Array<RenderNotice> notices(allocator);
//   lane.Notices().TakeAll(notices);
//   for (const RenderNotice& notice : notices) { ... }
//
// 스레드: 모든 함수가 스레드 안전이다. 넣기가 할당에 실패하면 그 통지를 버리고 Dropped를 올린다.
class ROCKS_RENDERBASE_API RenderNotices {
public:
    explicit RenderNotices(Allocator& allocator) noexcept;

    RenderNotices(const RenderNotices&) = delete;
    RenderNotices& operator=(const RenderNotices&) = delete;

    void Push(const RenderNotice& notice) noexcept;

    // 쌓인 통지를 넣은 순서대로 out 뒤에 붙이고 큐를 비운다. out을 늘리지 못하면 그 오류이며 큐는
    // 그대로다.
    Result<void> TakeAll(Array<RenderNotice>& out) noexcept;

    usize Count() const noexcept;

    // 할당 실패로 버린 통지 수다.
    uint64 Dropped() const noexcept;

private:
    mutable platform::Mutex gate_;
    Array<RenderNotice> queue_;
    uint64 dropped_ = 0;
};

// 렌더 lane이 소유하게 되는 byte 묶음이다(자원 초기 데이터 등). 게임 lane이 복사해 만들고 작업 값에
// 담아 넘긴다. 이동 전용이며 소멸 때 allocator에 돌려준다(렌더 lane에서 소멸할 수 있으므로
// allocator는 다른 스레드의 해제를 허용해야 한다).
//
// 사용 예:
//   auto pixels = RenderBlob::TryCopy(allocator, Span<const uint8>(data, size));
//   lane.PostResource(TextureUpload{handle, desc, Move(pixels.Value())}, &CreateTextureOnLane);
class ROCKS_RENDERBASE_API RenderBlob {
public:
    RenderBlob() noexcept = default;
    RenderBlob(RenderBlob&& other) noexcept;
    RenderBlob& operator=(RenderBlob&& other) noexcept;
    RenderBlob(const RenderBlob&) = delete;
    RenderBlob& operator=(const RenderBlob&) = delete;
    ~RenderBlob() noexcept;

    // bytes를 복사한다. 빈 입력은 빈 묶음이다. 실패: 할당 실패는 그 오류다.
    static Result<RenderBlob> TryCopy(Allocator& allocator, Span<const uint8> bytes) noexcept;

    // size byte를 0으로 채워 만든다(여러 조각을 이어 채울 때 — MutableView). 0이면 빈 묶음이다.
    // 실패: 할당 실패는 그 오류다.
    static Result<RenderBlob> TryAllocate(Allocator& allocator, usize size) noexcept;

    Span<const uint8> View() const noexcept {
        return Span<const uint8>(data_, size_);
    }

    // 채우는 쪽(게임 lane — 넘기기 전)이 쓴다.
    Span<uint8> MutableView() noexcept {
        return Span<uint8>(data_, size_);
    }

    usize Size() const noexcept {
        return size_;
    }

private:
    void Release() noexcept;

    Allocator* allocator_ = nullptr;
    uint8* data_ = nullptr;
    usize size_ = 0;
};

} // namespace rocks::render

ROCKS_RENDER_TRANSFERABLE(rocks::render::RenderBlob);
