using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

internal static class TypeConvenienceCompilerTests
{
    public static void Register(TestSuite suite, Options options, IReadOnlyDictionary<string, string?> environment)
    {
        suite.Add("Meta.TypeConvenienceGeneratedHeaderLinksAcrossTranslationUnits", async () =>
        {
            using TemporaryDirectory temporary = new();
            string compiler = options.Required("compiler");
            bool clangCl = Path.GetFileNameWithoutExtension(compiler).Equals("clang-cl", StringComparison.OrdinalIgnoreCase);
            string[] includeDirectories = Directory.GetDirectories(Path.Combine(options.Root, "Core", "Source", "Runtime"))
                .Select(module => Path.Combine(module, "Public")).Where(Directory.Exists).Order(StringComparer.Ordinal)
                .Concat([options.Root, temporary.Root, Path.Combine(MetaCompilerArguments.OutputRoot(temporary.Root), "Public")]).ToArray();
            Directory.CreateDirectory(Path.Combine(MetaCompilerArguments.OutputRoot(temporary.Root), "Public"));

            // 실제 메타컴파일러를 실행해 같은 생성 헤더를 두 번역 단위가 사용하게 한다.
            temporary.Write("Probe.h", "#pragma once\n#include <Meta/Annotations.h>\n#include <Object/Object.h>\n" +
                "namespace probe { ROCKS_CLASS() struct Value final : rocks::Object {}; }\n");
            string identity = temporary.FilePath("probe.rocksids.json");
            MetaProbeIdentity.Create(identity);
            string generator = Path.GetFullPath(options.Required("metacompiler"));
            string executable = generator;
            List<string> generate = [];
            if (generator.EndsWith(".dll", StringComparison.OrdinalIgnoreCase))
            {
                executable = options.Optional("dotnet") ?? "dotnet";
                generate.Add(generator);
            }

            generate.AddRange([.. MetaCompilerArguments.Single(temporary.Root, temporary.FilePath("Probe.h"), "Probe.h"),
                "--compiler", compiler, "--identity-manifest", identity]);
            foreach (string directory in includeDirectories)
            {
                generate.AddRange(["--include-dir", directory]);
            }

            foreach (string argument in options.ClangArguments)
            {
                generate.Add("--clang-arg=" + argument);
            }

            var generated = await ProcessRunner.RunAsync(new(executable, generate, options.Root, environment,
                Timeout: TimeSpan.FromSeconds(60)));
            Verify.Equal(0, generated.ExitCode, "metadata generation: " + generated.StdOut + generated.StdErr);

            temporary.Write("first.cpp", "#include <Probe.generated.h>\n" +
                "const rocks::TypeSchema* FirstSchema() noexcept { return &rocks::TypeOf<probe::Value>(); }\n" +
                "rocks::TypeId FirstId() noexcept { return rocks::TypeIdOf<probe::Value>(); }\n");
            temporary.Write("second.cpp", "#include <Probe.generated.h>\n" +
                "const rocks::TypeSchema* FirstSchema() noexcept; rocks::TypeId FirstId() noexcept;\n" +
                "int main() { const auto* left = FirstSchema(); const auto* right = &rocks::TypeOf<probe::Value>(); " +
                "const auto id = rocks::TypeIdOf<probe::Value>(); " +
                "return left == right && FirstId() == id && right->id == id ? 0 : 1; }\n");

            async Task<string> Compile(string name)
            {
                string source = temporary.FilePath(name + ".cpp");
                string output = temporary.FilePath(name + (clangCl ? ".obj" : ".o"));
                List<string> arguments = clangCl
                    ? ["/nologo", "/std:c++latest", "/W4", "/WX", "/c", source, "/Fo" + output]
                    : ["-std=c++23", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-c", source, "-o", output];
                foreach (string directory in includeDirectories)
                {
                    arguments.Add((clangCl ? "/I" : "-I") + directory);
                }

                arguments.AddRange(options.ClangArguments);
                var compiled = await ProcessRunner.RunAsync(new(compiler, arguments, options.Root, environment,
                    Timeout: TimeSpan.FromSeconds(60)));
                Verify.Equal(0, compiled.ExitCode, name + " compile: " + compiled.StdOut + compiled.StdErr);
                return output;
            }

            string first = await Compile("first");
            string second = await Compile("second");
            string program = temporary.FilePath(clangCl ? "type_odr.exe" : "type_odr");
            List<string> link = clangCl ? ["/nologo", first, second, "/Fe" + program] : [first, second, "-o", program];
            link.AddRange(options.ClangArguments);
            var linked = await ProcessRunner.RunAsync(new(compiler, link, options.Root, environment,
                Timeout: TimeSpan.FromSeconds(60)));
            Verify.Equal(0, linked.ExitCode, "two translation-unit link: " + linked.StdOut + linked.StdErr);
            var ran = await ProcessRunner.RunAsync(new(program, [], options.Root, environment, Timeout: TimeSpan.FromSeconds(20)));
            Verify.Equal(0, ran.ExitCode, "generated schema/ID mismatch across translation units: " + ran.StdOut + ran.StdErr);
        });

        suite.Add("Meta.TypeConvenienceMissingGeneratedHeaderIsDiagnosed", async () =>
        {
            using TemporaryDirectory temporary = new();
            string compiler = options.Required("compiler");
            bool clangCl = Path.GetFileNameWithoutExtension(compiler).Equals("clang-cl", StringComparison.OrdinalIgnoreCase);
            string[] includeDirectories = Directory.GetDirectories(Path.Combine(options.Root, "Core", "Source", "Runtime"))
                .Select(module => Path.Combine(module, "Public")).Where(Directory.Exists).Order(StringComparer.Ordinal).ToArray();

            async Task<ProcessResult> Compile(string name, string body)
            {
                temporary.Write(name + ".cpp", "#include <Meta/GeneratedType.h>\nstruct Missing {};\n" + body + "\n");
                string source = temporary.FilePath(name + ".cpp");
                string output = temporary.FilePath(name + ".o");
                List<string> arguments = clangCl
                    ? ["/nologo", "/std:c++latest", "/W4", "/WX", "/c", source, "/Fo" + output]
                    : ["-std=c++23", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-c", source, "-o", output];
                foreach (string directory in includeDirectories)
                {
                    arguments.Add((clangCl ? "/I" : "-I") + directory);
                }

                arguments.AddRange(options.ClangArguments);
                return await ProcessRunner.RunAsync(new(compiler, arguments, options.Root, environment,
                    Timeout: TimeSpan.FromSeconds(60)));
            }

            var valid = await Compile("valid", "int main() { return 0; }");
            Verify.Equal(0, valid.ExitCode, "native compiler/include control: " + valid.StdOut + valid.StdErr);

            var type = await Compile("missing_type", "int main() { (void)rocks::TypeOf<Missing>(); return 0; }");
            Verify.That(type.ExitCode != 0, "TypeOf accepted a type with missing metadata");
            Verify.Contains("TypeOf requires a reflected type and its generated header", type.StdOut + type.StdErr,
                "TypeOf missing metadata diagnostic");

            var id = await Compile("missing_id", "int main() { (void)rocks::TypeIdOf<Missing>(); return 0; }");
            Verify.That(id.ExitCode != 0, "TypeIdOf accepted a type with missing metadata");
            Verify.Contains("TypeIdOf requires a reflected type and its generated header", id.StdOut + id.StdErr,
                "TypeIdOf missing metadata diagnostic");
        });
    }
}
