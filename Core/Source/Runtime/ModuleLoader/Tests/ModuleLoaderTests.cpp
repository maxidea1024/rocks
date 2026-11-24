#include <Core/Memory/SystemAllocator.h>
#include <Core/Module/ModuleEntry.h>
#include <DynamicModuleTestApi/DynamicProbe.h>
#include <ModuleLoader/HostServices.h>
#include <ModuleLoader/ModuleLoader.h>
#include <ModuleLoader/ModuleStartContext.h>
#include <ModuleLoader/Private/ModuleLoaderTestAccess.h>
#include <Platform/Thread.h>
#include <TestSupport/Test.h>
#include <stdio.h>
#include <string.h>

using namespace rocks;

// 모듈 로더(Docs/Design/ModuleLoader 3.3·3.4·3.7절, ML0)를 시험용 모듈 표로 검사한다. 시험
// 모듈 ProbeModule<I>는 시작·PreUnload·Shutdown을 순서 기록(journal)에 남기고, 시험마다 behaviors로
// 실패·재진입 적재·자동 종료 거부를 고른다. 시험은 한 스레드에서 차례로 돌며 다른 스레드는
// WrongThread 시험이 명시적으로 Join한다(시간에 기대지 않는다).
namespace module_loader_test {

constexpr usize JournalCapacity = 64;
constexpr usize EventBytes = 48;
constexpr usize ProbeCount = 6;

constexpr const char* ProbeNames[ProbeCount] = {"Alpha", "Beta",    "Gamma",
                                                "Delta", "Epsilon", "Zeta"};

struct Journal {
    char events[JournalCapacity][EventBytes];
    usize count;
};

// 시험 모듈의 동작 선택이다. 시험마다 ResetProbes로 기본값(모두 성공)으로 되돌린다.
struct ProbeBehavior {
    ErrorCode startupError;
    bool refuseAutomaticShutdown;
    const char* loadDuringStartup;
    ModuleLoadCode observedLoad;
    bool keepLease;
    bool sawService;
    // 종료 콜백에서 Find로 찾아볼 모듈 이름과 결과(모듈 자신은 self*)다. activeLoader가 있을 때만
    // 본다.
    const char* findDuringShutdown;
    bool foundInPreUnload;
    bool foundInShutdown;
    bool selfInPreUnload;
    bool selfInShutdown;
};

Journal journal{};
ProbeBehavior behaviors[ProbeCount]{};
// keepLease 모듈이 Startup에서 복사해 둔 lease다. 로더를 파괴하기 전에 시험이 놓는다.
ModuleLease keptLeases[ProbeCount]{};
// 종료 콜백이 Find를 부를 로더다. 그 시험만 잠시 설정한다.
ModuleLoader* activeLoader = nullptr;

struct ProbeService {
    int value = 0;
};

void ResetProbes() noexcept {
    journal = Journal{};
    for (usize index = 0; index < ProbeCount; ++index) {
        behaviors[index] = ProbeBehavior{};
        keptLeases[index].Reset();
    }
}

void Note(const char* verb, const char* name) noexcept {
    if (journal.count < JournalCapacity) {
        snprintf(journal.events[journal.count], EventBytes, "%s:%s", verb, name);
        ++journal.count;
    }
}

// journal이 expected와 정확히 같은지다. 다르면 두 목록을 출력한다.
template <usize Count>
bool JournalIs(const char* const (&expected)[Count]) noexcept {
    bool same = journal.count == Count;
    for (usize index = 0; same && index < Count; ++index) {
        same = strcmp(journal.events[index], expected[index]) == 0;
    }

    if (!same) {
        printf("journal mismatch, actual:");
        for (usize index = 0; index < journal.count; ++index) {
            printf(" %s", journal.events[index]);
        }

        printf("\n");
    }

    return same;
}

} // namespace module_loader_test

ROCKS_DECLARE_HOST_SERVICE(module_loader_test::ProbeService, )
ROCKS_DEFINE_HOST_SERVICE(module_loader_test::ProbeService)

namespace module_loader_test {

template <usize Index>
class ProbeModule final : public ModuleImplementation {
public:
    static constexpr usize ProbeIndex = Index;

    Result<void> Startup(ModuleStartContext& context) noexcept override {
        Note("start", ProbeNames[Index]);
        ProbeBehavior& behavior = behaviors[Index];
        behavior.sawService = context.Services().Find<ProbeService>() != nullptr;
        if (behavior.keepLease) {
            keptLeases[Index] = context.Lease();
        }

        if (behavior.loadDuringStartup != nullptr) {
            behavior.observedLoad = context.Loader().Load(behavior.loadDuringStartup).code;
        }

        if (behavior.startupError != ErrorCode::None) {
            return Result<void>::Failure(behavior.startupError);
        }

        return Result<void>::Success();
    }

    void PreUnload() noexcept override {
        Note("preunload", ProbeNames[Index]);
        ProbeBehavior& behavior = behaviors[Index];
        if (activeLoader != nullptr && behavior.findDuringShutdown != nullptr) {
            behavior.foundInPreUnload = activeLoader->Find(behavior.findDuringShutdown) != nullptr;
            behavior.selfInPreUnload = activeLoader->Find(ProbeNames[Index]) != nullptr;
        }
    }

    void Shutdown() noexcept override {
        Note("shutdown", ProbeNames[Index]);
        ProbeBehavior& behavior = behaviors[Index];
        if (activeLoader != nullptr && behavior.findDuringShutdown != nullptr) {
            behavior.foundInShutdown = activeLoader->Find(behavior.findDuringShutdown) != nullptr;
            behavior.selfInShutdown = activeLoader->Find(ProbeNames[Index]) != nullptr;
        }
    }

    bool SupportsAutomaticShutdown() const noexcept override {
        return !behaviors[Index].refuseAutomaticShutdown;
    }

    // 자동 종료를 거부한 모듈의 객체 파괴 순서를 본다(그 밖의 모듈은 Shutdown 기록으로 충분하다).
    ~ProbeModule() noexcept override {
        if (behaviors[Index].refuseAutomaticShutdown) {
            Note("destroy", ProbeNames[Index]);
        }
    }
};

template <usize Index>
const ModuleEntry& ProbeEntry() noexcept {
    static constexpr ModuleEntry entry =
        detail::MakeModuleEntry<ProbeModule<Index>>(ProbeNames[Index]);
    return entry;
}

constexpr const char* AlphaOnly[] = {"Alpha"};
constexpr const char* BetaOnly[] = {"Beta"};
constexpr const char* AlphaBeta[] = {"Alpha", "Beta"};

// Alpha ← Beta ← Gamma, Alpha ← Delta, Epsilon·Zeta는 의존 없음.
constexpr ModuleTableRow ChainRows[] = {
    {"Alpha", &ProbeEntry<0>, nullptr, 0, LoadingPhase::EarliestPossible},
    {"Beta", &ProbeEntry<1>, AlphaOnly, 1, LoadingPhase::Default},
    {"Gamma", &ProbeEntry<2>, AlphaBeta, 2, LoadingPhase::PostDefault},
    {"Delta", &ProbeEntry<3>, AlphaOnly, 1, LoadingPhase::Default},
    {"Epsilon", &ProbeEntry<4>, nullptr, 0, LoadingPhase::Default},
    {"Zeta", &ProbeEntry<5>, nullptr, 0, LoadingPhase::Default},
};

UniquePtr<ModuleLoader> MakeLoader(Allocator& allocator, ModuleTable table) noexcept {
    auto created = ModuleLoader::TryCreate(allocator, table);
    if (!created) {
        return {};
    }

    return Move(created.Value());
}

bool HasState(const ModuleLoader& loader, StringView name, ModuleState expected) noexcept {
    const Optional<ModuleState> state = loader.State(name);
    return state.HasValue() && state.Value() == expected;
}

struct ChangeLog {
    char names[ProbeCount][EventBytes];
    usize count;
};

} // namespace module_loader_test

using namespace module_loader_test;

// 의존을 먼저 시작하고, 이미 시작한 모듈은 다시 시작하지 않으며, 묶음 종료는 전체 PreUnload 뒤 각
// Shutdown을 시작 역순으로 부른다(UE UnloadModulesAtShutdown 절차). Changed는 시작마다 한 번, 종료
// 절차에서는 내지 않는다.
TEST(ModuleLoaderStartsDependenciesFirstAndShutsDownInReverse) {
    ResetProbes();
    SystemAllocator allocator;
    {
        UniquePtr<ModuleLoader> loader = MakeLoader(allocator, ModuleTable(ChainRows));
        REQUIRE(loader);
        CHECK(loader->ModuleCount() == ProbeCount);
        ChangeLog changes{};
        auto subscribed =
            loader->Changed.TryAddLambda([&changes](StringView name, ModuleChange change) noexcept {
                if (change == ModuleChange::Started && changes.count < ProbeCount) {
                    snprintf(changes.names[changes.count], EventBytes, "%.*s",
                             static_cast<int>(name.ByteCount()), name.Data());
                    ++changes.count;
                }
            });
        REQUIRE(subscribed);

        const ModuleLoadReport gamma = loader->Load("Gamma");
        CHECK(gamma.IsOk());
        CHECK(strcmp(gamma.requested, "Gamma") == 0);
        CHECK(gamma.failed[0] == '\0');
        const char* const started[] = {"start:Alpha", "start:Beta", "start:Gamma"};
        CHECK(JournalIs(started));
        CHECK(loader->IsStarted("Alpha"));
        CHECK(loader->IsStarted("Gamma"));
        CHECK(!loader->IsStarted("Delta"));
        CHECK(HasState(*loader, "Delta", ModuleState::NotLoaded));
        CHECK(!loader->State("Missing").HasValue());

        // 이미 시작한 모듈의 재적재는 할 일 없이 성공한다.
        CHECK(loader->Load("Gamma").IsOk());
        CHECK(loader->Load("Delta").IsOk());
        const char* const withDelta[] = {"start:Alpha", "start:Beta", "start:Gamma", "start:Delta"};
        CHECK(JournalIs(withDelta));
        CHECK(changes.count == 4);
        CHECK(strcmp(changes.names[0], "Alpha") == 0);
        CHECK(strcmp(changes.names[1], "Beta") == 0);
        CHECK(strcmp(changes.names[2], "Gamma") == 0);
        CHECK(strcmp(changes.names[3], "Delta") == 0);

        loader->ShutdownGroup(ModuleGroup::Process);
        const char* const stopped[] = {"start:Alpha",    "start:Beta",      "start:Gamma",
                                       "start:Delta",    "preunload:Delta", "preunload:Gamma",
                                       "preunload:Beta", "preunload:Alpha", "shutdown:Delta",
                                       "shutdown:Gamma", "shutdown:Beta",   "shutdown:Alpha"};
        CHECK(JournalIs(stopped));
        CHECK(changes.count == 4);
        CHECK(HasState(*loader, "Alpha", ModuleState::Stopped));
        CHECK(HasState(*loader, "Gamma", ModuleState::Stopped));
        CHECK(HasState(*loader, "Epsilon", ModuleState::NotLoaded));
        CHECK(loader->Find("Alpha") == nullptr);

        // 프로세스 묶음 종료가 시작된 뒤에는 다시 적재하지 않는다.
        const ModuleLoadReport late = loader->Load("Epsilon");
        CHECK(late.code == ModuleLoadCode::ShuttingDown);
        CHECK(strcmp(late.failed, "Epsilon") == 0);
        CHECK(!loader->IsStarted("Epsilon"));
    }

    // 로더 소멸은 남은 모듈이 없으므로 기록을 더 남기지 않는다.
    CHECK(journal.count == 12);
}

// Startup이 실패하면 그 모듈의 Shutdown은 불리지 않고, 요청 모듈은 DependencyFailed(처음 실패한
// 모듈과 그 오류를 담는다)로 끝난다. 이미 시작한 의존 모듈은 그대로이며 호출자가 묶음 종료로 역순
// 정리한다. 실패한 모듈은 다시 Load하면 처음부터 시도한다.
TEST(ModuleLoaderStopsOnStartupFailureAndRetriesFailedModule) {
    ResetProbes();
    SystemAllocator allocator;
    UniquePtr<ModuleLoader> loader = MakeLoader(allocator, ModuleTable(ChainRows));
    REQUIRE(loader);
    // 정리 뒤에도 다시 적재해야 하므로 프로세스 묶음이 아닌 호스트 묶음에서 시작한다.
    detail::ModuleLoaderTestAccess::SetCurrentGroup(*loader, ModuleGroup::Host);
    behaviors[1].startupError = ErrorCode::Unsupported;

    const ModuleLoadReport report = loader->Load("Gamma");
    CHECK(report.code == ModuleLoadCode::DependencyFailed);
    CHECK(report.error == ErrorCode::Unsupported);
    CHECK(strcmp(report.requested, "Gamma") == 0);
    CHECK(strcmp(report.failed, "Beta") == 0);
    CHECK(strcmp(report.stage, "Startup") == 0);
    CHECK(strstr(report.message, "Beta") != nullptr);
    CHECK(!report.truncated);
    const char* const failed[] = {"start:Alpha", "start:Beta"};
    CHECK(JournalIs(failed));
    CHECK(HasState(*loader, "Alpha", ModuleState::Started));
    CHECK(HasState(*loader, "Beta", ModuleState::Failed));
    CHECK(HasState(*loader, "Gamma", ModuleState::NotLoaded));
    CHECK(loader->Find("Beta") == nullptr);

    // 실패한 모듈을 직접 요청하면 StartupFailed다.
    const ModuleLoadReport direct = loader->Load("Beta");
    CHECK(direct.code == ModuleLoadCode::StartupFailed);
    CHECK(strcmp(direct.failed, "Beta") == 0);

    // 호출자의 시작 실패 정리: 그 묶음에서 시작한 모듈을 역순으로 내린다(실패한 Beta는 대상이
    // 아니다).
    loader->ShutdownGroup(ModuleGroup::Host);
    const char* const cleaned[] = {"start:Alpha", "start:Beta", "start:Beta", "preunload:Alpha",
                                   "shutdown:Alpha"};
    CHECK(JournalIs(cleaned));
    CHECK(HasState(*loader, "Alpha", ModuleState::Stopped));
    CHECK(HasState(*loader, "Beta", ModuleState::Failed));

    // 실패 원인을 없애면 같은 요청이 처음부터 성공한다.
    behaviors[1].startupError = ErrorCode::None;
    journal = Journal{};
    CHECK(loader->Load("Gamma").IsOk());
    const char* const retried[] = {"start:Alpha", "start:Beta", "start:Gamma"};
    CHECK(JournalIs(retried));
    CHECK(loader->IsStarted("Beta"));
}

// 묶음은 Startup 때의 현재 묶음이다. ShutdownGroup은 그 묶음과 안쪽 묶음만 내리고, 호스트·엔진
// 묶음은 다시 시작할 수 있다(한 프로세스의 다음 Engine). 자동 종료를 거부한 모듈은 묶음 종료 뒤에도
// 남고, 로더가 파괴될 때 Shutdown 없이 객체만 파괴된다.
// 종료 중 조회는 UE와 같다: 모듈은 자기 Shutdown 직전에만 ShuttingDown이 된다(UE는 UnloadModule
// 직전에만 bIsReady를 끈다). 그래서 Beta의 PreUnload·Shutdown 동안 아직 내리지 않은 의존 Alpha가
// Find로 보이고, PreUnload 단계에서는 Beta 자신도 보이며, 자기 Shutdown 안에서는 자신이 보이지
// 않는다. 예전 구현은 1단계에서 대상 전부를 ShuttingDown으로 바꿔 Beta가 의존 Alpha를 찾지 못했다.
TEST(ModuleLoaderKeepsDependenciesVisibleDuringShutdown) {
    ResetProbes();
    SystemAllocator allocator;
    UniquePtr<ModuleLoader> loader = MakeLoader(allocator, ModuleTable(ChainRows));
    REQUIRE(loader);
    CHECK(loader->Load("Beta").IsOk());
    behaviors[1].findDuringShutdown = "Alpha";
    behaviors[0].findDuringShutdown = "Beta";
    activeLoader = loader.Get();
    loader->ShutdownGroup(ModuleGroup::Process);
    activeLoader = nullptr;

    CHECK(behaviors[1].foundInPreUnload);
    CHECK(behaviors[1].selfInPreUnload);
    CHECK(behaviors[1].foundInShutdown);
    CHECK(!behaviors[1].selfInShutdown);
    // Alpha의 PreUnload 때 Beta는 아직 Started이고, Alpha의 Shutdown 때 Beta는 이미 내렸다.
    CHECK(behaviors[0].foundInPreUnload);
    CHECK(behaviors[0].selfInPreUnload);
    CHECK(!behaviors[0].foundInShutdown);
    CHECK(!behaviors[0].selfInShutdown);
    const char* const stopped[] = {"start:Alpha",     "start:Beta",    "preunload:Beta",
                                   "preunload:Alpha", "shutdown:Beta", "shutdown:Alpha"};
    CHECK(JournalIs(stopped));
}

TEST(ModuleLoaderShutsDownGroupsFromTheInside) {
    ResetProbes();
    SystemAllocator allocator;
    {
        UniquePtr<ModuleLoader> loader = MakeLoader(allocator, ModuleTable(ChainRows));
        REQUIRE(loader);
        CHECK(loader->Load("Alpha").IsOk());
        detail::ModuleLoaderTestAccess::SetCurrentGroup(*loader, ModuleGroup::Host);
        CHECK(loader->Load("Beta").IsOk());
        behaviors[5].refuseAutomaticShutdown = true;
        CHECK(loader->Load("Zeta").IsOk());
        detail::ModuleLoaderTestAccess::SetCurrentGroup(*loader, ModuleGroup::Engine);
        CHECK(loader->Load("Gamma").IsOk());

        loader->ShutdownGroup(ModuleGroup::Engine);
        const char* const engineDown[] = {"start:Alpha", "start:Beta",      "start:Zeta",
                                          "start:Gamma", "preunload:Gamma", "shutdown:Gamma"};
        CHECK(JournalIs(engineDown));
        CHECK(loader->IsStarted("Beta"));

        // 다음 Engine: 엔진 묶음 모듈을 다시 시작한다(새 수명 객체).
        CHECK(loader->Load("Gamma").IsOk());
        CHECK(loader->IsStarted("Gamma"));
        journal = Journal{};

        // 호스트 묶음을 내리면 안쪽 엔진 묶음도 함께 시작 역순으로 내린다. Zeta는 자동 종료를
        // 거부했다.
        loader->ShutdownGroup(ModuleGroup::Host);
        const char* const hostDown[] = {"preunload:Gamma", "preunload:Beta", "shutdown:Gamma",
                                        "shutdown:Beta"};
        CHECK(JournalIs(hostDown));
        CHECK(loader->IsStarted("Alpha"));
        CHECK(loader->IsStarted("Zeta"));
        CHECK(HasState(*loader, "Beta", ModuleState::Stopped));

        // 호스트 묶음은 다시 시작할 수 있다.
        detail::ModuleLoaderTestAccess::SetCurrentGroup(*loader, ModuleGroup::Host);
        CHECK(loader->Load("Beta").IsOk());
        journal = Journal{};
    }

    // 소멸: 남은 Beta(호스트)·Alpha(프로세스)를 역순 절차로 내리고, Zeta는 Shutdown 없이 파괴한다.
    const char* const destroyed[] = {"preunload:Beta", "preunload:Alpha", "shutdown:Beta",
                                     "shutdown:Alpha", "destroy:Zeta"};
    CHECK(JournalIs(destroyed));
}

// 자동 종료를 거부한 모듈은 로더가 파괴될 때 Shutdown 없이 객체만 파괴되며, 그 순서는 표 순서가
// 아니라 시작 역순이다(나중에 시작한 모듈이 먼저 시작한 모듈을 쓸 수 있다). Zeta가 표에서 뒤지만
// 먼저 시작하므로 Epsilon보다 나중에 파괴된다.
TEST(ModuleLoaderDestroysRemainingModulesInReverseStartOrder) {
    ResetProbes();
    SystemAllocator allocator;
    {
        UniquePtr<ModuleLoader> loader = MakeLoader(allocator, ModuleTable(ChainRows));
        REQUIRE(loader);
        behaviors[4].refuseAutomaticShutdown = true;
        behaviors[5].refuseAutomaticShutdown = true;
        CHECK(loader->Load("Zeta").IsOk());
        CHECK(loader->Load("Epsilon").IsOk());
        loader->ShutdownGroup(ModuleGroup::Process);
        CHECK(loader->IsStarted("Zeta"));
        CHECK(loader->IsStarted("Epsilon"));
    }

    const char* const destroyed[] = {"start:Zeta", "start:Epsilon", "destroy:Epsilon",
                                     "destroy:Zeta"};
    CHECK(JournalIs(destroyed));
}

// 시작 중인 모듈을 다시 적재하면 Cycle이다(자기 자신, 또는 시작 중인 의존 사슬). Startup 안에서
// 관계 없는 모듈을 적재하는 것은 허용되며, 그 모듈이 먼저 시작을 마쳐 더 작은 순번을 받으므로 역순
// 종료에서 나중에 내려간다.
TEST(ModuleLoaderRejectsReentrantCyclesAndAllowsNestedLoads) {
    ResetProbes();
    SystemAllocator allocator;
    UniquePtr<ModuleLoader> loader = MakeLoader(allocator, ModuleTable(ChainRows));
    REQUIRE(loader);

    // Alpha가 자기 자신을 적재한다.
    behaviors[0].loadDuringStartup = "Alpha";
    CHECK(loader->Load("Alpha").IsOk());
    CHECK(behaviors[0].observedLoad == ModuleLoadCode::Cycle);

    // Beta의 Startup이 Gamma를 적재한다. Gamma는 시작 중인 Beta에 의존하므로 Cycle이다.
    behaviors[1].loadDuringStartup = "Gamma";
    CHECK(loader->Load("Beta").IsOk());
    CHECK(behaviors[1].observedLoad == ModuleLoadCode::Cycle);
    CHECK(HasState(*loader, "Gamma", ModuleState::NotLoaded));

    // 시작 중인 모듈을 요청하면 report도 Cycle이다(요청한 쪽과 실패한 쪽이 같다).
    behaviors[4].loadDuringStartup = "Epsilon";
    CHECK(loader->Load("Epsilon").IsOk());
    CHECK(behaviors[4].observedLoad == ModuleLoadCode::Cycle);

    // Delta의 Startup이 관계없는 Zeta를 적재한다.
    behaviors[3].loadDuringStartup = "Zeta";
    CHECK(loader->Load("Delta").IsOk());
    CHECK(behaviors[3].observedLoad == ModuleLoadCode::Success);
    CHECK(loader->IsStarted("Zeta"));

    journal = Journal{};
    loader->ShutdownGroup(ModuleGroup::Process);
    // 시작 순번: Alpha, Beta, Epsilon, Zeta(Delta의 Startup 안), Delta.
    const char* const stopped[] = {"preunload:Delta", "preunload:Zeta",   "preunload:Epsilon",
                                   "preunload:Beta",  "preunload:Alpha",  "shutdown:Delta",
                                   "shutdown:Zeta",   "shutdown:Epsilon", "shutdown:Beta",
                                   "shutdown:Alpha"};
    CHECK(JournalIs(stopped));
}

namespace {

struct ForeignThreadProbe {
    ModuleLoader* loader = nullptr;
    ModuleLoadCode code = ModuleLoadCode::Success;
    bool sawAlphaStarted = false;
    ModuleImplementation* foundAlpha = nullptr;
};

void ProbeFromForeignThread(void* context) noexcept {
    auto* probe = static_cast<ForeignThreadProbe*>(context);
    probe->code = probe->loader->Load("Delta").code;
    probe->sawAlphaStarted = probe->loader->IsStarted("Alpha");
    probe->foundAlpha = probe->loader->Find("Alpha");
}

} // namespace

// 적재는 로더를 만든 스레드에서만 한다(UE NotLoadedByGameThread). 다른 스레드의 Load는
// WrongThread이고 아무것도 시작하지 않는다. 조회(IsStarted·Find)는 어느 스레드에서든 Started 모듈을
// 본다. 다른 스레드는 Join으로 끝을 확정한 뒤 결과를 읽는다.
TEST(ModuleLoaderRejectsLoadsFromOtherThreads) {
    ResetProbes();
    SystemAllocator allocator;
    UniquePtr<ModuleLoader> loader = MakeLoader(allocator, ModuleTable(ChainRows));
    REQUIRE(loader);
    CHECK(loader->Load("Alpha").IsOk());

    ForeignThreadProbe probe;
    probe.loader = loader.Get();
    platform::Thread thread;
    REQUIRE(thread.Start(&ProbeFromForeignThread, &probe) == platform::ThreadStatus::Success);
    REQUIRE(thread.Join() == platform::ThreadStatus::Success);

    CHECK(probe.code == ModuleLoadCode::WrongThread);
    CHECK(probe.sawAlphaStarted);
    CHECK(probe.foundAlpha == loader->Find("Alpha"));
    CHECK(probe.foundAlpha != nullptr);
    CHECK(!loader->IsStarted("Delta"));
    const char* const started[] = {"start:Alpha"};
    CHECK(JournalIs(started));
}

// 형식 있는 조회는 검사 없는 static_cast이고, 표에 없는 이름은 NotFound report다. 모듈은
// Startup에서 호스트 서비스를 찾고 이 모듈의 lease를 복사해 둘 수 있다(로더가 발급한 원자 계수).
TEST(ModuleLoaderFindsModulesServicesAndCountsLeases) {
    ResetProbes();
    SystemAllocator allocator;
    {
        UniquePtr<ModuleLoader> loader = MakeLoader(allocator, ModuleTable(ChainRows));
        REQUIRE(loader);

        const ModuleLoadReport missing = loader->Load("Missing");
        CHECK(missing.code == ModuleLoadCode::NotFound);
        CHECK(strcmp(missing.stage, "Lookup") == 0);
        CHECK(loader->Find("Missing") == nullptr);
        CHECK(loader->LeaseCount("Missing") == 0);

        // 서비스가 없을 때 시작한 모듈은 null을 본다.
        CHECK(loader->Load("Epsilon").IsOk());
        CHECK(!behaviors[4].sawService);

        ProbeService service;
        service.value = 7;
        HostServices& services = loader->Services();
        CHECK(services.Find<ProbeService>() == nullptr);
        REQUIRE(services.Register(service));
        CHECK(services.Count() == 1);
        ProbeService other;
        const Result<void> duplicate = services.Register(other);
        CHECK(!duplicate);
        CHECK(duplicate.Error() == ErrorCode::AlreadyRegistered);
        CHECK(services.Find<ProbeService>() == &service);

        behaviors[3].keepLease = true;
        CHECK(loader->Load("Delta").IsOk());
        CHECK(behaviors[3].sawService);
        CHECK(loader->LeaseCount("Delta") == 1);
        CHECK(loader->LeaseCount("Alpha") == 0);

        auto* delta = loader->Find<ProbeModule<3>>("Delta");
        CHECK(delta != nullptr);
        CHECK(static_cast<ModuleImplementation*>(delta) == loader->Find("Delta"));
        CHECK(delta != nullptr && delta->ProbeIndex == 3);

        CHECK(!services.Unregister(other));
        CHECK(services.Unregister(service));
        CHECK(services.Find<ProbeService>() == nullptr);
        CHECK(services.Count() == 0);

        // lease를 놓으면 계수가 0이 된다. 로더를 파괴하기 전에 모두 놓아야 한다.
        keptLeases[3].Reset();
        CHECK(loader->LeaseCount("Delta") == 0);
    }
}

namespace module_loader_test {

constexpr const char* MissingDependency[] = {"Nowhere"};
constexpr const char* ForwardDependency[] = {"Beta"};

constexpr ModuleTableRow DuplicateRows[] = {
    {"Alpha", &ProbeEntry<0>, nullptr, 0, LoadingPhase::Default},
    {"Alpha", &ProbeEntry<0>, nullptr, 0, LoadingPhase::Default},
};

constexpr ModuleTableRow UnknownDependencyRows[] = {
    {"Alpha", &ProbeEntry<0>, MissingDependency, 1, LoadingPhase::Default},
};

// 의존이 뒤 행에 있다(의존 순서가 아니다).
constexpr ModuleTableRow ForwardDependencyRows[] = {
    {"Alpha", &ProbeEntry<0>, ForwardDependency, 1, LoadingPhase::Default},
    {"Beta", &ProbeEntry<1>, nullptr, 0, LoadingPhase::Default},
};

// 행 이름과 진입 기술 이름이 다르다.
constexpr ModuleTableRow MismatchedEntryRows[] = {
    {"Beta", &ProbeEntry<0>, nullptr, 0, LoadingPhase::Default},
};

constexpr ModuleTableRow BadNameRows[] = {
    {"9Alpha", &ProbeEntry<0>, nullptr, 0, LoadingPhase::Default},
};

constexpr ModuleTableRow SelfDependencyRows[] = {
    {"Beta", &ProbeEntry<1>, BetaOnly, 1, LoadingPhase::Default},
};

} // namespace module_loader_test

// 표 검사: 빌드가 만든 표가 계약(이름·의존 순서·진입 기술 일치)을 어기면 로더를 만들지 않는다.
TEST(ModuleLoaderRejectsMalformedTables) {
    ResetProbes();
    SystemAllocator allocator;
    const ModuleTable tables[] = {ModuleTable(),
                                  ModuleTable(DuplicateRows),
                                  ModuleTable(UnknownDependencyRows),
                                  ModuleTable(ForwardDependencyRows),
                                  ModuleTable(MismatchedEntryRows),
                                  ModuleTable(BadNameRows),
                                  ModuleTable(SelfDependencyRows)};
    for (const ModuleTable& table : tables) {
        auto created = ModuleLoader::TryCreate(allocator, table);
        CHECK(!created);
        CHECK(created.Error() == ErrorCode::InvalidArgument);
    }

    CHECK(journal.count == 0);
}

#if ROCKS_MONOLITHIC
// 정적 모듈 표(GeneratedModuleTable)로 로더를 하나 더 만드는 시험은 Monolithic에서만 돈다 —
// Modular에는 그 표가 없고 manifest 로더의 행은 진입 함수가 비어
// 있다(Docs/Design/BuildTargets 3.8절 BS8d5). 이 시험 실행 파일의 빌드 생성 모듈
// 표(GeneratedModuleTable)로 로더를 만들고 실행 파일 모듈 자신을 적재하면 표의 모든 모듈이 의존
// 순서로 시작한다(MD20: 시험 실행 파일도 로더로 모듈을 시작한다). 표 검사가 통과한다는 것은 빌드가
// 진입 기술 이름·의존 순서를 맞게 만들었다는 뜻이다.
TEST(ModuleLoaderStartsThisExecutableModuleTable) {
    SystemAllocator allocator;
    const ModuleTable table = GeneratedModuleTable();
    REQUIRE(table.Size() != 0);
    CHECK(strcmp(table[table.Size() - 1].name, ROCKS_MODULE_NAME_TEXT) == 0);
    bool sawCore = false;
    bool sawModuleLoader = false;
    for (const ModuleTableRow& row : table) {
        sawCore = sawCore || strcmp(row.name, "Core") == 0;
        sawModuleLoader = sawModuleLoader || strcmp(row.name, "ModuleLoader") == 0;
        // 면제 모듈(header-only·서드파티·Core 아래)은 표에 없다.
        CHECK(strcmp(row.name, "Base") != 0);
        CHECK(strcmp(row.name, "PlatformBase") != 0);
        CHECK(strcmp(row.name, "Zstd") != 0);
    }

    CHECK(sawCore);
    CHECK(sawModuleLoader);
    CHECK(strcmp(table[0].name, "Core") == 0);

    auto created = ModuleLoader::TryCreate(allocator, table);
    REQUIRE(created);
    UniquePtr<ModuleLoader> loader = Move(created.Value());
    const ModuleLoadReport report = loader->Load(ROCKS_MODULE_NAME_TEXT);
    CHECK(report.IsOk());
    // 실행 중 적재 모듈(DynamicModules — 3.8절)은 표에 있어도 실행 파일 Load가 시작하지 않는다
    // (DynamicModuleTests).
    for (const ModuleTableRow& row : table) {
        CHECK(loader->IsStarted(row.name) ==
              (strcmp(row.name, rocks::test::DynamicProbe::ModuleName) != 0));
    }

    loader->ShutdownGroup(ModuleGroup::Process);
    for (const ModuleTableRow& row : table) {
        CHECK(HasState(*loader, row.name,
                       strcmp(row.name, rocks::test::DynamicProbe::ModuleName) == 0
                           ? ModuleState::NotLoaded
                           : ModuleState::Stopped));
    }
}
#endif
