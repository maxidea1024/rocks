using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

// meta 시험 중 EditorOnly 조건 블록 시험이다(Docs/Design/BuildTargets 3.4절 BD6·BS2c): 어휘 스캔(RMC2501), EditorOnly와 블록의 양방향
// 일치(RMC2502), 에디터·게임 두 조합 비교(RMC2503), 예약 --clang-arg, 조건부 생성과 두 조합 컴파일, 합집합 descriptor.
internal static partial class MetaTests
{
    private const string EditorDataOpen = "#if ROCKS_WITH_EDITOR_DATA\n";

    private const string EditorDataClose = "#endif\n";

    private static void RegisterEditorOnlyBlocks(MetaContext context)
    {
        var (suite, options, actualEnvironment, generator, compiler) = context;
        // 받는 모양: 클래스(일부·전부 EditorOnly), struct, 상속한 EditorOnly 필드. descriptor·명세는 합집합이고, 생성 코드는 EditorOnly 항목을
        // 조건으로 감싸며 두 조합 모두 컴파일된다. 주석·문자열 안의 매크로 철자, 공백·줄 이음이 있는 조건 철자는 받는다.
        suite.Add("Meta.EditorOnlyBlocksGenerateBothCombinations", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            await probe.GenerateAsync(Preamble +
                "ROCKS_STRUCT()\nstruct Slot {\nROCKS_GENERATED_BODY()\nROCKS_PROPERTY() rocks::int32 codes = 0;\n" +
                EditorDataOpen + "ROCKS_PROPERTY(EditorOnly) rocks::int32 byKey = 0;\n" + EditorDataClose + "};\n" +
                "ROCKS_CLASS()\nstruct Value : public rocks::Object {\n" +
                "ROCKS_PROPERTY() rocks::int32 count = 0;\n" +
                "#  if   ROCKS_WITH_EDITOR_DATA // ROCKS_CLASS in a comment\n" +
                "ROCKS_PROPERTY(EditorOnly) rocks::int32 hidden = 0; /* ROCKS_ENUM */\n" +
                "ROCKS_PROPERTY(EditorOnly) Slot extra;\n" +
                "#endif\n" +
                "ROCKS_PROPERTY() Slot slot;\n" +
                "static constexpr const char* Text = \"ROCKS_STRUCT inside a string\";\n};\n" +
                "ROCKS_CLASS()\nstruct Child : public Value {\n" +
                "#if \\\n ROCKS_WITH_EDITOR_DATA\n" +
                "ROCKS_PROPERTY(EditorOnly, Name=\"Tag\") rocks::int32 extra = 0;\n" + EditorDataClose + "};\n" +
                "ROCKS_CLASS()\nstruct Other : public rocks::Object {\n" +
                EditorDataOpen + "ROCKS_PROPERTY(EditorOnly) rocks::int32 value = 0;\n" + EditorDataClose + "};\n}\n");
            using System.Text.Json.JsonDocument descriptor = System.Text.Json.JsonDocument.Parse(File.ReadAllBytes(probe.Descriptor));
            string[] Fields(string type) => descriptor.RootElement.GetProperty("types").EnumerateArray()
                .Single(item => item.GetProperty("name").GetString() == type).GetProperty("fields").EnumerateArray()
                .Select(field => field.GetProperty("name").GetString() ?? "").Order(StringComparer.Ordinal).ToArray();
            Verify.That(Fields("probe::Value").SequenceEqual(["count", "extra", "hidden", "slot"]), "union descriptor keeps EditorOnly fields");
            Verify.That(Fields("probe::Other").SequenceEqual(["value"]), "a class with only EditorOnly fields");
            string generated = Compact(probe.GeneratedText());
            Verify.Contains("#ifROCKS_WITH_EDITOR_DATA", generated, "EditorOnly entries are conditional");
            Verify.Contains("editorOnlyFieldIds", generated, "EditorOnly field ID list");
            Verify.Contains("ROCKS_WITH_EDITOR_DATA?4:2", generated, "field table length depends on the combination");
            Verify.Contains("#elseSpan<constFieldSchema>{}", generated, "a class with only EditorOnly fields has an empty table without editor data");
            foreach (bool editor in new[] { true, false })
            {
                ProcessResult compiled = await probe.CompileGeneratedAsync(editor: editor);
                Verify.Equal(0, compiled.ExitCode, $"generated header compiles with editor data {editor}: " + compiled.StdOut + compiled.StdErr);
            }
        });
        // 블록 안에 둘 수 없는 선언·조건(RMC2501): 다른 조건 철자, 정의 여부, 간접 매크로, #else·#elif 가지, 중첩, 서버 코드, 타입·열거자·
        // NATIVE·GENERATED_BODY 선언, 에디터 데이터 블록 안 FUNCTION.
        AddRejectGroup(context, "Meta.ReflectionDeclarationsOutsideConditionalBlocks", [
            Declaration("#ifdef ROCKS_WITH_EDITOR_DATA\nROCKS_PROPERTY(EditorOnly) rocks::int32 hidden = 0;\n#endif"),
            Declaration("#if defined(ROCKS_WITH_EDITOR_DATA)\nROCKS_PROPERTY(EditorOnly) rocks::int32 hidden = 0;\n#endif"),
            Declaration("#if ROCKS_WITH_EDITOR_DATA && 1\nROCKS_PROPERTY(EditorOnly) rocks::int32 hidden = 0;\n#endif"),
            Declaration("#define PROBE_EDITOR ROCKS_WITH_EDITOR_DATA\n#if PROBE_EDITOR\nROCKS_PROPERTY(EditorOnly) rocks::int32 hidden = 0;\n#endif"),
            Declaration("#if ROCKS_WITH_EDITOR_DATA\n#else\nROCKS_PROPERTY() rocks::int32 hidden = 0;\n#endif"),
            Declaration("#if 0\n#elif ROCKS_WITH_EDITOR_DATA\nROCKS_PROPERTY(EditorOnly) rocks::int32 hidden = 0;\n#endif"),
            Declaration("#if ROCKS_WITH_EDITOR_DATA\n#if 1\nROCKS_PROPERTY(EditorOnly) rocks::int32 hidden = 0;\n#endif\n#endif"),
            Declaration("#if ROCKS_WITH_SERVER_CODE\nROCKS_PROPERTY() rocks::int32 hidden = 0;\n#endif"),
            Declaration("#if ROCKS_WITH_EDITOR_DATA\nROCKS_NATIVE() rocks::int32 cache = 0;\n#endif"),
            Declaration("#if ROCKS_WITH_EDITOR_DATA\nROCKS_FUNCTION(EditorOnly) void Touch();\n#endif"),
            Preamble + "#if ROCKS_WITH_EDITOR_DATA\nROCKS_CLASS()\nstruct Value : public rocks::Object {};\n#endif\n}\n",
            Preamble + "#if ROCKS_WITH_EDITOR_DATA\nROCKS_ENUM()\nenum class Mode : rocks::uint8 { Off = 0 };\n#endif\n}\n",
            Preamble + "ROCKS_ENUM()\nenum class Mode : rocks::uint8 {\nOff = 0,\n#if ROCKS_WITH_EDITOR_DATA\nROCKS_ENUMERATOR(Hidden) Tool = 1,\n#endif\n};\n}\n",
            Preamble + "ROCKS_STRUCT()\nstruct Slot {\n#if ROCKS_WITH_EDITOR_DATA\nROCKS_GENERATED_BODY()\n#endif\n" +
                "ROCKS_PROPERTY() rocks::int32 codes = 0;\n};\n}\n"],
            "RMC2501", "inside a preprocessor conditional block");
        // 양방향 일치(RMC2502): 블록 밖 EditorOnly, 블록 안 EditorOnly 아닌 필드.
        AddRejectGroup(context, "Meta.EditorOnlyMatchesTheEditorDataBlock", [
            Declaration("ROCKS_PROPERTY(EditorOnly) rocks::int32 hidden = 0;"),
            Declaration(EditorDataOpen + "ROCKS_PROPERTY() rocks::int32 hidden = 0;\n" + EditorDataClose)],
            "RMC2502", "ROCKS_WITH_EDITOR_DATA");
        // 두 조합 비교(RMC2503): 어휘 스캔을 빠져나간 간접 매크로가 필드 형식·base를 바꾸거나 게임 조합이 컴파일되지 않는다.
        AddRejectGroup(context, "Meta.EditorAndGameCombinationsMatch", [
            Preamble + "#if ROCKS_WITH_EDITOR_DATA\n#define PROBE_COUNT rocks::int32\n#else\n#define PROBE_COUNT rocks::int64\n#endif\n" +
                "ROCKS_CLASS()\nstruct Value : public rocks::Object {\nROCKS_PROPERTY() PROBE_COUNT count = 0;\n};\n}\n",
            Preamble + "ROCKS_CLASS()\nstruct Other : public rocks::Object {};\n" +
                "#if ROCKS_WITH_EDITOR_DATA\n#define PROBE_BASE public rocks::Object\n#else\n#define PROBE_BASE public Other\n#endif\n" +
                "ROCKS_CLASS()\nstruct Value : PROBE_BASE {\nROCKS_PROPERTY() rocks::int32 count = 0;\n};\n}\n",
            Preamble + "ROCKS_CLASS()\nstruct Value : public rocks::Object {\n" + EditorDataOpen + "ROCKS_PROPERTY(EditorOnly) rocks::int32 hidden = 0;\n" +
                EditorDataClose + "rocks::int32 Hidden() const noexcept { return hidden; }\n};\n}\n"],
            "RMC2503", "editor");
        // 타깃 매크로는 메타 컴파일러가 정한다: --clang-arg로 넘기면 RMC1000이다.
        suite.Add("Meta.TargetMacrosAreReserved", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            foreach (string argument in new[] { "-DROCKS_WITH_EDITOR=1", "-DROCKS_WITH_EDITOR_DATA=0", "-DROCKS_WITH_CLIENT_CODE=1" })
            {
                await probe.GenerateAsync(Declaration(), extra: ["--clang-arg=" + argument], expectedCode: "RMC1000",
                    reason: "sets the target macros itself");
            }
        });
    }
}
