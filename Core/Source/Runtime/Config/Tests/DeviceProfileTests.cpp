#include "ConfigTestSupport.h"

#include <Config/DeviceProfileSettings.generated.h>
#include <Config/DeviceProfiles.h>

// 설정 CF4 기기 프로필 시험이다(Docs/Design/ConfigSystem.ko.md 6.2절): 기기 조건 판정, 프로필 선택
// 순서(명령줄 → 프로젝트 → 규칙 → 플랫폼 기본, 봉인 모드는 규칙 없음), 부모 사슬·메모리 등급
// 우선순위와 순환 진단, 프로필을 바꾸면 옛 값의 Unset.
using namespace rocks;
using namespace rocks::config;
using namespace rocks::test::config;

namespace {

bool HasCode(ConfigFixture& fixture, ConfigDiagnosticCode code, ConfigSeverity severity,
             StringView path) noexcept {
    const ConfigDiagnostics& all = fixture.Context().Diagnostics();
    for (usize index = 0; index < all.Count(); ++index) {
        const ConfigDiagnostic item = all.Get(index);
        if (item.code == code && item.severity == severity && item.path == path) {
            return true;
        }
    }

    fixture.PrintDiagnostics();
    return false;
}

constexpr StringView ProfileDefinitions = R"({"DeviceProfiles": {
  "memoryBuckets": [{"name": "Large", "minimumMiB": 16384}, {"name": "Small", "minimumMiB": 0}],
  "platformDefaults": {"Windows": "Desktop"},
  "profiles": {
    "Desktop": {"platforms": ["Windows"],
                "values": {"Game.Display.frameCap": "60", "game.rules.score_limit": "10",
                           "Game.Gameplay.lives": "3"},
                "memoryValues": {"Small": {"values": {"Game.Display.frameCap": "30",
                                                     "Game.Gameplay.lives": "2"}}}},
    "Desktop_Low": {"parent": "Desktop", "platforms": ["Windows"], "matchPriority": 5,
                    "match": [{"trait": "gpu.memory_mib", "below": 2048}],
                    "values": {"Game.Gameplay.lives": "1"},
                    "memoryValues": {"Small": {"values": {"game.rules.score_limit": "5"}}}},
    "Desktop_Mid": {"parent": "Desktop", "platforms": ["Windows"], "matchPriority": 5,
                    "match": [{"trait": "gpu.memory_mib", "atLeast": 1024}, {"trait": "gpu.name", "contains": "radeon"}],
                    "values": {"Game.Gameplay.lives": "4"}},
    "Mobile_Only": {"platforms": ["Android"], "match": [{"trait": "gpu.memory_mib", "below": 99999}]},
    "Loop_A": {"parent": "Loop_B"},
    "Loop_B": {"parent": "Loop_A"}}}})";

struct ProfileFixture {
    explicit ProfileFixture(ConfigMode mode) noexcept : fixture(Options(mode)), traits(system) {
        ConfigContext& context = fixture.Context();
        REQUIRE(RegisterDeviceProfileSettings(context));
        REQUIRE(context.RegisterSettings<DisplaySettings>());
        REQUIRE(context.AddLayerText(ConfigLayer::Engine, "DeviceProfiles.json", "DeviceProfiles",
                                     "engine/DeviceProfiles.json", ProfileDefinitions));
        REQUIRE(context.Sync());
        REQUIRE(traits.SetText(DeviceTraitNames::Platform, "Windows"));
        REQUIRE(traits.SetNumber(DeviceTraitNames::SystemMemoryMiB, 8192));
        REQUIRE(traits.SetNumber(DeviceTraitNames::GpuMemoryMiB, 1024));
        REQUIRE(traits.SetText(DeviceTraitNames::GpuName, "AMD Radeon 780M"));
    }

    static ConfigContextOptions Options(ConfigMode mode) noexcept {
        ConfigContextOptions options;
        options.mode = mode;
        options.platform = "Windows";
        return options;
    }

    // 프로필을 골라 적용하고 Sync한다.
    DeviceProfileSelection Apply(StringView commandLine) noexcept {
        DeviceProfileSelection selected(system);
        CHECK(ApplyDeviceProfile(fixture.Context(), commandLine, traits, selected));
        CHECK(fixture.Context().Sync());
        return selected;
    }

    SystemAllocator system;
    ConfigFixture fixture;
    DeviceTraits traits;
};
} // namespace

TEST(Config_DeviceConditionsMatchTraits) {
    SystemAllocator allocator;
    DeviceTraits traits(allocator);
    REQUIRE(traits.SetText(DeviceTraitNames::GpuName, "NVIDIA GeForce RTX 4070"));
    REQUIRE(traits.SetNumber(DeviceTraitNames::GpuMemoryMiB, 12288));
    CHECK(!traits.SetNumber("Bad Name", 1));
    volatile double zero = 0.0;
    CHECK(!traits.SetNumber(DeviceTraitNames::GpuMemoryMiB, zero / zero));
    DeviceCondition condition(allocator);
    String reason(allocator);
    REQUIRE(condition.trait.TryAssign(DeviceTraitNames::GpuName));
    REQUIRE(condition.contains.TryAssign("geforce"));
    auto matched = MatchDeviceCondition(traits, condition, reason);
    CHECK(matched && matched.Value());
    REQUIRE(condition.equals.TryAssign("nvidia geforce rtx 4070"));
    matched = MatchDeviceCondition(traits, condition, reason);
    CHECK(matched && matched.Value());
    condition.atLeast = 1.0; // 문자열 특성에 수 조건
    matched = MatchDeviceCondition(traits, condition, reason);
    CHECK(matched && !matched.Value());

    DeviceCondition memory(allocator);
    REQUIRE(memory.trait.TryAssign(DeviceTraitNames::GpuMemoryMiB));
    memory.atLeast = 12288.0;
    memory.below = 16384.0;
    matched = MatchDeviceCondition(traits, memory, reason);
    CHECK(matched && matched.Value());
    memory.below = 12288.0;
    matched = MatchDeviceCondition(traits, memory, reason);
    CHECK(matched && !matched.Value() && !reason.IsEmpty());
    REQUIRE(memory.trait.TryAssign("gpu.unknown_trait"));
    matched = MatchDeviceCondition(traits, memory, reason);
    CHECK(matched && !matched.Value());
}

// 선택 순서: 명령줄 → 프로젝트 → 규칙(이 플랫폼의 후보 가운데 matchPriority, 같으면 이름 순서) →
// 플랫폼 기본. 정의에 없는 이름은 진단 후 다음 단계다. 봉인 모드는 규칙을 쓰지 않는다.
TEST(Config_DeviceProfileSelectionOrder) {
    ProfileFixture profile(ConfigMode::Standard);
    ConfigFixture& fixture = profile.fixture;
    // 규칙: Desktop_Low(memory < 2048)와 Desktop_Mid(≥ 1024, radeon)가 맞고 우선순위가 같다 →
    // 이름 순서로 Desktop_Low. Mobile_Only는 다른 플랫폼이라 후보가 아니다.
    DeviceProfileSelection selected = profile.Apply({});
    CHECK(selected.profile.View() == "Desktop_Low" && selected.source == DeviceProfileSource::Rule);
    CHECK(selected.memoryBucket.View() == "Small");

    selected = profile.Apply("Desktop_Mid");
    CHECK(selected.profile.View() == "Desktop_Mid" &&
          selected.source == DeviceProfileSource::CommandLine);

    // 모르는 명령줄 이름 → 진단, 프로젝트 지정으로.
    REQUIRE(fixture.Context().AddAssignment(ConfigLayer::Project, "project",
                                            "DeviceProfiles.DeviceProfiles.profile", "Desktop"));
    REQUIRE(fixture.Context().Sync());
    fixture.Context().Diagnostics().Clear();
    selected = profile.Apply("Nope");
    CHECK(selected.profile.View() == "Desktop" && selected.source == DeviceProfileSource::Project);
    CHECK(HasCode(fixture, ConfigDiagnosticCode::InvalidDeviceProfile, ConfigSeverity::Error,
                  "Nope"));

    const StringView arguments[] = {"--frames", "--device-profile=A", "--device-profile=B"};
    CHECK(FindDeviceProfileArgument(arguments) == "B");
    CHECK(FindDeviceProfileArgument({}).IsEmpty());

    // 봉인 모드: 규칙 없이 플랫폼 기본(platformDefaults[Windows] = Desktop).
    // (선택 결과의 문자열은 그 fixture의 allocator 것이므로 fixture보다 먼저 사라지게 범위 안에
    // 둔다.)
    ProfileFixture sealed(ConfigMode::Sealed);
    {
        const DeviceProfileSelection fallback = sealed.Apply({});
        CHECK(fallback.profile.View() == "Desktop" &&
              fallback.source == DeviceProfileSource::PlatformDefault);
    }
}

// 상속: 자식 등급 값 > 자식 값 > 부모 등급 값 > 부모 값. 프로필을 바꾸면 옛 값은 Unset된다. 부모
// 순환은 진단 후 멈춘다.
TEST(Config_DeviceProfileInheritanceAndMemoryBuckets) {
    ProfileFixture profile(ConfigMode::Standard);
    ConfigContext& context = profile.fixture.Context();
    DeviceProfileSelection selected = profile.Apply("Desktop_Low");
    const GameplaySettings& gameplay = profile.fixture.Gameplay();
    const DisplaySettings& display = context.Get<DisplaySettings>();
    CHECK(gameplay.scoreLimit == 5); // 자식 등급 값(부모 값 10보다 강하다)
    CHECK(gameplay.lives == 1);      // 자식 값(부모 등급 값 2보다 강하다)
    CHECK(display.frameCap == 30);   // 부모 등급 값(부모 값 60보다 강하다)
    CHECK(selected.values == 3);
    const ConfigLayer profileTop[] = {ConfigLayer::DeviceProfile};
    CHECK(profile.fixture.SourcesAre("Game", "Gameplay", "lives", profileTop));

    // 메모리 등급 Large: 등급 값이 없다.
    REQUIRE(profile.traits.SetNumber(DeviceTraitNames::SystemMemoryMiB, 32768));
    selected = profile.Apply("Desktop_Mid");
    CHECK(selected.memoryBucket.View() == "Large");
    CHECK(gameplay.lives == 4 && gameplay.scoreLimit == 10 && display.frameCap == 60);
    CHECK(profile.fixture.SourcesAre("Game", "Gameplay", "lives", profileTop)); // 옛 값은 빠졌다

    context.Diagnostics().Clear();
    selected = profile.Apply("Loop_A");
    CHECK(selected.profile.View() == "Loop_A");
    CHECK(HasCode(profile.fixture, ConfigDiagnosticCode::InvalidDeviceProfile,
                  ConfigSeverity::Error, "Loop_A"));
    CHECK(gameplay.lives == 3); // 코드 기본값으로 돌아갔다
}

// 기기 프로필이 품질 등급 기본값을 정하고(품질 구독이 다시 적용한다) 사용자가 고른 등급이 그보다
// 강하다.
