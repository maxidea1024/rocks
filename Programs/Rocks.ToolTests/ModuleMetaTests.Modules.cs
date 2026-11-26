using System.Text.Json;
using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

// 모듈 모드 시험 중 모듈 생성·import·가시성·도구 이름과 모듈 descriptor 시험이다.
internal static partial class ModuleMetaTests
{
    // 모듈 생성·모듈 생성 헤더와 소스·import 오류·철자 충돌·포함 순서·상속 형질·타입 도구 이름·에셋 값 종류 왕복 시험이다.
    private static void RegisterModules(TestSuite suite, Options options, IReadOnlyDictionary<string, string?> environment)
    {
        suite.Add("Meta.ModuleNameMayEqualAReflectedClassName", async () =>
        {
            // 등록 함수 이름공간 rocks::generated::Lower와 클래스 rocks::Lower가 서로 가리지 않는다(생성 코드는 이름을 ::rocks::로 한정한다 —
            // --verify-sources 검증 컴파일이 생성 헤더와 모듈 생성 소스를 컴파일한다). World 모듈과 rocks::World의 관계와 같다.
            using Layout layout = new(options, environment);
            layout.Write("Lower/Public/Lower/Parent.h", SameNameParent);
            layout.Manifest("Lower", ["rocks::Lower"], ["rocks::Lower::value"]);
            ProcessResult lower = await layout.RunLowerAsync();
            Verify.Equal(0, lower.ExitCode, "module named like its reflected class: " + lower.StdErr);
            string aggregate = File.ReadAllText(layout.Generated("Lower", "Public/Lower/Lower.module.generated.h"));
            Verify.Contains("namespace rocks::generated::Lower", aggregate, "module header namespace");
            string moduleSource = File.ReadAllText(layout.ModuleSource("Lower"));
            Verify.Contains("namespace rocks::generated::Lower", moduleSource, "module source namespace");
            Verify.Contains("GeneratedType<::rocks::Lower>", moduleSource, "module source registers the same-named class");
            string generated = layout.GeneratedText("Lower", "Public/Lower/Parent.generated.h");
            Verify.Contains("struct GeneratedType<::rocks::Lower>", generated, "generated specialization is fully qualified");
        });

        suite.Add("Meta.ModuleGeneratesHeadersAggregatesAndDescriptor", async () =>
        {
            using Layout layout = new(options, environment);
            ProcessResult lower = await layout.RunLowerAsync();
            Verify.Equal(0, lower.ExitCode, "lower module: " + lower.StdErr);
            Verify.That(File.Exists(layout.Generated("Lower", "Public/Lower/Parent.generated.h")), "lower generated header");
            // 모듈 생성 헤더는 등록 함수의 선언만 담는다: include는 Object/ModuleRegistration.h 하나이고 타입별 생성 헤더·GeneratedType을
            // 끌어오지 않는다(등록만 하는 번역 단위가 모듈의 생성 코드를 모두 컴파일하지 않게 — ReflectionGeneratedSources 5.5절).
            string lowerAggregate = File.ReadAllText(layout.Generated("Lower", "Public/Lower/Lower.module.generated.h"));
            Verify.Contains("namespace rocks::generated::Lower", lowerAggregate, "lower module header namespace");
            Verify.That(Compact(lowerAggregate).Contains(Compact("::rocks::Result<void> RegisterModuleTypes(::rocks::ModuleRegistration& registration, " +
                "const ::rocks::ModuleLease& module = {}) noexcept;"), StringComparison.Ordinal), "lower module header declares the registration function:\n" + lowerAggregate);
            Verify.Equal(1, lowerAggregate.Split("#include").Length - 1, "module header include count");
            Verify.Contains("#include <Object/ModuleRegistration.h>", lowerAggregate, "module header include");
            Verify.That(!lowerAggregate.Contains("GeneratedType<", StringComparison.Ordinal) && !lowerAggregate.Contains("inline", StringComparison.Ordinal) &&
                !lowerAggregate.Contains("moduleKey", StringComparison.Ordinal), "module header must hold only the declaration:\n" + lowerAggregate);
            // 모듈 생성 소스가 자기 모듈 생성 헤더와 타입별 생성 헤더를 include하고 등록 함수를 한 번 정의한다.
            string lowerSource = File.ReadAllText(layout.ModuleSource("Lower"));
            Verify.Contains("#include <Lower/Lower.module.generated.h>", lowerSource, "module source includes its module header");
            Verify.Contains("#include <Lower/Parent.generated.h>", lowerSource, "module source includes the per-type generated header");
            Verify.Contains("GeneratedType<::probe::Parent>", lowerSource, "lower module source registers its type");
            Verify.That(Compact(lowerSource).Contains(Compact("::rocks::Result<void> RegisterModuleTypes(::rocks::ModuleRegistration& registration, " +
                "const ::rocks::ModuleLease& module) noexcept {"), StringComparison.Ordinal) && !lowerSource.Contains("inline", StringComparison.Ordinal) &&
                !lowerSource.Contains("RegisterPrivateModuleTypes", StringComparison.Ordinal),
                "lower module source defines only the public registration function, out of line and without a default argument:\n" + lowerSource);
            Verify.That(!Directory.Exists(layout.Generated("Lower", "Private/Lower")), "public-only module emitted a private module header");
            // 생성 소스는 입력 헤더마다 타입별 생성 소스 하나(Source/Public 또는 Source/Private 아래 원본 철자)와 모듈 생성 소스 하나다.
            Verify.That(File.Exists(layout.Generated("Lower", "Source/Public/Lower/Parent.generated.cpp")), "lower type generated source");
            Verify.Equal(2, Directory.GetFiles(layout.Generated("Lower", "Source"), "*", SearchOption.AllDirectories).Length, "generated source count");
            Verify.That(!File.ReadAllBytes(layout.Descriptor("Lower")).Contains((byte)'\r'), "descriptor must use LF on every platform");

            ProcessResult upper = await layout.RunUpperAsync();
            Verify.Equal(0, upper.ExitCode, "upper module: " + upper.StdErr);
            foreach (string relative in new[] { "Public/Upper/Value.generated.h", "Public/Upper/Leaf.generated.h", "Private/Upper/Detail.generated.h" })
            {
                Verify.That(File.Exists(layout.Generated("Upper", relative)), "missing generated header " + relative);
            }

            // 모듈 생성 헤더 둘(public·private)은 선언만 담고 의존 모듈의 헤더도 include하지 않는다.
            string aggregate = File.ReadAllText(layout.Generated("Upper", "Public/Upper/Upper.module.generated.h"));
            string privateAggregate = File.ReadAllText(layout.Generated("Upper", "Private/Upper/Upper.private.module.generated.h"));
            foreach ((string header, string function, string other) in new[] { (aggregate, "RegisterModuleTypes", "RegisterPrivateModuleTypes"),
                         (privateAggregate, "RegisterPrivateModuleTypes", "RegisterModuleTypes(") })
            {
                Verify.That(Compact(header).Contains(Compact($"::rocks::Result<void> {function}(::rocks::ModuleRegistration& registration, " +
                    "const ::rocks::ModuleLease& module = {}) noexcept;"), StringComparison.Ordinal), $"module header declares {function}:\n" + header);
                Verify.That(header.Split("#include").Length - 1 == 1 && header.Contains("#include <Object/ModuleRegistration.h>", StringComparison.Ordinal) &&
                    !header.Contains("GeneratedType<", StringComparison.Ordinal) && !header.Contains(other, StringComparison.Ordinal),
                    "module header must hold only its declaration:\n" + header);
            }

            // 모듈 생성 소스는 두 등록 함수를 정의한다. public 함수는 import한 모듈을 먼저 등록하고 이 모듈 타입을 base 먼저(헤더를 건너)
            // 등록한다. private 함수는 public 함수를 먼저 부른 뒤 private 타입만 등록한다. 함수마다 자기 모듈 키를 가진다.
            string source = File.ReadAllText(layout.ModuleSource("Upper"));
            foreach (string include in new[] { "Upper/Upper.module.generated.h", "Upper/Upper.private.module.generated.h", "Lower/Lower.module.generated.h",
                         "Upper/Value.generated.h", "Upper/Leaf.generated.h", "Upper/Detail.generated.h" })
            {
                Verify.Contains($"#include <{include}>", source, "module source include");
            }

            string compactSource = Compact(source);
            int privateDefinition = compactSource.IndexOf("::rocks::Result<void>RegisterPrivateModuleTypes(", StringComparison.Ordinal);
            Verify.That(privateDefinition > 0, "module source defines the private registration function after the public one:\n" + source);
            int ownDefinition = compactSource.IndexOf("::rocks::Result<void>RegisterModuleOwnTypes(", StringComparison.Ordinal);
            Verify.That(ownDefinition > privateDefinition, "module source defines the own-types function last:\n" + source);
            string publicBody = compactSource[..privateDefinition];
            string privateBody = compactSource[privateDefinition..ownDefinition];
            // 모듈 로더가 부르는 자기 타입 함수는 의존 모듈·모듈 키 없이 이 모듈 타입만 public 다음 private 순서로 등록한다
            // (Docs/Design/ModuleLoader 3.10절). 두 모듈 생성 헤더가 같은 선언을 둔다.
            string ownBody = compactSource[ownDefinition..];
            int ownValue = ownBody.IndexOf("GeneratedType<::probe::Value>::Schema()", StringComparison.Ordinal);
            int ownLeaf = ownBody.IndexOf("GeneratedType<::probe::Leaf>::Schema()", StringComparison.Ordinal);
            int ownDetail = ownBody.IndexOf("GeneratedType<::probe::Detail>::Schema()", StringComparison.Ordinal);
            Verify.That(ownValue >= 0 && ownLeaf > ownValue && ownDetail > ownLeaf && !ownBody.Contains("EnterModule", StringComparison.Ordinal) &&
                !ownBody.Contains("::rocks::generated::", StringComparison.Ordinal), "own-types function registers only this module's types in order:\n" + source);
            foreach (string header in new[] { aggregate, privateAggregate })
            {
                Verify.That(Compact(header).Contains(Compact("::rocks::Result<void> RegisterModuleOwnTypes(::rocks::ModuleRegistration& registration, " +
                    "const ::rocks::ModuleLease& module) noexcept;"), StringComparison.Ordinal), "module header declares RegisterModuleOwnTypes:\n" + header);
            }

            Verify.That(publicBody.Contains("registration.EnterModule(&moduleKey)", StringComparison.Ordinal) &&
                privateBody.Contains("registration.EnterModule(&moduleKey)", StringComparison.Ordinal) &&
                compactSource.Split("staticconstexprcharmoduleKey=0;").Length - 1 == 2, "each registration function enters the module once per registration with its own key");
            int dependency = publicBody.IndexOf("::rocks::generated::Lower::RegisterModuleTypes", StringComparison.Ordinal);
            int value = publicBody.IndexOf("GeneratedType<::probe::Value>::Schema()", StringComparison.Ordinal);
            int leaf = publicBody.IndexOf("GeneratedType<::probe::Leaf>::Schema()", StringComparison.Ordinal);
            Verify.That(dependency >= 0 && value > dependency && leaf > value, "public registration order:\n" + source);
            Verify.That(!publicBody.Contains("probe::Detail", StringComparison.Ordinal), "public registration function registers a private type");
            int viaPublic = privateBody.IndexOf("::rocks::generated::Upper::RegisterModuleTypes(registration,module)", StringComparison.Ordinal);
            int detail = privateBody.IndexOf("GeneratedType<::probe::Detail>::Schema()", StringComparison.Ordinal);
            Verify.That(viaPublic >= 0 && detail > viaPublic && !privateBody.Contains("::rocks::generated::Lower::", StringComparison.Ordinal) &&
                !privateBody.Contains("probe::Value>::Schema()", StringComparison.Ordinal),
                "private registration function calls the public one first and registers only private types:\n" + source);
            Verify.That(!source.Contains("inline", StringComparison.Ordinal) && !source.Contains("= {}", StringComparison.Ordinal) &&
                !source.Contains("namespace {", StringComparison.Ordinal) && !source.Contains("#define", StringComparison.Ordinal),
                "module source definitions are out of line, without default arguments, file-local names or macros:\n" + source);

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
            // 생성 소스 루트(Source)도 같은 검사를 받는다: 내용이 바뀐 모듈 생성 소스와 남은 잉여 파일을 찾는다.
            string upperSource = layout.ModuleSource("Upper");
            string upperSourceText = File.ReadAllText(upperSource);
            File.WriteAllText(upperSource, upperSourceText + "// edited\n");
            ProcessResult staleSource = await layout.RunUpperAsync(extra: ["--check"]);
            Verify.That(staleSource.ExitCode != 0, "check accepted an edited module generated source");
            Verify.Contains("RMC4000", staleSource.StdErr, "stale module source code");
            Verify.Contains("Upper.module.generated.cpp", staleSource.StdErr, "stale module source path");
            File.WriteAllText(upperSource, upperSourceText);
            string leftoverSource = layout.Generated("Upper", Path.Combine("Source", "Removed.generated.cpp"));
            File.WriteAllText(leftoverSource, "// leftover\n");
            ProcessResult extraSource = await layout.RunUpperAsync(extra: ["--check"]);
            Verify.That(extraSource.ExitCode != 0, "check accepted a leftover file in the generated source root");
            Verify.Contains("RMC4000", extraSource.StdErr, "leftover source code");
            Verify.Contains("Removed.generated.cpp", extraSource.StdErr, "leftover source path");
            File.Delete(leftoverSource);
            Verify.Equal(0, (await layout.RunUpperAsync(extra: ["--check"])).ExitCode, "check after restoring the generated source root");
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

        // 등록 함수는 이름을 ::rocks::로 한정하므로 rocks 타입과 같은 이름(Result)의 모듈도 그 모듈과 의존 모듈의 모듈 생성 소스가
        // 컴파일된다(--verify-sources).
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
            Verify.Contains("::rocks::generated::Result::RegisterModuleTypes", File.ReadAllText(layout.ModuleSource("Upper")),
                "dependency call");
        });

        // --verify-sources는 모듈 생성 소스(등록 함수 정의)도 게시 전에 컴파일한다(Docs/Design/ReflectionGeneratedSources.ko.md 7절). 의존
        // 모듈의 생성 public 루트(모듈 생성 헤더가 있는 곳)를 include 검색 순서에서 빼면 타입별 생성 헤더는 그대로 컴파일되지만 모듈 생성
        // 소스는 의존 모듈의 모듈 생성 헤더를 열지 못한다. 옵션이 있으면 RMC3000으로 거부하고 아무것도 게시하지 않는다. 옵션이 없으면
        // (빌드가 그 모듈의 compile action으로 컴파일하므로) 생성 헤더 검증만으로 게시한다.
        suite.Add("Meta.VerifySourcesCompilesTheModuleGeneratedSource", async () =>
        {
            using Layout layout = new(options, environment);
            Verify.Equal(0, (await layout.RunLowerAsync()).ExitCode, "lower module");
            Directory.CreateDirectory(layout.Generated("Absent", "Public"));
            // 타입 생성 소스는 컴파일되고 모듈 생성 소스(없는 의존 모듈의 집계 헤더를 include한다)만 실패하도록 의존 모듈 타입을 쓰지 않는
            // 입력을 쓴다(다른 모듈 클래스를 base·참조 대상으로 두면 타입 생성 소스가 그 생성 헤더를 include해 먼저 실패한다 — BS8b).
            layout.Write("Upper/Public/Upper/Solo.h", "#pragma once\n#include <Meta/Annotations.h>\n#include <Object/Object.h>\nnamespace probe {\n" +
                "ROCKS_CLASS()\nstruct Solo : public rocks::Object {\nROCKS_GENERATED_BODY()\nROCKS_PROPERTY() rocks::int32 count = 0;\n};\n}\n");
            layout.Manifest("Upper", ["probe::Solo"], ["probe::Solo::count"]);
            (string Path, string Include, bool Private)[] inputs = [(layout.Source("Upper/Public/Upper/Solo.h"), "Upper/Solo.h", false)];
            ProcessResult rejected = await layout.RunUpperInputsAsync(inputs, dependencyModule: "Absent");
            Verify.That(rejected.ExitCode != 0, "--verify-sources accepted a module generated source that does not compile");
            Verify.Contains("RMC3000", rejected.StdErr, "module source verification code");
            Verify.Contains("Source/Upper.module.generated.cpp", rejected.StdErr, "module source verification names the generated source");
            Verify.That(!File.Exists(layout.ModuleSource("Upper")) && !File.Exists(layout.Generated("Upper", "Public/Upper/Solo.generated.h")) &&
                !File.Exists(layout.Generated("Upper", "Public/Upper/Upper.module.generated.h")) && !File.Exists(layout.Descriptor("Upper")),
                "a rejected module generated source published output");

            ProcessResult accepted = await layout.RunUpperInputsAsync(inputs, dependencyModule: "Absent", verifySources: false);
            Verify.Equal(0, accepted.ExitCode, "without --verify-sources the generated source is left to the build: " + accepted.StdErr);
            Verify.That(File.Exists(layout.ModuleSource("Upper")) && File.Exists(layout.Generated("Upper", "Public/Upper/Upper.module.generated.h")),
                "the module generated header and source were not published");

            // 옵션은 한 번만 준다(--check와 같다).
            await Reject(layout.RunUpperAsync(extra: [MetaCompilerArguments.VerifySources]), "RMC1000", "duplicate option --verify-sources");
        });

        // 생성 소스의 native 키(Docs/Design/BuildTargets 3.8절 BS8b): 헤더는 GeneratedType<X>::NativeKey()를 모듈 API 매크로로
        // 선언하고(빌드 밖 fallback 정의 포함) 생성 소스가 정의한다. 생성 소스는 클래스 대상의 생성 헤더를 include한다 — 다른 모듈
        // base에서 물려받은 필드의 대상은 이름 없이 ID로만 이어지므로(import 클래스 대상) ID 경로가 Lower/Target.generated.h를 찾아야
        // 한다. 대상이 ROCKS_GENERATED_BODY 타입이라 빠지면 NativeTypeKey의 static_assert로 검증 컴파일이 실패한다.
        suite.Add("Meta.GeneratedSourceDefinesNativeKeyAndIncludesClassTargets", async () =>
        {
            using Layout layout = new(options, environment);
            layout.Write("Lower/Public/Lower/Target.h", "#pragma once\n#include <Meta/Annotations.h>\n#include <Object/Object.h>\nnamespace probe {\n" +
                "ROCKS_CLASS()\nstruct Target : public rocks::Object {\nROCKS_GENERATED_BODY()\nROCKS_PROPERTY() rocks::int32 value = 0;\n};\n}\n");
            layout.Write("Lower/Public/Lower/Parent.h", "#pragma once\n#include <Lower/Target.h>\n#include <Object/ObjectRefs.h>\nnamespace probe {\n" +
                "ROCKS_CLASS()\nstruct ROCKS_LOWER_API Parent : public rocks::Object {\nROCKS_GENERATED_BODY()\nROCKS_PROPERTY() rocks::int32 value = 0;\n" +
                "ROCKS_PROPERTY() rocks::ObjectRef<Target> target;\n};\n}\n");
            layout.Manifest("Lower", ["probe::Target"], ["probe::Target::value", "probe::Parent::target"]);
            Verify.Equal(0, (await layout.RunLowerAsync(extraInputs: [("Lower/Target.h", "Lower/Target.h")])).ExitCode, "lower module");
            layout.Write("Upper/Public/Upper/Child.h", "#pragma once\n#include <Lower/Parent.h>\nnamespace probe {\n" +
                "ROCKS_CLASS()\nstruct Child : public Parent {\nROCKS_GENERATED_BODY()\nROCKS_PROPERTY() rocks::int32 extra = 0;\n};\n}\n");
            layout.Manifest("Upper", ["probe::Child"], ["probe::Child::extra"]);
            ProcessResult upper = await layout.RunUpperInputsAsync([(layout.Source("Upper/Public/Upper/Child.h"), "Upper/Child.h", false)]);
            Verify.Equal(0, upper.ExitCode, "upper module with an inherited imported class target: " + upper.StdErr);

            string header = File.ReadAllText(layout.Generated("Upper", "Public/Upper/Child.generated.h"));
            string source = File.ReadAllText(layout.Generated("Upper", "Source/Public/Upper/Child.generated.cpp"));
            Verify.Contains("#ifndef ROCKS_UPPER_API", header, "API macro fallback");
            Verify.Contains("static ROCKS_UPPER_API const void* NativeKey() noexcept;", header, "native key declaration");
            Verify.That(!header.Contains("Target.generated.h", StringComparison.Ordinal), "class targets are not included by the generated header");
            Verify.Contains("const void* GeneratedType<::probe::Child>::NativeKey() noexcept {", source, "native key definition");
            Verify.Contains("#include <Lower/Parent.generated.h>", source, "class base generated header");
            Verify.Contains("#include <Lower/Target.generated.h>", source, "inherited imported class target found by ID");
        });

        // struct·enum의 native 키(Docs/Design/BuildTargets 3.8절 BS8d4): 클래스처럼 생성 헤더가 GeneratedType<X>::NativeKey()를
        // 모듈 API 매크로로 선언하고 생성 소스가 정의한다(Modular에서 공유 라이브러리마다 다른 키가 생기지 않게).
        suite.Add("Meta.StructsAndEnumsGetGeneratedNativeKeys", async () =>
        {
            using Layout layout = new(options, environment);
            layout.WriteStructModules();
            Verify.Equal(0, (await layout.RunLowerAsync()).ExitCode, "lower module with structs and an enum");
            string header = File.ReadAllText(layout.Generated("Lower", "Public/Lower/Parent.generated.h"));
            string source = File.ReadAllText(layout.Generated("Lower", "Source/Public/Lower/Parent.generated.cpp"));
            // 타입마다 선언 하나·정의 하나다(class Parent, struct Point·Link·WorldBits·Outer, enum Mode).
            string[] types = ["::probe::Parent", "::probe::Point", "::probe::Link", "::probe::WorldBits", "::probe::Outer", "::probe::Mode"];
            Verify.Equal(types.Length, header.Split("static ROCKS_LOWER_API const void* NativeKey() noexcept;").Length - 1,
                "one native key declaration per class, struct and enum");
            foreach (string type in types)
            {
                Verify.Contains($"const void* GeneratedType<{type}>::NativeKey() noexcept {{", source, "native key definition of " + type);
            }

            // Binds 미러는 실제 타입의 생성 키를 내고, 실제 타입이 BoundCoreValueV 목록에 있는지 생성 소스가 확인한다.
            using Layout bindings = new(options, environment);
            bindings.Write("Lower/Public/Lower/Parent.h", BindsParent);
            bindings.Manifest("Lower", ["probe::Vec3Binding", "probe::Vec3dBinding"],
                ["probe::Vec3Binding::x", "probe::Vec3Binding::y", "probe::Vec3Binding::z", "probe::Vec3dBinding::x", "probe::Vec3dBinding::y",
                    "probe::Vec3dBinding::z"]);
            Directory.CreateDirectory(bindings.Generated("CoreTypeBindings", "Public"));
            Directory.CreateDirectory(bindings.Generated("CoreTypeBindings", "Private"));
            ProcessResult bound = await bindings.RunLowerAsync(module: "CoreTypeBindings");
            Verify.Equal(0, bound.ExitCode, "bindings module: " + bound.StdErr);
            string boundSource = File.ReadAllText(bindings.Generated("CoreTypeBindings", "Source/Public/Lower/Parent.generated.cpp"));
            Verify.Contains("static_assert(::rocks::BoundCoreValueV<::rocks::Vector3<float>>", boundSource, "Binds target list check");
            Verify.Contains("const void* GeneratedType<::rocks::Vector3<float>>::NativeKey() noexcept {", boundSource, "Binds target native key");
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
                .Replace("ROCKS_PROPERTY() rocks::int32 value = 0;\n", "#if ROCKS_WITH_EDITOR_DATA\nROCKS_PROPERTY(Name=\"Amount\", EditorOnly) rocks::int32 value = 0;\n#endif\n",
                    StringComparison.Ordinal));
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

        // 리플렉션 B9d2: 타입 도구 이름(기본은 한정하지 않은 C++ 이름)이 descriptor toolName으로 전해지고, 이 모듈 타입의 이름은 import한
        // 타입의 이름과도 대소문자 무시로 달라야 한다(RMC2203 — 같은 Registry 등록의 AlreadyExists를 먼저 잡는다).
        suite.Add("Meta.TypeToolNamesAreUniqueAcrossImports", async () =>
        {
            using Layout layout = new(options, environment);
            Verify.Equal(0, (await layout.RunLowerAsync()).ExitCode, "lower module");
            using (JsonDocument lower = JsonDocument.Parse(File.ReadAllBytes(layout.Descriptor("Lower"))))
            {
                Verify.Equal("Parent", Type(lower, "probe::Parent").GetProperty("toolName").GetString(), "default type tool name in the descriptor");
            }

            layout.Write("Upper/Public/Upper/Value.h", Value.Replace("ROCKS_CLASS()", "ROCKS_CLASS(Name = \"PARENT\")", StringComparison.Ordinal));
            await Reject(layout.RunUpperAsync(), "RMC2203", "of module Lower");
        });

        // 리플렉션 B9d2: import한 두 모듈의 타입 도구 이름이 대소문자 무시로 겹쳐도 RMC2203이다(그 둘을 함께 쓰는 프로그램의 등록이
        // AlreadyExists가 되기 전에 — 정규 이름 중복의 RMC2004와 같은 자리). Upper가 Lower(probe::Parent — 기본 이름 Parent)와
        // 도구 이름을 PARENT로 바꾼 Elements(probe::Plain)를 함께 import한다.
        suite.Add("Meta.TypeToolNamesAreUniqueAmongImportedModules", async () =>
        {
            using Layout layout = new(options, environment);
            layout.WriteArrayModules();
            Verify.Equal(0, (await layout.RunArrayModuleAsync("Elements")).ExitCode, "elements module");
            Verify.Equal(0, (await layout.RunArrayModuleAsync("Lower")).ExitCode, "lower module");
            Verify.Equal(0, (await layout.RunArrayModuleAsync("Upper")).ExitCode, "upper module before the collision");
            string mutated = layout.Generated("Mutated", "Elements.rocksmeta.json");
            Directory.CreateDirectory(Path.GetDirectoryName(mutated)!);
            File.WriteAllText(mutated, MutateType(File.ReadAllText(layout.Descriptor("Elements")), "probe::Plain",
                plain => plain["toolName"] = "PARENT"));
            await Reject(layout.RunArrayModuleAsync("Upper", [layout.Descriptor("Lower"), mutated]), "RMC2203", "(module Lower)");
            // descriptor의 toolName도 형식을 검사한다(빌드 산출물 위조 — RMC1000).
            File.WriteAllText(mutated, MutateType(File.ReadAllText(layout.Descriptor("Elements")), "probe::Plain",
                plain => plain["toolName"] = "bad name"));
            await Reject(layout.RunArrayModuleAsync("Upper", [layout.Descriptor("Lower"), mutated]), "RMC1000", "toolName of probe::Plain");
        });

        // B3a: AssetId·Sha256Digest·SoftObjectRef 필드가 descriptor에 담기고, import한 base의 소프트 참조 기대 타입이 파생
        // 모듈로 전해진다(Docs/Design/ReflectionFoundations.ko.md 3.1–3.3절).
        suite.Add("Meta.AssetValueKindsRoundTripThroughImportedDescriptors", async () =>
        {
            using Layout layout = new(options, environment);
            layout.Write("Lower/Public/Lower/Parent.h", "#pragma once\n#include <Meta/Annotations.h>\n#include <Object/Object.h>\n" +
                "#include <Object/AssetId.h>\n#include <Object/SoftObjectRef.h>\n#include <Core/Encoding/Sha256.h>\nnamespace probe {\n" +
                "ROCKS_CLASS()\nstruct ROCKS_LOWER_API Parent : public rocks::Object {\nROCKS_GENERATED_BODY()\n" +
                "explicit Parent(rocks::Allocator& allocator) noexcept : icon(allocator), any(allocator) {}\n" +
                "ROCKS_PROPERTY() rocks::int32 value = 0;\nROCKS_PROPERTY() rocks::AssetId source;\nROCKS_PROPERTY() rocks::Sha256Digest hash;\n" +
                "ROCKS_PROPERTY(Required) rocks::SoftObjectRef<Parent> icon;\nROCKS_PROPERTY() rocks::SoftObjectRef<> any;\n};\n}\n");
            layout.Manifest("Lower", [], ["probe::Parent::source", "probe::Parent::hash", "probe::Parent::icon", "probe::Parent::any"]);
            layout.Write("Upper/Public/Upper/Value.h", Value.Replace("struct Value : public Parent {\n",
                "struct Value : public Parent {\nexplicit Value(rocks::Allocator& allocator) noexcept : Parent(allocator), back(allocator) {}\n" +
                "ROCKS_PROPERTY() rocks::SoftObjectRef<Parent> back;\n", StringComparison.Ordinal));
            layout.Manifest("Upper", [], ["probe::Value::back"]);
            ProcessResult lower = await layout.RunLowerAsync();
            Verify.Equal(0, lower.ExitCode, "lower module: " + lower.StdErr);
            ProcessResult upper = await layout.RunUpperInputsAsync([(layout.Source("Upper/Public/Upper/Value.h"), "Upper/Value.h", false)]);
            Verify.Equal(0, upper.ExitCode, "upper module: " + upper.StdErr);

            using JsonDocument lowerDescriptor = JsonDocument.Parse(File.ReadAllBytes(layout.Descriptor("Lower")));
            using JsonDocument upperDescriptor = JsonDocument.Parse(File.ReadAllBytes(layout.Descriptor("Upper")));
            string parentId = Type(lowerDescriptor, "probe::Parent").GetProperty("id").GetString()!;
            JsonElement Field(JsonDocument descriptor, string type, string name) => Type(descriptor, type).GetProperty("fields").EnumerateArray()
                .Single(field => field.GetProperty("name").GetString() == name);
            Verify.Equal("AssetId", Field(lowerDescriptor, "probe::Parent", "source").GetProperty("value").GetProperty("kind").GetString(), "AssetId kind");
            Verify.That(!Field(lowerDescriptor, "probe::Parent", "source").GetProperty("value").TryGetProperty("type", out _), "AssetId has no target type");
            Verify.Equal("Digest256", Field(lowerDescriptor, "probe::Parent", "hash").GetProperty("value").GetProperty("kind").GetString(), "digest kind");
            JsonElement icon = Field(lowerDescriptor, "probe::Parent", "icon");
            Verify.Equal("SoftObjectReference", icon.GetProperty("value").GetProperty("kind").GetString(), "soft reference kind");
            Verify.Equal(parentId, icon.GetProperty("value").GetProperty("type").GetString(), "soft reference target");
            Verify.Equal("Required", icon.GetProperty("flags")[0].GetString(), "Required on a soft reference");
            Verify.Equal("", Field(lowerDescriptor, "probe::Parent", "any").GetProperty("value").GetProperty("type").GetString(), "Object soft reference target");
            Verify.Equal("", Field(upperDescriptor, "probe::Value", "any").GetProperty("value").GetProperty("type").GetString(), "inherited Object soft reference target");

            // 파생 모듈의 자기 소프트 참조 필드도 import한 타입을 기대 타입으로 푼다.
            JsonElement own = Field(upperDescriptor, "probe::Value", "back");
            Verify.Equal("SoftObjectReference", own.GetProperty("value").GetProperty("kind").GetString(), "own soft reference kind");
            Verify.Equal(parentId, own.GetProperty("value").GetProperty("type").GetString(), "own soft reference to an imported type");

            // import한 base의 필드는 파생 descriptor와 생성 스키마에 같은 값 형식으로 펼쳐진다.
            JsonElement inherited = Field(upperDescriptor, "probe::Value", "icon");
            Verify.Equal("SoftObjectReference", inherited.GetProperty("value").GetProperty("kind").GetString(), "inherited soft reference kind");
            Verify.Equal(parentId, inherited.GetProperty("value").GetProperty("type").GetString(), "inherited soft reference target");
            Verify.Equal("Required", inherited.GetProperty("flags")[0].GetString(), "inherited Required flag");
            string generated = layout.GeneratedText("Upper", "Public/Upper/Value.generated.h");
            Verify.Contains("ValueKind::SoftObjectReference", generated, "generated soft reference schema");
            Verify.Contains("ValueKind::Digest256", generated, "generated digest schema");
        });

        // 리플렉션 B9e: Optional 필드(단일 값·enum·값 struct 내부, struct 안 Optional<String> 멤버)가 descriptor로 전해지고, import한
        // base의 Optional 필드가 파생 descriptor와 생성 스키마·함수표(Optional<struct>는 경로 thunk, 단일 값은 read·write)에 같은
        // 형식으로 펼쳐져 컴파일된다(descriptor 읽기의 element·maxElements 규칙).
        suite.Add("Meta.OptionalFieldsRoundTripThroughImportedDescriptors", async () =>
        {
            using Layout layout = new(options, environment);
            layout.Write("Lower/Public/Lower/Parent.h", "#pragma once\n#include <Meta/Annotations.h>\n#include <Object/Object.h>\n" +
                "#include <Core/Optional.h>\n#include <Core/Text/String.h>\nnamespace probe {\n" +
                "ROCKS_ENUM()\nenum class Mode : rocks::uint8 { First = 1, Second = 2 };\n" +
                "ROCKS_STRUCT()\nstruct Leaf {\nROCKS_GENERATED_BODY()\nROCKS_PROPERTY() rocks::Optional<rocks::String> note;\n};\n" +
                "ROCKS_CLASS()\nstruct ROCKS_LOWER_API Parent : public rocks::Object {\nROCKS_GENERATED_BODY()\n" +
                "ROCKS_PROPERTY() rocks::int32 value = 0;\nROCKS_PROPERTY() rocks::Optional<rocks::int32> count;\n" +
                "ROCKS_PROPERTY() rocks::Optional<Mode> mode;\nROCKS_PROPERTY() rocks::Optional<Leaf> leaf;\n};\n}\n");
            layout.Manifest("Lower", ["probe::Mode", "probe::Leaf"], ["probe::Leaf::note", "probe::Parent::count", "probe::Parent::mode", "probe::Parent::leaf"]);
            ProcessResult lower = await layout.RunLowerAsync();
            Verify.Equal(0, lower.ExitCode, "lower module: " + lower.StdErr);
            ProcessResult upper = await layout.RunUpperInputsAsync([(layout.Source("Upper/Public/Upper/Value.h"), "Upper/Value.h", false)]);
            Verify.Equal(0, upper.ExitCode, "upper module: " + upper.StdErr);

            using JsonDocument lowerDescriptor = JsonDocument.Parse(File.ReadAllBytes(layout.Descriptor("Lower")));
            using JsonDocument upperDescriptor = JsonDocument.Parse(File.ReadAllBytes(layout.Descriptor("Upper")));
            JsonElement Field(JsonDocument descriptor, string type, string name) => Type(descriptor, type).GetProperty("fields").EnumerateArray()
                .Single(field => field.GetProperty("name").GetString() == name);
            string leafId = Type(lowerDescriptor, "probe::Leaf").GetProperty("id").GetString()!;
            JsonElement inherited = Field(upperDescriptor, "probe::Value", "leaf");
            Verify.Equal("Optional", inherited.GetProperty("value").GetProperty("kind").GetString(), "inherited optional kind");
            Verify.Equal("Struct", inherited.GetProperty("element").GetProperty("kind").GetString(), "inherited optional inner kind");
            Verify.Equal(leafId, inherited.GetProperty("element").GetProperty("type").GetString(), "inherited optional inner target");
            Verify.Equal(0, inherited.GetProperty("maxElements").GetInt32(), "inherited optional maxElements");
            Verify.Equal("Enum", Field(upperDescriptor, "probe::Value", "mode").GetProperty("element").GetProperty("kind").GetString(), "inherited optional enum");
            Verify.Equal("Utf8String", Field(lowerDescriptor, "probe::Leaf", "note").GetProperty("element").GetProperty("kind").GetString(), "optional struct member");
            string generated = Compact(layout.GeneratedText("Upper", "Public/Upper/Value.generated.h"));
            Verify.Contains("{ValueKind::Optional,Guid{}}", generated, "generated inherited optional schema");
            Verify.Contains("PathClassField<::probe::Value,::rocks::ValueKind::Struct>(cursor,&::probe::Parent::leaf)", generated,
                "inherited Optional<struct> field path thunk");
            Verify.Contains("ReadGeneratedField<::probe::Value>(registry,access,object,&::probe::Parent::count,out)", generated,
                "inherited Optional<scalar> field read thunk");
        });
        // 리플렉션 B9f2: 다른 모듈 descriptor의 Map·Set 필드(enum 키·struct 값)를 상속하면 키 enum이 import enum으로 이어져(생성 헤더
        // include·키 native 키) 생성 스키마가 같은 키·값·상한을 싣는다.
        suite.Add("Meta.MapFieldsRoundTripThroughImportedDescriptors", async () =>
        {
            using Layout layout = new(options, environment);
            layout.Write("Lower/Public/Lower/Parent.h", "#pragma once\n#include <Meta/Annotations.h>\n#include <Object/Object.h>\n" +
                "#include <Core/Containers/OrderedMap.h>\n#include <Core/Containers/OrderedSet.h>\n#include <Core/Text/String.h>\nnamespace probe {\n" +
                "ROCKS_ENUM()\nenum class Mode : rocks::uint8 { First = 1, Second = 2 };\n" +
                "ROCKS_STRUCT()\nstruct Leaf {\nROCKS_GENERATED_BODY()\nROCKS_PROPERTY() rocks::int32 x = 0;\n};\n" +
                "ROCKS_CLASS()\nstruct ROCKS_LOWER_API Parent : public rocks::Object {\nROCKS_GENERATED_BODY()\n" +
                "explicit Parent(rocks::Allocator& allocator) noexcept : leaves(allocator), modes(allocator) {}\n" +
                "ROCKS_PROPERTY() rocks::int32 value = 0;\nROCKS_PROPERTY(MaxElements = 4) rocks::OrderedMap<Mode, Leaf> leaves;\n" +
                "ROCKS_PROPERTY() rocks::OrderedSet<Mode> modes;\n};\n}\n");
            layout.Manifest("Lower", ["probe::Mode", "probe::Leaf"], ["probe::Leaf::x", "probe::Parent::leaves", "probe::Parent::modes"]);
            ProcessResult lower = await layout.RunLowerAsync();
            Verify.Equal(0, lower.ExitCode, "lower module: " + lower.StdErr);
            layout.Write("Upper/Public/Upper/Value.h", "#pragma once\n#include <Lower/Parent.h>\n#include <Object/ObjectRefs.h>\nnamespace probe {\n" +
                "ROCKS_CLASS()\nstruct Value : public Parent {\nROCKS_GENERATED_BODY()\nusing Parent::Parent;\nROCKS_PROPERTY() rocks::ObjectRef<Parent> next;\n};\n}\n");
            ProcessResult upper = await layout.RunUpperInputsAsync([(layout.Source("Upper/Public/Upper/Value.h"), "Upper/Value.h", false)]);
            Verify.Equal(0, upper.ExitCode, "upper module: " + upper.StdErr);

            using JsonDocument lowerDescriptor = JsonDocument.Parse(File.ReadAllBytes(layout.Descriptor("Lower")));
            using JsonDocument upperDescriptor = JsonDocument.Parse(File.ReadAllBytes(layout.Descriptor("Upper")));
            JsonElement Field(JsonDocument descriptor, string type, string name) => Type(descriptor, type).GetProperty("fields").EnumerateArray()
                .Single(field => field.GetProperty("name").GetString() == name);
            string modeId = Type(lowerDescriptor, "probe::Mode").GetProperty("id").GetString()!;
            JsonElement inherited = Field(upperDescriptor, "probe::Value", "leaves");
            Verify.Equal("Map", inherited.GetProperty("value").GetProperty("kind").GetString(), "inherited map kind");
            Verify.Equal("Struct", inherited.GetProperty("element").GetProperty("kind").GetString(), "inherited map value kind");
            Verify.Equal(modeId, inherited.GetProperty("key").GetProperty("type").GetString(), "inherited map enum key target");
            Verify.Equal(4, inherited.GetProperty("maxElements").GetInt32(), "inherited map maxElements");
            Verify.Equal("Set", Field(upperDescriptor, "probe::Value", "modes").GetProperty("value").GetProperty("kind").GetString(), "inherited set");
            string generated = Compact(layout.GeneratedText("Upper", "Public/Upper/Value.generated.h"));
            Verify.Contains("{ValueKind::Map,Guid{}}", generated, "generated inherited map schema");
            Verify.Contains("NativeTypeKey<::probe::Mode>()}", generated, "inherited map enum key native key");
            Verify.Contains("#include<Lower/Parent.generated.h>", generated, "generated header includes the key enum header");
        });
    }

    // private 전용 모듈 등록·모듈 재선언·자기 import와 모듈 descriptor 엄격 읽기 시험이다(enum 시험 뒤에 등록한다).
    private static void RegisterModuleDescriptors(TestSuite suite, Options options, IReadOnlyDictionary<string, string?> environment)
    {
        // private 헤더만 있는 모듈도 private 등록 함수가 import한 모듈을 먼저 등록한다(base가 등록되어 있어야 한다).
        suite.Add("Meta.PrivateOnlyModuleRegistersImportedModules", async () =>
        {
            using Layout layout = new(options, environment);
            Verify.Equal(0, (await layout.RunLowerAsync()).ExitCode, "lower module");
            ProcessResult hidden = await layout.RunUpperInputsAsync([(layout.Source("Upper/Private/Upper/Hidden.h"), "Upper/Hidden.h", true)]);
            Verify.Equal(0, hidden.ExitCode, "private-only module: " + hidden.StdErr);
            Verify.That(!File.Exists(layout.Generated("Upper", "Public/Upper/Upper.module.generated.h")), "private-only module emitted a public module header");
            string privateHeader = File.ReadAllText(layout.Generated("Upper", "Private/Upper/Upper.private.module.generated.h"));
            Verify.That(privateHeader.Contains("RegisterPrivateModuleTypes", StringComparison.Ordinal) && !privateHeader.Contains("Lower", StringComparison.Ordinal),
                "private-only module header declares only the private registration function:\n" + privateHeader);
            string aggregate = File.ReadAllText(layout.ModuleSource("Upper"));
            Verify.Contains("#include <Upper/Upper.private.module.generated.h>", aggregate, "private-only module source includes its module header");
            Verify.Contains("#include <Lower/Lower.module.generated.h>", aggregate, "private-only module source includes the dependency module header");
            Verify.That(!aggregate.Contains("#include <Upper/Upper.module.generated.h>", StringComparison.Ordinal) &&
                !Compact(aggregate).Contains("::rocks::Result<void>RegisterModuleTypes(", StringComparison.Ordinal),
                "private-only module source must not include or define the public registration function:\n" + aggregate);
            int dependency = aggregate.IndexOf("::rocks::generated::Lower::RegisterModuleTypes", StringComparison.Ordinal);
            int own = aggregate.IndexOf("GeneratedType<::probe::Hidden>::Schema()", StringComparison.Ordinal);
            Verify.That(dependency >= 0 && own > dependency, "private-only registration must register the imported base first:\n" + aggregate);
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
        // 상속 export 진단(Docs/Design/BuildTargets 3.8절 BS8d3): 다른 모듈이 상속하는 기반은 선언부에 모듈 API 매크로가 있어야 하고
        // descriptor flags의 Exported로 전해진다. 없으면 파생 모듈이 RMC2002, 같은 모듈 안의 상속은 매크로가 없어도 된다.
        suite.Add("Meta.CrossModuleBaseMustBeExported", async () =>
        {
            using Layout layout = new(options, environment);
            Verify.Equal(0, (await layout.RunLowerAsync()).ExitCode, "lower module with an exported base");
            using (JsonDocument descriptor = JsonDocument.Parse(File.ReadAllBytes(layout.Descriptor("Lower"))))
            {
                Verify.Equal(5, descriptor.RootElement.GetProperty("schema").GetInt32(), "descriptor schema 5");
                Verify.That(Type(descriptor, "probe::Parent").GetProperty("flags").EnumerateArray().Any(flag => flag.GetString() == "Exported"),
                    "the exported class carries the Exported flag");
            }

            ProcessResult upper = await layout.RunUpperAsync();
            Verify.Equal(0, upper.ExitCode, "upper module derives from an exported base (same-module Leaf needs no macro): " + upper.StdErr);
            using (JsonDocument descriptor = JsonDocument.Parse(File.ReadAllBytes(layout.Descriptor("Upper"))))
            {
                Verify.That(!Type(descriptor, "probe::Value").GetProperty("flags").EnumerateArray().Any(flag => flag.GetString() == "Exported"),
                    "a class without the macro has no Exported flag");
            }

            // 기반에서 매크로를 빼면(키워드 앞 주석과 키워드·이름 사이 주석 속 언급은 세지 않는다) 파생 모듈이 RMC2002다.
            foreach (string declaration in new[] { "// ROCKS_LOWER_API\nstruct Parent", "struct /* ROCKS_LOWER_API */ Parent" })
            {
                layout.Write("Lower/Public/Lower/Parent.h", Parent.Replace("struct ROCKS_LOWER_API Parent", declaration, StringComparison.Ordinal));
                Verify.Equal(0, (await layout.RunLowerAsync()).ExitCode, "lower module without the export: " + declaration);
                await Reject(layout.RunUpperAsync(), "RMC2002", "without a class-level export");
            }

            // 다른 모듈의 매크로로 export하면 그 클래스의 모듈이 RMC2007이다(Modular에서 import가 된다).
            layout.Write("Lower/Public/Lower/Parent.h", Parent.Replace("struct ROCKS_LOWER_API Parent", "struct ROCKS_UPPER_API Parent",
                StringComparison.Ordinal));
            await Reject(layout.RunLowerAsync(retargetApiMacro: false), "RMC2007", "API macro of another module");

            // Modular 메타 단계처럼 매크로가 실제 export 철자로 펼쳐져도(AST에 export 속성이 붙는다) Exported로 판정한다.
            layout.Write("Lower/Public/Lower/Parent.h", Parent);
            layout.ApiValues["ROCKS_LOWER_API"] = OperatingSystem.IsWindows() ? "__declspec(dllexport)" : "__attribute__((visibility(\"default\")))";
            Verify.Equal(0, (await layout.RunLowerAsync()).ExitCode, "lower module with the export spelling");
            using (JsonDocument descriptor = JsonDocument.Parse(File.ReadAllBytes(layout.Descriptor("Lower"))))
            {
                Verify.That(Type(descriptor, "probe::Parent").GetProperty("flags").EnumerateArray().Any(flag => flag.GetString() == "Exported"),
                    "the export spelling still carries the Exported flag");
            }

            layout.ApiValues.Clear();

            // 이전 schema descriptor(schema 4 — 리플렉션 함수 이전)는 거부한다.
            Verify.Equal(0, (await layout.RunLowerAsync()).ExitCode, "lower module again");
            string mutated = layout.Generated("Mutated", "schema1.rocksmeta.json");
            Directory.CreateDirectory(Path.GetDirectoryName(mutated)!);
            File.WriteAllText(mutated, File.ReadAllText(layout.Descriptor("Lower")).Replace("\"schema\": 5", "\"schema\": 4", StringComparison.Ordinal));
            await Reject(layout.RunUpperAsync(imports: [mutated]), "RMC1000", "schema");
        });

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
                ("assetBundles", "\"assetBundles\": \"\"", "\"assetBundles\": \"UI\""),
                ("duplicate", "\"module\": \"Lower\"", "\"module\": \"Lower\",\n  \"module\": \"Lower\"")
            })
            {
                Verify.That(original.Contains(from, StringComparison.Ordinal), "descriptor fixture changed: " + name);
                string mutated = layout.Generated("Mutated", name + ".rocksmeta.json");
                Directory.CreateDirectory(Path.GetDirectoryName(mutated)!);
                File.WriteAllText(mutated, original.Replace(from, to, StringComparison.Ordinal));
                await Reject(layout.RunUpperAsync(imports: [mutated]), "RMC1000", "invalid descriptor");
            }

            // 값 종류는 MetaCompiler가 만드는 종류만 받고, 대상 타입 키와 Required는 종류와 맞아야 한다(B3a).
            const string Int32Kind = "\"kind\": \"Int32\"";
            const string ValueField = "\"fieldKind\": \"Value\"";
            int fieldFlags = original.LastIndexOf("\"flags\": []", StringComparison.Ordinal);
            Verify.That(fieldFlags > original.IndexOf(ValueField, StringComparison.Ordinal), "descriptor fixture changed: field flags");
            foreach ((string name, string mutatedText) in new[]
            {
                ("enumKind", original.Replace(Int32Kind, "\"kind\": \"Enum\"", StringComparison.Ordinal)),
                ("opaqueKind", original.Replace(Int32Kind, "\"kind\": \"Opaque\"", StringComparison.Ordinal)),
                ("unknownKind", original.Replace(Int32Kind, "\"kind\": \"Bogus\"", StringComparison.Ordinal)),
                ("softWithoutType", original.Replace(Int32Kind, "\"kind\": \"SoftObjectReference\"", StringComparison.Ordinal)),
                ("assetWithType", original.Replace(Int32Kind, "\"kind\": \"AssetId\",\n            \"type\": \"\"", StringComparison.Ordinal)),
                ("softAsStrongEdge", original.Replace(Int32Kind, "\"kind\": \"SoftObjectReference\",\n            \"type\": \"\"", StringComparison.Ordinal)
                    .Replace(ValueField, "\"fieldKind\": \"ObjectReference\"", StringComparison.Ordinal)),
                ("requiredScalar", original[..fieldFlags] + "\"flags\": [\"Required\"]" + original[(fieldFlags + "\"flags\": []".Length)..]),
                ("mapMergeScalar", original[..fieldFlags] + "\"flags\": [\"TextMapMergeByKey\"]" + original[(fieldFlags + "\"flags\": []".Length)..]),
                ("enumWithoutTarget", original.Replace(Int32Kind, "\"kind\": \"Enum\",\n            \"type\": \"\"", StringComparison.Ordinal))
            })
            {
                Verify.That(mutatedText != original, "descriptor mutation had no effect: " + name);
                string mutated = layout.Generated("Mutated", name + ".rocksmeta.json");
                Directory.CreateDirectory(Path.GetDirectoryName(mutated)!);
                File.WriteAllText(mutated, mutatedText);
                await Reject(layout.RunUpperAsync(imports: [mutated]), "RMC1000", "invalid descriptor");
            }
        });
    }
}
