using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

// native 클래스 등록(Object/NativeClass.h)의 컴파일 시 계약을 실제 컴파일러로 확인한다(Docs/Design/ReflectionFoundations.ko.md
// 9.1절 B8a): 손 Trace는 접근 지정과 무관하게 정확히 `void Trace(TraceVisitor&) const noexcept`여야 하고(다른 모양은 조용히
// 건너뛰지 않고 컴파일 오류), 매크로를 빠뜨린 파생과 native도 리플렉션도 아닌 기반은 안내 static_assert로 멈춘다. 생성 헤더
// 검증과 같이 구성 매크로가 필요하므로 meta 검사(--clang-arg로 ROCKS_BUILD_*를 받는다)에서 돈다.
internal static class NativeClassCompilerTests
{
    public static void Register(TestSuite suite, Options options, IReadOnlyDictionary<string, string?> environment)
    {
        suite.Add("Meta.NativeClassTraceDeclarationIsChecked", async () =>
        {
            using TemporaryDirectory temporary = new();
            string compiler = options.Required("compiler");
            bool clangCl = Path.GetFileNameWithoutExtension(compiler).Equals("clang-cl", StringComparison.OrdinalIgnoreCase);
            string[] includeDirectories = Directory.GetDirectories(Path.Combine(options.Root, "Core", "Source", "Runtime"))
                .Select(module => Path.Combine(module, "Public")).Where(Directory.Exists).Order(StringComparer.Ordinal).ToArray();

            async Task<ProcessResult> Compile(string name, string body)
            {
                temporary.Write(name + ".cpp", "#include <Object/NativeClass.h>\n" + body + "\n");
                string source = temporary.FilePath(name + ".cpp");
                List<string> arguments = clangCl
                    ? ["/nologo", "/std:c++latest", "/W4", "/WX", "/Zs", source]
                    : ["-std=c++23", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-fsyntax-only", source];
                foreach (string directory in includeDirectories)
                {
                    arguments.Add((clangCl ? "/I" : "-I") + directory);
                }

                arguments.AddRange(options.ClangArguments);
                return await ProcessRunner.RunAsync(new(compiler, arguments, options.Root, environment,
                    Timeout: TimeSpan.FromSeconds(60)));
            }

            // Base의 본문(members)을 바꿔 가며 RegisterNativeClass<Registered>를 인스턴스화한다. Leaf는 Base를 잇는 두 단계 사슬이다.
            static string Source(string members, string registered = "Base") =>
                "namespace probe {\nclass Base : public rocks::Object {\n    ROCKS_NATIVE_TYPE(Base, rocks::Object, 1ull, 2ull)\n" + members + "\n};\n" +
                "class Leaf final : public Base {\n    ROCKS_NATIVE_TYPE(Leaf, Base, 3ull, 4ull)\n};\n}\n" +
                $"rocks::Result<rocks::TypeRegistrationToken> Register(rocks::TypeRegistry& types) noexcept {{ return rocks::RegisterNativeClass<probe::{registered}>(types); }}\n";

            // 접근 지정과 무관하게 정확한 시그니처는 받는다(Trace가 없어도 된다).
            (string Name, string Members)[] accepted =
            [
                ("none", ""),
                ("public_trace", "public:\n    void Trace(rocks::TraceVisitor&) const noexcept {}"),
                ("protected_trace", "protected:\n    void Trace(rocks::TraceVisitor&) const noexcept {}"),
                ("private_trace", "private:\n    void Trace(rocks::TraceVisitor&) const noexcept {}")
            ];
            // 다른 모양의 Trace는 컴파일 오류다(이전에는 private·protected·다른 시그니처를 조용히 건너뛰어 GC 구멍이 될 수 있었다).
            (string Name, string Members)[] rejected =
            [
                ("non_const", "public:\n    void Trace(rocks::TraceVisitor&) noexcept {}"),
                ("throwing", "public:\n    void Trace(rocks::TraceVisitor&) const {}"),
                ("private_extra_parameter", "private:\n    void Trace(rocks::TraceVisitor&, int) const noexcept {}"),
                ("static", "public:\n    static void Trace(rocks::TraceVisitor&) noexcept {}"),
                ("overloaded", "public:\n    void Trace(rocks::TraceVisitor&) const noexcept {}\n    void Trace(int) const noexcept {}"),
                ("template", "private:\n    template <class V>\n    void Trace(V&) const noexcept {}"),
                ("data_member", "private:\n    int Trace = 0;")
            ];
            ProcessResult[] acceptedResults = await Task.WhenAll(accepted.Select(item => Compile("accepted_" + item.Name, Source(item.Members))));
            for (int index = 0; index < accepted.Length; ++index)
            {
                Verify.Equal(0, acceptedResults[index].ExitCode,
                    "native Trace declaration " + accepted[index].Name + " rejected: " + acceptedResults[index].StdOut + acceptedResults[index].StdErr);
            }

            ProcessResult[] rejectedResults = await Task.WhenAll(rejected.Select(item => Compile("rejected_" + item.Name, Source(item.Members))));
            for (int index = 0; index < rejected.Length; ++index)
            {
                Verify.That(rejectedResults[index].ExitCode != 0, "native Trace declaration " + rejected[index].Name + " compiled");
                Verify.Contains("declares Trace in another form", rejectedResults[index].StdOut + rejectedResults[index].StdErr,
                    rejected[index].Name + " Trace diagnostic");
            }

            // 사슬의 기반이 잘못 선언했으면 파생만 등록해도 거부한다.
            ProcessResult chain = await Compile("rejected_chain", Source("private:\n    void Trace(rocks::TraceVisitor&) noexcept {}", "Leaf"));
            Verify.That(chain.ExitCode != 0, "derived registration accepted a base with a malformed Trace");
            Verify.Contains("declares Trace in another form", chain.StdOut + chain.StdErr, "chain Trace diagnostic");

            // 매크로를 빠뜨린 파생의 ObjectTypeIdOf와, native도 리플렉션도 아닌 기반의 등록은 안내 static_assert로 멈춘다.
            ProcessResult missingMacro = await Compile("missing_macro", Source("") +
                "namespace probe { class Forgot : public Base {}; }\nrocks::TypeId ForgotId() noexcept { return rocks::ObjectTypeIdOf<probe::Forgot>(); }\n");
            Verify.That(missingMacro.ExitCode != 0, "ObjectTypeIdOf accepted a native derived class without ROCKS_NATIVE_TYPE");
            Verify.Contains("ObjectTypeIdOf<T> needs ROCKS_NATIVE_TYPE declared in T itself", missingMacro.StdOut + missingMacro.StdErr,
                "missing macro diagnostic");
            ProcessResult plainBase = await Compile("plain_base", "namespace probe {\nstruct Plain : rocks::Object {};\n" +
                "class Native final : public Plain {\n    ROCKS_NATIVE_TYPE(Native, Plain, 5ull, 6ull)\n};\n}\n" +
                "rocks::Result<rocks::TypeRegistrationToken> Register(rocks::TypeRegistry& types) noexcept { return rocks::RegisterNativeClass<probe::Native>(types); }\n");
            Verify.That(plainBase.ExitCode != 0, "RegisterNativeClass accepted a base that is neither native nor reflected");
            Verify.Contains("native class base is neither a ROCKS_NATIVE_TYPE class", plainBase.StdOut + plainBase.StdErr,
                "plain base diagnostic");
        });
    }
}
