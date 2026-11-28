using System.Text.Json;
using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

// meta 시험 중 변경 통지 지정자(ROCKS_PROPERTY(Notify)·ROCKS_FUNCTION(Notify) — Docs/Design/ChangeNotification.ko.md CN-D1)다: 생성 schema의
// FieldFlags::Notify(512)·FunctionFlags::Notify(32), descriptor schema 9의 Notify flag, 생성 코드 컴파일, 거부 사례(RMC2901 — struct 멤버·이벤트·
// EditorOnly 필드, RMC2902 — Pure·const가 아니거나 static·매개변수·EditorOnly 함수).
internal static partial class MetaTests
{
    private const string NotifyPreamble = "#include <Meta/Annotations.h>\n#include <Object/ObjectRefs.h>\n#include <Object/Object.h>\n" +
        "#include <Object/Event.h>\n#include <Core/Text/String.h>\nnamespace probe {\n";

    // Notify 멤버를 둔 클래스 Value의 원본이다.
    private static string NotifyValue(string members, string extra = "") =>
        NotifyPreamble + extra + "ROCKS_CLASS()\nclass Value : public rocks::Object {\nROCKS_GENERATED_BODY()\npublic:\n" +
        "explicit Value(rocks::Allocator& allocator) noexcept : label(allocator) {}\nROCKS_PROPERTY() rocks::String label;\n" + members + "\n};\n}\n";

    private static void RegisterNotify(MetaContext context)
    {
        var (suite, options, actualEnvironment, generator, compiler) = context;
        suite.Add("Meta.NotifySpecifiers", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            await probe.GenerateAsync(NotifyValue(
                "ROCKS_PROPERTY(Notify) rocks::int32 count = 0;\n" +
                "ROCKS_PROPERTY(Notify, SaveGame) rocks::int32 next = 0;\n" +
                "ROCKS_FUNCTION(Pure, Notify) rocks::int32 Touch() const noexcept { return count; }\n" +
                "ROCKS_FUNCTION(Pure) rocks::int32 Count() const noexcept { return count; }"));
            using (JsonDocument descriptor = JsonDocument.Parse(File.ReadAllBytes(probe.Descriptor)))
            {
                Verify.Equal(10, descriptor.RootElement.GetProperty("schema").GetInt32(), "descriptor schema 10");
                JsonElement value = DescriptorType(descriptor, "probe::Value");
                JsonElement Field(string name) => value.GetProperty("fields").EnumerateArray().Single(field => field.GetProperty("name").GetString() == name);
                JsonElement Function(string name) =>
                    value.GetProperty("functions").EnumerateArray().Single(function => function.GetProperty("name").GetString() == name);
                Verify.That(Strings(Field("count").GetProperty("flags")).SequenceEqual(["Notify"]), "Notify field flag");
                Verify.That(Strings(Field("next").GetProperty("flags")).SequenceEqual(["SaveGame", "Notify"]), "Notify after other flags");
                Verify.That(Strings(Field("label").GetProperty("flags")).Length == 0, "plain field");
                Verify.That(Strings(Function("Touch").GetProperty("flags")).SequenceEqual(["Const", "Pure", "Notify"]), "Notify function flag");
                Verify.That(!Strings(Function("Count").GetProperty("flags")).Contains("Notify"), "plain function");
            }

            // 생성 schema의 flag 값: Notify 필드 512(SaveGame 64와 합 576), Notify 함수는 Const 2 + Pure 4 + Notify 32 = 38.
            string generated = Compact(probe.GeneratedText());
            Verify.That(generated.Contains("FieldKind::Value,512u", StringComparison.Ordinal), "field flags 512:\n" + generated);
            Verify.That(generated.Contains("FieldKind::Value,576u", StringComparison.Ordinal), "field flags 576");
            Verify.That(generated.Contains("Span<constchar>(\"Touch\",5),{},38u", StringComparison.Ordinal), "Touch function flags 38");
            Verify.That(generated.Contains("Span<constchar>(\"Count\",5),{},6u", StringComparison.Ordinal), "Count function flags 6");
            ProcessResult compiled = await probe.CompileGeneratedAsync();
            Verify.Equal(0, compiled.ExitCode, "generated code compiles: " + compiled.StdOut + compiled.StdErr);
        });

        // 필드 자리(RMC2901): struct 멤버, 이벤트 필드, EditorOnly 필드.
        AddRejectGroup(context, "Meta.NotifyFieldPlacement", [
            NotifyPreamble + "ROCKS_STRUCT()\nstruct Leaf {\nROCKS_PROPERTY(Notify) rocks::int32 count = 0;\n};\n}\n",
            NotifyPreamble + "ROCKS_CLASS()\nclass Value : public rocks::Object {\nROCKS_GENERATED_BODY()\npublic:\n" +
                "explicit Value(rocks::Allocator& allocator) noexcept : changed(allocator) {}\nROCKS_PROPERTY(Notify) rocks::Event<> changed;\n};\n}\n",
            NotifyValue(EditorDataOpen + "ROCKS_PROPERTY(EditorOnly, Notify) rocks::int32 count = 0;\n#endif")],
            "RMC2901", "Notify");
        // 함수 조건(RMC2902): Pure 아님, const 아님(Pure는 반환을 요구하므로 반환 있음), static, 매개변수, EditorOnly.
        AddRejectGroup(context, "Meta.NotifyFunctionShape", [
            NotifyValue("ROCKS_FUNCTION(Notify) rocks::int32 Touch() const noexcept { return 0; }"),
            NotifyValue("ROCKS_FUNCTION(Pure, Notify) rocks::int32 Touch() noexcept { return 0; }"),
            NotifyValue("ROCKS_FUNCTION(Pure, Notify) static rocks::int32 Touch() noexcept { return 0; }"),
            NotifyValue("ROCKS_FUNCTION(Pure, Notify) rocks::int32 Touch(rocks::int32 amount) const noexcept { return amount; }"),
            NotifyValue("#if ROCKS_WITH_EDITOR\nROCKS_FUNCTION(Pure, EditorOnly, Notify) rocks::int32 Touch() const noexcept { return 0; }\n#endif")],
            "RMC2902", "Notify");
    }
}
