using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

// NativeTypeKey의 컴파일 시 계약을 실제 컴파일러로 확인한다(Docs/Design/BuildTargets 3.8절 BS8d4): 리플렉션 struct
// (ROCKS_GENERATED_BODY)와 Binds 미러의 실제 Core 값 타입(BoundCoreValueV — cv 포함)의 키를 생성 헤더 없이 쓰면 안내
// static_assert로 멈춘다(Modular에서 공유 라이브러리마다 다른 키가 생기지 않게). 리플렉션하지 않는 native 타입은 inline 키나 키 매크로
// 쌍으로 컴파일된다. 생성 헤더 검증과 같이 구성·대상 OS·모듈 API 매크로가 필요하므로 meta 검사(--clang-arg)에서 돈다.
internal static class NativeTypeKeyCompilerTests
{
    public static void Register(TestSuite suite, Options options, IReadOnlyDictionary<string, string?> environment)
    {
        suite.Add("Meta.NativeTypeKeyNeedsGeneratedHeaders", async () =>
        {
            using TemporaryDirectory temporary = new();
            string compiler = options.Required("compiler");
            bool clangCl = Path.GetFileNameWithoutExtension(compiler).Equals("clang-cl", StringComparison.OrdinalIgnoreCase);
            string[] includeDirectories = Directory.GetDirectories(Path.Combine(options.Root, "Core", "Source", "Runtime"))
                .Select(module => Path.Combine(module, "Public")).Where(Directory.Exists).Order(StringComparer.Ordinal).ToArray();

            async Task<ProcessResult> Compile(string name, string body)
            {
                temporary.Write(name + ".cpp", "#include <Meta/Annotations.h>\n#include <Object/Object.h>\n" + body + "\n");
                string source = temporary.FilePath(name + ".cpp");
                List<string> arguments = clangCl
                    ? ["/nologo", "/std:c++latest", "/W4", "/WX", "/Zs", source]
                    : ["-std=c++23", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-fsyntax-only", source];
                foreach (string directory in includeDirectories)
                {
                    arguments.Add((clangCl ? "/I" : "-I") + directory);
                }

                arguments.AddRange(options.ClangArguments);
                return await ProcessRunner.RunAsync(new(compiler, arguments, options.Root, environment, Timeout: TimeSpan.FromSeconds(60)));
            }

            (string Name, string Body)[] accepted =
            [
                ("native_struct", "namespace probe { struct Plain { int value = 0; }; }\nconst void* Key() noexcept { return rocks::NativeTypeKey<probe::Plain>(); }"),
                ("native_enum", "namespace probe { enum class Mode : int { Off }; }\nconst void* Key() noexcept { return rocks::NativeTypeKey<probe::Mode>(); }"),
                ("keyed_native", "namespace probe { struct Keyed { int value = 0; }; }\nROCKS_DECLARE_NATIVE_TYPE_KEY(probe::Keyed, )\n" +
                    "const void* Key() noexcept { return rocks::NativeTypeKey<probe::Keyed>(); }")
            ];
            (string Name, string Body)[] rejected =
            [
                ("reflected_struct", "namespace probe { struct Shape {\nROCKS_GENERATED_BODY()\nint value = 0;\n}; }\n" +
                    "const void* Key() noexcept { return rocks::NativeTypeKey<probe::Shape>(); }"),
                ("binds_target", "const void* Key() noexcept { return rocks::NativeTypeKey<rocks::Vec3>(); }"),
                ("const_binds_target", "const void* Key() noexcept { return rocks::NativeTypeKey<const rocks::Color>(); }")
            ];
            ProcessResult[] acceptedResults = await Task.WhenAll(accepted.Select(item => Compile("accepted_" + item.Name, item.Body)));
            for (int index = 0; index < accepted.Length; ++index)
            {
                Verify.Equal(0, acceptedResults[index].ExitCode,
                    "native key " + accepted[index].Name + " rejected: " + acceptedResults[index].StdOut + acceptedResults[index].StdErr);
            }

            ProcessResult[] rejectedResults = await Task.WhenAll(rejected.Select(item => Compile("rejected_" + item.Name, item.Body)));
            for (int index = 0; index < rejected.Length; ++index)
            {
                Verify.That(rejectedResults[index].ExitCode != 0, "native key " + rejected[index].Name + " compiled without its generated header");
                Verify.Contains("needs its generated header", rejectedResults[index].StdOut + rejectedResults[index].StdErr,
                    rejected[index].Name + " diagnostic");
            }
        });
    }
}
