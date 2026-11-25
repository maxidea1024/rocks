#include "../../../Runtime/AssetRuntime/Tests/AssetTestSupport.h"
#include "../../AssetDatabase/Tests/MemoryFileSystem.h"

#include <AssetCooker/AssetCooker.module.generated.h>
#include <AssetCooker/CookConfig.generated.h>
#include <AssetCooker/CookConfig.h>
#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>
#include <string.h>

// 쿠킹 설정 시험이다(Docs/Design/AssetSystem 10.2절 결정 2·3 — A4a): 프로파일 엔진·프로젝트
// 병합(멤버 병합·id 채움), 거부(id 형식·엔진 파일 없음·파일의 id 다름·bulkAlignment·JSON 문법·대입
// 진단·크기 상한), 쿠킹 규칙(기본값·모든 칸·경로 형식·PrimaryAsset 타입·디렉터리), SHA-256(같은
// 내용 같음·칸마다 바뀜), 엔진 기본 프로파일 파일, 읽기 오류, 할당 실패, 텍스처 칸(12.14절 — A5j).
using namespace rocks;

namespace cook_config_tests_file {

using namespace rocks::asset_test;
using reflection_text::TextDiagnostics;
using rocks::asset_database_test::MemoryFault;
using rocks::asset_database_test::MemoryFileSystem;

namespace {

constexpr StringView Root = "cook-config";
constexpr StringView EngineDirectory = "cook-config/Engine";
constexpr StringView ProjectDirectory = "cook-config/Project";

Span<const uint8> Text(const char* text) noexcept {
    return Span<const uint8>(reinterpret_cast<const uint8*>(text), strlen(text));
}

struct ConfigScene {
    ConfigScene() noexcept : files(env.allocator, Root) {
        registered = static_cast<bool>(generated::AssetCooker::RegisterModuleTypes(env.modules));
    }

    bool Ready() const noexcept {
        return env.Ready() && registered && files.Ready();
    }

    CookConfigContext Context(Allocator& allocator) noexcept {
        CookConfigContext context;
        context.allocator = &allocator;
        context.files = &files;
        context.registry = env.source.Get();
        context.access = env.source->OwnerAccess();
        context.types = &env.types;
        context.engineConfigDirectory = EngineDirectory;
        context.projectConfigDirectory = ProjectDirectory;
        return context;
    }

    bool Write(StringView relative, const char* text,
               MemoryFault fault = MemoryFault::None) noexcept {
        files.Remove(relative);
        return files.AddFile(relative, Text(text), fault);
    }

    Result<void> Profile(StringView id, TargetProfile& out, CookConfigFailure* failure) noexcept {
        TextDiagnostics diagnostics(env.allocator);
        return ReadTargetProfile(Context(env.allocator), id, out, diagnostics, failure);
    }

    Result<void> Settings(CookSettings& out, CookConfigFailure* failure,
                          TextDiagnostics* diagnostics = nullptr) noexcept {
        TextDiagnostics local(env.allocator);
        return ReadCookSettings(Context(env.allocator), out,
                                diagnostics != nullptr ? *diagnostics : local, failure);
    }

    AssetEnv env;
    MemoryFileSystem files;
    bool registered = false;
};

// 이 소스 파일 경로에서 저장소의 Engine/Config 경로를 만든다(엔진 기본 프로파일 파일 — 작업
// 디렉터리와 무관하게).
String EngineConfigFromSource(Allocator& allocator) noexcept {
    StringView file = __FILE__;
    String path(allocator);
    // .../Engine/Source/Developer/AssetCooker/Tests/CookConfigTests.cpp → .../Engine
    usize cut = file.ByteCount();
    constexpr int LevelsToEngine = 5;
    for (int level = 0; level < LevelsToEngine; ++level) {
        while (cut > 0 && file.Data()[cut - 1] != '/' && file.Data()[cut - 1] != '\\') {
            --cut;
        }

        if (cut > 0) {
            --cut;
        }
    }

    CHECK(path.TryAppend(StringView(file.Data(), cut)) && path.TryAppend("/Config"));
    return path;
}

} // namespace

// 프로파일: 엔진 파일만(id 채움·기본값), 엔진 위 프로젝트(멤버 병합 — 없는 칸은 엔진 값), 파일의
// id가 같으면 받음, 엔진 기본 파일 둘(Windows·Android)이 실제로 읽힌다.
TEST(CookConfig_ProfileMerge) {
    ConfigScene scene;
    REQUIRE(scene.Ready());
    REQUIRE(scene.Write("Engine/TargetProfiles/Windows.json",
                        "{\"graphicsBackends\": [\"D3D12\"], \"shaderFormats\": "
                        "{\"D3D12\": {\"formats\": [\"D3D12_SM6\"]}}, \"bulkAlignment\": 8192}"));
    TargetProfile profile(scene.env.allocator);
    REQUIRE(scene.Profile("Windows", profile, nullptr));
    CHECK(profile.id.View() == "Windows" && profile.graphicsBackends.Size() == 1 &&
          profile.graphicsBackends[0].View() == "D3D12" &&
          profile.bulkCodec == CookBulkCodec::None && profile.bulkAlignment == 8192);
    REQUIRE(scene.Write("Project/TargetProfiles/Windows.json",
                        "{\"id\": \"Windows\", \"bulkCodec\": \"Zstd\"}"));
    REQUIRE(scene.Profile("Windows", profile, nullptr));
    CHECK(profile.graphicsBackends.Size() == 1 && profile.bulkCodec == CookBulkCodec::Zstd &&
          profile.bulkAlignment == 8192);
    // 다시 읽어도 앞 값이 남지 않는다(기본값에서 시작).
    scene.files.Remove("Project/TargetProfiles/Windows.json");
    REQUIRE(scene.Profile("Windows", profile, nullptr));
    CHECK(profile.bulkCodec == CookBulkCodec::None);

    // 엔진 기본 파일(저장소 Engine/Config).
    String engineConfig = EngineConfigFromSource(scene.env.allocator);
    CookConfigContext real = scene.Context(scene.env.allocator);
    real.files = &platform::PhysicalFileSystem();
    real.engineConfigDirectory = engineConfig.View();
    real.projectConfigDirectory = "cook-config-no-project";
    const char* defaults[] = {"Windows", "Android", "Mac"};
    for (const char* id : defaults) {
        TextDiagnostics diagnostics(scene.env.allocator);
        CookConfigFailure failure;
        TargetProfile engine(scene.env.allocator);
        auto read = ReadTargetProfile(real, id, engine, diagnostics, &failure);
        CHECK(read && engine.id.View() == id);
    }
}

// 프로파일 거부: id 형식(빈·공백·구두점·65 byte), 엔진 파일 없음, 파일의 id가 다름(엔진·프로젝트),
// bulkAlignment(작음·2의 거듭제곱 아님·큼 — 경계는 받음), JSON 문법 위치, 모르는 키·잘못된 값
// (진단), 크기 상한, 읽기 오류.
TEST(CookConfig_ProfileRejects) {
    ConfigScene scene;
    REQUIRE(scene.Ready());
    TargetProfile profile(scene.env.allocator);
    CookConfigFailure failure;
    char longId[TargetProfileIdMaxBytes + 2] = {};
    memset(longId, 'a', TargetProfileIdMaxBytes + 1);
    const char* badIds[] = {"", "Win dows", "../x", "Win-dows", "Win~", longId};
    for (const char* id : badIds) {
        auto read = scene.Profile(id, profile, &failure);
        CHECK(!read && read.Error() == ErrorCode::InvalidArgument &&
              failure.code == AssetDiagnosticCode::InvalidSetting &&
              failure.field == CookConfigField::Id && failure.file == CookConfigFile::None);
    }

    char maxId[TargetProfileIdMaxBytes + 1] = {};
    memset(maxId, 'b', TargetProfileIdMaxBytes);
    CHECK(scene.Profile(maxId, profile, &failure).Error() == ErrorCode::InvalidArgument &&
          failure.file == CookConfigFile::EngineProfile && failure.field == CookConfigField::None);
    // 엔진 파일 없음(프로젝트 파일만 있어도).
    REQUIRE(scene.Write("Project/TargetProfiles/Console.json", "{}"));
    CHECK(scene.Profile("Console", profile, &failure).Error() == ErrorCode::InvalidArgument &&
          failure.code == AssetDiagnosticCode::InvalidSetting &&
          failure.file == CookConfigFile::EngineProfile);

    struct Case {
        const char* engine;
        const char* project;
        CookConfigFile file;
        CookConfigField field;
    };
    const Case cases[] = {
        {"{\"id\": \"Other\"}", nullptr, CookConfigFile::EngineProfile, CookConfigField::Id},
        {"{}", "{\"id\": \"windows\"}", CookConfigFile::ProjectProfile, CookConfigField::Id},
        {"{\"bulkAlignment\": 4}", nullptr, CookConfigFile::None, CookConfigField::BulkAlignment},
        {"{\"bulkAlignment\": 12}", nullptr, CookConfigFile::None, CookConfigField::BulkAlignment},
        {"{\"bulkAlignment\": 131072}", nullptr, CookConfigFile::None,
         CookConfigField::BulkAlignment},
        {"{}", "{\"bulkAlignment\": 3}", CookConfigFile::None, CookConfigField::BulkAlignment},
        {"{\"graphicsBackend\": D3D12}", nullptr, CookConfigFile::EngineProfile,
         CookConfigField::Json},
        {"{\"backend\": \"D3D12\"}", nullptr, CookConfigFile::EngineProfile, CookConfigField::Text},
        {"{\"bulkCodec\": \"Lz4\"}", nullptr, CookConfigFile::EngineProfile, CookConfigField::Text},
        {"{}", "{\"bulkAlignment\": \"big\"}", CookConfigFile::ProjectProfile,
         CookConfigField::Text},
    };
    for (const Case& item : cases) {
        REQUIRE(scene.Write("Engine/TargetProfiles/Windows.json", item.engine));
        scene.files.Remove("Project/TargetProfiles/Windows.json");
        if (item.project != nullptr) {
            REQUIRE(scene.Write("Project/TargetProfiles/Windows.json", item.project));
        }

        TextDiagnostics diagnostics(scene.env.allocator);
        auto read = ReadTargetProfile(scene.Context(scene.env.allocator), "Windows", profile,
                                      diagnostics, &failure);
        CHECK(!read && read.Error() == ErrorCode::InvalidArgument &&
              failure.code == AssetDiagnosticCode::InvalidSetting && failure.file == item.file &&
              failure.field == item.field);
        CHECK((item.field == CookConfigField::Text) == diagnostics.HasErrors());
        if (item.field == CookConfigField::Json) {
            CHECK(failure.line == 1 && failure.column == 21);
        }
    }

    // 경계 값은 받는다.
    const char* accepted[] = {"{\"bulkAlignment\": 0}", "{\"bulkAlignment\": 8}",
                              "{\"bulkAlignment\": 65536}"};
    scene.files.Remove("Project/TargetProfiles/Windows.json");
    for (const char* text : accepted) {
        REQUIRE(scene.Write("Engine/TargetProfiles/Windows.json", text));
        CHECK(scene.Profile("Windows", profile, &failure));
    }

    // 크기 상한·읽기 오류.
    Array<uint8> large(scene.env.allocator);
    REQUIRE(large.TryResize(CookConfigMaxFileBytes + 1));
    memset(large.Data(), ' ', large.Size());
    large[0] = '{';
    large[large.Size() - 1] = '}';
    scene.files.Remove("Engine/TargetProfiles/Windows.json");
    REQUIRE(scene.files.AddFile("Engine/TargetProfiles/Windows.json", large.View()));
    auto tooLarge = scene.Profile("Windows", profile, &failure);
    CHECK(!tooLarge && tooLarge.Error() == ErrorCode::CapacityExceeded &&
          failure.field == CookConfigField::File && failure.code == AssetDiagnosticCode::None);
    REQUIRE(scene.Write("Engine/TargetProfiles/Windows.json", "{}", MemoryFault::Read));
    auto unreadable = scene.Profile("Windows", profile, &failure);
    CHECK(!unreadable && unreadable.Error() != ErrorCode::InvalidArgument &&
          failure.code == AssetDiagnosticCode::None &&
          failure.file == CookConfigFile::EngineProfile);
    REQUIRE(scene.Write("Engine/TargetProfiles/Windows.json", "{}", MemoryFault::Size));
    CHECK(!scene.Profile("Windows", profile, nullptr));
    REQUIRE(scene.Write("Engine/TargetProfiles/Windows.json", "{}", MemoryFault::OpenBusy));
    CHECK(!scene.Profile("Windows", profile, nullptr));
}

// 쿠킹 규칙: 파일이 없으면 기본값, 모든 칸, 경로 형식(에셋·디렉터리·마운트 디렉터리) 판정.
TEST(CookConfig_SettingsFields) {
    ConfigScene scene;
    REQUIRE(scene.Ready());
    CookSettings settings(scene.env.allocator);
    REQUIRE(scene.Settings(settings, nullptr));
    CHECK(settings.alwaysCook.IsEmpty() && settings.neverCook.IsEmpty() &&
          settings.primaryAssets.IsEmpty() && !settings.cookSoftReferences);
    REQUIRE(scene.Write("Project/CookSettings.json",
                        "{\"alwaysCook\": [\"/Game/Maps/\", \"/Game/Boot\"], "
                        "\"neverCook\": [\"/Game/Dev/\"], \"cookSoftReferences\": true, "
                        "\"primaryAssets\": [{\"type\": \"TestAsset\", \"directories\": "
                        "[\"/Game/Items/\", \"/Game/Weapons/\"]}, {\"type\": \"TestAsset\", "
                        "\"directories\": [\"/Game/\"]}]}"));
    REQUIRE(scene.Settings(settings, nullptr));
    CHECK(settings.alwaysCook.Size() == 2 && settings.alwaysCook[1].View() == "/Game/Boot" &&
          settings.neverCook.Size() == 1 && settings.cookSoftReferences &&
          settings.primaryAssets.Size() == 2 &&
          settings.primaryAssets[0].type.View() == "TestAsset" &&
          settings.primaryAssets[0].directories.Size() == 2 &&
          settings.primaryAssets[0].directories[1].View() == "/Game/Weapons/" &&
          settings.primaryAssets[1].directories[0].View() == "/Game/");

    Allocator& allocator = scene.env.allocator;
    struct PathCase {
        const char* text;
        CookPathKind kind;
    };
    const PathCase paths[] = {
        {"/Game/A", CookPathKind::Asset},      {"/game/a/b", CookPathKind::Asset},
        {"/Game/A/", CookPathKind::Directory}, {"/Game/", CookPathKind::Directory},
        {"/game/", CookPathKind::Directory},   {"/Game", CookPathKind::Invalid},
        {"Game/A", CookPathKind::Invalid},     {"/Other/A", CookPathKind::Invalid},
        {"/Game//", CookPathKind::Invalid},    {"/Game/A b/", CookPathKind::Invalid},
        {"", CookPathKind::Invalid},           {"/", CookPathKind::Invalid},
        {"/Other/", CookPathKind::Invalid},
    };
    for (const PathCase& item : paths) {
        auto kind = ClassifyCookPath(allocator, item.text);
        CHECK(kind && kind.Value() == item.kind);
    }

    // 할당 실패는 규칙 위반과 구분한다.
    test::NFailAllocator failing(allocator);
    failing.FailOnAttempt(1);
    auto failed = ClassifyCookPath(failing, "/Game/SomewhatLongerDirectoryName/Asset");
    const bool attempted = failing.Attempts() >= 1;
    failing.DisableFailure();
    CHECK(!attempted || (!failed && failed.Error() == ErrorCode::OutOfMemory));
}

// 쿠킹 규칙 거부: 경로(alwaysCook·neverCook — 순번), PrimaryAsset 타입(모름·struct·Asset 아닌
// 클래스)·디렉터리(에셋 경로·잘못된 경로), JSON 문법·대입 진단(원소 안 배열 포함).
TEST(CookConfig_SettingsRejects) {
    ConfigScene scene;
    REQUIRE(scene.Ready());
    struct Case {
        const char* text;
        CookConfigField field;
        uint32 index;
        uint32 item = 0;
    };
    const Case cases[] = {
        {"{\"alwaysCook\": [\"/Game/A\", \"Game/B\"]}", CookConfigField::AlwaysCook, 1},
        {"{\"neverCook\": [\"/Game\"]}", CookConfigField::NeverCook, 0},
        {"{\"primaryAssets\": [{\"type\": \"NoSuchType\", \"directories\": [\"/Game/\"]}]}",
         CookConfigField::PrimaryAssetType, 0},
        {"{\"primaryAssets\": [{\"type\": \"TestAsset\", \"directories\": [\"/Game/\"]}, "
         "{\"type\": \"KeyedPoint\", \"directories\": [\"/Game/\"]}]}",
         CookConfigField::PrimaryAssetType, 1},
        {"{\"primaryAssets\": [{\"type\": \"Package\", \"directories\": [\"/Game/\"]}]}",
         CookConfigField::PrimaryAssetType, 0},
        {"{\"primaryAssets\": [{\"type\": \"TestAsset\", \"directories\": [\"/Game/Items\"]}]}",
         CookConfigField::PrimaryAssetDirectory, 0},
        {"{\"primaryAssets\": [{\"type\": \"TestAsset\", \"directories\": [\"/Nope/\"]}]}",
         CookConfigField::PrimaryAssetDirectory, 0},
        {"{\"primaryAssets\": [{\"type\": \"TestAsset\", \"directories\": [\"/Game/\", "
         "\"/Game/A/\", \"/Game/B\"]}]}",
         CookConfigField::PrimaryAssetDirectory, 0, 2},
        {"{\"primaryAssets\": [{\"type\": \"TestAsset\", \"directories\": [\"/Game/\"]}, "
         "{\"type\": \"TestAsset\", \"directories\": []}]}",
         CookConfigField::PrimaryAssetDirectory, 1},
        {"{\"primaryAssets\": [{\"type\": \"TestAsset\"}]}", CookConfigField::PrimaryAssetDirectory,
         0},
        {"{\"primaryAssets\": [{\"type\": \"TestAsset\", \"directories\": \"/Game/\"}]}",
         CookConfigField::Text, 0},
        {"{\"alwaysCook\": [\"/Game/A\",]}", CookConfigField::Json, 0},
        {"{\"alwaysCook\": \"/Game/A\"}", CookConfigField::Text, 0},
        {"{\"cookSoft\": true}", CookConfigField::Text, 0},
    };
    for (const Case& item : cases) {
        REQUIRE(scene.Write("Project/CookSettings.json", item.text));
        CookSettings settings(scene.env.allocator);
        CookConfigFailure failure;
        auto read = scene.Settings(settings, &failure);
        CHECK(!read && read.Error() == ErrorCode::InvalidArgument &&
              failure.code == AssetDiagnosticCode::InvalidSetting &&
              failure.file == CookConfigFile::CookSettings && failure.field == item.field &&
              failure.index == item.index && failure.item == item.item);
    }

    REQUIRE(scene.Write("Project/CookSettings.json", "{}", MemoryFault::Read));
    CookSettings settings(scene.env.allocator);
    CookConfigFailure failure;
    auto read = scene.Settings(settings, &failure);
    CHECK(!read && failure.code == AssetDiagnosticCode::None &&
          failure.file == CookConfigFile::CookSettings);
}

// SHA-256: 같은 내용은 같다(다시 읽어도·다른 객체여도), 칸마다(값·배열 원소·순서) 바뀐다,
// 프로파일과 규칙은 서로 다른 값이다.
TEST(CookConfig_Hashes) {
    ConfigScene scene;
    REQUIRE(scene.Ready());
    CookConfigContext context = scene.Context(scene.env.allocator);
    REQUIRE(scene.Write("Engine/TargetProfiles/Windows.json",
                        "{\"graphicsBackends\": [\"D3D12\"], \"shaderFormats\": "
                        "{\"D3D12\": {\"formats\": [\"D3D12_SM6\"]}}}"));
    TargetProfile base(scene.env.allocator);
    REQUIRE(scene.Profile("Windows", base, nullptr));
    auto baseHash = HashTargetProfile(context, base);
    REQUIRE(baseHash);
    TargetProfile again(scene.env.allocator);
    REQUIRE(scene.Profile("Windows", again, nullptr));
    CHECK(HashTargetProfile(context, again).Value() == baseHash.Value());
    Array<Sha256Digest> seen(scene.env.allocator);
    REQUIRE(seen.TryPushBack(baseHash.Value()));
    auto distinct = [&](const Sha256Digest& digest) noexcept {
        for (const Sha256Digest& other : seen) {
            if (other == digest) {
                return false;
            }
        }

        return static_cast<bool>(seen.TryPushBack(digest));
    };
    {
        TargetProfile changed(scene.env.allocator);
        REQUIRE(scene.Profile("Windows", changed, nullptr));
        REQUIRE(changed.id.TryAssign("Android"));
        CHECK(distinct(HashTargetProfile(context, changed).Value()));
        REQUIRE(scene.Profile("Windows", changed, nullptr));
        REQUIRE(changed.graphicsBackends[0].TryAssign("Vulkan"));
        CHECK(distinct(HashTargetProfile(context, changed).Value()));
        REQUIRE(scene.Profile("Windows", changed, nullptr));
        REQUIRE(changed.shaderFormats.Find(StringView("D3D12"))->formats[0].TryAssign("D3D12_SM5"));
        CHECK(distinct(HashTargetProfile(context, changed).Value()));
        REQUIRE(scene.Profile("Windows", changed, nullptr));
        changed.bulkCodec = CookBulkCodec::Zstd;
        CHECK(distinct(HashTargetProfile(context, changed).Value()));
        REQUIRE(scene.Profile("Windows", changed, nullptr));
        changed.bulkAlignment = 8;
        CHECK(distinct(HashTargetProfile(context, changed).Value()));
    }

    CookSettings settings(scene.env.allocator);
    REQUIRE(scene.Settings(settings, nullptr));
    auto settingsHash = HashCookSettings(context, settings);
    REQUIRE(settingsHash);
    CHECK(distinct(settingsHash.Value()));
    const char* variants[] = {
        "{\"alwaysCook\": [\"/Game/A\"]}",
        "{\"alwaysCook\": [\"/Game/B\"]}",
        "{\"alwaysCook\": [\"/Game/A\", \"/Game/B\"]}",
        "{\"alwaysCook\": [\"/Game/B\", \"/Game/A\"]}",
        "{\"neverCook\": [\"/Game/A\"]}",
        "{\"cookSoftReferences\": true}",
        "{\"primaryAssets\": [{\"type\": \"TestAsset\", \"directories\": [\"/Game/\"]}]}",
        "{\"primaryAssets\": [{\"type\": \"TestAsset\", \"directories\": [\"/Game/A/\"]}]}",
    };
    for (const char* text : variants) {
        REQUIRE(scene.Write("Project/CookSettings.json", text));
        CookSettings variant(scene.env.allocator);
        REQUIRE(scene.Settings(variant, nullptr));
        CHECK(distinct(HashCookSettings(context, variant).Value()));
    }
}

// 경계: failure 없이 문법 오류, 숫자·밑줄 id(엔진 파일 없음까지 간다), 설정 타입이 등록되지 않은
// 문맥(읽기·SHA-256 모두 NotFound — 거부 아님).
TEST(CookConfig_Boundaries) {
    ConfigScene scene;
    REQUIRE(scene.Ready());
    REQUIRE(scene.Write("Engine/TargetProfiles/Windows.json", "{"));
    TargetProfile profile(scene.env.allocator);
    CHECK(scene.Profile("Windows", profile, nullptr).Error() == ErrorCode::InvalidArgument);
    CookConfigFailure failure;
    CHECK(scene.Profile("Win_64", profile, &failure).Error() == ErrorCode::InvalidArgument &&
          failure.file == CookConfigFile::EngineProfile && failure.field == CookConfigField::None);

    // failure 없이 디렉터리 거부.
    REQUIRE(scene.Write("Project/CookSettings.json",
                        "{\"primaryAssets\": [{\"type\": \"TestAsset\", \"directories\": "
                        "[\"/Game/A\"]}]}"));
    CookSettings rejected(scene.env.allocator);
    CHECK(scene.Settings(rejected, nullptr).Error() == ErrorCode::InvalidArgument);

    AssetEnv bare;
    REQUIRE(bare.Ready());
    CookConfigContext context = scene.Context(scene.env.allocator);
    context.registry = bare.source.Get();
    context.access = bare.source->OwnerAccess();
    context.types = &bare.types;
    TextDiagnostics diagnostics(scene.env.allocator);
    auto read = ReadTargetProfile(context, "Windows", profile, diagnostics, &failure);
    CHECK(!read && read.Error() == ErrorCode::NotFound &&
          failure.code == AssetDiagnosticCode::None);
    CookSettings settings(scene.env.allocator);
    auto readSettings = ReadCookSettings(context, settings, diagnostics, &failure);
    CHECK(!readSettings && readSettings.Error() == ErrorCode::NotFound &&
          failure.code == AssetDiagnosticCode::None);
    CHECK(HashTargetProfile(context, profile).Error() == ErrorCode::NotFound &&
          HashCookSettings(context, settings).Error() == ErrorCode::NotFound);
}

// 할당 실패: 프로파일·규칙 읽기와 두 SHA-256 — 실패를 주입했으면 적어도 하나가 OutOfMemory이고
// 실패한 것은 모두 OutOfMemory다(규칙 위반으로 바꾸지 않는다). 실패 없는 결과는 시스템 allocator로
// 읽은 기준과 칸·SHA-256까지 같다.
TEST(CookConfig_AllocationFailures) {
    ConfigScene scene;
    REQUIRE(scene.Ready());
    REQUIRE(scene.Write("Engine/TargetProfiles/Windows.json",
                        "{\"graphicsBackends\": [\"D3D12\", \"Vulkan\"], \"shaderFormats\": "
                        "{\"D3D12\": {\"formats\": [\"D3D12_SM6_WITH_A_LONGER_FORMAT_NAME\"]}, "
                        "\"Vulkan\": {\"formats\": [\"VULKAN_SM5\"]}}}"));
    REQUIRE(scene.Write("Project/TargetProfiles/Windows.json", "{\"bulkCodec\": \"Zstd\"}"));
    REQUIRE(scene.Write("Project/CookSettings.json",
                        "{\"alwaysCook\": [\"/Game/Maps/LongerDirectoryName/\"], "
                        "\"neverCook\": [\"/Game/Development/OnlyForEditorTools/\"], "
                        "\"primaryAssets\": [{\"type\": \"TestAsset\", \"directories\": "
                        "[\"/Game/Items/SomewhatLongerName/\", \"/Game/Weapons/\"]}]}"));
    CookConfigContext baseContext = scene.Context(scene.env.allocator);
    TargetProfile baseProfile(scene.env.allocator);
    CookSettings baseSettings(scene.env.allocator);
    REQUIRE(scene.Profile("Windows", baseProfile, nullptr));
    REQUIRE(scene.Settings(baseSettings, nullptr));
    auto baseProfileHash = HashTargetProfile(baseContext, baseProfile);
    auto baseSettingsHash = HashCookSettings(baseContext, baseSettings);
    REQUIRE(baseProfileHash && baseSettingsHash);
    test::NFailAllocator failing(scene.env.allocator);
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        CookConfigContext context = scene.Context(failing);
        TargetProfile profile(failing);
        CookSettings settings(failing);
        TextDiagnostics diagnostics(scene.env.allocator);
        failing.FailOnAttempt(attempt);
        auto readProfile = ReadTargetProfile(context, "Windows", profile, diagnostics, nullptr);
        auto readSettings = ReadCookSettings(context, settings, diagnostics, nullptr);
        // 앞 읽기가 실패하면 SHA-256은 건너뛴다(성공으로 둔다 — 단언은 실패한 호출만 본다).
        auto profileHash = readProfile ? HashTargetProfile(context, profile)
                                       : Result<Sha256Digest>::Success(Sha256Digest{});
        auto settingsHash = readSettings ? HashCookSettings(context, settings)
                                         : Result<Sha256Digest>::Success(Sha256Digest{});
        const bool failed = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!failed) {
            REQUIRE(readProfile && readSettings && profileHash && settingsHash);
            CHECK(profileHash.Value() == baseProfileHash.Value() &&
                  settingsHash.Value() == baseSettingsHash.Value());
            CHECK(profile.id.View() == baseProfile.id.View() &&
                  profile.graphicsBackends.Size() == baseProfile.graphicsBackends.Size() &&
                  profile.shaderFormats.Count() == baseProfile.shaderFormats.Count() &&
                  profile.bulkCodec == baseProfile.bulkCodec &&
                  profile.bulkAlignment == baseProfile.bulkAlignment);
            CHECK(settings.alwaysCook.Size() == 1 && settings.neverCook.Size() == 1 &&
                  settings.primaryAssets.Size() == 1 &&
                  settings.primaryAssets[0].directories.Size() == 2 &&
                  settings.primaryAssets[0].directories[0].View() ==
                      baseSettings.primaryAssets[0].directories[0].View());
            break;
        }

        // 실패한 호출은 모두 OutOfMemory이고 적어도 하나는 실패했다(삼킨 실패 없음).
        CHECK((readProfile || readProfile.Error() == ErrorCode::OutOfMemory) &&
              (readSettings || readSettings.Error() == ErrorCode::OutOfMemory) &&
              (profileHash || profileHash.Error() == ErrorCode::OutOfMemory) &&
              (settingsHash || settingsHash.Error() == ErrorCode::OutOfMemory));
        CHECK(!readProfile || !readSettings || !profileHash || !settingsHash);
        ++failures;
    }

    CHECK(failures >= 5);
}

// 텍스처 칸(12.14절): 기본값(빈 표·norm16 true·표에 없는 그룹은 UE 멤버 기본값), 키별 병합(그룹
// 하나의 칸 하나만 덮음·엔진 그룹 유지·새 그룹은 기본값 뒤 병합), 검증(Min > Max·음수 크기·음수
// maxAniso·OnlyFirstMip·계약 mipGen — index = 그룹 값, 경계는 받음), 모르는 그룹·필터 이름은 대입
// 진단, 엔진 파일 둘의 27행·Android norm16 false, SHA-256이 텍스처 칸을 포함함.
TEST(CookConfig_TextureProfile) {
    ConfigScene scene;
    REQUIRE(scene.Ready());
    TargetProfile profile(scene.env.allocator);
    CookConfigFailure failure;

    // 기본값.
    REQUIRE(scene.Write("Engine/TargetProfiles/Windows.json", "{}"));
    REQUIRE(scene.Profile("Windows", profile, &failure));
    CHECK(profile.textureLodGroups.IsEmpty() && profile.textureNorm16Supported);
    const TextureLodGroup fallback = FindTextureLodGroup(profile, TextureGroup::World);
    CHECK(fallback.minLodSize == 1 && fallback.maxLodSize == 4096 && fallback.lodBias == 0 &&
          fallback.mipGenSettings == TextureMipGenSettings::SimpleAverage &&
          fallback.maxAniso == 0 && !fallback.highPriorityLoad &&
          fallback.mipLoadOptions == TextureMipLoadOptions::AllMips &&
          !fallback.cookPlatformTilingDisabled &&
          fallback.minMagFilter == TextureLodMinMagFilter::Aniso &&
          fallback.mipFilter == TextureLodMipFilter::Point && fallback.numStreamedMips == -1 &&
          fallback.maxLodSizeLow == -1 && fallback.maxLodSizeMedium == -1 &&
          fallback.maxLodSizeHigh == -1 && fallback.maxLodSizeEpic == -1);

    // 키별 병합.
    REQUIRE(scene.Write("Engine/TargetProfiles/Windows.json",
                        "{\"textureLodGroups\": {"
                        "\"World\": {\"maxLodSize\": 16384, \"minMagFilter\": \"Aniso\"},"
                        "\"UI\": {\"mipGenSettings\": \"NoMipmaps\"}}}"));
    REQUIRE(scene.Write("Project/TargetProfiles/Windows.json",
                        "{\"textureLodGroups\": {"
                        "\"World\": {\"lodBias\": 1},"
                        "\"Bokeh\": {\"maxLodSize\": 256, \"mipFilter\": \"Linear\"}},"
                        "\"textureNorm16Supported\": false}"));
    REQUIRE(scene.Profile("Windows", profile, &failure));
    CHECK(profile.textureLodGroups.Count() == 3 && !profile.textureNorm16Supported);
    const TextureLodGroup* world = profile.textureLodGroups.Find(TextureGroup::World);
    const TextureLodGroup* ui = profile.textureLodGroups.Find(TextureGroup::UI);
    const TextureLodGroup* bokeh = profile.textureLodGroups.Find(TextureGroup::Bokeh);
    REQUIRE(world != nullptr && ui != nullptr && bokeh != nullptr);
    CHECK(world->maxLodSize == 16384 && world->lodBias == 1 && world->minLodSize == 1);
    CHECK(ui->mipGenSettings == TextureMipGenSettings::NoMipmaps && ui->maxLodSize == 4096);
    CHECK(bokeh->maxLodSize == 256 && bokeh->mipFilter == TextureLodMipFilter::Linear &&
          bokeh->minMagFilter == TextureLodMinMagFilter::Aniso && bokeh->lodBias == 0);
    scene.files.Remove("Project/TargetProfiles/Windows.json");

    // 검증(index = TextureGroup 값).
    struct Case {
        const char* text;
        TextureGroup group;
    };
    const Case rejects[] = {
        {"{\"textureLodGroups\": {\"World\": {\"minLodSize\": 64, \"maxLodSize\": 32}}}",
         TextureGroup::World},
        {"{\"textureLodGroups\": {\"UI\": {\"minLodSize\": -1}}}", TextureGroup::UI},
        {"{\"textureLodGroups\": {\"Effects\": {\"minLodSize\": 0, \"maxLodSize\": -1}}}",
         TextureGroup::Effects},
        {"{\"textureLodGroups\": {\"Skybox\": {\"maxAniso\": -2}}}", TextureGroup::Skybox},
        {"{\"textureLodGroups\": {\"Bokeh\": {\"mipLoadOptions\": \"OnlyFirstMip\"}}}",
         TextureGroup::Bokeh},
        {"{\"textureLodGroups\": {\"Pixels2D\": {\"mipGenSettings\": \"FromTextureGroup\"}}}",
         TextureGroup::Pixels2D},
        {"{\"textureLodGroups\": {\"8BitData\": {\"mipGenSettings\": \"Sharpen0\"}}}",
         TextureGroup::EightBitData},
        {"{\"textureLodGroups\": {\"16BitData\": {\"mipGenSettings\": \"Blur5\"}}}",
         TextureGroup::SixteenBitData},
    };
    for (const Case& item : rejects) {
        REQUIRE(scene.Write("Engine/TargetProfiles/Windows.json", item.text));
        auto read = scene.Profile("Windows", profile, &failure);
        CHECK(!read && read.Error() == ErrorCode::InvalidArgument &&
              failure.code == AssetDiagnosticCode::InvalidSetting &&
              failure.field == CookConfigField::TextureLodGroup &&
              failure.file == CookConfigFile::None &&
              failure.index == static_cast<uint32>(item.group));
    }

    const char* accepted[] = {
        "{\"textureLodGroups\": {\"World\": {\"minLodSize\": 32, \"maxLodSize\": 32}}}",
        "{\"textureLodGroups\": {\"World\": {\"minLodSize\": 0, \"maxLodSize\": 0}}}",
        "{\"textureLodGroups\": {\"World\": {\"mipGenSettings\": \"Unfiltered\", "
        "\"maxAniso\": 16, \"mipLoadOptions\": \"Default\"}}}",
        "{\"textureLodGroups\": {\"Project01\": {\"numStreamedMips\": 3, \"maxLodSizeLow\": 512, "
        "\"maxLodSizeEpic\": 8192, \"highPriorityLoad\": true, "
        "\"cookPlatformTilingDisabled\": true}}}",
    };
    for (const char* text : accepted) {
        REQUIRE(scene.Write("Engine/TargetProfiles/Windows.json", text));
        CHECK(scene.Profile("Windows", profile, &failure));
    }

    // 모르는 그룹 이름·필터 이름은 대입 진단(UE처럼 aniso로 보지 않는다).
    const char* unknown[] = {
        "{\"textureLodGroups\": {\"Worlds\": {\"lodBias\": 1}}}",
        "{\"textureLodGroups\": {\"World\": {\"minMagFilter\": \"Anisotropic\"}}}",
        "{\"textureLodGroups\": {\"World\": {\"mipFilter\": \"Aniso\"}}}",
        "{\"textureLodGroups\": {\"World\": {\"filter\": \"Point\"}}}",
        "{\"textureNorm16Supported\": 1}",
    };
    for (const char* text : unknown) {
        REQUIRE(scene.Write("Engine/TargetProfiles/Windows.json", text));
        TextDiagnostics diagnostics(scene.env.allocator);
        auto read = ReadTargetProfile(scene.Context(scene.env.allocator), "Windows", profile,
                                      diagnostics, &failure);
        CHECK(!read && failure.field == CookConfigField::Text && diagnostics.HasErrors());
    }

    // SHA-256이 텍스처 칸을 포함한다.
    CookConfigContext context = scene.Context(scene.env.allocator);
    REQUIRE(scene.Write("Engine/TargetProfiles/Windows.json", "{}"));
    REQUIRE(scene.Profile("Windows", profile, nullptr));
    auto plain = HashTargetProfile(context, profile);
    REQUIRE(plain);
    profile.textureNorm16Supported = false;
    auto norm16 = HashTargetProfile(context, profile);
    REQUIRE(norm16);
    CHECK(!(norm16.Value() == plain.Value()));
    profile.textureNorm16Supported = true;
    TextureLodGroup changed;
    changed.maxAniso = 4;
    REQUIRE(profile.textureLodGroups.InsertOrAssign(TextureGroup::World, changed));
    auto grouped = HashTargetProfile(context, profile);
    REQUIRE(grouped);
    CHECK(!(grouped.Value() == plain.Value()) && !(grouped.Value() == norm16.Value()));

    // 엔진 기본 파일(UE 5.8 BaseDeviceProfiles GlobalDefaults 27행).
    String engineConfig = EngineConfigFromSource(scene.env.allocator);
    CookConfigContext real = scene.Context(scene.env.allocator);
    real.files = &platform::PhysicalFileSystem();
    real.engineConfigDirectory = engineConfig.View();
    real.projectConfigDirectory = "cook-config-no-project";
    const char* ids[] = {"Windows", "Android", "Mac"};
    for (const char* id : ids) {
        TextDiagnostics diagnostics(scene.env.allocator);
        TargetProfile engine(scene.env.allocator);
        REQUIRE(ReadTargetProfile(real, id, engine, diagnostics, &failure));
        CHECK(engine.textureLodGroups.Count() == 27);
        CHECK(engine.textureNorm16Supported == (StringView(id) != "Android"));
        const TextureLodGroup* group = engine.textureLodGroups.Find(TextureGroup::World);
        REQUIRE(group != nullptr);
        CHECK(group->maxLodSize == 16384 && group->minMagFilter == TextureLodMinMagFilter::Aniso &&
              group->mipFilter == TextureLodMipFilter::Point);
        group = engine.textureLodGroups.Find(TextureGroup::Effects);
        REQUIRE(group != nullptr);
        CHECK(group->minMagFilter == TextureLodMinMagFilter::Linear);
        group = engine.textureLodGroups.Find(TextureGroup::UI);
        REQUIRE(group != nullptr);
        CHECK(group->mipGenSettings == TextureMipGenSettings::NoMipmaps);
        group = engine.textureLodGroups.Find(TextureGroup::Shadowmap);
        REQUIRE(group != nullptr);
        CHECK(group->numStreamedMips == 3);
        group = engine.textureLodGroups.Find(TextureGroup::Bokeh);
        REQUIRE(group != nullptr);
        CHECK(group->maxLodSize == 256 && group->minMagFilter == TextureLodMinMagFilter::Linear &&
              group->mipFilter == TextureLodMipFilter::Linear);
        group = engine.textureLodGroups.Find(TextureGroup::Pixels2D);
        REQUIRE(group != nullptr);
        CHECK(group->minMagFilter == TextureLodMinMagFilter::Point);
        const TextureGroup dataGroups[] = {TextureGroup::EightBitData,
                                           TextureGroup::SixteenBitData};
        for (TextureGroup data : dataGroups) {
            group = engine.textureLodGroups.Find(data);
            REQUIRE(group != nullptr);
            CHECK(group->minLodSize == 1 && group->maxLodSize == 4096 &&
                  group->minMagFilter == TextureLodMinMagFilter::Point &&
                  group->mipFilter == TextureLodMipFilter::Point &&
                  group->mipGenSettings == TextureMipGenSettings::NoMipmaps);
        }

        // UE 행에 없는 그룹(ColorLookupTable 등)은 표에 없고 기본값이다.
        CHECK(engine.textureLodGroups.Find(TextureGroup::ColorLookupTable) == nullptr &&
              FindTextureLodGroup(engine, TextureGroup::ColorLookupTable).maxLodSize == 4096);
    }
}

} // namespace cook_config_tests_file
