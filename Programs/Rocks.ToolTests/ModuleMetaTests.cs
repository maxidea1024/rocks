using System.Text.Json;
using Rocks.MetaCompiler;
using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

// MetaCompiler 모듈 모드 시험이다(Docs/Design/ReflectionFoundations.ko.md 1.3–1.5절): 여러 헤더를 한 번에 생성하고,
// 의존 모듈 descriptor를 import해 base·참조를 풀고, private 헤더를 Private 생성 루트와 private 집계로 나눈다.
internal static partial class ModuleMetaTests
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

    // B4b: struct를 가진 Lower다(Point, 참조 struct Link, struct 필드를 가진 Parent, enum Mode, NATIVE WorldOnly 소유자
    // 형질의 WorldBits와 그것을 담아 형질이 전파되는 Outer).
    private const string StructParent = "#pragma once\n#include <Meta/Annotations.h>\n#include <Object/Object.h>\n#include <Object/ObjectRefs.h>\nnamespace probe {\n" +
        "struct Parent;\nROCKS_ENUM()\nenum class Mode : rocks::uint8 { Off = 0 };\n" +
        "struct Lease { static constexpr bool RocksRequiresWorldOnlyOwner = true; };\n" +
        "ROCKS_STRUCT()\nstruct Point {\nROCKS_PROPERTY() rocks::uint16 x = 0;\n};\n" +
        "ROCKS_STRUCT()\nstruct Link {\nROCKS_PROPERTY() rocks::ObjectRef<Parent> target;\nROCKS_PROPERTY() Point at;\n};\n" +
        "ROCKS_STRUCT()\nstruct WorldBits {\nROCKS_NATIVE() Lease lease;\nROCKS_PROPERTY() rocks::int32 value = 0;\n};\n" +
        "ROCKS_STRUCT()\nstruct Outer {\nROCKS_PROPERTY() WorldBits inner;\n};\n" +
        "ROCKS_CLASS()\nstruct Parent : public rocks::Object {\nROCKS_PROPERTY() rocks::int32 value = 0;\nROCKS_PROPERTY() Point origin;\n};\n}\n";

    // B4c: CoreTypeBindings로 생성하는 Lower다(Vec3·Vec3d Binds 미러와 Parent).
    private const string BindsParent = "#pragma once\n#include <Meta/Annotations.h>\n#include <Object/Object.h>\n#include <Core/Math/Vector3.h>\nnamespace probe {\n" +
        "ROCKS_STRUCT(Binds = \"rocks::Vec3\")\nstruct Vec3Binding {\nROCKS_PROPERTY() float x = 0;\nROCKS_PROPERTY() float y = 0;\nROCKS_PROPERTY() float z = 0;\n};\n" +
        "ROCKS_STRUCT(Binds = \"rocks::Vec3d\")\nstruct Vec3dBinding {\nROCKS_PROPERTY() double x = 0;\nROCKS_PROPERTY() double y = 0;\nROCKS_PROPERTY() double z = 0;\n};\n" +
        "ROCKS_CLASS()\nstruct Parent : public rocks::Object {\nROCKS_PROPERTY() rocks::int32 value = 0;\n};\n}\n";

    // Binds를 받는 유일한 모듈 이름이다(StructRules.BindingModule).
    private const string BindingsModule = "CoreTypeBindings";

    // 깊이 16의 struct 사슬(Chain1 <- … <- Chain16), 위조용 Top(UInt16 필드 하나), Parent를 가진 Lower다.
    private static readonly string ChainParent = "#pragma once\n#include <Meta/Annotations.h>\n#include <Object/Object.h>\nnamespace probe {\n" +
        "ROCKS_STRUCT()\nstruct Chain1 {\nROCKS_PROPERTY() rocks::int32 value = 0;\n};\n" +
        string.Concat(Enumerable.Range(2, 15).Select(level => $"ROCKS_STRUCT()\nstruct Chain{level} {{\nROCKS_PROPERTY() Chain{level - 1} inner;\n}};\n")) +
        "ROCKS_STRUCT()\nstruct Top {\nROCKS_PROPERTY() rocks::uint16 x = 0;\n};\n" +
        "ROCKS_CLASS()\nstruct Parent : public rocks::Object {\nROCKS_PROPERTY() rocks::int32 value = 0;\n};\n}\n";

    // Upper의 다른 헤더 struct다(import struct를 멤버로 담는다).
    private const string Shape = "#pragma once\n#include <Lower/Parent.h>\nnamespace probe {\n" +
        "ROCKS_STRUCT()\nstruct Shape {\nROCKS_PROPERTY() Point corner;\nROCKS_PROPERTY() Link link;\n};\n}\n";

    // B5c 세 모듈 구성(Docs/Design/ReflectionFoundations.ko.md 5.6절 시험)의 원소 모듈 Elements다: 원소 enum EA, 참조 struct SA,
    // 참조 없는 struct Plain.
    private const string ArrayElements = "#pragma once\n#include <Meta/Annotations.h>\n#include <Object/Object.h>\n#include <Object/ObjectRefs.h>\nnamespace probe {\n" +
        "ROCKS_ENUM()\nenum class EA : rocks::uint8 { Off = 0, On = 1 };\n" +
        "ROCKS_STRUCT()\nstruct SA {\nROCKS_PROPERTY() rocks::ObjectRef<rocks::Object> target;\nROCKS_PROPERTY() rocks::int32 count = 0;\n};\n" +
        "ROCKS_STRUCT()\nstruct Plain {\nROCKS_PROPERTY() rocks::uint16 x = 0;\n};\n}\n";

    // 세 모듈 구성의 base 모듈 Lower다: 다른 모듈 struct·enum의 배열 필드를 가진 Parent와, 다른 모듈 struct의 배열 멤버를 가진
    // struct Group(ContainsReferenceArray).
    private const string ArrayParent = "#pragma once\n#include <Elements/Items.h>\n#include <Core/Containers/Array.h>\n#include <Object/ObjectRefArray.h>\n" +
        "#include <Object/StructRefArray.h>\nnamespace probe {\n" +
        "ROCKS_STRUCT()\nstruct Group {\nexplicit Group(rocks::Allocator& allocator) noexcept : slots(allocator), plains(allocator) {}\n" +
        "ROCKS_PROPERTY() rocks::StructRefArray<SA> slots;\nROCKS_PROPERTY() rocks::Array<Plain> plains;\n};\n" +
        "ROCKS_CLASS()\nstruct Parent : public rocks::Object {\n" +
        "explicit Parent(rocks::Allocator& allocator) noexcept : slots(allocator), modes(allocator), plains(allocator), links(allocator) {}\n" +
        "ROCKS_PROPERTY() rocks::StructRefArray<SA> slots;\nROCKS_PROPERTY() rocks::Array<EA> modes;\nROCKS_PROPERTY(MaxElements = 7) rocks::Array<Plain> plains;\n" +
        "ROCKS_PROPERTY() rocks::ObjectRefArray<Parent> links;\n};\n}\n";

    // 세 모듈 구성의 파생 모듈 Upper다: Parent의 배열 필드를 상속하고 import struct Group을 필드로 둔다.
    private const string ArrayValue = "#pragma once\n#include <Lower/Parent.h>\nnamespace probe {\n" +
        "ROCKS_CLASS()\nstruct Value : public Parent {\nexplicit Value(rocks::Allocator& allocator) noexcept : Parent(allocator), group(allocator) {}\n" +
        "ROCKS_PROPERTY() rocks::ObjectRef<Parent> next;\nROCKS_PROPERTY() Group group;\n};\n}\n";

    // B8b: 모듈 이름과 같은 이름의 rocks 클래스(World 모듈의 rocks::World처럼)를 담는 Lower다. probe::Parent는 Lower 명세를 그대로 쓴다.
    private const string SameNameParent = Parent + "namespace rocks {\nROCKS_CLASS()\nstruct Lower : public Object {\n" +
        "ROCKS_PROPERTY() int32 value = 0;\n};\ninline int32 ReadLower(const Lower& lower) noexcept { return lower.value; }\n}\n";

    public static void Register(TestSuite suite, Options options, IReadOnlyDictionary<string, string?> environment)
    {
        RegisterModules(suite, options, environment);
        RegisterEnums(suite, options, environment);
        RegisterModuleDescriptors(suite, options, environment);
        RegisterStructs(suite, options, environment);
        RegisterArrays(suite, options, environment);
        RegisterBitfields(suite, options, environment);
    }

    // 공백을 모두 지운 텍스트다(clang-format 줄바꿈과 무관하게 생성 코드 조각을 찾는다).
    private static string Compact(string text) => System.Text.RegularExpressions.Regex.Replace(text, @"\s+", "",
        System.Text.RegularExpressions.RegexOptions.CultureInvariant, TimeSpan.FromSeconds(10));

    private static JsonElement Type(JsonDocument descriptor, string name) =>
        descriptor.RootElement.GetProperty("types").EnumerateArray().Single(type => type.GetProperty("name").GetString() == name);

    // descriptor의 name 타입 항목을 change로 바꾼 JSON이다(서식과 무관하게 위조 descriptor를 만든다).
    private static string MutateType(string descriptor, string name, Action<System.Text.Json.Nodes.JsonObject> change)
    {
        System.Text.Json.Nodes.JsonNode root = System.Text.Json.Nodes.JsonNode.Parse(descriptor)!;
        change(root["types"]!.AsArray().Select(node => node!.AsObject()).Single(node => (string?)node["name"] == name));
        return root.ToJsonString(new JsonSerializerOptions { WriteIndented = true });
    }

    private static System.Text.Json.Nodes.JsonObject FieldNode(System.Text.Json.Nodes.JsonObject type, string name) =>
        type["fields"]!.AsArray().Select(node => node!.AsObject()).Single(node => (string?)node["name"] == name);

    private static JsonElement FieldNodeOf(JsonDocument descriptor, string type, string name) =>
        Type(descriptor, type).GetProperty("fields").EnumerateArray().Single(field => field.GetProperty("name").GetString() == name);

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

        // Lower를 struct 판(Point·Link·Parent.origin)으로, Upper Value를 struct 필드(Shape·Point)를 쓰는 판으로 바꾸고 명세를 더한다.
        public void WriteStructModules()
        {
            Write("Lower/Public/Lower/Parent.h", StructParent);
            Write("Upper/Public/Upper/Shape.h", Shape);
            Write("Upper/Public/Upper/Value.h", Value.Replace("#include <Object/ObjectRefs.h>\n", "#include <Object/ObjectRefs.h>\n#include <Upper/Shape.h>\n", StringComparison.Ordinal)
                .Replace("ROCKS_PROPERTY() rocks::ObjectRef<Parent> next;\n",
                    "ROCKS_PROPERTY() rocks::ObjectRef<Parent> next;\nROCKS_PROPERTY() Shape shape;\nROCKS_PROPERTY() Point at;\n", StringComparison.Ordinal));
            Manifest("Lower", ["probe::Point", "probe::Link", "probe::Mode", "probe::WorldBits", "probe::Outer"],
                ["probe::Point::x", "probe::Link::target", "probe::Link::at", "probe::Parent::origin", "probe::WorldBits::value", "probe::Outer::inner"]);
            Manifest("Upper", ["probe::Shape"], ["probe::Shape::corner", "probe::Shape::link", "probe::Value::shape", "probe::Value::at"]);
        }

        // B5c 세 모듈 구성(Elements -> Lower -> Upper)의 원본과 명세를 쓴다.
        public void WriteArrayModules()
        {
            Write("Elements/Public/Elements/Items.h", ArrayElements);
            Write("Lower/Public/Lower/Parent.h", ArrayParent);
            Write("Upper/Public/Upper/Value.h", ArrayValue);
            Manifest("Elements", ["probe::EA", "probe::SA", "probe::Plain"], ["probe::SA::target", "probe::SA::count", "probe::Plain::x"]);
            Manifest("Lower", ["probe::Group"], ["probe::Group::slots", "probe::Group::plains", "probe::Parent::slots", "probe::Parent::modes",
                "probe::Parent::plains", "probe::Parent::links"]);
            Manifest("Upper", [], ["probe::Value::group"]);
        }

        // 세 모듈 구성의 한 모듈을 생성한다: Elements는 import가 없고, Lower는 Elements를, Upper는 Lower와 Elements를 import한다(빌드가
        // 넘기는 닫힌 descriptor 집합). imports를 주면 그 descriptor로 바꾼다.
        public Task<ProcessResult> RunArrayModuleAsync(string module, IReadOnlyList<string>? imports = null)
        {
            (string include, string[] dependencies, string[] defaultImports) = module switch
            {
                "Elements" => ("Elements/Items.h", Array.Empty<string>(), Array.Empty<string>()),
                "Lower" => ("Lower/Parent.h", new[] { "Elements" }, new[] { Descriptor("Elements") }),
                _ => ("Upper/Value.h", new[] { "Lower", "Elements" }, new[] { Descriptor("Lower"), Descriptor("Elements") })
            };
            Directory.CreateDirectory(Generated(module, "Public"));
            Directory.CreateDirectory(Generated(module, "Private"));
            List<string> arguments = ["--module", module, "--input", Source($"{module}/Public/{include}"), "--include", include];
            foreach (string import in imports ?? defaultImports)
            {
                arguments.AddRange(["--import", import]);
            }

            return RunAsync(module, arguments, [Source($"{module}/Public")], [],
                dependencies.SelectMany(dependency => new[] { Source($"{dependency}/Public"), Generated(dependency, "Public") }).ToList());
        }

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

        public void Manifest(string module, string[] types, string[] fields)
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

            return await ProcessRunner.RunAsync(new(executable, arguments, options.Root, environment, Timeout: MetaTests.ProbeTimeout));
        }

        public void Dispose() => temporary.Dispose();
    }
}
