using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

// meta 시험 중 경로 thunk 시험이다(Docs/Design/ReflectionFoundations.ko.md 7.1절).
internal static partial class MetaTests
{
    // 경로 thunk 시험이다(B6b — 클래스·struct 멤버 thunk, Binds 미러 thunk, 대상 종류 불일치).
    private static void RegisterPathThunks(MetaContext context)
    {
        var (suite, options, actualEnvironment, generator, compiler) = context;
        // B6b: 클래스 Struct·Array 필드의 함수표는 valueNativeType(없으면 nullptr 명시) 뒤에 PathClassField 경로 thunk를, 모든 struct
        // 멤버는 StructFieldOps의 둘째 인자로 PathStructMember를 내며 대상 종류는 TargetKind(배열이면 원소 종류)다. 단일 값 클래스
        // 필드도 path를 싣는다(리플렉션 B9i3c0 — Docs/Design/ReflectionExtensions.ko.md 10.2절).
        suite.Add("Meta.PathThunksReachStructAndArrayFields", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            await probe.GenerateAsync(ArraySource);
            string generated = Compact(probe.GeneratedText());
            const string lambda = "+[](const ::rocks::RuntimeFieldOps&, ::rocks::detail::PathCursor& cursor) noexcept -> ::rocks::Result<void> { return ::rocks::detail::PathClassField<";
            foreach ((string expected, string reason) in new[]
            {
                ("#include <Object/GeneratedPathAccess.h>", "generated header includes the path thunk templates"),
                ("&::probe::Value::ints, field, context); }, nullptr, " + lambda + "::probe::Value, ::rocks::ValueKind::Int32>(cursor, &::probe::Value::ints); } }",
                    "array without an Enum/Struct element spells nullptr before its path thunk"),
                ("NativeTypeKey<::probe::Mode>(), " + lambda + "::probe::Value, ::rocks::ValueKind::Enum>(cursor, &::probe::Value::modes); } }",
                    "enum element array path thunk follows the element native key"),
                (lambda + "::probe::Value, ::rocks::ValueKind::Utf8String>(cursor, &::probe::Value::names)", "string element target kind"),
                (lambda + "::probe::Value, ::rocks::ValueKind::SoftObjectReference>(cursor, &::probe::Value::softs)", "soft reference element target kind"),
                ("NativeTypeKey<::probe::Leaf>(), " + lambda + "::probe::Value, ::rocks::ValueKind::Struct>(cursor, &::probe::Value::leaves)", "Array<S> target kind"),
                (lambda + "::probe::Value, ::rocks::ValueKind::ObjectReference>(cursor, &::probe::Value::refs)", "ObjectRefArray target kind"),
                (lambda + "::probe::Value, ::rocks::ValueKind::ObjectReference>(cursor, &::probe::Value::anyRefs)", "ObjectRefArray<> target kind"),
                ("NativeTypeKey<::probe::Holder>(), " + lambda + "::probe::Value, ::rocks::ValueKind::Struct>(cursor, &::probe::Value::holders)", "StructRefArray target kind"),
                ("NativeTypeKey<::probe::Bundle>(), " + lambda + "::probe::Value, ::rocks::ValueKind::Struct>(cursor, &::probe::Value::bundle)", "struct field target kind"),
                (lambda + "::probe::WorldValue, ::rocks::ValueKind::Struct>(cursor, &::probe::WorldValue::bundle)", "owner is the class that emits the table"),
                ("detail::StructFieldOps<::probe::Leaf,&::probe::Leaf::count>(", "member codec is unchanged"),
                ("&detail::PathStructMember<::probe::Leaf, &::probe::Leaf::count, ValueKind::UInt32>)", "scalar member path thunk"),
                ("&detail::PathStructMember<::probe::Leaf, &::probe::Leaf::codes, ValueKind::UInt16>)", "array member path thunk uses the element kind"),
                ("&detail::PathStructMember<::probe::Holder, &::probe::Holder::target, ValueKind::ObjectReference>)", "reference member path thunk"),
                ("&detail::PathStructMember<::probe::Holder, &::probe::Holder::weights, ValueKind::Float32>)", "float array member path thunk"),
                ("&detail::PathStructMember<::probe::Bundle, &::probe::Bundle::refs, ValueKind::ObjectReference>)", "ObjectRefArray member path thunk"),
                ("&detail::PathStructMember<::probe::Bundle, &::probe::Bundle::holders, ValueKind::Struct>)", "StructRefArray member path thunk"),
                ("&detail::PathStructMember<::probe::WorldBundle, &::probe::WorldBundle::inner, ValueKind::Struct>)", "struct member path thunk")
            })
            {
                Verify.That(generated.Contains(Compact(expected), StringComparison.Ordinal), "generated code missing " + reason + ": " + expected);
            }

            int Count(string text, string needle) => (text.Length - text.Replace(needle, "", StringComparison.Ordinal).Length) / needle.Length;
            Verify.Equal(9, Count(generated, "PathClassField<::probe::Value,"), "every Value array and struct field has a path thunk");
            Verify.Equal(1, Count(generated, "PathClassField<::probe::WorldValue,"), "WorldValue struct field has a path thunk");
            Verify.Equal(Count(generated, "detail::StructFieldOps<"), Count(generated, "&detail::PathStructMember<"), "every struct member has a path thunk");
            Verify.Equal(7, Count(generated, "&detail::PathStructMember<"), "struct member count");
            Verify.That(!generated.Contains("PathStructMember<::probe::WorldBundle,&::probe::WorldBundle::lease", StringComparison.Ordinal), "NATIVE members have no path thunk");

            // 단일 값 클래스 필드(plain)도 valueNativeType(nullptr) 뒤에 path thunk를 싣고(리플렉션 B9i3c0 — staged·Detach·Attach용)
            // private 멤버·enum·String·EditorOnly 멤버에도 thunk가 나온다.
            await probe.GenerateAsync(StructSource);
            string structs = Compact(probe.GeneratedText());
            foreach ((string expected, string reason) in new[]
            {
                ("&::probe::Value::plain, field, context); }, nullptr, " + lambda + "::probe::Value, ::rocks::ValueKind::UInt32>(cursor, &::probe::Value::plain); } }",
                    "scalar class field spells nullptr before its path thunk"),
                (lambda + "::probe::Value, ::rocks::ValueKind::Struct>(cursor, &::probe::Value::holder)", "struct field path thunk"),
                (lambda + "::probe::Value, ::rocks::ValueKind::Struct>(cursor, &::probe::Value::extra)", "EditorOnly struct field path thunk"),
                (lambda + "::probe::WorldValue, ::rocks::ValueKind::Struct>(cursor, &::probe::WorldValue::bits)", "WorldOnly class struct field path thunk"),
                ("&detail::PathStructMember<::probe::Holder, &::probe::Holder::flag, ValueKind::UInt8>)", "private member path thunk through GENERATED_BODY"),
                ("&detail::PathStructMember<::probe::Leaf, &::probe::Leaf::mode, ValueKind::Enum>)", "enum member path thunk"),
                ("&detail::PathStructMember<::probe::Leaf, &::probe::Leaf::label, ValueKind::Utf8String>)", "string member path thunk"),
                ("&detail::PathStructMember<::probe::Middle, &::probe::Middle::offset, ValueKind::Int64>)", "EditorOnly member path thunk"),
                ("&detail::PathStructMember<::probe::Middle, &::probe::Middle::leaf, ValueKind::Struct>)", "nested struct member path thunk"),
                ("&detail::PathStructMember<::probe::Holder, &::probe::Holder::target, ValueKind::ObjectReference>)", "Required reference member path thunk")
            })
            {
                Verify.That(structs.Contains(Compact(expected), StringComparison.Ordinal), "generated code missing " + reason + ": " + expected);
            }

            Verify.Equal(3, Count(structs, "PathClassField<::probe::Value,"), "every field of Value has a path thunk");
            Verify.That(!structs.Contains("PathStructMember<::probe::Middle,&::probe::Middle::cache", StringComparison.Ordinal), "NATIVE struct member has no path thunk");
            Verify.Equal(Count(structs, "detail::StructFieldOps<"), Count(structs, "&detail::PathStructMember<"), "every struct member has a path thunk");
        });
        // Binds 미러 멤버의 path thunk는 미러가 아니라 실제 타입의 멤버 포인터를 쓴다(멤버가 다른 미러인 struct 멤버 포함).
        suite.Add("Meta.PathThunksOfBindsMirrorsUseTheRealType", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler, BindsModule);
            string source = "#include <Core/Containers/Array.h>\n" + BindsPreamble + BindsVec3 +
                "ROCKS_STRUCT(Binds = \"rocks::Vector3<double>\")\nstruct Vec3dBinding {\nROCKS_PROPERTY() double x = 0;\nROCKS_PROPERTY() double y = 0;\nROCKS_PROPERTY() double z = 0;\n};\n" +
                "struct Pair {\nrocks::Vec3 a;\nrocks::Vec3d b;\n};\n" +
                "ROCKS_STRUCT(Binds = \"probe::Pair\")\nstruct PairBinding {\nROCKS_PROPERTY() rocks::Vec3 a;\nROCKS_PROPERTY() rocks::Vector3<double> b;\n};\n" +
                "ROCKS_CLASS()\nstruct Value : public rocks::Object {\nexplicit Value(rocks::Allocator& allocator) noexcept : points(allocator) {}\n" +
                "ROCKS_PROPERTY() rocks::Vec3 position;\nROCKS_PROPERTY() Pair pair;\nROCKS_PROPERTY() rocks::Array<rocks::Vec3> points;\n};\n}\n" +
                "template <> inline constexpr bool rocks::BoundCoreValueV<probe::Pair> = true;\n";
            await probe.GenerateAsync(source);
            string generated = Compact(probe.GeneratedText());
            foreach ((string expected, string reason) in new[]
            {
                ("&detail::PathStructMember<::rocks::Vector3<float>, &::rocks::Vector3<float>::x, ValueKind::Float32>)", "mirror member of the real type"),
                ("&detail::PathStructMember<::rocks::Vector3<double>, &::rocks::Vector3<double>::z, ValueKind::Float64>)", "template mirror member of the real type"),
                ("&detail::PathStructMember<::probe::Pair, &::probe::Pair::a, ValueKind::Struct>)", "mirror member that is another mirror"),
                ("&detail::PathStructMember<::probe::Pair, &::probe::Pair::b, ValueKind::Struct>)", "mirror member spelled as the real template"),
                ("NativeTypeKey<::rocks::Vector3<float>>(), +[](const ::rocks::RuntimeFieldOps&, ::rocks::detail::PathCursor& cursor) noexcept -> ::rocks::Result<void> { " +
                    "return ::rocks::detail::PathClassField<::probe::Value, ::rocks::ValueKind::Struct>(cursor, &::probe::Value::position); }",
                    "class field of a Core value type"),
                ("::rocks::detail::PathClassField<::probe::Value, ::rocks::ValueKind::Struct>(cursor, &::probe::Value::pair)", "class field of a Binds probe type"),
                ("::rocks::detail::PathClassField<::probe::Value, ::rocks::ValueKind::Struct>(cursor, &::probe::Value::points)", "array of a Core value type")
            })
            {
                Verify.That(generated.Contains(Compact(expected), StringComparison.Ordinal), "generated code missing " + reason + ": " + expected);
            }

            Verify.That(!generated.Contains("&::probe::Vec3Binding::", StringComparison.Ordinal) && !generated.Contains("&::probe::Vec3dBinding::", StringComparison.Ordinal) &&
                !generated.Contains("&::probe::PairBinding::", StringComparison.Ordinal), "no member pointer of a mirror declaration");
        });
        // 대상 종류가 C++ 멤버 형식과 다르면(오래된 생성물·잘못된 descriptor) TargetValueCheck static_assert가 컴파일을 실패시킨다:
        // 생성 소스(경로 thunk는 Ops 본문에 있다)의 대상 종류를 바꾸면 클래스 배열·enum 원소 배열·struct 필드·ObjectRefArray·struct 멤버
        // 모두 거부된다.
        suite.Add("Meta.PathThunkTargetMismatchFailsToCompile", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            await probe.GenerateAsync(ArraySource);
            string original = File.ReadAllText(probe.Source);
            ProcessResult control = await probe.CompileGeneratedAsync();
            Verify.Equal(0, control.ExitCode, "generated header and source compile: " + control.StdOut + control.StdErr);
            foreach ((string anchor, bool before, string from, string to) in new[]
            {
                ("&::probe::Value::ints);", true, "ValueKind::Int32>", "ValueKind::UInt32>"),
                ("&::probe::Value::modes);", true, "ValueKind::Enum>", "ValueKind::UInt8>"),
                ("&::probe::Value::bundle);", true, "ValueKind::Struct>", "ValueKind::Enum>"),
                ("&::probe::Value::refs);", true, "ValueKind::ObjectReference>", "ValueKind::Struct>"),
                ("PathStructMember<::probe::Leaf, &::probe::Leaf::count,", false, "ValueKind::UInt32>", "ValueKind::Int32>"),
                ("PathStructMember<::probe::Holder, &::probe::Holder::weights,", false, "ValueKind::Float32>", "ValueKind::Float64>")
            })
            {
                int site = original.IndexOf(anchor, StringComparison.Ordinal);
                Verify.That(site >= 0, "generated path thunk missing: " + anchor);
                int at = before ? original.LastIndexOf(from, site, StringComparison.Ordinal) : original.IndexOf(from, site, StringComparison.Ordinal);
                Verify.That(at >= 0 && Math.Abs(at - site) < 200, "path thunk target kind missing near " + anchor);
                File.WriteAllText(probe.Source, original[..at] + to + original[(at + from.Length)..]);
                ProcessResult rejected = await probe.CompileGeneratedAsync();
                Verify.That(rejected.ExitCode != 0, "path thunk with a mismatched target kind compiled: " + anchor + " " + to);
                Verify.Contains("path thunk target kind does not match the C++ member type", rejected.StdOut + rejected.StdErr, "target kind static_assert diagnostic");
            }
        });
    }
}
