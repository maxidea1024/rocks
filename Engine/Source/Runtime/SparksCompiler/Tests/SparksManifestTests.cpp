#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/Fatal.h>
#include <SparksCompiler/Diagnostic.h>
#include <SparksCompiler/PackageManifest.h>
#include <SparksCompiler/Source.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// 패키지 manifest 시험이다(Docs/Design/SparksLanguage.ko.md 3.4절 "패키지 manifest", S1101–S1105):
// key마다 양성·음성, 진단 위치(값의 시작, 빠진 key는 최상위 객체), 문장, 여러 오류를 함께 알림,
// 할당 실패.
using namespace rocks;
using namespace rocks::sparks;
namespace sparks_manifest_tests_file {

namespace {

using C = DiagnosticCode;

struct Expected {
    DiagnosticCode code;
    uint32 line;
    uint32 column;
};

struct Read {
    explicit Read(Allocator& allocator) noexcept
        : sources(allocator), diagnostics(allocator), manifest(allocator) {
    }

    SourceSet sources;
    DiagnosticList diagnostics;
    PackageManifest manifest;
    bool valid = false;
};

bool ReadText(Read& read, StringView text) noexcept {
    auto id = read.sources.TryAdd("SparksPackage.json5", text, read.diagnostics);
    if (!id) {
        return false;
    }

    auto result = ReadPackageManifest(read.sources, id.Value(), read.diagnostics, read.manifest);
    if (!result) {
        return false;
    }

    read.valid = result.Value();
    read.diagnostics.Sort(read.sources);
    return true;
}

void PrintDiagnostics(Allocator& allocator, const Read& read) noexcept {
    StringBuilder<512> out(allocator);
    static_cast<void>(AppendDiagnostics(out, read.sources, read.diagnostics));
    platform::WriteDiagnostic(out.View().Data(), out.ByteCount());
}

// 진단이 정확히 이것뿐이고(순서·코드·위치) 결과가 무효인지 본다.
bool Reports(StringView text, Span<const Expected> expected) noexcept {
    SystemAllocator allocator;
    bool same = false;
    {
        Read read(allocator);
        if (ReadText(read, text)) {
            same = !read.valid && read.diagnostics.Count() == expected.Size();
            const SourceFile& file = read.sources.Get(SourceId{0});
            for (usize index = 0; same && index < expected.Size(); ++index) {
                const Diagnostic& diagnostic = read.diagnostics[index];
                const sparks::SourceLocation location = file.Locate(diagnostic.Span().begin);
                same = diagnostic.Code() == expected[index].code &&
                       location.line == expected[index].line &&
                       location.column == expected[index].column;
            }

            if (!same) {
                platform::WriteDiagnostic(text.Data(), text.ByteCount());
                platform::WriteDiagnostic("\n", 1);
                PrintDiagnostics(allocator, read);
            }
        }
    }

    return same && allocator.Stats().liveAllocations == 0;
}

bool Reports(StringView text, DiagnosticCode code, uint32 line, uint32 column) noexcept {
    const Expected expected[] = {{code, line, column}};
    return Reports(text, expected);
}

// 첫 진단의 문장 세 개다.
bool FirstSays(StringView text, StringView message, StringView explanation,
               StringView fix) noexcept {
    SystemAllocator allocator;
    bool same = false;
    {
        Read read(allocator);
        if (ReadText(read, text) && !read.diagnostics.IsEmpty()) {
            const Diagnostic& first = read.diagnostics[0];
            same = first.Message() == message && first.Explanation() == explanation &&
                   first.Fix() == fix;
            if (!same) {
                PrintDiagnostics(allocator, read);
            }
        }
    }

    return same && allocator.Stats().liveAllocations == 0;
}

} // namespace

TEST(SparksManifest_ReadsEveryKey) {
    SystemAllocator allocator;
    {
        Read read(allocator);
        REQUIRE(ReadText(read, "// Game package\n"
                               "{\n"
                               "  name: 'Game',\n"
                               "  sparks: 1,\n"
                               "  profile: \"full\",\n"
                               "  dependencies: [\"Weapons\", \"Ui\"],\n"
                               "  services: [\"Combat.Score.ScoreKeeper\"],\n"
                               "  logCategory: \"Game.Doors\",\n"
                               "}\n"));
        CHECK(read.valid);
        CHECK(read.diagnostics.IsEmpty());
        CHECK(read.manifest.Name() == StringView("Game"));
        CHECK(read.manifest.LanguageVersion() == CurrentLanguageVersion);
        CHECK(read.manifest.GetProfile() == Profile::Full);
        REQUIRE(read.manifest.Dependencies().Size() == 2);
        CHECK(read.manifest.Text(read.manifest.Dependencies()[0]) == StringView("Weapons"));
        CHECK(read.manifest.Text(read.manifest.Dependencies()[1]) == StringView("Ui"));
        REQUIRE(read.manifest.Services().Size() == 1);
        CHECK(read.manifest.Text(read.manifest.Services()[0]) ==
              StringView("Combat.Score.ScoreKeeper"));
        CHECK(read.manifest.LogCategory() == StringView("Game.Doors"));
        // 값의 원문 범위(따옴표 포함)다.
        const SourceFile& file = read.sources.Get(SourceId{0});
        CHECK(file.Locate(read.manifest.NameSpan().begin).line == 3);
        CHECK(file.Locate(read.manifest.NameSpan().begin).column == 9);
        CHECK(read.manifest.NameSpan().end - read.manifest.NameSpan().begin == 6);
        CHECK(file.Locate(read.manifest.Dependencies()[1].span.begin).column == 29);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksManifest_DefaultsAndCoreProfile) {
    SystemAllocator allocator;
    {
        Read read(allocator);
        REQUIRE(ReadText(read, "{ name: \"Kit\", sparks: 1, profile: \"core\" }"));
        CHECK(read.valid);
        CHECK(read.manifest.GetProfile() == Profile::Core);
        CHECK(read.manifest.Dependencies().IsEmpty());
        CHECK(read.manifest.Services().IsEmpty());
        CHECK(read.manifest.LogCategory() == StringView("Kit"));
        // 빈 배열도 받는다.
        Read empty(allocator);
        REQUIRE(ReadText(empty, "{ name: \"Kit\", sparks: 1, profile: \"core\", dependencies: [], "
                                "services: [] }"));
        CHECK(empty.valid);
        // 같은 객체를 다시 쓰면 이전 내용을 버린다.
        REQUIRE(ReadText(read, "{ name: \"Other\", sparks: 1, profile: \"full\", "
                               "dependencies: [\"A\"] }"));
        CHECK(read.manifest.Name() == StringView("Other"));
        CHECK(read.manifest.Dependencies().Size() == 1);
        CHECK(read.manifest.LogCategory() == StringView("Other"));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksManifest_SyntaxS1101) {
    CHECK(Reports("{ name: \"A\", sparks: 1,, profile: \"core\" }", C::ManifestSyntax, 1, 24));
    CHECK(Reports("{ name: \"A\", name: \"B\", sparks: 1, profile: \"core\" }", C::ManifestSyntax,
                  1, 14));
    CHECK(Reports("[1, 2]", C::ManifestSyntax, 1, 1));
    CHECK(Reports("\"Game\"", C::ManifestSyntax, 1, 1));
    CHECK(Reports("", C::ManifestSyntax, 1, 1));
    CHECK(FirstSays("{ name: }", "The package manifest is not valid JSON5.", "unexpected character",
                    "Fix the JSON5 syntax at this position."));
}

TEST(SparksManifest_UnknownKeyS1102) {
    CHECK(Reports("{ name: \"A\", sparks: 1, profile: \"core\", version: 2 }",
                  C::UnknownManifestKey, 1, 51));
    CHECK(FirstSays("{ name: \"A\", sparks: 1, profile: \"core\", profil: 1 }",
                    "Unknown manifest key 'profil'.",
                    "A manifest has name, sparks, profile, dependencies, services and logCategory.",
                    "Rename it to 'profile'."));
    CHECK(FirstSays("{ name: \"A\", sparks: 1, profile: \"core\", zzz: 1 }",
                    "Unknown manifest key 'zzz'.",
                    "A manifest has name, sparks, profile, dependencies, services and logCategory.",
                    "Remove the key."));
    // 대소문자만 다른 key도 알 수 없는 key이며 제안한다.
    CHECK(FirstSays("{ Name: \"A\", sparks: 1, profile: \"core\", name: \"A\" }",
                    "Unknown manifest key 'Name'.",
                    "A manifest has name, sparks, profile, dependencies, services and logCategory.",
                    "Rename it to 'name'."));
    // 인쇄할 수 없는 key는 문장에 넣지 않는다.
    CHECK(FirstSays("{ \"a\\nb\": 1, name: \"A\", sparks: 1, profile: \"core\" }",
                    "Unknown manifest key.",
                    "A manifest has name, sparks, profile, dependencies, services and logCategory.",
                    "Remove the key."));
}

TEST(SparksManifest_MissingKeyS1103) {
    CHECK(Reports("{ sparks: 1, profile: \"core\" }", C::MissingManifestKey, 1, 1));
    CHECK(Reports("\n  { name: \"A\", profile: \"core\" }", C::MissingManifestKey, 2, 3));
    CHECK(Reports("{ name: \"A\", sparks: 1 }", C::MissingManifestKey, 1, 1));
    const Expected all[] = {{C::MissingManifestKey, 1, 1},
                            {C::MissingManifestKey, 1, 1},
                            {C::MissingManifestKey, 1, 1}};
    CHECK(Reports("{}", all));
    CHECK(FirstSays("{ sparks: 1, profile: \"core\" }", "The manifest has no 'name'.",
                    "'name', 'sparks' and 'profile' are required.",
                    "Add a package name, as in 'name: \"Game\"'."));
}

TEST(SparksManifest_NameS1104) {
    CHECK(Reports("{ name: 1, sparks: 1, profile: \"core\" }", C::InvalidManifestValue, 1, 9));
    CHECK(
        Reports("{ name: \"game\", sparks: 1, profile: \"core\" }", C::InvalidManifestValue, 1, 9));
    CHECK(Reports("{ name: \"Game_Kit\", sparks: 1, profile: \"core\" }", C::InvalidManifestValue,
                  1, 9));
    CHECK(Reports("{ name: \"1Game\", sparks: 1, profile: \"core\" }", C::InvalidManifestValue, 1,
                  9));
    CHECK(Reports("{ name: \"\", sparks: 1, profile: \"core\" }", C::InvalidManifestValue, 1, 9));
    CHECK(Reports("{ name: \"Rocks\", sparks: 1, profile: \"core\" }", C::InvalidManifestValue, 1,
                  9));
    // 56 byte까지 받고 57 byte는 오류다(logCategory 기본값이 상한에 들게).
    SystemAllocator allocator;
    {
        StringBuilder<256> fits(allocator);
        StringBuilder<256> tooLong(allocator);
        fits.Append("{ name: \"A");
        tooLong.Append("{ name: \"A");
        for (usize index = 1; index < MaxPackageNameBytes; ++index) {
            fits.Append("b");
            tooLong.Append("b");
        }

        tooLong.Append("b");
        fits.Append("\", sparks: 1, profile: \"core\" }");
        tooLong.Append("\", sparks: 1, profile: \"core\" }");
        Read read(allocator);
        REQUIRE(ReadText(read, fits.View()));
        CHECK(read.valid);
        CHECK(MaxPackageNameBytes == 56);
        CHECK(read.manifest.Name().ByteCount() == 56);
        // 생략한 logCategory는 이름이며 logCategory 상한에 든다.
        CHECK(read.manifest.LogCategory() == read.manifest.Name());
        CHECK(read.manifest.LogCategory().ByteCount() <= MaxLogCategoryBytes);
        CHECK(Reports(tooLong.View(), C::InvalidManifestValue, 1, 9));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
    CHECK(FirstSays("{ name: \"Rocks\", sparks: 1, profile: \"core\" }",
                    "'Rocks' is the engine package name.",
                    "Every package already depends on the engine package 'Rocks'.",
                    "Choose another package name."));
    CHECK(FirstSays("{ name: \"game\", sparks: 1, profile: \"core\" }",
                    "Package name 'game' is not valid.",
                    "A package name starts with an uppercase English letter and uses only ASCII "
                    "letters and digits (at most 56 bytes).",
                    "Use a name such as 'Game' or 'DoorKit'."));
}

TEST(SparksManifest_VersionS1104AndS1105) {
    CHECK(
        Reports("{ name: \"A\", sparks: 1.0, profile: \"core\" }", C::InvalidManifestValue, 1, 22));
    CHECK(Reports("{ name: \"A\", sparks: \"1\", profile: \"core\" }", C::InvalidManifestValue, 1,
                  22));
    CHECK(Reports("{ name: \"A\", sparks: 2, profile: \"core\" }", C::UnsupportedLanguageVersion, 1,
                  22));
    CHECK(Reports("{ name: \"A\", sparks: 0, profile: \"core\" }", C::UnsupportedLanguageVersion, 1,
                  22));
    CHECK(Reports("{ name: \"A\", sparks: -1, profile: \"core\" }", C::UnsupportedLanguageVersion,
                  1, 22));
    CHECK(FirstSays("{ name: \"A\", sparks: 2, profile: \"core\" }",
                    "Sparks language version 2 is not supported.",
                    "This compiler knows language version 1 only.", "Write 'sparks: 1'."));
    // JSON5 정수 표기는 무엇이든 된다(16진). 소수·지수 표기는 정수가 아니다.
    {
        SystemAllocator allocator;
        {
            Read read(allocator);
            REQUIRE(ReadText(read, "{ name: \"A\", sparks: 0x1, profile: \"core\" }"));
            CHECK(read.valid);
            CHECK(read.diagnostics.IsEmpty());
            CHECK(read.manifest.LanguageVersion() == 1);
        }
        CHECK(allocator.Stats().liveAllocations == 0);
    }
    CHECK(
        Reports("{ name: \"A\", sparks: 1e0, profile: \"core\" }", C::InvalidManifestValue, 1, 22));
    CHECK(Reports("{ name: \"A\", sparks: Infinity, profile: \"core\" }", C::InvalidManifestValue,
                  1, 22));
    // int64를 넘는 정수도 정수다(지원하지 않는 버전).
    CHECK(FirstSays("{ name: \"A\", sparks: 18446744073709551615, profile: \"core\" }",
                    "Sparks language version 18446744073709551615 is not supported.",
                    "This compiler knows language version 1 only.", "Write 'sparks: 1'."));
}

// 다른 key가 틀려도 `name` 값이 맞으면 이름은 남는다(패키지 이름 중복 검사에 쓴다).
TEST(SparksManifest_NameSurvivesOtherErrors) {
    SystemAllocator allocator;
    {
        Read read(allocator);
        REQUIRE(ReadText(read, "{ name: \"Game\", sparks: 2, profile: \"x\", extra: 1 }"));
        CHECK(!read.valid);
        CHECK(read.diagnostics.Count() == 3);
        CHECK(read.manifest.Name() == StringView("Game"));
        CHECK(read.manifest.NameSpan().begin == 8);
        CHECK(read.manifest.NameSpan().end == 14);
    }
    {
        Read read(allocator);
        REQUIRE(ReadText(read, "{ name: \"game\", sparks: 1, profile: \"core\" }"));
        CHECK(!read.valid);
        CHECK(read.manifest.Name().IsEmpty());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// BOM과 CRLF가 있어도 위치는 그 값의 줄·열이다.
TEST(SparksManifest_PositionsWithBomAndCrlf) {
    CHECK(Reports("\xEF\xBB\xBF{\r\n  name: \"game\",\r\n  sparks: 1, profile: \"core\" }",
                  C::InvalidManifestValue, 2, 9));
    CHECK(Reports("\xEF\xBB\xBF{ name: \"A\",\r\n\r\n  sparks: 7, profile: \"core\" }",
                  C::UnsupportedLanguageVersion, 3, 11));
    CHECK(Reports("\xEF\xBB\xBF\r\n{ sparks: 1, profile: \"core\" }", C::MissingManifestKey, 2, 1));
}

TEST(SparksManifest_ProfileS1104) {
    CHECK(Reports("{ name: \"A\", sparks: 1, profile: \"Core\" }", C::InvalidManifestValue, 1, 34));
    CHECK(Reports("{ name: \"A\", sparks: 1, profile: 1 }", C::InvalidManifestValue, 1, 34));
    CHECK(Reports("{ name: \"A\", sparks: 1, profile: \"\" }", C::InvalidManifestValue, 1, 34));
}

TEST(SparksManifest_DependenciesS1104) {
    constexpr StringView Head = "{ name: \"Game\", sparks: 1, profile: \"core\", dependencies: ";
    SystemAllocator allocator;
    {
        auto with = [&](StringView value) noexcept {
            StringBuilder<256> text(allocator);
            text.Append(Head).Append(value).Append(" }");
            String result(allocator);
            static_cast<void>(result.TryAssign(text.View()));
            return result;
        };
        const uint32 at = static_cast<uint32>(Head.ByteCount()) + 1;
        CHECK(Reports(with("\"Weapons\"").View(), C::InvalidManifestValue, 1, at));
        CHECK(Reports(with("[1]").View(), C::InvalidManifestValue, 1, at + 1));
        CHECK(Reports(with("[\"weapons\"]").View(), C::InvalidManifestValue, 1, at + 1));
        CHECK(Reports(with("[\"Rocks\"]").View(), C::InvalidManifestValue, 1, at + 1));
        CHECK(Reports(with("[\"Game\"]").View(), C::InvalidManifestValue, 1, at + 1));
        CHECK(Reports(with("[\"A\", \"A\"]").View(), C::InvalidManifestValue, 1, at + 6));
        // 원소 오류는 모두 알린다.
        const Expected both[] = {{C::InvalidManifestValue, 1, at + 1},
                                 {C::InvalidManifestValue, 1, at + 4}};
        CHECK(Reports(with("[1, \"x\", \"Ok\"]").View(), both));
        CHECK(FirstSays(with("[\"A\", \"A\"]").View(), "Dependency 'A' is listed twice.", "",
                        "Remove the repeated entry."));
        CHECK(FirstSays(with("[\"Game\"]").View(), "A package cannot depend on itself.",
                        "Modules of the same package import each other without the package name.",
                        "Remove this dependency."));
        CHECK(FirstSays(with("[\"Rocks\"]").View(), "'Rocks' is always a dependency.",
                        "The engine package is available to every package without listing it.",
                        "Remove 'Rocks' from the dependencies."));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
    // 자기 자신 의존은 name이 뒤에 와도 판정한다.
    CHECK(Reports("{ dependencies: [\"Game\"], name: \"Game\", sparks: 1, profile: \"core\" }",
                  C::InvalidManifestValue, 1, 18));
}

TEST(SparksManifest_ServicesS1104) {
    constexpr StringView Head = "{ name: \"Game\", sparks: 1, profile: \"core\", services: ";
    SystemAllocator allocator;
    {
        auto with = [&](StringView value) noexcept {
            StringBuilder<256> text(allocator);
            text.Append(Head).Append(value).Append(" }");
            String result(allocator);
            static_cast<void>(result.TryAssign(text.View()));
            return result;
        };
        const uint32 at = static_cast<uint32>(Head.ByteCount()) + 1;
        CHECK(Reports(with("{}").View(), C::InvalidManifestValue, 1, at));
        CHECK(Reports(with("[true]").View(), C::InvalidManifestValue, 1, at + 1));
        CHECK(Reports(with("[\"Score\"]").View(), C::InvalidManifestValue, 1, at + 1));
        CHECK(Reports(with("[\"Combat.score\"]").View(), C::InvalidManifestValue, 1, at + 1));
        CHECK(Reports(with("[\"Combat..Score\"]").View(), C::InvalidManifestValue, 1, at + 1));
        CHECK(Reports(with("[\"A.B\", \"A.B\"]").View(), C::InvalidManifestValue, 1, at + 8));
        Read read(allocator);
        REQUIRE(ReadText(read, with("[\"A.B\", \"Combat.Score.Keeper\"]").View()));
        CHECK(read.valid);
        CHECK(read.manifest.Services().Size() == 2);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksManifest_LogCategoryS1104) {
    constexpr StringView Head = "{ name: \"Game\", sparks: 1, profile: \"core\", logCategory: ";
    SystemAllocator allocator;
    {
        auto with = [&](StringView value) noexcept {
            StringBuilder<256> text(allocator);
            text.Append(Head).Append(value).Append(" }");
            String result(allocator);
            static_cast<void>(result.TryAssign(text.View()));
            return result;
        };
        const uint32 at = static_cast<uint32>(Head.ByteCount()) + 1;
        CHECK(Reports(with("1").View(), C::InvalidManifestValue, 1, at));
        CHECK(Reports(with("\"game\"").View(), C::InvalidManifestValue, 1, at));
        CHECK(Reports(with("\"Game.\"").View(), C::InvalidManifestValue, 1, at));
        CHECK(Reports(with("\"\"").View(), C::InvalidManifestValue, 1, at));
        // 7구간까지 받고 8구간은 오류다.
        Read seven(allocator);
        REQUIRE(ReadText(seven, with("\"A.B.C.D.E.F.G\"").View()));
        CHECK(seven.valid);
        CHECK(Reports(with("\"A.B.C.D.E.F.G.H\"").View(), C::InvalidManifestValue, 1, at));
        // 56 byte까지 받고 57 byte는 오류다.
        StringBuilder<128> fits(allocator);
        fits.Append("\"A");
        for (usize index = 1; index < MaxLogCategoryBytes; ++index) {
            fits.Append("b");
        }

        StringBuilder<128> over(allocator);
        over.Append(fits.View()).Append("b\"");
        fits.Append("\"");
        Read longest(allocator);
        REQUIRE(ReadText(longest, with(fits.View()).View()));
        CHECK(longest.valid);
        CHECK(Reports(with(over.View()).View(), C::InvalidManifestValue, 1, at));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 오류가 여럿이면 끝까지 읽어 모두 알린다(key마다 하나).
TEST(SparksManifest_ReportsEveryProblem) {
    const Expected all[] = {{C::InvalidManifestValue, 1, 9},
                            {C::UnsupportedLanguageVersion, 1, 25},
                            {C::InvalidManifestValue, 1, 37},
                            {C::UnknownManifestKey, 1, 47}};
    CHECK(Reports("{ name: \"game\", sparks: 3, profile: \"x\", odd: 1 }", all));
}

TEST(SparksManifest_RejectsUnusableSources) {
    SystemAllocator allocator;
    {
        SourceSet sources(allocator);
        DiagnosticList diagnostics(allocator);
        auto id = sources.TryAdd("SparksPackage.json5", "\xFF", diagnostics);
        REQUIRE(id);
        PackageManifest manifest(allocator);
        auto read = ReadPackageManifest(sources, id.Value(), diagnostics, manifest);
        CHECK(!read && read.Error() == ErrorCode::InvalidArgument);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksManifest_AllocationFailureEverywhere) {
    SystemAllocator backing;
    constexpr StringView Text =
        "{ name: \"Game\", sparks: 1, profile: \"full\", dependencies: [\"Weapons\", \"x\"], "
        "services: [\"A.B\"], logCategory: \"Game.Doors\", odd: 1 }";
    bool succeeded = false;
    uint64 failures = 0;
    for (uint64 failAt = 1; !succeeded && failAt < 10000; ++failAt) {
        test::NFailAllocator allocator(backing);
        {
            SourceSet sources(allocator);
            DiagnosticList diagnostics(allocator);
            allocator.DisableFailure();
            auto id = sources.TryAdd("SparksPackage.json5", Text, diagnostics);
            REQUIRE(id);
            PackageManifest manifest(allocator);
            allocator.FailOnAttempt(failAt);
            auto read = ReadPackageManifest(sources, id.Value(), diagnostics, manifest);
            if (read) {
                succeeded = true;
                CHECK(!read.Value());
                CHECK(diagnostics.Count() == 2);
            } else {
                ++failures;
                CHECK(read.Error() == ErrorCode::OutOfMemory);
            }
        }
        CHECK(backing.Stats().liveAllocations == 0);
    }

    CHECK(succeeded);
    CHECK(failures >= 3);
}

} // namespace sparks_manifest_tests_file
