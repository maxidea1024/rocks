using Rocks.Build;

/// <summary>
/// 모듈 의존의 공개 범위(Docs/Design/BuildTargets.ko.md 3.5절, BS0)를 계획(ActionGraph)으로 검사한다. 컴파일하지 않는다.
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

        pass("dependency scope: a global-state module accepts one direct consumer and rejects two");

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
