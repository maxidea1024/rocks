using System.Text;
using System.Text.RegularExpressions;
using Rocks.Tooling.Common;
using Rocks.Build;
using Rocks.MetaCompiler;

namespace Rocks.ToolTests;

internal static class MetaTests
{
    private const string Preamble = "#include <Meta/Annotations.h>\n#include <Object/ObjectRefs.h>\n#include <Object/Object.h>\n#include <Core/Text/String.h>\nnamespace probe {\n";

    private static string Declaration(string? field = null,
        string bases = "public rocks::Object", string extra = "") =>
        Preamble + $"ROCKS_CLASS()\nstruct Value : {bases} {{\n" +
        (field ?? "ROCKS_PROPERTY() rocks::int32 count = 0;") + "\n" + extra + "\n};\n}\n";

    public static void Register(TestSuite suite, Options options, IReadOnlyDictionary<string, string?> environment)
    {
        string compiler = options.Required("compiler");
        string generator = Path.GetFullPath(options.Required("metacompiler"));
        if (!File.Exists(generator))
        {
            throw new FileNotFoundException("MetaCompiler executable missing", generator);
        }

        Dictionary<string, string?> actualEnvironment = new(environment, OperatingSystem.IsWindows() ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal);
        if (options.Optional("formatter") is string formatter)
        {
            actualEnvironment["ROCKS_CLANG_FORMAT"] = formatter;
        }

        GeneratedBodyReviewTests.Register(suite, options, actualEnvironment);
        TypeConvenienceCompilerTests.Register(suite, options, actualEnvironment);
        IdentityCompilerTests.Register(suite, options, actualEnvironment);
        IdentityCliReviewTests.Register(suite, options, actualEnvironment);

        suite.Add("Meta.FixtureDeterminismTimestampAndStaleCheck", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            await probe.GenerateAsync(fixture: true);
            byte[] expected = File.ReadAllBytes(probe.Output);
            Verify.That(!expected.Contains((byte)'\r'), "generated output must use LF");
            Verify.Contains("rocks::reflection_fixture::Node", Encoding.UTF8.GetString(expected), "fixture generated type");
            File.SetLastWriteTimeUtc(probe.Output, DateTime.UnixEpoch.AddSeconds(1_000_000_000));
            DateTime timestamp = File.GetLastWriteTimeUtc(probe.Output);
            await probe.GenerateAsync(fixture: true);
            Verify.That(File.ReadAllBytes(probe.Output).AsSpan().SequenceEqual(expected), "generator output changed for identical source");
            Verify.Equal(timestamp, File.GetLastWriteTimeUtc(probe.Output), "unchanged output was rewritten");
            await probe.GenerateAsync(fixture: true, extra: ["--check"]);
            Verify.Equal(timestamp, File.GetLastWriteTimeUtc(probe.Output), "check changed output timestamp");
            File.WriteAllText(probe.Output, "stale\n");
            await probe.GenerateAsync(fixture: true, extra: ["--check"], expectedCode: "RMC4000", reason: "stale");
            Verify.Equal("stale\n", File.ReadAllText(probe.Output), "stale check overwrote output");
        });
        suite.Add("Meta.GeneratedHeaderDirectoryControlsIncludeBinding", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            await probe.OutputDirectoryBindingAsync();
        });
        suite.Add("Meta.SymlinkedParentsAndShadowedHeaders", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            await probe.SymlinkLocationsAsync();
        });
        suite.Add("Meta.ClangSourceNotesDoNotMisidentifyNames", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            await probe.SourceNotesAsync();
        });
        suite.Add("Meta.IncludedHeaderDiagnosticUsesOriginalFileAndLine", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            await probe.NestedLocationAsync();
        });
        AddRejectGroup("Meta.MissingFieldAnnotation", [Declaration("rocks::int32 count = 0;")], "RMC2000", "field");
        AddRejectGroup("Meta.PointerAndCvFields", new[] { "rocks::int32* count = nullptr", "const rocks::int32 count = 0" }
            .Select(field => Declaration($"ROCKS_PROPERTY() {field};")), "RMC2000", "type");
        AddRejectGroup("Meta.PrivateWithoutGeneratedBody", [Declaration("private: ROCKS_PROPERTY() rocks::int32 count = 0;",
            extra: "public: rocks::int32 Read() const noexcept { return count; }")], "RMC2000", "GENERATED_BODY");
        AddRejectGroup("Meta.Bitfields", [Declaration("ROCKS_PROPERTY() unsigned int count : 1;")], "RMC2000", "bitfield");
        AddRejectGroup("Meta.StaticAnnotatedField", [Declaration("ROCKS_PROPERTY() static inline rocks::int32 count = 0;")], "RMC200", "nonstatic");
        AddRejectGroup("Meta.ManualTraceConflict", [Declaration(extra: "void Trace() noexcept {}")], "RMC2000", "Trace");
        AddRejectGroup("Meta.VirtualAndMultipleInheritance", [Declaration(bases: "public virtual rocks::Object"),
            Declaration(bases: "public rocks::Object, public Other").Replace("namespace probe {", "namespace probe { struct Other {};", StringComparison.Ordinal)], "RMC2000", "nonvirtual");
        AddRejectGroup("Meta.UnreflectedBaseAndReference", [
            Declaration(bases: "public Hidden").Replace("namespace probe {", "namespace probe { struct Hidden : rocks::Object {};", StringComparison.Ordinal),
            Declaration("ROCKS_PROPERTY() rocks::ObjectRef<Hidden> next;")
                .Replace("namespace probe {", "namespace probe { struct Hidden : rocks::Object {};", StringComparison.Ordinal)], "RMC2000", "reflected");
        AddRejectGroup("Meta.ClangSyntaxFailurePreservesOutput", [Declaration("this is not valid cxx;")], "RMC3000", "Clang");
        suite.Add("Meta.DiagnosticsAndResponseOverridesRejected", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            var arguments = new List<string> { "-Wno-error", "-w", "@hidden.rsp", "-I", "-Ihidden", "-iquote", "-iquotehidden", "-isystem", "-isystemhidden", "-idirafter", "-idirafterhidden", "-include", "-includehidden", "-imacros", "-imacroshidden", "-F", "-Fhidden", "-iframeworkhidden", "-Wp,-Ihidden", "-ivfsoverlay=hidden", "--config=hidden" };
            arguments.AddRange(["-iwithprefixbefore", "-iwithprefixbeforehidden", "-iwithprefix", "-iwithprefixhidden", "-iprefix", "-iprefixhidden", "-iwithsysroot", "-iwithsysroothidden", "-stdlib++-isystem", "-stdlib++-isystemhidden", "--gcc-toolchain", "--gcc-toolchain=hidden", "-vfsoverlay", "-vfsoverlay=hidden", "-working-directory", "-working-directory=hidden", "-D", "-D=invalid", "-D_CRT_SECURE_NO_WARNINGS", "--target=hidden"]);
            if (OperatingSystem.IsWindows())
            {
                arguments.AddRange(["/clang:-Wno-error", "/W0", "/I", "/Ihidden", "/FI", "/FIhidden", "/external:Ihidden", "/clang:-Ihidden"]);
            }

            foreach (string argument in arguments)
            {
                await probe.GenerateAsync(Declaration(), extra: ["--clang-arg=" + argument], expectedCode: "RMC1000", reason: "override");
                Verify.That(!File.Exists(probe.Output), "rejected CLI wrote output");
            }
        });
        if (OperatingSystem.IsMacOS())
        {
            suite.Add("Meta.MacAbsolutePathsAreNotWindowsSwitches", async () =>
            {
                using Probe probe = new(options, actualEnvironment, generator, compiler);
                // 경로가 없으므로 SDK 진단으로 거부한다. /I·/FI Windows 옵션으로 오인하지 않는다.
                foreach (string path in new[] { "/Install/RocksMissingSdk", "/Files/RocksMissingSdk" })
                {
                    await probe.GenerateAsync(Declaration(), extra: ["--clang-arg=-isysroot", "--clang-arg=" + path],
                        expectedCode: "RMC1000", reason: "SDK");
                }
            });
        }

        suite.Add("Meta.ConflictingOrDuplicateCompilerOptionsAreRejected", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            var cases = new List<string[]>
            {
                new[] { "-DROCKS_CONFLICT=1", "-DROCKS_CONFLICT=2" },
                new[] { "-DROCKS_DUPLICATE=1", "-DROCKS_DUPLICATE=1" },
                new[] { "-DROCKS_SAME", "-DROCKS_SAME=1" }
            };
            if (OperatingSystem.IsWindows())
            {
                cases.AddRange([["/MD", "/MDd"], ["/O2", "/Od"], ["/MD", "/MD"],
                    ["/MDd", "/fsanitize=address"], ["/DROCKS_MIXED=1", "-DROCKS_MIXED=2"]]);
            }
            else
            {
                cases.AddRange([["-O0", "-O2"], ["-Wall", "-Wall"],
                    ["-mmacosx-version-min=13.0", "-mmacosx-version-min=14.0"]]);
            }

            foreach (string[] conflict in cases)
            {
                await probe.GenerateAsync(Declaration(), extra: conflict.Select(value => "--clang-arg=" + value).ToArray(),
                    expectedCode: "RMC1000", reason: "conflict");
                Verify.That(!File.Exists(probe.Output), "conflicting options published metadata");
            }
        });
        suite.Add("Meta.AllNativeCompileProfilesAndExplicitDefinesAreAccepted", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            NativeToolchain toolchain = await NativeToolchains.DiscoverAsync(compiler);
            // 모든 구성 × 모든 모듈 최적화 재정의 조합이 AST 허용 목록을 통과해야 한다.
            foreach (NativeConfiguration configuration in Enum.GetValues<NativeConfiguration>())
            {
                foreach (CodeOptimization optimization in Enum.GetValues<CodeOptimization>())
                {
                    string[] arguments = NativeCompileProfile.Arguments(toolchain, configuration, optimization)
                        .Select(argument => "--clang-arg=" + argument)
                        .Append("--clang-arg=-DROCKS_META_PROFILE_PROBE=1").ToArray();
                    await probe.GenerateAsync(Declaration(), extra: arguments);
                }
            }
        });
        suite.Add("Meta.PrefixClassSupportsNestedScopesCommentsAndConditionalBlock", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            string marker = "ROCKS_CLASS()";
            string valid = Preamble + "struct Outer { public:\n#if 1\n" + marker +
                "\n// #if 0 is only a comment\n/* #include <ignored> */\n" +
                "class Inner : public rocks::Object { public: Inner() noexcept = default; };\n#endif\n};\n}\n";
            await probe.GenerateAsync(valid);
            Verify.Contains("probe::Outer::Inner", File.ReadAllText(probe.Output), "nested prefix CLASS");
        });
        suite.Add("Meta.PrefixClassNeverBindsAcrossTokensOrPreprocessing", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            string marker = "ROCKS_CLASS()";
            string definition = "struct Value : public rocks::Object {};\n";
            string[] invalid =
            [
                marker + "\nint intervening;\n" + definition,
                marker + ";\n" + definition,
                marker + "\n" + marker + "\n" + definition,
                marker + "\nstruct [[clang::annotate(\"rocks\", \"rocks.type\")]] Value : public rocks::Object {};\n",
                marker + "\n#if 0\nstruct Intended : public rocks::Object {};\n#endif\n" + definition,
                marker + "\n#include <Meta/Annotations.h>\n" + definition,
                marker + "\ntemplate <class T> struct Value : public rocks::Object {};\n",
                marker + "\nstruct Value;\n",
                marker + "\nnamespace other {}\n" + definition,
                "#define CLASS_WRAP(...) ROCKS_CLASS(__VA_ARGS__)\n" +
                    "CLASS_WRAP()\n" + definition,
                "#define CLASS_BODY struct Value : public rocks::Object {}\n" + marker + "\nCLASS_BODY;\n",
                "void Test() {\n" + marker + "\n" + definition + "}\n",
                "struct ROCKS_CLASS() Value : public rocks::Object {};\n",
                marker + "\n#line 100 \"nonexistent_virtual_source.hpp\"\n" + definition
            ];
            foreach (string body in invalid)
            {
                await probe.RejectAsync(Preamble + body + "}\n", "RMC", "ROCKS_CLASS");
            }
        });
        suite.Add("Meta.PrefixClassCannotBindAcrossIncludeFiles", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            await probe.PrefixDifferentFileAsync();
        });
        suite.Add("Meta.PrivateGeneratedBodyAndUnicodeSpecifiers", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            string source = Preamble + "ROCKS_CLASS()\nclass Value : public rocks::Object {\n" +
                "ROCKS_GENERATED_BODY()\n" + "ROCKS_PROPERTY(ReadOnly, Category=\"능력\", DisplayName=\"체력🎮\") rocks::int32 count = 0;\n" +
                "public: rocks::int32 Read() const noexcept { return count; }\n};\n}\n";
            await probe.GenerateAsync(source);
            string generated = File.ReadAllText(probe.Output);
            Verify.Contains("\"체력🎮\"", generated, "generated display name remains readable UTF-8");
            Verify.Contains("ErrorCode::Unsupported", generated, "ReadOnly public setter policy");
            Verify.Contains("WriteGeneratedStagedField", generated, "ReadOnly staged initialization remains available");
        });
        suite.Add("Meta.AbstractClassHasExplicitMetadata", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            string source = Preamble + "ROCKS_CLASS(Abstract)\nclass Value : public rocks::Object { public:\n" +
                "ROCKS_PROPERTY() rocks::int32 count=0; virtual void Tick() noexcept=0;\n};\n}\n";
            await probe.GenerateAsync(source);
            Verify.Contains("MakeRuntimeTypeOps<::probe::Value, true>", File.ReadAllText(probe.Output), "abstract generated runtime ops");
        });
        AddRejectGroup("Meta.UnknownDuplicateAndWrongValueSpecifiers", new[] { "UnknownFlag", "ReadOnly, ReadOnly", "ReadOnly=\"yes\"", "Category=12", "DisplayName" }
            .Select(spec => Declaration($"ROCKS_PROPERTY({spec}) rocks::int32 count=0;")), "RMC2000", "specifier");
        AddRejectGroup("Meta.UnannotatedOwnerAndParameterMisuse", [
            Preamble + "struct Plain { ROCKS_PROPERTY() rocks::int32 count=0; };\n}\n",
            Preamble + "void F(ROCKS_PROPERTY() rocks::int32 value);\n}\n"], "RMC200", "PROPERTY");
        AddRejectGroup("Meta.UnknownTypeSpecifier", [Declaration().Replace("ROCKS_CLASS()",
            "ROCKS_CLASS(UnknownClassFlag)", StringComparison.Ordinal)], "RMC2000", "specifier");

        void AddRejectGroup(string name, IEnumerable<string> sources, string code, string reason)
        {
            string[] fixedSources = sources.ToArray();
            suite.Add(name, async () =>
            {
                using Probe probe = new(options, actualEnvironment, generator, compiler);
                foreach (string source in fixedSources)
                {
                    await probe.RejectAsync(source, code, reason);
                }
            });
        }
    }

    private sealed class Probe : IDisposable
    {
        private readonly TemporaryDirectory temporary = new();
        private readonly Options options;
        private readonly IReadOnlyDictionary<string, string?> environment;
        private readonly string generator;
        private readonly string compiler;
        public Probe(Options options, IReadOnlyDictionary<string, string?> environment, string generator, string compiler)
        {
            this.options = options;
            this.environment = environment;
            this.generator = generator;
            this.compiler = compiler;
            MetaProbeIdentity.Create(IdentityPath);
        }
        public string Output => temporary.FilePath("Probe.generated.h");
        private string IdentityPath => temporary.FilePath("probe.rocksids.json");

        public async Task GenerateAsync(string? source = null, bool fixture = false, IReadOnlyList<string>? extra = null,
            string? expectedCode = null, string? reason = null)
        {
            string input = fixture ? Path.Combine(options.Root, "Core", "Source", "Runtime", "ObjectRuntime", "Tests", "Reflection", "ReflectionFixture.h") : temporary.FilePath("Probe.h");
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

            arguments.AddRange(["--input", input, "--include", fixture ? "Core/Source/Runtime/ObjectRuntime/Tests/Reflection/ReflectionFixture.h" : "Probe.h",
                "--output", Output, "--compiler", compiler]);
            AddDefaultIncludes(arguments);
            arguments.AddRange(["--identity-manifest", fixture
                ? Path.Combine(options.Root, "Core", "Source", "Runtime", "ObjectRuntime", "Tests", "Reflection", "Foundation.rocksids.json") : IdentityPath]);
            foreach (string argument in options.ClangArguments)
            {
                arguments.Add("--clang-arg=" + argument);
            }

            if (extra is not null)
            {
                arguments.AddRange(extra);
            }

            var result = await ProcessRunner.RunAsync(new(executable, arguments, options.Root, environment, Timeout: TimeSpan.FromSeconds(60)));
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
        public async Task OutputDirectoryBindingAsync()
        {
            string sourceDirectory = temporary.FilePath("Source");
            string generatedDirectory = temporary.FilePath("Generated");
            Directory.CreateDirectory(sourceDirectory);
            Directory.CreateDirectory(generatedDirectory);
            string author = Path.Combine(sourceDirectory, "Probe.h");
            string shadow = Path.Combine(generatedDirectory, "Probe.h");
            string output = Path.Combine(generatedDirectory, "Probe.generated.h");
            File.WriteAllText(author, Declaration());
            File.WriteAllText(shadow, Declaration(extra: "void Shadow() noexcept {}"));
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

                arguments.AddRange(["--input", author, "--include", "Probe.h", "--output", output, "--compiler", compiler]);
                foreach (string include in new[] { sourceDirectory }.Concat(Directory.GetDirectories(Path.Combine(options.Root, "Core", "Source", "Runtime"))
                    .Select(module => Path.Combine(module, "Public")).Where(Directory.Exists)))
                {
                    arguments.AddRange(["--include-dir", include]);
                }

                arguments.AddRange(["--identity-manifest", IdentityPath]);
                foreach (string argument in options.ClangArguments)
                {
                    arguments.Add("--clang-arg=" + argument);
                }

                return await ProcessRunner.RunAsync(new(executable, arguments, options.Root, environment, Timeout: TimeSpan.FromSeconds(60)));
            }

            var rejected = await Run();
            Verify.That(rejected.ExitCode != 0, "output-directory shadow header accepted");
            Verify.Contains("RMC1000", rejected.StdErr, "output shadow error code");
            Verify.Contains(shadow, rejected.StdErr, "actual output-directory shadow path");
            Verify.Equal("previous accepted output\n", File.ReadAllText(output), "shadow failure changed accepted output");
            File.Delete(shadow);
            var accepted = await Run();
            Verify.Equal(0, accepted.ExitCode, "original header not selected after shadow removal: " + accepted.StdErr);
            Verify.That(!Directory.EnumerateFiles(generatedDirectory, ".rocks-meta-validation-*").Any(), "successful validation temporary file leaked");
            string validOutput = File.ReadAllText(output);
            File.WriteAllText(author, Preamble + "ROCKS_CLASS()\nstruct Value : public rocks::Object { Value() {} };\n}\n");
            var invalid = await Run();
            Verify.That(invalid.ExitCode != 0, "invalid generated contract accepted");
            Verify.Contains("RMC3000", invalid.StdErr, "generated contract rejection code");
            Verify.Equal(validOutput, File.ReadAllText(output), "failed validation published output");
            Verify.That(!Directory.EnumerateFiles(generatedDirectory, ".rocks-meta-validation-*").Any(), "failed validation temporary file leaked");
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
            async Task<ProcessResult> Run(string input, string output, params string[] includeDirs)
            {
                List<string> arguments = [];
                string executable = generator;
                if (generator.EndsWith(".dll", StringComparison.OrdinalIgnoreCase))
                {
                    executable = options.Optional("dotnet") ?? "dotnet";
                    arguments.Add(generator);
                }

                arguments.AddRange(["--input", input, "--include", "Probe.h", "--output", output, "--compiler", compiler]);
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

                return await ProcessRunner.RunAsync(new(executable, arguments, options.Root, environment, Timeout: TimeSpan.FromSeconds(60)));
            }

            var valid = await Run(aliasInput, Output, real);
            Verify.Equal(0, valid.ExitCode, "same file through symlinked parent rejected: " + valid.StdErr);
            var shadowed = await Run(aliasInput, Output, shadow, real);
            Verify.That(shadowed.ExitCode != 0, "different shadow header accepted");
            Verify.Contains("resolved input=", shadowed.StdErr, "resolved input path missing");
            Verify.Contains("resolved target=", shadowed.StdErr, "resolved shadow target missing");
            Verify.Contains(Path.Combine(shadow, "Probe.h"), shadowed.StdErr, "shadow header path missing");
            var sameOutput = await Run(actualInput, aliasInput, real);
            Verify.That(sameOutput.ExitCode != 0, "input/output symlink alias was accepted");
            Verify.Contains("RMC1000", sameOutput.StdErr, "same input/output rejection code");
            Verify.Equal(original, File.ReadAllText(actualInput), "source header changed after rejected output alias");
        }
        public async Task SourceNotesAsync()
        {
            string input = temporary.FilePath("Probe.h");
            string source = "#pragma once\n#include <Meta/Annotations.h>\n#include <Object/Object.h>\n" +
                "namespace a { ROCKS_CLASS()\nstruct Thing : public rocks::Object { Thing() {} }; }\n" +
                "namespace b { ROCKS_CLASS()\nstruct Thing : public rocks::Object {}; }\n" +
                "namespace a { ROCKS_CLASS()\nstruct ThingExtended : public rocks::Object {}; }\n";
            File.WriteAllText(input, source);
            List<string> arguments = [];
            string executable = generator;
            if (generator.EndsWith(".dll", StringComparison.OrdinalIgnoreCase))
            {
                executable = options.Optional("dotnet") ?? "dotnet";
                arguments.Add(generator);
            }

            arguments.AddRange(["--input", input, "--include", "Probe.h", "--output", Output, "--compiler", compiler]);
            AddDefaultIncludes(arguments);
            arguments.AddRange(["--identity-manifest", IdentityPath]);
            foreach (string argument in options.ClangArguments)
            {
                arguments.Add("--clang-arg=" + argument);
            }

            var result = await ProcessRunner.RunAsync(new(executable, arguments, options.Root, environment, Timeout: TimeSpan.FromSeconds(60)));
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

                arguments.AddRange(["--input", main, "--include", "Main.h", "--output", Output, "--compiler", compiler]);
                AddDefaultIncludes(arguments);
                arguments.AddRange(["--identity-manifest", IdentityPath]);
                foreach (string argument in options.ClangArguments)
                {
                    arguments.Add("--clang-arg=" + argument);
                }

                var result = await ProcessRunner.RunAsync(new(executable, arguments, options.Root, environment, Timeout: TimeSpan.FromSeconds(60)));
                Verify.That(result.ExitCode != 0, "invalid nested declaration was accepted");
                Verify.Contains($"{bad}:{line}: error RMC2000", result.StdErr, "nested source file and physical line");
                Verify.Contains("hint:", result.StdErr, "nested declaration correction guidance");
            }
        }
        public async Task RejectAsync(string source, string code, string reason)
        {
            byte[] previous = Encoding.UTF8.GetBytes("previous accepted output\n");
            File.WriteAllBytes(Output, previous);
            await GenerateAsync(source, expectedCode: code, reason: reason);
            Verify.That(File.ReadAllBytes(Output).AsSpan().SequenceEqual(previous), "rejected input changed previous accepted output");
            Verify.That(!Directory.EnumerateFiles(temporary.Root, "*.tmp*").Any(), "temporary generated output leaked");
        }

        private void AddDefaultIncludes(List<string> arguments)
        {
            foreach (string directory in Directory.GetDirectories(Path.Combine(options.Root, "Core", "Source", "Runtime"))
                .Select(module => Path.Combine(module, "Public")).Where(Directory.Exists).Order(StringComparer.Ordinal)
                .Concat([options.Root, temporary.Root]))
            {
                arguments.AddRange(["--include-dir", directory]);
            }
        }

        public void Dispose() => temporary.Dispose();
    }
}

// 각 테스트는 실제 도구와 동일한 읽기 전용 명세를 제공한다. 테스트 데이터의 ID는
// 저작 API가 한 번 발급하며 선언 본문에 GUID를 직접 넣지 않는다.
internal static class MetaProbeIdentity
{
    public static void Create(string path)
    {
        StableIdManifest.Update(path, true, manifest =>
        {
            foreach (string name in new[] { "probe::Value", "probe::Outer::Inner", "probe::Object", "probe::Parent",
                "first::Good", "example::Invalid", "a::Thing", "b::Thing", "a::ThingExtended" })
            {
                manifest.Register("type", name);
            }

            foreach (string name in new[] { "probe::Value::count", "probe::Value::next", "probe::Object::value",
                "example::Invalid::valid", "example::Invalid::bad" })
            {
                manifest.Register("field", name);
            }

            return 0;
        });
    }
}
