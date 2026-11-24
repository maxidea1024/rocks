#include <Core/Memory/SystemAllocator.h>
#include <Core/Module/ModuleEntry.h>
#include <ModuleLoader/ModuleLoader.h>
#include <TestSupport/HostFailure.h>

// 모듈 로더의 계약 위반이 진행되지 않고 Fatal로 끝나는지 자식 프로세스에서 확인한다. 하네스가
// 시험 실행 파일을 --negative-control 인자로 다시 띄우며, 반환에 도달하면 실패로 판정한다.
namespace rocks::test {

namespace {

const ModuleEntry& SoloEntry() noexcept {
    static constexpr ModuleEntry entry =
        detail::MakeModuleEntry<DefaultModuleImplementation>("Solo");
    return entry;
}

constexpr ModuleTableRow SoloRows[] = {
    {"Solo", &SoloEntry, nullptr, 0, LoadingPhase::Default},
};

// 단계 실행 계약 위반의 종류다(RunModuleLoaderPhaseMisuseControl).
enum class PhaseMisuse : uint32 {
    // StartPhase(None): None은 단계 실행이 없다.
    NonePhase,
    // 엔진 묶음을 시작한 뒤 앞 묶음 단계(PostConfigInit)를 다시 부른다(호스트 순서 오류).
    EarlierGroupPhase,
    // 엔진 묶음 사용자가 남았는데 호스트 묶음을 내린다(Engine의 모듈이 사라진다).
    ShutdownWithEngineUsers,
    Count
};

} // namespace

// Changed 구독자는 Load 안에서 불린다. 그 안의 ShutdownGroup은 시작 중인 의존 사슬 아래에서 모듈을
// 내리게 되므로(Startup 안에서 부른 것과 같다) Load 깊이 검사가 Fatal로 막는다.
int RunModuleLoaderShutdownDuringLoadControl() noexcept {
    PrepareFatalTestProcess();
    SystemAllocator allocator;
    auto created = ModuleLoader::TryCreate(allocator, ModuleTable(SoloRows));
    if (!created) {
        return 2;
    }

    UniquePtr<ModuleLoader> loader = Move(created.Value());
    ModuleLoader* raw = loader.Get();
    auto subscribed = loader->Changed.TryAddLambda(
        [raw](StringView, ModuleChange) noexcept { raw->ShutdownGroup(ModuleGroup::Process); });
    if (!subscribed) {
        return 2;
    }

    (void)loader->Load("Solo");
    return 0;
}

// 단계 실행의 계약 위반은 Fatal이다(Docs/Design/ModuleLoader 3.11절). operation은 PhaseMisuse
// 값이다.
int RunModuleLoaderPhaseMisuseControl(uint32 operation) noexcept {
    PrepareFatalTestProcess();
    if (operation >= static_cast<uint32>(PhaseMisuse::Count)) {
        return 2;
    }

    SystemAllocator allocator;
    auto created = ModuleLoader::TryCreate(allocator, ModuleTable(SoloRows));
    if (!created) {
        return 2;
    }

    UniquePtr<ModuleLoader> loader = Move(created.Value());
    switch (static_cast<PhaseMisuse>(operation)) {
    case PhaseMisuse::NonePhase:
        (void)loader->StartPhase(LoadingPhase::None);
        break;
    case PhaseMisuse::EarlierGroupPhase:
        (void)loader->StartPhase(LoadingPhase::Default);
        (void)loader->StartPhase(LoadingPhase::PostConfigInit);
        break;
    case PhaseMisuse::ShutdownWithEngineUsers:
        (void)loader->StartEngineGroup();
        loader->ShutdownGroup(ModuleGroup::Host);
        break;
    case PhaseMisuse::Count:
        break;
    }

    return 0;
}

} // namespace rocks::test
