using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

// CVar 정의의 컴파일 타임 검사를 실제 컴파일러로 확인한다(Docs/Design/CVarSystem.ko.md 4.3·19절 — CVR1, 구문 검사만 한다).
// 1·4절 예시는 컴파일되고, 잘못된 정의는 실패하며 진단에 기대한 영어 static_assert 문구나 표지 함수 이름이 나와야 한다(문법
// 오류로 실패한 것은 통과로 치지 않는다 — 대조 컴파일이 같은 prelude로 성공해야 한다).
internal static class CVarCompilerTests
{
    private const string Prelude = """
        #include <Config/CVar.h>

        namespace probe {

        using namespace rocks;

        enum class ShadowQuality : int32 { Off, Low, Medium, High, Epic, Count };

        constexpr Span<const StringView> CVarEnumNames(ShadowQuality) noexcept {
            static constexpr StringView names[] = {"Off", "Low", "Medium", "High", "Epic"};
            return names;
        }

        enum class Tier : int32 { Low = 10, High = 20 };

        constexpr Span<const cvar::EnumEntry> CVarEnumNames(Tier) noexcept {
            static constexpr cvar::EnumEntry entries[] = {{"Low", 10}, {"High", 20}};
            return entries;
        }

        enum class Plain : int32 { A, B };

        enum class Short : int32 { A, B, C, Count };

        constexpr Span<const StringView> CVarEnumNames(Short) noexcept {
            static constexpr StringView names[] = {"A", "B"};
            return names;
        }

        enum class Twice : int32 { A, B };

        constexpr Span<const StringView> CVarEnumNames(Twice) noexcept {
            static constexpr StringView names[] = {"Same", "same"};
            return names;
        }

        CVar<float> ShadowDistance = cvar::Var("render.shadow.distance", 8000.f)
            .Help("Max shadow distance (cm)").Range(500, 20000).Clamp();
        CVar<ShadowQuality> ShadowQualityLevel = cvar::Var("render.shadow.quality", ShadowQuality::High)
            .Help("Shadow quality level");
        BootCVar<String> GraphicsBackend = cvar::Boot("render.backend", "vulkan").Help("Graphics backend");
        CVar<bool> GodMode = cvar::Var("game.god", false).Help("Invulnerable player").Cheat();
        CVar<int32> Samples = cvar::Var("render.aa.samples", 4).Help("MSAA sample count")
            .Validate([](const int32& value) noexcept -> StringView {
                return (value & (value - 1)) == 0 ? StringView{} : "must be a power of two";
            });

        """;

    private const string Epilogue = "\n} // namespace probe\n";

    public static void Register(TestSuite suite, Options options, IReadOnlyDictionary<string, string?> environment)
    {
        if (options.Optional("compiler") is not string compiler)
        {
            Console.WriteLine("[SKIP] Native.CVarCompilerContracts: --compiler was not supplied");
            return;
        }

        suite.Add("Native.CVarCompilerContracts", async () =>
        {
            using TemporaryDirectory temporary = new();
            bool clangCl = Path.GetFileNameWithoutExtension(compiler).Equals("clang-cl", StringComparison.OrdinalIgnoreCase);
            string[] includeDirectories = Directory.GetDirectories(Path.Combine(options.Root, "Core", "Source", "Runtime"))
                .Select(module => Path.Combine(module, "Public")).Where(Directory.Exists).Order(StringComparer.Ordinal).ToArray();

            async Task<ProcessResult> Compile(string name, string body)
            {
                temporary.Write(name + ".cpp", Prelude + body + Epilogue);
                string source = temporary.FilePath(name + ".cpp");
                List<string> arguments = clangCl
                    ? ["/nologo", "/std:c++latest", "/utf-8", "/W4", "/WX", "/Zs", source]
                    : ["-std=c++23", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-fsyntax-only", source];
                foreach (string directory in includeDirectories)
                {
                    arguments.Add((clangCl ? "/I" : "-I") + directory);
                }

                arguments.AddRange(options.ClangArguments);
                return await ProcessRunner.RunAsync(new(compiler, arguments, options.Root, environment,
                    Timeout: TimeSpan.FromSeconds(120)));
            }

            // 1·4절 예시(prelude), 4.1절 표의 나머지 타입, 값을 정한 enum, 등록 호출, 상수 초기화(constinit)는 컴파일된다.
            string valid = string.Join('\n',
                "constinit CVar<int64> Budget = cvar::Var(\"stream.budget\", int64{1} << 40).Help(\"Budget\").Category(\"stream\");",
                "constinit CVar<double> Scale = cvar::Var(\"render.scale\", 1.0).Help(\"Scale\").Range(0.25, 2.0);",
                "constinit CVar<Name> Profile = cvar::Var<Name>(\"game.profile\", \"Default\").Help(\"Profile\");",
                "constinit CVar<String> Style = cvar::Var(\"ui.style\", \"plain\").Help(\"Style\").UserSetting().ConsoleReadOnly()",
                "    .Validate([](StringView text) noexcept -> StringView { return text.IsEmpty() ? \"empty\" : StringView{}; });",
                "constinit CVar<Tier> TierLevel = cvar::Var(\"game.tier\", Tier::High).Help(\"Tier\");",
                "constinit CVar<bool> Shared = cvar::Var(\"net.shared\", true).Help(\"Shared\").Replicated();",
                "constinit BootCVar<int32> Threads = cvar::Boot(\"game.threads\", 4).Help(\"Threads\").Range(1, 64).UserSetting();",
                "constinit CVarGroup ShadowGroup = cvar::Group(\"quality.shadow\", 1).Help(\"Shadow scalability\")",
                "    .Level(0, {{ShadowQualityLevel, ShadowQuality::Off}, {ShadowDistance, 1000}})",
                "    .Level(1, {{ShadowQualityLevel, ShadowQuality::High}, {ShadowDistance, 15000}});",
                "constinit CVarGroup OverallGroup = cvar::Group(\"quality.overall\", 0).Help(\"Overall\")",
                "    .Level(0, {{ShadowGroup, 0}, {Style, \"plain\"}}).Level(1);",
                "Result<void> RegisterProbeVars(config::ConfigContext& config) noexcept {",
                "    return config.Register(ShadowDistance, ShadowQualityLevel, GraphicsBackend, GodMode, Samples, Budget, Scale,",
                "                           Profile, Style, TierLevel, Shared, Threads, ShadowGroup, OverallGroup);",
                "}",
                "int32 LevelOf() noexcept { return ShadowGroup.Get(); }",
                "cvar::Index IndexOf() noexcept { return ShadowDistance.Index(); }");

            (string Name, string Source, string Diagnostic)[] rejected =
            [
                ("bool_range", ("CVar<bool> X = cvar::Var(\"game.x\", true).Help(\"x\").Range(false, true);"),
                    "Range() is only available for numeric CVars (int32, int64, float, double)"),
                ("enum_range", ("CVar<Tier> X = cvar::Var(\"game.x\", Tier::Low).Help(\"x\").Range(Tier::Low, Tier::High);"),
                    "Range() is only available for numeric CVars (int32, int64, float, double)"),
                ("string_clamp", ("CVar<String> X = cvar::Var(\"game.x\", \"a\").Help(\"x\").Clamp();"),
                    "Clamp() is only available for numeric CVars (int32, int64, float, double)"),
                ("auto_builder", "void F() noexcept { auto builder = cvar::Var(\"game.x\", 1).Help(\"x\"); (void)builder; }",
                    "deleted"),
                ("builder_get", "int32 F() noexcept { return cvar::Var(\"game.x\", 1).Help(\"x\").Get(); }",
                    "no member named 'Get'"),
                ("missing_help", ("CVar<int32> X = cvar::Var(\"game.x\", 1);"), "CVarError_MissingHelp"),
                ("name_uppercase", ("CVar<int32> X = cvar::Var(\"Render.X\", 1).Help(\"x\");"), "CVarError_InvalidName"),
                ("name_one_segment", ("CVar<int32> X = cvar::Var(\"render\", 1).Help(\"x\");"), "CVarError_InvalidName"),
                ("default_out_of_range", ("CVar<int32> X = cvar::Var(\"game.x\", 100).Help(\"x\").Range(0, 10);"),
                    "CVarError_DefaultOutOfRange"),
                ("default_out_of_clamped_range", ("CVar<float> X = cvar::Var(\"game.x\", 100.f).Help(\"x\").Range(0, 10).Clamp();"),
                    "CVarError_DefaultOutOfRange"),
                ("clamp_without_range", ("CVar<int32> X = cvar::Var(\"game.x\", 1).Help(\"x\").Clamp();"),
                    "CVarError_ClampWithoutRange"),
                ("range_order", ("CVar<int32> X = cvar::Var(\"game.x\", 5).Help(\"x\").Range(10, 0);"),
                    "CVarError_RangeMinimumAboveMaximum"),
                ("validate_default", ("CVar<int32> X = cvar::Var(\"game.x\", 3).Help(\"x\")" +
                    ".Validate([](const int32& v) noexcept -> StringView { return v % 2 == 0 ? StringView{} : \"even\"; });"),
                    "CVarError_DefaultFailsValidate"),
                ("enum_without_names", ("CVar<Plain> X = cvar::Var(\"game.x\", Plain::A).Help(\"x\");"),
                    "enum CVar needs names: mark the enum ROCKS_ENUM() or define constexpr CVarEnumNames(E) in the enum's namespace"),
                ("enum_count_mismatch", ("CVar<Short> X = cvar::Var(\"game.x\", Short::A).Help(\"x\");"),
                    "CVarEnumNames(E) size must equal E::Count"),
                ("enum_default_not_named", ("CVar<Tier> X = cvar::Var(\"game.x\", static_cast<Tier>(15)).Help(\"x\");"),
                    "CVarError_DefaultNotInEnum"),
                ("enum_duplicate_names", ("CVar<Twice> X = cvar::Var(\"game.x\", Twice::A).Help(\"x\");"),
                    "CVarError_InvalidEnumNames"),
                ("type_mismatch", ("CVar<int32> X = cvar::Var(\"game.x\", 1.f).Help(\"x\");"),
                    "CVar type must match the default value type"),
                ("unsupported_type", ("CVar<uint32> X = cvar::Var(\"game.x\", 1u).Help(\"x\");"),
                    "CVar type must be bool, int32, int64, float, double, String, Name or an enum class"),
                ("boot_set", "void F() noexcept { GraphicsBackend.Set(\"metal\"); }",
                    "BootCVar is fixed at boot; set it in a config file or with --set and restart"),
                ("boot_clear", "void F() noexcept { GraphicsBackend.Clear(); }",
                    "BootCVar is fixed at boot; set it in a config file or with --set and restart"),
                ("boot_cheat", ("BootCVar<int32> X = cvar::Boot(\"game.x\", 1).Help(\"x\").Cheat();"),
                    "no member named 'Cheat'"),
                ("boot_clamp", ("BootCVar<int32> X = cvar::Boot(\"game.x\", 1).Help(\"x\").Range(0, 4).Clamp();"),
                    "no member named 'Clamp'"),
                ("heap_new", "CVar<int32>* F() noexcept { return new CVar<int32>(cvar::Var(\"game.x\", 1).Help(\"x\")); }",
                    "operator new"),
                ("heap_new_boot", "BootCVar<int32>* F() noexcept { return new BootCVar<int32>(cvar::Boot(\"game.x\", 1).Help(\"x\")); }",
                    "operator new"),
                ("placement_new", "void F(void* storage) noexcept { new (storage) CVar<int32>(cvar::Var(\"game.x\", 1).Help(\"x\")); }",
                    "operator new"),
                ("name_too_long", ("CVar<int32> X = cvar::Var(\"game." + new string('x', 130) + "\", 1).Help(\"x\");"),
                    "CVarError_TextTooLong"),
                ("help_nul", ("CVar<int32> X = cvar::Var(\"game.x\", 1).Help(StringView(\"a\\0b\", 3));"),
                    "CVarError_InvalidUtf8"),
                ("help_invalid_utf8", ("CVar<int32> X = cvar::Var(\"game.x\", 1).Help(\"bad \\xff\");"),
                    "CVarError_InvalidUtf8"),
                ("string_default_invalid_utf8", ("CVar<String> X = cvar::Var(\"game.x\", \"\\xff\").Help(\"x\");"),
                    "CVarError_InvalidUtf8"),
                ("name_default_not_ascii", ("CVar<Name> X = cvar::Var<Name>(\"game.x\", \"caf\\xc3\\xa9\").Help(\"x\");"),
                    "CVarError_NameDefaultNotAscii"),
                ("default_nan", ("CVar<float> X = cvar::Var(\"game.x\", __builtin_nanf(\"\")).Help(\"x\");"),
                    "CVarError_DefaultOutOfRange"),
                ("default_infinity", ("CVar<double> X = cvar::Var(\"game.x\", __builtin_inf()).Help(\"x\");"),
                    "CVarError_DefaultOutOfRange"),
                ("scoped_override_boot", "void F() noexcept { cvar::ScopedOverride scoped{GraphicsBackend, \"metal\"}; }",
                    "BootCVar is fixed at boot; set it in a config file or with --set and restart"),
                ("preset_type_mismatch", "CVarGroup G = cvar::Group(\"quality.g\", 0).Help(\"g\").Level(0, {{ShadowDistance, ShadowQuality::Low}});",
                    "Preset entry value type must match the CVar type"),
                ("preset_boot", "CVarGroup G = cvar::Group(\"quality.g\", 0).Help(\"g\").Level(0, {{GraphicsBackend, \"metal\"}});",
                    "BootCVar is fixed at boot; set it in a config file or with --set and restart"),
                ("preset_duplicate_level", ("CVarGroup G = cvar::Group(\"quality.g\", 0).Help(\"g\")" +
                    ".Level(0, {{ShadowDistance, 1000}}).Level(0, {{ShadowDistance, 2000}});"), "CVarError_DuplicateLevel"),
                ("preset_duplicate_entry", ("CVarGroup G = cvar::Group(\"quality.g\", 0).Help(\"g\")" +
                    ".Level(0, {{ShadowDistance, 1000}, {ShadowDistance, 2000}});"), "CVarError_DuplicateEntry"),
                ("preset_default_undefined", "CVarGroup G = cvar::Group(\"quality.g\", 5).Help(\"g\").Level(0).Level(1);",
                    "CVarError_DefaultLevelUndefined"),
                ("preset_too_many_levels", ("CVarGroup G = cvar::Group(\"quality.g\", 0).Help(\"g\")" +
                    ".Level(0).Level(1).Level(2).Level(3).Level(4).Level(5).Level(6).Level(7).Level(8);"), "CVarError_TooManyLevels"),
                ("preset_too_many_entries", ("CVarGroup G = cvar::Group(\"quality.g\", 0).Help(\"g\").Level(0, {" +
                    string.Join(", ", Enumerable.Range(0, 33).Select(index => "{ShadowDistance, " + (1000 + index) + "}")) + "});"),
                    "A preset level holds at most MaxEntriesPerLevel entries"),
                ("preset_missing_help", "CVarGroup G = cvar::Group(\"quality.g\", 0).Level(0);", "CVarError_MissingHelp"),
                ("preset_auto_builder", "void F() noexcept { auto builder = cvar::Group(\"quality.g\", 0).Help(\"g\"); (void)builder; }",
                    "deleted"),
                ("register_non_handle", "Result<void> F(config::ConfigContext& config) noexcept { int32 value = 1; return config.Register(value); }",
                    "ConfigContext::Register takes CVar, BootCVar or CVarGroup handles (Config/CVar.h)"),
            ];

            // 서로 독립인 컴파일이라 함께 돌린다.
            Task<ProcessResult> control = Compile("valid", valid);
            Task<ProcessResult>[] failures = rejected.Select(item => Compile(item.Name, item.Source)).ToArray();
            ProcessResult accepted = await control;
            Verify.Equal(0, accepted.ExitCode, "CVar compiler/include control: " + accepted.StdOut + accepted.StdErr);
            for (int index = 0; index < rejected.Length; ++index)
            {
                ProcessResult result = await failures[index];
                string output = result.StdOut + result.StdErr;
                Verify.That(result.ExitCode != 0, rejected[index].Name + " compiled but must be rejected");
                Verify.Contains(rejected[index].Diagnostic, output, rejected[index].Name + " diagnostic");
            }
        });
    }
}
