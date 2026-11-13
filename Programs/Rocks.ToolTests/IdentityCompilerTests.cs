using System.Text;
using System.Text.Json;
using Rocks.MetaCompiler;
using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

// 실제 Clang과 메타컴파일러로 자동 식별자, 선언 위치, 검증 실패 시 출력 보존을 검사한다.
internal static class IdentityCompilerTests
{
    private const string TypeId = "b1000000000000000000000000000001";
    private const string FieldId = "b1000000000000000000000000000011";
    private const string Prefix = "#include <Meta/Annotations.h>\n#include <Object/Object.h>\nnamespace probe {\n";

    private static string Source(string type, string field) => Prefix + type +
        "\nstruct Value : public rocks::Object {\n" + field + " rocks::int32 count = 7;\n};\n}\n";

    public static void Register(TestSuite suite, Options options, IReadOnlyDictionary<string, string?> environment)
    {
        suite.Add("Meta.IdentityAutomaticDeterminismAndClassSpecifiers", async () =>
        {
            using var probe = new Probe(options, environment);
            probe.Manifest(TypeId, FieldId);
            byte[] manifestBefore = File.ReadAllBytes(probe.ManifestPath);
            probe.Source(Source("ROCKS_CLASS()", "ROCKS_PROPERTY(ReadOnly, Category = \"Game\")"));
            await probe.Success("--identity-manifest", probe.ManifestPath);
            byte[] generated = File.ReadAllBytes(probe.Output);
            string text = Encoding.UTF8.GetString(generated);
            Verify.Contains("0xb100000000000000ull, 0x0000000000000001ull", text, "type ID");
            Verify.Contains("0xb100000000000000ull, 0x0000000000000011ull", text, "field ID");
            Verify.Contains("Id(),   1,", text, "internal class version");
            await probe.Success("--identity-manifest", probe.ManifestPath);
            Verify.That(generated.AsSpan().SequenceEqual(File.ReadAllBytes(probe.Output)), "unchanged metadata output changed");
            Verify.That(manifestBefore.AsSpan().SequenceEqual(File.ReadAllBytes(probe.ManifestPath)), "build mutated identity manifest");
            probe.Source(Source("ROCKS_CLASS(Abstract)", "ROCKS_PROPERTY()"));
            await probe.Success("--identity-manifest", probe.ManifestPath);
            Verify.Contains("Id(),   1,", File.ReadAllText(probe.Output), "abstract class internal version");
        });
        suite.Add("Meta.IdentityMissingOrUnsupportedSyntaxDoesNotPublish", async () =>
        {
            using var probe = new Probe(options, environment);
            probe.Source(Source("ROCKS_CLASS()", "ROCKS_PROPERTY()"));
            probe.WritePrevious("previous output");
            await probe.Failure("requires an identity manifest");
            probe.Manifest(TypeId, FieldId);
            foreach (var value in new[] {
                ("ROCKS_CLASS(1)", "ROCKS_PROPERTY()"),
                ("ROCKS_CLASS(\"b1000000000000000000000000000001\", 1)", "ROCKS_PROPERTY()"),
                ("ROCKS_CLASS((Abstract))", "ROCKS_PROPERTY()"),
                ("ROCKS_CLASS(Abstract + 2)", "ROCKS_PROPERTY()"),
                ("ROCKS_CLASS(Abstract, Abstract)", "ROCKS_PROPERTY()"),
                ("ROCKS_CLASS()", "ROCKS_PROPERTY(ReadOnly,)"),
                ("ROCKS_CLASS()", "ROCKS_PROPERTY(\"b1000000000000000000000000000011\")") })
            {
                probe.Source(Source(value.Item1, value.Item2));
                await probe.Failure("RMC2000", "--identity-manifest", probe.ManifestPath);
                Verify.Equal("previous output", File.ReadAllText(probe.Output), "invalid declaration overwrote output");
            }
        });
        suite.Add("Meta.IdentityInventoryHasPhysicalDeclarationSites", async () =>
        {
            using var probe = new Probe(options, environment);
            probe.Manifest(TypeId, FieldId);
            probe.Source(Source("ROCKS_CLASS()", "ROCKS_PROPERTY()"));
            await probe.Success("--identity-manifest", probe.ManifestPath, "--identity-inventory", probe.InventoryPath);
            using var json = JsonDocument.Parse(File.ReadAllBytes(probe.InventoryPath));
            Verify.Equal(1, json.RootElement.GetProperty("schema").GetInt32(), "inventory schema");
            var entries = json.RootElement.GetProperty("declarations").EnumerateArray().ToArray();
            Verify.Equal(2, entries.Length, "inventory count");
            var type = entries.Single(x => x.GetProperty("kind").GetString() == "type");
            var field = entries.Single(x => x.GetProperty("kind").GetString() == "field");
            Verify.Equal("probe::Value", type.GetProperty("name").GetString(), "type inventory name");
            Verify.Equal(TypeId, type.GetProperty("id").GetString(), "type inventory ID");
            Verify.Equal(5, type.GetProperty("line").GetInt32(), "class definition location");
            Verify.Equal("probe::Value::count", field.GetProperty("name").GetString(), "field inventory name");
            Verify.Equal(FieldId, field.GetProperty("id").GetString(), "field inventory ID");
            Verify.Equal(6, field.GetProperty("line").GetInt32(), "field definition location");
            Verify.Equal(CanonicalPath.Resolve(probe.Input), CanonicalPath.Resolve(type.GetProperty("file").GetString()!), "type physical file");
            Verify.Equal(CanonicalPath.Resolve(probe.Input), CanonicalPath.Resolve(field.GetProperty("file").GetString()!), "field physical file");
            byte[] previous = File.ReadAllBytes(probe.InventoryPath);
            probe.Source(Source("ROCKS_CLASS(Invalid)", "ROCKS_PROPERTY()"));
            await probe.Failure("specifier", "--identity-manifest", probe.ManifestPath, "--identity-inventory", probe.InventoryPath);
            Verify.That(previous.AsSpan().SequenceEqual(File.ReadAllBytes(probe.InventoryPath)), "failed compilation overwrote inventory");
        });
        suite.Add("Meta.PropertyDirectPrefixAndSourceBoundaries", async () =>
        {
            using var probe = new Probe(options, environment);
            probe.Manifest(TypeId, FieldId);
            string multiline = "ROCKS_PROPERTY(\n    ReadOnly, // flag\n    Category = \"Game\"\n) /* separate type */";
            probe.Source(Source("ROCKS_CLASS()", multiline));
            await probe.Success("--identity-manifest", probe.ManifestPath);
            probe.Source(Prefix + "ROCKS_CLASS()\nclass Value : public rocks::Object {\n" +
                "    ROCKS_GENERATED_BODY()\nprivate:\n#if 1\n    " + multiline +
                " rocks::int32 count = 7;\n#endif\n};\n}\n");
            await probe.Success("--identity-manifest", probe.ManifestPath);

            probe.Source(Source("ROCKS_CLASS()", "ROCKS_PROPERTY()\n#if 1\n") + "\n#endif\n");
            await probe.FailureAt("direct prefix", 6, "--identity-manifest", probe.ManifestPath);
            probe.Source(Prefix + "#define WRAP(...) ROCKS_PROPERTY(__VA_ARGS__)\n" +
                "ROCKS_CLASS()\nstruct Value : public rocks::Object {\n    WRAP() rocks::int32 count = 7;\n};\n}\n");
            await probe.FailureAt("direct prefix", 7, "--identity-manifest", probe.ManifestPath);
            probe.Source(Source("ROCKS_CLASS()", "[[clang::annotate(\"rocks\", \"rocks.field.arguments\", \"\")]]"));
            await probe.Failure("direct prefix", "--identity-manifest", probe.ManifestPath);
            probe.Source(Prefix + "ROCKS_CLASS()\nstruct Value : public rocks::Object {\n" +
                "    ROCKS_PROPERTY() rocks::int32 count = 7, other = 8;\n};\n}\n");
            await probe.Failure("exactly one field", "--identity-manifest", probe.ManifestPath);
        });
        suite.Add("Meta.IdentityManifestMutationNeverPublishesGeneratedOutput", async () =>
        {
            using var probe = new Probe(options, environment);
            probe.Source(Source("ROCKS_CLASS()", "ROCKS_PROPERTY()"));
            foreach (bool duringLoad in new[] { true, false })
            {
                probe.Manifest(TypeId, FieldId);
                probe.WritePrevious("previous output");
                await probe.MutationFailure(duringLoad);
                Verify.Equal("previous output", File.ReadAllText(probe.Output), "changed manifest published generated output");
            }
        }, exclusive: true);
    }

    private sealed class Probe : IDisposable
    {
        private readonly TemporaryDirectory temporary = new();
        private readonly Options options;
        private readonly IReadOnlyDictionary<string, string?> environment;

        public Probe(Options options, IReadOnlyDictionary<string, string?> environment)
        {
            this.options = options;
            this.environment = environment;
        }

        public string Input => temporary.FilePath("Probe.h");

        public string Output => MetaCompilerArguments.GeneratedPath(temporary.Root, "Probe.h");

        // 실패한 실행이 덮어쓰지 않아야 하는 이전 출력을 만든다.
        public void WritePrevious(string text)
        {
            Directory.CreateDirectory(Path.GetDirectoryName(Output)!);
            File.WriteAllText(Output, text);
        }

        public string ManifestPath => temporary.FilePath("identities.rocksids.json");

        public string InventoryPath => temporary.FilePath("inventory.json");

        public void Source(string text) => temporary.Write("Probe.h", text);

        public void Manifest(string typeId, string fieldId)
        {
            var entries = new[] {
                new { kind = "type", name = "probe::Value", id = typeId, aliases = Array.Empty<string>(), retired = false },
                new { kind = "field", name = "probe::Value::count", id = fieldId, aliases = Array.Empty<string>(), retired = false }
            };
            temporary.Write("identities.rocksids.json", JsonSerializer.Serialize(new { schema = 1, entries }));
        }

        private async Task<ProcessResult> Run(params string[] extra)
        {
            string generator = Path.GetFullPath(options.Required("metacompiler"));
            string executable = generator.EndsWith(".dll", StringComparison.OrdinalIgnoreCase) ? options.Optional("dotnet") ?? "dotnet" : generator;
            List<string> args = [];
            if (executable != generator)
            {
                args.Add(generator);
            }

            args.AddRange([.. MetaCompilerArguments.Single(temporary.Root, Input, "Probe.h"), "--compiler", options.Required("compiler")]);
            foreach (string argument in options.ClangArguments)
            {
                args.Add("--clang-arg=" + argument);
            }

            args.AddRange(extra);
            return await ProcessRunner.RunAsync(new(executable, args, options.Root, environment, Timeout: TimeSpan.FromSeconds(60)));
        }

        public async Task Success(params string[] extra)
        {
            ProcessResult result = await Run(extra);
            Verify.Equal(0, result.ExitCode, "metadata generation: " + result.StdErr);
        }

        public async Task Failure(string message, params string[] extra)
        {
            ProcessResult result = await Run(extra);
            Verify.That(result.ExitCode != 0, "invalid identity input accepted");
            Verify.Contains(message, result.StdErr, "identity diagnostic");
            Verify.Contains("hint:", result.StdErr, "identity diagnostic hint");
        }

        public async Task FailureAt(string message, int line, params string[] extra)
        {
            ProcessResult result = await Run(extra);
            Verify.That(result.ExitCode != 0, "invalid identity input accepted");
            Verify.Contains(message, result.StdErr, "identity diagnostic");
            Verify.Contains(Input.Replace('\\', '/') + ":" + line + ": error",
                result.StdErr.Replace('\\', '/'), "identity source location");
        }

        public async Task MutationFailure(bool duringLoad)
        {
            List<string> arguments = [.. MetaCompilerArguments.Single(temporary.Root, Input, "Probe.h"),
                "--compiler", options.Required("compiler"), "--identity-manifest", ManifestPath, "--include-dir", temporary.Root];
            foreach (string directory in Directory.GetDirectories(Path.Combine(options.Root, "Core", "Source", "Runtime")))
            {
                string include = Path.Combine(directory, "Public");
                if (Directory.Exists(include))
                {
                    arguments.AddRange(["--include-dir", include]);
                }
            }

            foreach (string argument in options.ClangArguments)
            {
                arguments.Add("--clang-arg=" + argument);
            }

            string? oldFormatter = Environment.GetEnvironmentVariable("ROCKS_CLANG_FORMAT");
            if (options.Optional("formatter") is string formatter)
            {
                Environment.SetEnvironmentVariable("ROCKS_CLANG_FORMAT", formatter);
            }

            try
            {
                Action mutate = () => File.AppendAllText(ManifestPath, " ");
                try
                {
                    await CompilerDriver.RunAsync(CompilerOptions.Parse(arguments.ToArray()),
                        afterManifestRead: duringLoad ? mutate : null, beforeManifestVerification: duringLoad ? null : mutate);
                    throw new InvalidOperationException("changed manifest was accepted");
                }
                catch (MetadataException error)
                {
                    Verify.Equal("RMC2100", error.Code, "manifest mutation diagnostic code");
                    Verify.Contains("changed during metadata generation", error.Message, "manifest mutation diagnostic");
                }
            }
            finally
            {
                Environment.SetEnvironmentVariable("ROCKS_CLANG_FORMAT", oldFormatter);
            }
        }

        public void Dispose() => temporary.Dispose();
    }
}
