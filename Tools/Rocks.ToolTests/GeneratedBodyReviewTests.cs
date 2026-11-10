using System.Text;
using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

internal static class GeneratedBodyReviewTests
{
    private const string Preamble = "#include <Meta/Annotations.h>\n#include <Object/Object.h>\nnamespace probe {\n";
    private const string ValidClass = "ROCKS_CLASS(\"a7100000000000000000000000000001\", 1)\n" +
        "class Object : public rocks::Object {\n" +
        "    ROCKS_GENERATED_BODY()\n" +
        "    ROCKS_PROPERTY(\"a7100000000000000000000000000002\") int value = 7;\n" +
        "public: int Read() const noexcept { return value; }\n};\n";

    public static void Register(TestSuite suite, Options options, IReadOnlyDictionary<string, string?> environment)
    {
        suite.Add("Meta.GeneratedBodyWorksInReflectedClass", async () =>
        {
            using Probe probe = new(options, environment);
            ProcessResult result = await probe.Run(Preamble + ValidClass + "}\n");
            Verify.Equal(0, result.ExitCode, "valid generated body rejected: " + result.StdErr);
            Verify.That(File.Exists(probe.Output), "valid generated body did not publish output");
        });

        suite.Add("Meta.GeneratedBodyWorksWithReflectTypeAlias", async () =>
        {
            using Probe probe = new(options, environment);
            string source = Preamble + ValidClass.Replace("ROCKS_CLASS", "ROCKS_REFLECT_TYPE", StringComparison.Ordinal)
                .Replace("ROCKS_PROPERTY", "ROCKS_REFLECT_FIELD", StringComparison.Ordinal) + "}\n";
            ProcessResult result = await probe.Run(source);
            Verify.Equal(0, result.ExitCode, "prefix alias with private field rejected: " + result.StdErr);
            Verify.Contains("probe::Object", File.ReadAllText(probe.Output), "alias metadata missing");
        });

        suite.Add("Meta.GeneratedBodyOnUnreflectedClassIsRejected", async () =>
        {
            using Probe probe = new(options, environment);
            string source = Preamble + "struct Plain {\n    ROCKS_GENERATED_BODY()\n};\n" + ValidClass + "}\n";
            await probe.Reject(source, "Probe.h:5:", "ROCKS_GENERATED_BODY");
        });

        suite.Add("Meta.GeneratedBodyOnUnreflectedNestedClassIsRejected", async () =>
        {
            using Probe probe = new(options, environment);
            string source = Preamble + "ROCKS_CLASS(\"a7100000000000000000000000000001\", 1)\n" +
                "class Parent : public rocks::Object {\npublic:\n" +
                "    struct Child {\n        ROCKS_GENERATED_BODY()\n    };\n};\n}\n";
            await probe.Reject(source, "Probe.h:8:", "ROCKS_GENERATED_BODY");
        });

        suite.Add("Meta.GeneratedBodyRejectsForgedAndWrappedMarkers", async () =>
        {
            using Probe probe = new(options, environment);
            string prefix = Preamble + "ROCKS_CLASS(\"a7100000000000000000000000000001\", 1)\n" +
                "struct Object : rocks::Object {\n";
            string forged = prefix + "    [[clang::annotate(\"rocks\", \"rocks.generated\")]] " +
                "static void rocksGeneratedAccessMarker() noexcept;\n};\n}\n";
            await probe.Reject(forged, "Probe.h:6:", "ROCKS_GENERATED_BODY");
            string wrapper = Preamble + "#define MY_BODY() ROCKS_GENERATED_BODY()\n" +
                "ROCKS_CLASS(\"a7100000000000000000000000000001\", 1)\n" +
                "struct Object : rocks::Object {\n    MY_BODY()\n};\n}\n";
            await probe.Reject(wrapper, "Probe.h:7:", "ROCKS_GENERATED_BODY");
        });

        suite.Add("Meta.GeneratedBodyRejectsMalformedMarkerAndDuplicates", async () =>
        {
            using Probe probe = new(options, environment);
            string[] markers =
            [
                "[[clang::annotate(\"rocks\", \"rocks.generated\")]] static int value;",
                "[[clang::annotate(\"rocks\", \"rocks.generated\")]] static void other() noexcept;",
                "[[clang::annotate(\"rocks\", \"rocks.generated\")]] void rocksGeneratedAccessMarker() noexcept;",
                "[[clang::annotate(\"rocks\", \"rocks.generated\")]] static void rocksGeneratedAccessMarker();",
                "[[clang::annotate(\"rocks\", \"rocks.generated\")]] static void rocksGeneratedAccessMarker(int) noexcept;",
                "[[clang::annotate(\"rocks\", \"rocks.generated\", \"unexpected\")]] static void rocksGeneratedAccessMarker() noexcept;",
                "[[clang::annotate(\"rocks\", \"rocks.generated\")]] static void rocksGeneratedAccessMarker() noexcept;\n" +
                "[[clang::annotate(\"rocks\", \"rocks.generated\")]] static void another() noexcept;"
            ];
            for (int index = 0; index < markers.Length; ++index)
            {
                string source = Preamble + "ROCKS_CLASS(\"a7100000000000000000000000000001\", 1)\n" +
                    "struct Object : rocks::Object {\n    " + markers[index] + "\n};\n}\n";
                string expectedLocation = index == markers.Length - 1 ? "Probe.h:7:" : "Probe.h:6:";
                await probe.Reject(source, expectedLocation, "ROCKS_GENERATED_BODY");
            }
        });
    }

    private sealed class Probe(Options options, IReadOnlyDictionary<string, string?> environment) : IDisposable
    {
        private readonly TemporaryDirectory temporary = new();
        public string Output => temporary.FilePath("Probe.generated.h");

        public async Task<ProcessResult> Run(string source)
        {
            temporary.Write("Probe.h", source);
            string generator = Path.GetFullPath(options.Required("metacompiler"));
            string executable = generator;
            List<string> arguments = [];
            if (generator.EndsWith(".dll", StringComparison.OrdinalIgnoreCase))
            {
                executable = options.Optional("dotnet") ?? "dotnet";
                arguments.Add(generator);
            }
            arguments.AddRange(["--input", temporary.FilePath("Probe.h"), "--include", "Probe.h",
                "--output", Output, "--compiler", options.Required("compiler")]);
            foreach (string argument in options.ClangArguments)
                arguments.Add("--clang-arg=" + argument);
            return await ProcessRunner.RunAsync(new(executable, arguments, options.Root, environment,
                Timeout: TimeSpan.FromSeconds(60)));
        }

        public async Task Reject(string source, string location, string reason)
        {
            const string previous = "previous accepted output\n";
            File.WriteAllText(Output, previous, new UTF8Encoding(false));
            ProcessResult result = await Run(source);
            Verify.That(result.ExitCode != 0, "invalid generated body accepted");
            Verify.Contains("RMC2000", result.StdErr, "generated body diagnostic code");
            Verify.Contains(reason, result.StdErr, "generated body diagnostic reason");
            Verify.Contains(location, result.StdErr, "generated body source location");
            Verify.Contains("hint:", result.StdErr, "generated body correction hint");
            Verify.Equal(previous, File.ReadAllText(Output), "rejected generated body changed output");
        }

        public void Dispose() => temporary.Dispose();
    }
}
