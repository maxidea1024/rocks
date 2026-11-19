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

} // namespace rocks::test
