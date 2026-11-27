#include "ShaderManagerTestSupport.h"

#include <AssetCooker/CookConfig.h>
#include <AssetNugget/NuggetFileSystemBackend.h>
#include <Base/InitializerList.h>
#include <Core/FileRead.h>
#include <Core/Text/StringBuilder.h>
#include <ShaderCompiler/ShaderLibraryCookStage.h>
#include <ShaderCompiler/ShaderNugget.h>
#include <ShaderCore/ShaderLibrary.h>
#include <TargetProfiles/TargetProfile.h>

// 셰이더 라이브러리 비에셋 산출물 단계 시험이다(Docs/Design/ShaderSystem 16.1절 SSD44–SSD46): 형식
// 목록 순서로 Engine 라이브러리 Produce, 같은 cookKey 재사용(Reuse), cookDevelopmentShaders 토글과
// 형식 정의 변경의 cookKey 변화, verify 대조, 보고서 절, 형식 정의 대조 음성(없는 형식·자리만 둔
// 형식·backend 다름·GLES31_PC — InvalidSetting, 컴파일 없음), 만들 수 없는 형식·job 실패·프로젝트
// 소속 타입·등록부 원천 실패의 진단, 단계 표 등록. 실제 워커와 도구 저장소를 쓴다(없으면 SKIP).
namespace rocks::shader_cook_stage_test {

using namespace rocks::shader_manager_test;

namespace {

ROCKS_SHADER_TYPE_NAME(StagePixelName, "CookStagePixel");
ROCKS_SHADER_TYPE_NAME(StageBrokenName, "CookStageBroken");
ROCKS_SHADER_TYPE_NAME(StageProjectName, "CookStageProject");

constexpr char StageSource[] =
    "float4 PSMain() : SV_Target { return float4(0.25, 0.5, 0.75, 1.0); }\n";
constexpr char StageEntry[] = "/Plugin/CookStage/Stage.rsf";
constexpr char StageRoot[] = "/Plugin/CookStage";
constexpr ShaderTypeDesc StagePixel = MakeShaderTypeDesc(
    StagePixelName, StageEntry, "PSMain", ShaderStagePixel, ShaderTypeOwner::EnginePlugin);
constexpr ShaderTypeDesc StageBroken = MakeShaderTypeDesc(
    StageBrokenName, StageEntry, "NoSuchEntry", ShaderStagePixel, ShaderTypeOwner::EnginePlugin);
constexpr ShaderTypeDesc StageProject = MakeShaderTypeDesc(
    StageProjectName, "/Project/Stage.rsf", "PSMain", ShaderStagePixel, ShaderTypeOwner::Project);
// 시험이 복사하는 형식 정의다(저장소 Engine/Config/ShaderFormats).
constexpr const char* CopiedFormats[] = {"D3D12_SM6", "D3D11_SM5", "VULKAN_SM5",
                                         "METAL_SM5", "GLES31_PC", "VULKAN_ANDROID"};
// 형식 정의 파일 읽기 상한이다.
constexpr uint64 FormatFileMaxBytes = 1u << 20;

// 고정 등록부 원천이다(Acquire·Release 수를 센다, fail이면 Acquire가 실패한다).
class FixedRegistrySource final : public ShaderCookRegistrySource {
public:
    explicit FixedRegistrySource(const ShaderTypeRegistry& registry) noexcept
        : registry_(registry) {
    }

    Result<const ShaderTypeRegistry*> Acquire() noexcept override {
        ++acquired;
        if (fail) {
            return Result<const ShaderTypeRegistry*>::Failure(ErrorCode::DependencyFailed);
        }

        return Result<const ShaderTypeRegistry*>::Success(&registry_);
    }

    void Release() noexcept override {
        ++released;
    }

    StringView Problem() const noexcept override {
        return fail ? StringView("module RenderBase failed at Startup: test") : StringView();
    }

    uint32 acquired = 0;
    uint32 released = 0;
    bool fail = false;

private:
    const ShaderTypeRegistry& registry_;
};

// 단계 출력 하나다.
struct Produced {
    explicit Produced(Allocator& allocator) noexcept
        : name(allocator), formatId(allocator), bytes(allocator) {
    }

    String name;
    String formatId;
    Sha256Digest cookKey{};
    Array<uint8> bytes;
};

// 기록 sink다(쿠커 sink 계약 중 단계가 지켜야 하는 것 — staging 경로·이름·형식 — 을 단언한다).
class RecordingSink final : public CookArtifactSink {
public:
    explicit RecordingSink(const char* staging) noexcept
        : produced(TestAllocator()), reused(TestAllocator()), diagnostics(TestAllocator()),
          statNames(TestAllocator()), statValues(TestAllocator()), staging_(staging) {
    }

    Result<void> NewStagingPath(StringBuilderBase& out) noexcept override {
        return out.TryAppendFormat("{}/artifact-{}.tmp", StringView(staging_), next_++);
    }

    Result<void> Produce(const CookArtifactOutput& output) noexcept override {
        if (!(output.name == CookedArtifactEngineName) ||
            !output.stagingFile.StartsWith(StringView(staging_), StringComparison::CaseSensitive)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        if (!produced.TryEmplaceBack(TestAllocator())) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }

        Produced& line = produced[produced.Size() - 1];
        bool existed = false;
        if (!line.name.TryAssign(output.name) || !line.formatId.TryAssign(output.formatId) ||
            !ReadWholeFile(platform::PhysicalFileSystem(), output.stagingFile, FormatFileMaxBytes,
                           line.bytes, existed) ||
            !existed) {
            return Result<void>::Failure(ErrorCode::IoError);
        }

        line.cookKey = output.cookKey;
        return Result<void>::Success();
    }

    Result<void> Reuse(const CookedManifestArtifact& artifact) noexcept override {
        String formatId(TestAllocator());
        if (!formatId.TryAssign(artifact.formatId.View()) || !reused.TryPushBack(Move(formatId))) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }

        return Result<void>::Success();
    }

    Result<void> Report(const CookDiagnostic& diagnostic) noexcept override {
        return diagnostics.TryPushBack(diagnostic) ? Result<void>::Success()
                                                   : Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    Result<void> ReportStat(StringView name, uint64 value) noexcept override {
        String copy(TestAllocator());
        if (!copy.TryAssign(name) || !statNames.TryPushBack(Move(copy)) ||
            !statValues.TryPushBack(value)) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }

        return Result<void>::Success();
    }

    uint64 Stat(StringView name) const noexcept {
        for (usize index = 0; index < statNames.Size(); ++index) {
            if (statNames[index].View() == name) {
                return statValues[index];
            }
        }

        return NumericLimits<uint64>::Max();
    }

    const Produced* Find(StringView formatId) const noexcept {
        for (const Produced& line : produced) {
            if (line.formatId.View() == formatId) {
                return &line;
            }
        }

        return nullptr;
    }

    bool Reused(StringView formatId) const noexcept {
        for (const String& line : reused) {
            if (line.View() == formatId) {
                return true;
            }
        }

        return false;
    }

    Array<Produced> produced;
    Array<String> reused;
    Array<CookDiagnostic> diagnostics;
    Array<String> statNames;
    Array<uint64> statValues;

private:
    const char* staging_;
    uint32 next_ = 0;
};

// 디렉터리 하나의 자식 이름 목록이다(EnumerateDirectory 방문자 문맥).
struct ChildNames {
    explicit ChildNames(Allocator& allocator) noexcept : files(allocator), directories(allocator) {
    }

    Array<String> files;
    Array<String> directories;
};

bool CollectChild(const platform::DirectoryEntry& entry, void* context) noexcept {
    auto& names = *static_cast<ChildNames*>(context);
    String name(TestAllocator());
    if (!name.TryAssign(StringView(entry.name, entry.nameBytes))) {
        return false;
    }

    Array<String>& target =
        entry.info.type == platform::FileType::Directory ? names.directories : names.files;
    return static_cast<bool>(target.TryPushBack(Move(name)));
}

// source 디렉터리 트리를 fixture의 target(상대)으로 복사한다(시험 엔진 폴더의 Shaders — 전처리가
// /Engine/Public/Platform.rsh를 먼저 넣는다).
bool CopyTree(const Fixture& fixture, const char* source, const char* target) noexcept {
    ChildNames names(TestAllocator());
    if (!platform::PhysicalFileSystem().EnumerateDirectory(source, strlen(source), &CollectChild,
                                                           &names) ||
        !fixture.MakeDirectory(target)) {
        return false;
    }

    for (const String& file : names.files) {
        char from[ManagerPathBytes];
        char to[ManagerPathBytes];
        snprintf(from, sizeof(from), "%s/%s", source, file.CStr());
        snprintf(to, sizeof(to), "%s/%s", target, file.CStr());
        Array<uint8> bytes(TestAllocator());
        bool existed = false;
        if (!ReadWholeFile(platform::PhysicalFileSystem(), StringView(from), FormatFileMaxBytes,
                           bytes, existed) ||
            !existed || !fixture.WriteBytes(to, bytes.Data(), bytes.Size())) {
            return false;
        }
    }

    for (const String& directory : names.directories) {
        char from[ManagerPathBytes];
        char to[ManagerPathBytes];
        snprintf(from, sizeof(from), "%s/%s", source, directory.CStr());
        snprintf(to, sizeof(to), "%s/%s", target, directory.CStr());
        if (!CopyTree(fixture, from, to)) {
            return false;
        }
    }

    return true;
}

// 프로필에 backend 하나와 형식 목록을 더한다.
bool AddBackend(TargetProfile& profile, StringView backend,
                std::initializer_list<StringView> formats) noexcept {
    String name(TestAllocator());
    String key(TestAllocator());
    ShaderFormatList list(TestAllocator());
    if (!name.TryAssign(backend) || !key.TryAssign(backend)) {
        return false;
    }

    for (const StringView format : formats) {
        if (!list.formats.TryEmplaceBack(TestAllocator()) ||
            !list.formats[list.formats.Size() - 1].TryAssign(format)) {
            return false;
        }
    }

    return profile.graphicsBackends.TryPushBack(Move(name)) &&
           static_cast<bool>(profile.shaderFormats.Insert(Move(key), Move(list)));
}

// 워커·도구·가짜 엔진 폴더(형식 정의·엔진 원문 사본)·시험 원문·너겟 캐시가 준비된 한 벌이다.
struct StageCase {
    StageCase() noexcept : cache(TestAllocator(), platform::PhysicalFileSystem(), clock) {
    }

    ~StageCase() noexcept {
        cache.Shutdown(0);
    }

    // false면 SKIP(skip에 이유)이거나 준비 실패(skip null — 실패를 기록했다)다.
    bool Make(const char* tag, const char*& skip) noexcept {
        if (!WorkerExecutable(worker)) {
            skip = "ShaderCompileWorker is not next to the test executable";
            return false;
        }

        SHADER_MANAGER_TEST_REQUIRE(fixture.Make(tag));
        if (!ToolsReady(worker, fixture)) {
            skip = "shader tool store is not bootstrapped (dotnet msbuild Rocks.proj "
                   "/t:ThirdPartyBootstrap)";
            return false;
        }

        for (const char* id : CopiedFormats) {
            SHADER_MANAGER_TEST_REQUIRE(CopyFormat(id));
        }

        char shaders[ManagerPathBytes];
        SHADER_MANAGER_TEST_REQUIRE(RepositoryShaderDirectory(shaders, sizeof(shaders)));
        SHADER_MANAGER_TEST_REQUIRE(CopyTree(fixture, shaders, "Engine/Shaders"));
        SHADER_MANAGER_TEST_REQUIRE(fixture.MakeDirectory("Staging"));
        SHADER_MANAGER_TEST_REQUIRE(fixture.MakeDirectory("Project"));
        SHADER_MANAGER_TEST_REQUIRE(fixture.Write("Plugin/Stage.rsf", StageSource));
        SHADER_MANAGER_TEST_REQUIRE(fixture.Child("Engine", engine, sizeof(engine)));
        SHADER_MANAGER_TEST_REQUIRE(fixture.Child("Plugin", plugin, sizeof(plugin)));
        SHADER_MANAGER_TEST_REQUIRE(fixture.Child("Staging", staging, sizeof(staging)));
        SHADER_MANAGER_TEST_REQUIRE(fixture.Child("Cooked", cooked, sizeof(cooked)));
        char project[ManagerPathBytes];
        SHADER_MANAGER_TEST_REQUIRE(fixture.Child("Project", project, sizeof(project)));
        SHADER_MANAGER_TEST_REQUIRE(OpenShaderNuggetCache(
            cache, TestAllocator(), platform::PhysicalFileSystem(), StringView(project), nullptr));
        mount = ShaderCookSourceDirectory{StringView(StageRoot), StringView(plugin)};
        return true;
    }

    bool CopyFormat(const char* id, StringView suffix = StringView()) noexcept {
        char source[ManagerPathBytes];
        char target[ManagerPathBytes];
        snprintf(source, sizeof(source), "%s/%s.json", ShaderFormatDirectory, id);
        snprintf(target, sizeof(target), "Engine/Config/ShaderFormats/%s.json", id);
        Array<uint8> bytes(TestAllocator());
        bool existed = false;
        if (!ReadWholeFile(platform::PhysicalFileSystem(), StringView(source), FormatFileMaxBytes,
                           bytes, existed) ||
            !existed ||
            !bytes.TryAppend(Span<const uint8>(reinterpret_cast<const uint8*>(suffix.Data()),
                                               suffix.ByteCount()))) {
            return false;
        }

        return fixture.WriteBytes(target, bytes.Data(), bytes.Size());
    }

    ShaderLibraryCookStageConfig Config(ShaderCookRegistrySource& source) noexcept {
        ShaderLibraryCookStageConfig config;
        config.allocator = &TestAllocator();
        config.registry = &source;
        config.engineDirectory = StringView(engine);
        config.workerExecutable = StringView(worker);
        config.sourceDirectories = Span<const ShaderCookSourceDirectory>(&mount, 1);
        return config;
    }

    CookArtifactRequest Request(const TargetProfile& profile, const CookSettings& settings,
                                Span<const CookedManifestArtifact> reusable = {}) noexcept {
        CookArtifactRequest request;
        request.allocator = &TestAllocator();
        request.files = &platform::PhysicalFileSystem();
        request.profile = &profile;
        request.settings = &settings;
        request.cookedRoot = StringView(cooked);
        request.stagingDirectory = StringView(staging);
        request.nuggets = &cache;
        request.reusable = reusable;
        return request;
    }

    Fixture fixture;
    NuggetSystemClock clock;
    NuggetCache cache;
    ShaderCookSourceDirectory mount;
    char worker[ManagerPathBytes] = {};
    char engine[ManagerPathBytes] = {};
    char plugin[ManagerPathBytes] = {};
    char staging[ManagerPathBytes] = {};
    char cooked[ManagerPathBytes] = {};
};

// 만든 출력을 재사용 후보로 바꾼다(쿠커가 게시 파일 대조를 통과한 이전 항목).
bool ToCandidates(const RecordingSink& sink, Array<CookedManifestArtifact>& out) noexcept {
    for (const Produced& line : sink.produced) {
        if (!out.TryEmplaceBack(TestAllocator())) {
            return false;
        }

        CookedManifestArtifact& artifact = out[out.Size() - 1];
        artifact.kind = CookArtifactKind::ShaderLibrary;
        artifact.bytes = line.bytes.Size();
        artifact.sha256 = Sha256::Hash(line.bytes.View());
        artifact.cookKey = line.cookKey;
        if (!artifact.name.TryAssign(line.name.View()) ||
            !artifact.formatId.TryAssign(line.formatId.View())) {
            return false;
        }
    }

    return true;
}

bool HasDiagnostic(const RecordingSink& sink, AssetDiagnosticCode code, ErrorCode error,
                   uint32 setting) noexcept {
    for (const CookDiagnostic& diagnostic : sink.diagnostics) {
        if (diagnostic.code == code && diagnostic.error == error && diagnostic.setting == setting &&
            !diagnostic.warning) {
            return true;
        }
    }

    return false;
}

bool SameBytes(const Array<uint8>& left, const Array<uint8>& right) noexcept {
    return left.Size() == right.Size() && memcmp(left.Data(), right.Data(), left.Size()) == 0;
}

} // namespace

TEST(ShaderLibraryCookStage_ProducesReusesAndRekeys) {
    StageCase test;
    const char* skip = nullptr;
    if (!test.Make("cookstage", skip)) {
        if (skip != nullptr) {
            printf("[SKIP] ShaderLibraryCookStage_ProducesReusesAndRekeys: %s\n", skip);
        }

        return;
    }

    ShaderTypeRegistry registry(TestAllocator());
    const ShaderTypeDesc* const types[] = {&StagePixel};
    REQUIRE(registry.Register(types));
    REQUIRE(registry.Freeze());
    FixedRegistrySource source(registry);
    ShaderLibraryCookStage stage(test.Config(source));
    CHECK(stage.Kind() == CookArtifactKind::ShaderLibrary);
    CHECK(stage.Identity().processor == ShaderLibraryCookerGuid);
    // 단계 표에 명시 등록한다(같은 종류 두 번은 AlreadyExists — 모듈 수명 객체가 Warning으로
    // 넘긴다).
    CookArtifactStageTable table;
    REQUIRE(table.Register(stage));
    ShaderLibraryCookStage second(test.Config(source));
    CHECK(table.Register(second).Error() == ErrorCode::AlreadyExists);
    CHECK(table.Find(CookArtifactKind::ShaderLibrary) == &stage);

    TargetProfile profile(TestAllocator());
    REQUIRE(profile.id.TryAssign("Windows"));
    REQUIRE(AddBackend(profile, "D3D12", {"D3D12_SM6"}));
    REQUIRE(AddBackend(profile, "Vulkan", {"VULKAN_SM5"}));
    CookSettings settings(TestAllocator());

    // 1. 형식 목록 순서로 Engine 라이브러리 둘을 만든다.
    RecordingSink first(test.staging);
    const CookArtifactRequest request = test.Request(profile, settings);
    REQUIRE(stage.Run(request, first));
    CHECK(first.diagnostics.IsEmpty());
    REQUIRE(first.produced.Size() == 2 && first.reused.IsEmpty());
    CHECK(first.produced[0].formatId.View() == "D3D12_SM6" &&
          first.produced[1].formatId.View() == "VULKAN_SM5");
    CHECK(source.acquired == 1 && source.released == 1);
    for (const Produced& line : first.produced) {
        ShaderLibraryView view;
        REQUIRE(view.Open(line.bytes.View()));
        CHECK(view.Kind() == ShaderLibraryKind::Engine &&
              view.Facts().FormatId() == line.formatId.View() &&
              view.TypeSetSha256() == registry.TypeSetDigest() && view.EntryCount() == 1);
    }

    CHECK(!(first.produced[0].cookKey == first.produced[1].cookKey));
    CHECK(first.Stat("formats") == 2 && first.Stat("jobs") == 2 && first.Stat("entries") == 2 &&
          first.Stat("reused") == 0);

    // 2. 같은 입력이면 cookKey가 같아 재사용 후보를 그대로 쓴다(너겟 적중 — 컴파일하지 않는다).
    Array<CookedManifestArtifact> candidates(TestAllocator());
    REQUIRE(ToCandidates(first, candidates));
    RecordingSink again(test.staging);
    REQUIRE(stage.Run(test.Request(profile, settings, candidates.View()), again));
    CHECK(again.diagnostics.IsEmpty() && again.produced.IsEmpty());
    CHECK(again.Reused("D3D12_SM6") && again.Reused("VULKAN_SM5"));
    CHECK(again.Stat("cacheHits") == 2 && again.Stat("reused") == 2);

    // 3. cookDevelopmentShaders는 개발 전용 타입이 없어 항목이 같아도 cookKey를 바꾼다(A4f3 1번 —
    // 재사용 판정은 CookSettings SHA를 보지 않는다). 라이브러리 byte는 같다.
    settings.cookDevelopmentShaders = true;
    RecordingSink development(test.staging);
    REQUIRE(stage.Run(test.Request(profile, settings, candidates.View()), development));
    REQUIRE(development.produced.Size() == 2);
    CHECK(development.reused.IsEmpty());
    for (usize index = 0; index < 2; ++index) {
        CHECK(!(development.produced[index].cookKey == first.produced[index].cookKey));
        CHECK(SameBytes(development.produced[index].bytes, first.produced[index].bytes));
    }

    settings.cookDevelopmentShaders = false;

    // 4. 형식 정의가 바뀌면(파일 SHA-256) 그 형식만 cookKey가 바뀐다.
    REQUIRE(test.CopyFormat("D3D12_SM6", StringView("\n")));
    RecordingSink edited(test.staging);
    REQUIRE(stage.Run(test.Request(profile, settings, candidates.View()), edited));
    CHECK(edited.diagnostics.IsEmpty());
    REQUIRE(edited.produced.Size() == 1);
    CHECK(edited.produced[0].formatId.View() == "D3D12_SM6" &&
          !(edited.produced[0].cookKey == first.produced[0].cookKey));
    CHECK(edited.Reused("VULKAN_SM5") && !edited.Reused("D3D12_SM6"));

    // 5. verify 쿠킹은 캐시 없이 다시 만든 byte와 같아 진단이 없다.
    CookArtifactRequest verify = test.Request(profile, settings);
    verify.verifyNuggets = true;
    RecordingSink verified(test.staging);
    REQUIRE(stage.Run(verify, verified));
    CHECK(verified.diagnostics.IsEmpty() && verified.produced.Size() == 2);
}

TEST(ShaderLibraryCookStage_RejectsFormatsAgainstDefinitions) {
    StageCase test;
    const char* skip = nullptr;
    if (!test.Make("cookstagedefs", skip)) {
        if (skip != nullptr) {
            printf("[SKIP] ShaderLibraryCookStage_RejectsFormatsAgainstDefinitions: %s\n", skip);
        }

        return;
    }

    ShaderTypeRegistry registry(TestAllocator());
    const ShaderTypeDesc* const types[] = {&StagePixel};
    REQUIRE(registry.Register(types));
    REQUIRE(registry.Freeze());
    FixedRegistrySource source(registry);
    ShaderLibraryCookStage stage(test.Config(source));
    // 순번 0 통과, 1 없는 형식, 2 자리만 둔 형식, 3 backend가 키와 다름, 4 GLES31_PC.
    TargetProfile profile(TestAllocator());
    REQUIRE(AddBackend(profile, "D3D12", {"D3D12_SM6", "D3D12_NOPE"}));
    REQUIRE(AddBackend(profile, "Vulkan", {"VULKAN_ANDROID"}));
    REQUIRE(AddBackend(profile, "D3D11", {"VULKAN_SM5"}));
    REQUIRE(AddBackend(profile, "OpenGL", {"GLES31_PC"}));
    CookSettings settings(TestAllocator());
    RecordingSink sink(test.staging);
    REQUIRE(stage.Run(test.Request(profile, settings), sink));
    CHECK(sink.diagnostics.Size() == 4);
    for (uint32 setting = 1; setting <= 4; ++setting) {
        CHECK(HasDiagnostic(sink, AssetDiagnosticCode::InvalidSetting, ErrorCode::None, setting));
    }

    // 위반이 있으면 등록부를 받지 않고 컴파일하지 않는다.
    CHECK(sink.produced.IsEmpty() && source.acquired == 0);
}

TEST(ShaderLibraryCookStage_DiagnosesUnavailableFormatsAndJobs) {
    StageCase test;
    const char* skip = nullptr;
    if (!test.Make("cookstagefail", skip)) {
        if (skip != nullptr) {
            printf("[SKIP] ShaderLibraryCookStage_DiagnosesUnavailableFormatsAndJobs: %s\n", skip);
        }

        return;
    }

    CookSettings settings(TestAllocator());
    // 이 호스트가 도구를 갖지 못한 형식(Windows의 METAL_SM5 — Apple Metal 도구 없음)은 code None +
    // UnsupportedCapability이고 다른 형식은 그대로 만든다.
    {
        ShaderTypeRegistry registry(TestAllocator());
        const ShaderTypeDesc* const types[] = {&StagePixel};
        REQUIRE(registry.Register(types));
        REQUIRE(registry.Freeze());
        FixedRegistrySource source(registry);
        ShaderLibraryCookStage stage(test.Config(source));
        TargetProfile profile(TestAllocator());
        REQUIRE(AddBackend(profile, "D3D12", {"D3D12_SM6"}));
        REQUIRE(AddBackend(profile, "Metal", {"METAL_SM5"}));
        RecordingSink sink(test.staging);
        REQUIRE(stage.Run(test.Request(profile, settings), sink));
        if (sink.produced.Size() == 2) {
            printf("[SKIP] ShaderLibraryCookStage_DiagnosesUnavailableFormatsAndJobs Metal: this "
                   "host can produce METAL_SM5\n");
        } else {
            CHECK(sink.diagnostics.Size() == 1 &&
                  HasDiagnostic(sink, AssetDiagnosticCode::None, ErrorCode::UnsupportedCapability,
                                1));
            CHECK(sink.produced.Size() == 1 && sink.Find("D3D12_SM6") != nullptr);
        }
    }

    // job 실패(없는 엔트리)는 그 형식의 DependencyFailed 하나이고 그 형식은 만들지 않는다.
    {
        ShaderTypeRegistry registry(TestAllocator());
        const ShaderTypeDesc* const types[] = {&StagePixel, &StageBroken};
        REQUIRE(registry.Register(types));
        REQUIRE(registry.Freeze());
        FixedRegistrySource source(registry);
        ShaderLibraryCookStage stage(test.Config(source));
        TargetProfile profile(TestAllocator());
        REQUIRE(AddBackend(profile, "D3D12", {"D3D12_SM6"}));
        RecordingSink sink(test.staging);
        REQUIRE(stage.Run(test.Request(profile, settings), sink));
        CHECK(sink.diagnostics.Size() == 1 &&
              HasDiagnostic(sink, AssetDiagnosticCode::None, ErrorCode::DependencyFailed, 0));
        CHECK(sink.produced.IsEmpty() && source.released == 1);
    }

    // 프로젝트 소속 타입은 ModuleLoader ML5b 전 쿠킹 오류다(조용히 빼지 않는다).
    {
        ShaderTypeRegistry registry(TestAllocator());
        const ShaderTypeDesc* const types[] = {&StagePixel, &StageProject};
        REQUIRE(registry.Register(types));
        REQUIRE(registry.Freeze());
        FixedRegistrySource source(registry);
        ShaderLibraryCookStage stage(test.Config(source));
        TargetProfile profile(TestAllocator());
        REQUIRE(AddBackend(profile, "D3D12", {"D3D12_SM6"}));
        RecordingSink sink(test.staging);
        REQUIRE(stage.Run(test.Request(profile, settings), sink));
        CHECK(sink.diagnostics.Size() == 1 &&
              HasDiagnostic(sink, AssetDiagnosticCode::None, ErrorCode::Unsupported, 0));
        CHECK(sink.produced.IsEmpty() && source.acquired == 1 && source.released == 1);
    }

    // 등록부 원천 실패(모듈 시작 실패 등)는 DependencyFailed이고 Release를 부르지 않는다.
    {
        ShaderTypeRegistry registry(TestAllocator());
        REQUIRE(registry.Freeze());
        FixedRegistrySource source(registry);
        source.fail = true;
        ShaderLibraryCookStage stage(test.Config(source));
        TargetProfile profile(TestAllocator());
        REQUIRE(AddBackend(profile, "D3D12", {"D3D12_SM6"}));
        RecordingSink sink(test.staging);
        REQUIRE(stage.Run(test.Request(profile, settings), sink));
        CHECK(sink.diagnostics.Size() == 1 &&
              HasDiagnostic(sink, AssetDiagnosticCode::None, ErrorCode::DependencyFailed, 0));
        CHECK(source.acquired == 1 && source.released == 0);
    }

    // 요청·설정이 비면 InvalidArgument다(단계 코드 결함).
    {
        ShaderTypeRegistry registry(TestAllocator());
        FixedRegistrySource source(registry);
        ShaderLibraryCookStage stage(test.Config(source));
        TargetProfile profile(TestAllocator());
        RecordingSink sink(test.staging);
        CookArtifactRequest request = test.Request(profile, settings);
        request.profile = nullptr;
        CHECK(stage.Run(request, sink).Error() == ErrorCode::InvalidArgument);
    }
}

} // namespace rocks::shader_cook_stage_test
