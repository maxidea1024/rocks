#include "../../AssetDatabase/Tests/MemoryFileSystem.h"

#include <Base/InitializerList.h>
#include <Core/Memory/SystemAllocator.h>
#include <HostConfig/HostSettings.h>
#include <Object/ModuleRegistration.h>
#include <Object/ObjectRegistry.h>
#include <Object/TypeRegistry.h>
#include <Meta/GeneratedType.h>
#include <TargetProfiles/TargetProfile.generated.h>
#include <TargetProfiles/TargetProfile.h>
#include <TargetProfiles/TargetProfiles.module.generated.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

#include <string.h>

// 대상 프로파일 시험이다(Docs/Design/AssetSystem 10.3절 1항·10.3.1절 A4f1 — AssetCooker 없이
// TargetProfiles 단독): 셰이더 칸 구조 검증(어휘·중복·상한·형식 목록·키·형식 id 문법·전역
// 중복·셰이더 없는 프로파일), Map Replace(프로젝트가 한 backend만 적으면 형식 Map 전체 교체)와 LOD
// 그룹 키별 병합(필드 플래그), 프로젝트 층 생략, 설정 JSON 층 도우미의 단계, 호스트 프로파일, 엔진
// 파일 셋.
using namespace rocks;
namespace target_profile_tests_file {

using reflection_text::TextDiagnostics;
using rocks::asset_database_test::MemoryFileSystem;

namespace {

constexpr StringView Root = "profiles";
constexpr StringView EngineDirectory = "profiles/Engine";
constexpr StringView ProjectDirectory = "profiles/Project";

Span<const uint8> Text(const char* text) noexcept {
    return Span<const uint8>(reinterpret_cast<const uint8*>(text), strlen(text));
}

struct ProfileScene {
    ProfileScene() noexcept
        : types(allocator), registration(types, allocator),
          objects(allocator, types, ContextId{0x5452474550ull, 1}), files(allocator, Root) {
        registered =
            static_cast<bool>(generated::TargetProfiles::RegisterModuleTypes(registration));
    }

    ~ProfileScene() noexcept {
        CHECK(registration.Unregister());
    }

    bool Ready() const noexcept {
        return registered && files.Ready();
    }

    TargetProfileContext Context(bool project = true) noexcept {
        TargetProfileContext context;
        context.allocator = &allocator;
        context.files = &files;
        context.registry = &objects;
        context.access = objects.OwnerAccess();
        context.types = &types;
        context.engineConfigDirectory = EngineDirectory;
        context.projectConfigDirectory = project ? ProjectDirectory : StringView{};
        return context;
    }

    bool Write(StringView relative, const char* text) noexcept {
        files.Remove(relative);
        return files.AddFile(relative, Text(text));
    }

    Result<void> Read(StringView id, TargetProfile& out, TargetProfileFailure& failure,
                      bool project = true) noexcept {
        TextDiagnostics diagnostics(allocator);
        return ReadTargetProfile(Context(project), id, out, diagnostics, &failure);
    }

    SystemAllocator allocator;
    TypeRegistry types;
    ModuleRegistration registration;
    ObjectRegistry objects;
    MemoryFileSystem files;
    bool registered = false;
};

// 이 소스 파일 경로에서 저장소의 Engine/Config 경로를 만든다.
String EngineConfigFromSource(Allocator& allocator) noexcept {
    StringView file = __FILE__;
    String path(allocator);
    // .../Engine/Source/Developer/TargetProfiles/Tests/TargetProfileTests.cpp → .../Engine
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

// Replace: 프로젝트가 형식 Map을 적으면 Map 전체를 바꾼다(한 backend만 적고 backend 목록은 그대로면
// 빠진 backend의 형식 목록이 없어 거부 — 반쪽만 덮이지 않는다). LOD 그룹 표는 키별 병합이다.
// 프로젝트 층이 없는 문맥(빈 디렉터리)은 엔진 파일만 읽는다.
TEST(TargetProfile_ShaderFieldsReplaceAndLodMerge) {
    ProfileScene scene;
    REQUIRE(scene.Ready());
    REQUIRE(scene.Write("Engine/TargetProfiles/Windows.json",
                        "{\"graphicsBackends\": [\"D3D12\", \"Vulkan\"], \"shaderFormats\": "
                        "{\"D3D12\": {\"formats\": [\"D3D12_SM6\"]}, "
                        "\"Vulkan\": {\"formats\": [\"VULKAN_SM5\", \"VULKAN_SM6\"]}}, "
                        "\"textureLodGroups\": {\"World\": {\"maxLodSize\": 16384}, "
                        "\"UI\": {\"mipGenSettings\": \"NoMipmaps\"}, "
                        "\"Character\": {\"mipGenSettings\": \"Sharpen5\"}, "
                        "\"Effects\": {\"mipGenSettings\": \"Blur3\"}, "
                        "\"Skybox\": {\"mipGenSettings\": \"Angular\"}}}"));
    TargetProfile profile(scene.allocator);
    TargetProfileFailure failure;
    // 그룹의 Sharpen·Blur·Angular는 받는다(12.18 TP1).
    REQUIRE(scene.Read("Windows", profile, failure));
    CHECK(profile.textureLodGroups.Find(TextureGroup::Character)->mipGenSettings ==
          TextureMipGenSettings::Sharpen5);
    CHECK(profile.graphicsBackends.Size() == 2 && profile.shaderFormats.Count() == 2);
    const ShaderFormatList* vulkan = profile.shaderFormats.Find(StringView("Vulkan"));
    CHECK(vulkan != nullptr && vulkan->formats.Size() == 2);

    // 프로젝트가 형식 Map을 한 backend만 적는다 → Map 통째 교체 → Vulkan 형식 목록 없음.
    REQUIRE(scene.Write("Project/TargetProfiles/Windows.json",
                        "{\"shaderFormats\": {\"D3D12\": {\"formats\": [\"D3D12_SM5\"]}}}"));
    CHECK(!scene.Read("Windows", profile, failure) &&
          failure.field == TargetProfileField::ShaderFormats && failure.index == 1 &&
          failure.code == AssetDiagnosticCode::InvalidSetting);

    // backend 목록과 형식 Map을 함께 덮으면 받는다. LOD 그룹은 키별 병합(엔진 그룹 유지).
    REQUIRE(scene.Write("Project/TargetProfiles/Windows.json",
                        "{\"graphicsBackends\": [\"D3D12\"], \"shaderFormats\": "
                        "{\"D3D12\": {\"formats\": [\"D3D12_SM5\"]}}, "
                        "\"textureLodGroups\": {\"World\": {\"lodBias\": 1}}}"));
    REQUIRE(scene.Read("Windows", profile, failure));
    CHECK(profile.graphicsBackends.Size() == 1 && profile.shaderFormats.Count() == 1 &&
          profile.shaderFormats.Find(StringView("D3D12"))->formats[0].View() == "D3D12_SM5");
    const TextureLodGroup* world = profile.textureLodGroups.Find(TextureGroup::World);
    REQUIRE(world != nullptr);
    CHECK(world->maxLodSize == 16384 && world->lodBias == 1 &&
          profile.textureLodGroups.Find(TextureGroup::UI) != nullptr);

    // 프로젝트 층 없음(빈 디렉터리)은 엔진 값 그대로다.
    REQUIRE(scene.Read("Windows", profile, failure, false));
    CHECK(profile.graphicsBackends.Size() == 2);
}

// 셰이더 칸 구조 검증(위반마다 칸·번호)과 받는 경계(셰이더 없는 프로파일·어휘 다섯).
TEST(TargetProfile_ShaderChecks) {
    ProfileScene scene;
    REQUIRE(scene.Ready());
    struct Case {
        const char* text;
        TargetProfileField field;
        uint32 index;
    };
    const Case rejects[] = {
        {"{\"graphicsBackends\": [\"DX12\"], \"shaderFormats\": {}}",
         TargetProfileField::GraphicsBackends, 0},
        {"{\"graphicsBackends\": [\"D3D12\", \"D3D12\"], \"shaderFormats\": "
         "{\"D3D12\": {\"formats\": [\"A\"]}}}",
         TargetProfileField::GraphicsBackends, 1},
        {"{\"graphicsBackends\": [\"D3D12\", \"D3D12\", \"D3D12\", \"D3D12\", \"D3D12\", "
         "\"D3D12\", \"D3D12\", \"D3D12\", \"D3D12\"]}",
         TargetProfileField::GraphicsBackends, 8},
        {"{\"graphicsBackends\": [\"D3D12\"]}", TargetProfileField::ShaderFormats, 0},
        {"{\"graphicsBackends\": [\"D3D12\"], \"shaderFormats\": {\"D3D12\": {\"formats\": []}}}",
         TargetProfileField::ShaderFormats, 0},
        {"{\"graphicsBackends\": [\"D3D12\"], \"shaderFormats\": "
         "{\"D3D12\": {\"formats\": [\"A\"]}, \"Vulkan\": {\"formats\": [\"B\"]}}}",
         TargetProfileField::ShaderFormats, 1},
        {"{\"graphicsBackends\": [\"D3D12\"], \"shaderFormats\": "
         "{\"D3D12\": {\"formats\": [\"d3d12_sm6\"]}}}",
         TargetProfileField::ShaderFormats, 0},
        {"{\"graphicsBackends\": [\"D3D12\", \"D3D11\"], \"shaderFormats\": "
         "{\"D3D12\": {\"formats\": [\"SAME\"]}, \"D3D11\": {\"formats\": [\"SAME\"]}}}",
         TargetProfileField::ShaderFormats, 1},
        {"{\"graphicsBackends\": [\"D3D12\"], \"shaderFormats\": "
         "{\"D3D12\": {\"formats\": [\"X\", \"X\"]}}}",
         TargetProfileField::ShaderFormats, 0},
        {"{\"graphicsBackends\": [\"D3D12\"], \"shaderFormats\": {\"D3D12\": {\"formats\": "
         "[\"F0\", \"F1\", \"F2\", \"F3\", \"F4\", \"F5\", \"F6\", \"F7\", \"F8\", \"F9\", "
         "\"F10\", \"F11\", \"F12\", \"F13\", \"F14\", \"F15\", \"F16\"]}}}",
         TargetProfileField::ShaderFormats, 0},
        {"{\"graphicsBackends\": [], \"shaderFormats\": {\"D3D12\": {\"formats\": [\"A\"]}}}",
         TargetProfileField::ShaderFormats, 0},
    };
    for (const Case& item : rejects) {
        REQUIRE(scene.Write("Engine/TargetProfiles/Windows.json", item.text));
        TargetProfile profile(scene.allocator);
        TargetProfileFailure failure;
        auto read = scene.Read("Windows", profile, failure);
        CHECK(!read && read.Error() == ErrorCode::InvalidArgument &&
              failure.code == AssetDiagnosticCode::InvalidSetting && failure.field == item.field &&
              failure.index == item.index && failure.file == TargetProfileFile::None);
    }

    const char* accepted[] = {
        "{}",
        "{\"graphicsBackends\": [], \"shaderFormats\": {}}",
        "{\"graphicsBackends\": [\"D3D12\", \"D3D11\", \"Vulkan\", \"Metal\", \"OpenGL\"], "
        "\"shaderFormats\": {\"D3D12\": {\"formats\": [\"A\"]}, \"D3D11\": {\"formats\": [\"B\"]}, "
        "\"Vulkan\": {\"formats\": [\"C\"]}, \"Metal\": {\"formats\": [\"D\"]}, "
        "\"OpenGL\": {\"formats\": [\"E_0\"]}}}",
    };
    for (const char* text : accepted) {
        REQUIRE(scene.Write("Engine/TargetProfiles/Windows.json", text));
        TargetProfile profile(scene.allocator);
        TargetProfileFailure failure;
        CHECK(scene.Read("Windows", profile, failure));
    }

    CHECK(IsKnownGraphicsBackend("Metal") && !IsKnownGraphicsBackend("metal") &&
          !IsKnownGraphicsBackend(""));
    char longId[ShaderFormatIdMaxBytes + 2] = {};
    memset(longId, 'A', ShaderFormatIdMaxBytes + 1);
    CHECK(IsValidShaderFormatId(StringView(longId, ShaderFormatIdMaxBytes)) &&
          !IsValidShaderFormatId(longId) && !IsValidShaderFormatId("A-B") &&
          !IsValidShaderFormatId(""));
}

// 오디오 쿠킹 칸(Docs/Design/AudioSystem 3.39.1절): 기본값은 UE 기본(끔·48/32/24/12/8 kHz·배수 1),
// 프로젝트가 칸 하나만 덮으면 나머지는 엔진 값이다(struct 병합). rate는 0 또는 [8000, 192000],
// 배수는 0 이상이어야 한다(index = 칸 번호).
TEST(TargetProfile_AudioCookFields) {
    ProfileScene scene;
    REQUIRE(scene.Ready());
    REQUIRE(scene.Write("Engine/TargetProfiles/Windows.json", "{}"));
    {
        TargetProfile profile(scene.allocator);
        TargetProfileFailure failure;
        REQUIRE(scene.Read("Windows", profile, failure, false));
        const AudioCookOverrides& audio = profile.audioCook;
        CHECK(!audio.resampleForDevice && audio.maxSampleRate == 48000 &&
              audio.highSampleRate == 32000 && audio.mediumSampleRate == 24000 &&
              audio.lowSampleRate == 12000 && audio.minSampleRate == 8000 &&
              audio.compressionQualityModifier == 1.0f);
    }

    REQUIRE(
        scene.Write("Engine/TargetProfiles/Windows.json",
                    "{\"audioCook\": {\"resampleForDevice\": true, \"highSampleRate\": 22050}}"));
    REQUIRE(scene.Write(
        "Project/TargetProfiles/Windows.json",
        "{\"audioCook\": {\"lowSampleRate\": 0, \"compressionQualityModifier\": 0.5}}"));
    {
        TargetProfile profile(scene.allocator);
        TargetProfileFailure failure;
        REQUIRE(scene.Read("Windows", profile, failure));
        const AudioCookOverrides& audio = profile.audioCook;
        CHECK(audio.resampleForDevice && audio.highSampleRate == 22050 &&
              audio.lowSampleRate == 0 && audio.maxSampleRate == 48000 &&
              audio.compressionQualityModifier == 0.5f);
    }

    scene.files.Remove("Project/TargetProfiles/Windows.json");
    struct Case {
        const char* text;
        uint32 index;
    };
    const Case rejects[] = {
        {"{\"audioCook\": {\"maxSampleRate\": 7999}}", 1},
        {"{\"audioCook\": {\"highSampleRate\": 192001}}", 2},
        {"{\"audioCook\": {\"mediumSampleRate\": 1}}", 3},
        {"{\"audioCook\": {\"lowSampleRate\": 400000}}", 4},
        {"{\"audioCook\": {\"minSampleRate\": 4000}}", 5},
        {"{\"audioCook\": {\"compressionQualityModifier\": -0.25}}", 6},
    };
    for (const Case& item : rejects) {
        REQUIRE(scene.Write("Engine/TargetProfiles/Windows.json", item.text));
        TargetProfile profile(scene.allocator);
        TargetProfileFailure failure;
        auto read = scene.Read("Windows", profile, failure);
        CHECK(!read && read.Error() == ErrorCode::InvalidArgument &&
              failure.code == AssetDiagnosticCode::InvalidSetting &&
              failure.field == TargetProfileField::AudioCook && failure.index == item.index);
    }

    // NaN·Infinity는 프로파일 JSON(Strict — RFC 8259)의 문법이 막는다(Json 단계). 검증의 비유한
    // 판정은 다른 출처(코드로 채운 프로파일)를 위한 것이다.
    for (const char* text : {"{\"audioCook\": {\"compressionQualityModifier\": NaN}}",
                             "{\"audioCook\": {\"compressionQualityModifier\": Infinity}}"}) {
        REQUIRE(scene.Write("Engine/TargetProfiles/Windows.json", text));
        TargetProfile profile(scene.allocator);
        TargetProfileFailure failure;
        auto read = scene.Read("Windows", profile, failure);
        CHECK(!read && read.Error() == ErrorCode::InvalidArgument &&
              failure.code == AssetDiagnosticCode::InvalidSetting &&
              failure.field == TargetProfileField::Json);
    }

    // 경계는 받는다: rate 0·8000·192000, 배수 0.
    REQUIRE(scene.Write("Engine/TargetProfiles/Windows.json",
                        "{\"audioCook\": {\"maxSampleRate\": 192000, \"minSampleRate\": 8000, "
                        "\"highSampleRate\": 0, \"compressionQualityModifier\": 0}}"));
    TargetProfile profile(scene.allocator);
    TargetProfileFailure failure;
    CHECK(scene.Read("Windows", profile, failure));
}

// 설정 JSON 층 도우미: 없는 파일(existed false·성공), 크기 상한(File·CapacityExceeded), JSON
// 문법(Json·위치), 대입 진단(Text), 성공(existed true).
TEST(TargetProfile_ConfigJsonLayer) {
    ProfileScene scene;
    REQUIRE(scene.Ready());
    auto lease = scene.types.AcquireValueType(GeneratedType<TargetProfile>::Id(), TypeKind::Struct);
    REQUIRE(lease);
    TargetProfile profile(scene.allocator);
    const auto target = reflection_text::AssignmentTarget::Value(
        scene.objects, scene.objects.OwnerAccess(), lease.Value(), &profile, scene.allocator);
    TextDiagnostics diagnostics(scene.allocator);
    ConfigJsonOutcome outcome;
    REQUIRE(ApplyConfigJsonFile(scene.allocator, scene.files, "profiles/none.json", target,
                                diagnostics, outcome));
    CHECK(!outcome.existed && outcome.stage == ConfigJsonStage::None);

    Array<uint8> large(scene.allocator);
    REQUIRE(large.TryResize(ConfigJsonMaxFileBytes + 1));
    memset(large.Data(), ' ', large.Size());
    large[0] = '{';
    large[large.Size() - 1] = '}';
    REQUIRE(scene.files.AddFile("large.json", large.View()));
    auto tooLarge = ApplyConfigJsonFile(scene.allocator, scene.files, "profiles/large.json", target,
                                        diagnostics, outcome);
    CHECK(!tooLarge && tooLarge.Error() == ErrorCode::CapacityExceeded &&
          outcome.stage == ConfigJsonStage::File);

    REQUIRE(scene.Write("broken.json", "{\n  \"bulkAlignment\": ]\n}"));
    auto broken = ApplyConfigJsonFile(scene.allocator, scene.files, "profiles/broken.json", target,
                                      diagnostics, outcome);
    CHECK(!broken && broken.Error() == ErrorCode::InvalidArgument &&
          outcome.stage == ConfigJsonStage::Json && outcome.line == 2 && outcome.column > 0);

    REQUIRE(scene.Write("unknown.json", "{\"bulkAlignmentX\": 1}"));
    auto unknown = ApplyConfigJsonFile(scene.allocator, scene.files, "profiles/unknown.json",
                                       target, diagnostics, outcome);
    CHECK(!unknown && outcome.stage == ConfigJsonStage::Text && diagnostics.HasErrors());

    REQUIRE(scene.Write("good.json", "{\"bulkAlignment\": 8}"));
    REQUIRE(ApplyConfigJsonFile(scene.allocator, scene.files, "profiles/good.json", target,
                                diagnostics, outcome));
    CHECK(outcome.existed && profile.bulkAlignment == 8);
}

// 호스트 프로파일: 재정의 이름을 따르고, 비면 HostPlatformName()이다. 엔진 파일이 없는 플랫폼은
// 엔진 파일 없음(InvalidSetting).
TEST(TargetProfile_HostProfile) {
    ProfileScene scene;
    REQUIRE(scene.Ready());
    const char* names[] = {"Windows", "Mac", "Android"};
    for (const char* name : names) {
        String path(scene.allocator);
        REQUIRE(path.TryAppend("Engine/TargetProfiles/") && path.TryAppend(name) &&
                path.TryAppend(".json"));
        REQUIRE(scene.Write(path.View(), "{}"));
    }

    TextDiagnostics diagnostics(scene.allocator);
    TargetProfile profile(scene.allocator);
    TargetProfileFailure failure;
    REQUIRE(ReadHostTargetProfile(scene.Context(), "", profile, diagnostics, &failure));
    CHECK(profile.id.View() == HostPlatformName());
    REQUIRE(ReadHostTargetProfile(scene.Context(), "Android", profile, diagnostics, &failure));
    CHECK(profile.id.View() == "Android");
    auto missing = ReadHostTargetProfile(scene.Context(), "Linux", profile, diagnostics, &failure);
    CHECK(!missing && failure.code == AssetDiagnosticCode::InvalidSetting &&
          failure.file == TargetProfileFile::EngineProfile);
}

// 엔진 파일 셋(저장소 Engine/Config — 10.3절 1항 값, LOD 그룹 27행, norm16은 Android만 false).
TEST(TargetProfile_EngineFiles) {
    ProfileScene scene;
    REQUIRE(scene.Ready());
    String engineConfig = EngineConfigFromSource(scene.allocator);
    TargetProfileContext real = scene.Context(false);
    real.files = &platform::PhysicalFileSystem();
    real.engineConfigDirectory = engineConfig.View();
    struct Expected {
        const char* id;
        Span<const StringView> backends;
        Span<const StringView> formats;
        bool norm16;
    };
    const StringView windowsBackends[] = {"D3D12", "D3D11", "Vulkan"};
    const StringView windowsFormats[] = {"D3D12_SM6", "D3D11_SM5", "VULKAN_SM5"};
    const StringView androidBackends[] = {"OpenGL"};
    const StringView androidFormats[] = {"GLES31_ANDROID"};
    const StringView macBackends[] = {"Metal"};
    const StringView macFormats[] = {"METAL_SM5"};
    const Expected rows[] = {{"Windows", windowsBackends, windowsFormats, true},
                             {"Android", androidBackends, androidFormats, false},
                             {"Mac", macBackends, macFormats, true}};
    for (const Expected& row : rows) {
        TextDiagnostics diagnostics(scene.allocator);
        TargetProfile profile(scene.allocator);
        TargetProfileFailure failure;
        REQUIRE(ReadTargetProfile(real, row.id, profile, diagnostics, &failure));
        REQUIRE(profile.graphicsBackends.Size() == row.backends.Size());
        for (usize index = 0; index < row.backends.Size(); ++index) {
            CHECK(profile.graphicsBackends[index].View() == row.backends[index]);
            const ShaderFormatList* list = profile.shaderFormats.Find(row.backends[index]);
            REQUIRE(list != nullptr && list->formats.Size() == 1);
            CHECK(list->formats[0].View() == row.formats[index]);
        }

        CHECK(profile.textureLodGroups.Count() == 27 &&
              profile.textureNorm16Supported == row.norm16);

        // 매니페스트 backend 표(10.3.1절 A4f2 8번): 프로필 순서 그대로이고 매니페스트 검증을
        // 통과한다.
        CookedManifest manifest(scene.allocator);
        REQUIRE(manifest.profileId.TryAssign(row.id));
        REQUIRE(MakeCookedManifestBackends(scene.allocator, profile, manifest.backends));
        REQUIRE(manifest.backends.Size() == row.backends.Size());
        for (usize index = 0; index < row.backends.Size(); ++index) {
            CHECK(manifest.backends[index].name.View() == row.backends[index] &&
                  manifest.backends[index].formats.Size() == 1 &&
                  manifest.backends[index].formats[0].View() == row.formats[index]);
        }

        CHECK(ValidateCookedManifest(manifest, scene.allocator));
    }
}

// MakeCookedManifestBackends(10.3.1절 A4f2 8번): 형식 여럿의 순서를 지키고, 셰이더 없는 프로필은 빈
// 표이며, 호출 전 out 내용은 지운다. 검증하지 않은 프로필(형식 목록 없는 backend)은
// InvalidArgument다. 할당 실패(모든 지점)는 그 오류다.
TEST(TargetProfile_ManifestBackends) {
    ProfileScene scene;
    REQUIRE(scene.Ready());
    Allocator& allocator = scene.allocator;
    TargetProfile profile(allocator);
    REQUIRE(profile.graphicsBackends.TryEmplaceBack(allocator));
    REQUIRE(profile.graphicsBackends[0].TryAssign("Vulkan"));
    REQUIRE(profile.graphicsBackends.TryEmplaceBack(allocator));
    REQUIRE(profile.graphicsBackends[1].TryAssign("D3D12"));
    auto addFormats = [&](StringView backend, StringView first, StringView second) {
        String key(allocator);
        ShaderFormatList list(allocator);
        return key.TryAssign(backend).IsOk() && list.formats.TryEmplaceBack(allocator).IsOk() &&
               list.formats[0].TryAssign(first).IsOk() &&
               (second.IsEmpty() || (list.formats.TryEmplaceBack(allocator).IsOk() &&
                                     list.formats[1].TryAssign(second).IsOk())) &&
               profile.shaderFormats.Insert(Move(key), Move(list)).IsOk();
    };
    // Map 순서(D3D12 먼저)와 무관하게 graphicsBackends 순서(Vulkan 먼저)다.
    REQUIRE(addFormats("D3D12", "D3D12_SM6", "D3D12_SM5"));
    REQUIRE(addFormats("Vulkan", "VULKAN_SM5", StringView()));
    Array<CookedManifestBackend> out(allocator);
    REQUIRE(out.TryEmplaceBack(allocator));
    REQUIRE(MakeCookedManifestBackends(allocator, profile, out));
    REQUIRE(out.Size() == 2);
    CHECK(out[0].name.View() == "Vulkan" && out[0].formats.Size() == 1 &&
          out[1].name.View() == "D3D12" && out[1].formats.Size() == 2 &&
          out[1].formats[0].View() == "D3D12_SM6" && out[1].formats[1].View() == "D3D12_SM5");

    test::NFailAllocator failing(allocator);
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        failing.FailOnAttempt(attempt);
        Array<CookedManifestBackend> table(failing);
        auto made = MakeCookedManifestBackends(failing, profile, table);
        if (failing.Attempts() < attempt) {
            REQUIRE(made);
            break;
        }

        CHECK(made.Error() == ErrorCode::OutOfMemory);
        ++failures;
    }

    failing.DisableFailure();
    CHECK(failures > 0);

    TargetProfile shaderless(allocator);
    REQUIRE(MakeCookedManifestBackends(allocator, shaderless, out));
    CHECK(out.IsEmpty());

    REQUIRE(profile.graphicsBackends.TryEmplaceBack(allocator));
    REQUIRE(profile.graphicsBackends[2].TryAssign("Metal"));
    CHECK(MakeCookedManifestBackends(allocator, profile, out).Error() ==
          ErrorCode::InvalidArgument);
}

} // namespace target_profile_tests_file
