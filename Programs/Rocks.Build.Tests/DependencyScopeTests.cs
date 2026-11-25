using Rocks.Build;

/// <summary>
/// 모듈 의존의 공개 범위(Docs/Design/BuildTargets 3.5절, BS0)를 계획(ActionGraph)으로 검사한다. 컴파일하지 않는다.
/// <list type="bullet">
/// <item>보임: 비공개 의존(Mid → Base)의 공개 include는 Mid에 보이지만 Mid의 소비자(OnlyMid)에게는 보이지 않는다. 공개 의존(Pub →
/// Base)은 소비자(ThroughPub)에게도 보인다. 비공개 의존의 공개 의존(Mid → Base → Root)도 Mid에 보인다.</item>
/// <item>도달: 비공개 의존도 링크한다(OnlyMid 링크가 Base archive를 받는다).</item>
/// <item>입력 오류: 한 모듈을 두 목록에 함께 적으면 거부한다.</item>
/// </list>
/// </summary>
internal static class DependencyScopeTests
{
    public static void Run(NativeBuildRequest request, string root, Action<string> pass)
    {
        string fixture = Directory.CreateDirectory(Path.Combine(root, "dependency-scope")).FullName;
        string Write(string relative, string content)
        {
            string path = Path.GetFullPath(Path.Combine(fixture, relative));
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            File.WriteAllText(path, content);
            return path;
        }

        string Include(string name) => Directory.CreateDirectory(Path.Combine(fixture, name, "include")).FullName;
        NativeModuleInput[] modules =
        [
            new() { Name = "Root", Sources = [Write("root/root.cpp", "int RootValue() noexcept { return 1; }\n")], PublicIncludeDirectories = [Include("root")] },
            new() { Name = "Base", Sources = [Write("base/base.cpp", "int BaseValue() noexcept { return 2; }\n")], PublicIncludeDirectories = [Include("base")], PublicDependencies = ["Root"] },
            new() { Name = "Mid", Sources = [Write("mid/mid.cpp", "int MidValue() noexcept { return 3; }\n")], PublicIncludeDirectories = [Include("mid")],
                PrivateDependencies = ["Base"] },
            new() { Name = "Pub", Sources = [Write("pub/pub.cpp", "int PubValue() noexcept { return 4; }\n")], PublicIncludeDirectories = [Include("pub")],
                PublicDependencies = ["Base"] },
            new() { Name = "OnlyMid", Kind = NativeModuleKind.Executable, Sources = [Write("onlymid/main.cpp", "int main() { return 0; }\n")],
                PublicDependencies = ["Mid"] },
            new() { Name = "ThroughPub", Kind = NativeModuleKind.Executable, Sources = [Write("throughpub/main.cpp", "int main() { return 0; }\n")],
                PublicDependencies = ["Pub"] }
        ];
        NativeBuildRequest build = request with
        {
            RootDirectory = Path.GetFullPath(request.RootDirectory),
            OutputDirectory = Path.Combine(fixture, "out"),
            Modules = modules,
            Targets = ["OnlyMid", "ThroughPub"],
            Unity = false,
            Pch = false
        };
        NativePlan plan = ActionGraph.Create(build);
        string Arguments(string module) => string.Join(" ", plan.Actions.Single(action => action.Kind == "Compile" && action.Module == module).Arguments);
        bool Sees(string module, string what) => Arguments(module).Contains(what, StringComparison.Ordinal);
        string baseInclude = Path.Combine(fixture, "base", "include");
        string rootInclude = Path.Combine(fixture, "root", "include");

        Check(Sees("Mid", baseInclude) && Sees("Mid", rootInclude),
            "a private dependency and its public dependencies must be visible to the module: " + Arguments("Mid"));
        Check(!Sees("OnlyMid", baseInclude) && !Sees("OnlyMid", rootInclude) && Sees("OnlyMid", Path.Combine(fixture, "mid", "include")),
            "a private dependency must not be visible to the consumer: " + Arguments("OnlyMid"));
        Check(Sees("ThroughPub", baseInclude) && Sees("ThroughPub", rootInclude),
            "a public dependency must be visible to the consumer: " + Arguments("ThroughPub"));
        var byName = modules.ToDictionary(module => module.Name, StringComparer.OrdinalIgnoreCase);
        Check(ModuleVisibility.Of(byName["OnlyMid"], byName).Select(module => module.Name).SequenceEqual(["Mid"]) &&
            ModuleVisibility.Of(byName["Mid"], byName).Select(module => module.Name).SequenceEqual(["Root", "Base"]),
            "visible module order (dependency first)");
        pass("dependency scope: a private dependency is visible to the module only, a public one to consumers too");

        // 비공개 의존도 링크한다(정적 링크 — 실행 파일은 두 목록의 도달 집합을 링크한다).
        NativeAction link = plan.Actions.Single(action => action.Kind == "Link" && action.Module == "OnlyMid");
        Check(link.Inputs.Any(input => Path.GetFileNameWithoutExtension(input).Contains("Base", StringComparison.Ordinal)) &&
            link.Inputs.Any(input => Path.GetFileNameWithoutExtension(input).Contains("Root", StringComparison.Ordinal)),
            "a private dependency must still be linked: " + string.Join(", ", link.Inputs));
        pass("dependency scope: private dependencies are linked");

        try
        {
            InputValidation.Validate(build with
            {
                Modules = modules.Select(module => module.Name == "Mid" ? module with { PublicDependencies = ["Base"] } : module).ToArray()
            });
            Check(false, "a module listed in both dependency lists must be rejected");
        }
        catch (ArgumentException error)
        {
            Check(error.Message.Contains("is in both PublicDependencies and PrivateDependencies", StringComparison.Ordinal),
                "unexpected message for a module listed twice: " + error.Message);
        }

        pass("dependency scope: a module in both lists is rejected");

        // 프로세스 전역 상태가 있는 서드파티(HasGlobalState — BuildTargets 3.8절 BD29·BS8c)는 직접 의존 모듈이 하나여야 한다. Root를 그런
        // 모듈로 표시하면 Base 하나만 직접 의존하므로 통과하고, Mid가 비공개로 더 의존하면 둘이라 두 이름을 알리며 거부한다. 간접 소비(Pub →
        // Base → Root)는 세지 않는다.
        NativeModuleInput[] stateful = modules.Select(module => module.Name == "Root" ? module with { HasGlobalState = true, ThirdParty = true } : module).ToArray();
        InputValidation.Validate(build with { Modules = stateful });
        try
        {
            InputValidation.Validate(build with
            {
                Modules = stateful.Select(module => module.Name == "Mid" ? module with { PrivateDependencies = ["Base", "Root"] } : module).ToArray()
            });
            Check(false, "a global-state module with two direct consumers must be rejected");
        }
        catch (ArgumentException error)
        {
            Check(error.Message.Contains("Module Root has process-wide state", StringComparison.Ordinal) &&
                error.Message.Contains("found Base, Mid", StringComparison.Ordinal),
                "unexpected message for two direct consumers of a global-state module: " + error.Message);
        }

        // Monolithic 시험 실행 파일(GlobalStateTestConsumer)은 소유 모듈과 함께 직접 소비해도 세지 않는다(3.8절 시험 실행 파일 예외 —
        // 개정 38). 실행 파일이 아니거나 Modular이면 표지 자체를 거부한다.
        NativeModuleInput[] withTestRunner = [.. stateful, new NativeModuleInput
        {
            Name = "Runner", Kind = NativeModuleKind.Executable, PrivateDependencies = ["Root"], GlobalStateTestConsumer = true
        }];
        InputValidation.Validate(build with { Modules = withTestRunner });
        try
        {
            InputValidation.Validate(build with
            {
                Modules = withTestRunner.Select(module => module.Name == "Runner" ? module with { GlobalStateTestConsumer = false } : module).ToArray()
            });
            Check(false, "a test executable without GlobalStateTestConsumer still counts as a consumer");
        }
        catch (ArgumentException error)
        {
            Check(error.Message.Contains("found Base, Runner", StringComparison.Ordinal),
                "unexpected message for an unmarked test executable consumer: " + error.Message);
        }

        try
        {
            InputValidation.Validate(build with
            {
                Modules = stateful.Select(module => module.Name == "Mid" ? module with { GlobalStateTestConsumer = true } : module).ToArray()
            });
            Check(false, "GlobalStateTestConsumer on a library module must be rejected");
        }
        catch (ArgumentException error)
        {
            Check(error.Message.Contains("GlobalStateTestConsumer is only for a test executable", StringComparison.Ordinal),
                "unexpected message for GlobalStateTestConsumer on a library: " + error.Message);
        }

        try
        {
            InputValidation.Validate(build with { Modules = withTestRunner, LinkType = NativeLinkType.Modular, TargetName = "Scope" });
            Check(false, "GlobalStateTestConsumer in a Modular link must be rejected");
        }
        catch (ArgumentException error)
        {
            Check(error.Message.Contains("GlobalStateTestConsumer is only allowed with LinkType Monolithic", StringComparison.Ordinal),
                "unexpected message for GlobalStateTestConsumer in a Modular link: " + error.Message);
        }

        try
        {
            InputValidation.Validate(build with
            {
                Modules = modules.Select(module => module.Name == "Root" ? module with { HasGlobalState = true } : module).ToArray()
            });
            Check(false, "HasGlobalState on a non-third-party module must be rejected");
        }
        catch (ArgumentException error)
        {
            Check(error.Message.Contains("HasGlobalState is only for a third-party module", StringComparison.Ordinal),
                "unexpected message for HasGlobalState on an engine module: " + error.Message);
        }

        // 서드파티 모듈만 거쳐 닿는 Rocks 모듈도 소비 모듈이고, 서드파티 모듈 자체는 세지 않는다(BD29 — ICU를 쓰는 HarfBuzz). Base를
        // 서드파티로 두면 Base는 세지 않고 Base를 의존하는 Mid·Pub이 둘 다 Root에 닿아 거부한다.
        try
        {
            InputValidation.Validate(build with
            {
                Modules = stateful.Select(module => module.Name == "Base" ? module with { ThirdParty = true } : module).ToArray()
            });
            Check(false, "two Rocks modules reaching a global-state module through a third-party module must be rejected");
        }
        catch (ArgumentException error)
        {
            Check(error.Message.Contains("Module Root has process-wide state", StringComparison.Ordinal) &&
                error.Message.Contains("found Mid, Pub", StringComparison.Ordinal),
                "unexpected message for consumers through a third-party chain: " + error.Message);
        }

        pass("dependency scope: a global-state module accepts one direct consumer and rejects two");

        // RTTI는 서드파티 모듈만 켤 수 있고(ThirdParty.json rtti — ICU4C), 켠 모듈의 C++ 번역 단위만 프로필의 RTTI 끔 뒤에 켬을 받는다
        // (Docs/Design/Compression.ko.md 3.3절 C++ 서드파티).
        bool windows = request.Toolchain.Platform == NativePlatform.Windows;
        string rttiOff = windows ? "/GR-" : "-fno-rtti";
        string rttiOn = windows ? "/GR" : "-frtti";
        NativeModuleInput[] rttiModules = modules.Select(module => module.Name == "Root" ? module with { ThirdParty = true, RuntimeTypeInformation = true } : module).ToArray();
        NativePlan rttiPlan = ActionGraph.Create(build with { Modules = rttiModules });
        IReadOnlyList<string> rootArguments = rttiPlan.Actions.Single(action => action.Kind == "Compile" && action.Module == "Root").Arguments;
        IReadOnlyList<string> baseArguments = rttiPlan.Actions.Single(action => action.Kind == "Compile" && action.Module == "Base").Arguments;
        int offIndex = rootArguments.ToList().LastIndexOf(rttiOff);
        int onIndex = rootArguments.ToList().LastIndexOf(rttiOn);
        Check(offIndex >= 0 && onIndex > offIndex, "an RTTI third-party module must turn RTTI on after the profile turns it off: " + string.Join(" ", rootArguments));
        Check(!baseArguments.Contains(rttiOn), "a module without RuntimeTypeInformation keeps RTTI off: " + string.Join(" ", baseArguments));
        try
        {
            InputValidation.Validate(build with
            {
                Modules = modules.Select(module => module.Name == "Root" ? module with { RuntimeTypeInformation = true } : module).ToArray()
            });
            Check(false, "RuntimeTypeInformation on a non-third-party module must be rejected");
        }
        catch (ArgumentException error)
        {
            Check(error.Message.Contains("RuntimeTypeInformation is only for a third-party module", StringComparison.Ordinal),
                "unexpected message for RuntimeTypeInformation on an engine module: " + error.Message);
        }

        pass("dependency scope: only a third-party module turns RTTI on, for its own C++ sources");

        // 공유 PCH 제공 모듈(Mid)의 closure는 공개 환경이다(비공개 의존 Base는 넣지 않는다). 도달 closure를 쓰면 소비자 OnlyMid의 include에
        // Base가 없어 "cannot use the shared PCH"로 계획이 실패한다.
        Write("mid/include/mid_pch.h", "#pragma once\n");
        NativeModuleInput[] pchModules = modules.Select(module => module.Name == "Mid" ? module with { SharedPchHeader = "mid_pch.h" } : module).ToArray();
        NativePlan pchPlan = ActionGraph.Create(build with { Modules = pchModules, Pch = true });
        Check(pchPlan.Actions.Any(action => action.Kind == "Pch"),
            "a shared PCH provider with a private dependency must still plan its PCH for consumers");
        pass("dependency scope: a shared PCH provider's closure is its public environment");
    }

    private static void Check(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}
