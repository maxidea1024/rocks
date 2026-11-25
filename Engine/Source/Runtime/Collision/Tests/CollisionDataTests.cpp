#include <Base/InitializerList.h>
#include <Config/Tests/ConfigTestSupport.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Move.h>
#include <Core/Text/Json.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/File.h>
#include <TestSupport/Test.h>
#include <Collision/BodySettings.h>
#include <Collision/CollisionProfileTable.h>
#include <Collision/CollisionResponses.h>
#include <Collision/CollisionSettings.generated.h>
#include <Collision/CollisionSettings.h>
#include <Collision/CollisionTypes.h>

// 충돌 데이터 시험이다(Docs/Design/CollisionData.ko.md 7절 — CD1): 채널·응답·충돌 켬 값과 판정,
// 응답 묶음 연산, 엔진 기본 profile 데이터(Engine/Config/Engine.json — UE BaseEngine.ini와 같은
// 내용), 표 생성 규칙과 진단, 실효 충돌 값 해석, BodySettings 기본값, 프로젝트 층 배열 연산.
using namespace rocks;
namespace collision_data_tests_file {

using rocks::test::config::ConfigFixture;

namespace {

constexpr const char* EngineConfigPath = "Engine/Config/Engine.json";

// 기대표의 profile 하나가 바꾸는 칸의 최대 수다(UE 기본 profile 중 가장 많은 것이 8칸).
constexpr usize MaxExpectedOverrides = 8;

struct ExpectedOverride {
    CollisionChannel channel;
    CollisionResponse response;
};

struct ExpectedProfile {
    const char* name;
    CollisionEnabled collisionEnabled;
    CollisionChannel objectType;
    bool canModify;
    usize overrideCount;
    ExpectedOverride overrides[MaxExpectedOverrides];
};

// UE 5.8.4 Engine/Config/BaseEngine.ini:3103-3121의 +Profiles를 옮긴 기대표다(순서·값 그대로,
// WaterBodyCollision의 빈 ObjectTypeName은 WorldStatic). 응답은 기본 묶음(전부 Block, gizmo
// Ignore)에 이 칸을 차례로 적용한 것이다.
// clang-format off
const ExpectedProfile UnrealDefaultProfiles[] = {
    {"NoCollision", CollisionEnabled::NoCollision, CollisionChannel::WorldStatic, false, 2, {{CollisionChannel::Visibility, CollisionResponse::Ignore}, {CollisionChannel::Camera, CollisionResponse::Ignore}}},
    {"BlockAll", CollisionEnabled::QueryAndPhysics, CollisionChannel::WorldStatic, false, 0, {}},
    {"OverlapAll", CollisionEnabled::QueryOnly, CollisionChannel::WorldStatic, false, 8, {{CollisionChannel::WorldStatic, CollisionResponse::Overlap}, {CollisionChannel::Pawn, CollisionResponse::Overlap}, {CollisionChannel::Visibility, CollisionResponse::Overlap}, {CollisionChannel::WorldDynamic, CollisionResponse::Overlap}, {CollisionChannel::Camera, CollisionResponse::Overlap}, {CollisionChannel::PhysicsBody, CollisionResponse::Overlap}, {CollisionChannel::Vehicle, CollisionResponse::Overlap}, {CollisionChannel::Destructible, CollisionResponse::Overlap}}},
    {"BlockAllDynamic", CollisionEnabled::QueryAndPhysics, CollisionChannel::WorldDynamic, false, 0, {}},
    {"OverlapAllDynamic", CollisionEnabled::QueryOnly, CollisionChannel::WorldDynamic, false, 8, {{CollisionChannel::WorldStatic, CollisionResponse::Overlap}, {CollisionChannel::Pawn, CollisionResponse::Overlap}, {CollisionChannel::Visibility, CollisionResponse::Overlap}, {CollisionChannel::WorldDynamic, CollisionResponse::Overlap}, {CollisionChannel::Camera, CollisionResponse::Overlap}, {CollisionChannel::PhysicsBody, CollisionResponse::Overlap}, {CollisionChannel::Vehicle, CollisionResponse::Overlap}, {CollisionChannel::Destructible, CollisionResponse::Overlap}}},
    {"IgnoreOnlyPawn", CollisionEnabled::QueryOnly, CollisionChannel::WorldDynamic, false, 2, {{CollisionChannel::Pawn, CollisionResponse::Ignore}, {CollisionChannel::Vehicle, CollisionResponse::Ignore}}},
    {"OverlapOnlyPawn", CollisionEnabled::QueryOnly, CollisionChannel::WorldDynamic, false, 3, {{CollisionChannel::Pawn, CollisionResponse::Overlap}, {CollisionChannel::Vehicle, CollisionResponse::Overlap}, {CollisionChannel::Camera, CollisionResponse::Ignore}}},
    {"Pawn", CollisionEnabled::QueryAndPhysics, CollisionChannel::Pawn, false, 1, {{CollisionChannel::Visibility, CollisionResponse::Ignore}}},
    {"Spectator", CollisionEnabled::QueryOnly, CollisionChannel::Pawn, false, 8, {{CollisionChannel::WorldStatic, CollisionResponse::Block}, {CollisionChannel::Pawn, CollisionResponse::Ignore}, {CollisionChannel::Visibility, CollisionResponse::Ignore}, {CollisionChannel::WorldDynamic, CollisionResponse::Ignore}, {CollisionChannel::Camera, CollisionResponse::Ignore}, {CollisionChannel::PhysicsBody, CollisionResponse::Ignore}, {CollisionChannel::Vehicle, CollisionResponse::Ignore}, {CollisionChannel::Destructible, CollisionResponse::Ignore}}},
    {"CharacterMesh", CollisionEnabled::QueryOnly, CollisionChannel::Pawn, false, 3, {{CollisionChannel::Pawn, CollisionResponse::Ignore}, {CollisionChannel::Vehicle, CollisionResponse::Ignore}, {CollisionChannel::Visibility, CollisionResponse::Ignore}}},
    {"PhysicsActor", CollisionEnabled::QueryAndPhysics, CollisionChannel::PhysicsBody, false, 0, {}},
    {"Destructible", CollisionEnabled::QueryAndPhysics, CollisionChannel::Destructible, false, 0, {}},
    {"InvisibleWall", CollisionEnabled::QueryAndPhysics, CollisionChannel::WorldStatic, false, 1, {{CollisionChannel::Visibility, CollisionResponse::Ignore}}},
    {"InvisibleWallDynamic", CollisionEnabled::QueryAndPhysics, CollisionChannel::WorldDynamic, false, 1, {{CollisionChannel::Visibility, CollisionResponse::Ignore}}},
    {"Trigger", CollisionEnabled::QueryOnly, CollisionChannel::WorldDynamic, false, 8, {{CollisionChannel::WorldStatic, CollisionResponse::Overlap}, {CollisionChannel::Pawn, CollisionResponse::Overlap}, {CollisionChannel::Visibility, CollisionResponse::Ignore}, {CollisionChannel::WorldDynamic, CollisionResponse::Overlap}, {CollisionChannel::Camera, CollisionResponse::Overlap}, {CollisionChannel::PhysicsBody, CollisionResponse::Overlap}, {CollisionChannel::Vehicle, CollisionResponse::Overlap}, {CollisionChannel::Destructible, CollisionResponse::Overlap}}},
    {"Ragdoll", CollisionEnabled::QueryAndPhysics, CollisionChannel::PhysicsBody, false, 2, {{CollisionChannel::Pawn, CollisionResponse::Ignore}, {CollisionChannel::Visibility, CollisionResponse::Ignore}}},
    {"Vehicle", CollisionEnabled::QueryAndPhysics, CollisionChannel::Vehicle, false, 0, {}},
    {"UI", CollisionEnabled::QueryOnly, CollisionChannel::WorldDynamic, false, 8, {{CollisionChannel::WorldStatic, CollisionResponse::Overlap}, {CollisionChannel::Pawn, CollisionResponse::Overlap}, {CollisionChannel::Visibility, CollisionResponse::Block}, {CollisionChannel::WorldDynamic, CollisionResponse::Overlap}, {CollisionChannel::Camera, CollisionResponse::Overlap}, {CollisionChannel::PhysicsBody, CollisionResponse::Overlap}, {CollisionChannel::Vehicle, CollisionResponse::Overlap}, {CollisionChannel::Destructible, CollisionResponse::Overlap}}},
    {"WaterBodyCollision", CollisionEnabled::QueryOnly, CollisionChannel::WorldStatic, false, 7, {{CollisionChannel::WorldDynamic, CollisionResponse::Overlap}, {CollisionChannel::Pawn, CollisionResponse::Overlap}, {CollisionChannel::Visibility, CollisionResponse::Ignore}, {CollisionChannel::Camera, CollisionResponse::Ignore}, {CollisionChannel::PhysicsBody, CollisionResponse::Overlap}, {CollisionChannel::Vehicle, CollisionResponse::Overlap}, {CollisionChannel::Destructible, CollisionResponse::Overlap}}},
};
// clang-format on

constexpr usize UnrealDefaultProfileCount =
    sizeof(UnrealDefaultProfiles) / sizeof(UnrealDefaultProfiles[0]);

CollisionResponses ExpectedResponses(const ExpectedProfile& profile) noexcept {
    CollisionResponses responses(CollisionResponse::Block);
    (void)responses.Set(GizmoCollisionChannel, CollisionResponse::Ignore);
    for (usize index = 0; index < profile.overrideCount; ++index) {
        (void)responses.Set(profile.overrides[index].channel, profile.overrides[index].response);
    }

    return responses;
}

bool ReadFile(const char* path, Array<uint8>& out) noexcept {
    platform::File file;
    if (!file.Open(path, platform::FileOpenMode::ReadExisting)) {
        return false;
    }

    uint64 size = 0;
    bool ok = static_cast<bool>(file.Size(size)) &&
              static_cast<bool>(out.TryResize(static_cast<usize>(size)));
    if (ok && size != 0) {
        ok = static_cast<bool>(file.ReadExact(out.Data(), static_cast<usize>(size)).status);
    }

    const bool closed = static_cast<bool>(file.Close());
    return ok && closed;
}

// Engine/Config/Engine.json에서 "Collision" 섹션만 떼어 `{ "Collision": … }` 문서로 쓴다. 다른
// 섹션의 설정 struct를 이 시험에 등록하지 않아도 되게 한다.
bool ExtractCollisionSection(Allocator& allocator, StringBuilderBase& out) noexcept {
    Array<uint8> bytes(allocator);
    if (!ReadFile(EngineConfigPath, bytes)) {
        return false;
    }

    JsonDocument document(allocator);
    JsonReadOptions options;
    options.allowLineComments = true;
    const StringView text(reinterpret_cast<const char*>(bytes.Data()), bytes.Size());
    if (!document.Parse(text, options).Succeeded()) {
        return false;
    }

    const JsonValue section = document.Root().Find("Collision");
    if (!section.IsObject()) {
        return false;
    }

    JsonWriter writer(out);
    return writer.BeginObject().IsOk() && writer.Key("Collision").IsOk() &&
           writer.Value(section).IsOk() && writer.EndObject().IsOk() && writer.Finish().IsOk();
}

// Collision 타입과 충돌 설정을 등록한 설정 fixture다.
struct CollisionConfig {
    ConfigFixture fixture;

    CollisionConfig() noexcept {
        REQUIRE(RegisterCollisionTypes(fixture.registration));
        REQUIRE(fixture.Context().RegisterSettings<CollisionSettings>());
    }

    config::ConfigContext& Context() noexcept {
        return fixture.Context();
    }

    bool AddEngineLayer(StringView text) noexcept {
        return Context()
            .AddLayerText(config::ConfigLayer::Engine, "Engine.json", "Engine", "<engine>", text)
            .IsOk();
    }

    bool AddProjectLayer(StringView text) noexcept {
        return Context()
            .AddLayerText(config::ConfigLayer::Project, "Engine.json", "Engine", "<project>", text)
            .IsOk();
    }

    const CollisionSettings& Settings() noexcept {
        return Context().Get<CollisionSettings>();
    }
};

bool HasDiagnostic(const CollisionProfileTable& table, CollisionProfileDiagnosticKind kind,
                   StringView subject, StringView detail) noexcept {
    for (const CollisionProfileDiagnostic& diagnostic : table.Diagnostics()) {
        if (diagnostic.kind == kind && diagnostic.subject.View() == subject &&
            diagnostic.detail.View() == detail) {
            return true;
        }
    }

    return false;
}

bool IsChannel(const Optional<CollisionChannel>& found, CollisionChannel expected) noexcept {
    return found.HasValue() && *found == expected;
}

} // namespace

// 채널 번호와 이름은 UE 5.8과 같다.
TEST(Collision_ChannelValuesMatchUnreal) {
    CHECK(CollisionChannelIndex(CollisionChannel::WorldStatic) == 0u);
    CHECK(CollisionChannelIndex(CollisionChannel::Visibility) == 3u);
    CHECK(CollisionChannelIndex(CollisionChannel::Destructible) == 7u);
    CHECK(CollisionChannelIndex(CollisionChannel::EngineTraceChannel1) == 8u);
    CHECK(CollisionChannelIndex(CollisionChannel::EngineTraceChannel6) == 13u);
    CHECK(CollisionChannelIndex(CollisionChannel::GameTraceChannel1) == 14u);
    CHECK(CollisionChannelIndex(CollisionChannel::GameTraceChannel50) == 63u);
    CHECK(GizmoCollisionChannel == CollisionChannel::EngineTraceChannel1);

    SystemAllocator allocator;
    const CollisionProfileTable table(allocator);
    CHECK(table.ChannelName(CollisionChannel::WorldDynamic) == StringView("WorldDynamic"));
    CHECK(table.ChannelName(CollisionChannel::EngineTraceChannel3) ==
          StringView("EngineTraceChannel3"));
    CHECK(table.ChannelName(CollisionChannel::GameTraceChannel50) ==
          StringView("GameTraceChannel50"));
    CHECK(table.ChannelKind(CollisionChannel::Pawn) == CollisionChannelKind::Object);
    CHECK(table.ChannelKind(CollisionChannel::Camera) == CollisionChannelKind::Trace);
    CHECK(table.ChannelKind(CollisionChannel::GameTraceChannel1) == CollisionChannelKind::Unused);
    CHECK(table.IsStaticObjectChannel(CollisionChannel::WorldStatic));
    CHECK(!table.IsStaticObjectChannel(CollisionChannel::WorldDynamic));
    // 빈 표의 기본 응답: 전부 Block, gizmo만 Ignore.
    CollisionResponses expected(CollisionResponse::Block);
    (void)expected.Set(GizmoCollisionChannel, CollisionResponse::Ignore);
    CHECK(table.DefaultResponses() == expected);
    CHECK(table.Profiles().Size() == 0u);
}

// 충돌 켬 판정과 교집합은 UE 함수와 같다(EngineTypes.h:1847-1900). 행렬은 UE 원문을 옮긴 것이다.
TEST(Collision_EnabledPredicatesAndIntersectionMatchUnreal) {
    using enum CollisionEnabled;
    const CollisionEnabled all[] = {NoCollision,     QueryOnly, PhysicsOnly,
                                    QueryAndPhysics, ProbeOnly, QueryAndProbe};
    const bool query[] = {false, true, false, true, false, true};
    const bool physics[] = {false, false, true, true, false, false};
    const bool probe[] = {false, false, false, false, true, true};
    // UE IntersectionMatrix에 NoCollision 행·열을 더한 6×6이다.
    // clang-format off
    const CollisionEnabled expected[CollisionEnabledCount][CollisionEnabledCount] = {
        {NoCollision, NoCollision, NoCollision, NoCollision,     NoCollision, NoCollision},
        {NoCollision, QueryOnly,   NoCollision, QueryOnly,       NoCollision, QueryOnly},
        {NoCollision, NoCollision, PhysicsOnly, PhysicsOnly,     ProbeOnly,   ProbeOnly},
        {NoCollision, QueryOnly,   PhysicsOnly, QueryAndPhysics, ProbeOnly,   QueryAndProbe},
        {NoCollision, NoCollision, ProbeOnly,   ProbeOnly,       ProbeOnly,   ProbeOnly},
        {NoCollision, QueryOnly,   ProbeOnly,   QueryAndProbe,   ProbeOnly,   QueryAndProbe},
    };
    // clang-format on
    for (usize a = 0; a < CollisionEnabledCount; ++a) {
        CHECK(CollisionEnabledHasQuery(all[a]) == query[a]);
        CHECK(CollisionEnabledHasPhysics(all[a]) == physics[a]);
        CHECK(CollisionEnabledHasProbe(all[a]) == probe[a]);
        for (usize b = 0; b < CollisionEnabledCount; ++b) {
            CHECK(CollisionEnabledIntersection(all[a], all[b]) == expected[a][b]);
        }
    }
}

// 응답 묶음 연산: 한 칸·전부·바꾸기의 변경 여부, 칸마다 최소(9쌍), 비교.
TEST(Collision_ResponsesOperations) {
    CollisionResponses responses;
    CHECK(responses.Get(CollisionChannel::GameTraceChannel50) == CollisionResponse::Block);
    CHECK(responses.Set(CollisionChannel::Pawn, CollisionResponse::Overlap));
    CHECK(!responses.Set(CollisionChannel::Pawn, CollisionResponse::Overlap));
    CHECK(responses.Get(CollisionChannel::Pawn) == CollisionResponse::Overlap);
    CHECK(responses.Replace(CollisionResponse::Overlap, CollisionResponse::Ignore));
    CHECK(responses.Get(CollisionChannel::Pawn) == CollisionResponse::Ignore);
    CHECK(!responses.Replace(CollisionResponse::Overlap, CollisionResponse::Block));
    // 같은 값으로 바꿔도 그 칸이 있었으면 참이다(UE ReplaceChannels — PhysUtils.cpp:234-247).
    CHECK(responses.Replace(CollisionResponse::Ignore, CollisionResponse::Ignore));
    CHECK(responses.SetAll(CollisionResponse::Block));
    CHECK(!responses.SetAll(CollisionResponse::Block));
    CHECK(responses == CollisionResponses(CollisionResponse::Block));
    CHECK(!(responses == CollisionResponses(CollisionResponse::Ignore)));

    using enum CollisionResponse;
    const CollisionResponse values[] = {Ignore, Overlap, Block};
    for (const CollisionResponse a : values) {
        for (const CollisionResponse b : values) {
            CollisionResponses left(Block);
            CollisionResponses right(Block);
            (void)left.Set(CollisionChannel::Vehicle, a);
            (void)right.Set(CollisionChannel::Vehicle, b);
            const CollisionResponses minimum = CollisionResponses::Min(left, right);
            CHECK(minimum.Get(CollisionChannel::Vehicle) == (a < b ? a : b));
            CHECK(minimum.Get(CollisionChannel::Pawn) == Block);
        }
    }
}

// 엔진 기본 데이터는 UE BaseEngine.ini의 profile과 같다: 이름·순서·충돌 켬·object type·canModify,
// profile마다 64칸 응답. 진단은 없다.
TEST(Collision_EngineDefaultProfilesMatchUnreal) {
    SystemAllocator allocator;
    StringBuilder<> text(allocator);
    REQUIRE(ExtractCollisionSection(allocator, text));
    CollisionConfig config;
    REQUIRE(config.AddEngineLayer(text.View()));
    REQUIRE(config.Context().Sync());
    auto table = BuildCollisionProfileTable(config.Settings(), allocator);
    REQUIRE(table);
    CHECK(table.Value().Diagnostics().Size() == 0u);
    REQUIRE(table.Value().Profiles().Size() == UnrealDefaultProfileCount);
    for (usize index = 0; index < UnrealDefaultProfileCount; ++index) {
        const ExpectedProfile& expected = UnrealDefaultProfiles[index];
        const CollisionProfile& profile = table.Value().Profiles()[index];
        CHECK(profile.name.View() == StringView(expected.name));
        CHECK(profile.collisionEnabled == expected.collisionEnabled);
        CHECK(profile.objectType == expected.objectType);
        CHECK(profile.canModify == expected.canModify);
        CHECK(profile.responses == ExpectedResponses(expected));
        CHECK(!profile.helpMessage.IsEmpty());
    }

    // 설명 원문(BaseEngine.ini 그대로 — 끝 공백 포함).
    CHECK(table.Value().Profiles()[0].helpMessage.View() == StringView("No collision"));
    CHECK(table.Value().FindProfile("Pawn")->helpMessage.View() ==
          StringView("Pawn object. Can be used for capsule of any playerable character or AI. "));

    // D2 클래스 기본 profile 셋이 있다(대소문자 무시).
    CHECK(table.Value().FindProfile("BlockAll") != nullptr);
    CHECK(table.Value().FindProfile("overlapalldynamic") != nullptr);
    CHECK(table.Value().FindProfile("TRIGGER") != nullptr);
}

// 표 생성 규칙: 사용자 채널 이름·종류·기본 응답, 범위 밖·이미 이름 있음·같은 이름 채널, 예약·빈
// profile 이름, 같은 이름 profile은 마지막만, 없는·trace·미사용 object type, 없는 응답 채널,
// profileEdits, redirect 둘.
TEST(Collision_TableBuildRulesAndDiagnostics) {
    CollisionConfig config;
    REQUIRE(config.AddEngineLayer(R"({ "Collision": {
        "channels": [
            { "channel": "GameTraceChannel2", "name": "Projectile", "defaultResponse": "Overlap" },
            { "channel": "GameTraceChannel1", "name": "Weapon", "defaultResponse": "Ignore",
              "traceType": true },
            { "channel": "GameTraceChannel3", "name": "Rock", "staticObject": true },
            { "channel": "GameTraceChannel1", "name": "Again", "defaultResponse": "Overlap" },
            { "name": "NoChannel" },
            { "channel": "Pawn", "name": "NotAllowed" },
            { "channel": "GameTraceChannel4", "name": "weapon", "defaultResponse": "Overlap" },
            { "channel": "GameTraceChannel5", "defaultResponse": "Overlap" }
        ],
        "channelRedirects": [
            { "oldName": "Bullet", "newName": "Projectile" },
            { "oldName": "Nowhere", "newName": "NoSuchChannel" }
        ],
        "profiles": [
            { "name": "Shot", "collisionEnabled": "QueryOnly", "objectTypeName": "Bullet",
              "customResponses": [ { "channel": "Pawn", "response": "Overlap" },
                                   { "channel": "NoSuchChannel", "response": "Ignore" } ] },
            { "name": "Custom", "collisionEnabled": "QueryOnly" },
            { "name": "", "collisionEnabled": "QueryOnly" },
            { "name": "Dup", "collisionEnabled": "QueryOnly" },
            { "name": "BadType", "objectTypeName": "NoSuchChannel" },
            { "name": "TraceType", "objectTypeName": "Weapon" },
            { "name": "UnusedType", "objectTypeName": "GameTraceChannel9" },
            { "name": "dup", "collisionEnabled": "PhysicsOnly", "objectTypeName": "Rock" },
            { "name": "Plain" }
        ],
        "profileEdits": [
            { "name": "Shot", "customResponses": [ { "channel": "Camera", "response": "Ignore" } ] }
        ],
        "profileRedirects": [
            { "oldName": "OldShot", "newName": "SHOT" },
            { "oldName": "Gone", "newName": "BadType" },
            { "oldName": "", "newName": "Shot" }
        ]
    } })"));
    REQUIRE(config.Context().Sync());
    SystemAllocator allocator;
    auto built = BuildCollisionProfileTable(config.Settings(), allocator);
    REQUIRE(built);
    const CollisionProfileTable& table = built.Value();

    // 채널: channel 순으로 처리하므로 GameTraceChannel1(Weapon)이 먼저 이름을 갖는다.
    CHECK(table.ChannelName(CollisionChannel::GameTraceChannel1) == StringView("Weapon"));
    CHECK(table.ChannelKind(CollisionChannel::GameTraceChannel1) == CollisionChannelKind::Trace);
    CHECK(table.ChannelName(CollisionChannel::GameTraceChannel2) == StringView("Projectile"));
    CHECK(table.ChannelKind(CollisionChannel::GameTraceChannel2) == CollisionChannelKind::Object);
    CHECK(!table.IsStaticObjectChannel(CollisionChannel::GameTraceChannel2));
    CHECK(table.IsStaticObjectChannel(CollisionChannel::GameTraceChannel3));
    CHECK(HasDiagnostic(table, CollisionProfileDiagnosticKind::ChannelAlreadyNamed, "Again",
                        "GameTraceChannel1"));
    CHECK(HasDiagnostic(table, CollisionProfileDiagnosticKind::ChannelNotConfigurable, "NotAllowed",
                        "Pawn"));
    CHECK(HasDiagnostic(table, CollisionProfileDiagnosticKind::ChannelNameInUse, "weapon",
                        "GameTraceChannel4"));
    CHECK(table.ChannelKind(CollisionChannel::GameTraceChannel4) == CollisionChannelKind::Unused);
    // 이름 없는 항목은 기본 응답만 정한다.
    CHECK(table.ChannelKind(CollisionChannel::GameTraceChannel5) == CollisionChannelKind::Unused);
    CHECK(table.DefaultResponses().Get(CollisionChannel::GameTraceChannel5) ==
          CollisionResponse::Overlap);
    CHECK(table.DefaultResponses().Get(CollisionChannel::GameTraceChannel1) ==
          CollisionResponse::Ignore);
    CHECK(table.DefaultResponses().Get(CollisionChannel::GameTraceChannel2) ==
          CollisionResponse::Overlap);
    // 이미 이름이 있는 채널의 항목(Again)은 기본 응답도 건너뛴다(UE와 같다 — GTC1은 Ignore 그대로).
    // 다른 채널과 같은 이름(weapon)은 이름만 무시하고 기본 응답은 받는다.
    CHECK(table.DefaultResponses().Get(CollisionChannel::GameTraceChannel4) ==
          CollisionResponse::Overlap);
    // 이름 없는 항목은 진단을 남기고, channel을 빠뜨린 항목(기본 WorldStatic)은 설정할 수 없는
    // 채널이다.
    CHECK(HasDiagnostic(table, CollisionProfileDiagnosticKind::UnnamedChannel, "",
                        "GameTraceChannel5"));
    CHECK(HasDiagnostic(table, CollisionProfileDiagnosticKind::ChannelNotConfigurable, "NoChannel",
                        "WorldStatic"));
    // 채널 조회: 대소문자 무시, redirect, 이름 없는 채널은 enum 이름.
    CHECK(IsChannel(table.FindChannel("WEAPON"), CollisionChannel::GameTraceChannel1));
    CHECK(IsChannel(table.FindChannel("bullet"), CollisionChannel::GameTraceChannel2));
    CHECK(IsChannel(table.FindChannel("GameTraceChannel9"), CollisionChannel::GameTraceChannel9));
    CHECK(!table.FindChannel("GameTraceChannel1").HasValue());
    CHECK(HasDiagnostic(table, CollisionProfileDiagnosticKind::InvalidRedirect, "Nowhere",
                        "NoSuchChannel"));

    // profile.
    CHECK(HasDiagnostic(table, CollisionProfileDiagnosticKind::InvalidProfileName, "Custom", ""));
    CHECK(HasDiagnostic(table, CollisionProfileDiagnosticKind::InvalidProfileName, "", ""));
    CHECK(HasDiagnostic(table, CollisionProfileDiagnosticKind::UnknownObjectType, "BadType",
                        "NoSuchChannel"));
    CHECK(HasDiagnostic(table, CollisionProfileDiagnosticKind::ObjectTypeNotObjectChannel,
                        "TraceType", "Weapon"));
    CHECK(HasDiagnostic(table, CollisionProfileDiagnosticKind::ObjectTypeNotObjectChannel,
                        "UnusedType", "GameTraceChannel9"));
    CHECK(HasDiagnostic(table, CollisionProfileDiagnosticKind::UnknownResponseChannel, "Shot",
                        "NoSuchChannel"));
    REQUIRE(table.Profiles().Size() == 3u);
    CHECK(table.Profiles()[0].name.View() == StringView("Shot"));
    CHECK(table.Profiles()[1].name.View() == StringView("dup"));
    CHECK(table.Profiles()[2].name.View() == StringView("Plain"));

    const CollisionProfile* shot = table.FindProfile("shot");
    REQUIRE(shot != nullptr);
    CHECK(shot->objectType == CollisionChannel::GameTraceChannel2);
    CHECK(shot->collisionEnabled == CollisionEnabled::QueryOnly);
    CHECK(shot->responses.Get(CollisionChannel::Pawn) == CollisionResponse::Overlap);
    CHECK(shot->responses.Get(CollisionChannel::Camera) == CollisionResponse::Ignore);
    // 기본 응답이 profile 응답의 시작이다.
    CHECK(shot->responses.Get(CollisionChannel::GameTraceChannel1) == CollisionResponse::Ignore);
    CHECK(shot->responses.Get(CollisionChannel::GameTraceChannel5) == CollisionResponse::Overlap);
    CHECK(shot->responses.Get(GizmoCollisionChannel) == CollisionResponse::Ignore);

    // 같은 이름은 마지막 것(dup — PhysicsOnly, Rock)만 남는다.
    const CollisionProfile* dup = table.FindProfile("DUP");
    REQUIRE(dup != nullptr);
    CHECK(dup->collisionEnabled == CollisionEnabled::PhysicsOnly);
    CHECK(dup->objectType == CollisionChannel::GameTraceChannel3);

    // 빈 objectTypeName = WorldStatic, 기본 collisionEnabled = NoCollision, canModify 기본 참.
    const CollisionProfile* plain = table.FindProfile("Plain");
    REQUIRE(plain != nullptr);
    CHECK(plain->objectType == CollisionChannel::WorldStatic);
    CHECK(plain->collisionEnabled == CollisionEnabled::NoCollision);
    CHECK(plain->canModify);

    // profile redirect.
    CHECK(table.FindProfile("oldshot") == shot);
    CHECK(table.FindProfile("Gone") == nullptr);
    CHECK(HasDiagnostic(table, CollisionProfileDiagnosticKind::InvalidRedirect, "Gone", "BadType"));
    CHECK(HasDiagnostic(table, CollisionProfileDiagnosticKind::InvalidRedirect, "", "Shot"));
    CHECK(table.FindProfile("Custom") == nullptr);
}

// redirect·profileEdits·층을 넘는 같은 이름: redirect는 같은 옛 이름이면 마지막 것, profile 이름과
// 겹치면 redirect가 먼저(UE ReadConfig), edit는 같은 이름의 첫 것만 customResponses 뒤에 적용,
// 없는 profile의 edit는 무시, 프로젝트 층이 $add로 넣은 같은 이름 profile이 엔진 것을 대신한다(끝
// 자리).
TEST(Collision_RedirectEditAndLayerOverrideRules) {
    CollisionConfig config;
    REQUIRE(config.AddEngineLayer(R"({ "Collision": {
        "channels": [ { "channel": "GameTraceChannel1", "name": "Weapon" },
                      { "channel": "GameTraceChannel2", "name": "Rock" } ],
        "channelRedirects": [ { "oldName": "Gun", "newName": "Weapon" },
                              { "oldName": "gun", "newName": "Rock" } ],
        "profiles": [
            { "name": "A", "collisionEnabled": "QueryOnly",
              "customResponses": [ { "channel": "Pawn", "response": "Overlap" } ] },
            { "name": "B", "collisionEnabled": "PhysicsOnly" },
            { "name": "C", "collisionEnabled": "NoCollision" }
        ],
        "profileEdits": [
            { "name": "a", "customResponses": [ { "channel": "Pawn", "response": "Ignore" } ] },
            { "name": "A", "customResponses": [ { "channel": "Pawn", "response": "Block" },
                                               { "channel": "Camera", "response": "Ignore" } ] },
            { "name": "Missing", "customResponses": [ { "channel": "Pawn", "response": "Ignore" } ] }
        ],
        "profileRedirects": [ { "oldName": "Old", "newName": "A" },
                              { "oldName": "OLD", "newName": "B" },
                              { "oldName": "C", "newName": "A" } ]
    } })"));
    REQUIRE(config.AddProjectLayer(R"({ "Collision": {
        "profiles": { "$add": [ { "name": "A", "collisionEnabled": "QueryAndPhysics",
                                  "objectTypeName": "WorldDynamic" } ] }
    } })"));
    REQUIRE(config.Context().Sync());
    SystemAllocator allocator;
    auto built = BuildCollisionProfileTable(config.Settings(), allocator);
    REQUIRE(built);
    const CollisionProfileTable& table = built.Value();
    CHECK(table.Diagnostics().Size() == 0u);

    // 같은 이름은 마지막(프로젝트 층) A만 남고 자리는 끝이다. 첫 edit만 적용된다(Pawn Ignore).
    REQUIRE(table.Profiles().Size() == 3u);
    CHECK(table.Profiles()[0].name.View() == StringView("B"));
    CHECK(table.Profiles()[1].name.View() == StringView("C"));
    CHECK(table.Profiles()[2].name.View() == StringView("A"));
    const CollisionProfile* a = table.FindProfile("A");
    REQUIRE(a != nullptr);
    CHECK(a->collisionEnabled == CollisionEnabled::QueryAndPhysics);
    CHECK(a->objectType == CollisionChannel::WorldDynamic);
    CHECK(a->responses.Get(CollisionChannel::Pawn) == CollisionResponse::Ignore);
    CHECK(a->responses.Get(CollisionChannel::Camera) == CollisionResponse::Block);

    // redirect: 같은 옛 이름은 마지막(B), profile 이름과 겹치면 redirect(C → A)가 먼저다.
    CHECK(table.FindProfile("old") == table.FindProfile("B"));
    CHECK(table.FindProfile("C") == a);
    CHECK(IsChannel(table.FindChannel("GUN"), CollisionChannel::GameTraceChannel2));

    // body 사용자 지정 응답의 채널 이름도 redirect를 따른다.
    BodySettings body(allocator);
    ChannelResponse gun(allocator);
    REQUIRE(gun.channel.TryAssign("Gun"));
    gun.response = CollisionResponse::Ignore;
    REQUIRE(body.customResponses.TryPushBack(Move(gun)));
    const ResolvedCollision custom = ResolveCollision(table, CustomCollisionProfileName, body);
    CHECK(custom.responses.Get(CollisionChannel::GameTraceChannel2) == CollisionResponse::Ignore);
    CHECK(custom.responses.Get(CollisionChannel::GameTraceChannel1) == CollisionResponse::Block);

    // 사용자 지정으로 옮길 때 이름 있는 채널은 사용자 이름, 이름 없는 채널은 enum 이름으로 적는다.
    ResolvedCollision changed = custom;
    (void)changed.responses.Set(CollisionChannel::GameTraceChannel1, CollisionResponse::Overlap);
    (void)changed.responses.Set(CollisionChannel::GameTraceChannel9, CollisionResponse::Overlap);
    BodySettings moved(allocator);
    REQUIRE(MakeCustomCollision(table, changed, moved));
    REQUIRE(moved.customResponses.Size() == 3u);
    CHECK(moved.customResponses[0].channel.View() == StringView("Weapon"));
    CHECK(moved.customResponses[1].channel.View() == StringView("Rock"));
    CHECK(moved.customResponses[2].channel.View() == StringView("GameTraceChannel9"));
    CHECK(moved.customResponses[2].response == CollisionResponse::Overlap);
}

// 실효 값 해석: profile, Custom, 없는 이름, redirect, 사용자 지정으로 옮기기 왕복.
TEST(Collision_ResolveProfileCustomAndMissing) {
    CollisionConfig config;
    REQUIRE(config.AddEngineLayer(R"({ "Collision": {
        "channels": [ { "channel": "GameTraceChannel1", "name": "Weapon",
                        "defaultResponse": "Overlap" } ],
        "profiles": [ { "name": "Pawnish", "collisionEnabled": "QueryAndPhysics",
                        "objectTypeName": "Pawn",
                        "customResponses": [ { "channel": "Visibility", "response": "Ignore" } ] } ],
        "profileRedirects": [ { "oldName": "OldPawnish", "newName": "Pawnish" } ]
    } })"));
    REQUIRE(config.Context().Sync());
    SystemAllocator allocator;
    auto built = BuildCollisionProfileTable(config.Settings(), allocator);
    REQUIRE(built);
    const CollisionProfileTable& table = built.Value();

    BodySettings body(allocator);
    body.collisionEnabled = CollisionEnabled::QueryOnly;
    body.objectType = CollisionChannel::WorldDynamic;
    ChannelResponse cameraIgnore(allocator);
    REQUIRE(cameraIgnore.channel.TryAssign("camera"));
    cameraIgnore.response = CollisionResponse::Ignore;
    REQUIRE(body.customResponses.TryPushBack(Move(cameraIgnore)));
    ChannelResponse unknown(allocator);
    REQUIRE(unknown.channel.TryAssign("NoSuchChannel"));
    unknown.response = CollisionResponse::Ignore;
    REQUIRE(body.customResponses.TryPushBack(Move(unknown)));

    // profile 값.
    const ResolvedCollision fromProfile = ResolveCollision(table, "pawnish", body);
    CHECK(fromProfile.fromProfile);
    CHECK(!fromProfile.profileMissing);
    CHECK(fromProfile.profileName == StringView("Pawnish"));
    CHECK(fromProfile.objectType == CollisionChannel::Pawn);
    CHECK(fromProfile.collisionEnabled == CollisionEnabled::QueryAndPhysics);
    CHECK(fromProfile.responses.Get(CollisionChannel::Visibility) == CollisionResponse::Ignore);
    CHECK(fromProfile.responses.Get(CollisionChannel::GameTraceChannel1) ==
          CollisionResponse::Overlap);
    // redirect 이름은 새 profile로 간다.
    const ResolvedCollision redirected = ResolveCollision(table, "OldPawnish", body);
    CHECK(redirected.fromProfile);
    CHECK(redirected.profileName == StringView("Pawnish"));

    // Custom: body 값(+ 기본 묶음, 없는 채널 이름은 건너뜀).
    for (const StringView name : {StringView("Custom"), StringView("custom"), StringView()}) {
        const ResolvedCollision custom = ResolveCollision(table, name, body);
        CHECK(!custom.fromProfile);
        CHECK(!custom.profileMissing);
        CHECK(custom.collisionEnabled == CollisionEnabled::QueryOnly);
        CHECK(custom.objectType == CollisionChannel::WorldDynamic);
        CHECK(custom.responses.Get(CollisionChannel::Camera) == CollisionResponse::Ignore);
        CHECK(custom.responses.Get(CollisionChannel::GameTraceChannel1) ==
              CollisionResponse::Overlap);
        CHECK(custom.responses.Get(CollisionChannel::Pawn) == CollisionResponse::Block);
    }

    // 없는 이름: Custom 값 + profileMissing.
    const ResolvedCollision missing = ResolveCollision(table, "NoSuchProfile", body);
    CHECK(!missing.fromProfile);
    CHECK(missing.profileMissing);
    CHECK(missing.objectType == CollisionChannel::WorldDynamic);

    // 사용자 지정으로 옮기기: 기본 묶음과 다른 칸만 채널 번호 순으로 남고, Custom 해석이 같은
    // 값이다.
    BodySettings moved(allocator);
    REQUIRE(MakeCustomCollision(table, fromProfile, moved));
    CHECK(moved.collisionEnabled == CollisionEnabled::QueryAndPhysics);
    CHECK(moved.objectType == CollisionChannel::Pawn);
    REQUIRE(moved.customResponses.Size() == 1u);
    CHECK(moved.customResponses[0].channel.View() == StringView("Visibility"));
    CHECK(moved.customResponses[0].response == CollisionResponse::Ignore);
    const ResolvedCollision roundTrip = ResolveCollision(table, CustomCollisionProfileName, moved);
    CHECK(roundTrip.responses == fromProfile.responses);
    CHECK(roundTrip.collisionEnabled == fromProfile.collisionEnabled);
    CHECK(roundTrip.objectType == fromProfile.objectType);
}

// 빈 표(설정 문맥이 없는 World)에서는 모든 이름이 profileMissing이고 body 값을 쓴다.
TEST(Collision_EmptyTableResolvesEveryNameAsMissing) {
    SystemAllocator allocator;
    const CollisionProfileTable table(allocator);
    const BodySettings body(allocator);
    const ResolvedCollision resolved = ResolveCollision(table, "BlockAll", body);
    CHECK(resolved.profileMissing);
    CHECK(!resolved.fromProfile);
    CHECK(resolved.collisionEnabled == CollisionEnabled::QueryAndPhysics);
    CHECK(resolved.objectType == CollisionChannel::WorldStatic);
    CHECK(resolved.responses == table.DefaultResponses());
}

// BodySettings 기본값은 UE 생성자와 같다(Docs/Design/CollisionData.ko.md 6절 표).
TEST(Collision_BodySettingsDefaultsMatchUnreal) {
    SystemAllocator allocator;
    const BodySettings body(allocator);
    CHECK(body.collisionEnabled == CollisionEnabled::QueryAndPhysics);
    CHECK(body.objectType == CollisionChannel::WorldStatic);
    CHECK(body.customResponses.Size() == 0u);
    CHECK(!body.simulatePhysics);
    CHECK(body.startAwake);
    CHECK(body.enableGravity);
    CHECK(!body.overrideMass);
    CHECK(body.massInKgOverride == 100.0f);
    CHECK(body.massScale == 1.0f);
    CHECK(body.centerOfMassOffset == Vec3d(0.0, 0.0, 0.0));
    CHECK(body.inertiaTensorScale == Vec3d(1.0, 1.0, 1.0));
    CHECK(body.updateMassWhenScaleChanges);
    CHECK(body.linearDamping == 0.01f);
    CHECK(body.angularDamping == 0.0f);
    CHECK(!body.gyroscopicTorque);
    CHECK(!body.overrideMaxAngularVelocity);
    CHECK(body.maxAngularVelocity == 3600.0f);
    CHECK(!body.continuousCollision);
    CHECK(!body.smoothEdgeCollisions);
    CHECK(!body.overrideSolverIterations);
    CHECK(body.positionIterations == 8u);
    CHECK(body.velocityIterations == 2u);
    CHECK(!body.autoWeld);
    CHECK(!body.notifyRigidBodyCollision);
    CHECK(!body.generateWakeEvents);
    CHECK(body.dofMode == DofMode::Default);
    CHECK(body.lockTranslation);
    CHECK(body.lockRotation);
    CHECK(!body.lockXTranslation && !body.lockYTranslation && !body.lockZTranslation);
    CHECK(!body.lockXRotation && !body.lockYRotation && !body.lockZRotation);
    CHECK(body.customDofPlaneNormal == Vec3d(0.0, 0.0, 0.0));
    CHECK(!body.overrideWalkableSlope);
    CHECK(body.walkableSlope == WalkableSlope{});
}

// 프로젝트 층이 배열 연산($add)으로 엔진 profile 위에 채널·profile을 더한다.
TEST(Collision_ProjectLayerAddsChannelsAndProfiles) {
    SystemAllocator allocator;
    StringBuilder<> text(allocator);
    REQUIRE(ExtractCollisionSection(allocator, text));
    CollisionConfig config;
    REQUIRE(config.AddEngineLayer(text.View()));
    REQUIRE(config.AddProjectLayer(R"({ "Collision": {
        "channels": { "$add": [ { "channel": "GameTraceChannel1", "name": "Weapon",
                                  "defaultResponse": "Ignore", "traceType": true } ] },
        "profiles": { "$add": [ { "name": "Projectile", "collisionEnabled": "QueryOnly",
                                  "objectTypeName": "WorldDynamic",
                                  "customResponses": [ { "channel": "Weapon",
                                                         "response": "Block" } ] } ] }
    } })"));
    REQUIRE(config.Context().Sync());
    auto table = BuildCollisionProfileTable(config.Settings(), allocator);
    REQUIRE(table);
    CHECK(table.Value().Diagnostics().Size() == 0u);
    CHECK(table.Value().Profiles().Size() == UnrealDefaultProfileCount + 1);
    // 사용자 채널의 기본 응답이 엔진 profile에도 들어간다(UE: 새 채널은 profile마다 기본 응답).
    const CollisionProfile* blockAll = table.Value().FindProfile("BlockAll");
    REQUIRE(blockAll != nullptr);
    CHECK(blockAll->responses.Get(CollisionChannel::GameTraceChannel1) ==
          CollisionResponse::Ignore);
    const CollisionProfile* projectile = table.Value().FindProfile("Projectile");
    REQUIRE(projectile != nullptr);
    CHECK(projectile->responses.Get(CollisionChannel::GameTraceChannel1) ==
          CollisionResponse::Block);
    CHECK(projectile->objectType == CollisionChannel::WorldDynamic);
}

} // namespace collision_data_tests_file
