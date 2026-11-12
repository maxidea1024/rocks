using System.Text.Json;
using Rocks.MetaCompiler;
using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

// MetaCompiler 모듈 모드 시험이다(Docs/Design/ReflectionFoundations.ko.md 1.3–1.5절): 여러 헤더를 한 번에 생성하고,
// 의존 모듈 descriptor를 import해 base·참조를 풀고, private 헤더를 Private 생성 루트와 private 집계로 나눈다.
internal static class ModuleMetaTests
{
    private const string Parent = "#pragma once\n#include <Meta/Annotations.h>\n#include <Object/Object.h>\nnamespace probe {\n" +
        "ROCKS_CLASS()\nstruct Parent : public rocks::Object {\nROCKS_PROPERTY() rocks::int32 value = 0;\n};\n}\n";

    private const string Value = "#pragma once\n#include <Lower/Parent.h>\n#include <Object/ObjectRefs.h>\nnamespace probe {\n" +
        "ROCKS_CLASS()\nstruct Value : public Parent {\nROCKS_PROPERTY() rocks::ObjectRef<Parent> next;\n};\n}\n";

    private const string Leaf = "#pragma once\n#include <Upper/Value.h>\nnamespace probe {\n" +
        "ROCKS_CLASS()\nstruct Leaf final : public Value {};\n}\n";

    private const string Detail = "#pragma once\n#include <Upper/Value.h>\nnamespace probe {\n" +
        "ROCKS_CLASS()\nstruct Detail final : public Value {};\n}\n";

    // private 헤더만 있는 모듈의 타입이 import한 타입을 상속한다.
    private const string Hidden = "#pragma once\n#include <Lower/Parent.h>\nnamespace probe {\n" +
        "ROCKS_CLASS()\nstruct Hidden final : public Parent {};\n}\n";

    // import한 Parent와 이름이 같지만 다른 파일의 선언(include하지 않은 재정의)이다.
    private const string FakeParent = "#pragma once\n#include <Meta/Annotations.h>\n#include <Object/Object.h>\nnamespace probe {\n" +
        "ROCKS_CLASS()\nstruct Parent : public rocks::Object {\nROCKS_PROPERTY() rocks::int32 value = 0;\n};\n}\n";

    private const string Impostor = "#pragma once\n#include <Upper/FakeParent.h>\nnamespace probe {\n" +
        "ROCKS_CLASS()\nstruct Impostor final : public Parent {};\n}\n";

    public static void Register(TestSuite suite, Options options, IReadOnlyDictionary<string, string?> environment)
    {
        suite.Add("Meta.ModuleGeneratesHeadersAggregatesAndDescriptor", async () =>
        {
            using Layout layout = new(options, environment);
            ProcessResult lower = await layout.RunLowerAsync();
            Verify.Equal(0, lower.ExitCode, "lower module: " + lower.StdErr);
            Verify.That(File.Exists(layout.Generated("Lower", "Public/Lower/Parent.generated.h")), "lower generated header");
            string lowerAggregate = File.ReadAllText(layout.Generated("Lower", "Public/Lower/Lower.module.generated.h"));
            Verify.Contains("namespace rocks::generated::Lower", lowerAggregate, "lower aggregate namespace");
            Verify.Contains("GeneratedType<::probe::Parent>", lowerAggregate, "lower aggregate registers its type");
            Verify.That(!Directory.Exists(layout.Generated("Lower", "Private/Lower")), "public-only module emitted a private aggregate");
            Verify.That(!File.ReadAllBytes(layout.Descriptor("Lower")).Contains((byte)'\r'), "descriptor must use LF on every platform");

            ProcessResult upper = await layout.RunUpperAsync();
            Verify.Equal(0, upper.ExitCode, "upper module: " + upper.StdErr);
            foreach (string relative in new[] { "Public/Upper/Value.generated.h", "Public/Upper/Leaf.generated.h", "Private/Upper/Detail.generated.h" })
            {
                Verify.That(File.Exists(layout.Generated("Upper", relative)), "missing generated header " + relative);
            }

            // public 집계는 import한 모듈을 먼저 등록하고, 이 모듈 타입을 base 먼저(헤더를 건너) 등록한다.
            string aggregate = File.ReadAllText(layout.Generated("Upper", "Public/Upper/Upper.module.generated.h"));
            Verify.Contains("#include <Lower/Lower.module.generated.h>", aggregate, "dependency aggregate include");
            Verify.Contains("registration.EnterModule(&moduleKey)", aggregate, "aggregate registers each module once per registration");
            int dependency = aggregate.IndexOf("::rocks::generated::Lower::RegisterModuleTypes", StringComparison.Ordinal);
            int value = aggregate.IndexOf("GeneratedType<::probe::Value>::Schema()", StringComparison.Ordinal);
            int leaf = aggregate.IndexOf("GeneratedType<::probe::Leaf>::Schema()", StringComparison.Ordinal);
            Verify.That(dependency >= 0 && value > dependency && leaf > value, "aggregate registration order:\n" + aggregate);
            Verify.That(!aggregate.Contains("probe::Detail", StringComparison.Ordinal), "public aggregate registers a private type");
            string privateAggregate = File.ReadAllText(layout.Generated("Upper", "Private/Upper/Upper.private.module.generated.h"));
            Verify.Contains("#include <Upper/Upper.module.generated.h>", privateAggregate, "private aggregate includes public aggregate");
            Verify.Contains("RegisterPrivateModuleTypes", privateAggregate, "private aggregate function");
            Verify.Contains("GeneratedType<::probe::Detail>", privateAggregate, "private aggregate registers private type");

            // descriptor는 상속 필드를 펼치고, import한 타입을 가리키는 참조의 대상 ID를 담는다.
            using JsonDocument lowerDescriptor = JsonDocument.Parse(File.ReadAllBytes(layout.Descriptor("Lower")));
            using JsonDocument upperDescriptor = JsonDocument.Parse(File.ReadAllBytes(layout.Descriptor("Upper")));
            JsonElement parent = Type(lowerDescriptor, "probe::Parent");
            Verify.Equal("Lower/Parent.h", parent.GetProperty("header").GetString(), "descriptor header spelling");
            JsonElement valueType = Type(upperDescriptor, "probe::Value");
            Verify.Equal("probe::Parent", valueType.GetProperty("base").GetString(), "imported base");
            // 필드는 상속 포함 안정 ID 순이다(위치가 아니라 이름으로 찾는다).
            JsonElement[] fields = valueType.GetProperty("fields").EnumerateArray().ToArray();
            Verify.Equal(2, fields.Length, "inherited and own fields");
            JsonElement inherited = fields.Single(field => field.GetProperty("name").GetString() == "value");
            JsonElement next = fields.Single(field => field.GetProperty("name").GetString() == "next");
            Verify.Equal("probe::Parent", inherited.GetProperty("owner").GetString(), "inherited field owner");
            Verify.Equal(parent.GetProperty("fields")[0].GetProperty("id").GetString(), inherited.GetProperty("id").GetString(), "inherited field ID");
            Verify.Equal(parent.GetProperty("id").GetString(), next.GetProperty("value").GetProperty("type").GetString(), "imported reference target ID");
            Verify.Equal("private", Type(upperDescriptor, "probe::Detail").GetProperty("visibility").GetString(), "private visibility");

            // --check는 모든 생성물과 descriptor를 비교하고, Metadata에서 빠진 헤더의 낡은 생성물도 찾는다. 단독 실행은
            // 남은 생성물을 지우지 않는다(지우기는 빌드의 게시 트랜잭션이 맡는다).
            ProcessResult check = await layout.RunUpperAsync(extra: ["--check"]);
            Verify.Equal(0, check.ExitCode, "up-to-date check: " + check.StdErr);
            ProcessResult withoutLeaf = await layout.RunUpperAsync(withLeaf: false);
            Verify.Equal(0, withoutLeaf.ExitCode, "upper module without Leaf: " + withoutLeaf.StdErr);
            ProcessResult stale = await layout.RunUpperAsync(withLeaf: false, extra: ["--check"]);
            Verify.That(stale.ExitCode != 0, "check accepted the generated header of a removed input");
            Verify.Contains("RMC4000", stale.StdErr, "stale extra code");
            Verify.Contains("Leaf.generated.h", stale.StdErr, "stale extra path");
        });

        suite.Add("Meta.ModuleImportAndVisibilityErrors", async () =>
        {
            using Layout layout = new(options, environment);
            Verify.Equal(0, (await layout.RunLowerAsync()).ExitCode, "lower module");

            // 의존 모듈의 리플렉션 선언은 그 모듈 descriptor를 import해야 쓸 수 있다.
            await Reject(layout.RunUpperAsync(imports: []), "RMC2003", "imported descriptor");

            // 같은 모듈을 두 번, 또는 같은 타입을 선언한 두 모듈을 import하면 거부한다.
            await Reject(layout.RunUpperAsync(imports: [layout.Descriptor("Lower"), layout.Descriptor("Lower")]), "RMC2004", "imported twice");
            string other = layout.Descriptor("Other");
            File.WriteAllText(other, File.ReadAllText(layout.Descriptor("Lower")).Replace("\"module\": \"Lower\"", "\"module\": \"Other\"", StringComparison.Ordinal));
            await Reject(layout.RunUpperAsync(imports: [layout.Descriptor("Lower"), other]), "RMC2004", "two imported modules");

            // public 타입은 private 타입을 base로 둘 수 없고, 다른 모듈의 private 타입은 쓸 수 없다.
            await Reject(layout.RunUpperAsync(privateValue: true), "RMC2006", "derives from private type");
            Verify.Equal(0, (await layout.RunLowerAsync(privateParent: true)).ExitCode, "private lower module");
            await Reject(layout.RunUpperAsync(), "RMC2006", "private reflected type");
        });

        suite.Add("Meta.ModuleSpellingCollisionsAreRejected", async () =>
        {
            using Layout layout = new(options, environment);
            await Reject(layout.RunLowerAsync(extraInputs: [("Lower/Parent.h", "lower/parent.h")]), "RMC2005", "case-only");
            layout.Write("Lower/Public/Lower/Types.module.h", Parent);
            await Reject(layout.RunLowerAsync(extraInputs: [("Lower/Types.module.h", "Lower/Types.module.h")]), "RMC2005", "reserved");
            layout.Write("Lower/Public/Lower/Types.generated.h", Parent);
            await Reject(layout.RunLowerAsync(extraInputs: [("Lower/Types.generated.h", "Lower/Types.generated.h")]), "RMC2005", "reserved");

            // 출력 루트의 파일은 모두 생성 헤더다. descriptor·inventory를 그 안에 두면 --check가 잉여 파일로 보므로 거부한다.
            await Reject(layout.RunLowerAsync(descriptor: layout.Generated("Lower", "Lower.rocksmeta.json")), "RMC1000", "outside the output root");
            await Reject(layout.RunLowerAsync(extra: ["--identity-inventory", layout.Generated("Lower", "inventory.json")]), "RMC1000", "outside the output root");
        });

        // 집계 헤더는 이름을 ::rocks::로 한정하므로 rocks 타입과 같은 이름(Result)의 모듈도 그 모듈과 의존 모듈이 컴파일된다.
        suite.Add("Meta.ModuleNamedLikeRocksTypesCompiles", async () =>
        {
            using Layout layout = new(options, environment);
            Directory.CreateDirectory(layout.Generated("Result", "Public"));
            Directory.CreateDirectory(layout.Generated("Result", "Private"));
            ProcessResult lower = await layout.RunLowerAsync(module: "Result");
            Verify.Equal(0, lower.ExitCode, "module named Result: " + lower.StdErr);
            ProcessResult upper = await layout.RunUpperInputsAsync([(layout.Source("Upper/Public/Upper/Value.h"), "Upper/Value.h", false)],
                imports: [layout.Descriptor("Result")], dependencyModule: "Result");
            Verify.Equal(0, upper.ExitCode, "module importing Result: " + upper.StdErr);
            Verify.Contains("::rocks::generated::Result::RegisterModuleTypes", File.ReadAllText(layout.Generated("Upper", "Public/Upper/Upper.module.generated.h")),
                "dependency call");
        });

        // 생성물 검증 컴파일은 소비자와 같은 include 순서를 쓴다. 작성자 디렉터리의 낡은 .generated.h가 생성 헤더보다 먼저
        // 풀리면 소비자도 그 파일을 보므로 검증이 실패해야 한다(검증 루트를 맨 앞에 두면 통과해 버린다).
        suite.Add("Meta.ValidationUsesTheConsumerIncludeOrder", async () =>
        {
            using Layout layout = new(options, environment);
            Verify.Equal(0, (await layout.RunLowerAsync()).ExitCode, "lower module");
            layout.Write("Upper/Public/Upper/Value.generated.h", "#error stale generated header in the author directory\n");
            await Reject(layout.RunUpperAsync(), "RMC3000", "stale generated header");
        });

        // import한 base의 WorldOnly와 도구 이름은 descriptor로 전해져 파생 모듈에 상속된다(B2).
        suite.Add("Meta.ImportedWorldOnlyAndToolNamesAreInherited", async () =>
        {
            using Layout layout = new(options, environment);
            layout.Write("Lower/Public/Lower/Parent.h", Parent.Replace("ROCKS_CLASS()", "ROCKS_CLASS(WorldOnly)", StringComparison.Ordinal)
                .Replace("ROCKS_PROPERTY()", "ROCKS_PROPERTY(Name=\"Amount\", EditorOnly)", StringComparison.Ordinal));
            Verify.Equal(0, (await layout.RunLowerAsync()).ExitCode, "lower module");
            ProcessResult upper = await layout.RunUpperAsync();
            Verify.Equal(0, upper.ExitCode, "upper module: " + upper.StdErr);
            using JsonDocument descriptor = JsonDocument.Parse(File.ReadAllBytes(layout.Descriptor("Upper")));
            JsonElement value = Type(descriptor, "probe::Value");
            Verify.Equal("WorldOnly", value.GetProperty("flags")[0].GetString(), "WorldOnly inherited from an imported base");
            JsonElement inherited = value.GetProperty("fields").EnumerateArray().Single(field => field.GetProperty("name").GetString() == "value");
            Verify.Equal("Amount", inherited.GetProperty("toolName").GetString(), "inherited tool name");
            Verify.Equal("EditorOnly", inherited.GetProperty("flags")[0].GetString(), "inherited field flags round-trip");

            // 상속한 도구 이름과 대소문자만 다른 이름은 RMC2203이다.
            layout.Write("Upper/Public/Upper/Value.h", Value.Replace("ROCKS_PROPERTY()", "ROCKS_PROPERTY(Name=\"amount\")", StringComparison.Ordinal));
            await Reject(layout.RunUpperAsync(), "RMC2203", "tool name");
        });

        // private 헤더만 있는 모듈도 private 집계가 import한 모듈을 먼저 등록한다(base가 등록되어 있어야 한다).
        suite.Add("Meta.PrivateOnlyModuleRegistersImportedModules", async () =>
        {
            using Layout layout = new(options, environment);
            Verify.Equal(0, (await layout.RunLowerAsync()).ExitCode, "lower module");
            ProcessResult hidden = await layout.RunUpperInputsAsync([(layout.Source("Upper/Private/Upper/Hidden.h"), "Upper/Hidden.h", true)]);
            Verify.Equal(0, hidden.ExitCode, "private-only module: " + hidden.StdErr);
            Verify.That(!File.Exists(layout.Generated("Upper", "Public/Upper/Upper.module.generated.h")), "private-only module emitted a public aggregate");
            string aggregate = File.ReadAllText(layout.Generated("Upper", "Private/Upper/Upper.private.module.generated.h"));
            Verify.Contains("#include <Lower/Lower.module.generated.h>", aggregate, "private-only aggregate includes the dependency aggregate");
            int dependency = aggregate.IndexOf("::rocks::generated::Lower::RegisterModuleTypes", StringComparison.Ordinal);
            int own = aggregate.IndexOf("GeneratedType<::probe::Hidden>::Schema()", StringComparison.Ordinal);
            Verify.That(dependency >= 0 && own > dependency, "private-only aggregate must register the imported base first:\n" + aggregate);
        });

        // 모듈은 자기 descriptor를 import할 수 없고, import한 타입을 다시 정의하거나 같은 이름을 다른 파일에서 선언할 수 없다.
        suite.Add("Meta.ModuleRedeclarationAndSelfImportAreRejected", async () =>
        {
            using Layout layout = new(options, environment);
            Verify.Equal(0, (await layout.RunLowerAsync()).ExitCode, "lower module");
            Verify.Equal(0, (await layout.RunUpperAsync()).ExitCode, "upper module");
            await Reject(layout.RunUpperAsync(imports: [layout.Descriptor("Lower"), layout.Descriptor("Upper")]), "RMC2004", "imports its own descriptor");

            // 입력 밖 파일의 같은 이름 선언(같은 이름·다른 물리 파일)은 RMC2003이다.
            await Reject(layout.RunUpperInputsAsync([(layout.Source("Upper/Public/Upper/Impostor.h"), "Upper/Impostor.h", false)]), "RMC2003", "same name and header");

            // 이 모듈 입력 헤더가 import한 이름을 다시 정의하면 RMC2004다.
            await Reject(layout.RunUpperInputsAsync([(layout.Source("Upper/Public/Upper/FakeParent.h"), "Upper/FakeParent.h", false)]), "RMC2004", "declared by this module and by imported module");
        });

        // descriptor는 빌드 산출물이다. 모르는 속성, 잘못된 모듈 이름·헤더 철자·이름·버전은 생성 C++에 들어가기 전에 거부한다.
        suite.Add("Meta.ModuleDescriptorIsReadStrictly", async () =>
        {
            using Layout layout = new(options, environment);
            Verify.Equal(0, (await layout.RunLowerAsync()).ExitCode, "lower module");
            string original = File.ReadAllText(layout.Descriptor("Lower"));
            foreach ((string name, string from, string to) in new[]
            {
                ("value", "\"kind\": \"Int32\"", "\"kind\": \"Int32\",\n            \"extra\": 1"),
                ("module", "\"module\": \"Lower\"", "\"module\": \"Bad-Name\""),
                ("header", "\"header\": \"Lower/Parent.h\"", "\"header\": \"../Parent.h\""),
                ("version", "\"version\": 1", "\"version\": 4294967296"),
                ("name", "\"name\": \"probe::Parent\"", "\"name\": \"probe::Parent; int x\""),
                ("newline", "\"name\": \"probe::Parent\"", "\"name\": \"probe::Parent\\n\""),
                ("element", "\"element\": null", "\"element\": 1"),
                ("maxElements", "\"maxElements\": 0", "\"maxElements\": 3"),
                ("toolName", "\"toolName\": \"value\"", "\"toolName\": \"1bad\""),
                ("duplicate", "\"module\": \"Lower\"", "\"module\": \"Lower\",\n  \"module\": \"Lower\"")
            })
            {
                Verify.That(original.Contains(from, StringComparison.Ordinal), "descriptor fixture changed: " + name);
                string mutated = layout.Generated("Mutated", name + ".rocksmeta.json");
                Directory.CreateDirectory(Path.GetDirectoryName(mutated)!);
                File.WriteAllText(mutated, original.Replace(from, to, StringComparison.Ordinal));
                await Reject(layout.RunUpperAsync(imports: [mutated]), "RMC1000", "invalid descriptor");
            }
        });
    }

    private static JsonElement Type(JsonDocument descriptor, string name) =>
        descriptor.RootElement.GetProperty("types").EnumerateArray().Single(type => type.GetProperty("name").GetString() == name);

    private static async Task Reject(Task<ProcessResult> run, string code, string reason)
    {
        ProcessResult result = await run;
        Verify.That(result.ExitCode != 0, $"accepted input that must fail with {code}");
        Verify.Contains(code, result.StdErr, "stable diagnostic code");
        Verify.That(result.StdErr.Contains(reason, StringComparison.OrdinalIgnoreCase), "diagnostic reason missing: " + reason + "\n" + result.StdErr);
    }

    // Lower(Parent)와 Upper(Value·Leaf public, Detail private) 두 모듈의 원본·명세·생성 루트다.
    private sealed class Layout : IDisposable
    {
        private readonly TemporaryDirectory temporary = new();
        private readonly Options options;
        private readonly IReadOnlyDictionary<string, string?> environment;

        public Layout(Options options, IReadOnlyDictionary<string, string?> environment)
        {
            this.options = options;
            this.environment = environment;
            Write("Lower/Public/Lower/Parent.h", Parent);
            Write("Upper/Public/Upper/Value.h", Value);
            Write("Upper/Public/Upper/Leaf.h", Leaf);
            Write("Upper/Private/Upper/Detail.h", Detail);
            Write("Upper/Private/Upper/Hidden.h", Hidden);
            Write("Upper/Public/Upper/FakeParent.h", FakeParent);
            Write("Upper/Public/Upper/Impostor.h", Impostor);
            Manifest("Lower", ["probe::Parent"], ["probe::Parent::value"]);
            // Upper 명세는 재정의 시험이 식별자 단계를 지나 RMC2004에 닿도록 probe::Parent도 담는다.
            Manifest("Upper", ["probe::Value", "probe::Leaf", "probe::Detail", "probe::Hidden", "probe::Impostor", "probe::Parent"],
                ["probe::Value::next", "probe::Parent::value"]);
            foreach (string module in new[] { "Lower", "Upper" })
            {
                Directory.CreateDirectory(Generated(module, "Public"));
                Directory.CreateDirectory(Generated(module, "Private"));
            }
        }

        public string Generated(string module, string relative) => Path.Combine(temporary.Root, "out", module, relative);

        public string Descriptor(string module) => Path.Combine(temporary.Root, "out", module + ".rocksmeta.json");

        public void Write(string relative, string text) => temporary.Write(relative, text);

        // module은 생성할 모듈 이름이다(원본·명세는 Lower 것을 쓴다). extra는 그대로 덧붙인다.
        public Task<ProcessResult> RunLowerAsync(bool privateParent = false, IReadOnlyList<(string Header, string Include)>? extraInputs = null,
            string? descriptor = null, string module = "Lower", IReadOnlyList<string>? extra = null)
        {
            List<string> arguments = ["--module", module, "--input", Source("Lower/Public/Lower/Parent.h"), "--include", "Lower/Parent.h"];
            foreach ((string header, string include) in extraInputs ?? [])
            {
                arguments.AddRange(["--input", Source("Lower/Public/" + header), "--include", include]);
            }

            if (privateParent)
            {
                arguments.AddRange(["--private", "Lower/Parent.h"]);
            }

            arguments.AddRange(extra ?? []);
            return RunAsync(module, arguments, [Source("Lower/Public")], [], [], descriptor, manifestModule: "Lower");
        }

        public Task<ProcessResult> RunUpperAsync(bool withLeaf = true, bool privateValue = false, IReadOnlyList<string>? imports = null,
            IReadOnlyList<string>? extra = null)
        {
            List<string> arguments = ["--module", "Upper", "--input", Source("Upper/Public/Upper/Value.h"), "--include", "Upper/Value.h"];
            if (withLeaf)
            {
                arguments.AddRange(["--input", Source("Upper/Public/Upper/Leaf.h"), "--include", "Upper/Leaf.h"]);
            }

            arguments.AddRange(["--input", Source("Upper/Private/Upper/Detail.h"), "--include", "Upper/Detail.h", "--private", "Upper/Detail.h"]);
            if (privateValue)
            {
                arguments.AddRange(["--private", "Upper/Value.h"]);
            }

            foreach (string import in imports ?? [Descriptor("Lower")])
            {
                arguments.AddRange(["--import", import]);
            }

            arguments.AddRange(extra ?? []);
            return RunAsync("Upper", arguments, [Source("Upper/Public")], [Source("Upper/Private")], [Source("Lower/Public"), Generated("Lower", "Public")]);
        }

        // 임의 입력 집합으로 Upper를 생성한다(Lower descriptor import).
        public Task<ProcessResult> RunUpperInputsAsync(IReadOnlyList<(string Path, string Include, bool Private)> inputs, IReadOnlyList<string>? imports = null,
            string dependencyModule = "Lower")
        {
            List<string> arguments = ["--module", "Upper"];
            foreach ((string path, string include, bool isPrivate) in inputs)
            {
                arguments.AddRange(["--input", path, "--include", include]);
                if (isPrivate)
                {
                    arguments.AddRange(["--private", include]);
                }
            }

            foreach (string import in imports ?? [Descriptor("Lower")])
            {
                arguments.AddRange(["--import", import]);
            }

            return RunAsync("Upper", arguments, [Source("Upper/Public")], [Source("Upper/Private")], [Source("Lower/Public"), Generated(dependencyModule, "Public")]);
        }

        public string Source(string relative) => temporary.FilePath(relative);

        private void Manifest(string module, string[] types, string[] fields)
        {
            StableIdManifest.Update(Source(module + "/" + module + ".rocksids.json"), true, manifest =>
            {
                foreach (string type in types)
                {
                    manifest.Register("type", type);
                }

                foreach (string field in fields)
                {
                    manifest.Register("field", field);
                }

                return 0;
            });
        }

        // include 검색 순서는 빌드(MetadataLayout.IncludeOrder)와 같다: 자기 public 원본·생성 Public, 자기 private 원본·생성
        // Private, 의존 모듈의 public 원본·생성 루트, Core.
        private async Task<ProcessResult> RunAsync(string module, List<string> moduleArguments, IReadOnlyList<string> ownPublic,
            IReadOnlyList<string> ownPrivate, IReadOnlyList<string> dependencies, string? descriptor = null, string? manifestModule = null)
        {
            string generator = Path.GetFullPath(options.Required("metacompiler"));
            string executable = generator;
            List<string> arguments = [];
            if (generator.EndsWith(".dll", StringComparison.OrdinalIgnoreCase))
            {
                executable = options.Optional("dotnet") ?? "dotnet";
                arguments.Add(generator);
            }

            arguments.AddRange(moduleArguments);
            arguments.AddRange(["--output-root", Path.Combine(temporary.Root, "out", module), "--descriptor", descriptor ?? Descriptor(module),
                "--compiler", options.Required("compiler"), "--identity-manifest", Source((manifestModule ?? module) + "/" + (manifestModule ?? module) + ".rocksids.json")]);
            IEnumerable<string> includes = ownPublic.Append(Generated(module, "Public"))
                .Concat(ownPrivate).Append(Generated(module, "Private"))
                .Concat(dependencies)
                .Concat(Directory.GetDirectories(Path.Combine(options.Root, "Core", "Source", "Runtime"))
                    .Select(directory => Path.Combine(directory, "Public")).Where(Directory.Exists).Order(StringComparer.Ordinal));
            foreach (string include in includes)
            {
                arguments.AddRange(["--include-dir", include]);
            }

            foreach (string argument in options.ClangArguments)
            {
                arguments.Add("--clang-arg=" + argument);
            }

            return await ProcessRunner.RunAsync(new(executable, arguments, options.Root, environment, Timeout: TimeSpan.FromSeconds(120)));
        }

        public void Dispose() => temporary.Dispose();
    }
}
