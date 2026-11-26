using System.Text;
using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

// MetaCompiler 셰이더 파라미터 모드 시험이다(Docs/Design/ShaderSystem 16.2절 SSD58 2): 양성 생성물(C++ 표·static_assert·HLSL)과
// 금지 타입·배치·지정자·상한의 음성 진단.
internal static partial class MetaTests
{
    private const string ShaderParameterPreamble = "#pragma once\n#include <ShaderCore/ShaderParameterTypes.h>\nnamespace probe {\nusing namespace rocks::shader;\n";

    private sealed class ShaderParameterProbe(Options options, IReadOnlyDictionary<string, string?> environment, string generator, string compiler)
        : IDisposable
    {
        private readonly TemporaryDirectory temporary = new();

        public string LastStdErr { get; private set; } = "";

        public string OutputRoot => temporary.FilePath("out");

        public string Header => File.ReadAllText(Path.Combine(OutputRoot, "Include", "Probe.shaderparams.h"));

        public string Shader(string name) => File.ReadAllText(Path.Combine(OutputRoot, "Shaders", name + ".rsh"));

        public async Task<int> RunAsync(string body, string? preamble = null)
        {
            temporary.Write("Probe.h", (preamble ?? ShaderParameterPreamble) + body + "}\n");
            List<string> arguments = [];
            string executable = generator;
            if (generator.EndsWith(".dll", StringComparison.OrdinalIgnoreCase))
            {
                executable = options.Optional("dotnet") ?? "dotnet";
                arguments.Add(generator);
            }

            arguments.AddRange(["shader-parameters", "--module", "Probe", "--input", temporary.FilePath("Probe.h"), "--include", "Probe.h",
                "--output-root", OutputRoot, "--compiler", compiler]);
            foreach (string directory in Directory.GetDirectories(Path.Combine(options.Root, "Core", "Source", "Runtime"))
                .Select(module => Path.Combine(module, "Public")).Where(Directory.Exists).Order(StringComparer.Ordinal)
                .Append(Path.Combine(options.Root, "Engine", "Source", "Runtime", "ShaderCore", "Public")).Append(temporary.Root))
            {
                arguments.AddRange(["--include-dir", directory]);
            }

            foreach (string argument in options.ClangArguments)
            {
                arguments.Add("--clang-arg=" + argument);
            }

            ProcessResult result = await ProcessRunner.RunAsync(new(executable, arguments, options.Root, environment, Timeout: ProbeTimeout));
            LastStdErr = result.StdErr;
            return result.ExitCode;
        }

        public async Task RejectAsync(string body, string code, string reason, string? preamble = null)
        {
            int exit = await RunAsync(body, preamble);
            Verify.That(exit != 0, "invalid shader parameter input accepted:\n" + body);
            Verify.Contains(code, LastStdErr, "stable shader parameter diagnostic code");
            Verify.Contains("hint:", LastStdErr, "actionable diagnostic hint");
            Verify.That(LastStdErr.Contains(reason, StringComparison.OrdinalIgnoreCase), "diagnostic reason missing: " + reason + "\n" + LastStdErr);
        }

        public void Dispose() => temporary.Dispose();
    }

    private static void RegisterShaderParameters(MetaContext context)
    {
        var (suite, options, actualEnvironment, generator, compiler) = context;
        suite.Add("Meta.ShaderParametersGenerateTablesAndHlsl", async () =>
        {
            using ShaderParameterProbe probe = new(options, actualEnvironment, generator, compiler);
            int exit = await probe.RunAsync(
                "ROCKS_SHADER_PARAMETERS()\nstruct alignas(16) Inner {\nShaderFloat3 direction;\nfloat intensity;\n};\n" +
                "ROCKS_SHADER_PARAMETERS(Name = \"Pass\")\nstruct alignas(16) PassParameters {\nstatic constexpr float Default = 1.0f;\n" +
                "ShaderFloat4x4 transform;\nShaderFloat3 tint;\nfloat exposure;\nShaderInt3 offsets;\nrocks::int32 layer;\nShaderFloat4 values[2];\nInner inner;\n};\n" +
                "ROCKS_SHADER_PARAMETERS(Name = \"Draw\", RootConstants)\nstruct alignas(16) DrawParameters {\nShaderFloat4 color;\nrocks::uint32 id;\n};\n");
            Verify.Equal(0, exit, probe.LastStdErr);
            string header = probe.Header;
            Verify.Contains("struct ShaderParameterStruct<::probe::PassParameters>", header, "trait specialization");
            Verify.Contains("{\"rocksPadding0\", ShaderParameterType::Float, 76, 0}", header, "float padding after a 3-component vector");
            Verify.Contains("{\"rocksPadding1\", ShaderParameterType::Float, 84, 0}", header, "gaps the D3D packing could fill are padded");
            Verify.Contains("{\"rocksPadding4\", ShaderParameterType::Int, 108, 0}", header, "int padding keeps the slot base type");
            Verify.Contains("{\"values\", ShaderParameterType::Float4, 128, 2}", header, "array element count");
            Verify.Contains("{\"inner\", ShaderParameterType::Struct, 160, 0, &ShaderParameterStruct<::probe::Inner>::Layout}", header, "nested struct link");
            Verify.Contains("ROCKS_OFFSET_OF(::probe::PassParameters, exposure) == 80", header, "offset static_assert");
            Verify.Contains("\"probe::PassParameters\", \"/Engine/Generated/Probe/PassParameters.rsh\"", header, "identity and virtual path");
            Verify.That(!header.Contains("Default", StringComparison.Ordinal), "static members are not parameters");
            string pass = probe.Shader("PassParameters");
            Verify.Contains("cbuffer Pass {\n    float4x4 transform;\n    float3 tint;\n    float rocksPadding0;\n    float exposure;\n", pass, "cbuffer members");
            Verify.Contains("#ifndef ROCKS_SHADER_STRUCT_Inner\n#define ROCKS_SHADER_STRUCT_Inner\nstruct Inner {\n", pass, "nested struct definition");
            Verify.Contains("    Inner inner;\n};\n", pass, "nested member");
            Verify.Contains("ROCKS_ROOT_CONSTANTS(DrawParameters, Draw);", probe.Shader("DrawParameters"), "root constants declaration");
            // 지운 구조체의 생성물은 다음 실행이 지운다.
            Verify.Equal(0, await probe.RunAsync("ROCKS_SHADER_PARAMETERS()\nstruct alignas(16) Only {\nShaderFloat4 value;\n};\n"), probe.LastStdErr);
            Verify.That(!File.Exists(Path.Combine(probe.OutputRoot, "Shaders", "PassParameters.rsh")), "stale generated shader source is removed");
        });

        // 금지 입력이다(7.3절 (f)를 생성 구조체로).
        (string Name, string Body, string Code, string Reason)[] rejected =
        [
            ("Bool", "ROCKS_SHADER_PARAMETERS()\nstruct alignas(16) S {\nbool flag;\n};\n", "RMC6007", "not a shader parameter type"),
            ("Double", "ROCKS_SHADER_PARAMETERS()\nstruct alignas(16) S {\ndouble value;\n};\n", "RMC6007", "not a shader parameter type"),
            ("Pointer", "ROCKS_SHADER_PARAMETERS()\nstruct alignas(16) S {\nfloat* value;\n};\n", "RMC6007", "not a shader parameter type"),
            ("ScalarArray", "ROCKS_SHADER_PARAMETERS()\nstruct alignas(16) S {\nfloat values[4];\n};\n", "RMC6007", "16-byte elements"),
            ("TwoDimensional", "ROCKS_SHADER_PARAMETERS()\nstruct alignas(16) S {\nShaderFloat4 values[2][2];\n};\n", "RMC6007", "multi-dimensional"),
            ("StructArray", "ROCKS_SHADER_PARAMETERS()\nstruct alignas(16) I {\nShaderFloat4 v;\n};\nROCKS_SHADER_PARAMETERS()\nstruct alignas(16) S {\nI items[2];\n};\n",
                "RMC6007", "arrays of structs"),
            ("TwoLevels", "ROCKS_SHADER_PARAMETERS()\nstruct alignas(16) A {\nShaderFloat4 v;\n};\nROCKS_SHADER_PARAMETERS()\nstruct alignas(16) B {\nA a;\n};\n" +
                "ROCKS_SHADER_PARAMETERS()\nstruct alignas(16) C {\nB b;\n};\n", "RMC6007", "one level"),
            ("RootNested", "ROCKS_SHADER_PARAMETERS()\nstruct alignas(16) A {\nShaderFloat4 v;\n};\nROCKS_SHADER_PARAMETERS(Name = \"R\", RootConstants)\nstruct alignas(16) B {\nA a;\n};\n",
                "RMC6007", "root constants take"),
            ("RootTooLarge", "ROCKS_SHADER_PARAMETERS(Name = \"R\", RootConstants)\nstruct alignas(16) S {\nShaderFloat4x4 a;\nShaderFloat4x4 b;\nShaderFloat4 c;\n};\n",
                "RMC6006", "root constants limit"),
            ("RootSameName", "ROCKS_SHADER_PARAMETERS(RootConstants)\nstruct alignas(16) S {\nShaderFloat4 v;\n};\n", "RMC6005", "different from the struct name"),
            ("NotSixteen", "ROCKS_SHADER_PARAMETERS()\nstruct S {\nfloat value;\n};\n", "RMC6006", "multiple of 16"),
            ("Align32", "ROCKS_SHADER_PARAMETERS()\nstruct alignas(32) S {\nShaderFloat4 value;\n};\n", "RMC6006", "alignment"),
            ("Base", "struct B {\nShaderFloat4 v;\n};\nROCKS_SHADER_PARAMETERS()\nstruct alignas(16) S : B {\nShaderFloat4 w;\n};\n", "RMC6002", "bases"),
            ("Template", "ROCKS_SHADER_PARAMETERS()\ntemplate <class T>\nstruct alignas(16) S {\nShaderFloat4 v;\n};\n", "RMC6002", "template"),
            ("Function", "ROCKS_SHADER_PARAMETERS()\nstruct alignas(16) S {\nShaderFloat4 v;\nvoid Touch() noexcept {}\n};\n", "RMC6003", "data members only"),
            ("UnknownSpecifier", "ROCKS_SHADER_PARAMETERS(Version = 2)\nstruct alignas(16) S {\nShaderFloat4 v;\n};\n", "RMC6005", "unknown ROCKS_SHADER_PARAMETERS specifier"),
            ("BadName", "ROCKS_SHADER_PARAMETERS(Name = \"1st\")\nstruct alignas(16) S {\nShaderFloat4 v;\n};\n", "RMC6005", "identifier"),
            ("NameClash", "ROCKS_SHADER_PARAMETERS(Name = \"Same\")\nstruct alignas(16) A {\nShaderFloat4 v;\n};\nROCKS_SHADER_PARAMETERS(Name = \"Same\")\nstruct alignas(16) B {\nShaderFloat4 v;\n};\n",
                "RMC6004", "distinct Name"),
            ("MixedSlot", "ROCKS_SHADER_PARAMETERS()\nstruct alignas(16) S {\nfloat a;\nrocks::int32 b;\nShaderFloat2 c;\n};\n", "RMC6008", "must not mix"),
            ("PaddingName", "ROCKS_SHADER_PARAMETERS()\nstruct alignas(16) S {\nShaderFloat4 rocksPadding0;\n};\n", "RMC6003", "must not start"),
        ];
        foreach ((string name, string body, string code, string reason) in rejected)
        {
            suite.Add("Meta.ShaderParametersReject" + name, async () =>
            {
                using ShaderParameterProbe probe = new(options, actualEnvironment, generator, compiler);
                await probe.RejectAsync(body, code, reason);
            });
        }

        // 정렬 타입이 아닌 값 타입(Core Matrix4·Vector3와 같은 꼴 — 정렬 지정 없는 일반 struct)은 받지 않는다.
        suite.Add("Meta.ShaderParametersRejectPlainValueStructAndTooManyMembers", async () =>
        {
            using ShaderParameterProbe probe = new(options, actualEnvironment, generator, compiler);
            await probe.RejectAsync("struct Matrix4 {\nfloat m[16];\n};\nROCKS_SHADER_PARAMETERS()\nstruct alignas(16) S {\nMatrix4 m;\n};\n",
                "RMC6007", "not a shader parameter type");
            var many = new StringBuilder("ROCKS_SHADER_PARAMETERS()\nstruct alignas(16) S {\n");
            for (int index = 0; index < 257; ++index)
            {
                many.Append(System.Globalization.CultureInfo.InvariantCulture, $"ShaderFloat4 m{index};\n");
            }

            many.Append("};\n");
            await probe.RejectAsync(many.ToString(), "RMC6006", "1 to 256 members");
        });
    }
}
