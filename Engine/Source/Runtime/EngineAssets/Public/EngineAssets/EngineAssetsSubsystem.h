#pragma once

#include <AssetRuntime/AssetLoader.h>
#include <AssetRuntime/AssetManager.h>
#include <AssetRuntime/CookedManifest.h>
#include <AssetRuntime/CookedPackageIo.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Text/String.h>
#include <Engine/Engine.h>
#include <Platform/AsyncFile.h>
#include <Task/WorkerScheduler.h>

// 게임 엔진에 에셋 런타임을 붙이는 Subsystem이다(Docs/Design/AssetSystem.ko.md 11.8절 — A1e).
// 게임이 소유하는 AssetManager 하나를 만들고, 매 프레임 World Tick 전에 Pump하며, 프레임 끝에 에셋
// 객체 Registry GC를 한 조각 진행하고, 엔진 종료 순서 안에서 끝낸다. 설정은 `asset.*` CVar다.
//
// 사용 예(게임 실행 파일):
//   const EngineSubsystemType subsystems[] = {EngineAssetsSubsystemType()};
//   EngineConfig config;
//   config.subsystems = subsystems;
//   config.settings = &hostSettings.Context();     // 필수 — asset.* CVar를 읽는다
//   ...
//   AssetManager& assets = engine.GetSubsystem<EngineAssetsSubsystem>()->Manager();
//   LoadTicket ticket = assets.RequestAsset(request).Value();
//
// 종료 계약(11.8절 6): lease·티켓·scope·명령 handle 객체는 이 Subsystem의 정리 전에 소멸해야 한다 —
// World가 쥔 것은 World 파괴가, EngineClient는 OnEngineShutdown까지, EngineLoopHooks는
// CompleteInFlightWork까지, 이 Subsystem에 의존하는 다른 Subsystem은 자기 Deinitialize까지 놓는다.
// 남으면 AssetManager 파괴가 platform::Fatal이다.
namespace rocks {

namespace config {
class ConfigContext;
} // namespace config

// `asset.*` CVar를 등록한다(모듈 RegisterSettings가 부르고, 호스트 없이 설정 문맥을 만드는 시험도
// 부른다). 오류는 ConfigContext::Register와 같다.
Result<void> RegisterEngineAssetsSettings(config::ConfigContext& context) noexcept;

// 쿠킹 루트를 정한다(11.8절 3): configured가 비지 않았으면 그것, 비었으면 executableDir + "/Cooked"
// 다. out 뒤에 붙인다. 오류: executableDir도 비었으면 InvalidArgument, 할당 실패.
Result<void> ResolveCookedRoot(StringView configured, StringView executableDir,
                               String& out) noexcept;

// configured가 비지 않았으면 그것, 비었으면 빌드 플랫폼의 기본 실행 프로파일 id(Windows·Mac·
// Android — 10절 TargetProfile)다.
StringView ResolveRuntimeProfile(StringView configured) noexcept;

// 실행 파일 경로에서 디렉터리 부분이다('/'·'\\' 마지막 구분자 앞, 없으면 빈 값).
StringView ExecutableDirectoryOf(StringView executablePath) noexcept;

// 쿠킹 매니페스트 파일 이름이다(11.4절 — A1f에서 서명 봉투로 바뀐다).
inline constexpr StringView CookedManifestFileName = "CookedManifest.rman";

// 관측 값이다(시험·도구 — 11.8절 5).
struct EngineAssetsStats {
    // 이번 실행에서 끝난 에셋 Registry GC cycle 수다.
    uint64 gcCycles = 0;
    // Busy(열린 생성 트랜잭션·Borrow)로 GC 조각을 건너뛴 프레임 수다.
    uint64 gcBusySkips = 0;
    // 실패한 Pump 수다(처음 한 번만 Error — 실패는 되돌아오지 않는다, 종료 뒤 등).
    uint64 pumpFailures = 0;
    // 실패한 GC 조각 수(Busy 제외 — 매번 기록)다.
    uint64 gcFailures = 0;
    // GC 수집 요청 대기 중인지다(Discard한 패키지의 RootRef를 놓았고 아직 cycle이 끝나지 않음).
    bool gcPending = false;
    // 끝난 cycle 뒤에도 Discard된 객체가 남아 재시도 시간을 세는 중인지다(11.8절 5).
    bool lingering = false;
    // 로더 worker scheduler의 worker 수다(asset.loader.workers — 초기화 뒤).
    uint32 loaderWorkers = 0;
};

class EngineAssetsSubsystem final : public EngineSubsystem {
public:
    // allocator는 쿠킹 루트·프로파일 문자열의 저장소다(Subsystem 생성 정보가 엔진 allocator를
    // 준다).
    explicit EngineAssetsSubsystem(Allocator& allocator) noexcept;
    EngineAssetsSubsystem(const EngineAssetsSubsystem&) = delete;
    EngineAssetsSubsystem& operator=(const EngineAssetsSubsystem&) = delete;
    // 부분 초기화에도 안전하다(만든 것만 역순으로 끝낸다).
    ~EngineAssetsSubsystem() override;

    // 11.8절 4의 순서로 만든다. 실패하면 만든 것을 역순으로 되돌리고 그 오류를 돌려준다(기록 한
    // 번). 오류: 엔진 설정 문맥 없음 InvalidArgument, 쿠킹 루트 결정·매니페스트 읽기(없으면
    // NotFound)·검증(CorruptPackage 진단)·IO context 시작·scheduler 시작·AssetManager 초기화
    // (프로파일 다름 IncompatibleCook 등)의 오류, 할당 실패.
    Result<void> Initialize(Engine& engine) noexcept override;
    // World Tick 전 Pump(11.8절 5).
    void OnFrameBegin(Engine& engine) noexcept override;
    // 에셋 객체 Registry GC 한 조각(11.8절 5).
    void OnFrameEnd(Engine& engine) noexcept override;
    // AssetManager::Shutdown(11.8절 6 — 대기 callback에 ShuttingDown, 진행 중 IO·worker 대기).
    Result<void> CompleteInFlightWork(Engine& engine) noexcept override;
    // AssetManager 파괴와 실행기·IO 종료(11.8절 6).
    void Deinitialize(Engine& engine) noexcept override;

    // 초기화 뒤 유효하다(게임 스레드).
    AssetManager& Manager() noexcept {
        return *manager_;
    }

    // 결정된 쿠킹 루트와 실행 프로파일이다(초기화 뒤).
    StringView CookedRoot() const noexcept {
        return cookedRoot_.View();
    }

    StringView RuntimeProfile() const noexcept {
        return profile_.View();
    }

    EngineAssetsStats Stats() const noexcept {
        return stats_;
    }

private:
    // 만든 것을 역순으로 끝낸다(Deinitialize·초기화 실패·소멸).
    void Teardown() noexcept;
    void StepGarbageCollection() noexcept;

    Engine* engine_ = nullptr;
    Allocator* allocator_ = nullptr;
    String cookedRoot_;
    String profile_;
    // 생성 순서대로 선언한다(역순 소멸 — 11.8절 4).
    UniquePtr<CookedManifest> manifest_;
    platform::AsyncIoContext io_;
    bool ioStarted_ = false;
    UniquePtr<AsyncCookedPackageIo> files_;
    UniquePtr<WorkerScheduler> scheduler_;
    UniquePtr<WorkerSchedulerLoaderWorker> worker_;
    UniquePtr<AssetManager> manager_;
    EngineAssetsStats stats_;
    // cycle 도중 RootRef를 놓은 패키지가 있었는지다(끝난 뒤 한 번 더 돈다).
    bool releasedDuringCycle_ = false;
    // lingering 동안 흐른 엔진 프레임 시간(delta 합, 초)이다.
    double lingerSeconds_ = 0.0;
};

// 엔진 Subsystem 목록에 넣는 생성 정보다.
EngineSubsystemType EngineAssetsSubsystemType() noexcept;

} // namespace rocks
