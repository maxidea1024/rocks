#include "CookProjectTestSupport.h"

#include <Platform/File.h>
#include <TestSupport/NFailAllocator.h>

// 비에셋 산출물 단계 시험이다(Docs/Design/AssetSystem 10.3.1절 A4f3): 단계 표(등록·중복·모르는
// 종류·해제·사용 중 Busy·프로세스 표), 가짜 셰이더 라이브러리 단계로 쿠킹 흐름(게시·크기·SHA를
// 쿠커가 직접 계산·FindArtifact·CheckCookedArtifactFile, 같은 내용 다시 쿠킹, 게시 파일 손상
// CorruptEntry, 전체 쿠킹 정리·부분 쿠킹 유지, 재사용 후보와 cookKey, verify는 후보 없음), job
// 오류· 미등록 UnsupportedCapability·셰이더 없는 프로필·에셋 오류가 있어도 단계가 돎, 계약 위반과
// stageFailure, cookDevelopmentShaders 전달, 보고서 두 칸, 할당 실패. 엔진 전용 쿠킹
// CookEngine(10.3.1절 A4f4 3번 — Engine만·Project는 계약 위반·entryCount 0·재사용·셰이더 없는
// 프로필·미등록·verify·깨진 이전 매니페스트·잠금·인자·할당 실패).
using namespace rocks;

namespace cook_artifact_tests_file {

using namespace rocks::cook_project_test;

namespace {

constexpr Guid FakeStageGuid{0xA4F3000000000001ull, 0x5354414745000001ull};
constexpr StringView ShadersRoot = "Saved/Cooked/Windows/Shaders";
// staging 경로 조립 버퍼의 인라인 크기다.
constexpr usize StagingInlineBytes = 64;

// 계약 위반 종류다(Run 첫머리에서 한 번 시도하고 그 결과를 돌려준다).
enum class Violation : uint8 {
    None,
    BadName,
    BadFormat,
    NotInTable,
    ForeignPath,
    PathTwice,
    Duplicate,
    EmptyFile,
    ReuseForeign
};

Result<void> WriteStaged(platform::FileSystem& files, StringView path, StringView text) noexcept {
    platform::File file;
    if (!file.Open(files, path.Data(), path.ByteCount(), platform::FileOpenMode::CreateAlways)) {
        return Result<void>::Failure(ErrorCode::IoError);
    }

    const platform::FileTransfer written =
        file.WriteAll(reinterpret_cast<const uint8*>(text.Data()), text.ByteCount());
    const bool closed = static_cast<bool>(file.Close());
    return written.status && closed ? Result<void>::Success()
                                    : Result<void>::Failure(ErrorCode::IoError);
}

// 가짜 셰이더 라이브러리 단계다: 프로필의 형식마다 Engine·Project 파일 하나("library <이름> <형식>
// <seed>"), cookKey = SHA-256(이름·형식·seed·cookDevelopmentShaders). 같은 키의 후보는 Reuse한다.
class FakeStage final : public CookArtifactStage {
public:
    CookArtifactKind Kind() const noexcept override {
        return CookArtifactKind::ShaderLibrary;
    }

    NuggetProcessorId Identity() const noexcept override {
        return NuggetProcessorId{FakeStageGuid, Guid{1, 1}};
    }

    Result<void> Run(const CookArtifactRequest& request, CookArtifactSink& sink) noexcept override {
        ++runs;
        offered = request.reusable.Size();
        packages = request.packages.Size();
        packagesOrdered = true;
        for (usize index = 1; index < request.packages.Size(); ++index) {
            packagesOrdered = packagesOrdered && GuidLess(request.packages[index - 1]->asset.value,
                                                          request.packages[index]->asset.value);
        }

        sawEngineOnly = request.engineOnly;
        sawVerify = request.verifyNuggets;
        sawDevelopment = request.settings->cookDevelopmentShaders;
        if (table != nullptr) {
            auto unregistered = table->Unregister(*this);
            unregisterDuring = unregistered ? ErrorCode::None : unregistered.Error();
            auto registered = table->Register(*this);
            registerDuring = registered ? ErrorCode::None : registered.Error();
        }

        if (violation != Violation::None) {
            return Violate(request, sink);
        }

        const StringView names[] = {CookedArtifactEngineName, CookedArtifactProjectName};
        const TargetProfile& profile = *request.profile;
        for (const String& backend : profile.graphicsBackends) {
            const ShaderFormatList* list = profile.shaderFormats.Find(backend);
            if (list == nullptr) {
                continue;
            }

            for (const String& format : list->formats) {
                for (StringView name : names) {
                    // 엔진 전용 요청은 Engine만 만든다(respectEngineOnly 거짓이면 계약 위반 시험).
                    if (request.engineOnly && respectEngineOnly &&
                        name == CookedArtifactProjectName) {
                        continue;
                    }

                    if (auto made = Make(request, sink, name, format.View()); !made) {
                        return made;
                    }
                }
            }
        }

        if (failJob) {
            if (auto reported = sink.Report(CookDiagnostic{
                    AssetDiagnosticCode::None, false, {}, {}, 0, ErrorCode::InvalidArgument});
                !reported) {
                return reported;
            }
        }

        return sink.ReportStat("libraries", produced + reused);
    }

    uint32 seed = 1;
    bool failJob = false;
    bool respectEngineOnly = true;
    // 관찰값: 마지막 Run의 engineOnly·verifyNuggets다.
    bool sawEngineOnly = false;
    bool sawVerify = false;
    Violation violation = Violation::None;
    // 있으면 Run 중에 이 표의 해제·등록을 시도한다(사용 중 Busy).
    CookArtifactStageTable* table = nullptr;
    // 관찰값이다.
    uint32 runs = 0;
    uint32 produced = 0;
    uint32 reused = 0;
    usize offered = 0;
    // 마지막 Run이 받은 쿠킹 패키지 수와 AssetId 엄격 오름차순 여부다(ShaderSystem 16.5절 SSD87).
    usize packages = 0;
    bool packagesOrdered = false;
    bool sawDevelopment = false;
    ErrorCode unregisterDuring = ErrorCode::None;
    ErrorCode registerDuring = ErrorCode::None;

private:
    Sha256Digest Key(Allocator& allocator, StringView name, StringView format,
                     bool development) const noexcept {
        StringBuilder<StagingInlineBytes> text(allocator);
        (void)text.TryAppendFormat("{}|{}|{}|{}", name, format, seed, development ? 1 : 0);
        return Sha256::Hash(Span<const uint8>(reinterpret_cast<const uint8*>(text.View().Data()),
                                              text.View().ByteCount()));
    }

    Result<void> Write(const CookArtifactRequest& request, CookArtifactSink& sink, StringView name,
                       StringView format, StringBuilderBase& path, bool empty = false) noexcept {
        if (auto issued = sink.NewStagingPath(path); !issued) {
            return issued;
        }

        StringBuilder<StagingInlineBytes> text(*request.allocator);
        if (!empty) {
            if (auto made = text.TryAppendFormat("library {} {} {}", name, format, seed); !made) {
                return made;
            }
        }

        return WriteStaged(*request.files, path.View(), text.View());
    }

    Result<void> Make(const CookArtifactRequest& request, CookArtifactSink& sink, StringView name,
                      StringView format) noexcept {
        const Sha256Digest key =
            Key(*request.allocator, name, format, request.settings->cookDevelopmentShaders);
        for (const CookedManifestArtifact& candidate : request.reusable) {
            if (candidate.name.View() == name && candidate.formatId.View() == format &&
                candidate.cookKey == key) {
                ++reused;
                return sink.Reuse(candidate);
            }
        }

        StringBuilder<StagingInlineBytes> path(*request.allocator);
        if (auto written = Write(request, sink, name, format, path); !written) {
            return written;
        }

        ++produced;
        return sink.Produce(CookArtifactOutput{name, format, key, path.View()});
    }

    // 위반 하나를 시도한다(준비 단계의 쓰기 실패는 그 오류다 — 위반이 아니다).
    Result<void> Violate(const CookArtifactRequest& request, CookArtifactSink& sink) noexcept {
        Allocator& allocator = *request.allocator;
        const StringView engine = CookedArtifactEngineName;
        const StringView project = CookedArtifactProjectName;
        const Sha256Digest key = Key(allocator, engine, "D3D12_SM6", false);
        StringBuilder<StagingInlineBytes> path(allocator);
        auto produce = [&](StringView name, StringView format) noexcept {
            return sink.Produce(CookArtifactOutput{name, format, key, path.View()});
        };
        auto writeThenProduce = [&](StringView name, StringView format, bool empty) noexcept {
            if (auto written = Write(request, sink, name, format, path, empty); !written) {
                return written;
            }

            return produce(name, format);
        };
        switch (violation) {
        case Violation::BadName:
            return writeThenProduce("engine", "D3D12_SM6", false);
        case Violation::BadFormat:
            return writeThenProduce(engine, "d3d12_sm6", false);
        case Violation::NotInTable:
            return writeThenProduce(engine, "METAL_SM5", false);
        case Violation::EmptyFile:
            return writeThenProduce(engine, "D3D12_SM6", true);
        case Violation::ForeignPath: {
            // sink가 주지 않은 staging 바로 아래 경로다.
            if (auto made = path.TryAppendFormat("{}/foreign.tmp", request.stagingDirectory);
                !made) {
                return made;
            }

            if (auto written = WriteStaged(*request.files, path.View(), "library"); !written) {
                return written;
            }

            return produce(engine, "D3D12_SM6");
        }
        case Violation::PathTwice:
            if (auto first = writeThenProduce(engine, "D3D12_SM6", false); !first) {
                return first;
            }

            // 이미 쓴 경로를 다른 이름으로 다시 넘긴다.
            return produce(project, "D3D12_SM6");
        case Violation::Duplicate: {
            if (auto first = writeThenProduce(engine, "VULKAN_SM5", false); !first) {
                return first;
            }

            StringBuilder<StagingInlineBytes> again(allocator);
            if (auto written = Write(request, sink, engine, "VULKAN_SM5", again); !written) {
                return written;
            }

            return sink.Produce(CookArtifactOutput{engine, "VULKAN_SM5", key, again.View()});
        }
        case Violation::ReuseForeign: {
            CookedManifestArtifact foreign(allocator);
            foreign.bytes = 1;
            if (auto assigned = foreign.name.TryAssign(engine); !assigned) {
                return assigned;
            }

            if (auto assigned = foreign.formatId.TryAssign("D3D12_SM6"); !assigned) {
                return assigned;
            }

            return sink.Reuse(foreign);
        }
        case Violation::None:
            break;
        }

        return Result<void>::Success();
    }
};

// 장면 프로필에 셰이더 칸(D3D12 [D3D12_SM6]·Vulkan [VULKAN_SM5])을 더한다.
bool AddShaders(AssetEnv& env, TargetProfile& profile) noexcept {
    const StringView backends[] = {"D3D12", "Vulkan"};
    const StringView formats[] = {"D3D12_SM6", "VULKAN_SM5"};
    for (usize index = 0; index < 2; ++index) {
        String name(env.allocator);
        String key(env.allocator);
        ShaderFormatList list(env.allocator);
        if (!name.TryAssign(backends[index]) || !key.TryAssign(backends[index]) ||
            !list.formats.TryEmplaceBack(env.allocator) ||
            !list.formats[0].TryAssign(formats[index]) ||
            !profile.graphicsBackends.TryPushBack(Move(name)) ||
            !profile.shaderFormats.Insert(Move(key), Move(list))) {
            return false;
        }
    }

    return true;
}

// 장면과 단계 표·가짜 단계다.
struct StageScene {
    StageScene(AssetEnv& env, const char* tag) noexcept : project(env, tag) {
        ready = project.ready && AddShaders(env, project.profile) &&
                static_cast<bool>(table.Register(stage));
    }

    ~StageScene() {
        (void)table.Unregister(stage);
    }

    CookProjectRequest Request(Span<const StringView> paths = {}) noexcept {
        CookProjectRequest request = project.Request(paths);
        request.stages = &table;
        return request;
    }

    Result<CookedManifest> Manifest() noexcept {
        return ReadCookedManifest(project.env.allocator,
                                  project.files.Bytes(cook_project_test::Manifest));
    }

    // 쿠킹 루트 기준 artifact 파일의 메모리 파일 시스템 이름이다.
    String Published(const CookedManifestArtifact& artifact) noexcept {
        StringBuilder<StagingInlineBytes> path(project.env.allocator);
        CHECK(path.TryAppend("Saved/Cooked/Windows/") && AppendCookedArtifactPath(path, artifact));
        String name(project.env.allocator);
        CHECK(name.TryAssign(path.View()));
        return name;
    }

    // Shaders/ 안 파일 수다.
    usize ShaderFiles() noexcept {
        usize count = 0;
        for (usize index = 0; index < project.files.NodeCount(); ++index) {
            const StringView name = project.files.NodeName(index);
            if (name.StartsWith(ShadersRoot, StringComparison::CaseSensitive) &&
                name.ByteCount() > ShadersRoot.ByteCount() && project.files.Has(name) &&
                name.EndsWith(".rshl", StringComparison::CaseSensitive)) {
                ++count;
            }
        }

        return count;
    }

    ProjectScene project;
    CookArtifactStageTable table;
    FakeStage stage;
    bool ready = false;
};

bool HasDiagnostic(const CookProjectResult& result, AssetDiagnosticCode code, ErrorCode error,
                   bool warning) noexcept {
    for (const CookDiagnostic& diagnostic : result.diagnostics) {
        if (diagnostic.code == code && diagnostic.error == error && diagnostic.warning == warning &&
            diagnostic.asset.IsNull()) {
            return true;
        }
    }

    return false;
}

// 공백을 뺀 보고서에 part가 있는지 본다.
bool ReportHas(ProjectScene& scene, StringView part) noexcept {
    const String text = scene.ReportText();
    return text.View().Contains(part);
}

} // namespace

// 단계 표(10.3.1절 A4f3 2번): 등록·찾기, 같은 종류 AlreadyExists, 모르는 종류 InvalidArgument,
// 다른 객체 해제 NotFound, 사용 중 등록·해제 Busy(사용 구간이 끝나면 다시 된다), 프로세스 표는
// 하나다.
TEST(CookArtifact_StageTable) {
    CookArtifactStageTable table;
    FakeStage stage;
    FakeStage other;
    CHECK(table.Find(CookArtifactKind::ShaderLibrary) == nullptr);
    REQUIRE(table.Register(stage));
    CHECK(table.Find(CookArtifactKind::ShaderLibrary) == &stage);
    CHECK(table.Find(static_cast<CookArtifactKind>(0)) == nullptr &&
          table.Find(static_cast<CookArtifactKind>(2)) == nullptr);
    CHECK(table.Register(other).Error() == ErrorCode::AlreadyExists);
    CHECK(table.Unregister(other).Error() == ErrorCode::NotFound);
    {
        CookArtifactStageUse use(table);
        CHECK(table.InUse());
        CHECK(table.Unregister(stage).Error() == ErrorCode::Busy);
    }

    CHECK(!table.InUse());
    REQUIRE(table.Unregister(stage));
    CHECK(table.Find(CookArtifactKind::ShaderLibrary) == nullptr);
    {
        CookArtifactStageUse use(table);
        CHECK(table.Register(stage).Error() == ErrorCode::Busy);
    }

    // 모르는 종류의 단계는 등록할 수 없다.
    class UnknownStage final : public CookArtifactStage {
    public:
        CookArtifactKind Kind() const noexcept override {
            return static_cast<CookArtifactKind>(2);
        }

        NuggetProcessorId Identity() const noexcept override {
            return {};
        }

        Result<void> Run(const CookArtifactRequest&, CookArtifactSink&) noexcept override {
            return Result<void>::Success();
        }
    };
    UnknownStage unknown;
    CHECK(table.Register(unknown).Error() == ErrorCode::InvalidArgument);
    CHECK(table.Unregister(unknown).Error() == ErrorCode::NotFound);

    CHECK(&CookArtifactStages() == &CookArtifactStages());
    REQUIRE(CookArtifactStages().Register(stage));
    CHECK(CookArtifactStages().Find(CookArtifactKind::ShaderLibrary) == &stage);
    REQUIRE(CookArtifactStages().Unregister(stage));
    CHECK(CookArtifactCallName(CookArtifactCall::None) == "None" &&
          CookArtifactCallName(CookArtifactCall::Run) == "Run" &&
          CookArtifactCallName(CookArtifactCall::NewStagingPath) == "NewStagingPath" &&
          CookArtifactCallName(CookArtifactCall::Produce) == "Produce" &&
          CookArtifactCallName(CookArtifactCall::Reuse) == "Reuse" &&
          CookArtifactCallName(CookArtifactCall::Report) == "Report" &&
          CookArtifactCallName(CookArtifactCall::ReportStat) == "ReportStat");
    // 범위 밖 값은 None이다(계약 밖 — 진단 문자열이 비지 않게).
    CHECK(CookArtifactCallName(static_cast<CookArtifactCall>(0xFF)) == "None");
}

// 게시와 재사용(10.3.1절 A4f3 3·4번): 전체 쿠킹은 형식 둘 × 이름 둘을 게시하고 매니페스트 항목의
// 크기·SHA-256은 게시 파일과 같다(쿠커가 계산 — FindArtifact·CheckCookedArtifactFile 통과).
// 보고서는 artifacts·stageStats를 담는다. 두 번째 쿠킹은 후보 넷을 모두 재사용한다(새로 쓴 byte 0).
// 게시 파일을 바꾸면 그 항목은 후보가 아니고, 다시 만든 같은 이름이 손상 파일을 만나 CorruptEntry
// 경고 뒤 교체한다. seed가 바뀐 전체 쿠킹은 새 파일을 게시하고 옛 파일과 떠도는 파일을 지운다.
TEST(CookArtifact_PublishesAndReuses) {
    AssetEnv env;
    REQUIRE(env.Ready());
    StageScene scene(env, "cook-artifact-publish");
    REQUIRE(scene.ready);
    CookProjectResult first(env.allocator);
    REQUIRE(CookProject(env.allocator, scene.Request(), first));
    CHECK(first.published && !first.HasErrors() && scene.stage.runs == 1 &&
          scene.stage.produced == 4 && scene.stage.offered == 0);
    REQUIRE(first.artifacts.Size() == 4);
    auto manifest = scene.Manifest();
    REQUIRE(manifest && manifest.Value().artifacts.Size() == 4);
    for (const CookedManifestArtifact& artifact : manifest.Value().artifacts) {
        const String name = scene.Published(artifact);
        const Span<const uint8> bytes = scene.project.files.Bytes(name.View());
        CHECK(!bytes.IsEmpty() && bytes.Size() == artifact.bytes);
        CHECK(CheckCookedArtifactFile(artifact, bytes));
        CHECK(manifest.Value().FindArtifact(artifact.kind, artifact.name.View(),
                                            artifact.formatId.View()) == &artifact);
    }

    CHECK(manifest.Value().artifacts[0].name.View() == "Engine" &&
          manifest.Value().artifacts[0].formatId.View() == "D3D12_SM6");
    // 단계는 이번 게시가 담을 패키지 전부를 AssetId 순으로 받는다.
    CHECK(manifest.Value().entries.Size() > 1 &&
          scene.stage.packages == manifest.Value().entries.Size() && scene.stage.packagesOrdered);
    CHECK(ReportHas(scene.project, "\"kind\":\"ShaderLibrary\"") &&
          ReportHas(scene.project, "\"reused\":false") &&
          ReportHas(scene.project, "\"name\":\"libraries\",\"value\":4"));
    CHECK(scene.ShaderFiles() == 4);

    // 다시 쿠킹: 후보 넷을 모두 재사용한다.
    CookProjectResult again(env.allocator);
    REQUIRE(CookProject(env.allocator, scene.Request(), again));
    CHECK(again.published && scene.stage.offered == 4 && scene.stage.reused == 4 &&
          scene.stage.produced == 4 && again.bytesWritten == 0);
    REQUIRE(again.artifacts.Size() == 4);
    CHECK(again.artifacts[0].reused && ReportHas(scene.project, "\"reused\":true"));
    // 재사용한 패키지도 목록에 있다(Cook이 불리지 않은 대상 — 16.5절 SSD87).
    CHECK(again.reused > 0 && scene.stage.packages == manifest.Value().entries.Size() &&
          scene.stage.packagesOrdered);

    // 게시 파일 손상: 그 항목은 후보가 아니고, 다시 만든 파일이 손상 파일을 교체한다.
    auto current = scene.Manifest();
    REQUIRE(current);
    const String damaged = scene.Published(current.Value().artifacts[1]);
    Array<uint8> broken(env.allocator);
    REQUIRE(broken.TryAppend(scene.project.files.Bytes(damaged.View())));
    broken[0] ^= 1;
    scene.project.files.Remove(damaged.View());
    REQUIRE(scene.project.files.AddFile(damaged.View(), broken.View()));
    CookProjectResult repaired(env.allocator);
    REQUIRE(CookProject(env.allocator, scene.Request(), repaired));
    CHECK(repaired.published && scene.stage.offered == 3 && scene.stage.produced == 5);
    CHECK(HasDiagnostic(repaired, AssetDiagnosticCode::CorruptEntry, ErrorCode::None, true));
    CHECK(CheckCookedArtifactFile(current.Value().artifacts[1],
                                  scene.project.files.Bytes(damaged.View())));

    // seed가 바뀐 전체 쿠킹: 새 파일 넷, 옛 파일과 떠도는 파일은 지운다.
    const uint8 junk[] = {'J'};
    REQUIRE(scene.project.files.AddFile("Saved/Cooked/Windows/Shaders/Stray.rshl", junk));
    scene.stage.seed = 2;
    CookProjectResult fresh(env.allocator);
    REQUIRE(CookProject(env.allocator, scene.Request(), fresh));
    CHECK(fresh.published && scene.ShaderFiles() == 4 &&
          !scene.project.files.Has("Saved/Cooked/Windows/Shaders/Stray.rshl") &&
          !scene.project.files.Has(damaged.View()));
}

// 부분 쿠킹과 verify(10.3.1절 A4f3 3·4번): seed가 바뀐 부분 쿠킹은 새 파일을 게시하고 옛 파일을
// 지우지 않는다(매니페스트는 새 항목). verify 쿠킹은 후보가 없다(같은 내용이라 쓴 byte는 0).
// 머리(프로파일 출력 설정)가 다르면 후보가 없다.
TEST(CookArtifact_PartialAndVerify) {
    AssetEnv env;
    REQUIRE(env.Ready());
    StageScene scene(env, "cook-artifact-partial");
    REQUIRE(scene.ready);
    CookProjectResult first(env.allocator);
    REQUIRE(CookProject(env.allocator, scene.Request(), first) && first.published);
    REQUIRE(scene.ShaderFiles() == 4);

    scene.stage.seed = 2;
    const StringView hero[] = {"/Game/Characters/Hero"};
    CookProjectResult partial(env.allocator);
    REQUIRE(CookProject(env.allocator, scene.Request(hero), partial));
    CHECK(partial.published && partial.partial && scene.ShaderFiles() == 8);
    auto manifest = scene.Manifest();
    REQUIRE(manifest && manifest.Value().artifacts.Size() == 4);
    // 부분 쿠킹의 패키지 목록도 게시 전체다(다시 쿠킹한 것·재사용한 것, 병합할 이전 항목).
    CHECK(scene.stage.packages == manifest.Value().entries.Size() && scene.stage.packagesOrdered);
    for (const CookedManifestArtifact& artifact : manifest.Value().artifacts) {
        CHECK(CheckCookedArtifactFile(artifact,
                                      scene.project.files.Bytes(scene.Published(artifact).View())));
    }

    CookProjectRequest verify = scene.Request();
    verify.verifyNuggets = true;
    CookProjectResult verified(env.allocator);
    REQUIRE(CookProject(env.allocator, verify, verified));
    CHECK(verified.published && scene.stage.offered == 0);

    CookProjectRequest other = scene.Request();
    other.profileSettings = Sha256::Hash(Span<const uint8>(reinterpret_cast<const uint8*>("Z"), 1));
    CookProjectResult changed(env.allocator);
    REQUIRE(CookProject(env.allocator, other, changed));
    CHECK(changed.published && scene.stage.offered == 0);
}

// 오류와 능력(10.3.1절 A4f3 3번): job 오류는 게시하지 않는다(보고서에 남음). 단계 미등록은 오류
// 진단(code None, UnsupportedCapability, asset null)이고 게시하지 않는다. 셰이더 없는 프로필은
// 등록되어 있어도 단계를 부르지 않는다. 에셋 오류(사라진 파일)가 있어도 단계는 돈다.
TEST(CookArtifact_ErrorsAndCapability) {
    AssetEnv env;
    REQUIRE(env.Ready());
    StageScene scene(env, "cook-artifact-errors");
    REQUIRE(scene.ready);
    scene.stage.failJob = true;
    CookProjectResult failed(env.allocator);
    REQUIRE(CookProject(env.allocator, scene.Request(), failed));
    CHECK(!failed.published && failed.HasErrors() && scene.stage.runs == 1 &&
          !scene.project.files.Has(cook_project_test::Manifest));
    CHECK(HasDiagnostic(failed, AssetDiagnosticCode::None, ErrorCode::InvalidArgument, false));

    scene.stage.failJob = false;
    CookProjectRequest missing = scene.Request();
    missing.stages = nullptr;
    CookProjectResult unsupported(env.allocator);
    REQUIRE(CookProject(env.allocator, missing, unsupported));
    CHECK(!unsupported.published && scene.stage.runs == 1 &&
          HasDiagnostic(unsupported, AssetDiagnosticCode::None, ErrorCode::UnsupportedCapability,
                        false));
    CHECK(ReportHas(scene.project, "\"error\":\"UnsupportedCapability\""));

    TargetProfile shaderless(env.allocator);
    REQUIRE(shaderless.id.TryAssign("Windows"));
    CookProjectRequest plain = scene.Request();
    plain.profile = &shaderless;
    CookProjectResult quiet(env.allocator);
    REQUIRE(CookProject(env.allocator, plain, quiet));
    CHECK(quiet.published && scene.stage.runs == 1 && quiet.artifacts.IsEmpty());

    scene.project.files.Remove("Content/Leaf.rasset");
    CookProjectResult assetError(env.allocator);
    REQUIRE(CookProject(env.allocator, scene.Request(), assetError));
    CHECK(!assetError.published && scene.stage.runs == 2 && assetError.artifacts.Size() == 4);
}

// 계약 위반(10.3.1절 A4f3 3번 5): 위반마다 CookProject가 InvalidArgument로 멈추고(게시 안 함),
// stageFailure가 호출·단계 정체성·형식을 담는다.
TEST(CookArtifact_ContractViolations) {
    struct Row {
        const char* name;
        Violation violation;
        CookArtifactCall call;
        StringView format;
    };
    const Row rows[] = {
        {"bad name", Violation::BadName, CookArtifactCall::Produce, "D3D12_SM6"},
        {"bad format", Violation::BadFormat, CookArtifactCall::Produce, "d3d12_sm6"},
        {"format not in profile", Violation::NotInTable, CookArtifactCall::Produce, "METAL_SM5"},
        {"foreign staging path", Violation::ForeignPath, CookArtifactCall::Produce, "D3D12_SM6"},
        {"staging path twice", Violation::PathTwice, CookArtifactCall::Produce, "D3D12_SM6"},
        {"duplicate key", Violation::Duplicate, CookArtifactCall::Produce, "VULKAN_SM5"},
        {"empty file", Violation::EmptyFile, CookArtifactCall::Produce, "D3D12_SM6"},
        {"reuse not offered", Violation::ReuseForeign, CookArtifactCall::Reuse, "D3D12_SM6"},
    };
    AssetEnv env;
    REQUIRE(env.Ready());
    StageScene scene(env, "cook-artifact-contract");
    REQUIRE(scene.ready);
    for (const Row& row : rows) {
        scene.stage.violation = row.violation;
        CookProjectResult result(env.allocator);
        auto cooked = CookProject(env.allocator, scene.Request(), result);
        const CookArtifactFailure& failure = result.stageFailure;
        const bool same = !cooked && cooked.Error() == ErrorCode::InvalidArgument &&
                          failure.call == row.call && failure.stage == FakeStageGuid &&
                          failure.formatId.View() == row.format &&
                          failure.error == ErrorCode::InvalidArgument &&
                          !scene.project.files.Has(cook_project_test::Manifest);
        if (!same) {
            test::RecordFailure(row.name, __FILE__, __LINE__);
        }
    }
}

// 설정과 사용 중(10.3.1절 A4f3 1·2번): cookDevelopmentShaders는 요청으로 단계에 닿는다(cookKey가
// 바뀌어 재사용 후보와 다르다 — 다시 만든다, SHA-256 변화는 CookConfig_Hashes). 단계가 도는 동안 표
// 해제·등록은 Busy다.
TEST(CookArtifact_SettingsAndBusy) {
    AssetEnv env;
    REQUIRE(env.Ready());
    StageScene scene(env, "cook-artifact-settings");
    REQUIRE(scene.ready);
    CookProjectResult first(env.allocator);
    REQUIRE(CookProject(env.allocator, scene.Request(), first) && first.published);
    CHECK(!scene.stage.sawDevelopment);
    scene.project.settings.cookDevelopmentShaders = true;
    scene.stage.table = &scene.table;
    CookProjectResult second(env.allocator);
    REQUIRE(CookProject(env.allocator, scene.Request(), second) && second.published);
    CHECK(scene.stage.sawDevelopment && scene.stage.offered == 4 && scene.stage.reused == 0 &&
          scene.stage.produced == 8);
    CHECK(scene.stage.unregisterDuring == ErrorCode::Busy &&
          scene.stage.registerDuring == ErrorCode::Busy);
    CHECK(scene.table.Find(CookArtifactKind::ShaderLibrary) == &scene.stage &&
          !scene.table.InUse());
}

// 할당 실패(모든 지점): 첫 쿠킹(게시)과 재사용 쿠킹이 OutOfMemory로 끝나고 표는 사용 중이 아니다.
TEST(CookArtifact_AllocationFailures) {
    AssetEnv env;
    REQUIRE(env.Ready());
    StageScene scene(env, "cook-artifact-oom");
    REQUIRE(scene.ready);
    test::NFailAllocator failing(env.allocator);
    for (int pass = 0; pass < 2; ++pass) {
        uint32 failures = 0;
        for (uint64 attempt = 1;; ++attempt) {
            if (pass == 0) {
                scene.project.files.Remove(cook_project_test::Manifest);
            }

            CookProjectResult result(failing);
            failing.FailOnAttempt(attempt);
            auto cooked = CookProject(failing, scene.Request(), result);
            const bool hit = failing.Attempts() >= attempt;
            failing.DisableFailure();
            CHECK(!scene.table.InUse());
            if (!hit) {
                REQUIRE(cooked);
                CHECK(result.published && result.artifacts.Size() == 4);
                break;
            }

            CHECK(cooked.Error() == ErrorCode::OutOfMemory);
            ++failures;
        }

        CHECK(failures >= 20u);
    }
}

namespace {

// 엔진 전용 쿠킹 장면이다: 임시 "엔진" 디렉터리(Saved 아래 잠금·쿠킹 루트)와 그 메모리 파일 시스템,
// 설정·보고서 struct를 등록한 TypeRegistry·Registry, 셰이더 있는 프로필과 가짜 단계.
struct EngineScene {
    explicit EngineScene(AssetEnv& sceneEnv, const char* tag) noexcept
        : env(sceneEnv), root(tag), saved(env.allocator), files(env.allocator, root.Root()),
          registry(env.allocator, env.types, ContextId{0x454e47494e455431ull, 1}),
          profile(env.allocator), settings(env.allocator) {
        ready = root.made && files.Ready() && RegisterCookerTypes(env) &&
                saved.TryAppendFormat("{}/Saved", root.Root()) && profile.id.TryAssign("Windows") &&
                AddShaders(env, profile) &&
                static_cast<bool>(lock.Acquire(env.allocator, saved.View(), "Windows", 0)) &&
                static_cast<bool>(table.Register(stage));
    }

    ~EngineScene() {
        (void)table.Unregister(stage);
    }

    CookEngineRequest Request() noexcept {
        CookEngineRequest request;
        request.files = &files;
        request.savedDirectory = saved.View();
        request.cookLock = &lock;
        request.types = &env.types;
        request.registry = &registry;
        request.access = registry.OwnerAccess();
        request.profile = &profile;
        request.profileSettings =
            Sha256::Hash(Span<const uint8>(reinterpret_cast<const uint8*>("E"), 1));
        request.settings = &settings;
        request.cookSettings =
            Sha256::Hash(Span<const uint8>(reinterpret_cast<const uint8*>("S"), 1));
        request.cookedBy = TestEngineVersion;
        request.stages = &table;
        return request;
    }

    // 쿠킹 루트 기준 메모리 파일 시스템 이름이다.
    String Name(StringView rest) noexcept {
        String name(env.allocator);
        CHECK(name.TryAssign("Saved/Cooked/Windows/") && name.TryAppend(rest));
        return name;
    }

    Result<CookedManifest> Manifest() noexcept {
        const String name = Name(CookedManifestFileName);
        return ReadCookedManifest(env.allocator, files.Bytes(name.View()));
    }

    AssetEnv& env;
    TempProject root;
    StringBuilder<StagingInlineBytes> saved;
    MemoryFileSystem files;
    ObjectRegistry registry;
    TargetProfile profile;
    CookSettings settings;
    CookRootLock lock;
    CookArtifactStageTable table;
    FakeStage stage;
    bool ready = false;
};

} // namespace

// 엔진 전용 쿠킹(10.3.1절 A4f4 3번): Engine artifact만(형식 둘) 게시하고 항목은 없다(entryCount 0),
// 단계는 engineOnly 요청을 받는다. 다시 쿠킹은 후보를 재사용한다. verify는 요청에 닿고 후보가 없다.
// 셰이더 없는 프로필은 빈 매니페스트(backend 0·artifact 0)를 게시하고 단계를 부르지 않는다.
// 미등록은 UnsupportedCapability로 게시하지 않는다. 깨진 이전 매니페스트는 경고 뒤 전체 쿠킹이다.
// 보고서를 쓴다.
TEST(CookEngine_PublishesEngineLibraries) {
    AssetEnv env;
    REQUIRE(env.Ready());
    EngineScene scene(env, "cook-engine");
    REQUIRE(scene.ready);
    CookProjectResult first(env.allocator);
    REQUIRE(CookEngine(env.allocator, scene.Request(), first));
    CHECK(first.published && !first.HasErrors() && scene.stage.sawEngineOnly &&
          scene.stage.produced == 2 && first.targets == 0);
    auto manifest = scene.Manifest();
    REQUIRE(manifest);
    CHECK(manifest.Value().entries.IsEmpty() && manifest.Value().backends.Size() == 2 &&
          manifest.Value().artifacts.Size() == 2);
    for (const CookedManifestArtifact& artifact : manifest.Value().artifacts) {
        CHECK(artifact.name.View() == CookedArtifactEngineName);
        StringBuilder<StagingInlineBytes> path(env.allocator);
        REQUIRE(AppendCookedArtifactPath(path, artifact));
        const String name = scene.Name(path.View());
        CHECK(CheckCookedArtifactFile(artifact, scene.files.Bytes(name.View())));
    }

    CHECK(scene.files.Has(scene.Name(CookReportFileName).View()));

    CookProjectResult again(env.allocator);
    REQUIRE(CookEngine(env.allocator, scene.Request(), again));
    CHECK(again.published && scene.stage.offered == 2 && scene.stage.reused == 2 &&
          again.bytesWritten == 0);

    CookEngineRequest verify = scene.Request();
    verify.verifyNuggets = true;
    CookProjectResult verified(env.allocator);
    REQUIRE(CookEngine(env.allocator, verify, verified));
    CHECK(verified.published && scene.stage.sawVerify && scene.stage.offered == 0);

    // 깨진 이전 매니페스트: 경고 뒤 없는 것으로 보고 전체 쿠킹한다.
    const String manifestName = scene.Name(CookedManifestFileName);
    const uint8 junk[] = {'J', 'U', 'N', 'K'};
    scene.files.Remove(manifestName.View());
    REQUIRE(scene.files.AddFile(manifestName.View(), junk));
    CookProjectResult repaired(env.allocator);
    REQUIRE(CookEngine(env.allocator, scene.Request(), repaired));
    CHECK(
        repaired.published && scene.stage.offered == 0 &&
        HasDiagnostic(repaired, AssetDiagnosticCode::CorruptPackage, ErrorCode::CorruptData, true));

    CookEngineRequest missing = scene.Request();
    missing.stages = nullptr;
    CookProjectResult unsupported(env.allocator);
    const uint32 runs = scene.stage.runs;
    REQUIRE(CookEngine(env.allocator, missing, unsupported));
    CHECK(!unsupported.published && scene.stage.runs == runs &&
          HasDiagnostic(unsupported, AssetDiagnosticCode::None, ErrorCode::UnsupportedCapability,
                        false));

    TargetProfile shaderless(env.allocator);
    REQUIRE(shaderless.id.TryAssign("Windows"));
    CookEngineRequest plain = scene.Request();
    plain.profile = &shaderless;
    CookProjectResult quiet(env.allocator);
    REQUIRE(CookEngine(env.allocator, plain, quiet));
    auto empty = scene.Manifest();
    REQUIRE(quiet.published && empty);
    CHECK(empty.Value().backends.IsEmpty() && empty.Value().artifacts.IsEmpty() &&
          scene.stage.runs == runs);
}

// 계약·거부(10.3.1절 A4f4 3번): engineOnly를 따르지 않은 단계(Project Produce)는 InvalidArgument로
// 멈추고 stageFailure가 Produce(D3D12_SM6)다. 인자 null·빈 savedDirectory·잠금 없음·다른 쿠킹
// 루트의 잠금은 InvalidArgument다.
TEST(CookEngine_ContractAndRejects) {
    AssetEnv env;
    REQUIRE(env.Ready());
    EngineScene scene(env, "cook-engine-rejects");
    REQUIRE(scene.ready);
    scene.stage.respectEngineOnly = false;
    CookProjectResult result(env.allocator);
    auto cooked = CookEngine(env.allocator, scene.Request(), result);
    CHECK(!cooked && cooked.Error() == ErrorCode::InvalidArgument &&
          result.stageFailure.call == CookArtifactCall::Produce &&
          result.stageFailure.formatId.View() == "D3D12_SM6" &&
          !scene.files.Has(scene.Name(CookedManifestFileName).View()));
    scene.stage.respectEngineOnly = true;

    const auto rejects = [&](void (*mutate)(CookEngineRequest&)) noexcept {
        CookEngineRequest request = scene.Request();
        mutate(request);
        return CookEngine(env.allocator, request, result).Error() == ErrorCode::InvalidArgument;
    };
    CHECK(rejects([](CookEngineRequest& r) noexcept { r.files = nullptr; }));
    CHECK(rejects([](CookEngineRequest& r) noexcept { r.savedDirectory = {}; }));
    CHECK(rejects([](CookEngineRequest& r) noexcept { r.cookLock = nullptr; }));
    CHECK(rejects([](CookEngineRequest& r) noexcept { r.types = nullptr; }));
    CHECK(rejects([](CookEngineRequest& r) noexcept { r.registry = nullptr; }));
    CHECK(rejects([](CookEngineRequest& r) noexcept { r.profile = nullptr; }));
    CHECK(rejects([](CookEngineRequest& r) noexcept { r.settings = nullptr; }));
    CookRootLock unheld;
    CookEngineRequest unlocked = scene.Request();
    unlocked.cookLock = &unheld;
    CHECK(CookEngine(env.allocator, unlocked, result).Error() == ErrorCode::InvalidArgument);
    CookRootLock other;
    REQUIRE(other.Acquire(env.allocator, scene.saved.View(), "Android", 0));
    CookEngineRequest elsewhere = scene.Request();
    elsewhere.cookLock = &other;
    CHECK(CookEngine(env.allocator, elsewhere, result).Error() == ErrorCode::InvalidArgument);
}

// 할당 실패(모든 지점): 첫 엔진 쿠킹과 재사용 쿠킹이 OutOfMemory로 끝나고 표는 사용 중이 아니다.
TEST(CookEngine_AllocationFailures) {
    AssetEnv env;
    REQUIRE(env.Ready());
    EngineScene scene(env, "cook-engine-oom");
    REQUIRE(scene.ready);
    const String manifestName = scene.Name(CookedManifestFileName);
    test::NFailAllocator failing(env.allocator);
    for (int pass = 0; pass < 2; ++pass) {
        uint32 failures = 0;
        for (uint64 attempt = 1;; ++attempt) {
            if (pass == 0) {
                scene.files.Remove(manifestName.View());
            }

            CookProjectResult result(failing);
            failing.FailOnAttempt(attempt);
            auto cooked = CookEngine(failing, scene.Request(), result);
            const bool hit = failing.Attempts() >= attempt;
            failing.DisableFailure();
            CHECK(!scene.table.InUse());
            if (!hit) {
                REQUIRE(cooked);
                CHECK(result.published && result.artifacts.Size() == 2);
                break;
            }

            CHECK(cooked.Error() == ErrorCode::OutOfMemory);
            ++failures;
        }

        CHECK(failures >= 10u);
    }
}

} // namespace cook_artifact_tests_file
