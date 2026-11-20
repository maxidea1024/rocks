#include <Core/Memory/SystemAllocator.h>
#include <Core/Module/ModuleEntry.h>
#include <ModuleLoader/ModuleLoader.h>
#include <ModuleLoader/ModuleStartContext.h>
#include <TestSupport/Test.h>
#include <stdio.h>
#include <string.h>

using namespace rocks;

// 모듈 로더의 단계 실행과 묶음(Docs/Design/ModuleLoader.ko.md 3.3·3.11절, ML1)을 시험용 모듈 표로
// 검사한다. 시험 모듈 PhaseProbe<I>는 시작·PreUnload·Shutdown을 순서 기록에 남긴다. 표는 단계마다
// 모듈 하나와, 단계 실행이 건너뛰는 None 모듈·요청 시 적재 모듈을 담는다.
namespace module_loader_phase_tests_file {

constexpr usize JournalCapacity = 64;
constexpr usize EventBytes = 48;
constexpr usize ProbeCount = 8;

constexpr const char* ProbeNames[ProbeCount] = {"Boot", "Host", "Pre",    "Main",
                                                "Post", "Late", "Manual", "Probe"};

struct Journal {
    char events[JournalCapacity][EventBytes];
    usize count;
};

Journal journal{};
// 모듈마다 Startup이 돌려줄 오류다.
ErrorCode startupErrors[ProbeCount]{};
// 모듈마다 Startup이 받은 단계다.
LoadingPhase startedPhases[ProbeCount]{};

void Reset() noexcept {
    journal = Journal{};
    for (ErrorCode& error : startupErrors) {
        error = ErrorCode::None;
    }
}

void ClearJournal() noexcept {
    journal = Journal{};
}

void Note(const char* verb, const char* name) noexcept {
    if (journal.count < JournalCapacity) {
        snprintf(journal.events[journal.count], EventBytes, "%s:%s", verb, name);
        ++journal.count;
    }
}

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

template <usize Index>
class PhaseProbe final : public ModuleImplementation {
public:
    Result<void> Startup(ModuleStartContext& context) noexcept override {
        startedPhases[Index] = context.Phase();
        if (startupErrors[Index] != ErrorCode::None) {
            return Result<void>::Failure(startupErrors[Index]);
        }

        Note("start", ProbeNames[Index]);
        return Result<void>::Success();
    }

    void PreUnload() noexcept override {
        Note("preunload", ProbeNames[Index]);
    }

    void Shutdown() noexcept override {
        Note("shutdown", ProbeNames[Index]);
    }
};

template <usize Index>
const ModuleEntry& PhaseEntry() noexcept {
    static constexpr ModuleEntry entry =
        detail::MakeModuleEntry<PhaseProbe<Index>>(ProbeNames[Index]);
    return entry;
}

constexpr const char* BootOnly[] = {"Boot"};
constexpr const char* PreOnly[] = {"Pre"};
constexpr const char* MainOnly[] = {"Main"};
constexpr const char* LateOnly[] = {"Late"};

// 단계마다 모듈 하나다. Manual(None)은 늦은 단계 모듈(Late)에 의존할 수 있고 단계 실행이 시작하지
// 않는다. Probe는 요청 시 적재 행(DynamicModules로만 표에 든 모듈)이라 단계 값(Default)과 무관하게
// 단계 실행이 건너뛴다.
constexpr ModuleTableRow PhaseRows[] = {
    {"Boot", &PhaseEntry<0>, nullptr, 0, LoadingPhase::EarliestPossible, false},
    {"Host", &PhaseEntry<1>, BootOnly, 1, LoadingPhase::PostConfigInit, false},
    {"Pre", &PhaseEntry<2>, BootOnly, 1, LoadingPhase::PreDefault, false},
    {"Main", &PhaseEntry<3>, PreOnly, 1, LoadingPhase::Default, false},
    {"Post", &PhaseEntry<4>, nullptr, 0, LoadingPhase::PostDefault, false},
    {"Late", &PhaseEntry<5>, MainOnly, 1, LoadingPhase::PostEngineInit, false},
    {"Manual", &PhaseEntry<6>, LateOnly, 1, LoadingPhase::None, false},
    {"Probe", &PhaseEntry<7>, MainOnly, 1, LoadingPhase::Default, true},
};

UniquePtr<ModuleLoader> MakeLoader(Allocator& allocator) noexcept {
    auto created = ModuleLoader::TryCreate(allocator, ModuleTable(PhaseRows));
    if (!created) {
        return {};
    }

    return Move(created.Value());
}

bool IsState(const ModuleLoader& loader, StringView name, ModuleState expected) noexcept {
    const Optional<ModuleState> state = loader.State(name);
    return state.HasValue() && state.Value() == expected;
}

// 단계는 값 순서대로 돌고, 단계마다 지금 묶음이 바뀐다(3.3절 표). 엔진 묶음은 참조 수로 함께 쓰며
// 마지막 사용자가 내린다. 묶음을 내리면 그 묶음의 단계를 다시 돌 수 있다(다음 Engine). None 행과
// 요청 시 적재 행은 단계 실행이 시작하지 않고, Load로 시작하면 지금 묶음에 속한다.
TEST(ModuleLoaderStartsPhasesAndGroupsInOrder) {
    Reset();
    SystemAllocator allocator;
    UniquePtr<ModuleLoader> loader = MakeLoader(allocator);
    REQUIRE(loader);
    CHECK(loader->CurrentGroup() == ModuleGroup::Process);
    CHECK(!loader->PhaseStarted(LoadingPhase::EarliestPossible));

    CHECK(loader->StartPhase(LoadingPhase::EarliestPossible).IsOk());
    CHECK(loader->PhaseStarted(LoadingPhase::EarliestPossible));
    CHECK(loader->CurrentGroup() == ModuleGroup::Process);
    CHECK(loader->StartPhase(LoadingPhase::PostConfigInit).IsOk());
    CHECK(loader->CurrentGroup() == ModuleGroup::Host);
    // 이미 돈 같은 묶음 단계는 할 일이 없다.
    CHECK(loader->StartPhase(LoadingPhase::PostConfigInit).IsOk());
    const char* const host[] = {"start:Boot", "start:Host"};
    CHECK(JournalIs(host));

    ClearJournal();
    CHECK(loader->StartEngineGroup().IsOk());
    CHECK(loader->EngineGroupUsers() == 1);
    CHECK(loader->CurrentGroup() == ModuleGroup::Engine);
    CHECK(loader->PhaseStarted(LoadingPhase::PostDefault));
    CHECK(!loader->PhaseStarted(LoadingPhase::PostEngineInit));
    CHECK(loader->StartPhase(LoadingPhase::PostEngineInit).IsOk());
    CHECK(loader->StartPhase(LoadingPhase::Default).IsOk());
    CHECK(!loader->PhaseStarted(LoadingPhase::None));
    CHECK(IsState(*loader, "Manual", ModuleState::NotLoaded));
    CHECK(IsState(*loader, "Probe", ModuleState::NotLoaded));
    CHECK(startedPhases[3] == LoadingPhase::Default);
    CHECK(loader->Load("Probe").IsOk());
    CHECK(loader->Load("Manual").IsOk());
    const char* const engine[] = {"start:Pre",  "start:Main",  "start:Post",
                                  "start:Late", "start:Probe", "start:Manual"};
    CHECK(JournalIs(engine));

    // 엔진 묶음(요청 시 적재한 모듈 포함)만 시작 역순으로 내리고 호스트 묶음은 남는다.
    ClearJournal();
    loader->ShutdownEngineGroup();
    CHECK(loader->EngineGroupUsers() == 0);
    CHECK(loader->CurrentGroup() == ModuleGroup::Host);
    CHECK(!loader->PhaseStarted(LoadingPhase::PreDefault));
    CHECK(loader->PhaseStarted(LoadingPhase::PostConfigInit));
    CHECK(IsState(*loader, "Main", ModuleState::Stopped));
    CHECK(loader->IsStarted("Host"));
    const char* const engineDown[] = {"preunload:Manual", "preunload:Probe", "preunload:Late",
                                      "preunload:Post",   "preunload:Main",  "preunload:Pre",
                                      "shutdown:Manual",  "shutdown:Probe",  "shutdown:Late",
                                      "shutdown:Post",    "shutdown:Main",   "shutdown:Pre"};
    CHECK(JournalIs(engineDown));

    // 다음 Engine이 엔진 단계를 다시 돈다. 동시에 둘이면 처음이 시작하고 마지막이 내린다.
    ClearJournal();
    CHECK(loader->StartEngineGroup().IsOk());
    CHECK(loader->StartEngineGroup().IsOk());
    CHECK(loader->EngineGroupUsers() == 2);
    loader->ShutdownEngineGroup();
    CHECK(loader->IsStarted("Main"));
    loader->ShutdownEngineGroup();
    CHECK(!loader->IsStarted("Main"));
    const char* const again[] = {"start:Pre",      "start:Main",     "start:Post",
                                 "preunload:Post", "preunload:Main", "preunload:Pre",
                                 "shutdown:Post",  "shutdown:Main",  "shutdown:Pre"};
    CHECK(JournalIs(again));

    ClearJournal();
    loader->ShutdownGroup(ModuleGroup::Host);
    CHECK(loader->CurrentGroup() == ModuleGroup::Process);
    loader->ShutdownGroup(ModuleGroup::Process);
    const char* const down[] = {"preunload:Host", "shutdown:Host", "preunload:Boot",
                                "shutdown:Boot"};
    CHECK(JournalIs(down));
}

// 앞 단계를 건너뛰고 부르면 돌지 않은 단계부터 차례로 돈다(설정을 쓰지 않는 도구가 Engine을 만드는
// 경우). 단계 안 첫 실패에서 그 단계를 멈추고 report(requested는 단계 이름)를 돌려주며 그 단계는 돈
// 것으로 치지 않는다. 엔진 묶음의 처음 시작이 실패하면 로더가 그 묶음을 내리고 사용자 수를
// 되돌리므로 다시 시도할 수 있다.
TEST(ModuleLoaderStopsAPhaseOnFailure) {
    Reset();
    SystemAllocator allocator;
    UniquePtr<ModuleLoader> loader = MakeLoader(allocator);
    REQUIRE(loader);
    startupErrors[3] = ErrorCode::OutOfMemory;
    const ModuleLoadReport failed = loader->StartEngineGroup();
    CHECK(failed.code == ModuleLoadCode::StartupFailed);
    CHECK(failed.error == ErrorCode::OutOfMemory);
    CHECK(strcmp(failed.requested, "Default") == 0);
    CHECK(strcmp(failed.failed, "Main") == 0);
    CHECK(loader->EngineGroupUsers() == 0);
    CHECK(loader->PhaseStarted(LoadingPhase::PreDefault) == false);
    CHECK(loader->PhaseStarted(LoadingPhase::PostConfigInit));
    CHECK(IsState(*loader, "Pre", ModuleState::Stopped));
    CHECK(IsState(*loader, "Main", ModuleState::Failed));
    CHECK(loader->IsStarted("Host"));
    const char* const failedRun[] = {"start:Boot", "start:Host", "start:Pre", "preunload:Pre",
                                     "shutdown:Pre"};
    CHECK(JournalIs(failedRun));

    ClearJournal();
    startupErrors[3] = ErrorCode::None;
    CHECK(loader->StartEngineGroup().IsOk());
    const char* const retried[] = {"start:Pre", "start:Main", "start:Post"};
    CHECK(JournalIs(retried));
    loader->ShutdownEngineGroup();
    loader->ShutdownGroup(ModuleGroup::Process);
}

} // namespace module_loader_phase_tests_file
