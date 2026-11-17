using System.Runtime.Loader;

namespace Rocks.Build.Rules;

/// <summary>타깃 하나를 고른 규칙 평가 결과다. <see cref="Modules"/>는 모든 모듈 규칙의 Configure 결과다.</summary>
public sealed record LoadedRules(IReadOnlyList<NativeModuleInput> Modules, TargetRules Target);

/// <summary>모든 타깃을 담은 규칙 평가 결과다(정책 검사가 타깃마다 도달 집합을 본다). 타깃은 이름 순이다.</summary>
public sealed record LoadedRuleSet(IReadOnlyList<NativeModuleInput> Modules, IReadOnlyList<TargetRules> Targets);

/// <summary>
/// 규칙 어셈블리(Rocks.Engine.Rules 또는 게임 규칙)를 읽어 모든 모듈 규칙을 한 문맥으로 평가한다. 빌드 CLI와 정책 검사(Release 배제,
/// Docs/Design/ShaderSystem.ko.md 9절)가 같은 평가를 쓴다. 사용 예:
/// <code>
/// var rules = RuleLoader.Load(rulesPath, "Sandbox", context);          // 빌드
/// var all = RuleLoader.LoadAll(rulesPath, context);                    // 정책 검사
/// </code>
/// </summary>
public static class RuleLoader
{
    /// <summary>
    /// 규칙 어셈블리의 모든 모듈 규칙을 평가하고 targetName 타깃을 고른다.
    /// </summary>
    /// <exception cref="ArgumentException">규칙 생성·평가 실패, 같은 이름의 모듈·타깃, 없는 타깃, 모듈이 없는 타깃.</exception>
    public static LoadedRules Load(string assemblyPath, string targetName, BuildContext context)
    {
        LoadedRuleSet set = LoadAll(assemblyPath, context);
        var selected = set.Targets.SingleOrDefault(target => target.Name == targetName)
            ?? throw new ArgumentException($"Unknown target '{targetName}'. Available: {string.Join(", ", set.Targets.Select(target => target.Name))}");
        if (selected.Modules.Count == 0)
        {
            throw new ArgumentException("A target must select at least one module.");
        }

        return new(set.Modules, selected);
    }

    /// <summary>
    /// 규칙 어셈블리의 모든 모듈 규칙을 평가하고 모든 타깃 규칙을 만든다. 규칙은 문맥(플랫폼·구성)마다 다시 평가해야 하므로 호출마다
    /// Configure를 다시 부른다(어셈블리는 한 번만 올라간다).
    /// </summary>
    /// <exception cref="ArgumentException">규칙 생성·평가 실패, 같은 이름의 모듈·타깃.</exception>
    public static LoadedRuleSet LoadAll(string assemblyPath, BuildContext context)
    {
        var assembly = AssemblyLoadContext.Default.LoadFromAssemblyPath(Path.GetFullPath(assemblyPath));
        var types = assembly.GetTypes().Where(type => !type.IsAbstract).OrderBy(type => type.FullName, StringComparer.Ordinal).ToArray();
        var modules = types.Where(type => typeof(ModuleRules).IsAssignableFrom(type))
            .Select(type => Configure(type, context)).ToArray();
        var targets = types.Where(type => typeof(TargetRules).IsAssignableFrom(type))
            .Select(type => Create<TargetRules>(type)).OrderBy(target => target.Name, StringComparer.Ordinal).ToArray();
        if (modules.GroupBy(module => module.Name, StringComparer.Ordinal).Any(group => group.Count() != 1) ||
            targets.GroupBy(target => target.Name, StringComparer.Ordinal).Any(group => group.Count() != 1))
        {
            throw new ArgumentException("Duplicate module/target rule name.");
        }

        return new(modules, targets);
    }

    private static T Create<T>(Type type)
    {
        try
        {
            return (T)Activator.CreateInstance(type)!;
        }
        catch (Exception error)
        {
            throw new ArgumentException($"Rule '{type.FullName}' could not be constructed: {(error.InnerException ?? error).Message}", error);
        }
    }

    private static NativeModuleInput Configure(Type type, BuildContext context)
    {
        try
        {
            return Create<ModuleRules>(type).Configure(context);
        }
        catch (Exception error)
        {
            throw new ArgumentException($"Module rule '{type.FullName}' failed for {context.Platform}/{context.Configuration}: {(error.InnerException ?? error).Message}", error);
        }
    }
}
