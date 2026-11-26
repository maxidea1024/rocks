using System.Text.Json;
using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

// meta 시험 중 Sparks binding schema의 함수(Docs/Design/SparksBinding.ko.md EI1b B11–B16) 시험이다: ScriptCallable 함수의 schema 항목(이름·
// TryX → X·ScriptName·static·매개변수 표기·perSecond·반환·효과 정규화·pure·oneShot·선언 순서), descriptor schema 6의 노출 flag·scriptName,
// 거부 사례마다 RMC2601–RMC2608.
internal static partial class MetaTests
{
    // 노출 클래스 Thing의 함수 하나(members)를 둔 원본이다. Sound는 asset 대상(Soft<T>)이다.
    private static string SparksFunctionThing(string members) =>
        BindingThing(members, "ROCKS_CLASS(ScriptType)\nstruct Sound : public rocks::Asset {};\n");

    private static void RegisterSparksFunctions(MetaContext context)
    {
        var (suite, options, actualEnvironment, generator, compiler) = context;
        suite.Add("Meta.SparksBindingFunctions", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            await probe.GenerateAsync(SparksFunctionThing(
                "ROCKS_FUNCTION(ScriptCallable) void Jump() noexcept;\n" +
                "ROCKS_FUNCTION(ScriptCallable, ScriptEffects = \"io, spawn\", ScriptPerSecond = \"speed\") rocks::Result<void> TryMove(float speed, rocks::int32 steps) noexcept;\n" +
                "ROCKS_FUNCTION(Pure, ScriptCallable) rocks::Result<rocks::int32> TryCount() const noexcept;\n" +
                "ROCKS_FUNCTION(ScriptCallable, ScriptName = \"Spawn\", ScriptOneShot) static rocks::String Make(const rocks::Array<rocks::String>& names, " +
                "rocks::WeakObjectRef<Thing> owner, const rocks::SoftObjectRef<Sound>& sound) noexcept;\n" +
                "ROCKS_FUNCTION() void Hidden() noexcept;\n" +
                "ROCKS_FUNCTION(ScriptCallable) rocks::Optional<rocks::int32> Limit() const noexcept;"));
            using (JsonDocument schema = JsonDocument.Parse(File.ReadAllBytes(probe.BindingSchema)))
            {
                JsonElement thing = BindingType(schema, "Thing");
                JsonElement[] functions = [.. thing.GetProperty("functions").EnumerateArray()];
                string Shape(JsonElement function)
                {
                    Verify.Equal(32, function.GetProperty("id").GetString()!.Length, "function ID");
                    return Compact(string.Join(",", function.EnumerateObject().Where(property => property.Name != "id")
                        .Select(property => property.Name + "=" + property.Value.GetRawText())));
                }

                string[] shapes = [.. functions.Select(Shape)];
                string[] expected =
                [
                    "name=\"Jump\"",
                    "name=\"Move\",parameters=[{\"name\":\"speed\",\"type\":\"float32\",\"perSecond\":true},{\"name\":\"steps\",\"type\":\"int32\"}]," +
                        "returns=\"Result<void>\",effects=[\"spawn\",\"io\"]",
                    "name=\"Count\",returns=\"Result<int32>\",pure=true",
                    "name=\"Spawn\",static=true,parameters=[{\"name\":\"names\",\"type\":\"Array<string>\"},{\"name\":\"owner\",\"type\":\"Thing?\"}," +
                        "{\"name\":\"sound\",\"type\":\"Soft<Sound>?\"}],returns=\"string\",oneShot=true",
                    "name=\"Limit\",returns=\"int32?\""
                ];
                Verify.That(shapes.SequenceEqual(expected.Select(Compact)), "functions:\n" + string.Join("\n", shapes));
                string text = File.ReadAllText(probe.BindingSchema);
                Verify.That(text.IndexOf("\"fields\"", StringComparison.Ordinal) < 0 || text.IndexOf("\"functions\"", StringComparison.Ordinal) > 0, "functions follow fields");
            }

            using (JsonDocument descriptor = JsonDocument.Parse(File.ReadAllBytes(probe.Descriptor)))
            {
                JsonElement[] functions = [.. DescriptorType(descriptor, "probe::Thing").GetProperty("functions").EnumerateArray()];
                JsonElement Function(string name) => functions.Single(function => function.GetProperty("name").GetString() == name);
                Verify.Equal("Move", Function("TryMove").GetProperty("scriptName").GetString(), "descriptor scriptName after TryX");
                Verify.Equal("", Function("Hidden").GetProperty("scriptName").GetString(), "unexposed function");
                Verify.That(Strings(Function("TryCount").GetProperty("flags")).SequenceEqual(["Const", "Pure", "ReturnsResult", "ScriptCallable"]), "flags");
            }

            ProcessResult compiled = await probe.CompileGeneratedAsync();
            Verify.Equal(0, compiled.ExitCode, "generated code compiles: " + compiled.StdOut + compiled.StdErr);
        });

        // 부속 지정자·조합(RMC2603).
        AddRejectGroup(context, "Meta.SparksFunctionSpecifiers", [
            SparksFunctionThing("ROCKS_FUNCTION(ScriptName = \"Go\") void Act() noexcept;"),
            SparksFunctionThing("ROCKS_FUNCTION(ScriptEffects = \"io\") void Act() noexcept;"),
            SparksFunctionThing("ROCKS_FUNCTION(ScriptOneShot) void Act() noexcept;"),
            SparksFunctionThing("ROCKS_FUNCTION(Pure, ScriptCallable, ScriptEffects = \"io\") rocks::int32 Act() const noexcept;"),
            SparksFunctionThing("ROCKS_FUNCTION(Pure, ScriptCallable, ScriptOneShot) rocks::int32 Act() const noexcept;"),
            SparksFunctionThing("ROCKS_FUNCTION(ScriptCallable, ScriptEffects = \"io, io\") void Act() noexcept;"),
            SparksFunctionThing("ROCKS_FUNCTION(ScriptCallable, ScriptEffects = \"fly\") void Act() noexcept;"),
            SparksFunctionThing("ROCKS_FUNCTION(ScriptCallable, ScriptEffects = \"io,,spawn\") void Act() noexcept;"),
            SparksFunctionThing("ROCKS_FUNCTION(ScriptCallable, ScriptPerSecond = \"speedy\") void Act(float speed) noexcept;"),
            SparksFunctionThing("ROCKS_FUNCTION(ScriptCallable, ScriptPerSecond = \"speed, speed\") void Act(float speed) noexcept;")],
            "RMC2603", "");
        // 표기할 수 없는 형식(RMC2601): perSecond가 속도 형식이 아님, Result<T?>, Sparks 대응이 없는 값.
        AddRejectGroup(context, "Meta.SparksFunctionTypes", [
            SparksFunctionThing("ROCKS_FUNCTION(ScriptCallable, ScriptPerSecond = \"steps\") void Act(rocks::int32 steps) noexcept;"),
            SparksFunctionThing("ROCKS_FUNCTION(ScriptCallable) rocks::Result<rocks::WeakObjectRef<Thing>> TryAct() noexcept;"),
            SparksFunctionThing("ROCKS_FUNCTION(ScriptCallable) rocks::Result<rocks::Optional<rocks::int32>> TryAct() noexcept;"),
            SparksFunctionThing("ROCKS_FUNCTION(ScriptCallable) void Act(rocks::Guid key) noexcept;"),
            SparksFunctionThing("ROCKS_FUNCTION(ScriptCallable) rocks::Guid Act() noexcept;")],
            "RMC2601", "");
        // 노출하지 않은 대상(RMC2602): ObjectHandle(rocks::Object), 비노출 클래스·enum.
        AddRejectGroup(context, "Meta.SparksFunctionTargets", [
            SparksFunctionThing("ROCKS_FUNCTION(ScriptCallable) void Act(rocks::ObjectHandle other) noexcept;"),
            SparksFunctionThing("ROCKS_FUNCTION(ScriptCallable) void Act(rocks::WeakObjectRef<Plain> other) noexcept;"),
            SparksFunctionThing("ROCKS_FUNCTION(ScriptCallable) Secret Act() noexcept;")],
            "RMC2602", "");
        // 이름 형식(RMC2604): `_`, Result가 아닌 Try 접두, ScriptName의 Try 접두, 매개변수 대문자·`_`, ScriptName 형식.
        AddRejectGroup(context, "Meta.SparksFunctionNames", [
            SparksFunctionThing("ROCKS_FUNCTION(ScriptCallable) void Do_It() noexcept;"),
            SparksFunctionThing("ROCKS_FUNCTION(ScriptCallable) void TryAct() noexcept;"),
            SparksFunctionThing("ROCKS_FUNCTION(ScriptCallable, ScriptName = \"TryGo\") void Act() noexcept;"),
            SparksFunctionThing("ROCKS_FUNCTION(ScriptCallable) void Act(rocks::int32 Steps) noexcept;"),
            SparksFunctionThing("ROCKS_FUNCTION(ScriptCallable) void Act(rocks::int32 max_steps) noexcept;"),
            SparksFunctionThing("ROCKS_FUNCTION(ScriptCallable, ScriptName = \"go\") void Act() noexcept;")],
            "RMC2604", "");
        // 한 이름 공간(RMC2605): 필드와 함수, TryX와 X, 기반의 함수.
        AddRejectGroup(context, "Meta.SparksFunctionNameCollisions", [
            SparksFunctionThing("ROCKS_PROPERTY(ScriptReadOnly) rocks::int32 score = 0;\nROCKS_FUNCTION(ScriptCallable) void Score() noexcept;"),
            SparksFunctionThing("ROCKS_FUNCTION(ScriptCallable) rocks::Result<void> TryMove() noexcept;\nROCKS_FUNCTION(ScriptCallable, ScriptName = \"Move\") void Act() noexcept;"),
            BindingPreamble + "ROCKS_CLASS(ScriptExtensible)\nstruct Thing : public rocks::Object {\nROCKS_FUNCTION(ScriptCallable) void Act() noexcept;\n};\n" +
                "ROCKS_CLASS()\nstruct Child : public Thing {\nROCKS_FUNCTION(ScriptCallable, ScriptName = \"Act\") void Jump() noexcept;\n};\n}\n",
            SparksFunctionThing("ROCKS_PROPERTY(ScriptWritable) rocks::int64 score = 0;\nROCKS_FUNCTION(ScriptCallable) void SetScore(rocks::int64 value) noexcept;")],
            "RMC2605", "");
        // 언어 이름(RMC2606): hook 이름, 키워드 매개변수, 예약 이름(TryX → prelude 값, ScriptName 문맥 이름, 필드의 문맥 이름).
        AddRejectGroup(context, "Meta.SparksFunctionReservedNames", [
            SparksFunctionThing("ROCKS_FUNCTION(ScriptCallable) void Update() noexcept;"),
            SparksFunctionThing("ROCKS_FUNCTION(ScriptCallable) rocks::Result<rocks::int32> TryParse() noexcept;"),
            SparksFunctionThing("ROCKS_FUNCTION(ScriptCallable, ScriptName = \"World\") void Act() noexcept;"),
            SparksFunctionThing("ROCKS_PROPERTY(ScriptReadOnly) rocks::int32 time = 0;"),
            SparksFunctionThing("ROCKS_FUNCTION(ScriptCallable, ScriptName = \"Destroy\") void Act() noexcept;"),
            SparksFunctionThing("ROCKS_FUNCTION(ScriptCallable) void Act(rocks::int32 open) noexcept;")],
            "RMC2606", "");
        AddRejectGroup(context, "Meta.SparksFunctionEditorOnly", [
            SparksFunctionThing("#if ROCKS_WITH_EDITOR\nROCKS_FUNCTION(EditorOnly, ScriptCallable) void Act() noexcept;\n#endif")],
            "RMC2607", "EditorOnly");
        AddRejectGroup(context, "Meta.SparksFunctionOnHiddenType", [
            BindingPreamble + "ROCKS_CLASS()\nstruct Plain : public rocks::Object {\nROCKS_FUNCTION(ScriptCallable) void Act() noexcept;\n};\n}\n"],
            "RMC2608", "not a Sparks type");
    }
}
