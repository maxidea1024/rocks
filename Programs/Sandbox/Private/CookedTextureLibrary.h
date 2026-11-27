#pragma once

#include "ScreenHost.h"

#include <AssetRuntime/AssetManager.h>
#include <AssetRuntime/CookedManifest.h>
#include <Core/Containers/Array.h>
#include <WorldIntegration/Textures/TextureRenderCache.h>

// 쿠킹 텍스처를 엔진 에셋 런타임에서 요청해 화면 표시 목록에 올린다(Docs/Design/RenderBase 18.5절
// 7·8항). 업로드는 WorldIntegration 텍스처 렌더 캐시(TextureRenderCache)가 한다: 이 목록은
// 매니페스트 에서 대표 타입이 Texture 파생인 항목을 경로 순서로 Acquire하고, 캐시 항목이 Loading을
// 벗어나면 표시 목록에 더한다(ScreenHost::AddCachedTexture — handle은 캐시 것). CPU Availability
// 텍스처는 캐시가 올리지 않고 표시는 검정 대체다. 캐시는 이 객체가 가지고, ScreenHost에 붙여 렌더
// 통지를 함께 받게 한다.
//
// 사용 예(엔진 호스트 — 엔진 Initialize·화면 Start 뒤):
//   CookedTextureLibrary library(allocator, assets.Manager(), assets.Manifest(), engine.Types(),
//                                screen);
//   library.RequestAll();
//   // 프레임 끝(EndFrame — 에셋 Pump 뒤): library.Update();
//   // 엔진 종료 hook(BeginShutdown, 화면 BlockFrames 전): library.Release();
//
// 스레드: 게임 스레드 전용이다(AssetManager 소유 스레드). manager·manifest·types·screen은 이
// 객체보다 오래 산다. 수명: 캐시의 준비 callback이 이 객체 안 캐시를 가리키므로 AssetManager
// Shutdown(엔진 Shutdown)이 끝날 때까지 산다(엔진보다 먼저 선언한다). Release가 사용을 놓고 캐시를
// Shutdown한다(handle Retire → lease 놓기 — EngineAssets 종료 계약 11.8절 6).
namespace sandbox {

using namespace rocks;

class CookedTextureLibrary {
public:
    CookedTextureLibrary(Allocator& allocator, AssetManager& manager,
                         const CookedManifest& manifest, TypeRegistry& types,
                         screen::ScreenHost& screen) noexcept;
    ~CookedTextureLibrary() noexcept;
    CookedTextureLibrary(const CookedTextureLibrary&) = delete;
    CookedTextureLibrary& operator=(const CookedTextureLibrary&) = delete;

    // 매니페스트의 텍스처 항목을 모두 Acquire한다. 요청한 수를 돌려준다. 오류: 할당 실패(요청
    // 하나의 실패는 Warning을 남기고 그 항목을 건너뛴다).
    Result<uint32> RequestAll() noexcept;

    // 캐시를 갱신하고(공개된 텍스처 업로드) Loading을 벗어난 항목을 표시 목록에 더한다(프레임마다).
    // 화면 lane이 시작 전이면 다음 Update로 미룬다.
    void Update() noexcept;

    // 사용을 모두 놓고 캐시를 Shutdown한다(엔진 종료 hook — 에셋 Subsystem 정리 전). 다시 부르면
    // 무동작이다.
    void Release() noexcept;

    uint32 Requested() const noexcept {
        return static_cast<uint32>(entries_.Size());
    }

    // 표시 목록에 올린 수(대체 항목 포함)와 실패 수(로드·변환·검사)다.
    uint32 Uploaded() const noexcept {
        return uploaded_;
    }

    uint32 Failed() const noexcept {
        return failed_;
    }

    // 모든 요청이 끝났는가(올렸거나 실패).
    bool IsSettled() const noexcept {
        return uploaded_ + failed_ == entries_.Size();
    }

    // 올린 항목의 표시 목록 index다(i번째 요청 — 못 올렸으면 ScreenHost::NoInspectorEntry).
    uint32 EntryOf(uint32 request) const noexcept;

    // i번째 요청이 실패했으면 그 오류다(로드·변환·검사 — 성공·진행 중이면 None).
    ErrorCode FailureOf(uint32 request) const noexcept;

    // i번째 요청의 경로 힌트다.
    StringView PathOf(uint32 request) const noexcept;

    // i번째 요청이 올린 텍스처의 쿠킹 데이터를 visit으로 본다(self-test의 CPU 기준 — 캐시의 lease로
    // 연다). 오류: TextureRenderCache::WithPlatformData의 오류(lease 없음 NotReady 등).
    Result<void> InspectCookedData(uint32 request, TextureRenderCache::PlatformDataVisitor visit,
                                   void* context) noexcept;

    TextureRenderCache& Cache() noexcept {
        return cache_;
    }

private:
    enum class State : uint8 {
        Requested,
        Uploaded,
        Failed
    };

    struct Entry {
        explicit Entry(Allocator& allocator) noexcept : path(allocator) {
        }

        Entry(Entry&&) noexcept = default;
        Entry& operator=(Entry&&) noexcept = default;

        AssetId asset{};
        String path;
        State state = State::Requested;
        ErrorCode error = ErrorCode::None;
        uint32 inspector = screen::ScreenHost::NoInspectorEntry;
    };

    void Show(Entry& entry, const TextureRenderInfo& info) noexcept;

    Allocator& allocator_;
    const CookedManifest& manifest_;
    TypeRegistry& types_;
    screen::ScreenHost& screen_;
    TextureRenderCache cache_;
    Array<Entry> entries_;
    uint32 uploaded_ = 0;
    uint32 failed_ = 0;
    bool released_ = false;
};

} // namespace sandbox
