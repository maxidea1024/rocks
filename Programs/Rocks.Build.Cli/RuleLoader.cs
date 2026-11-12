using System.Reflection;
using System.Runtime.Loader;
using Rocks.Build.Rules;

namespace Rocks.Build.Cli;

public sealed record LoadedRules(IReadOnlyList<NativeModuleInput> Modules, TargetRules Target);

public static class RuleLoader
{
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

    public static LoadedRules Load(string assemblyPath, string targetName, BuildContext context)
    {
        var assembly = AssemblyLoadContext.Default.LoadFromAssemblyPath(Path.GetFullPath(assemblyPath));
        var types = assembly.GetTypes().Where(type => !type.IsAbstract).OrderBy(type => type.FullName, StringComparer.Ordinal).ToArray();
        var modules = types.Where(type => typeof(ModuleRules).IsAssignableFrom(type))
            .Select(type => Configure(type, context)).ToArray();
        var targets = types.Where(type => typeof(TargetRules).IsAssignableFrom(type))
            .Select(type => Create<TargetRules>(type)).ToArray();
        if (modules.GroupBy(module => module.Name, StringComparer.Ordinal).Any(group => group.Count() != 1) ||
            targets.GroupBy(target => target.Name, StringComparer.Ordinal).Any(group => group.Count() != 1))
        {
            throw new ArgumentException("Duplicate module/target rule name.");
        }

        var selected = targets.SingleOrDefault(target => target.Name == targetName)
            ?? throw new ArgumentException($"Unknown target '{targetName}'. Available: {string.Join(", ", targets.Select(target => target.Name))}");
        if (selected.Modules.Count == 0)
        {
            throw new ArgumentException("A target must select at least one module.");
        }

        return new(modules, selected);
    }
}
