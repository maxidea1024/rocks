using System.Text;
using System.Text.RegularExpressions;
using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

// meta 시험이 쓰는 MetaCompiler·Clang 탐침이다(임시 디렉터리의 입력 헤더·명세·생성물).
internal static partial class MetaTests
{
    private sealed class Probe : IDisposable
    {
        private readonly TemporaryDirectory temporary = new();
        private readonly Options options;
        private readonly IReadOnlyDictionary<string, string?> environment;
        private readonly string generator;
        private readonly string compiler;
        private readonly string module;

        // module은 생성할 모듈 이름이다(Binds는 CoreTypeBindings 모듈에서만 받으므로 Binds 시험은 그 이름으로 실행한다).
        public Probe(Options options, IReadOnlyDictionary<string, string?> environment, string generator, string compiler,
            string module = MetaCompilerArguments.Module)
        {
            this.options = options;
            this.environment = environment;
            this.generator = generator;
            this.compiler = compiler;
            this.module = module;
            MetaProbeIdentity.Create(IdentityPath);
        }

        private string lastInclude = "Probe.h";

        // 마지막 GenerateAsync의 표준 오류다(진단 코드를 정확히 단언하는 시험이 읽는다).
        public string LastStdErr { get; private set; } = "";

        // 마지막으로 생성한 헤더의 생성 경로다(fixture 모드는 fixture 철자).
        public string Output => MetaCompilerArguments.GeneratedPath(temporary.Root, lastInclude);

        // 마지막으로 생성한 헤더의 타입별 생성 소스(본문) 경로다.
        public string Source => MetaCompilerArguments.GeneratedSourcePath(temporary.Root, lastInclude);

        // 생성 헤더와 그 생성 소스의 글이다(헤더가 먼저다). 선언·형질·inline Trace는 헤더에, 본문(Schema·Ops·Register·정적 검사)은 생성
        // 소스에 있으므로(Docs/Design/ReflectionGeneratedSources.ko.md 5.1절) 생성 코드의 문구를 보는 시험은 둘을 이어 읽는다.
        public string GeneratedText() => File.ReadAllText(Output) + File.ReadAllText(Source);

        public string Descriptor => MetaCompilerArguments.Descriptor(temporary.Root);

        // Sparks binding schema 출력이다(Docs/Design/SparksBinding.ko.md B2).
        public string BindingSchema => MetaCompilerArguments.BindingSchema(temporary.Root);

        // 모듈 생성 헤더(등록 함수 선언)의 경로다.
        public string Aggregate => MetaCompilerArguments.AggregatePath(temporary.Root);

        // 모듈 생성 소스(등록 함수 정의 — 등록 순서를 보는 시험이 읽는다)의 경로다.
        public string ModuleSource => MetaCompilerArguments.ModuleSourcePath(temporary.Root);

        private string IdentityPath => temporary.FilePath("probe.rocksids.json");

        // 입력 헤더 옆(include 루트 바로 아래)에 헤더를 둔다 — MetaCompiler 탐침이 모듈 public 헤더로 더한다.
        public void WriteHeader(string name, string text) => temporary.Write(name, text);

        // 임시 디렉터리(include 루트) 아래 파일의 경로다.
        public string HeaderPath(string name) => temporary.FilePath(name);

        public async Task GenerateAsync(string? source = null, bool fixture = false, IReadOnlyList<string>? extra = null,
            string? expectedCode = null, string? reason = null)
        {
            string include = fixture ? "ReflectionTestFixtures/ReflectionFixture.h" : "Probe.h";
            string input = fixture ? Path.Combine(FixturePublic, "ReflectionTestFixtures", "ReflectionFixture.h") : temporary.FilePath("Probe.h");
            lastInclude = include;
            if (source is not null)
            {
                File.WriteAllText(input, source, new UTF8Encoding(false, true));
            }

            List<string> arguments = [];
            string executable = generator;
            if (generator.EndsWith(".dll", StringComparison.OrdinalIgnoreCase))
            {
                executable = options.Optional("dotnet") ?? "dotnet";
                arguments.Add(generator);
            }

            // 실제 fixture 헤더는 그 모듈 이름으로 생성한다(클래스의 모듈 API 매크로가 그 모듈의 것이어야 한다 — BS8d3 RMC2007).
            arguments.AddRange([.. MetaCompilerArguments.Single(temporary.Root, input, include, fixture ? "ReflectionTestFixtures" : module), "--compiler", compiler]);
            AddDefaultIncludes(arguments);
            arguments.AddRange(["--identity-manifest", fixture
                ? Path.Combine(options.Root, "Core", "Source", "Developer", "ReflectionTestFixtures", "ReflectionTestFixtures.rocksids.json") : IdentityPath]);
            // 시험이 구성 프로필 전체를 넘기면(구성·대상 OS 매크로 포함) 기본 인자의 같은 매크로는 빼서 중복 정의가 되지
            // 않게 한다.
            bool extraDefinesBuild = extra?.Any(argument => argument.Contains("ROCKS_BUILD_", StringComparison.Ordinal)) == true;
            bool extraDefinesPlatform = extra?.Any(argument => argument.Contains("ROCKS_PLATFORM_", StringComparison.Ordinal)) == true;
            bool extraDefinesLink = extra?.Any(argument => argument.Contains(Rocks.Build.LinkTypes.MonolithicMacroName, StringComparison.Ordinal)) == true;
            foreach (string argument in options.ClangArguments)
            {
                if ((extraDefinesBuild && argument.Contains("ROCKS_BUILD_", StringComparison.Ordinal)) ||
                    (extraDefinesPlatform && argument.Contains("ROCKS_PLATFORM_", StringComparison.Ordinal)) ||
                    (extraDefinesLink && argument.Contains(Rocks.Build.LinkTypes.MonolithicMacroName, StringComparison.Ordinal)))
                {
                    continue;
                }

                arguments.Add("--clang-arg=" + argument);
            }

            if (extra is not null)
            {
                arguments.AddRange(extra);
            }

            var result = await ProcessRunner.RunAsync(new(executable, arguments, options.Root, environment, Timeout: ProbeTimeout));
            LastStdErr = result.StdErr;
            if (expectedCode is null)
            {
                Verify.Equal(0, result.ExitCode, result.StdErr);
            }
            else
            {
                Verify.That(result.ExitCode != 0, "invalid reflection input accepted");
                Verify.Contains(expectedCode, result.StdErr, "stable diagnostic code");
                Verify.Contains("hint:", result.StdErr.ToLowerInvariant(), "actionable diagnostic hint");
                Verify.That(Regex.IsMatch(result.StdErr, @":\d+:.*RMC", RegexOptions.CultureInvariant, TimeSpan.FromSeconds(1)), "file/line diagnostic missing:\n" + result.StdErr);
                if (reason is not null)
                {
                    Verify.That(result.StdErr.Contains(reason, StringComparison.OrdinalIgnoreCase), "diagnostic reason missing: " + reason + "\n" + result.StdErr);
                }
            }
        }

        public async Task PrefixDifferentFileAsync()
        {
            string source = Preamble + "ROCKS_CLASS()\n#include <Other.h>\n}\n";
            File.WriteAllText(temporary.FilePath("Other.h"), "struct Value : public rocks::Object {};\n");
            await RejectAsync(source, "RMC2000", "ROCKS_CLASS");
        }

        // 생성 헤더는 원본을 <철자>로 포함하므로 --include 철자는 --include-dir 검색 순서로 원본에 풀려야 한다.
        // 검색 순서에서 앞선 다른 헤더가 같은 철자를 가리면 거부하고(이전 출력 보존), 지운 뒤에는 받아들인다.
        public async Task OutputDirectoryBindingAsync()
        {
            string sourceDirectory = temporary.FilePath("Source");
            string shadowDirectory = temporary.FilePath("Shadow");
            Directory.CreateDirectory(sourceDirectory);
            Directory.CreateDirectory(shadowDirectory);
            string author = Path.Combine(sourceDirectory, "Probe.h");
            string shadow = Path.Combine(shadowDirectory, "Probe.h");
            string output = MetaCompilerArguments.GeneratedPath(temporary.Root, "Probe.h");
            File.WriteAllText(author, Declaration());
            File.WriteAllText(shadow, Declaration(extra: "void Shadow() noexcept {}"));
            Directory.CreateDirectory(Path.GetDirectoryName(output)!);
            File.WriteAllText(output, "previous accepted output\n");
            async Task<ProcessResult> Run()
            {
                List<string> arguments = [];
                string executable = generator;
                if (generator.EndsWith(".dll", StringComparison.OrdinalIgnoreCase))
                {
                    executable = options.Optional("dotnet") ?? "dotnet";
                    arguments.Add(generator);
                }

                arguments.AddRange([.. MetaCompilerArguments.Single(temporary.Root, author, "Probe.h"), "--compiler", compiler]);
                IEnumerable<string> searchOrder = File.Exists(shadow) ? [shadowDirectory, sourceDirectory] : [sourceDirectory];
                foreach (string include in searchOrder.Concat(Directory.GetDirectories(Path.Combine(options.Root, "Core", "Source", "Runtime"))
                    .Select(module => Path.Combine(module, "Public")).Where(Directory.Exists)))
                {
                    arguments.AddRange(["--include-dir", include]);
                }

                arguments.AddRange(["--identity-manifest", IdentityPath]);
                foreach (string argument in options.ClangArguments)
                {
                    arguments.Add("--clang-arg=" + argument);
                }

                return await ProcessRunner.RunAsync(new(executable, arguments, options.Root, environment, Timeout: ProbeTimeout));
            }

            bool ValidationLeaked() => Directory.EnumerateDirectories(temporary.Root, ".rocks-meta-validation-*").Any();
            var rejected = await Run();
            Verify.That(rejected.ExitCode != 0, "shadowing header accepted");
            Verify.Contains("RMC1000", rejected.StdErr, "shadow error code");
            Verify.Contains(shadow, rejected.StdErr, "actual shadow path");
            Verify.Equal("previous accepted output\n", File.ReadAllText(output), "shadow failure changed accepted output");
            File.Delete(shadow);
            var accepted = await Run();
            Verify.Equal(0, accepted.ExitCode, "original header not selected after shadow removal: " + accepted.StdErr);
            Verify.That(!ValidationLeaked(), "successful validation temporary directory leaked");
            string validOutput = File.ReadAllText(output);
            File.WriteAllText(author, Preamble + "ROCKS_CLASS()\nstruct Value : public rocks::Object { Value() {} };\n}\n");
            var invalid = await Run();
            Verify.That(invalid.ExitCode != 0, "invalid generated contract accepted");
            Verify.Contains("RMC3000", invalid.StdErr, "generated contract rejection code");
            Verify.Equal(validOutput, File.ReadAllText(output), "failed validation published output");
            Verify.That(!ValidationLeaked(), "failed validation temporary directory leaked");
        }

        public async Task SymlinkLocationsAsync()
        {
            string real = temporary.FilePath("real");
            string alias = temporary.FilePath("alias");
            string shadow = temporary.FilePath("shadow");
            Directory.CreateDirectory(real);
            Directory.CreateDirectory(shadow);
            try
            {
                Directory.CreateSymbolicLink(alias, real);
            }
            catch (Exception error) when (OperatingSystem.IsWindows() && error is UnauthorizedAccessException or IOException)
            {
                Console.WriteLine("[SKIP] Meta symlink fixture: Windows account cannot create symbolic links: " + error.Message);
                return;
            }

            string original = Declaration();
            string actualInput = Path.Combine(real, "Probe.h");
            string aliasInput = Path.Combine(alias, "Probe.h");
            File.WriteAllText(actualInput, original);
            File.WriteAllText(Path.Combine(shadow, "Probe.h"), Declaration(extra: "void Shadow() noexcept {}"));
            async Task<ProcessResult> Run(string input, string outputRoot, params string[] includeDirs)
            {
                List<string> arguments = [];
                string executable = generator;
                if (generator.EndsWith(".dll", StringComparison.OrdinalIgnoreCase))
                {
                    executable = options.Optional("dotnet") ?? "dotnet";
                    arguments.Add(generator);
                }

                arguments.AddRange(["--module", MetaCompilerArguments.Module, "--input", input, "--include", "Probe.h", "--output-root", outputRoot,
                    "--descriptor", MetaCompilerArguments.Descriptor(temporary.Root), MetaCompilerArguments.VerifySources, "--compiler", compiler]);
                foreach (string include in includeDirs.Concat(Directory.GetDirectories(Path.Combine(options.Root, "Core", "Source", "Runtime"))
                    .Select(module => Path.Combine(module, "Public")).Where(Directory.Exists)))
                {
                    arguments.AddRange(["--include-dir", include]);
                }

                arguments.AddRange(["--identity-manifest", IdentityPath]);
                foreach (string argument in options.ClangArguments)
                {
                    arguments.Add("--clang-arg=" + argument);
                }

                return await ProcessRunner.RunAsync(new(executable, arguments, options.Root, environment, Timeout: ProbeTimeout));
            }

            var valid = await Run(aliasInput, MetaCompilerArguments.OutputRoot(temporary.Root), real);
            Verify.Equal(0, valid.ExitCode, "same file through symlinked parent rejected: " + valid.StdErr);
            var shadowed = await Run(aliasInput, MetaCompilerArguments.OutputRoot(temporary.Root), shadow, real);
            Verify.That(shadowed.ExitCode != 0, "different shadow header accepted");
            Verify.Contains("resolved input=", shadowed.StdErr, "resolved input path missing");
            Verify.Contains("resolved target=", shadowed.StdErr, "resolved shadow target missing");
            Verify.Contains(Path.Combine(shadow, "Probe.h"), shadowed.StdErr, "shadow header path missing");
            // 출력 루트가 (symlink 별칭으로) 입력 헤더를 담으면 거부한다(원본 헤더를 덮어쓰지 않는다).
            var sameOutput = await Run(actualInput, alias, real);
            Verify.That(sameOutput.ExitCode != 0, "input/output symlink alias was accepted");
            Verify.Contains("RMC1000", sameOutput.StdErr, "same input/output rejection code");
            Verify.Equal(original, File.ReadAllText(actualInput), "source header changed after rejected output alias");
        }

        public async Task SourceNotesAsync()
        {
            string input = temporary.FilePath("Probe.h");
            string source = "#pragma once\n#include <Meta/Annotations.h>\n#include <Object/Object.h>\n" +
                "namespace a { ROCKS_CLASS()\nstruct Thing : public rocks::Object { Thing() {} }; }\n" +
                "namespace b { ROCKS_CLASS(Name = \"OtherThing\")\nstruct Thing : public rocks::Object {}; }\n" +
                "namespace a { ROCKS_CLASS()\nstruct ThingExtended : public rocks::Object {}; }\n";
            File.WriteAllText(input, source);
            List<string> arguments = [];
            string executable = generator;
            if (generator.EndsWith(".dll", StringComparison.OrdinalIgnoreCase))
            {
                executable = options.Optional("dotnet") ?? "dotnet";
                arguments.Add(generator);
            }

            arguments.AddRange([.. MetaCompilerArguments.Single(temporary.Root, input, "Probe.h"), "--compiler", compiler]);
            AddDefaultIncludes(arguments);
            arguments.AddRange(["--identity-manifest", IdentityPath]);
            foreach (string argument in options.ClangArguments)
            {
                arguments.Add("--clang-arg=" + argument);
            }

            var result = await ProcessRunner.RunAsync(new(executable, arguments, options.Root, environment, Timeout: ProbeTimeout));
            Verify.That(result.ExitCode != 0, "throwing constructor was accepted");
            Verify.Contains("RMC3000", result.StdErr, "generated contract failure");
            Verify.Contains("diagnostic references CLASS a::Thing", result.StdErr, "qualified class reference");
            Verify.That(!result.StdErr.Contains("note: diagnostic references CLASS b::Thing", StringComparison.Ordinal),
                "unrelated namespaced class was falsely identified");
            Verify.That(!result.StdErr.Contains("note: diagnostic references CLASS a::ThingExtended", StringComparison.Ordinal),
                "prefix name was falsely identified");
        }

        public async Task NestedLocationAsync()
        {
            string main = temporary.FilePath("Main.h");
            string a = temporary.FilePath("First.h");
            string middle = temporary.FilePath("Middle.h");
            string bad = temporary.FilePath("Deep.h");
            File.WriteAllText(main, "#pragma once\n#include <First.h>\n#include <Middle.h>\n");
            File.WriteAllText(a, "#pragma once\n#include <Meta/Annotations.h>\n#include <Object/Object.h>\n" +
                "namespace first { ROCKS_CLASS()\nstruct Good : public rocks::Object {}; }\n");
            File.WriteAllText(middle, "#pragma once\n#include <Deep.h>\n");
            var fixtures = new[]
            {
                ("#pragma once\n#include <Meta/Annotations.h>\n#include <Object/Object.h>\n\n\nnamespace example {\n" +
                    "ROCKS_CLASS(UnknownClassFlag)\nstruct Invalid : public rocks::Object {};\n}\n", 7),
                ("#pragma once\n#include <Meta/Annotations.h>\n#include <Object/Object.h>\n\nnamespace example {\n" +
                    "ROCKS_CLASS()\nstruct Invalid : public rocks::Object {\n" +
                    "ROCKS_PROPERTY() int valid;\n\n" +
                    "ROCKS_PROPERTY(ReadOnyl) int bad;\n};\n}\n", 10)
            };
            foreach (var (source, line) in fixtures)
            {
                File.WriteAllText(bad, source);
                List<string> arguments = [];
                string executable = generator;
                if (generator.EndsWith(".dll", StringComparison.OrdinalIgnoreCase))
                {
                    executable = options.Optional("dotnet") ?? "dotnet";
                    arguments.Add(generator);
                }

                // 리플렉션 선언은 모듈 입력 헤더에 있어야 한다: 오류 선언이 있는 Deep.h를 입력으로 두고 진단 위치를 본다.
                arguments.AddRange(["--module", MetaCompilerArguments.Module, "--input", a, "--include", "First.h", "--input", bad, "--include", "Deep.h",
                    "--output-root", MetaCompilerArguments.OutputRoot(temporary.Root), "--descriptor", MetaCompilerArguments.Descriptor(temporary.Root), "--compiler", compiler]);
                AddDefaultIncludes(arguments);
                arguments.AddRange(["--identity-manifest", IdentityPath]);
                foreach (string argument in options.ClangArguments)
                {
                    arguments.Add("--clang-arg=" + argument);
                }

                var result = await ProcessRunner.RunAsync(new(executable, arguments, options.Root, environment, Timeout: ProbeTimeout));
                Verify.That(result.ExitCode != 0, "invalid nested declaration was accepted");
                Verify.Contains($"{bad}:{line}: error RMC2000".Replace('\\', '/'), result.StdErr.Replace('\\', '/'), "nested source file and physical line");
                Verify.Contains("hint:", result.StdErr, "nested declaration correction guidance");
            }
        }

        // 마지막으로 생성한 헤더와 그 생성 소스를 한 번역 단위로 소비자와 같은 경고 프로필로 컴파일한다(생성 static_assert probe —
        // 생성 헤더를 고친 뒤 부른다). 정적 검사는 생성 소스의 VerifyGenerated 정의에 있으므로 생성 소스까지 컴파일해야 평가된다.
        // consumer는 include 뒤에 붙이는 소비 코드다(생성 템플릿을 인스턴스화할 때 쓴다).
        // editor가 거짓이면 게임 조합(ROCKS_WITH_EDITOR_DATA=0 — EditorOnly 항목이 빠진 생성 코드, Docs/Design/BuildTargets 3.4절 BS2c)이다.
        public Task<ProcessResult> CompileGeneratedAsync(string consumer = "", bool editor = true) =>
            CompileSourceAsync($"#include <{lastInclude[..^2]}.generated.h>\n#include \"{Source.Replace('\\', '/')}\"\n{consumer}", editor);

        // 소비 번역 단위 하나를 소비자와 같은 경고 프로필로 구문 검사한다. 무엇을 include할지는 text가 정한다(생성 헤더를 빼고
        // 리플렉션 헤더만 include한 소비자를 볼 때 쓴다). probe 입력의 include 철자는 Probe.h다.
        public async Task<ProcessResult> CompileSourceAsync(string text, bool editor = true)
        {
            bool clangCl = Path.GetFileNameWithoutExtension(compiler).Contains("clang-cl", StringComparison.OrdinalIgnoreCase);
            string source = temporary.FilePath("generated-check.cpp");
            File.WriteAllText(source, text, new UTF8Encoding(false, true));
            List<string> arguments = clangCl
                ? ["/nologo", "/TP", "/utf-8", "/clang:-std=c++23", "/W4", "/WX", "/GR-", "/EHs-c-", "/Zc:__cplusplus", "/Zs", source]
                : ["-x", "c++", "-std=c++23", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-fno-exceptions", "-fno-rtti", "-fsyntax-only", source];
            List<string> includes = [];
            AddDefaultIncludes(includes);
            foreach (string directory in new[] { Path.Combine(MetaCompilerArguments.OutputRoot(temporary.Root), "Public") }
                .Concat(includes.Where(argument => argument != "--include-dir")))
            {
                arguments.Add((clangCl ? "/I" : "-I") + directory);
            }

            arguments.AddRange(options.ClangArguments);
            // 타깃 매크로는 시험이 정한다(메타 컴파일러와 같이 — 기본은 에디터 조합).
            arguments.AddRange(TargetMacroArguments.Of(editor));
            return await ProcessRunner.RunAsync(new(compiler, arguments, options.Root, environment, Timeout: ProbeTimeout));
        }

        public async Task RejectAsync(string source, string code, string reason)
        {
            byte[] previous = Encoding.UTF8.GetBytes("previous accepted output\n");
            Directory.CreateDirectory(Path.GetDirectoryName(Output)!);
            File.WriteAllBytes(Output, previous);
            await GenerateAsync(source, expectedCode: code, reason: reason);
            Verify.That(File.ReadAllBytes(Output).AsSpan().SequenceEqual(previous), "rejected input changed previous accepted output");
            Verify.That(!Directory.EnumerateFiles(temporary.Root, "*.tmp*").Any(), "temporary generated output leaked");
        }

        private void AddDefaultIncludes(List<string> arguments)
        {
            foreach (string directory in Directory.GetDirectories(Path.Combine(options.Root, "Core", "Source", "Runtime"))
                .Select(module => Path.Combine(module, "Public")).Where(Directory.Exists).Order(StringComparer.Ordinal)
                .Concat([FixturePublic, options.Root, temporary.Root]))
            {
                arguments.AddRange(["--include-dir", directory]);
            }
        }

        private string FixturePublic => Path.Combine(options.Root, "Core", "Source", "Developer", "ReflectionTestFixtures", "Public");

        public void Dispose() => temporary.Dispose();
    }
}
