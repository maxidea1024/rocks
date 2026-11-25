#pragma once

#include "ScreenHost.h"

#include <AssetRuntime/AssetManager.h>
#include <AssetRuntime/CookedManifest.h>
#include <Core/Containers/Array.h>

// 쿠킹 텍스처를 엔진 에셋 런타임에서 요청해 화면 표시 목록에 올린다(Docs/Design/RenderBase 6.5절
// "업로드" — 패키지 게시에서 시작, 18.5절 7항). 매니페스트에서 대표 타입이 Texture 파생인 항목을
// 경로 순서로 요청하고, 준비 callback(패키지 공개 — 게임 lane)이 lease를 쥐면 다음 Update가
// TexturePlatformData와 패키지 bulk를 ScreenHost::AddTexture로 올린다. CPU Availability 텍스처는
// 올리지 않고 대체 항목이다.
//
// 사용 예(엔진 호스트 — 엔진 Initialize·화면 Start 뒤):
//   CookedTextureLibrary library(allocator, assets.Manager(), assets.Manifest(), engine.Types());
//   library.RequestAll();
//   // 프레임 끝(EndFrame): library.Update(screenHost);
//   // 엔진 종료 hook(BeginShutdown): library.Release();   — EngineAssets 종료 계약(11.8절 6)
//
// 스레드: 게임 스레드 전용이다(AssetManager 소유 스레드). manager·types는 이 객체보다 오래 산다.
// 수명: 준비 callback이 이 객체를 가리키므로 AssetManager Shutdown(엔진 Shutdown)이 끝날 때까지
// 산다(엔진보다 먼저 선언한다). Release 뒤의 callback은 아무것도 하지 않는다.
namespace sandbox {

using namespace rocks;

class CookedTextureLibrary {
public:
    CookedTextureLibrary(Allocator& allocator, AssetManager& manager,
                         const CookedManifest& manifest, TypeRegistry& types) noexcept;
    ~CookedTextureLibrary() noexcept;
    CookedTextureLibrary(const CookedTextureLibrary&) = delete;
    CookedTextureLibrary& operator=(const CookedTextureLibrary&) = delete;

    // 매니페스트의 텍스처 항목을 모두 요청한다. 요청한 수를 돌려준다. 오류: 할당 실패(요청 하나의
    // 실패는 Warning을 남기고 그 항목을 건너뛴다).
    Result<uint32> RequestAll() noexcept;

    // 준비된 텍스처를 screen에 올린다(프레임마다). 화면 lane이 시작 전이면 다음 Update로 미룬다.
    void Update(screen::ScreenHost& screen) noexcept;

    // 티켓·lease를 모두 놓는다(엔진 종료 hook — 에셋 Subsystem 정리 전). 다시 부르면 무동작이다.
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

    // i번째 요청의 경로 힌트다.
    StringView PathOf(uint32 request) const noexcept;

    // 쿠킹 데이터 방문 함수다(data는 방문 동안만 유효 — reader·readerContext로 bulk를 읽는다).
    using CookedDataVisitor = void (*)(void* context, const TexturePlatformData& data,
                                       PlatformBulkReader reader, void* readerContext) noexcept;

    // i번째 요청이 올린 텍스처의 쿠킹 데이터를 visit으로 본다(self-test의 CPU 기준). 오류: lease
    // 없음 NotReady, 객체 접근 오류.
    Result<void> InspectCookedData(uint32 request, CookedDataVisitor visit, void* context) noexcept;

private:
    enum class State : uint8 {
        Requested,
        Ready,
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
        LoadTicket ticket;
        AssetLease lease;
        State state = State::Requested;
        uint32 inspector = screen::ScreenHost::NoInspectorEntry;
    };

    static Result<Span<const uint8>> ReadPackageBulk(void* context,
                                                     const BulkDataRef& ref) noexcept;

    void Upload(Entry& entry, screen::ScreenHost& screen) noexcept;

    Allocator& allocator_;
    AssetManager& manager_;
    const CookedManifest& manifest_;
    TypeRegistry& types_;
    Array<Entry> entries_;
    uint32 uploaded_ = 0;
    uint32 failed_ = 0;
    bool released_ = false;
};

} // namespace sandbox
