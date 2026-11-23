#include "../../../Runtime/AssetRuntime/Tests/AssetFixtureValues.h"
#include "../../../Runtime/AssetRuntime/Tests/PackageEditing.h"
#include "AssetDatabaseTestSupport.h"
#include "EditorPackageSetTestSupport.h"

#include <AssetDatabase/AssetService.h>
#include <AssetDatabase/LogicalPath.h>
#include <AssetRuntime/AssetTypeHooks.h>
#include <AssetRuntime/Package.h>
#include <TestSupport/NFailAllocator.h>

// 에셋 서비스 API 시험이다(Docs/Design/AssetSystem 13절·7.1절 15 A2c3): asset-new(타입 도구
// 이름·대입·참조·저장, 타입·경로·대입·저장 거부), asset-set(경로·AssetId 대상, 대입 모두 검증,
// --force, 거부), asset-info(Edit 줄·Report 줄·JSON, 줄로 쓸 수 없는 값), asset-list(--type 파생
// 포함·등록되지 않은 타입, --under), 수명(Initialize 전·두 번·설정 오류·Content 열거 실패·집합
// 초기화 실패), 모든 할당 실패.
using namespace rocks;
namespace asset_service_tests_file {

using namespace rocks::asset_test;
using namespace rocks::asset_database_test;
using namespace rocks::editor_set_test;
using reflection_text::TextAssignment;
using reflection_text::TextDiagnosticCode;
using reflection_text::TextDiagnostics;

namespace {

// Content 파일 이름(정렬 순)과 그 논리 경로다.
constexpr StringView ContentFiles[] = {"Base.rasset", "Derived.rasset", "Hero.rasset",
                                       "Hub.rasset",  "Leaf.rasset",    "Villain.rasset"};
constexpr usize ContentCount = sizeof(ContentFiles) / sizeof(ContentFiles[0]);

AssetServiceConfig ServiceConfig(AssetEnv& env, Project& project,
                                 const AssetTypeHookTable& hooks) noexcept {
    AssetServiceConfig config;
    config.packages = Config(env, project);
    config.packages.hooks = &hooks;
    return config;
}

bool AddContent(MemoryFileSystem& files, Sources& sources) noexcept {
    const Array<uint8>* const bytes[] = {&sources.base, &sources.derived, &sources.hero,
                                         &sources.hub,  &sources.leaf,    &sources.villain};
    for (usize index = 0; index < ContentCount; ++index) {
        if (!files.AddFile(ContentFiles[index], bytes[index]->View())) {
            return false;
        }
    }

    return true;
}

// 서비스 장면이다: 루트는 디스크 임시 디렉터리(잠금 파일), Content는 메모리(Base·Derived·Hero·Hub·
// Leaf·Villain — Hub의 CycleA·B는 없다), 빈 hook 표, 초기화한 서비스, 잡은 잠금. allocator는
// 서비스의 allocator다(할당 실패 시험이 바꾼다).
struct ServiceScene {
    ServiceScene(AssetEnv& env, Sources& sources, Allocator& allocator, TempProject& root) noexcept
        : project(env.allocator, root.Root()), hooks(env.allocator, env.types),
          service(allocator, project.layout, ServiceConfig(env, project, hooks)) {
        ready = project.ready && project.files.Ready() && AddContent(project.files, sources) &&
                root.made && static_cast<bool>(service.Initialize());
    }

    Project project;
    AssetTypeHookTable hooks;
    AssetService service;
    bool ready = false;
};

// 같은 Content의 새 서비스로 대상의 Edit 줄을 읽는다(저장된 결과 확인).
bool ReadBack(AssetEnv& env, Project& project, const AssetTypeHookTable& hooks, StringView target,
              String& out) noexcept {
    AssetService again(env.allocator, project.layout, ServiceConfig(env, project, hooks));
    return again.Initialize() && again.Info(target, AssetInfoOptions{}, out);
}

bool Has(const String& text, StringView expected) noexcept {
    return text.View().Contains(expected, StringComparison::CaseSensitive);
}

// 대입 텍스트 목록을 대입 묶음으로 만든다.
bool MakeAssignments(Span<const StringView> texts, Array<TextAssignment>& out) noexcept {
    for (usize index = 0; index < texts.Size(); ++index) {
        TextAssignment assignment;
        if (!reflection_text::MakeTextAssignment(texts[index], static_cast<uint32>(index),
                                                 assignment) ||
            !out.TryPushBack(assignment)) {
            return false;
        }
    }

    return true;
}

bool HasDiagnostic(const TextDiagnostics& diagnostics, TextDiagnosticCode code) noexcept {
    for (usize index = 0; index < diagnostics.Count(); ++index) {
        if (diagnostics.Get(index).code == code) {
            return true;
        }
    }

    return false;
}

String IdText(Allocator& allocator, AssetId id) noexcept {
    String text(allocator);
    auto formatted = TryFormat(allocator, "{}", id);
    CHECK(formatted && text.TryAssign(formatted.Value().View()));
    return text;
}

} // namespace

// asset-new: 타입 도구 이름의 새 패키지를 만들고 대입(값·문자열·Hard·Soft 참조)을 넣어 저장한다 —
// 파일·색인 항목이 생기고 새 서비스가 같은 값을 읽는다. 파생 에셋 클래스도 만든다.
TEST(AssetService_NewCreatesAndSaves) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    TempProject root("service-new");
    ServiceScene scene(env, sources, env.allocator, root);
    REQUIRE(scene.ready);
    AssetDatabaseLock lock;
    REQUIRE(lock.Acquire(scene.project.layout, 0));
    Array<TextAssignment> assignments(env.allocator);
    const StringView texts[] = {"value=5", "label=hi", "hard=/Game/Leaf", "soft=/Game/Hero"};
    REQUIRE(MakeAssignments(texts, assignments));
    TextDiagnostics diagnostics(env.allocator);
    Array<EditorWarning> warnings(env.allocator);
    auto made = scene.service.New(lock, "TestAsset", "/Game/Fresh/One", assignments.View(),
                                  diagnostics, &warnings);
    REQUIRE(made);
    CHECK(!made.Value().IsNull() && diagnostics.Count() == 0);
    CHECK(scene.project.files.Has("Fresh/One.rasset"));
    auto entry = scene.service.Index().FindById(made.Value());
    REQUIRE(entry);
    CHECK(entry.Value()->path.View() == "/Game/Fresh/One" &&
          entry.Value()->type == IdOf<TestAsset>());
    const EditorPackageInfo* info = scene.service.Packages().Find(made.Value());
    REQUIRE(info != nullptr);
    CHECK(!info->dirty && !info->unsaved);
    String lines(env.allocator);
    REQUIRE(ReadBack(env, scene.project, scene.hooks, "/Game/Fresh/One", lines));
    CHECK(Has(lines, "value=5\n") && Has(lines, "label=hi\n") && Has(lines, "hard=/Game/Leaf\n") &&
          Has(lines, "soft=/Game/Hero\n"));

    auto derived = scene.service.New(lock, "DerivedGameAsset", "/Game/Fresh/Two", {}, diagnostics);
    REQUIRE(derived);
    auto derivedEntry = scene.service.Index().FindById(derived.Value());
    REQUIRE(derivedEntry);
    CHECK(derivedEntry.Value()->type == IdOf<DerivedGameAsset>());
}

// asset-new 거부: 모르는 타입 이름 NotFound(진단 None), 빈 이름 InvalidArgument(진단 None), 에셋이
// 아닌 클래스·추상 에셋 클래스·struct TypeMismatch / InvalidArgument, 경로 규칙 위반 InvalidPath,
// 있는 경로 DuplicatePath, 대입 오류는 모두 모은 진단(모르는 속성·잘못된 값·풀 수 없는 참조)과
// InvalidArgument(진단 None)이고 아무것도 만들지 않는다. 저장 실패(파일 만들기 고장)는 그 오류이고
// 만든 패키지는 저장하지 않은 채 집합에 남는다. Initialize 전은 InvalidArgument다.
TEST(AssetService_NewRefusals) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    TempProject root("service-new-refusals");
    ServiceScene scene(env, sources, env.allocator, root);
    REQUIRE(scene.ready);
    AssetDatabaseLock lock;
    REQUIRE(lock.Acquire(scene.project.layout, 0));
    AssetService& service = scene.service;
    TextDiagnostics diagnostics(env.allocator);
    struct TypeCase {
        StringView type;
        AssetDiagnosticCode code;
        ErrorCode error;
    };
    const TypeCase types[] = {
        {"NoSuchType", AssetDiagnosticCode::None, ErrorCode::NotFound},
        {"", AssetDiagnosticCode::None, ErrorCode::InvalidArgument},
        {"TestPart", AssetDiagnosticCode::TypeMismatch, ErrorCode::InvalidArgument},
        {"Asset", AssetDiagnosticCode::TypeMismatch, ErrorCode::InvalidArgument},
        {"BulkDataRef", AssetDiagnosticCode::TypeMismatch, ErrorCode::InvalidArgument}};
    for (const TypeCase& item : types) {
        AssetDiagnostic failure;
        CHECK(service.New(lock, item.type, "/Game/Fresh/T", {}, diagnostics, nullptr, &failure)
                  .Error() == item.error);
        CHECK(failure.code == item.code && failure.error == item.error);
    }

    const StringView badPaths[] = {"Game/Fresh/T", "/Game//T", "/Game/T.x"};
    for (StringView path : badPaths) {
        AssetDiagnostic failure;
        CHECK(service.New(lock, "TestAsset", path, {}, diagnostics, nullptr, &failure).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(failure.code == AssetDiagnosticCode::InvalidPath);
    }

    AssetDiagnostic duplicate;
    CHECK(service.New(lock, "TestAsset", "/Game/Hero", {}, diagnostics, nullptr, &duplicate)
              .Error() == ErrorCode::AlreadyExists);
    CHECK(duplicate.code == AssetDiagnosticCode::DuplicatePath);

    Array<TextAssignment> wrong(env.allocator);
    const StringView texts[] = {"nope=1", "value=abc", "hard=/Game/Missing"};
    REQUIRE(MakeAssignments(texts, wrong));
    AssetDiagnostic rejected;
    CHECK(service
              .New(lock, "TestAsset", "/Game/Fresh/Bad", wrong.View(), diagnostics, nullptr,
                   &rejected)
              .Error() == ErrorCode::InvalidArgument);
    CHECK(rejected.code == AssetDiagnosticCode::None && diagnostics.Count() == 3);
    CHECK(HasDiagnostic(diagnostics, TextDiagnosticCode::UnknownProperty) &&
          HasDiagnostic(diagnostics, TextDiagnosticCode::InvalidValue) &&
          HasDiagnostic(diagnostics, TextDiagnosticCode::Unresolved));
    CHECK(service.Packages().Count() == 0 && !scene.project.files.Has("Fresh/Bad.rasset"));

    // 저장 실패.
    diagnostics.Clear();
    scene.project.files.faults.create = MemoryBusy;
    AssetDiagnostic saveFailure;
    auto unsaved = service.New(lock, "TestAsset", "/Game/Fresh/Unsaved", {}, diagnostics, nullptr,
                               &saveFailure);
    scene.project.files.faults.create = {};
    CHECK(!unsaved && saveFailure.code == AssetDiagnosticCode::None);
    REQUIRE(service.Packages().Count() == 1);
    CHECK(service.Packages().At(0).unsaved && service.Packages().At(0).dirty);

    // 잡지 않은 잠금.
    AssetDatabaseLock idle;
    CHECK(service.New(idle, "TestAsset", "/Game/Fresh/Locked", {}, diagnostics).Error() ==
          ErrorCode::InvalidArgument);
}

// asset-set: 경로·AssetId 대상에 대입을 모두 검증한 뒤 게시해 저장한다(새 서비스가 같은 값을
// 읽는다). 빈 대입도 저장한다. 미해결 import가 있는 패키지(Hub — CycleA 없음)는 force 없이
// MissingDependency, force면 저장하고 경고 DroppedUnresolvedImport다. 대입 오류는 모두 모은 진단과
// InvalidArgument이고 파일은 그대로다. 대상 텍스트 형식 InvalidPath, 없는 대상 NotFound, 잡지 않은
// 잠금 InvalidArgument다.
TEST(AssetService_SetAssignsAndSaves) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    TempProject root("service-set");
    ServiceScene scene(env, sources, env.allocator, root);
    REQUIRE(scene.ready);
    AssetDatabaseLock lock;
    REQUIRE(lock.Acquire(scene.project.layout, 0));
    AssetService& service = scene.service;
    TextDiagnostics diagnostics(env.allocator);
    Array<TextAssignment> assignments(env.allocator);
    const StringView texts[] = {"value=9", "label=set", "hardList=/Game/Leaf,/Game/Base"};
    REQUIRE(MakeAssignments(texts, assignments));
    REQUIRE(service.Set(lock, "/Game/Hero", assignments.View(), false, diagnostics));
    String lines(env.allocator);
    REQUIRE(ReadBack(env, scene.project, scene.hooks, "/Game/Hero", lines));
    CHECK(Has(lines, "value=9\n") && Has(lines, "label=set\n") &&
          Has(lines, "hardList=/Game/Leaf,/Game/Base\n"));

    const String leaf = IdText(env.allocator, FixedId(LeafId));
    Array<TextAssignment> leafValue(env.allocator);
    const StringView leafTexts[] = {"value=3"};
    REQUIRE(MakeAssignments(leafTexts, leafValue));
    REQUIRE(service.Set(lock, leaf.View(), leafValue.View(), false, diagnostics));
    lines.Clear();
    REQUIRE(ReadBack(env, scene.project, scene.hooks, "/Game/Leaf", lines));
    CHECK(Has(lines, "value=3\n"));
    CHECK(service.Set(lock, "/Game/Villain", {}, false, diagnostics));

    // force.
    AssetDiagnostic missing;
    CHECK(service.Set(lock, "/Game/Hub", {}, false, diagnostics, nullptr, &missing).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(missing.code == AssetDiagnosticCode::MissingDependency);
    Array<EditorWarning> warnings(env.allocator);
    REQUIRE(service.Set(lock, "/Game/Hub", {}, true, diagnostics, &warnings));
    bool dropped = false;
    for (const EditorWarning& warning : warnings) {
        dropped = dropped || warning.kind == EditorWarningKind::DroppedUnresolvedImport;
    }

    CHECK(dropped);

    // 대입 오류: 모두 모으고 아무것도 바꾸지 않는다.
    Array<uint8> before(env.allocator);
    REQUIRE(before.TryAppend(scene.project.files.Bytes("Hero.rasset")));
    Array<TextAssignment> wrong(env.allocator);
    const StringView wrongTexts[] = {"nope=1", "value=abc"};
    REQUIRE(MakeAssignments(wrongTexts, wrong));
    AssetDiagnostic rejected;
    CHECK(service.Set(lock, "/Game/Hero", wrong.View(), false, diagnostics, nullptr, &rejected)
              .Error() == ErrorCode::InvalidArgument);
    CHECK(rejected.code == AssetDiagnosticCode::None && diagnostics.Count() == 2);
    CHECK(scene.project.files.Bytes("Hero.rasset").Size() == before.Size());

    // 대상 거부.
    char upperBytes[AssetId::TextBytes];
    for (usize index = 0; index < AssetId::TextBytes; ++index) {
        const char c = leaf.Data()[index];
        upperBytes[index] = c >= 'a' && c <= 'f' ? static_cast<char>(c - 'a' + 'A') : c;
    }

    const StringView badTargets[] = {"Hero", "/Game//Hero",
                                     StringView(upperBytes, AssetId::TextBytes),
                                     "00000000-0000-0000-0000-000000000000"};
    for (StringView target : badTargets) {
        AssetDiagnostic failure;
        CHECK(service.Set(lock, target, {}, false, diagnostics, nullptr, &failure).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(failure.code == AssetDiagnosticCode::InvalidPath);
    }

    CHECK(service.Set(lock, "/Game/Nope", {}, false, diagnostics).Error() == ErrorCode::NotFound);
    const String nowhere = IdText(env.allocator, FixedId(NowhereId));
    CHECK(service.Set(lock, nowhere.View(), {}, false, diagnostics).Error() == ErrorCode::NotFound);
    AssetDatabaseLock idle;
    CHECK(service.Set(idle, "/Game/Hero", {}, false, diagnostics).Error() ==
          ErrorCode::InvalidArgument);
}

// Weak 참조 대입(5.1절 — 같은 패키지 안만 보존): asset-set의 Weak은 대상 패키지 안의 객체만
// 받고(저장·다시 읽기에서 그대로), 다른 패키지는 진단 Unresolved로 거부한다. asset-new의 Weak은 새
// 패키지의 객체가 아직 공개 전이라 거부한다(한계 — 만든 뒤 asset-set으로 넣는다).
TEST(AssetService_WeakReferences) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    TempProject root("service-weak");
    ServiceScene scene(env, sources, env.allocator, root);
    REQUIRE(scene.ready);
    AssetDatabaseLock lock;
    REQUIRE(lock.Acquire(scene.project.layout, 0));
    AssetService& service = scene.service;
    TextDiagnostics diagnostics(env.allocator);
    REQUIRE(service.New(lock, "ComboAsset", "/Game/Fresh/Combo", {}, diagnostics));
    Array<TextAssignment> inside(env.allocator);
    const StringView insideTexts[] = {"values.cWeak.value=/Game/Fresh/Combo"};
    REQUIRE(MakeAssignments(insideTexts, inside));
    REQUIRE(service.Set(lock, "/Game/Fresh/Combo", inside.View(), false, diagnostics));
    String lines(env.allocator);
    REQUIRE(ReadBack(env, scene.project, scene.hooks, "/Game/Fresh/Combo", lines));
    CHECK(Has(lines, "values.cWeak.value=/Game/Fresh/Combo\n"));

    Array<TextAssignment> outside(env.allocator);
    const StringView outsideTexts[] = {"values.cWeak.value=/Game/Hero"};
    REQUIRE(MakeAssignments(outsideTexts, outside));
    diagnostics.Clear();
    CHECK(service.Set(lock, "/Game/Fresh/Combo", outside.View(), false, diagnostics).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(diagnostics.Count() == 1 && diagnostics.Get(0).code == TextDiagnosticCode::Unresolved);
    diagnostics.Clear();
    CHECK(
        service.New(lock, "ComboAsset", "/Game/Fresh/Other", outside.View(), diagnostics).Error() ==
        ErrorCode::InvalidArgument);
    CHECK(diagnostics.Count() == 1 && diagnostics.Get(0).code == TextDiagnosticCode::Unresolved);
}

// 대표 에셋이 없어진 대상(리뷰 필수 — 서비스가 집합을 공개하므로 호출자가 열린 패키지의 대표 에셋을
// Discard할 수 있다): asset-info·asset-set은 NotFound(진단 None·그 패키지 id)다. 호출자의
// TypeRegistry가 닫히면 asset-list --type은 타입 조회의 ShuttingDown이다.
TEST(AssetService_DiscardedRepresentativeAndClosedTypes) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    TempProject root("service-discarded");
    ServiceScene scene(env, sources, env.allocator, root);
    REQUIRE(scene.ready);
    AssetDatabaseLock lock;
    REQUIRE(lock.Acquire(scene.project.layout, 0));
    AssetService& service = scene.service;
    String out(env.allocator);
    REQUIRE(service.Info("/Game/Leaf", AssetInfoOptions{}, out));
    EditorPackageSet& packages = service.Packages();
    REQUIRE(packages.Registry().Discard(packages.Access(), RepresentativeOf(packages, LeafId)));
    REQUIRE(packages.Registry().Collect(packages.Access()));
    AssetDiagnostic failure;
    CHECK(service.Info("/Game/Leaf", AssetInfoOptions{}, out, nullptr, &failure).Error() ==
          ErrorCode::NotFound);
    CHECK(failure.code == AssetDiagnosticCode::None && failure.asset == FixedId(LeafId));
    TextDiagnostics diagnostics(env.allocator);
    CHECK(service.Set(lock, "/Game/Leaf", {}, false, diagnostics).Error() == ErrorCode::NotFound);

    (void)env.types.Shutdown();
    Array<const AssetIndexEntry*> listed(env.allocator);
    CHECK(service.List("TestAsset", "", listed, &failure).Error() == ErrorCode::ShuttingDown);
    CHECK(failure.code == AssetDiagnosticCode::None);
}

// asset-info: Edit 줄(참조는 논리 경로, 색인에 없는 Soft는 AssetId), Report 줄(TestAsset은 Edit와
// 같다 — 출력 제외 플래그가 Transient뿐), JSON, AssetId 대상. 줄바꿈이 든 값은 줄로 쓸 수 없어
// Unsupported(진단 None)이고 JSON은 쓴다. 대상 거부는 asset-set과 같다.
TEST(AssetService_InfoFormats) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    TempProject root("service-info");
    ServiceScene scene(env, sources, env.allocator, root);
    REQUIRE(scene.ready);
    AssetService& service = scene.service;
    String lines(env.allocator);
    REQUIRE(service.Info("/Game/Hero", AssetInfoOptions{}, lines));
    const String softOnly = IdText(env.allocator, FixedId(SoftOnlyId));
    String softLine(env.allocator);
    REQUIRE(softLine.TryAssign("soft=") && softLine.TryAppend(softOnly.View()) &&
            softLine.TryAppend("\n"));
    CHECK(Has(lines, "value=7\n") && Has(lines, "hard=/Game/Base\n") &&
          Has(lines, "editorRef=/Game/Derived\n") && Has(lines, softLine.View()));
    String report(env.allocator);
    AssetInfoOptions all;
    all.all = true;
    REQUIRE(service.Info("/Game/Hero", all, report));
    CHECK(report.View() == lines.View());
    String json(env.allocator);
    AssetInfoOptions asJson;
    asJson.json = true;
    REQUIRE(service.Info("/Game/Hero", asJson, json));
    CHECK(json.View().StartsWith("{", StringComparison::CaseSensitive) &&
          Has(json, "\"value\": 7") && Has(json, "\"hard\": \"/Game/Base\""));
    const String hero = IdText(env.allocator, FixedId(HeroId));
    String byId(env.allocator);
    REQUIRE(service.Info(hero.View(), AssetInfoOptions{}, byId));
    CHECK(byId.View() == lines.View());

    // 줄로 쓸 수 없는 값.
    TestAsset* asset = Pointer<TestAsset>(service.Packages().Registry(),
                                          RepresentativeOf(service.Packages(), HeroId));
    REQUIRE(asset != nullptr);
    REQUIRE(asset->label.TryAssign("a\nb"));
    AssetDiagnostic failure;
    String broken(env.allocator);
    CHECK(service.Info("/Game/Hero", AssetInfoOptions{}, broken, nullptr, &failure).Error() ==
          ErrorCode::Unsupported);
    CHECK(failure.code == AssetDiagnosticCode::None);
    String brokenJson(env.allocator);
    REQUIRE(service.Info("/Game/Hero", asJson, brokenJson));
    CHECK(Has(brokenJson, "\"label\": \"a\\nb\""));

    AssetDiagnostic bad;
    CHECK(service.Info("Hero", AssetInfoOptions{}, broken, nullptr, &bad).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(bad.code == AssetDiagnosticCode::InvalidPath);
    CHECK(service.Info("/Game/Nope", AssetInfoOptions{}, broken).Error() == ErrorCode::NotFound);
    const String nowhere = IdText(env.allocator, FixedId(NowhereId));
    CHECK(service.Info(nowhere.View(), AssetInfoOptions{}, broken).Error() == ErrorCode::NotFound);

    // 호출자 출력 문자열의 할당 실패(JSON은 다 만든 뒤 붙인다).
    test::NFailAllocator failingOut(env.allocator);
    String jsonOut(failingOut);
    failingOut.FailOnAttempt(1);
    CHECK(service.Info("/Game/Leaf", asJson, jsonOut, nullptr, &failure).Error() ==
          ErrorCode::OutOfMemory);
    failingOut.DisableFailure();
    CHECK(failure.code == AssetDiagnosticCode::None && jsonOut.IsEmpty());
}

// asset-list: 경로 키 순 전체, --type은 그 클래스와 파생(TestAsset 넷, BaseGameAsset은
// Base·Derived, 추상 Asset은 전체, 에셋이 아닌 클래스는 빈 목록), 모르는 이름 NotFound, struct
// TypeMismatch,
// --under는 그 디렉터리 아래만(없는 디렉터리는 빈 목록, 논리 디렉터리가 아니면 InvalidPath). 타입을
// 등록하지 않은 Registry의 서비스에서는 그 항목이 어느 --type에도 맞지 않는다.
TEST(AssetService_ListFilters) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    TempProject root("service-list");
    ServiceScene scene(env, sources, env.allocator, root);
    REQUIRE(scene.ready);
    AssetService& service = scene.service;
    auto count = [&](StringView type, StringView under) noexcept -> usize {
        Array<const AssetIndexEntry*> out(env.allocator);
        auto listed = service.List(type, under, out);
        CHECK(listed);
        return out.Size();
    };
    Array<const AssetIndexEntry*> all(env.allocator);
    REQUIRE(service.List("", "", all));
    REQUIRE(all.Size() == ContentCount);
    const StringView paths[] = {"/Game/Base", "/Game/Derived", "/Game/Hero",
                                "/Game/Hub",  "/Game/Leaf",    "/Game/Villain"};
    for (usize index = 0; index < ContentCount; ++index) {
        CHECK(all[index]->path.View() == paths[index]);
    }

    CHECK(count("TestAsset", "") == 4);
    CHECK(count("BaseGameAsset", "") == 2);
    CHECK(count("DerivedGameAsset", "") == 1);
    CHECK(count("Asset", "") == ContentCount);
    CHECK(count("TestPart", "") == 0);
    CHECK(count("TestAsset", "/Game/Nowhere") == 0);
    AssetDiagnostic failure;
    Array<const AssetIndexEntry*> out(env.allocator);
    CHECK(service.List("NoSuchType", "", out, &failure).Error() == ErrorCode::NotFound);
    CHECK(service.List("BulkDataRef", "", out, &failure).Error() == ErrorCode::InvalidArgument);
    CHECK(failure.code == AssetDiagnosticCode::TypeMismatch);
    CHECK(service.List("", "Game", out, &failure).Error() == ErrorCode::InvalidArgument);
    CHECK(failure.code == AssetDiagnosticCode::InvalidPath);
    // 호출자 목록의 할당 실패.
    test::NFailAllocator failingOut(env.allocator);
    Array<const AssetIndexEntry*> outOfMemory(failingOut);
    failingOut.FailOnAttempt(1);
    CHECK(service.List("", "", outOfMemory, &failure).Error() == ErrorCode::OutOfMemory);
    failingOut.DisableFailure();
    CHECK(failure.code == AssetDiagnosticCode::None && outOfMemory.IsEmpty());

    AssetDatabaseLock lock;
    REQUIRE(lock.Acquire(scene.project.layout, 0));
    TextDiagnostics diagnostics(env.allocator);
    REQUIRE(service.New(lock, "TestAsset", "/Game/Fresh/Listed", {}, diagnostics));
    CHECK(count("", "/Game/Fresh") == 1 && count("TestAsset", "") == 5);

    // 타입을 등록하지 않은 Registry(AssetRuntime 타입만)의 서비스.
    TypeRegistry bare(env.allocator);
    ModuleRegistration registration(bare, env.allocator);
    REQUIRE(RegisterAssetRuntimeTypes(registration));
    {
        AssetServiceConfig config = ServiceConfig(env, scene.project, scene.hooks);
        config.packages.types = &bare;
        AssetService other(env.allocator, scene.project.layout, config);
        REQUIRE(other.Initialize());
        Array<const AssetIndexEntry*> listed(env.allocator);
        REQUIRE(other.List("Asset", "", listed));
        CHECK(listed.IsEmpty());
        REQUIRE(other.List("", "", listed));
        CHECK(listed.Size() == ContentCount + 1);
    }

    CHECK(registration.Unregister());
    CHECK(bare.Shutdown());
}

// 수명: Initialize 전에는 모든 함수가 InvalidArgument(진단 None), 두 번째 Initialize·타입이나 파일
// 시스템 없는 설정 InvalidArgument, Content가 파일이면 색인 Rebuild의 오류, 집합 설정 오류(파일
// 캐시 용량 0)는 집합 Initialize의 InvalidArgument다.
TEST(AssetService_Lifecycle) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    TempProject root("service-life");
    REQUIRE(root.made);
    Project project(env.allocator, root.Root());
    REQUIRE(project.ready && project.files.Ready() && AddContent(project.files, sources));
    AssetTypeHookTable hooks(env.allocator, env.types);
    AssetDatabaseLock lock;
    REQUIRE(lock.Acquire(project.layout, 0));
    TextDiagnostics diagnostics(env.allocator);
    {
        AssetService idle(env.allocator, project.layout, ServiceConfig(env, project, hooks));
        AssetDiagnostic failure;
        String out(env.allocator);
        Array<const AssetIndexEntry*> listed(env.allocator);
        CHECK(idle.New(lock, "TestAsset", "/Game/X", {}, diagnostics, nullptr, &failure).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(idle.Set(lock, "/Game/Hero", {}, false, diagnostics, nullptr, &failure).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(idle.Info("/Game/Hero", AssetInfoOptions{}, out, nullptr, &failure).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(idle.List("", "", listed, &failure).Error() == ErrorCode::InvalidArgument);
        CHECK(failure.code == AssetDiagnosticCode::None);
        REQUIRE(idle.Initialize());
        CHECK(idle.Initialize().Error() == ErrorCode::InvalidArgument);
    }

    AssetServiceConfig noTypes = ServiceConfig(env, project, hooks);
    noTypes.packages.types = nullptr;
    AssetServiceConfig noFiles = ServiceConfig(env, project, hooks);
    noFiles.packages.files = nullptr;
    AssetServiceConfig noCache = ServiceConfig(env, project, hooks);
    noCache.packages.fileCacheCapacity = 0;
    const AssetServiceConfig* const configs[] = {&noTypes, &noFiles, &noCache};
    for (const AssetServiceConfig* config : configs) {
        AssetService broken(env.allocator, project.layout, *config);
        CHECK(broken.Initialize().Error() == ErrorCode::InvalidArgument);
    }

    // Content가 파일(실제 디스크).
    TempProject fileRoot("service-content-file");
    REQUIRE(fileRoot.made);
    String content(env.allocator);
    REQUIRE(content.TryAssign(fileRoot.Root()) && content.TryAppend("/Content"));
    {
        platform::File file;
        REQUIRE(file.Open(platform::PhysicalFileSystem(), content.View().Data(),
                          content.View().ByteCount(), platform::FileOpenMode::CreateAlways));
        REQUIRE(file.Close());
    }

    ProjectLayout layout(env.allocator);
    REQUIRE(layout.Initialize(fileRoot.Root()));
    AssetServiceConfig physical = ServiceConfig(env, project, hooks);
    physical.packages.files = &platform::PhysicalFileSystem();
    AssetService onFile(env.allocator, layout, physical);
    CHECK(!onFile.Initialize());
}

namespace {

// 할당 실패 시험의 한 번이다: 실패하지 않는 allocator로 장면을 만든 뒤 서비스 allocator의
// attempt번째 할당을 실패시켜 run을 부른다. 실패면 OutOfMemory, 실패가 없어지면 true다.
template <class Run>
bool SweepOnce(AssetEnv& env, Sources& sources, uint64 attempt, Run&& run,
               uint32& failures) noexcept {
    TempProject root("service-alloc");
    test::NFailAllocator failing(env.allocator);
    ServiceScene scene(env, sources, failing, root);
    AssetDatabaseLock lock;
    const bool ready = scene.ready && static_cast<bool>(lock.Acquire(scene.project.layout, 0));
    CHECK(ready);
    if (!ready) {
        return true;
    }

    failing.FailOnAttempt(attempt);
    const ErrorCode error = run(scene.service, lock);
    const bool triggered = failing.Attempts() >= attempt;
    failing.DisableFailure();
    if (!triggered) {
        CHECK(error == ErrorCode::None);
        return true;
    }

    // 대체 경로가 있는 할당(GC 참조 표 — trace로 대신한다)은 실패해도 성공한다.
    if (error != ErrorCode::None) {
        CHECK(error == ErrorCode::OutOfMemory);
        ++failures;
    }

    return false;
}

} // namespace

// 할당 실패: New(대입·참조 포함 — 초기화가 참조 대상 패키지를
// 연다)·Set·Info(줄·JSON)·List(--type)의 서비스 allocator 할당을 하나씩 실패시키면
// OutOfMemory이고(대체 경로가 있는 할당은 성공), 실패가 없어지면 성공한다. 이 시험이 Create의 표
// 자리를 초기화 앞에 잡던 결함(초기화의 열기가 그 자리를 써 공개 뒤 삽입이 실패했다)을 찾았다.
TEST(AssetService_AllocationFailures) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    Array<TextAssignment> assignments(env.allocator);
    const StringView texts[] = {"value=5", "label=a long label value", "hard=/Game/Leaf",
                                "soft=/Game/Hero", "hardList=/Game/Base,/Game/Leaf"};
    REQUIRE(MakeAssignments(texts, assignments));
    using Step = ErrorCode (*)(AssetService&, AssetDatabaseLock&, Span<const TextAssignment>,
                               Allocator&) noexcept;
    const Step steps[] = {
        [](AssetService& service, AssetDatabaseLock& lock, Span<const TextAssignment> inputs,
           Allocator& allocator) noexcept {
            TextDiagnostics diagnostics(allocator);
            auto made = service.New(lock, "TestAsset", "/Game/Fresh/Alloc", inputs, diagnostics);
            return made ? ErrorCode::None : made.Error();
        },
        [](AssetService& service, AssetDatabaseLock& lock, Span<const TextAssignment> inputs,
           Allocator& allocator) noexcept {
            TextDiagnostics diagnostics(allocator);
            auto set = service.Set(lock, "/Game/Villain", inputs, false, diagnostics);
            return set ? ErrorCode::None : set.Error();
        },
        [](AssetService& service, AssetDatabaseLock&, Span<const TextAssignment>,
           Allocator& allocator) noexcept {
            String out(allocator);
            auto shown = service.Info("/Game/Hero", AssetInfoOptions{}, out);
            return shown ? ErrorCode::None : shown.Error();
        },
        [](AssetService& service, AssetDatabaseLock&, Span<const TextAssignment>,
           Allocator& allocator) noexcept {
            String out(allocator);
            AssetInfoOptions json;
            json.json = true;
            auto shown = service.Info("/Game/Hero", json, out);
            return shown ? ErrorCode::None : shown.Error();
        },
        [](AssetService& service, AssetDatabaseLock&, Span<const TextAssignment>,
           Allocator& allocator) noexcept {
            Array<const AssetIndexEntry*> out(allocator);
            auto listed = service.List("TestAsset", "", out);
            return listed ? ErrorCode::None : listed.Error();
        }};
    for (Step step : steps) {
        uint32 failures = 0;
        for (uint64 attempt = 1;; ++attempt) {
            const bool done = SweepOnce(
                env, sources, attempt,
                [&](AssetService& service, AssetDatabaseLock& lock) noexcept {
                    return step(service, lock, assignments.View(), env.allocator);
                },
                failures);
            if (done) {
                break;
            }
        }

        CHECK(failures > 0);
    }
}

} // namespace asset_service_tests_file
