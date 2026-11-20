#include <Core/Memory/SystemAllocator.h>
#include <Core/Module/ModuleEntry.h>
#include <DynamicModuleTestApi/DynamicProbe.h>
#include <ModuleLoader/ModuleLoader.h>
#include <ModuleLoader/Private/ModuleLoaderTestAccess.h>
#include <TestSupport/Test.h>
#include <string.h>

// 동적 모듈의 public 헤더는 이 실행 파일의 include 경로에 없다(interface 모듈의 헤더만 보인다).
#if __has_include(<DynamicModuleTestFixture/DynamicProbeModule.h>)
#error "a run-time loaded module's headers must not be on the consumer's include path"
#endif

using namespace rocks;

// 실행 중 적재 모듈(DynamicModules — Docs/Design/ModuleLoader.ko.md 3.8절, ML3)을 이 실행 파일의
// 실제 모듈 표로 검사한다. 시험 실행 파일 규칙은 DynamicModuleTestFixture를 DynamicModules로 두고
// 그 interface 모듈(DynamicModuleTestApi)만 링크 의존으로 둔다. 그래서 빌드는 그 모듈을 링크하고
// 표에 행을 싣지만, 실행 파일 Load는 그 모듈을 시작하지 않고 시험은 그 헤더를 보지 못한다.
namespace dynamic_module_tests_file {

constexpr usize RowNone = static_cast<usize>(-1);

usize RowIndex(ModuleTable table, const char* name) noexcept {
    for (usize index = 0; index < table.Size(); ++index) {
        if (strcmp(table[index].name, name) == 0) {
            return index;
        }
    }

    return RowNone;
}

bool RowDependsOn(const ModuleTableRow& row, const char* name) noexcept {
    for (usize index = 0; index < row.dependencyCount; ++index) {
        if (strcmp(row.dependencies[index], name) == 0) {
            return true;
        }
    }

    return false;
}

bool HasState(const ModuleLoader& loader, StringView name, ModuleState expected) noexcept {
    const Optional<ModuleState> state = loader.State(name);
    return state.HasValue() && state.Value() == expected;
}

// 표: 동적 모듈 행은 실행 파일 행(마지막) 앞에 있고, 행의 의존은 링크 의존뿐이다(실행 파일 행은
// 동적 모듈을 의존으로 싣지 않는다). 실행 파일 Load는 동적 모듈을 시작하지 않는다. 코드가
// Load(이름)로 시작하면 Find<T>(이름)이 interface로 수명 객체를 돌려주고, 다시 Load하면 이미
// 시작했으므로 성공이다.
TEST(ModuleLoaderLoadsDynamicModuleOnRequest) {
    const char* const name = rocks::test::DynamicProbe::ModuleName;
    const ModuleTable table = GeneratedModuleTable();
    const usize dynamicRow = RowIndex(table, name);
    const usize executableRow = RowIndex(table, ROCKS_MODULE_NAME_TEXT);
    REQUIRE(dynamicRow != RowNone);
    REQUIRE(executableRow == table.Size() - 1);
    CHECK(dynamicRow < executableRow);
    CHECK(RowDependsOn(table[dynamicRow], "Core"));
    CHECK(!RowDependsOn(table[executableRow], name));

    SystemAllocator allocator;
    auto created = ModuleLoader::TryCreate(allocator, table);
    REQUIRE(created);
    UniquePtr<ModuleLoader> loader = Move(created.Value());
    REQUIRE(loader->Load(ROCKS_MODULE_NAME_TEXT).IsOk());
    CHECK(loader->IsStarted("Core"));
    CHECK(!loader->IsStarted(name));
    CHECK(HasState(*loader, name, ModuleState::NotLoaded));
    CHECK(loader->Find<rocks::test::DynamicProbe>(name) == nullptr);

    const ModuleLoadReport loaded = loader->Load(name);
    CHECK(loaded.IsOk());
    rocks::test::DynamicProbe* probe = loader->Find<rocks::test::DynamicProbe>(name);
    REQUIRE(probe != nullptr);
    CHECK(probe->Started());
    CHECK(probe->Answer() == rocks::test::DynamicProbe::ExpectedAnswer);
    CHECK(loader->Load(name).IsOk());
    CHECK(loader->Find<rocks::test::DynamicProbe>(name) == probe);

    loader->ShutdownGroup(ModuleGroup::Process);
    CHECK(HasState(*loader, name, ModuleState::Stopped));
    CHECK(HasState(*loader, ROCKS_MODULE_NAME_TEXT, ModuleState::Stopped));
}

// 묶음(3.3절): 동적 모듈은 적재될 때 로더의 현재 묶음(가장 안쪽)에 속한다. 엔진 묶음 중에 적재한
// 동적 모듈은 엔진 묶음 종료에서 내려가고, 프로세스 묶음 모듈은 그대로다. 묶음 종료 뒤 다시 적재할
// 수 있다.
TEST(ModuleLoaderDynamicModuleJoinsTheCurrentGroup) {
    const char* const name = rocks::test::DynamicProbe::ModuleName;
    SystemAllocator allocator;
    auto created = ModuleLoader::TryCreate(allocator, GeneratedModuleTable());
    REQUIRE(created);
    UniquePtr<ModuleLoader> loader = Move(created.Value());
    REQUIRE(loader->Load(ROCKS_MODULE_NAME_TEXT).IsOk());
    rocks::detail::ModuleLoaderTestAccess::SetCurrentGroup(*loader, ModuleGroup::Engine);
    REQUIRE(loader->Load(name).IsOk());
    loader->ShutdownGroup(ModuleGroup::Engine);
    CHECK(HasState(*loader, name, ModuleState::Stopped));
    CHECK(loader->IsStarted(ROCKS_MODULE_NAME_TEXT));
    CHECK(loader->IsStarted("Core"));

    REQUIRE(loader->Load(name).IsOk());
    rocks::test::DynamicProbe* probe = loader->Find<rocks::test::DynamicProbe>(name);
    REQUIRE(probe != nullptr);
    CHECK(probe->Started());
    loader->ShutdownGroup(ModuleGroup::Process);
    CHECK(HasState(*loader, name, ModuleState::Stopped));
}

} // namespace dynamic_module_tests_file
