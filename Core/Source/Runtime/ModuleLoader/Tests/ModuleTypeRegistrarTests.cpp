#include <Core/Memory/SystemAllocator.h>
#include <Core/Module/ModuleEntry.h>
#include <ModuleLoader/ModuleLoader.h>
#include <ModuleLoader/ModuleStartContext.h>
#include <ModuleLoader/ModuleTypeRegistrar.h>
#include <TestSupport/Test.h>
#include <stdio.h>
#include <string.h>

using namespace rocks;

// 모듈 로더의 타입 등록기 연결(Docs/Design/ModuleLoader.ko.md 3.6·3.10절 MD15·MD16)과 남은 lease
// 진단을 검사한다. 로더는 타입 시스템을 모르므로 등록기는 시험용 기록기다: 등록·해제를 순서 기록에
// 남기고 로더가 넘긴 lease를 복사해 붙잡는다(실제 등록기는 그 lease를 RuntimeTypeOps.module에
// 넣는다). 등록 함수 포인터는 칸이 채워졌는지만 보므로 부르지 않는다.
namespace module_types_test {

constexpr usize JournalCapacity = 48;
constexpr usize EventBytes = 48;
constexpr usize ProbeCount = 4;
constexpr usize NoProbe = ProbeCount;

constexpr const char* ProbeNames[ProbeCount] = {"Alpha", "Beta", "Gamma", "Delta"};

struct Journal {
    char events[JournalCapacity][EventBytes];
    usize count;
};

Journal journal{};
// 모듈마다 Startup이 돌려줄 오류와, Startup에서 복사해 둘 lease(Shutdown이 놓지 않는다 — 남은 lease
// 진단 시험)다.
ErrorCode startupErrors[ProbeCount]{};
bool keepLeaseAfterShutdown[ProbeCount]{};
ModuleLease keptLeases[ProbeCount]{};

void Reset() noexcept {
    journal = Journal{};
    for (usize index = 0; index < ProbeCount; ++index) {
        startupErrors[index] = ErrorCode::None;
        keepLeaseAfterShutdown[index] = false;
        keptLeases[index].Reset();
    }
}

void Note(const char* verb, StringView name) noexcept {
    if (journal.count < JournalCapacity) {
        snprintf(journal.events[journal.count], EventBytes, "%s:%.*s", verb,
                 static_cast<int>(name.ByteCount()), name.Data());
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

// 진입 기술의 registerTypes 칸을 채우는 함수다. 시험 등록기는 부르지 않는다.
Result<void> DeclaredTypes(ModuleRegistration& registration, const ModuleLease& lease) noexcept {
    (void)registration;
    (void)lease;
    return Result<void>::Success();
}

// 시험 등록기다. failOn 모듈의 등록을 Unsupported로 거부하고, 등록한 모듈마다 lease 복사본을 쥔다.
class RecordingRegistrar final : public ModuleTypeRegistrar {
public:
    Result<void> Register(StringView module, ModuleEntry::RegisterTypesFunction registerTypes,
                          const ModuleLease& lease) noexcept override {
        if (registerTypes == nullptr || !lease) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        if (failOn != nullptr && module == StringView(failOn)) {
            return Result<void>::Failure(ErrorCode::Unsupported);
        }

        Note("types", module);
        const usize index = IndexOf(module);
        if (index != NoProbe) {
            leases[index] = lease;
        }

        return Result<void>::Success();
    }

    void Unregister(StringView module) noexcept override {
        Note("untypes", module);
        const usize index = IndexOf(module);
        if (index != NoProbe) {
            leases[index].Reset();
        }
    }

    const char* failOn = nullptr;
    ModuleLease leases[ProbeCount]{};

private:
    static usize IndexOf(StringView module) noexcept {
        for (usize index = 0; index < ProbeCount; ++index) {
            if (module == StringView(ProbeNames[index])) {
                return index;
            }
        }

        return NoProbe;
    }
};

template <usize Index>
class TypedProbe final : public ModuleImplementation {
public:
    Result<void> Startup(ModuleStartContext& context) noexcept override {
        Note("start", ProbeNames[Index]);
        if (keepLeaseAfterShutdown[Index]) {
            keptLeases[Index] = context.Lease();
        }

        if (startupErrors[Index] != ErrorCode::None) {
            return Result<void>::Failure(startupErrors[Index]);
        }

        return Result<void>::Success();
    }

    void PreUnload() noexcept override {
        Note("preunload", ProbeNames[Index]);
    }

    void Shutdown() noexcept override {
        Note("shutdown", ProbeNames[Index]);
    }
};

// Gamma(색인 2)는 리플렉션 메타데이터가 없는 모듈이다(registerTypes가 null).
template <usize Index>
const ModuleEntry& TypedEntry() noexcept {
    static constexpr ModuleEntry entry = detail::MakeModuleEntry<TypedProbe<Index>>(
        ProbeNames[Index], Index == 2 ? nullptr : &DeclaredTypes);
    return entry;
}

constexpr const char* AlphaOnly[] = {"Alpha"};
constexpr const char* BetaOnly[] = {"Beta"};

// Alpha ← Beta ← Gamma, Alpha ← Delta.
constexpr ModuleTableRow TypedRows[] = {
    {"Alpha", &TypedEntry<0>, nullptr, 0, LoadingPhase::Default},
    {"Beta", &TypedEntry<1>, AlphaOnly, 1, LoadingPhase::Default},
    {"Gamma", &TypedEntry<2>, BetaOnly, 1, LoadingPhase::Default},
    {"Delta", &TypedEntry<3>, AlphaOnly, 1, LoadingPhase::Default},
};

UniquePtr<ModuleLoader> MakeLoader(Allocator& allocator) noexcept {
    auto created = ModuleLoader::TryCreate(allocator, ModuleTable(TypedRows));
    if (!created) {
        return {};
    }

    return Move(created.Value());
}

} // namespace module_types_test

namespace module_type_registrar_tests_file {

using namespace module_types_test;

// 등록기를 붙이면 이미 시작한 모듈의 타입을 시작 순서로 몰아서 등록하고, 붙은 뒤 시작하는 모듈은
// Startup 직전에 등록한다(UE도 StartupModule 전에 UObject를 등록한다). 메타데이터가 없는
// 모듈(Gamma)은 등록기를 부르지 않는다. 묶음 종료는 각 모듈의 Shutdown 뒤 그 모듈 타입을 되돌린다.
// 등록기가 쥔 lease는 로더가 발급한 그 모듈의 실제 lease다.
TEST(ModuleLoaderRegistersModuleTypesThroughRegistrar) {
    Reset();
    SystemAllocator allocator;
    RecordingRegistrar registrar;
    UniquePtr<ModuleLoader> loader = MakeLoader(allocator);
    REQUIRE(loader);
    CHECK(loader->Load("Beta").IsOk());
    const ModuleLoadReport attached = loader->AttachTypeRegistrar(registrar);
    CHECK(attached.IsOk());
    CHECK(loader->TypeRegistrar() == &registrar);
    CHECK(loader->LeaseCount("Alpha") == 1);
    CHECK(loader->LeaseCount("Beta") == 1);

    CHECK(loader->Load("Gamma").IsOk());
    CHECK(loader->Load("Delta").IsOk());
    CHECK(loader->LeaseCount("Delta") == 1);
    CHECK(loader->LeaseCount("Gamma") == 0);

    loader->ShutdownGroup(ModuleGroup::Process);
    const char* const expected[] = {
        "start:Alpha",     "start:Beta",     "types:Alpha",     "types:Beta",      "start:Gamma",
        "types:Delta",     "start:Delta",    "preunload:Delta", "preunload:Gamma", "preunload:Beta",
        "preunload:Alpha", "shutdown:Delta", "untypes:Delta",   "shutdown:Gamma",  "shutdown:Beta",
        "untypes:Beta",    "shutdown:Alpha", "untypes:Alpha"};
    CHECK(JournalIs(expected));
    StringView holders[ProbeCount];
    CHECK(loader->ModulesWithLeases(Span<StringView>(holders)) == 0);

    // 남은 등록이 없으므로 떼어도 기록이 늘지 않는다.
    loader->DetachTypeRegistrar();
    CHECK(loader->TypeRegistrar() == nullptr);
    CHECK(journal.count == 18);
}

// 몰아서 등록하다 실패하면 이번에 등록한 것을 되돌리고 붙이지 않는다. 붙은 뒤의 등록 실패는
// StartupFailed(단계 "RegisterTypes")이고 Startup을 부르지 않는다. Startup이 실패하면 등록한 타입을
// 되돌린다. 떼면 남은 등록을 시작 역순으로 되돌린다.
TEST(ModuleLoaderRollsBackModuleTypesOnFailure) {
    Reset();
    SystemAllocator allocator;
    RecordingRegistrar registrar;
    UniquePtr<ModuleLoader> loader = MakeLoader(allocator);
    REQUIRE(loader);
    CHECK(loader->Load("Beta").IsOk());

    registrar.failOn = "Beta";
    const ModuleLoadReport refused = loader->AttachTypeRegistrar(registrar);
    CHECK(refused.code == ModuleLoadCode::StartupFailed);
    CHECK(refused.error == ErrorCode::Unsupported);
    CHECK(strcmp(refused.failed, "Beta") == 0);
    CHECK(strcmp(refused.stage, "RegisterTypes") == 0);
    CHECK(loader->TypeRegistrar() == nullptr);
    CHECK(loader->LeaseCount("Alpha") == 0);

    registrar.failOn = nullptr;
    CHECK(loader->AttachTypeRegistrar(registrar).IsOk());
    registrar.failOn = "Delta";
    const ModuleLoadReport typed = loader->Load("Delta");
    CHECK(typed.code == ModuleLoadCode::StartupFailed);
    CHECK(strcmp(typed.stage, "RegisterTypes") == 0);
    CHECK(loader->State("Delta").Value() == ModuleState::Failed);

    registrar.failOn = nullptr;
    startupErrors[3] = ErrorCode::OutOfMemory;
    const ModuleLoadReport started = loader->Load("Delta");
    CHECK(started.code == ModuleLoadCode::StartupFailed);
    CHECK(strcmp(started.stage, "Startup") == 0);
    CHECK(loader->LeaseCount("Delta") == 0);

    loader->DetachTypeRegistrar();
    const char* const expected[] = {"start:Alpha",   "start:Beta",   "types:Alpha",
                                    "untypes:Alpha", "types:Alpha",  "types:Beta",
                                    "types:Delta",   "start:Delta",  "untypes:Delta",
                                    "untypes:Beta",  "untypes:Alpha"};
    CHECK(JournalIs(expected));
    CHECK(loader->LeaseCount("Alpha") == 0);
    CHECK(loader->LeaseCount("Beta") == 0);
}

// 남은 lease 진단(MD16): 모듈 코드를 가리키는 등록(여기서는 Startup에서 복사한 lease)을 Shutdown이
// 놓지 않으면 묶음 종료 뒤에도 그 모듈의 lease가 남고 ModulesWithLeases가 그 모듈을 알린다.
// 호스트는 로더를 파괴하기 전에 이것으로 기록한다(파괴의 Fatal은 마지막 안전장치). AcquireLease는
// 모듈마다 lease를 더 발급하고 표에 없는 이름은 NotFound다.
TEST(ModuleLoaderDiagnosesLeasesLeftAfterShutdown) {
    Reset();
    SystemAllocator allocator;
    {
        UniquePtr<ModuleLoader> loader = MakeLoader(allocator);
        REQUIRE(loader);
        keepLeaseAfterShutdown[3] = true;
        CHECK(loader->Load("Delta").IsOk());
        auto extra = loader->AcquireLease("Alpha");
        REQUIRE(extra);
        CHECK(loader->LeaseCount("Alpha") == 1);
        auto missing = loader->AcquireLease("Missing");
        CHECK(!missing);
        CHECK(missing.Error() == ErrorCode::NotFound);

        loader->ShutdownGroup(ModuleGroup::Process);
        StringView holders[ProbeCount];
        CHECK(loader->ModulesWithLeases(Span<StringView>(holders)) == 2);
        CHECK(holders[0] == StringView("Alpha"));
        CHECK(holders[1] == StringView("Delta"));
        // 이름 칸이 모자라도 수는 모두 센다.
        StringView one[1];
        CHECK(loader->ModulesWithLeases(Span<StringView>(one)) == 2);

        // 붙잡은 쪽이 놓으면 진단이 비고 로더를 파괴할 수 있다.
        extra.Value().Reset();
        keptLeases[3].Reset();
        CHECK(loader->ModulesWithLeases(Span<StringView>(holders)) == 0);
    }

    // start 둘, preunload 둘, shutdown 둘이다(등록기가 없어 타입 기록은 없다).
    CHECK(journal.count == 6);
}
} // namespace module_type_registrar_tests_file
