#include <Core/FileRead.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/Json.h>
#include <Core/Text/Path.h>
#include <Core/Text/String.h>
#include <Core/Text/StringBuilder.h>
#include <HostConfig/HostSettings.h>
#include <Platform/File.h>
#include <Platform/Process.h>
#include <SparksCompiler/LanguageCatalog.h>
#include <SparksTools/SparksCommand.h>
#include <TestSupport/Test.h>
#include "SparksToolsTestSupport.h"

// Sparks 엔진 binding(Docs/Design/SparksBinding.ko.md EI1a)의 C++ 쪽 시험이다. (1) 메타 컴파일러가
// 읽는 이름 표(SparksNames.json)가 컴파일러 목록과 같은지(B6 — 사본이 어긋나지 않게), (2) 빌드가
// 생성한 실제 엔진 schema로 Sparks 코드가 진단 없이 컴파일되는지(B9). 생성 규칙 자체(지정자·타입
// 대응·오류 코드)는 C# meta 시험(MetaTests.SparksBinding*)이 본다.
using namespace rocks;
using namespace rocks::sparks;
using namespace rocks::sparks::tools;
namespace sparks_tools_binding_tests_file {

using namespace rocks::test::sparks_tools;

namespace {

constexpr StringView NamesPath = "Programs/Rocks.MetaCompiler/SparksNames.json";

// 이름 표·schema 파일 읽기 상한이다(지금 수 KB — 엔진 schema는 수십 KB).
constexpr uint64 FileMaxBytes = 16 * 1024 * 1024;

constexpr usize PathCapacity = 4096;

bool ReadText(StringView path, Array<uint8>& bytes) noexcept {
    bool existed = false;
    return ReadWholeFile(platform::PhysicalFileSystem(), path, FileMaxBytes, bytes, existed) &&
           existed;
}

StringView TextOf(const Array<uint8>& bytes) noexcept {
    return StringView(reinterpret_cast<const char*>(bytes.Data()), bytes.Size());
}

// path의 마지막 구분자(`/`·`\`) 앞까지의 길이다(없으면 0).
usize ParentLength(StringView path) noexcept {
    usize at = path.ByteCount();
    while (at > 0 && path.Data()[at - 1] != '/' && path.Data()[at - 1] != '\\') {
        --at;
    }

    return at == 0 ? 0 : at - 1;
}

bool Contains(const Array<StringView>& words, StringView word) noexcept {
    for (usize at = 0; at < words.Size(); ++at) {
        if (words[at] == word) {
            return true;
        }
    }

    return false;
}

// JSON 문자열 배열을 words에 넣는다. 문자열이 아니거나 겹치면 거짓이다.
bool Strings(JsonValue array, Array<StringView>& words) noexcept {
    if (!array.IsValid() || !array.IsArray()) {
        return false;
    }

    for (usize at = 0; at < array.Size(); ++at) {
        const JsonValue item = array.At(at);
        if (!item.IsString() || Contains(words, item.GetString()) ||
            !words.TryPushBack(item.GetString())) {
            return false;
        }
    }

    return true;
}

} // namespace

TEST(SparksBinding_NamesMatchCompiler) {
    SystemAllocator allocator;
    {
        char engine[PathCapacity];
        auto found = FindEngineDirectory(Span<char>(engine, PathCapacity));
        REQUIRE(found);
        // 엔진 폴더(`<저장소>/Engine`)의 부모가 저장소 뿌리다.
        const usize root = ParentLength(StringView(engine, found.Value()));
        REQUIRE(root > 0);
        auto path = Path::Combine(allocator, StringView(engine, root), NamesPath);
        REQUIRE(path);
        Array<uint8> bytes(allocator);
        REQUIRE(ReadText(path.Value().View(), bytes));
        JsonDocument document(allocator);
        REQUIRE(document.Parse(TextOf(bytes)).Succeeded());

        Array<StringView> owned(allocator);
        Array<StringView> hooks(allocator);
        Array<StringView> unity(allocator);
        Array<StringView> keywords(allocator);
        Array<StringView> reserved(allocator);
        REQUIRE(Strings(document.Root().Find("languageOwnedNames"), owned));
        REQUIRE(Strings(document.Root().Find("keywords"), keywords));
        REQUIRE(Strings(document.Root().Find("reservedNames"), reserved));
        REQUIRE(Strings(document.Root().Find("hooks"), hooks));
        REQUIRE(Strings(document.Root().Find("unityHookAliases"), unity));

        // 집합이 같다(크기가 같고 컴파일러의 이름이 모두 표에 있다 — 표에 겹침이 없음은 위에서
        // 봤다).
        CHECK(owned.Size() == LanguageOwnedNameCount());
        for (uint32 at = 0; at < LanguageOwnedNameCount(); ++at) {
            CHECK(Contains(owned, LanguageOwnedNameAt(at)));
        }

        CHECK(hooks.Size() == LanguageHooks().Size());
        for (const LanguageHook& hook : LanguageHooks()) {
            CHECK(Contains(hooks, hook.name));
        }

        usize unityHooks = 0;
        for (const UnityAlias& alias : UnityAliases()) {
            if (alias.hook) {
                ++unityHooks;
                CHECK(Contains(unity, alias.unity));
            }
        }

        CHECK(unity.Size() == unityHooks);

        // 키워드는 함수 매개변수 이름 검사가 쓴다(SparksBinding B13).
        CHECK(keywords.Size() == LanguageKeywordCount());
        for (uint32 at = 0; at < LanguageKeywordCount(); ++at) {
            CHECK(Contains(keywords, LanguageKeywordAt(at).name));
        }

        // 예약 이름은 노출 멤버 이름 검사가 쓴다(SparksBinding B13).
        CHECK(reserved.Size() == ReservedNameCount());
        for (uint32 at = 0; at < ReservedNameCount(); ++at) {
            CHECK(Contains(reserved, ReservedNameAt(at)));
        }
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

namespace {

// 빌드 출력 `Metadata/`의 modules schema를 temp의 `Schemas/`로 옮긴다(SparksBinding B9 — 시험
// 실행 파일 `<출력>/bin/` 옆).
bool CopySchemas(Allocator& allocator, TempFolder& temp, Span<const StringView> modules) noexcept {
    char executable[PathCapacity];
    usize bytes = 0;
    if (!platform::GetExecutablePath(executable, PathCapacity, bytes)) {
        return false;
    }

    const usize bin = ParentLength(StringView(executable, bytes));
    const usize output = ParentLength(StringView(executable, bin));
    auto metadata = Path::Combine(allocator, StringView(executable, output), "Metadata");
    if (output == 0 || !metadata) {
        return false;
    }

    for (const StringView module : modules) {
        StringBuilder<128> name(allocator);
        StringBuilder<128> target(allocator);
        if (!name.TryAppendFormat("{}.sparkbind.json", module) ||
            !target.TryAppendFormat("Schemas/{}", name.View())) {
            return false;
        }

        auto source = Path::Combine(allocator, metadata.Value().View(), name.View());
        Array<uint8> text(allocator);
        if (!source || !ReadText(source.Value().View(), text) ||
            !temp.Write(target.View(), TextOf(text))) {
            return false;
        }
    }

    return true;
}

// 이벤트 fixture(ReflectionTestFixtures EventBeacon)를 쓰는 Sparks 원본 하나를 검사하고 종료 코드와
// 출력(요약·진단)을 돌려준다.
struct EventCheck {
    int32 exit = 0;
    String output;
};

EventCheck CheckWithEvents(Allocator& allocator, StringView source) noexcept {
    EventCheck checked{ExitFailure, String(allocator)};
    TempFolder temp(allocator);
    constexpr StringView Modules[] = {"CoreTypeBindings", "World", "ReflectionTestFixtures"};
    if (!CopySchemas(allocator, temp, Modules) ||
        !temp.Write("Pkg/SparksPackage.json5", Manifest) ||
        !temp.Write("Pkg/Watch.spark", source)) {
        return checked;
    }

    const String schemas = temp.PathOf("Schemas");
    const String package = temp.PathOf("Pkg");
    BufferOutput result(allocator);
    const StringView arguments[] = {"check", "--schema", schemas.View(), package.View()};
    checked.exit = RunCommand(allocator, platform::PhysicalFileSystem(), arguments, result);
    (void)checked.output.TryAssign(result.output.View());
    return checked;
}

} // namespace

// 빌드 출력 배치 `<출력>/bin/<실행 파일>` → `<출력>/Metadata/`에
// 기댄다(Docs/Design/SparksBinding.ko.md B9 — 폴더가 없으면 건너뛰지 않고 실패한다).
TEST(SparksBinding_EngineSchemasCompile) {
    SystemAllocator allocator;
    {
        // 읽을 모듈은 목록으로 정한다(도구·픽스처 모듈과 남은 옛 파일이 섞이지 않게).
        constexpr StringView Modules[] = {"CoreTypeBindings", "World", "Gameplay"};
        TempFolder temp(allocator);
        REQUIRE(CopySchemas(allocator, temp, Modules));

        // 실제 엔진 타입·필드를 읽는다(필드 타입이 기대와 같아야 대입·비교가 진단 없이 통과한다).
        REQUIRE(temp.Write("Pkg/SparksPackage.json5", Manifest));
        REQUIRE(
            temp.Write("Pkg/Probe.spark",
                       "fn Probe(a: Actor, s: SceneComponent, p: Pawn): bool {\n"
                       "    let root: SceneComponent? = a.RootComponent;\n"
                       "    let parts: Array<Component> = a.Components;\n"
                       "    let place: Transform = s.RelativeTransform;\n"
                       "    let parent: SceneComponent? = s.AttachParent;\n"
                       "    let moves: bool = s.Mobility == ComponentMobility.Movable;\n"
                       "    let shown: bool = s.Visible && !s.HiddenInGame;\n"
                       "    let fixed: bool = s.AbsoluteLocation || s.AbsoluteRotation || "
                       "s.AbsoluteScale;\n"
                       "    let component: Component = s;\n"
                       "    return a.Enabled && p.Enabled && component.Enabled && moves && shown "
                       "&& !fixed && root != null && parent == null && parts.AliveCount() > 0 && "
                       "place.Scale.X > 0.0;\n"
                       "}\n"));
        // 노출 함수(EI1b — SparksBinding B17)를 부른다: Result<void>·TryX → X·Pure 값·enum 인자.
        // 상태를 바꾸는 엔진 함수는 hook 문맥에서만 부를 수 있다(S0602 — 최상위 함수는 안 된다).
        REQUIRE(temp.Write("Pkg/Mover.spark",
                           "class Mover : Actor {\n"
                           "    fn Move(s: SceneComponent): Result<void> {\n"
                           "        try s.SetRelativeLocation(s.WorldLocation());\n"
                           "        let placed: Transform = try s.WorldTransform();\n"
                           "        s.SetMobility(ComponentMobility.Movable);\n"
                           "        let shown: bool = s.IsVisible();\n"
                           "        _ = placed;\n"
                           "        _ = shown;\n"
                           "        return;\n"
                           "    }\n"
                           "\n"
                           "    on Start() {\n"
                           "        if (let root = RootComponent) {\n"
                           "            _ = Move(root);\n"
                           "        }\n"
                           "    }\n"
                           "}\n"));
        const String schemas = temp.PathOf("Schemas");
        const String package = temp.PathOf("Pkg");
        BufferOutput result(allocator);
        const StringView arguments[] = {"check", "--schema", schemas.View(), package.View()};
        CHECK(RunCommand(allocator, platform::PhysicalFileSystem(), arguments, result) ==
              ExitSuccess);
        // 원본 파일 하나가 모듈 하나다(Probe·Mover).
        CHECK(result.output.View() == "0 errors in 2 modules\n");
        CHECK(result.error.View().IsEmpty());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 노출 이벤트(Docs/Design/SparksBinding.ko.md EI1c B29): 생성된 fixture schema의 이벤트에 `on`
// handler(인자 일부 — 앞 인자만, 전부, 인자 없음, 약한 참조 인자)와 `await`가 진단 없이 컴파일되고,
// 인자 타입이 다른 handler는 S0810, 엔진 이벤트 `emit`은 S0811이다.
TEST(SparksBinding_EngineEventsCompile) {
    SystemAllocator allocator;
    {
        const EventCheck accepted = CheckWithEvents(
            allocator, "class Watcher : Actor {\n"
                       "    @edit var Source: EventBeacon?;\n"
                       "    @edit var Spare: EventBeacon?;\n"
                       "    var Total: int32 = 0;\n"
                       "\n"
                       "    on Source.Scored(amount: int32) { Total = Total + amount; }\n"
                       "    on Source.Tagged(label: string) { _ = label; }\n"
                       "    on Spare.Tagged(label: string, mood: BeaconMood) {\n"
                       "        if (mood == BeaconMood.Alert) { Total = Total + 1; }\n"
                       "        _ = label;\n"
                       "    }\n"
                       "    on Source.Pinged() { Total = Total + 1; }\n"
                       "    on Source.Linked(other: EventBeacon?) { _ = other; }\n"
                       "\n"
                       "    async fn Wait(beacon: EventBeacon) { await beacon.Pinged; }\n"
                       "}\n");
        CHECK(accepted.exit == ExitSuccess);
        CHECK(accepted.output.View() == "0 errors in 1 modules\n");

        const EventCheck mismatched =
            CheckWithEvents(allocator, "class Watcher : Actor {\n"
                                       "    @edit var Source: EventBeacon?;\n"
                                       "\n"
                                       "    on Source.Scored(amount: string) { _ = amount; }\n"
                                       "}\n");
        CHECK(mismatched.exit == ExitFailure);
        CHECK(mismatched.output.View().Contains("S0810", StringComparison::CaseSensitive));

        const EventCheck emitted = CheckWithEvents(
            allocator, "class Watcher : Actor {\n"
                       "    fn Ring(beacon: EventBeacon) { emit beacon.Pinged(); }\n"
                       "}\n");
        CHECK(emitted.exit == ExitFailure);
        CHECK(emitted.output.View().Contains("S0811", StringComparison::CaseSensitive));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

} // namespace sparks_tools_binding_tests_file
