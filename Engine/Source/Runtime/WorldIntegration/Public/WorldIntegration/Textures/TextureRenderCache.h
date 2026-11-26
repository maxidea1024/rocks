#pragma once

#include <AssetRuntime/AssetManager.h>
#include <Core/Containers/HashMap.h>
#include <Core/Containers/Span.h>
#include <Core/Hash.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Graphics/Device.h>
#include <Object/AssetId.h>
#include <RenderBase/CookedTexture.h>
#include <RenderBase/RenderHandle.h>
#include <RenderBase/RenderLane.h>
#include <RenderBase/RenderNotices.h>
#include <WorldIntegration/Textures/PlatformTextureSource.h>
#include <WorldIntegration/Textures/TextureRenderInfo.h>

namespace rocks {

// 텍스처 에셋(AssetId)마다 GPU 텍스처 하나를 만드는 캐시다(Docs/Design/RenderBase 18.5절 8항 —
// SM8, UE UTexture::UpdateResource·ReleaseResource 자리). 쓰는 쪽(컴포넌트 렌더 상태·도구 표시)이
// Acquire로 사용을 알리면 텍스처를 요청하고, 패키지가 공개되면 쿠킹 데이터를 RenderBase 업로드로
// 바꿔 렌더 lane에 올린다. 쓰는 쪽은 Find의 handle과 표시 값을 프레임 packet에 담는다. 마지막
// Release가 handle을 Retire한 뒤 lease를 놓는다(RenderBase 6.5절 — GPU 자원은 업로드 때 복사한
// byte로 만들어 패키지 byte를 빌리지 않는다).
//
// 사용 예(렌더 lane 소유자 — 게임 lane):
//   TextureRenderCache cache(allocator, assets.Manager(), lane, device);
//   cache.Acquire(textureId);                     // 첫 Acquire가 에셋을 요청한다
//   // 프레임마다(에셋 Pump 뒤 — 엔진 EndFrame hook):
//   lane.Notices().TakeAll(notices);
//   cache.HandleNotices(notices.View());          // ResourceReady·ResourceFailed
//   cache.Update();                               // 공개된 텍스처를 올린다
//   TextureRenderInfo info;
//   if (cache.Find(textureId, info) && !info.handle.IsNull()) { /* packet에 info.handle */ }
//   // 다 쓰면: cache.Release(textureId);
//   // 엔진 종료 hook BeginShutdown(lane BlockFrames 전): cache.Shutdown();
//
// 스레드: 게임 lane(AssetManager 소유 스레드) 전용이다. 함수는 재진입하지 않는다.
// 수명:
// - allocator·manager·lane·device는 캐시보다 오래 산다. allocator는 다른 스레드의 해제를 허용해야
//   한다(업로드 byte RenderBlob을 렌더 lane이 놓는다). lane은 Start 뒤에 쓴다.
// - 준비 callback이 이 객체를 가리키므로 캐시는 AssetManager Shutdown이 끝날 때까지 산다(엔진
//   호스트는 엔진보다 먼저 선언해 엔진 Shutdown 뒤에 파괴한다). Release·Shutdown 뒤에 늦게 온
//   callback은 무동작이다.
// - lease·티켓은 Shutdown(엔진 종료 hook)에서 놓는다 — EngineAssets 종료 계약(AssetSystem 11.8절 6:
//   EngineLoopHooks는 CompleteInFlightWork까지).
class ROCKS_WORLDINTEGRATION_API TextureRenderCache {
public:
    TextureRenderCache(Allocator& allocator, AssetManager& manager, render::RenderLane& lane,
                       const graphics::Device& device) noexcept;
    // Shutdown을 부른다.
    ~TextureRenderCache() noexcept;

    TextureRenderCache(const TextureRenderCache&) = delete;
    TextureRenderCache& operator=(const TextureRenderCache&) = delete;

    // texture 사용자를 하나 더한다. 첫 사용자면 텍스처를 요청하고(기대 타입 Texture) 준비
    // callback을 건다 — 항목은 Loading이다. 이미 있는 항목이면 사용자 수만 올린다(상태 그대로).
    // 오류: null id InvalidArgument, Shutdown 뒤 ShuttingDown, AssetManager RequestAsset·OnReady의
    // 오류(매니페스트에 없는 id NotFound 등 — 항목을 만들지 않는다), 할당 실패.
    Result<void> Acquire(AssetId texture) noexcept;

    // texture 사용자를 하나 뺀다. 0이 되면 handle을 Retire하고(있으면) lease·티켓을 놓은 뒤 항목을
    // 지운다. Retire 게시 실패는 Warning을 남기고 계속한다(lane 종료 뒤 등). 오류: 모르는 id
    // NotFound.
    Result<void> Release(AssetId texture) noexcept;

    // lease가 온 Loading 항목을 올린다(프레임마다 — 에셋 Pump 뒤). 텍스처·패키지를 열어 CPU
    // Availability면 CpuPlaceholder, 아니면 변환(MakePlatformTextureSource) → handle 예약 →
    // PrepareCookedTexture → PostResource(CreateCookedTextureOnLane)으로 Uploading이다. 실패하면
    // Failed(예약한 handle은 그 자리에서 Retire)이고 Warning을 남긴다. 이번 호출에서 처리한 항목
    // 수를 돌려준다. Shutdown 뒤 무동작이다.
    uint32 Update() noexcept;

    // lane 소유자가 거둔 렌더 통지를 넘긴다. 캐시 handle의 ResourceReady는 Ready, ResourceFailed는
    // Failed(오류)로 바꾸고 다른 통지는 무시한다. 바뀐 항목 수를 돌려준다.
    uint32 HandleNotices(Span<const render::RenderNotice> notices) noexcept;

    // texture 항목의 표시 값을 out에 쓴다. 항목이 없으면 false다(out은 그대로).
    bool Find(AssetId texture, TextureRenderInfo& out) const noexcept;

    // 쿠킹 데이터 방문 함수다(data는 방문 동안만 유효 — reader·readerContext로 bulk를 읽는다).
    using PlatformDataVisitor = void (*)(void* context, const TexturePlatformData& data,
                                         PlatformBulkReader reader, void* readerContext) noexcept;

    // texture 항목의 lease로 쿠킹 데이터를 열어 visit을 부른다(도구·self-test의 CPU 기준). 오류:
    // 모르는 id NotFound, lease 없음(Loading·로드 실패) NotReady, 객체 접근 오류.
    Result<void> WithPlatformData(AssetId texture, PlatformDataVisitor visit,
                                  void* context) noexcept;

    // 모든 handle을 Retire한 뒤 모든 lease·티켓을 놓는다. 이후 Acquire는 ShuttingDown, Update는
    // 무동작이다. 엔진 종료 hook BeginShutdown(lane BlockFrames 전)에서 부른다. 두 번째 호출은
    // 무동작이다.
    void Shutdown() noexcept;

    bool IsShutDown() const noexcept {
        return shutDown_;
    }

    // 항목 수와 state 항목 수다.
    uint32 Count() const noexcept {
        return static_cast<uint32>(entries_.Count());
    }

    uint32 CountIn(TextureRenderState state) const noexcept;

    // texture의 사용자 수다(항목이 없으면 0).
    uint32 UsersOf(AssetId texture) const noexcept;

private:
    struct AssetIdHash {
        uint64 operator()(const AssetId& id) const noexcept {
            return Hash<Guid>{}(id.value);
        }
    };

    struct Entry {
        // 준비 callback이 항목을 알아보는 순번이다(같은 id를 놓았다 다시 요청하면 바뀐다).
        uint64 serial = 0;
        uint32 users = 0;
        LoadTicket ticket;
        AssetLease lease;
        TextureRenderInfo info;
    };

    void OnReady(AssetId texture, uint64 serial, Result<AssetLease> lease) noexcept;
    void Upload(AssetId texture, Entry& entry) noexcept;
    void Fail(AssetId texture, Entry& entry, ErrorCode error,
              render::CookedTextureIssue issue) noexcept;
    void RetireHandle(AssetId texture, Entry& entry) noexcept;

    static uint64 HandleKey(render::RenderHandle handle) noexcept {
        return (static_cast<uint64>(handle.slot.index) << 32) | handle.slot.generation;
    }

    Allocator& allocator_;
    AssetManager& manager_;
    render::RenderLane& lane_;
    const graphics::Device& device_;
    HashMap<AssetId, Entry, AssetIdHash> entries_;
    // 캐시 handle → 항목 id다(통지 대조).
    HashMap<uint64, AssetId> handles_;
    uint64 nextSerial_ = 1;
    bool shutDown_ = false;
};

} // namespace rocks
