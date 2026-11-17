using System.Text.RegularExpressions;

namespace Rocks.ToolTests;

internal static partial class MetaTests
{
    private const string Preamble = "#include <Meta/Annotations.h>\n#include <Object/ObjectRefs.h>\n#include <Object/Object.h>\n#include <Core/Text/String.h>\nnamespace probe {\n";

    // MetaCompiler·Clang probe 프로세스 하나의 제한 시간이다. meta 시험은 CPU 수만큼 병렬로 돌고, 코어가 적은
    // CI 러너(macOS)에서는 Binds 모듈처럼 무거운 probe가 60초를 넘을 수 있어 넉넉히 둔다. 멈춘 프로세스는 단계
    // 제한(meta 30분)보다 훨씬 먼저 이 제한에 걸린다.
    internal static readonly TimeSpan ProbeTimeout = TimeSpan.FromSeconds(300);

    private static string Declaration(string? field = null,
        string bases = "public rocks::Object", string extra = "") =>
        Preamble + $"ROCKS_CLASS()\nstruct Value : {bases} {{\n" +
        (field ?? "ROCKS_PROPERTY() rocks::int32 count = 0;") + "\n" + extra + "\n};\n}\n";

    // 공백을 모두 지운 텍스트다(clang-format 줄바꿈과 무관하게 생성 코드 조각을 찾는다).
    private static string Compact(string text) => Regex.Replace(text, @"\s+", "", RegexOptions.CultureInvariant, TimeSpan.FromSeconds(10));

    // meta 시험 등록이 함께 쓰는 실행 문맥이다: 시험 묶음, 옵션, MetaCompiler·Clang 실행 환경(formatter 반영),
    // MetaCompiler 실행 파일, Clang 경로.
    private sealed record MetaContext(TestSuite Suite, Options Options, IReadOnlyDictionary<string, string?> Environment,
        string Generator, string Compiler);

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
        NativeClassCompilerTests.Register(suite, options, actualEnvironment);
        IdentityCompilerTests.Register(suite, options, actualEnvironment);
        IdentityCliReviewTests.Register(suite, options, actualEnvironment);
        ModuleMetaTests.Register(suite, options, actualEnvironment);
        IdentityModuleCliTests.Register(suite, options, actualEnvironment);
        ValueKindTableTests.Register(suite, options, actualEnvironment);
        MetaContext context = new(suite, options, actualEnvironment, generator, compiler);
        RegisterCli(context);
        RegisterValueKinds(context);
        RegisterClassRefs(context);
        RegisterOptionals(context);
        RegisterMaps(context);
        RegisterObjectRefMaps(context);
        RegisterStaticArrays(context);
        RegisterInstancedStructs(context);
        RegisterStructInheritance(context);
        RegisterTextFields(context);
        RegisterBitfields(context);
        RegisterFieldFlags(context);
        RegisterClassFlags(context);
        RegisterStructs(context);
        RegisterBinds(context);
        RegisterArrays(context);
        RegisterPathThunks(context);
        RegisterArrayRestrictions(context);
        RegisterNative(context);
        RegisterSettings(context);
    }

    // 거부 시험 하나(name)를 등록한다. sources의 원본마다 MetaCompiler가 code 진단(reason을 담은 문장)으로 거부해야 한다.
    // module은 생성할 모듈 이름이다(Binds 시험은 CoreTypeBindings).
    private static void AddRejectGroup(MetaContext context, string name, IEnumerable<string> sources, string code, string reason,
        string module = MetaCompilerArguments.Module)
    {
        string[] fixedSources = sources.ToArray();
        context.Suite.Add(name, async () =>
        {
            using Probe probe = new(context.Options, context.Environment, context.Generator, context.Compiler, module);
            foreach (string source in fixedSources)
            {
                await probe.RejectAsync(source, code, reason);
            }
        });
    }
}
