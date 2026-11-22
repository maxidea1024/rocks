using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

// meta 시험 중 설정 struct·필드 설정 지정자 시험이다(Docs/Design/ConfigSystem.ko.md 7절 — 설정 CF1).
internal static partial class MetaTests
{
    // ROCKS_STRUCT(Settings(Branch, Section))가 GeneratedType<S>::Settings()를 내고, 설정 지정자가 있는 직접 멤버만 스키마 순서로 SettingsFieldFlags
    // 비트(UserConfig 1, RestartRequired 2, StartupOnly 4, RenderRead 8, DevelopmentOnly 16)와 콘솔 이름을 싣는다. 설정 struct가 아닌 struct는
    // Settings()가 없다. 지정자를 잘못 쓰면 RMC2210(설정 규칙)·RMC2202(필드 종류)·RMC2000(문법)이다.
    private static void RegisterSettings(MetaContext context)
    {
        var (suite, options, actualEnvironment, generator, compiler) = context;
        suite.Add("Meta.SettingsStructReachesGeneratedSettingsSchema", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            string source = Preamble +
                "ROCKS_ENUM()\nenum class Mode : rocks::uint8 { Off, On };\n" +
                "ROCKS_STRUCT()\nstruct Plain {\nROCKS_PROPERTY() rocks::int32 at = 0;\n};\n" +
                "ROCKS_STRUCT(Settings(Branch = \"Render\", Section = \"RenderGraph\"), Version = 2)\nstruct Leaf {\n" +
                "ROCKS_PROPERTY(ConsoleName = \"render.graph.immediate\", RenderRead) bool tiny = false;\n" +
                "ROCKS_PROPERTY() rocks::int32 count = 0;\n" +
                "ROCKS_PROPERTY(UserConfig, RestartRequired) rocks::int64 weight = 0;\n" +
                "ROCKS_PROPERTY(StartupOnly, DevelopmentOnly, ConsoleName = \"render.graph.mode\") Mode mode = Mode::Off;\n" +
                "ROCKS_PROPERTY() Plain named;\n};\n" +
                "ROCKS_STRUCT(Settings(Section = \"Empty\", Branch = \"Game\"))\nstruct Box {\nROCKS_PROPERTY() rocks::int32 count = 0;\n};\n" +
                "ROCKS_CLASS()\nstruct Value : public rocks::Object {\nROCKS_PROPERTY() rocks::int32 count = 0;\n};\n}\n";
            await probe.GenerateAsync(source);
            string generated = Compact(probe.GeneratedText());
            foreach ((string expected, string reason) in new[]
            {
                ("#include <Meta/SettingsSchema.h>", "settings schema include"),
                (", 8u, Span<const char>(\"render.graph.immediate\", 22)},", "console name with RenderRead"),
                (", 3u, Span<const char>(\"\", 0)},", "UserConfig with RestartRequired"),
                (", 20u, Span<const char>(\"render.graph.mode\", 17)},", "StartupOnly with DevelopmentOnly"),
                ("static const SettingsSchema settings{Span<const char>(\"Render\", 6), Span<const char>(\"RenderGraph\", 11), Span<const SettingsFieldSchema>(settingsFields, 3)};",
                    "settings struct schema"),
                ("static const SettingsSchema settings{Span<const char>(\"Game\", 4), Span<const char>(\"Empty\", 5), Span<const SettingsFieldSchema>{}};",
                    "settings struct without field specifiers")
            })
            {
                Verify.That(generated.Contains(Compact(expected), StringComparison.Ordinal), "generated code missing " + reason + ": " + expected);
            }

            // 설정 struct가 아닌 struct(Plain)에는 Settings()가 없다: Settings() 정의는 설정 struct 둘뿐이다.
            Verify.Equal(2, generated.Split(Compact("static ROCKS_PROBE_API const SettingsSchema& Settings() noexcept"), StringSplitOptions.None).Length - 1,
                "only settings structs get Settings()");
            ProcessResult compiled = await probe.CompileGeneratedAsync();
            Verify.Equal(0, compiled.ExitCode, "generated settings header compiles: " + compiled.StdOut + compiled.StdErr);
        });
        // 시험 원본의 타입·필드 이름은 probe 명세(MetaProbeIdentity)에 등록된 것만 쓴다(Leaf·Plain과 그 필드).
        string SettingsStruct(string specifiers, string member) =>
            Preamble + "ROCKS_STRUCT()\nstruct Plain {\nROCKS_PROPERTY() rocks::int32 at = 0;\n};\n" +
            $"ROCKS_STRUCT({specifiers})\nstruct Leaf {{\n{member}\n}};\n}}\n";
        AddRejectGroup(context, "Meta.SettingsSpecifierShapeIsChecked", [
            SettingsStruct("Settings(Branch = \"Render\")", "ROCKS_PROPERTY() rocks::int32 count = 0;"),
            SettingsStruct("Settings(Section = \"Graph\")", "ROCKS_PROPERTY() rocks::int32 count = 0;"),
            SettingsStruct("Settings(Branch = \"Render\", Section = \"Settings\")", "ROCKS_PROPERTY() rocks::int32 count = 0;"),
            SettingsStruct("Settings(Branch = \"Ren der\", Section = \"Graph\")", "ROCKS_PROPERTY() rocks::int32 count = 0;"),
            SettingsStruct("Settings(Branch = \"Render\", Section = \"Graph\", Version = 2)", "ROCKS_PROPERTY() rocks::int32 count = 0;"),
            SettingsStruct("Settings(Branch = \"Render\", Section = \"Graph\")", "ROCKS_PROPERTY(ConsoleName = \"Render.Graph\") rocks::int32 count = 0;"),
            SettingsStruct("Settings(Branch = \"Render\", Section = \"Graph\")", "ROCKS_PROPERTY(ConsoleName = \"render\") rocks::int32 count = 0;"),
            SettingsStruct("Settings(Branch = \"Render\", Section = \"Graph\")", "ROCKS_PROPERTY(RestartRequired, StartupOnly) rocks::int32 count = 0;"),
            SettingsStruct("Settings(Branch = \"Render\", Section = \"Graph\")",
                "ROCKS_PROPERTY(ConsoleName = \"render.graph.a\") rocks::int32 count = 0;\nROCKS_PROPERTY(ConsoleName = \"render.graph.a\") rocks::int32 weight = 0;"),
            SettingsStruct("Settings(Branch = \"Render\", Section = \"Graph\")", "ROCKS_PROPERTY() rocks::ObjectRef<rocks::Object> target;"),
            SettingsStruct("", "ROCKS_PROPERTY(UserConfig) rocks::int32 count = 0;"),
            Declaration("ROCKS_PROPERTY(RenderRead) rocks::int32 count = 0;")],
            "RMC2210", "");
        AddRejectGroup(context, "Meta.SettingsConsoleFieldKindIsChecked", [
            SettingsStruct("Settings(Branch = \"Render\", Section = \"Graph\")", "ROCKS_PROPERTY(ConsoleName = \"render.graph.list\") Plain named;"),
            SettingsStruct("Settings(Branch = \"Render\", Section = \"Graph\")", "ROCKS_PROPERTY(RenderRead) rocks::uint32 count = 0;")],
            "RMC2202", "ConsoleName and RenderRead apply only");
        AddRejectGroup(context, "Meta.SettingsSpecifierSyntaxIsChecked", [
            SettingsStruct("Settings", "ROCKS_PROPERTY() rocks::int32 count = 0;"),
            SettingsStruct("Settings = \"Render\"", "ROCKS_PROPERTY() rocks::int32 count = 0;"),
            SettingsStruct("Settings(Branch = \"Render\", Section = \"Graph\", Nested(Value = 1))", "ROCKS_PROPERTY() rocks::int32 count = 0;"),
            SettingsStruct("Atomic(Branch = \"Render\")", "ROCKS_PROPERTY() rocks::int32 count = 0;"),
            Preamble + "ROCKS_CLASS(Settings(Branch = \"Render\", Section = \"Graph\"))\nstruct Value : public rocks::Object {\nROCKS_PROPERTY() rocks::int32 count = 0;\n};\n}\n",
            Declaration("ROCKS_PROPERTY(Category(Name = \"x\")) rocks::int32 count = 0;")],
            "RMC2000", "");
    }
}
