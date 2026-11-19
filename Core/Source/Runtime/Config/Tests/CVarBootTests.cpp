#include "ConfigTestSupport.h"

#include <Config/CVar.h>

// CVar CVR4 시험이다(Docs/Design/CVarSystem.ko.md 9·12절): 봉인 때 부팅 값을 불변 부팅 블록으로
// 굳히고 봉인 뒤 snapshot이 공유한다, 봉인 뒤 직접 쓰기(콘솔)는 BootSealed로 넣을 때 거부, 파일 층
// 변경은 재시작 대기(되돌리면 없어진다), 파생·휘발 층은 BootValueIgnored, UserSetting 부팅 변수의
// 사용자 층 쓰기는 다음 실행용 대기, 봉인 뒤 등록한 부팅 변수는 등록 때 값으로 굳는다(봉인 뒤 바로
// 쓰는 층의 대기 값은 받지 않고 BootSealed), 남은 쓰기가 있으면 봉인은 Busy, 대기 값 늦은
// 적용(LateBound), ReportUnresolved(`did you mean`·명령 이름·멱등), TakeDiagnostics(넘친 수·자기
// 자신), DescribeConsoleSetting.
// 봉인 전 BootCVar::Get의 Fatal은 CVarThreadTests.cpp의 사망 시험이다.
using namespace rocks;
using namespace rocks::config;
using namespace rocks::test::config;

namespace cvar_boot_test {

constinit BootCVar<int32> BootWorkers = cvar::Boot("game.boot.workers", 4).Help("Worker count");

constinit BootCVar<String> BootBackend =
    cvar::Boot("render.boot.backend", "vulkan").Help("BootBackend");

constinit BootCVar<int32> BootQuality =
    cvar::Boot("render.boot.quality", 1).Help("Next-run quality").UserSetting();

constinit BootCVar<int32> LateBoot = cvar::Boot("game.boot.late", 3).Help("Registered late");

constinit BootCVar<int32> SealedLateBoot =
    cvar::Boot("game.boot.sealed", 2).Help("Registered late after sealed writes");

constinit BootCVar<int32> QueuedBoot = cvar::Boot("game.boot.queued", 1).Help("Queued at seal");

constinit CVar<float> BootDistance = cvar::Var("render.boot.distance", 100.f).Help("BootDistance");

} // namespace cvar_boot_test

namespace {

using namespace cvar_boot_test;

bool Pending(ConfigContext& context, StringView name, StringView value) noexcept {
    SystemAllocator allocator;
    Array<ConfigPendingRestart> pending(allocator);
    if (!context.GetPendingRestarts(pending)) {
        return false;
    }

    for (const ConfigPendingRestart& entry : pending) {
        if (entry.name == name) {
            return entry.pending == value;
        }
    }

    return value.IsEmpty();
}

usize CountCode(ConfigContext& context, ConfigDiagnosticCode code) noexcept {
    usize count = 0;
    for (usize index = 0; index < context.Diagnostics().Count(); ++index) {
        count += context.Diagnostics().Get(index).code == code ? 1 : 0;
    }

    return count;
}

} // namespace

// 봉인 때 부팅 값이 불변 부팅 블록으로 굳고 봉인 뒤 snapshot이 그 블록을 공유한다. 봉인 뒤 콘솔
// 쓰기는 넣을 때 BootSealed, 파일 층 변경은 재시작 대기(되돌리면 없어진다), 파생 층(기기 프로필)은
// BootValueIgnored 한 번이다. 이번 실행 값은 바뀌지 않는다.
TEST(CVar_BootValuesSealIntoASharedBootBlock) {
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    REQUIRE(context.Register(BootWorkers, BootBackend, BootDistance));
    REQUIRE(context.AddLayerText(ConfigLayer::Project, "Game.json", "Game", "project",
                                 R"({"Settings": {"game.boot.workers": 6}})"));
    REQUIRE(context.Sync());
    CHECK(!context.Acquire()->IsSealed());
    REQUIRE(context.CompleteStartup());
    SharedPtr<const ConfigSnapshot> sealed = context.Acquire();
    REQUIRE(sealed->IsSealed());
    CHECK(BootWorkers.Get() == 6 && BootBackend.Get() == "vulkan");
    const CVarSlot* bootSlot = sealed->FindBootSlot(BootWorkers.Index().value);
    REQUIRE(bootSlot != nullptr);

    // 봉인 뒤 바로 쓰는 층은 거부한다(이유와 안내 문구).
    context.Diagnostics().Clear();
    auto console = context.AddAssignment(ConfigLayer::Console, "console", "game.boot.workers", "9");
    CHECK(!console && console.Error() == ErrorCode::InvalidArgument);
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::BootSealed));
    REQUIRE(context.Diagnostics().Count() >= 1);
    CHECK(context.Diagnostics().Get(0).message ==
          "is a boot variable; set it in a config file or with --set and restart");
    auto line = context.ExecuteConsoleLine("game.boot.workers 9", SharedPtr<ConsoleSink>());
    REQUIRE(line);
    CHECK(line.Value() == ConsoleLineResult::Rejected);

    // 파일 층 변경은 다음 실행용 대기다. 이번 실행 값과 부팅 블록은 그대로이고, 다른 변수가 바뀐
    // snapshot도 같은 부팅 블록을 공유한다.
    REQUIRE(context.ReplaceLayerText(ConfigLayer::Project, "Game.json", "Game", "project",
                                     R"({"Settings": {"game.boot.workers": 8}})"));
    REQUIRE(BootDistance.Set(200.f));
    REQUIRE(context.Sync());
    SharedPtr<const ConfigSnapshot> later = context.Acquire();
    REQUIRE(later.Get() != sealed.Get());
    CHECK(later->FindBootSlot(BootWorkers.Index().value) == bootSlot);
    CHECK(BootWorkers.Get() == 6 && BootDistance.Get() == 200.f);
    CHECK(Pending(context, "game.boot.workers", "8"));
    auto workers = context.FindConsoleSetting("game.boot.workers");
    REQUIRE(workers);
    auto info = context.DescribeConsoleSetting(workers.Value());
    REQUIRE(info);
    CHECK(info.Value().boot && info.Value().hasPendingRestart &&
          info.Value().pendingRestart == "8" && info.Value().value == "6");

    // 되돌리면 대기가 없어진다.
    REQUIRE(context.ReplaceLayerText(ConfigLayer::Project, "Game.json", "Game", "project",
                                     R"({"Settings": {"game.boot.workers": 6}})"));
    REQUIRE(context.Sync());
    CHECK(Pending(context, "game.boot.workers", ""));

    // 파생 층은 대기 없이 BootValueIgnored를 같은 이름·층·출처에 한 번만 낸다.
    context.Diagnostics().Clear();
    REQUIRE(context.AddAssignment(ConfigLayer::DeviceProfile, "DeviceProfile", "game.boot.workers",
                                  "12"));
    REQUIRE(context.Sync());
    REQUIRE(context.AddAssignment(ConfigLayer::Feature, "feature", "render.boot.distance", "300"));
    REQUIRE(context.Sync());
    CHECK(CountCode(context, ConfigDiagnosticCode::BootValueIgnored) == 1);
    CHECK(Pending(context, "game.boot.workers", "") && BootWorkers.Get() == 6);
}

// UserSetting 부팅 변수는 봉인 뒤에도 사용자 층 쓰기(다음 실행용)를 받는다 — 재시작 대기에 오르고
// 이번 실행 값은 그대로다. 봉인 뒤에 등록한 부팅 변수는 등록 때 값(대기 값 포함)으로 굳는다.
TEST(CVar_BootUserSettingAndLateRegistration) {
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    REQUIRE(context.Register(BootQuality));
    REQUIRE(context.AddLayerText(ConfigLayer::Project, "Game.json", "Game", "project",
                                 R"({"Settings": {"game.boot.late": 5}})"));
    REQUIRE(context.Sync());
    REQUIRE(context.CompleteStartup());
    REQUIRE(context.AddLayerText(ConfigLayer::User, "Game.json", "Game", "user",
                                 R"({"Settings": {"render.boot.quality": 3}})"));
    REQUIRE(context.Sync());
    CHECK(BootQuality.Get() == 1);
    CHECK(Pending(context, "render.boot.quality", "3"));
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::RestartRequired));

    context.Diagnostics().Clear();
    REQUIRE(context.Register(LateBoot));
    CHECK(LateBoot.Get() == 5);
    CHECK(context.Acquire()->FindBootSlot(LateBoot.Index().value) != nullptr);
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::LateBound));
}

// 봉인 뒤 이름을 몰라 받은 바로 쓰는 층(콘솔·코드)의 값은 그 이름을 늦게 등록할 때 받지 않고
// BootSealed를 남긴다(BootCVar와 CF2 독립 콘솔 설정 모두). 봉인 전 값과 파일·휘발 층 값은 받는다.
// 뒤 Sync에서도 그 값은 닿지 않는다(BootValueIgnored도 없다).
TEST(CVar_LateBootRegistrationRejectsSealedDirectWrites) {
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    REQUIRE(context.AddAssignment(ConfigLayer::Session, "--set", "game.boot.sealed", "4"));
    REQUIRE(context.Sync());
    REQUIRE(context.CompleteStartup());
    REQUIRE(context.AddAssignment(ConfigLayer::Console, "console", "game.boot.sealed", "9"));
    REQUIRE(context.AddAssignment(ConfigLayer::Code, "code", "net.boot.port", "9000"));
    REQUIRE(context.Sync());

    context.Diagnostics().Clear();
    REQUIRE(context.Register(SealedLateBoot));
    CHECK(SealedLateBoot.Get() == 4);
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::BootSealed));
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::LateBound));

    ConsoleSettingDefinition port;
    port.name = "net.boot.port";
    port.help = "Listen port";
    port.type = ConsoleValueType::Int32;
    port.defaultValue = "7777";
    port.flags = static_cast<uint32>(ConsoleSettingFlags::RestartRequired);
    context.Diagnostics().Clear();
    REQUIRE(context.RegisterConsoleSetting(port));
    auto portId = context.FindConsoleSetting("net.boot.port");
    REQUIRE(portId);
    CHECK(context.ConsoleValue(portId.Value()).signedInteger == 7777);
    CHECK(CountCode(context, ConfigDiagnosticCode::BootSealed) == 1);

    context.Diagnostics().Clear();
    REQUIRE(context.AddAssignment(ConfigLayer::Feature, "feature", "render.boot.distance", "1"));
    REQUIRE(context.Sync());
    CHECK(SealedLateBoot.Get() == 4);
    CHECK(context.ConsoleValue(portId.Value()).signedInteger == 7777);
    CHECK(CountCode(context, ConfigDiagnosticCode::BootValueIgnored) == 0);
    CHECK(Pending(context, "game.boot.sealed", "") && Pending(context, "net.boot.port", ""));
}

// 큐에 남은 쓰기가 있으면 봉인은 Busy다(그대로 봉인하면 그 쓰기가 부팅 값에서 빠진다). Sync한 뒤
// 봉인하면 그 값이 부팅 블록에 들어간다.
TEST(CVar_CompleteStartupNeedsSyncedInputs) {
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    REQUIRE(context.Register(QueuedBoot));
    REQUIRE(context.AddAssignment(ConfigLayer::Session, "--set", "game.boot.queued", "7"));
    REQUIRE(context.HasPendingChanges());
    auto early = context.CompleteStartup();
    CHECK(!early && early.Error() == ErrorCode::Busy);
    CHECK(!context.Acquire()->IsSealed());
    REQUIRE(context.Sync());
    REQUIRE(context.CompleteStartup());
    CHECK(context.Acquire()->IsSealed() && QueuedBoot.Get() == 7);
}

// 시작 뒤 남은 대기 이름은 이름마다 한 번 UnresolvedName이고, 가까운 등록 이름이 있으면 제안한다.
// TakeDiagnostics는 진단을 옮기고 문맥의 모음을 비운다.
TEST(CVar_ReportUnresolvedSuggestsNearNames) {
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    REQUIRE(context.Register(BootDistance));
    REQUIRE(context.AddLayerText(ConfigLayer::Project, "Render.json", "Render", "project",
                                 "{\"Settings\": {\"render.boot.distanse\": 5,\n"
                                 "  \"render.unrelated.thing\": 1}}"));
    REQUIRE(context.AddAssignment(ConfigLayer::Session, "--set", "render.boot.distanse", "6"));
    REQUIRE(context.Sync());
    REQUIRE(context.CompleteStartup());
    context.Diagnostics().Clear();
    auto reported = context.ReportUnresolved();
    REQUIRE(reported);
    CHECK(reported.Value() == 2);
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::UnresolvedName, 1, 39));
    bool suggested = false;
    bool plain = false;
    for (usize index = 0; index < context.Diagnostics().Count(); ++index) {
        const ConfigDiagnostic item = context.Diagnostics().Get(index);
        if (item.path == "render.boot.distanse") {
            suggested = item.message == "no console setting or command has this name; did you mean "
                                        "'render.boot.distance'?";
        } else if (item.path == "render.unrelated.thing") {
            plain = item.message == "no console setting or command has this name";
        }
    }

    CHECK(suggested && plain);
    ConfigDiagnostics taken(fixture.system);
    REQUIRE(context.TakeDiagnostics(taken));
    CHECK(taken.Count() == 2 && context.Diagnostics().Count() == 0);

    // 같은 문맥에서 다시 불러도 이미 보고한 이름은 다시 보고하지 않는다.
    auto again = context.ReportUnresolved();
    REQUIRE(again);
    CHECK(again.Value() == 0 && context.Diagnostics().Count() == 0);
}

namespace cvar_boot_test {

Result<void> NoOp(cvar::CommandContext&) noexcept {
    return Result<void>::Success();
}

constinit ConsoleCommand BootFlushCommand =
    cvar::Command<&NoOp>("log.boot.flush").Help("flushes logs");

} // namespace cvar_boot_test

// 콘솔 명령 이름에 넣은 값은 "명령은 값을 받지 않는다"로 알린다(제안 대신).
TEST(CVar_ReportUnresolvedFlagsValuesForCommands) {
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    REQUIRE(context.Register(BootFlushCommand));
    REQUIRE(context.AddLayerText(ConfigLayer::Project, "Game.json", "Game", "project",
                                 R"({"Settings": {"log.boot.flush": 1}})"));
    REQUIRE(context.Sync());
    REQUIRE(context.CompleteStartup());
    context.Diagnostics().Clear();
    auto reported = context.ReportUnresolved();
    REQUIRE(reported);
    CHECK(reported.Value() == 1);
    REQUIRE(context.Diagnostics().Count() == 1);
    const ConfigDiagnostic item = context.Diagnostics().Get(0);
    CHECK(item.code == ConfigDiagnosticCode::UnresolvedName && item.path == "log.boot.flush" &&
          item.message == "is a console command, not a setting; it does not take a value");
}

// TakeDiagnostics는 넘친 수도 옮기고, 문맥의 모음 자신을 out으로 주면 InvalidArgument다.
TEST(CVar_TakeDiagnosticsMovesOmittedCount) {
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    REQUIRE(context.AddLayerText(ConfigLayer::Project, "Game.json", "Game", "project",
                                 R"({"Settings": {"game.boot.one": 1, "game.boot.two": 2}})"));
    REQUIRE(context.Sync());
    REQUIRE(context.CompleteStartup());
    context.Diagnostics().Clear();
    context.Diagnostics().SetLimit(1);
    REQUIRE(context.ReportUnresolved());
    REQUIRE(context.Diagnostics().Count() == 1 && context.Diagnostics().Omitted() == 1);
    CHECK(context.TakeDiagnostics(context.Diagnostics()).Error() == ErrorCode::InvalidArgument);
    CHECK(context.Diagnostics().Count() == 1);

    ConfigDiagnostics taken(fixture.system);
    REQUIRE(context.TakeDiagnostics(taken));
    CHECK(taken.Count() == 1 && taken.Omitted() == 1);
    CHECK(context.Diagnostics().Count() == 0 && context.Diagnostics().Omitted() == 0);
}
