#include "../../../Runtime/AssetRuntime/Tests/AssetFixtureValues.h"
#include "../../../Runtime/AssetRuntime/Tests/PackageEditing.h"
#include "AssetDatabaseTestSupport.h"
#include "EditorPackageSetTestSupport.h"

#include <AssetDatabase/AssetTextResolver.h>
#include <AssetDatabase/EditorPackageSet.h>
#include <AssetDatabase/LogicalPath.h>
#include <AssetRuntime/Package.h>
#include <ReflectionText/TextAssignments.h>
#include <ReflectionText/TextLines.h>
#include <TestSupport/NFailAllocator.h>

// 에셋 참조 텍스트 resolver 시험이다(Docs/Design/AssetSystem.ko.md 4.1·4.2절·7.1절 15 A2c3): 논리
// 경로·AssetId 텍스트 × 대표 에셋·하위 객체·중첩 하위 객체 × Hard·Weak·Soft의 해석(Hard·Weak는
// 패키지를 열고 Soft는 열지 않는다), 기대 타입, 저장하지 않은 새 패키지, 형식 거부, 색인의
// 없음·같은 경로 키 둘, 열기 실패와 열기 경고, 출력(색인 경로·새 패키지 경로·AssetId 대신·하위 객체
// 경로· Package 자신 거부·사라진 handle), 대입 묶음·줄 출력과의 왕복, 모든 할당 실패.
using namespace rocks;
namespace asset_text_resolver_tests_file {

using namespace rocks::asset_test;
using namespace rocks::asset_database_test;
using namespace rocks::editor_set_test;
using reflection_text::AssignmentTarget;
using reflection_text::TextAssignment;
using reflection_text::TextCodecOptions;
using reflection_text::TextDiagnostics;
using reflection_text::TextReferenceKind;

namespace {

// 참조 종류의 반복 값이다(Hard·Weak는 같은 해석이다).
constexpr TextReferenceKind OpenKinds[] = {TextReferenceKind::Hard, TextReferenceKind::Weak};
// 출력 버퍼의 inline 크기다(작게 두어 출력 할당이 일어나게 한다).
constexpr usize SmallBuilderBytes = 8;
// 객체 경로 읽기가 출력 버퍼를 늘리게 하는 긴 하위 객체 이름이다(64 byte 이름 상한 안).
constexpr StringView LongPartName = "PartWithAVeryLongNameThatGrowsTheObjectPathBuffer";

// 장면이다: Content에 Base·Derived·Hero·Villain·Leaf·Hub(Hub의 CycleA·B는 없다 — 열기 경고), 선택
// 으로 같은 경로 키 둘(Twin·twin)과 Leaf와 같은 id의 사본(LeafCopy), 색인, 연 집합.
struct ResolverScene {
    ResolverScene(AssetEnv& env, Sources& sources, bool duplicates = false) noexcept
        : project(env.allocator), set(env.allocator, Config(env, project)) {
        MemoryFileSystem& files = project.files;
        ready = project.ready && files.Ready() &&
                files.AddFile("Base.rasset", sources.base.View()) &&
                files.AddFile("Derived.rasset", sources.derived.View()) &&
                files.AddFile("Hero.rasset", sources.hero.View()) &&
                files.AddFile("Villain.rasset", sources.villain.View()) &&
                files.AddFile("Leaf.rasset", sources.leaf.View()) &&
                files.AddFile("Hub.rasset", sources.hub.View()) &&
                (!duplicates || (files.AddFile("Twin.rasset", sources.cycleA.View()) &&
                                 files.AddFile("twin.rasset", sources.cycleB.View()) &&
                                 files.AddFile("LeafCopy.rasset", sources.leaf.View()))) &&
                project.Rebuild() && static_cast<bool>(set.Initialize());
    }

    // Hero를 열고 대표 에셋 아래 Body, 그 아래 Socket(TestPart)을 만든다.
    bool AddParts() noexcept {
        if (!set.Open(FixedId(HeroId))) {
            return false;
        }

        hero = RepresentativeOf(set, HeroId);
        ObjectRegistry& registry = set.Registry();
        auto madeBody = registry.NewObject<TestPart>(set.Access(), TestPartType(), hero, "Body");
        if (!madeBody) {
            return false;
        }

        body = madeBody.Value().Handle();
        auto madeSocket =
            registry.NewObject<TestPart>(set.Access(), TestPartType(), body, "Socket");
        if (!madeSocket) {
            return false;
        }

        socket = madeSocket.Value().Handle();
        return true;
    }

    Project project;
    EditorPackageSet set;
    ObjectHandle hero{};
    ObjectHandle body{};
    ObjectHandle socket{};
    bool ready = false;
};

String IdText(Allocator& allocator, uint64 id) noexcept {
    String text(allocator);
    auto formatted = TryFormat(allocator, "{}", FixedId(id));
    CHECK(formatted && text.TryAssign(formatted.Value().View()));
    return text;
}

// Hard·Weak 해석 결과 handle이다(실패면 그 오류).
Result<ObjectHandle> ResolveHandle(AssetTextResolver& resolver, Allocator& allocator,
                                   StringView text, TypeId expected = {},
                                   TextReferenceKind kind = TextReferenceKind::Hard) noexcept {
    FieldValue value(allocator);
    if (auto resolved = resolver.Resolve(text, expected, kind, value); !resolved) {
        return Result<ObjectHandle>::Failure(resolved.Error());
    }

    return Result<ObjectHandle>::Success(value.reference);
}

bool Formats(AssetTextResolver& resolver, Allocator& allocator, TextReferenceKind kind,
             const FieldValue& value, StringView expected) noexcept {
    StringBuilder<SmallBuilderBytes> out(allocator);
    auto formatted = resolver.Format(kind, value, out);
    return formatted && out.View() == expected;
}

} // namespace

// Hard 해석: 논리 경로·AssetId 텍스트가 대표 에셋을 열어(닫힘째 — Hero는 Base·Derived도) 그
// handle을 주고, 이미 열린 패키지는 그대로 잇는다. 하위 객체·중첩 하위 객체 경로를 찾는다. 기대
// 타입이 0(Object)·대상 자신·조상이면 받고 아니면 TypeMismatch다. 저장하지 않은 새 패키지는 경로·
// AssetId 둘 다로 찾는다. Hub를 열면 없는 의존(CycleA)의 경고가 warnings에 붙는다.
TEST(AssetTextResolver_ResolvesHard) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    ResolverScene scene(env, sources);
    REQUIRE(scene.ready);
    Array<EditorWarning> warnings(env.allocator);
    AssetTextResolver resolver(env.allocator, scene.set, scene.project.index, &warnings);
    EditorPackageSet& set = scene.set;
    const String leafId = IdText(env.allocator, LeafId);
    {
        const TextReferenceKind kind = TextReferenceKind::Hard;
        auto villain = ResolveHandle(resolver, env.allocator, "/Game/Villain", {}, kind);
        REQUIRE(villain);
        CHECK(villain.Value() == RepresentativeOf(set, VillainId));
        CHECK(set.Find(FixedId(HeroId)) != nullptr && set.Find(FixedId(BaseId)) != nullptr &&
              set.Find(FixedId(DerivedId)) != nullptr);
        auto leaf = ResolveHandle(resolver, env.allocator, leafId.View(), IdOf<TestAsset>(), kind);
        REQUIRE(leaf);
        CHECK(leaf.Value() == RepresentativeOf(set, LeafId));
        auto again = ResolveHandle(resolver, env.allocator, "/game/LEAF", IdOf<Asset>(), kind);
        CHECK(again && again.Value() == leaf.Value());
        CHECK(ResolveHandle(resolver, env.allocator, "/Game/Leaf", IdOf<BaseGameAsset>(), kind)
                  .Error() == ErrorCode::TypeMismatch);
        auto derived =
            ResolveHandle(resolver, env.allocator, "/Game/Derived", IdOf<BaseGameAsset>(), kind);
        CHECK(derived && derived.Value() == RepresentativeOf(set, DerivedId));
    }

    REQUIRE(scene.AddParts());
    auto body = ResolveHandle(resolver, env.allocator, "/Game/Hero.Body", TestPartType());
    CHECK(body && body.Value() == scene.body);
    auto socket = ResolveHandle(resolver, env.allocator, "/Game/Hero.Body.Socket");
    CHECK(socket && socket.Value() == scene.socket);
    const String heroId = IdText(env.allocator, HeroId);
    String heroSocket(env.allocator);
    REQUIRE(heroSocket.TryAssign(heroId.View()) && heroSocket.TryAppend(".body.SOCKET"));
    auto byId = ResolveHandle(resolver, env.allocator, heroSocket.View());
    CHECK(byId && byId.Value() == scene.socket);
    CHECK(ResolveHandle(resolver, env.allocator, "/Game/Hero.Body", IdOf<TestAsset>()).Error() ==
          ErrorCode::TypeMismatch);
    CHECK(ResolveHandle(resolver, env.allocator, "/Game/Hero.Missing").Error() ==
          ErrorCode::NotFound);
    CHECK(ResolveHandle(resolver, env.allocator, "/Game/Hero.Body.Missing").Error() ==
          ErrorCode::NotFound);

    // 저장하지 않은 새 패키지.
    auto path = ParseLogicalPath(env.allocator, "/Game/Fresh/Made");
    REQUIRE(path);
    auto made = set.Create(path.Value(), TestAssetType());
    REQUIRE(made);
    auto fresh = ResolveHandle(resolver, env.allocator, "/Game/Fresh/MADE");
    REQUIRE(fresh);
    auto freshRepresentative = set.Registry().FindChild(set.Access(), made.Value(), "");
    REQUIRE(freshRepresentative && fresh.Value() == freshRepresentative.Value());
    const AssetId freshId = AssetOf(set, made.Value())->Id();
    auto freshIdText = TryFormat(env.allocator, "{}", freshId);
    REQUIRE(freshIdText);
    auto freshById = ResolveHandle(resolver, env.allocator, freshIdText.Value().View());
    CHECK(freshById && freshById.Value() == fresh.Value());

    // 열기 경고(Hub의 없는 의존).
    CHECK(warnings.IsEmpty());
    auto hub = ResolveHandle(resolver, env.allocator, "/Game/Hub");
    REQUIRE(hub);
    CHECK(!warnings.IsEmpty());
}

// Weak 해석은 대입 대상의 패키지 안만이다(5.1절 — 저장이 밖 대상을 null로 쓰므로 받지 않는다):
// 범위가 없으면 모든 Weak 텍스트 InvalidArgument, 범위 패키지(Hero)의 대표 에셋·하위 객체·중첩 하위
// 객체(경로·AssetId)는 받고 기대 타입을 본다, 다른 패키지는 InvalidArgument이고 열지 않는다, 범위
// 패키지가 열려 있지 않으면 NotFound다. 출력은 Hard와 같다(시험 Formats).
TEST(AssetTextResolver_WeakStaysInPackage) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    ResolverScene scene(env, sources);
    REQUIRE(scene.ready);
    REQUIRE(scene.AddParts());
    AssetTextResolver resolver(env.allocator, scene.set, scene.project.index);
    const TextReferenceKind weak = TextReferenceKind::Weak;
    CHECK(ResolveHandle(resolver, env.allocator, "/Game/Hero.Body", {}, weak).Error() ==
          ErrorCode::InvalidArgument);
    resolver.SetWeakScope(FixedId(HeroId));
    auto hero = ResolveHandle(resolver, env.allocator, "/Game/Hero", {}, weak);
    CHECK(hero && hero.Value() == scene.hero);
    auto body = ResolveHandle(resolver, env.allocator, "/Game/Hero.Body", TestPartType(), weak);
    CHECK(body && body.Value() == scene.body);
    const String heroId = IdText(env.allocator, HeroId);
    String heroSocket(env.allocator);
    REQUIRE(heroSocket.TryAssign(heroId.View()) && heroSocket.TryAppend(".Body.Socket"));
    auto socket = ResolveHandle(resolver, env.allocator, heroSocket.View(), {}, weak);
    CHECK(socket && socket.Value() == scene.socket);
    CHECK(ResolveHandle(resolver, env.allocator, "/Game/Hero.Body", IdOf<TestAsset>(), weak)
              .Error() == ErrorCode::TypeMismatch);
    CHECK(ResolveHandle(resolver, env.allocator, "/Game/Leaf", {}, weak).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(scene.set.Find(FixedId(LeafId)) == nullptr);
    resolver.SetWeakScope(FixedId(LeafId));
    CHECK(ResolveHandle(resolver, env.allocator, "/Game/Leaf", {}, weak).Error() ==
          ErrorCode::NotFound);
    CHECK(scene.set.Find(FixedId(LeafId)) == nullptr);
}

// 호출자의 TypeRegistry가 닫힌 뒤 기대 타입이 있는 Hard 해석은 대상 타입 조회의 ShuttingDown이다
// (리뷰 필수 — A2c2b 저장의 같은 경로와 같다).
TEST(AssetTextResolver_TypeRegistryClosed) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    ResolverScene scene(env, sources);
    REQUIRE(scene.ready);
    AssetTextResolver resolver(env.allocator, scene.set, scene.project.index);
    REQUIRE(ResolveHandle(resolver, env.allocator, "/Game/Leaf"));
    (void)env.types.Shutdown();
    CHECK(ResolveHandle(resolver, env.allocator, "/Game/Leaf", IdOf<TestAsset>()).Error() ==
          ErrorCode::ShuttingDown);
}

// 형식 거부(InvalidArgument): 앞 `/` 없는 이름, 대문자·35자·37자·null AssetId, 논리 경로 규칙
// 위반(빈 구성 요소·끝 `/`·`\`), 빈 객체 경로(`/Game/Hero.`), 객체 경로 구성 요소 규칙(빈 구성
// 요소·공백). 색인에 없는 경로·id는 NotFound(Hard는 열기의 NotFound), 같은 경로 키 둘은
// AlreadyExists, 같은 id 둘의 열기는 열기의 오류다. 거부는 아무것도 열지 않는다.
TEST(AssetTextResolver_RejectsText) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    ResolverScene scene(env, sources, true);
    REQUIRE(scene.ready);
    AssetTextResolver resolver(env.allocator, scene.set, scene.project.index);
    const String leafId = IdText(env.allocator, LeafId);
    char upperBytes[AssetId::TextBytes];
    for (usize index = 0; index < AssetId::TextBytes; ++index) {
        const char c = leafId.Data()[index];
        upperBytes[index] = c >= 'a' && c <= 'f' ? static_cast<char>(c - 'a' + 'A') : c;
    }

    const StringView upper(upperBytes, AssetId::TextBytes);

    String shortId(env.allocator);
    REQUIRE(shortId.TryAssign(StringView(leafId.Data(), leafId.ByteCount() - 1)));
    String longId(env.allocator);
    REQUIRE(longId.TryAssign(leafId.View()) && longId.TryAppend("0"));
    const StringView invalid[] = {"Game/Leaf",
                                  "Leaf",
                                  upper,
                                  shortId.View(),
                                  longId.View(),
                                  "00000000-0000-0000-0000-000000000000",
                                  "/Game//Leaf",
                                  "/Game/Leaf/",
                                  "/Game\\Leaf",
                                  "/Game/Hero.",
                                  "/Game/Hero..Body",
                                  "/Game/Hero.Bad Name"};
    const TextReferenceKind kinds[] = {TextReferenceKind::Hard, TextReferenceKind::Weak,
                                       TextReferenceKind::Soft};
    for (TextReferenceKind kind : kinds) {
        for (StringView text : invalid) {
            FieldValue value(env.allocator);
            CHECK(resolver.Resolve(text, TypeId{}, kind, value).Error() ==
                  ErrorCode::InvalidArgument);
        }

        FieldValue value(env.allocator);
        CHECK(resolver.Resolve("/Game/Nope", TypeId{}, kind, value).Error() == ErrorCode::NotFound);
        CHECK(resolver.Resolve("/Game/TWIN", TypeId{}, kind, value).Error() ==
              ErrorCode::AlreadyExists);
    }

    CHECK(scene.set.Count() == 0);
    const String nowhere = IdText(env.allocator, NowhereId);
    CHECK(ResolveHandle(resolver, env.allocator, nowhere.View()).Error() == ErrorCode::NotFound);
    CHECK(ResolveHandle(resolver, env.allocator, leafId.View()).Error() ==
          ErrorCode::AlreadyExists);
    CHECK(scene.set.Count() == 0);
}

// Soft 해석은 열지 않는다: 논리 경로는 색인(또는 저장하지 않은 새 패키지)의 id와 객체 경로를 주고,
// AssetId 텍스트는 색인에 없어도 그대로다(SoftOnly). 객체 경로는 그대로(대소문자 포함) 둔다.
TEST(AssetTextResolver_ResolvesSoftWithoutOpening) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    ResolverScene scene(env, sources);
    REQUIRE(scene.ready);
    AssetTextResolver resolver(env.allocator, scene.set, scene.project.index);
    FieldValue value(env.allocator);
    REQUIRE(resolver.Resolve("/Game/Hero", TypeId{}, TextReferenceKind::Soft, value));
    CHECK(AssetId{value.guid} == FixedId(HeroId) && value.string.IsEmpty());
    REQUIRE(resolver.Resolve("/Game/Hero.Body.Socket", TypeId{}, TextReferenceKind::Soft, value));
    CHECK(AssetId{value.guid} == FixedId(HeroId) && value.string.View() == "Body.Socket");
    const String soft = IdText(env.allocator, SoftOnlyId);
    String softPart(env.allocator);
    REQUIRE(softPart.TryAssign(soft.View()) && softPart.TryAppend(".Part"));
    REQUIRE(resolver.Resolve(softPart.View(), TypeId{}, TextReferenceKind::Soft, value));
    CHECK(AssetId{value.guid} == FixedId(SoftOnlyId) && value.string.View() == "Part");
    CHECK(scene.set.Count() == 0);

    auto path = ParseLogicalPath(env.allocator, "/Game/Fresh/Soft");
    REQUIRE(path);
    auto made = scene.set.Create(path.Value(), TestAssetType());
    REQUIRE(made);
    REQUIRE(resolver.Resolve("/Game/Fresh/Soft", TypeId{}, TextReferenceKind::Soft, value));
    CHECK(AssetId{value.guid} == AssetOf(scene.set, made.Value())->Id());
}

// 출력: Hard·Weak는 소속 패키지의 경로(열린 색인 패키지는 색인 경로, 저장하지 않은 새 패키지는 그
// 경로)와 객체 경로, Soft는 id의 경로(색인에 없으면 36자 AssetId, 같은 id가 둘이어도 AssetId)와
// 객체 경로다. Package 객체 자신은 Unsupported, 사라진 handle은 Registry 조회의 오류다.
TEST(AssetTextResolver_Formats) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    ResolverScene scene(env, sources, true);
    REQUIRE(scene.ready);
    REQUIRE(scene.AddParts());
    AssetTextResolver resolver(env.allocator, scene.set, scene.project.index);
    EditorPackageSet& set = scene.set;
    for (TextReferenceKind kind : OpenKinds) {
        FieldValue value(env.allocator);
        value.reference = scene.hero;
        CHECK(Formats(resolver, env.allocator, kind, value, "/Game/Hero"));
        value.reference = scene.body;
        CHECK(Formats(resolver, env.allocator, kind, value, "/Game/Hero.Body"));
        value.reference = scene.socket;
        CHECK(Formats(resolver, env.allocator, kind, value, "/Game/Hero.Body.Socket"));
        value.reference = set.Find(FixedId(HeroId))->package;
        StringBuilder<SmallBuilderBytes> out(env.allocator);
        CHECK(resolver.Format(kind, value, out).Error() == ErrorCode::Unsupported);
    }

    auto path = ParseLogicalPath(env.allocator, "/Game/Fresh/Shown");
    REQUIRE(path);
    auto made = set.Create(path.Value(), TestAssetType());
    REQUIRE(made);
    FieldValue fresh(env.allocator);
    fresh.reference = set.Registry().FindChild(set.Access(), made.Value(), "").Value();
    CHECK(Formats(resolver, env.allocator, TextReferenceKind::Hard, fresh, "/Game/Fresh/Shown"));

    FieldValue soft(env.allocator);
    soft.guid = FixedId(BaseId).value;
    CHECK(Formats(resolver, env.allocator, TextReferenceKind::Soft, soft, "/Game/Base"));
    REQUIRE(soft.string.TryAssign("Body.Socket"));
    CHECK(
        Formats(resolver, env.allocator, TextReferenceKind::Soft, soft, "/Game/Base.Body.Socket"));
    soft.guid = AssetOf(set, made.Value())->Id().value;
    CHECK(Formats(resolver, env.allocator, TextReferenceKind::Soft, soft,
                  "/Game/Fresh/Shown.Body.Socket"));
    const String softOnly = IdText(env.allocator, SoftOnlyId);
    soft.guid = FixedId(SoftOnlyId).value;
    soft.string.Clear();
    CHECK(Formats(resolver, env.allocator, TextReferenceKind::Soft, soft, softOnly.View()));
    const String leafId = IdText(env.allocator, LeafId);
    soft.guid = FixedId(LeafId).value;
    CHECK(Formats(resolver, env.allocator, TextReferenceKind::Soft, soft, leafId.View()));

    // 사라진 handle(Discard한 하위 객체).
    REQUIRE(set.Registry().Discard(set.Access(), scene.socket));
    REQUIRE(set.Registry().Collect(set.Access()));
    FieldValue stale(env.allocator);
    stale.reference = scene.socket;
    StringBuilder<SmallBuilderBytes> out(env.allocator);
    CHECK(!resolver.Format(TextReferenceKind::Hard, stale, out));
}

// 대입 묶음·줄 출력과의 왕복(asset-set·asset-info): Villain에 Hard(`hard`)·typed·ObjectRefArray·
// Soft(하위 객체 경로)를 참조 텍스트로 대입하면 대상 패키지가 열리고, 줄 출력은 같은 텍스트를 쓰며
// 그 줄을 다시 넣으면 받는다. 기대 타입이 맞지 않는 대입(typed ← Base)은 진단 Unresolved다.
TEST(AssetTextResolver_RoundTripsThroughAssignments) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    ResolverScene scene(env, sources);
    REQUIRE(scene.ready);
    REQUIRE(scene.AddParts());
    EditorPackageSet& set = scene.set;
    REQUIRE(set.Open(FixedId(VillainId)));
    AssetTextResolver resolver(env.allocator, set, scene.project.index);
    TextCodecOptions options;
    options.resolver = &resolver;
    options.names = &env.names;
    TextDiagnostics diagnostics(env.allocator);
    const ObjectHandle villain = RepresentativeOf(set, VillainId);
    const AssignmentTarget target = AssignmentTarget::Live(set.Registry(), set.Access(), villain);
    TextAssignment inputs[4];
    const StringView texts[] = {"hard=/Game/Leaf", "typed=/Game/Hero",
                                "hardList=/Game/Base,/Game/Hero.Body",
                                "soft=/Game/Hero.Body.Socket"};
    for (usize index = 0; index < 4; ++index) {
        REQUIRE(reflection_text::MakeTextAssignment(texts[index], static_cast<uint32>(index),
                                                    inputs[index]));
    }

    auto prepared = reflection_text::PrepareAssignments(target, inputs, options, diagnostics);
    REQUIRE(prepared);
    REQUIRE(prepared.Value().Publish());
    CHECK(set.Find(FixedId(LeafId)) != nullptr);
    String lines(env.allocator);
    REQUIRE(reflection_text::ExportLines(target, lines, options));
    const StringView expected[] = {"hard=/Game/Leaf\n", "typed=/Game/Hero\n",
                                   "hardList=/Game/Base,/Game/Hero.Body\n",
                                   "soft=/Game/Hero.Body.Socket\n"};
    for (StringView line : expected) {
        CHECK(lines.View().Contains(line, StringComparison::CaseSensitive));
    }

    auto again = reflection_text::PrepareAssignments(target, inputs, options, diagnostics);
    CHECK(again && again.Value().Publish());

    TextAssignment wrong;
    REQUIRE(reflection_text::MakeTextAssignment("typed=/Game/Base", 0, wrong));
    diagnostics.Clear();
    auto rejected = reflection_text::PrepareAssignments(
        target, Span<const TextAssignment>(&wrong, 1), options, diagnostics);
    CHECK(!rejected && diagnostics.Count() == 1 &&
          diagnostics.Get(0).code == reflection_text::TextDiagnosticCode::Unresolved);
}

// 할당 실패: resolver allocator(경로 해석·객체 경로 읽기)·값 문자열·출력 버퍼의 할당을 하나씩
// 실패시키면 OutOfMemory이고(열기의 할당은 집합 allocator라 여기 밖이다), 실패가 없어지면 같은
// 결과다. 해석 모양: 논리 경로 Hard(열린 패키지)·저장하지 않은 새 패키지 경로·Soft 객체 경로,
// 출력 모양: 하위 객체 경로·Soft 객체 경로·긴 이름 하위 객체.
TEST(AssetTextResolver_AllocationFailures) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    ResolverScene scene(env, sources);
    REQUIRE(scene.ready);
    REQUIRE(scene.AddParts());
    auto path = ParseLogicalPath(env.allocator, "/Game/Fresh/Failing");
    REQUIRE(path && scene.set.Create(path.Value(), TestAssetType()));
    test::NFailAllocator failing(env.allocator);
    AssetTextResolver resolver(failing, scene.set, scene.project.index);
    struct Shape {
        StringView text;
        TextReferenceKind kind;
    };
    const Shape shapes[] = {{"/Game/Hero.Body.Socket", TextReferenceKind::Hard},
                            {"/Game/Fresh/Failing", TextReferenceKind::Hard},
                            {"/Game/Hero.Body.Socket", TextReferenceKind::Soft}};
    for (const Shape& shape : shapes) {
        uint32 failures = 0;
        for (uint64 attempt = 1;; ++attempt) {
            FieldValue value(failing);
            failing.FailOnAttempt(attempt);
            auto resolved = resolver.Resolve(shape.text, TypeId{}, shape.kind, value);
            const bool triggered = failing.Attempts() >= attempt;
            failing.DisableFailure();
            if (!triggered) {
                REQUIRE(resolved);
                break;
            }

            CHECK(!resolved && resolved.Error() == ErrorCode::OutOfMemory);
            ++failures;
        }

        CHECK(failures > 0);
    }

    // 긴 이름 하위 객체(객체 경로 읽기가 버퍼를 늘린다).
    auto madeLong = scene.set.Registry().NewObject<TestPart>(scene.set.Access(), TestPartType(),
                                                             scene.hero, LongPartName);
    REQUIRE(madeLong);
    FieldValue part(env.allocator);
    part.reference = scene.socket;
    FieldValue longPart(env.allocator);
    longPart.reference = madeLong.Value().Handle();
    FieldValue soft(env.allocator);
    soft.guid = FixedId(HeroId).value;
    REQUIRE(soft.string.TryAssign("Body.Socket"));
    String longText(env.allocator);
    REQUIRE(longText.TryAssign("/Game/Hero.") && longText.TryAppend(LongPartName));
    const FieldValue* const values[] = {&part, &soft, &longPart};
    const TextReferenceKind kinds[] = {TextReferenceKind::Hard, TextReferenceKind::Soft,
                                       TextReferenceKind::Weak};
    const StringView texts[] = {"/Game/Hero.Body.Socket", "/Game/Hero.Body.Socket",
                                longText.View()};
    for (usize index = 0; index < 3; ++index) {
        uint32 failures = 0;
        for (uint64 attempt = 1;; ++attempt) {
            StringBuilder<SmallBuilderBytes> out(failing);
            failing.FailOnAttempt(attempt);
            auto formatted = resolver.Format(kinds[index], *values[index], out);
            const bool triggered = failing.Attempts() >= attempt;
            failing.DisableFailure();
            if (!triggered) {
                REQUIRE(formatted);
                CHECK(out.View() == texts[index]);
                break;
            }

            CHECK(!formatted && formatted.Error() == ErrorCode::OutOfMemory);
            ++failures;
        }

        CHECK(failures > 0);
    }
}

} // namespace asset_text_resolver_tests_file
