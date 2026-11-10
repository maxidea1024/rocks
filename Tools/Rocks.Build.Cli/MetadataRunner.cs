using Rocks.Build.Rules;
using Rocks.Tooling.Common;

namespace Rocks.Build.Cli;

public static class MetadataRunner
{
    public static async Task<int> GenerateAsync(BuildContext context, NativeToolchain toolchain,
        IReadOnlyList<NativeModuleInput> modules, TargetRules target, string compilerTool,
        CancellationToken cancellationToken)
    {
        var byName = modules.ToDictionary(module => module.Name, StringComparer.OrdinalIgnoreCase);
        var outputs = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var unit in target.Metadata)
        {
            if (!byName.TryGetValue(unit.Module, out var owner))
                throw new ArgumentException($"Metadata '{unit.Input}': unknown owner module '{unit.Module}'.");
            if (string.IsNullOrWhiteSpace(unit.Output) || Path.GetFileName(unit.Output) != unit.Output ||
                unit.Output.Contains('\\') || !unit.Output.EndsWith(".generated.h", StringComparison.Ordinal) ||
                !outputs.Add(unit.Output))
                throw new ArgumentException($"Metadata '{unit.Input}': output must be a unique *.generated.h basename.");
            var input = context.PathInRoot(unit.Input);
            var relative = Path.GetRelativePath(context.RootDirectory, input);
            if (!File.Exists(input) || Path.IsPathRooted(relative) || relative.Split('/', '\\').Any(part => part == ".."))
                throw new ArgumentException($"Metadata '{unit.Input}': input must exist inside the project root.");
            if (unit.Include.IndexOfAny(['"', '\r', '\n', '<', '>']) >= 0 ||
                Path.IsPathRooted(unit.Include) || unit.Include.Split('/', '\\').Any(part => part == ".."))
                throw new ArgumentException($"Metadata '{unit.Input}': include must be a project-relative header path.");
            var dependencies = new List<NativeModuleInput>();
            var marks = new Dictionary<string, bool>(StringComparer.OrdinalIgnoreCase);
            void Visit(string name)
            {
                if (!byName.TryGetValue(name, out var module))
                    throw new ArgumentException($"Module '{unit.Module}': unknown dependency '{name}'.");
                if (marks.TryGetValue(name, out var done))
                {
                    if (!done) throw new ArgumentException($"Module dependency cycle at '{name}'.");
                    return;
                }
                marks[name] = false;
                foreach (var dependency in module.Dependencies) Visit(dependency);
                marks[name] = true;
                if (module != owner) dependencies.Add(module);
            }
            Visit(owner.Name);
            var arguments = new List<string> { compilerTool, "--input", input, "--include", unit.Include,
                "--output", Path.Combine(context.OutputDirectory, "Generated", unit.Output),
                "--compiler", toolchain.CompilerPath };
            // native compile과 같은 순서/프로필을 사용한다. 다른 모듈의 private 설정을 누출하지 않는다.
            foreach (var argument in owner.AdditionalCompileArguments.Concat(NativeCompileProfile.Arguments(toolchain, context.Configuration)))
                arguments.Add("--clang-arg=" + argument);
            foreach (var define in owner.PublicDefines.Concat(owner.PrivateDefines).Concat(dependencies.SelectMany(module => module.PublicDefines)).Distinct())
                arguments.Add("--clang-arg=" + (toolchain.Platform == NativePlatform.Windows ? "/D" : "-D") + define);
            foreach (var directory in owner.PublicIncludeDirectories.Concat(owner.PrivateIncludeDirectories)
                .Concat(dependencies.SelectMany(module => module.PublicIncludeDirectories)).Select(context.PathInRoot).Distinct())
                arguments.AddRange(["--include-dir", directory]);
            var generated = await ProcessRunner.RunAsync(new("dotnet", arguments, context.RootDirectory,
                toolchain.Environment, Timeout: TimeSpan.FromMinutes(3)), cancellationToken);
            Console.Write(generated.StdOut);
            Console.Error.Write(generated.StdErr);
            if (generated.ExitCode != 0) return generated.ExitCode;
        }
        return 0;
    }
}
