using System.Text.Json;
using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

// meta 시험 중 리플렉션 함수(ROCKS_FUNCTION — Docs/Design/FunctionReflection.ko.md FR1) 시험이다: 받는 선언(인스턴스·static·const·Pure·
// Name·private·EditorOnly·리플렉션하지 않은 오버로드 옆·클래스 밖 정의)의 descriptor·생성 schema·thunk와 두 조합 컴파일, 거부 사례마다
// RMC2701–RMC2706·RMC2503 코드와 위치(FD14).
internal static partial class MetaTests
{
    private const string FunctionPreamble = "#include <Meta/Annotations.h>\n#include <Object/ObjectRefs.h>\n#include <Object/Object.h>\n" +
        "#include <Object/SoftObjectRef.h>\n#include <Core/Text/String.h>\n#include <Core/Text/StringView.h>\n#include <Core/Containers/Array.h>\n" +
        "#include <Core/Result.h>\nnamespace probe {\n";

    private const string EditorOpen = "#if ROCKS_WITH_EDITOR\n";

    // 함수 members를 둔 클래스 Value 하나의 원본이다(extra는 Value 앞, after는 Value 뒤 선언).
    private static string FunctionValue(string members, string extra = "", string after = "") =>
        FunctionPreamble + extra + "ROCKS_CLASS()\nclass Value : public rocks::Object {\nROCKS_GENERATED_BODY()\npublic:\nROCKS_PROPERTY() rocks::int32 count = 0;\n" +
        members + "\n};\n" + after + "}\n";

    private static JsonElement DescriptorType(JsonDocument descriptor, string name) =>
        descriptor.RootElement.GetProperty("types").EnumerateArray().Single(type => type.GetProperty("name").GetString() == name);

    private static string[] Strings(JsonElement array) => array.EnumerateArray().Select(item => item.GetString() ?? "").ToArray();

    private static void RegisterFunctions(MetaContext context)
    {
        var (suite, options, actualEnvironment, generator, compiler) = context;
        suite.Add("Meta.FunctionsGenerateSchemaAndThunks", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            await probe.GenerateAsync(FunctionValue(
                "ROCKS_FUNCTION() void Touch() noexcept;\n" +
                "ROCKS_FUNCTION(Pure) rocks::int32 Count() const noexcept { return count; }\n" +
                "ROCKS_FUNCTION(Name = \"Make\") static rocks::Result<rocks::int32> Build(rocks::int32 seed, const rocks::String& label) noexcept;\n" +
                "ROCKS_FUNCTION() rocks::Result<void> Apply(rocks::StringView text, rocks::ObjectHandle other, rocks::WeakObjectRef<Value> weak, " +
                "Mode mode, const Leaf& leaf, const rocks::Array<rocks::int32>& values, const rocks::SoftObjectRef<Value>& soft) noexcept;\n" +
                "void Other() noexcept;\nvoid Other(rocks::int32 value) noexcept;\n" +
                EditorOpen + "ROCKS_FUNCTION(EditorOnly) rocks::String Inspect() const noexcept;\n#endif\n" +
                "ROCKS_FUNCTION() void Place(const Mixed& mixed) noexcept;\n" +
                "ROCKS_FUNCTION() void Settle() noexcept(true);\n" +
                "ROCKS_FUNCTION() auto Label() const noexcept -> rocks::String;\n" +
                "ROCKS_FUNCTION() Nothing Rest() noexcept;\n" +
                "ROCKS_FUNCTION() auto Guess() const noexcept { return count; }\n" +
                "private:\nROCKS_FUNCTION() rocks::int32 Secret(rocks::int32 step, const Leaf& leaf) noexcept;",
                after: "inline void Value::Touch() noexcept { ++count; }\n",
                extra: "ROCKS_ENUM()\nenum class Mode : rocks::uint8 { Off, On };\nROCKS_STRUCT()\nstruct Leaf {\nROCKS_PROPERTY() rocks::int32 count = 0;\n};\n" +
                    "ROCKS_STRUCT()\nstruct Root {\nROCKS_PROPERTY() rocks::int32 a = 0;\n};\n" +
                    "ROCKS_STRUCT()\nstruct Mixed : public Root {\nROCKS_GENERATED_BODY()\npublic:\nROCKS_PROPERTY() rocks::int32 b = 0;\n" +
                    "private:\nROCKS_PROPERTY() rocks::int32 c = 0;\n};\nusing Nothing = void;\n"));
            using (JsonDocument descriptor = JsonDocument.Parse(File.ReadAllBytes(probe.Descriptor)))
            {
                Verify.Equal(7, descriptor.RootElement.GetProperty("schema").GetInt32(), "descriptor schema 7");
                JsonElement[] functions = [.. DescriptorType(descriptor, "probe::Value").GetProperty("functions").EnumerateArray()];
                string[] names = [.. functions.Select(function => function.GetProperty("name").GetString() ?? "")];
                Verify.That(names.SequenceEqual(["Touch", "Count", "Build", "Apply", "Inspect", "Place", "Settle", "Label", "Rest", "Guess", "Secret"]),
                    "reflected functions in declaration order (the unreflected overload set is ignored): " + string.Join(",", names));
                JsonElement Function(string name) => functions.Single(function => function.GetProperty("name").GetString() == name);
                Verify.That(Strings(Function("Touch").GetProperty("flags")).Length == 0, "Touch flags");
                Verify.That(Strings(Function("Count").GetProperty("flags")).SequenceEqual(["Const", "Pure"]), "Count flags");
                Verify.That(Strings(Function("Build").GetProperty("flags")).SequenceEqual(["Static", "ReturnsResult"]), "Build flags");
                Verify.Equal("Make", Function("Build").GetProperty("toolName").GetString(), "tool name");
                Verify.Equal("Touch", Function("Touch").GetProperty("toolName").GetString(), "default tool name");
                Verify.That(Strings(Function("Inspect").GetProperty("flags")).SequenceEqual(["Const", "EditorOnly"]), "Inspect flags");
                Verify.That(Strings(Function("Apply").GetProperty("flags")).SequenceEqual(["ReturnsResult"]), "Result<void> returns no value");
                Verify.Equal(JsonValueKind.Null, Function("Apply").GetProperty("result").ValueKind, "Result<void> has no result slot");
                Verify.Equal("Int32", Function("Build").GetProperty("result").GetProperty("value").GetProperty("kind").GetString(), "Result<T> unwraps");
                string[] kinds = [.. Function("Apply").GetProperty("parameters").EnumerateArray()
                    .Select(parameter => parameter.GetProperty("name").GetString() + ":" + parameter.GetProperty("value").GetProperty("kind").GetString())];
                Verify.That(kinds.SequenceEqual(["text:Utf8String", "other:WeakObjectReference", "weak:WeakObjectReference", "mode:Enum", "leaf:Struct",
                    "values:Array", "soft:SoftObjectReference"]), "parameter kinds: " + string.Join(",", kinds));
                Verify.That(Function("Build").GetProperty("id").GetString()!.Length == 32, "function ID");
                // noexcept(true)·끝 반환형·void typedef 반환, private 함수의 매개변수·반환(private 멤버 함수 포인터 탐침).
                Verify.That(Strings(Function("Label").GetProperty("flags")).SequenceEqual(["Const"]), "trailing return type keeps const");
                Verify.Equal("Utf8String", Function("Label").GetProperty("result").GetProperty("value").GetProperty("kind").GetString(), "trailing return type");
                Verify.Equal(JsonValueKind.Null, Function("Rest").GetProperty("result").ValueKind, "a void typedef returns nothing");
                Verify.Equal("Int32", Function("Guess").GetProperty("result").GetProperty("value").GetProperty("kind").GetString(), "a deduced return type");
                Verify.Equal(JsonValueKind.Null, Function("Settle").GetProperty("result").ValueKind, "noexcept(true)");
                Verify.Equal("Int32", Function("Secret").GetProperty("result").GetProperty("value").GetProperty("kind").GetString(), "private result");
                Verify.Equal(2, Function("Secret").GetProperty("parameters").GetArrayLength(), "private parameters");
                Verify.Equal("Struct", Function("Place").GetProperty("parameters")[0].GetProperty("value").GetProperty("kind").GetString(),
                    "a struct with a base and mixed access is a parameter");
            }

            string generated = Compact(probe.GeneratedText());
            foreach ((string expected, string reason) in new[]
            {
                ("staticROCKS_PROBE_APIResult<void>RocksInvoke_Touch(ObjectRegistry&registry,constObjectAccessContext&access,ObjectHandleself,void*frame)noexcept;", "thunk declaration"),
                ("registry.ResolveForWrite<::probe::Value>(access,self)", "non-const functions resolve for write"),
                ("registry.Resolve<::probe::Value>(access,self)", "const functions resolve for read"),
                ("registry.VerifyAccess(access)", "static functions check the access context"),
                ("f.p0.Get().View()", "StringView parameters are held as String"),
                ("detail::FrameSlot<::probe::Mixed>p0;", "frame slots keep frames standard-layout"),
                ("f.error=done.Error();", "Result failures go to the frame"),
                ("#ifROCKS_WITH_EDITOR", "EditorOnly functions are conditional"),
                ("FunctionSchema{},", "function table end marker"),
                ("ops.functions=Span<constRuntimeFunctionOps>(functions,", "runtime ops")
            })
            {
                Verify.That(generated.Contains(Compact(expected), StringComparison.Ordinal), "generated code missing " + reason + ": " + expected);
            }

            foreach (bool editor in new[] { true, false })
            {
                ProcessResult compiled = await probe.CompileGeneratedAsync(editor: editor);
                Verify.Equal(0, compiled.ExitCode, $"generated functions compile with editor {editor}: " + compiled.StdOut + compiled.StdErr);
            }
        });

        // 대상·자리·모양(RMC2701): struct 멤버, 자유 함수, 함수 템플릿, 생성자, 삭제 함수, noexcept 없음, 참조 한정자, 이름 없는 매개변수,
        // 기본 인자, 매개변수 17개.
        AddRejectGroup(context, "Meta.FunctionDeclarationShape", [
            FunctionPreamble + "ROCKS_STRUCT()\nstruct Leaf {\nROCKS_PROPERTY() rocks::int32 count = 0;\nROCKS_FUNCTION() void Touch() noexcept;\n};\n}\n",
            FunctionPreamble + "ROCKS_FUNCTION() void Touch() noexcept;\n}\n",
            FunctionValue("template <class T>\nROCKS_FUNCTION() void Touch(T value) noexcept;"),
            FunctionValue("ROCKS_FUNCTION() Value() noexcept;"),
            FunctionValue("ROCKS_FUNCTION() void Touch() noexcept = delete;"),
            FunctionValue("ROCKS_FUNCTION() void Touch();"),
            FunctionValue("ROCKS_FUNCTION() void Touch() & noexcept;"),
            FunctionValue("ROCKS_FUNCTION() void Touch(rocks::int32) noexcept;"),
            FunctionValue("ROCKS_FUNCTION() void Touch(rocks::int32 value = 1) noexcept;"),
            FunctionValue("ROCKS_FUNCTION() bool operator==(const Value& other) const noexcept;"),
            FunctionValue("ROCKS_FUNCTION() void Touch() noexcept(false);"),
            FunctionValue("ROCKS_FUNCTION() void Touch() noexcept(sizeof(int) == 4);"),
            FunctionValue("ROCKS_FUNCTION() void Touch(rocks::int32 \u00e9tape) noexcept;"),
            FunctionValue("ROCKS_FUNCTION() void Touch(" + string.Join(", ", Enumerable.Range(0, 17).Select(index => $"rocks::int32 a{index}")) + ") noexcept;"),
            FunctionPreamble + "ROCKS_CLASS()\nclass Value : public rocks::Object {\nROCKS_FUNCTION() void Touch() noexcept;\n};\n}\n"],
            "RMC2701", "");
        // 이름(RMC2702·RMC2703): 리플렉션 함수 이름의 C++ 오버로드, 대소문자 무시 겹침, 기반 클래스 함수와 겹침, 잘못된 Name.
        AddRejectGroup(context, "Meta.FunctionNameIsNotOverloaded", [
            FunctionValue("ROCKS_FUNCTION() void Touch() noexcept;\nvoid Touch(rocks::int32 value) noexcept;")],
            "RMC2702", "overloaded");
        AddRejectGroup(context, "Meta.FunctionNamesAreUnique", [
            FunctionValue("ROCKS_FUNCTION() void Touch() noexcept;\nROCKS_FUNCTION() void touch() noexcept;"),
            FunctionValue("ROCKS_FUNCTION() void Touch() noexcept;",
                after: "ROCKS_CLASS()\nclass Child : public Value {\npublic:\nROCKS_FUNCTION() void touch() noexcept;\n};\n"),
            FunctionValue("ROCKS_FUNCTION(Name = \"1bad\") void Touch() noexcept;")],
            "RMC2703", "");
        // 형식(RMC2704): 포인터·비 const 참조(출력)·rvalue 참조·참조 반환, 강한 참조와 그 컨테이너, InstancedStruct, C 배열.
        AddRejectGroup(context, "Meta.FunctionSlotTypes", [
            FunctionValue("ROCKS_FUNCTION() void Touch(rocks::int32* value) noexcept;"),
            FunctionValue("ROCKS_FUNCTION() void Touch(rocks::int32& value) noexcept;"),
            FunctionValue("ROCKS_FUNCTION() void Touch(rocks::String&& value) noexcept;"),
            FunctionValue("ROCKS_FUNCTION() const rocks::String& Touch() noexcept;"),
            FunctionValue("ROCKS_FUNCTION() void Touch(rocks::ObjectRef<Value> value) noexcept;"),
            FunctionValue("ROCKS_FUNCTION() rocks::ObjectRef<Value> Touch() noexcept;"),
            FunctionValue("ROCKS_FUNCTION() void Touch(const rocks::Array<rocks::ObjectRef<Value>>& values) noexcept;"),
            FunctionValue("ROCKS_FUNCTION() rocks::StringView Touch() const noexcept;"),
            FunctionValue("ROCKS_FUNCTION() rocks::Result<rocks::StringView> Touch() const noexcept;"),
            FunctionValue("ROCKS_FUNCTION() auto Touch() noexcept -> const rocks::String&;"),
            FunctionValue("ROCKS_FUNCTION() void Touch(const rocks::Array<rocks::ObjectHandle>& handles) noexcept;")],
            "RMC2704", "");
        AddRejectGroup(context, "Meta.PureFunctionReturnsAValue", [
            FunctionValue("ROCKS_FUNCTION(Pure) void Touch() const noexcept;"),
            FunctionValue("ROCKS_FUNCTION(Pure) rocks::Result<void> Touch() const noexcept;")],
            "RMC2705", "Pure");
        // EditorOnly 지정과 #if ROCKS_WITH_EDITOR 블록의 양방향 일치(RMC2706).
        AddRejectGroup(context, "Meta.EditorOnlyFunctionMatchesTheEditorBlock", [
            FunctionValue("ROCKS_FUNCTION(EditorOnly) void Touch() noexcept;"),
            FunctionValue(EditorOpen + "ROCKS_FUNCTION() void Touch() noexcept;\n#endif")],
            "RMC2706", "ROCKS_WITH_EDITOR");
        // 어휘 스캔을 빠져나간 간접 매크로가 매개변수 형식을 바꾸면 두 조합의 함수 목록이 다르다(RMC2503).
        AddRejectGroup(context, "Meta.FunctionListMatchesInBothCombinations", [
            FunctionValue("ROCKS_FUNCTION() void Touch(PROBE_COUNT value) noexcept;",
                "#if ROCKS_WITH_EDITOR\n#define PROBE_COUNT rocks::int32\n#else\n#define PROBE_COUNT rocks::int64\n#endif\n")],
            "RMC2503", "function list");
        // 생성 static_assert가 막는 형식(RMC2704 — 메타 컴파일러의 생성 코드 검증 컴파일이 거부한다): 강한 참조를 담는 struct(struct 형질은
        // 생성 코드가 안다), 복사할 수 없는 값 매개변수(Invoke는 같은 frame으로 여러 번 부르므로 인자를 옮기지 않는다).
        const string HolderStruct = "ROCKS_CLASS()\nstruct Node : public rocks::Object {};\nROCKS_STRUCT()\nstruct Holder {\n" +
            "ROCKS_PROPERTY() rocks::ObjectRef<Node> target;\n};\n";
        AddRejectGroup(context, "Meta.FunctionSlotsCheckedByGeneratedCode", [
            FunctionValue("ROCKS_FUNCTION() void Touch(const Holder& holder) noexcept;", HolderStruct),
            FunctionValue("ROCKS_FUNCTION() void Touch(rocks::String label) noexcept;"),
            FunctionValue("ROCKS_FUNCTION() void Touch(rocks::SoftObjectRef<Value> soft) noexcept;")],
            "RMC2704", "");
    }
}
