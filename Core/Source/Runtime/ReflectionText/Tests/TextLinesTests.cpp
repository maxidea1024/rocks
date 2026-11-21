#include "TextAssignTestSupport.h"

#include <Core/Text/NamePool.h>
#include <Object/RuntimeTypeOps.h>
#include <ReflectionTestFixtures/BytesFixture.generated.h>
#include <ReflectionTestFixtures/InstancedStructFixture.generated.h>
#include <ReflectionTestFixtures/ReflectionTestFixtures.module.generated.h>
#include <ReflectionTestFixtures/StaticArrayFixture.generated.h>
#include <ReflectionTestFixtures/TextFieldFixture.generated.h>
#include <ReflectionTestFixtures/TextFixture.generated.h>
#include <ReflectionText/TextJson.h>
#include <ReflectionText/TextLines.h>

// `<경로>=<값>` 줄 출력 시험이다(Docs/Design/ReflectionExtensions.ko.md 10.4절 "asset-info 출력",
// Docs/Design/AssetSystem.ko.md 7.1절 15 A2c3a): 모든 종류의 줄 표기와 값 보존 왕복(줄을 대입
// 묶음으로 새 staged 객체·같은 객체에 넣으면 같은 줄·무동작), Optional unset의 `--unset=` 줄,
// struct·struct 원소·struct 값 Map·InstancedStruct·정적 배열·바이트 배열·Text의 줄, 줄로 쓸 수
// 없는 값(줄바꿈·Localized Text·resolver가 쓰지 못한 참조)의 Unsupported, 용도별 제외, 대상 종류,
// 옵션 오류, 모든 할당 실패.
using namespace rocks;
namespace text_lines_tests_file {

using namespace rocks::reflection_text;
using namespace rocks::test::assign;
using reflection_fixture::BytesHolder;
using reflection_fixture::FixedOwner;
using reflection_fixture::InstancedHolder;
using reflection_fixture::TextFieldHolder;
using reflection_fixture::TextHolder;
using reflection_fixture::TextPoint;

namespace {

constexpr StringView UnsetPrefix = "--unset=";
constexpr TextId LineCaptionId{0x4c49, 3};
// TextQuality에 이름이 없는 값이다.
constexpr uint8 UnnamedQualityValue = 99;

// fixture 모듈 객체 Registry(Name pool, 할당 실패 주입 allocator)다.
struct LineObjects {
    LineObjects() noexcept
        : failing(system), types(system), registration(types, system), names(system),
          objects(failing, types, {0x4c49, 1}, MakeOptions(names)), access(objects.OwnerAccess()),
          diagnostics(system) {
        REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(registration));
        REQUIRE(names.Initialize());
    }

    ~LineObjects() noexcept {
        failing.failAt = 0;
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
        CHECK(registration.Unregister());
        CHECK(names.Shutdown());
        CHECK(types.Shutdown());
    }

    static RegistryOptions MakeOptions(NamePool& pool) noexcept {
        RegistryOptions options;
        options.names = &pool;
        return options;
    }

    template <class T>
    RootRef<T> Make() noexcept {
        auto created = objects.NewObject<T>(access, GeneratedType<T>::Schema().id);
        CHECK(created);
        return created ? Move(created.Value()) : RootRef<T>{};
    }

    template <class T, class Edit>
    void With(ObjectHandle object, Edit&& edit) noexcept {
        auto borrowed = objects.Resolve<T>(access, object);
        REQUIRE(borrowed);
        auto value = borrowed.Value().Get();
        REQUIRE(value);
        edit(*value.Value());
    }

    // 대입 묶음을 준비·게시한다.
    bool Apply(const AssignmentTarget& target, Span<const TextAssignment> inputs) noexcept {
        diagnostics.Clear();
        auto prepared = PrepareAssignments(target, inputs, options, diagnostics);
        if (!prepared) {
            return false;
        }

        return static_cast<bool>(prepared.Value().Publish());
    }

    // 줄 텍스트를 대입 묶음으로 넣는다(`--unset=` 줄은 Unset 대입).
    bool ApplyLines(const AssignmentTarget& target, StringView lines) noexcept {
        Array<TextAssignment> inputs(system);
        usize start = 0;
        for (usize index = 0; index < lines.ByteCount(); ++index) {
            if (lines.Data()[index] != '\n') {
                continue;
            }

            const StringView line(lines.Data() + start, index - start);
            start = index + 1;
            TextAssignment assignment;
            if (line.StartsWith(UnsetPrefix, StringComparison::CaseSensitive)) {
                assignment.kind = AssignmentKind::Unset;
                assignment.path = StringView(line.Data() + UnsetPrefix.ByteCount(),
                                             line.ByteCount() - UnsetPrefix.ByteCount());
                assignment.sourceIndex = static_cast<uint32>(inputs.Size());
            } else if (!MakeTextAssignment(line, static_cast<uint32>(inputs.Size()), assignment)) {
                return false;
            }

            if (!inputs.TryPushBack(assignment)) {
                return false;
            }
        }

        CHECK(start == lines.ByteCount());
        return Apply(target, inputs.View());
    }

    Result<void> Lines(const AssignmentTarget& target, String& out) noexcept {
        out.Clear();
        return ExportLines(target, out, options);
    }

    SystemAllocator system;
    FailingAllocator failing;
    TypeRegistry types;
    ModuleRegistration registration;
    NamePool names;
    ObjectRegistry objects;
    ObjectAccessContext access;
    TextDiagnostics diagnostics;
    TextCodecOptions options;
};

bool Has(const String& lines, StringView expected) noexcept {
    return lines.View().Contains(expected, StringComparison::CaseSensitive);
}

// 값을 만든 live 객체의 줄을 새 staged 객체에 넣어 다시 쓰면 같은 줄이고, 같은 live 객체에 다시
// 넣으면 받는다(값 보존 왕복).
template <class T>
void CheckRoundTrip(LineObjects& fixture, ObjectHandle object, const String& lines) noexcept {
    const AssignmentTarget live = AssignmentTarget::Live(fixture.objects, fixture.access, object);
    CHECK(fixture.ApplyLines(live, lines.View()));
    ConstructionContext transaction(fixture.objects, fixture.access);
    auto created = transaction.Create<T>(GeneratedType<T>::Schema().id);
    REQUIRE(created);
    const AssignmentTarget staged = AssignmentTarget::Staged(transaction, created.Value());
    CHECK(fixture.ApplyLines(staged, lines.View()));
    String again(fixture.system);
    REQUIRE(fixture.Lines(staged, again));
    CHECK(again.View() == lines.View());
    transaction.Rollback();
}

struct Scene {
    const char* json;
    int kind;
};

// 종류마다 값을 채운 장면이다(JSON import로 만든다).
constexpr Scene Scenes[] = {
    {R"({"value":{"_structType":"InstancedPoint","x":3,"label":"hi"},"spare":null,
         "list":[{"_structType":"InstancedScale","factor":2,"steps":[1,2]},null],
         "fixed":[{"_structType":"InstancedPoint","y":4},null],
         "table":{"5":{"_structType":"InstancedPoint","x":1},"6":null},
         "host":{"value":{"_structType":"InstancedScale"},"count":2},
         "hosts":[{"value":{"_structType":"InstancedPoint","x":8},"count":1}],
         "slots":[{"value":null,"count":3},{"value":{"_structType":"InstancedPoint"},"count":0}]})",
     0},
    {R"({"points":[{"pair":[1,2]},{"pair":[3,4]}],"labels":["a","b,c"],"ints":[1,2,3,4],
         "modes":["Second","First"]})",
     1},
    {R"({"payload":"AAECAw==","small":"","blob":{"data":"/w==","tag":2},
         "blobs":[{"data":"AQ==","tag":1},{"data":"","tag":0}],"fixed":[1,2,3,4],
         "maybe":{"data":"Ag==","tag":5},"byKey":{"7":{"data":"Bw==","tag":7}}})",
     2},
    {R"({"title":"plain","note":null,"lines":["a","b"],"byKey":{"1":"one"}})", 3}};

// 장면의 객체(Root가 잡는다)를 만들고 JSON을 import한다.
struct SceneObject {
    RootRef<InstancedHolder> instanced;
    RootRef<FixedOwner> fixed;
    RootRef<BytesHolder> bytes;
    RootRef<TextFieldHolder> text;
    ObjectHandle handle{};
};

void MakeScene(LineObjects& fixture, const Scene& scene, SceneObject& out) noexcept {
    if (scene.kind == 0) {
        out.instanced = fixture.Make<InstancedHolder>();
        out.handle = out.instanced.Handle();
    } else if (scene.kind == 1) {
        out.fixed = fixture.Make<FixedOwner>();
        out.handle = out.fixed.Handle();
    } else if (scene.kind == 2) {
        out.bytes = fixture.Make<BytesHolder>();
        out.handle = out.bytes.Handle();
    } else {
        out.text = fixture.Make<TextFieldHolder>();
        out.handle = out.text.Handle();
    }

    JsonDocument document(fixture.system);
    REQUIRE(document.Parse(scene.json).Succeeded());
    fixture.diagnostics.Clear();
    auto imported = ImportJson(document.Root(),
                               AssignmentTarget::Live(fixture.objects, fixture.access, out.handle),
                               fixture.options, fixture.diagnostics);
    REQUIRE(imported);
    REQUIRE(imported.Value().Publish());
}

} // namespace

// 모든 단일 값 종류·목록·Map·struct·struct 원소·struct 값 Map·Optional의 줄(10.4절 표): 스칼라
// `f=v`, Flags `A+B`, 목록 쉼표, Map `k:v`, struct 멤버 `f.m=v`, struct 원소 배열
// `f=`·`f[i]={}`·멤버, struct 값 Map `f=`·`f[k]={}`·멤버, Optional set `f=v`·`f={}`+멤버, unset
// `--unset=f`, null 참조 `none`. Edit 출력은 Transient를 빼고 ReadOnly는 쓴다. 줄을 staged 객체에
// 넣어 다시 쓰면 같고 같은 객체에 다시 넣으면 ReadOnly도 같은 값이라 받는다. 기본 객체(Optional
// unset)도 왕복한다.
TEST(TextLines_RoundTripsEveryKind) {
    LineObjects fixture;
    auto root = fixture.Make<TextHolder>();
    const ObjectHandle object = root.Handle();
    const TextAssignment inputs[] = {
        Set("enabled", "true"),
        Set("small", "200", 1),
        Set("count", "4000000000", 2),
        Set("big", "18446744073709551615", 3),
        Set("wide", "-9223372036854775808", 4),
        Set("ratio", "0.1", 5),
        Set("precise", "nan", 6),
        Set("id", "00112233-4455-6677-8899-aabbccddeeff", 7),
        Set("asset", "10112233-4455-6677-8899-aabbccddeeff", 8),
        Set("digest", "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff", 9),
        Set("label", "a,b=c", 10),
        Set("tag", "Hero", 11),
        Set("quality", "High", 12),
        Set("channels", "Red+Blue", 13),
        Set("source", "20112233-4455-6677-8899-aabbccddeeff", 14),
        Set("kind", "TextKindDerived", 15),
        Set("numbers", "3,1,2", 16),
        Set("words", "x\\,y,z", 17),
        Set("points[0]", "{}", 18),
        Set("points[0].x", "4", 19),
        Set("points[1]", "{}", 20),
        Set("triple", "1,2,3", 21),
        Set("tags", "b,a", 22),
        Set("scores", "z:1,a\\:b:2", 23),
        Set("byQuality", "High:h,Low:l", 24),
        Set("byName[k]", "{}", 25),
        Set("byName[k].y", "6", 26),
        Set("byClass", "TextKindDerived:1", 27),
        Set("bySoft", "30112233-4455-6677-8899-aabbccddeeff:5", 28),
        Set("limit", "9", 29),
        Set("anchor", "{}", 30),
        Set("anchor.x", "7", 31),
        Set("origin.y", "-1", 32)};
    REQUIRE(fixture.Apply(AssignmentTarget::Live(fixture.objects, fixture.access, object), inputs));
    fixture.With<TextHolder>(object, [](TextHolder& value) noexcept {
        value.locked = 3;
        value.cache = 5;
    });
    String lines(fixture.system);
    REQUIRE(fixture.Lines(AssignmentTarget::Live(fixture.objects, fixture.access, object), lines));
    const StringView expected[] = {
        "enabled=true\n",
        "big=18446744073709551615\n",
        "ratio=0.1\n",
        "precise=nan\n",
        "label=a,b=c\n",
        "tag=Hero\n",
        "quality=High\n",
        "channels=Red+Blue\n",
        "kind=TextKindDerived\n",
        "numbers=3,1,2\n",
        "words=x\\,y,z\n",
        "points=\npoints[0]={}\npoints[0].x=4\npoints[0].y=0\npoints[1]={}\n",
        "triple=1,2,3\n",
        "tags=b,a\n",
        "scores=z:1,a\\:b:2\n",
        "byQuality=High:h,Low:l\n",
        "byName=\nbyName[k]={}\nbyName[k].x=0\nbyName[k].y=6\n",
        "byClass=TextKindDerived:1\n",
        "limit=9\n",
        "anchor={}\nanchor.x=7\nanchor.y=0\n",
        "origin.x=0\norigin.y=-1\n",
        "next=none\n",
        "peer=none\n",
        "locked=3\n"};
    for (StringView line : expected) {
        CHECK(Has(lines, line));
    }

    CHECK(!Has(lines, "cache="));
    CheckRoundTrip<TextHolder>(fixture, object, lines);

    // 기본 객체: Optional은 unset 줄이다.
    auto empty = fixture.Make<TextHolder>();
    String defaults(fixture.system);
    REQUIRE(fixture.Lines(AssignmentTarget::Live(fixture.objects, fixture.access, empty.Handle()),
                          defaults));
    CHECK(Has(defaults, "--unset=limit\n") && Has(defaults, "--unset=anchor\n"));
    CHECK(Has(defaults, "points=\n") && Has(defaults, "byName=\n") && Has(defaults, "words=\n"));
    CheckRoundTrip<TextHolder>(fixture, empty.Handle(), defaults);
}

// InstancedStruct(필드·배열 원소·정적 배열 원소·Map 값·struct 멤버·struct 원소 멤버, 비어 있음
// `none`), struct 원소 정적 배열(원소마다 멤버 줄, 비우기 줄 없음), 바이트 배열(Base64 — 빈 배열
// 포함, struct 안·struct 원소·Optional·struct 값 Map), Invariant Text·빈 Text의 줄도 값 보존
// 왕복이다(값은 JSON import로 만든다).
TEST(TextLines_InstancedStaticBytesAndText) {
    LineObjects fixture;
    for (const Scene& scene : Scenes) {
        SceneObject made;
        MakeScene(fixture, scene, made);
        const ObjectHandle object = made.handle;
        const AssignmentTarget live =
            AssignmentTarget::Live(fixture.objects, fixture.access, object);
        String lines(fixture.system);
        REQUIRE(fixture.Lines(live, lines));
        if (scene.kind == 0) {
            CHECK(Has(lines, "value=InstancedPoint\n") && Has(lines, "value.x=3\n") &&
                  Has(lines, "spare=none\n"));
            CHECK(Has(lines, "list=\nlist[0]=InstancedScale\n") &&
                  Has(lines, "list[0].factor=2\n"));
            CHECK(Has(lines, "list[1]=none\n") && Has(lines, "table[6]=none\n"));
            CHECK(Has(lines, "hosts=\nhosts[0]={}\nhosts[0].value=InstancedPoint\n"));
            CHECK(Has(lines, "hosts[0].value.x=8\n") && Has(lines, "slots[0].value=none\n"));
            CheckRoundTrip<InstancedHolder>(fixture, object, lines);
        } else if (scene.kind == 1) {
            CHECK(Has(lines, "points[0].pair=1,2\n") && Has(lines, "points[1].pair=3,4\n"));
            CHECK(!Has(lines, "points=\n") && Has(lines, "labels=a,b\\,c\n"));
            CHECK(Has(lines, "refs[0]=none\nrefs[1]=none\nrefs[2]=none\n") &&
                  !Has(lines, "refs=\n"));
            CheckRoundTrip<FixedOwner>(fixture, object, lines);
        } else if (scene.kind == 2) {
            CHECK(Has(lines, "payload=AAECAw==\n") && Has(lines, "small=\n"));
            CHECK(Has(lines, "blobs=\nblobs[0]={}\n") && Has(lines, "blobs[0].data=AQ==\n"));
            CHECK(Has(lines, "maybe={}\n") && Has(lines, "maybe.data=Ag==\n"));
            CHECK(Has(lines, "fixed=1,2,3,4\n"));
            CheckRoundTrip<BytesHolder>(fixture, object, lines);
        } else {
            CHECK(Has(lines, "title=plain\n") && Has(lines, "lines=a,b\n"));
            CheckRoundTrip<TextFieldHolder>(fixture, object, lines);
        }
    }
}

namespace {

// 논리 경로 `/t`를 target으로 풀고 target을 `/t`로 쓰는 resolver다(그 밖의 handle은 쓰지 못한다).
class LineResolver final : public TextReferenceResolver {
public:
    Result<void> Resolve(StringView text, TypeId, TextReferenceKind,
                         FieldValue& out) noexcept override {
        if (text != "/t") {
            return Result<void>::Failure(ErrorCode::NotFound);
        }

        out.reference = target;
        return Result<void>::Success();
    }

    Result<void> Format(TextReferenceKind, const FieldValue& value,
                        StringBuilderBase& out) noexcept override {
        if (!(value.reference == target)) {
            return Result<void>::Failure(ErrorCode::NotFound);
        }

        return out.TryAppend("/t");
    }

    ObjectHandle target{};
};

} // namespace

// 줄로 쓸 수 없는 값(10.4절): 이름 없는 enum 값 Unsupported(숨김 열거자는 이름으로 쓰고 그 줄의
// 재입력은 HiddenEnumerator — 10.1절), 줄바꿈이 든 String 값(값 `\n`·`\r`) Unsupported(단일 값 Map
// key의 줄바꿈도 값 목록 줄 안이라 같다 — 경로 key의 줄바꿈은 경로 표기가 `\n`으로 escape한다),
// Localized Text Unsupported, resolver 없이 non-null Hard 참조 Unsupported, resolver가 쓰지 못한
// 참조 Unsupported. resolver가 쓰는 참조는 그 텍스트로 쓰고 왕복한다.
TEST(TextLines_UnsupportedValuesAndReferences) {
    LineObjects fixture;
    auto root = fixture.Make<TextHolder>();
    const ObjectHandle object = root.Handle();
    const AssignmentTarget live = AssignmentTarget::Live(fixture.objects, fixture.access, object);
    String lines(fixture.system);
    const char* const breaks[] = {"a\nb", "a\rb"};
    for (const char* value : breaks) {
        fixture.With<TextHolder>(
            object, [&](TextHolder& holder) noexcept { REQUIRE(holder.label.TryAssign(value)); });
        CHECK(fixture.Lines(live, lines).Error() == ErrorCode::Unsupported);
    }

    fixture.With<TextHolder>(
        object, [&](TextHolder& holder) noexcept { REQUIRE(holder.label.TryAssign("ok")); });
    const TextAssignment key[] = {Set("scores[\"a\\nb\"]", "1")};
    REQUIRE(fixture.Apply(live, key));
    CHECK(fixture.Lines(live, lines).Error() == ErrorCode::Unsupported);

    // Hard 참조: resolver 없음 · 쓰지 못함은 Unsupported, 쓰면 그 텍스트로 왕복한다.
    auto other = fixture.Make<TextHolder>();
    LineResolver resolver;
    resolver.target = other.Handle();
    fixture.options.resolver = &resolver;
    const TextAssignment link[] = {Set("next", "/t"), Set("scores", "")};
    REQUIRE(fixture.Apply(live, link));
    fixture.options.resolver = nullptr;
    CHECK(fixture.Lines(live, lines).Error() == ErrorCode::Unsupported);
    LineResolver mute;
    fixture.options.resolver = &mute;
    CHECK(fixture.Lines(live, lines).Error() == ErrorCode::Unsupported);
    fixture.options.resolver = &resolver;
    REQUIRE(fixture.Lines(live, lines));
    CHECK(Has(lines, "next=/t\n"));
    CheckRoundTrip<TextHolder>(fixture, object, lines);
    fixture.options.resolver = nullptr;
    REQUIRE(fixture.Apply(live, Span<const TextAssignment>(link + 1, 1)));
    {
        const TextAssignment unlink[] = {Set("next", "none")};
        REQUIRE(fixture.Apply(live, unlink));
    }

    // 이름 없는 enum 값은 Unsupported, 숨김 열거자는 이름으로 쓴다.
    fixture.With<TextHolder>(object, [](TextHolder& holder) noexcept {
        holder.quality = static_cast<reflection_fixture::TextQuality>(UnnamedQualityValue);
    });
    CHECK(fixture.Lines(live, lines).Error() == ErrorCode::Unsupported);
    fixture.With<TextHolder>(object, [](TextHolder& holder) noexcept {
        holder.quality = reflection_fixture::TextQuality::Secret;
    });
    REQUIRE(fixture.Lines(live, lines));
    CHECK(Has(lines, "quality=Secret\n"));
    // 숨김 열거자 재입력은 거부한다(10.1절 — 값 보존 왕복의 예외).
    CHECK(!fixture.ApplyLines(live, lines.View()));
    bool hidden = false;
    for (usize index = 0; index < fixture.diagnostics.Count(); ++index) {
        hidden =
            hidden || fixture.diagnostics.Get(index).code == TextDiagnosticCode::HiddenEnumerator;
    }

    CHECK(hidden);

    // Localized Text.
    auto texts = fixture.Make<TextFieldHolder>();
    fixture.With<TextFieldHolder>(texts.Handle(), [&](TextFieldHolder& holder) noexcept {
        auto localized = Text::Localized(fixture.objects.ObjectAllocator(), LineCaptionId, 1, "C");
        REQUIRE(localized);
        holder.title = Move(localized.Value());
    });
    CHECK(fixture
              .Lines(AssignmentTarget::Live(fixture.objects, fixture.access, texts.Handle()), lines)
              .Error() == ErrorCode::Unsupported);
}

// 용도·대상·옵션(10.1절): Edit은 Transient·NonTryoutDuplicateTransient를 빼고 Report는 Transient만
// 뺀다(TextExportTransient·DuplicateTransient는 둘 다 쓴다). struct 값 대상은 그 멤버 줄, staged
// 대상도 쓴다. 같은 Name pool을 명시하면 기본과 같다. 잘못된 값 대상(저장소 없음·빈 lease·클래스
// lease)·잘못된 옵션·다른 Name pool은
// InvalidArgument, 사라진 대상은 그 handle 오류다.
TEST(TextLines_UsesTargetsAndOptions) {
    LineObjects fixture;
    auto root = fixture.Make<TextHolder>();
    const ObjectHandle object = root.Handle();
    const AssignmentTarget live = AssignmentTarget::Live(fixture.objects, fixture.access, object);
    String lines(fixture.system);
    REQUIRE(fixture.Lines(live, lines));
    CHECK(!Has(lines, "cache=") && !Has(lines, "instance=") && Has(lines, "exportOnly=") &&
          Has(lines, "copied.x="));
    fixture.options.use = TextUse::Report;
    REQUIRE(fixture.Lines(live, lines));
    CHECK(!Has(lines, "cache=") && Has(lines, "instance=") && Has(lines, "exportOnly="));
    fixture.options.use = TextUse::Edit;

    // struct 값 대상.
    TextPoint point;
    point.x = 2;
    point.y = 5;
    auto lease = fixture.types.AcquireValueType(GeneratedType<TextPoint>::Id(), TypeKind::Struct);
    REQUIRE(lease);
    REQUIRE(fixture.Lines(AssignmentTarget::Value(fixture.objects, fixture.access, lease.Value(),
                                                  &point, fixture.system),
                          lines));
    CHECK(lines.View() == "x=2\ny=5\n");
    CHECK(fixture
              .Lines(AssignmentTarget::Value(fixture.objects, fixture.access, lease.Value(),
                                             nullptr, fixture.system),
                     lines)
              .Error() == ErrorCode::InvalidArgument);
    const TypeLease empty;
    CHECK(fixture
              .Lines(AssignmentTarget::Value(fixture.objects, fixture.access, empty, &point,
                                             fixture.system),
                     lines)
              .Error() == ErrorCode::InvalidArgument);
    auto classLease = fixture.objects.AcquireType(fixture.access, object);
    REQUIRE(classLease);
    CHECK(fixture
              .Lines(AssignmentTarget::Value(fixture.objects, fixture.access, classLease.Value(),
                                             &point, fixture.system),
                     lines)
              .Error() == ErrorCode::InvalidArgument);

    // 잘못된 옵션(문맥 시작의 옵션 검사).
    fixture.options.maxDiagnostics = 0;
    CHECK(fixture.Lines(live, lines).Error() == ErrorCode::InvalidArgument);
    fixture.options.maxDiagnostics = DefaultMaxTextDiagnostics;

    // staged 대상.
    {
        ConstructionContext transaction(fixture.objects, fixture.access);
        auto created = transaction.Create<TextHolder>(GeneratedType<TextHolder>::Schema().id);
        REQUIRE(created);
        REQUIRE(fixture.Lines(AssignmentTarget::Staged(transaction, created.Value()), lines));
        CHECK(Has(lines, "--unset=limit\n"));
        transaction.Rollback();
    }

    // 대상과 같은 Name pool을 명시하면 기본(null)과 같다.
    String implicitNames(fixture.system);
    REQUIRE(fixture.Lines(live, implicitNames));
    fixture.options.names = &fixture.names;
    REQUIRE(fixture.Lines(live, lines));
    CHECK(lines.View() == implicitNames.View());
    fixture.options.names = nullptr;

    // 다른 Name pool.
    NamePool otherNames(fixture.system);
    REQUIRE(otherNames.Initialize());
    fixture.options.names = &otherNames;
    CHECK(fixture.Lines(live, lines).Error() == ErrorCode::InvalidArgument);
    fixture.options.names = nullptr;
    CHECK(otherNames.Shutdown());

    // 사라진 대상.
    auto doomed = fixture.Make<TextHolder>();
    const ObjectHandle stale = doomed.Handle();
    REQUIRE(fixture.objects.RequestDestroy(fixture.access, stale));
    doomed.Reset();
    REQUIRE(fixture.objects.Collect(fixture.access));
    CHECK(!fixture.Lines(AssignmentTarget::Live(fixture.objects, fixture.access, stale), lines));
}

namespace {

// target의 줄 출력에서 Registry allocator(값·임시 버퍼)와 출력 문자열의 할당을 하나씩 실패시키면
// OutOfMemory이고, 끝나면 실패 없는 출력과 같다.
void SweepAllocations(LineObjects& fixture, const AssignmentTarget& target) noexcept {
    String expected(fixture.system);
    REQUIRE(fixture.Lines(target, expected));
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        String lines(fixture.failing);
        fixture.failing.allocations = 0;
        fixture.failing.failAt = attempt;
        auto written = ExportLines(target, lines, fixture.options);
        const bool failed = fixture.failing.allocations >= attempt;
        fixture.failing.failAt = 0;
        if (!failed) {
            REQUIRE(written);
            CHECK(lines.View() == expected.View());
            break;
        }

        CHECK(!written && written.Error() == ErrorCode::OutOfMemory);
        ++failures;
    }

    CHECK(failures > 0);
}

} // namespace

// 할당 실패: 모든 줄 모양(단일 값·목록·String 값 Map·struct 원소·struct 값 Map·Optional·
// InstancedStruct 필드·원소·Map 값·struct 안·정적 배열·강한 참조 정적 배열·바이트 배열·Text)을 가진
// 객체마다 모든 할당을 하나씩 실패시킨다.
TEST(TextLines_AllocationFailures) {
    LineObjects fixture;
    auto root = fixture.Make<TextHolder>();
    const ObjectHandle object = root.Handle();
    const TextAssignment inputs[] = {Set("words", "aaaaaaaaaaaaaaaaaaaaaaaa,bbbbbbbbbbbbbbbbbbbb"),
                                     Set("points[0]", "{}", 1),
                                     Set("byName[k]", "{}", 2),
                                     Set("scores", "z:1,a:2", 3),
                                     Set("limit", "3", 4),
                                     Set("label", "a long label value that grows the line", 5),
                                     Set("byQuality", "High:a long string value,Low:another", 6),
                                     Set("anchor", "{}", 7)};
    const AssignmentTarget live = AssignmentTarget::Live(fixture.objects, fixture.access, object);
    REQUIRE(fixture.Apply(live, inputs));
    SweepAllocations(fixture, live);
    // 기본 객체: Optional unset 줄(`--unset=` 앞말).
    auto empty = fixture.Make<TextHolder>();
    SweepAllocations(fixture,
                     AssignmentTarget::Live(fixture.objects, fixture.access, empty.Handle()));
    for (const Scene& scene : Scenes) {
        SceneObject made;
        MakeScene(fixture, scene, made);
        SweepAllocations(fixture,
                         AssignmentTarget::Live(fixture.objects, fixture.access, made.handle));
    }
}

namespace {

// 경로 앞부분의 InstancedStruct 담긴 타입을 live 객체에서 읽는 resolver다.
class LiveInstancedTypes final : public PathInstancedTypeResolver {
public:
    LiveInstancedTypes(LineObjects& fixture, ObjectHandle object) noexcept
        : fixture_(fixture), object_(object) {
    }

    Result<void> InstancedType(PropertyPath prefix, TypeLease& out) noexcept override {
        auto type =
            PathReadInstancedType(fixture_.objects, fixture_.access, PathTarget(object_), prefix);
        if (!type) {
            return Result<void>::Failure(type.Error());
        }

        out = Move(type.Value());
        return Result<void>::Success();
    }

private:
    LineObjects& fixture_;
    ObjectHandle object_;
};

} // namespace

// FormatTextPath(10.4절 줄 출력이 쓴다)는 InstancedStruct 뒤 멤버 단계를 문맥의 InstancedTypes()로
// 푼다: 없으면 InvalidArgument, 있으면 `value.x`·`hosts[0].value.x`를 쓴다. 담긴 값이 비어 있으면
// resolver가 빈 lease를 주어 NotFound다.
TEST(TextLines_FormatTextPathUsesContextInstancedTypes) {
    LineObjects fixture;
    auto root = fixture.Make<InstancedHolder>();
    const ObjectHandle object = root.Handle();
    const TextAssignment inputs[] = {Set("value", "InstancedPoint"), Set("hosts[0]", "{}", 1),
                                     Set("hosts[0].value", "InstancedPoint", 2)};
    REQUIRE(fixture.Apply(AssignmentTarget::Live(fixture.objects, fixture.access, object), inputs));
    auto lease = fixture.objects.AcquireType(fixture.access, object);
    REQUIRE(lease);
    TextDiagnostics diagnostics(fixture.system);
    TextCodecContext context(fixture.types, fixture.options, diagnostics, fixture.system);
    LiveInstancedTypes resolver(fixture, object);
    const StringView texts[] = {"value.x", "hosts[0].value.x"};
    for (StringView text : texts) {
        PropertyPathBuffer path;
        REQUIRE(ParsePropertyPath(fixture.objects, fixture.access, PathTarget(object), text, path));
        String out(fixture.system);
        context.SetInstancedTypes(nullptr);
        CHECK(FormatTextPath(context, lease.Value(), path.View(), out).Error() ==
              ErrorCode::InvalidArgument);
        context.SetInstancedTypes(&resolver);
        auto formatted = FormatTextPath(context, lease.Value(), path.View(), out);
        REQUIRE(formatted);
        CHECK(formatted.Value() && out.View() == text);
    }

    PropertyPathBuffer member;
    REQUIRE(
        ParsePropertyPath(fixture.objects, fixture.access, PathTarget(object), "value.x", member));
    fixture.With<InstancedHolder>(object,
                                  [](InstancedHolder& holder) noexcept { holder.value.Reset(); });
    String out(fixture.system);
    CHECK(FormatTextPath(context, lease.Value(), member.View(), out).Error() ==
          ErrorCode::NotFound);
}

namespace {

// 손으로 등록한 클래스의 Opaque 필드(비typed 메타데이터 — 생성 타입에는 없다)다.
struct OpaqueLineHolder final : Object {};

constexpr TypeId OpaqueLineHolderType{0x4c49, 7};
constexpr Guid OpaqueLineFieldId{0x4c49, 8};
constexpr StringView OpaqueLineHolderName = "OpaqueLineHolder";

// 도구 이름 없이 손으로 등록한 struct다(InstancedStruct에 담기면 줄에 쓸 타입 이름이 없다).
struct NamelessLineStruct {
    int32 v = 0;
};

constexpr TypeId NamelessLineStructType{0x4c49, 9};
constexpr StringView NamelessLineStructName = "NamelessLineStruct";

} // namespace

// 줄로 쓸 수 없는 모양: Opaque 필드(JSON 출력과 같다), 도구 이름 없는 struct(수동 등록)를 담은
// InstancedStruct, 경로 단계 상한(MaxPropertyPathSteps)을 넘는
// 자리(C++ Emplace는 위치 깊이를 보지 않으므로 struct 멤버 InstancedStruct에 깊이 16 사슬을 담을 수
// 있다 — 대입·decode는 거부한다)는 Unsupported다.
TEST(TextLines_OpaqueAndTooDeepAreUnsupported) {
    LineObjects fixture;
    String lines(fixture.system);
    {
        auto root = fixture.Make<InstancedHolder>();
        fixture.With<InstancedHolder>(root.Handle(), [&](InstancedHolder& holder) noexcept {
            REQUIRE(holder.host.value.Emplace<reflection_fixture::InstancedDepth16>(
                fixture.types, fixture.objects.ObjectAllocator()));
        });
        CHECK(fixture
                  .Lines(AssignmentTarget::Live(fixture.objects, fixture.access, root.Handle()),
                         lines)
                  .Error() == ErrorCode::Unsupported);
        fixture.With<InstancedHolder>(root.Handle(), [&](InstancedHolder& holder) noexcept {
            REQUIRE(holder.host.value.Emplace<reflection_fixture::InstancedDepth15>(
                fixture.types, fixture.objects.ObjectAllocator()));
        });
        REQUIRE(fixture.Lines(
            AssignmentTarget::Live(fixture.objects, fixture.access, root.Handle()), lines));
        CHECK(Has(lines, "host.value=InstancedDepth15\n"));

        // 도구 이름 없는 struct를 담은 InstancedStruct.
        auto nameless = fixture.types.RegisterStruct(
            {NamelessLineStructType, 1, Span<const FieldSchema>(),
             Span<const char>(NamelessLineStructName.Data(), NamelessLineStructName.ByteCount())},
            MakeRuntimeStructOps<NamelessLineStruct>());
        REQUIRE(nameless);
        {
            auto lease = fixture.types.AcquireValueType(NamelessLineStructType, TypeKind::Struct);
            REQUIRE(lease);
            fixture.With<InstancedHolder>(root.Handle(), [&](InstancedHolder& holder) noexcept {
                REQUIRE(
                    holder.value.Emplace(Move(lease.Value()), fixture.objects.ObjectAllocator()));
            });
        }

        CHECK(fixture
                  .Lines(AssignmentTarget::Live(fixture.objects, fixture.access, root.Handle()),
                         lines)
                  .Error() == ErrorCode::Unsupported);
        fixture.With<InstancedHolder>(
            root.Handle(), [](InstancedHolder& holder) noexcept { holder.value.Reset(); });
        CHECK(fixture.types.Unregister(nameless.Value()));
    }

    FieldSchema field;
    field.id = OpaqueLineFieldId;
    field.name = Span<const char>("blob", 4);
    field.value = ValueType{ValueKind::Opaque, {}};
    auto token = fixture.types.Register(
        {OpaqueLineHolderType, 1, Span<const FieldSchema>(&field, 1),
         Span<const char>(OpaqueLineHolderName.Data(), OpaqueLineHolderName.ByteCount())},
        MakeRuntimeTypeOps<OpaqueLineHolder>());
    REQUIRE(token);
    {
        auto created =
            fixture.objects.NewObject<OpaqueLineHolder>(fixture.access, OpaqueLineHolderType);
        REQUIRE(created);
        const ObjectHandle object = created.Value().Handle();
        CHECK(fixture.Lines(AssignmentTarget::Live(fixture.objects, fixture.access, object), lines)
                  .Error() == ErrorCode::Unsupported);
        REQUIRE(fixture.objects.RequestDestroy(fixture.access, object));
    }

    REQUIRE(fixture.objects.Collect(fixture.access));
    CHECK(fixture.types.Unregister(token.Value()));
}

} // namespace text_lines_tests_file
