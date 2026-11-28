using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

// meta 시험 중 생성 setter·getter와 코드 통지(ROCKS_SETTER·ROCKS_GETTER·Apply·NotifyId·SameValue — Docs/Design/ChangeNotification.ko.md CN-D9·CN-D13·
// CN-D15)다: 생성 정의(같은 값 생략·Notify 필드의 대입 전 검사·Apply·통지, EditorOnly 필드 setter의 조건 블록), 생성 헤더의 NotifyId·HasSetter·
// struct SameValue(NATIVE 멤버 struct는 늘 다름), 생성 코드 컴파일, 거부 사례(RMC2903 — 대상·자리·종류·중복, RMC2904 — Apply, RMC2501·RMC2502 —
// 조건 블록).
internal static partial class MetaTests
{
    private const string SetterPreamble = "#include <Meta/Annotations.h>\n#include <Object/ObjectRefs.h>\n#include <Object/ObjectRefArray.h>\n" +
        "#include <Object/ObjectRefMap.h>\n#include <Object/StructRefArray.h>\n" +
        "#include <Object/Object.h>\n#include <Object/Event.h>\n#include <Core/Text/String.h>\nnamespace probe {\n";

    // setter·getter 멤버를 둔 클래스 Value의 원본이다(label은 Apply가 있는 Notify 문자열, count는 Notify 정수, plain은 통지 없는 정수).
    private static string SetterValue(string members, string extra = "", string label = "ROCKS_PROPERTY(Notify, Apply = \"Relayout\") rocks::String label;") =>
        SetterPreamble + extra + "ROCKS_CLASS()\nclass Value : public rocks::Object {\nROCKS_GENERATED_BODY()\npublic:\n" +
        "explicit Value(rocks::Allocator& allocator) noexcept : label(allocator) {}\nvoid Relayout() noexcept {}\n" + label + "\n" +
        "ROCKS_PROPERTY(Notify) rocks::int32 count = 0;\nROCKS_PROPERTY() rocks::int32 plain = 0;\n" + members + "\n};\n}\n";

    // 참조 struct(강한 참조를 담는다 — StructRefArray 원소·참조 struct 필드)다.
    private const string SetterSlot = "ROCKS_STRUCT()\nstruct Slot {\nROCKS_GENERATED_BODY()\nexplicit Slot(rocks::Allocator&) noexcept {}\n" +
        "ROCKS_PROPERTY() rocks::ObjectRef<rocks::Object> target;\n};\n";

    private static void RegisterSetters(MetaContext context)
    {
        var (suite, options, actualEnvironment, generator, compiler) = context;
        suite.Add("Meta.GeneratedSetters", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            await probe.GenerateAsync(SetterValue(
                "ROCKS_PROPERTY() rocks::ObjectRef<Value> partner;\n" +
                "ROCKS_PROPERTY(Notify) Shade shade;\n" +
                "ROCKS_PROPERTY(Notify) Cached cached;\n" +
                EditorDataOpen + "ROCKS_PROPERTY(EditorOnly) rocks::int32 note = 0;\nROCKS_SETTER(SetNote, note)\n#endif\n" +
                "ROCKS_SETTER(SetLabel, label)\nROCKS_GETTER(GetLabel, label)\nROCKS_SETTER(SetCount, count)\nROCKS_SETTER(SetPlain, plain)\n" +
                "ROCKS_SETTER(SetShade, shade)\nROCKS_GETTER(GetPartner, partner)\n" +
                "ROCKS_FUNCTION(Pure, Notify) virtual rocks::int32 Total() const noexcept { return count + plain; }",
                "ROCKS_STRUCT()\nstruct Shade {\nROCKS_GENERATED_BODY()\nROCKS_PROPERTY() float tone = 0.0f;\n" +
                EditorDataOpen + "ROCKS_PROPERTY(EditorOnly) rocks::int32 hint = 0;\n#endif\n};\n" +
                "ROCKS_STRUCT()\nstruct Cached {\nROCKS_GENERATED_BODY()\nROCKS_PROPERTY() rocks::int32 value = 0;\nROCKS_NATIVE() rocks::uint32 cache = 0;\n};\n"));
            string header = Compact(File.ReadAllText(probe.Output));
            string source = File.ReadAllText(probe.Source);
            string compact = Compact(source);
            Verify.That(header.Contains("#include<Object/ObjectSetter.h>", StringComparison.Ordinal), "generated header includes ObjectSetter.h:\n" + header);
            Verify.That(header.Contains("staticconstexprboolHasSetter=false", StringComparison.Ordinal) &&
                header.Contains("||::rocks::detail::SameMemberV<Member,&::probe::Value::label>", StringComparison.Ordinal) &&
                header.Contains("#ifROCKS_WITH_EDITOR_DATA||::rocks::detail::SameMemberV<Member,&::probe::Value::note>#endif", StringComparison.Ordinal),
                "HasSetter lists the setter fields (EditorOnly guarded):\n" + header);
            Verify.That(header.Contains("SameMemberV<Member,&::probe::Value::Total>", StringComparison.Ordinal), "NotifyId lists the Notify function");
            Verify.That(!header.Contains("SameMemberV<Member,&::probe::Value::partner>", StringComparison.Ordinal), "a field with neither Notify nor a setter is not listed");
            Verify.That(header.Contains("staticconstexprboolValueComparable=false;", StringComparison.Ordinal), "NATIVE struct is never the same");
            Verify.That(header.Contains("ValueSame<decltype(::probe::Shade::tone)>::Same(left.tone,right.tone)", StringComparison.Ordinal), "struct member comparison");
            Verify.That(compact.Contains("auto::probe::Value::SetLabel(::rocks::detail::SetterParam<decltype(::probe::Value::label)>value)noexcept->::rocks::Result<void>{", StringComparison.Ordinal),
                "setter definition:\n" + source);
            Verify.That(compact.Contains("(self.*static_cast<void(Self::*)()noexcept>(&Self::Relayout))();}}(static_cast<::probe::Value&>(object));returnAccess::templateNotify<&::probe::Value::label>(object);",
                StringComparison.Ordinal), "Apply before notify");
            Verify.That(compact.Contains("SetCount(") && compact.Contains("::rocks::Object&object=*this;usingAccess", StringComparison.Ordinal) && compact.Contains("Access::Prepare(object)", StringComparison.Ordinal), "Notify setter checks access first");
            int plainAt = compact.IndexOf("::probe::Value::SetPlain(", StringComparison.Ordinal);
            Verify.That(plainAt >= 0 && !compact[plainAt..].Split("::probe::Value::SetShade(")[0].Contains("Prepare", StringComparison.Ordinal), "a setter without Notify has no access check");
            Verify.That(compact.Contains("#ifROCKS_WITH_EDITOR_DATA//", StringComparison.Ordinal) && compact.Contains("::probe::Value::SetNote(", StringComparison.Ordinal),
                "EditorOnly setter is guarded");
            Verify.That(!compact.Contains("::probe::Value::GetLabel(", StringComparison.Ordinal), "getters have no generated definition");
            // 리플렉션 쓰기의 setter thunk(CN-D14): 생성 헤더의 SetterOf가 상자 값을 교환으로 넣고(AssignOwned) Apply·통지하며, 함수표의 끝 칸이
            // SetterOf를 부른다(setter 없는 필드는 칸이 없다).
            Verify.That(header.Contains("staticconstexprFieldSetterFunctionSetterOf()noexcept{", StringComparison.Ordinal) &&
                header.Contains("Access::templateAssignOwned<&::probe::Value::label>(object,*static_cast<decltype(::probe::Value::label)*>(value))", StringComparison.Ordinal),
                "SetterOf thunk swaps the boxed value in:\n" + header);
            Verify.That(compact.Contains("GeneratedType<::probe::Value>::templateSetterOf<&::probe::Value::label>()}", StringComparison.Ordinal),
                "the ops table carries the setter thunk");
            Verify.That(!compact.Contains("SetterOf<&::probe::Value::partner>", StringComparison.Ordinal), "a field without a setter has no setter slot");
            ProcessResult compiled = await probe.CompileGeneratedAsync();
            Verify.Equal(0, compiled.ExitCode, "generated code compiles: " + compiled.StdOut + compiled.StdErr);
        });

        // 대상·자리·종류(RMC2903): NATIVE 멤버, 상속 필드, struct 멤버, 이벤트 필드, bitfield, 강한 참조·참조 배열·참조 struct 배열·참조 Map·참조 struct
        // 필드 setter, 같은 필드의 setter 둘, 식별자가 아닌 대상, 리플렉션 클래스가 아닌 클래스.
        AddRejectGroup(context, "Meta.SetterTargets", [
            SetterValue("ROCKS_NATIVE() rocks::int32 cache = 0;\nROCKS_SETTER(SetCache, cache)"),
            SetterValue("", "ROCKS_CLASS()\nclass Base : public rocks::Object {\nROCKS_GENERATED_BODY()\npublic:\nROCKS_PROPERTY() rocks::int32 inherited = 0;\n};\n")
                .Replace("class Value : public rocks::Object", "class Value : public Base", StringComparison.Ordinal)
                .Replace("ROCKS_PROPERTY() rocks::int32 plain = 0;\n", "ROCKS_PROPERTY() rocks::int32 plain = 0;\nROCKS_SETTER(SetInherited, inherited)\n", StringComparison.Ordinal),
            SetterPreamble + "ROCKS_STRUCT()\nstruct Leaf {\nROCKS_GENERATED_BODY()\nROCKS_PROPERTY() rocks::int32 count = 0;\nROCKS_SETTER(SetCount, count)\n};\n}\n",
            SetterValue("ROCKS_PROPERTY() rocks::Event<> changed;\nROCKS_SETTER(SetChanged, changed)")
                .Replace(": label(allocator)", ": label(allocator), changed(allocator)", StringComparison.Ordinal),
            SetterValue("ROCKS_PROPERTY() rocks::uint8 flag : 1;\nROCKS_SETTER(SetFlag, flag)"),
            SetterValue("ROCKS_PROPERTY() rocks::ObjectRef<Value> partner;\nROCKS_SETTER(SetPartner, partner)"),
            SetterValue("ROCKS_PROPERTY() rocks::ObjectRefArray<Value> partners;\nROCKS_SETTER(SetPartners, partners)")
                .Replace(": label(allocator)", ": label(allocator), partners(allocator)", StringComparison.Ordinal),
            SetterValue("ROCKS_PROPERTY() rocks::StructRefArray<Slot> slots;\nROCKS_SETTER(SetSlots, slots)", SetterSlot)
                .Replace(": label(allocator)", ": label(allocator), slots(allocator)", StringComparison.Ordinal),
            SetterValue("ROCKS_PROPERTY() rocks::ObjectRefMap<rocks::int32, rocks::Object> byKey;\nROCKS_SETTER(SetByKey, byKey)")
                .Replace(": label(allocator)", ": label(allocator), byKey(allocator)", StringComparison.Ordinal),
            SetterValue("ROCKS_PROPERTY() Slot slot;\nROCKS_SETTER(SetSlot, slot)", SetterSlot)
                .Replace(": label(allocator)", ": label(allocator), slot(allocator)", StringComparison.Ordinal),
            SetterValue("ROCKS_SETTER(SetCount, count)\nROCKS_SETTER(PutCount, count)"),
            SetterValue("ROCKS_SETTER(SetCount, (count))"),
            SetterPreamble + "class Plain {\npublic:\nint count = 0;\nROCKS_SETTER(SetCount, count)\n};\n}\n"],
            "RMC2903", "ROCKS_");
        // Apply 함수의 모양은 생성 정의가 본다: 오버로드가 있어도 `void () noexcept` 하나를 고르고, 기반 클래스의 함수도 받는다. 그 모양이 없으면
        // 메타 단계의 생성 코드 검증이 RMC2904 문구의 static_assert로 거부한다(RMC3000).
        suite.Add("Meta.SetterApplyShape", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            await probe.GenerateAsync(SetterValue("void Relayout(rocks::int32) noexcept {}\nROCKS_SETTER(SetLabel, label)"));
            ProcessResult overloaded = await probe.CompileGeneratedAsync();
            Verify.Equal(0, overloaded.ExitCode, "an overloaded Apply method compiles: " + overloaded.StdOut + overloaded.StdErr);
            await probe.RejectAsync(SetterValue("ROCKS_SETTER(SetLabel, label)", label: "ROCKS_PROPERTY(Notify, Apply = \"Missing\") rocks::String label;"),
                "RMC3000", "RMC2904");
        });
        // ReadOnly 필드도 setter가 있으면 ScriptWritable일 수 있다(SparksBinding B1 — 리플렉션 쓰기가 setter로 간다, CN-D14). setter가 없으면
        // 그대로 RMC2603이다(Meta.SparksBindingRejectsConflictingSpecifiers).
        suite.Add("Meta.SetterAllowsReadOnlyScriptWritable", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            await probe.GenerateAsync(BindingThing("ROCKS_PROPERTY(ReadOnly, ScriptWritable) rocks::int32 count = 0;\nROCKS_SETTER(SetCount, count)"));
            ProcessResult compiled = await probe.CompileGeneratedAsync();
            Verify.Equal(0, compiled.ExitCode, "generated code compiles: " + compiled.StdOut + compiled.StdErr);
        });
        // Apply(RMC2904): setter 없는 필드, 식별자가 아닌 값.
        AddRejectGroup(context, "Meta.SetterApply", [
            SetterValue(""),
            SetterValue("ROCKS_SETTER(SetLabel, label)", label: "ROCKS_PROPERTY(Notify, Apply = \"Re layout\") rocks::String label;")],
            "RMC2904", "Apply");
        // 조건 블록: 다른 조건 블록 안의 setter(RMC2501), EditorOnly 필드 setter·getter가 블록 밖·보통 필드 setter·getter가 블록 안(RMC2502).
        AddRejectGroup(context, "Meta.SetterConditionalBlock", [
            SetterValue("#if 1\nROCKS_SETTER(SetCount, count)\n#endif\nROCKS_SETTER(SetLabel, label)")],
            "RMC2501", "ROCKS_SETTER");
        AddRejectGroup(context, "Meta.SetterEditorBlock", [
            SetterValue(EditorDataOpen + "ROCKS_PROPERTY(EditorOnly) rocks::int32 note = 0;\n#endif\nROCKS_SETTER(SetNote, note)\nROCKS_SETTER(SetLabel, label)"),
            SetterValue(EditorDataOpen + "ROCKS_SETTER(SetCount, count)\n#endif\nROCKS_SETTER(SetLabel, label)"),
            SetterValue(EditorDataOpen + "ROCKS_PROPERTY(EditorOnly) rocks::int32 note = 0;\n#endif\nROCKS_GETTER(GetNote, note)\nROCKS_SETTER(SetLabel, label)"),
            SetterValue(EditorDataOpen + "ROCKS_GETTER(GetCount, count)\n#endif\nROCKS_SETTER(SetLabel, label)")],
            "RMC2502", "ROCKS_");
    }
}
