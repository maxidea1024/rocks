using System.Security;
using System.Text.Json;
using Rocks.Build.Rules;
using Rocks.Tooling.Common;

namespace Rocks.Build.Cli;

public sealed record SdkModule(string Name, string[] Includes, string[] Defines, string[] Dependencies, string[] Libraries);
public sealed record SdkManifest(int Version, string Configuration, string Platform, string Architecture,
    string CompilerIdentity, string SdkIdentity, SdkModule[] Modules, Dictionary<string, string> Files);

public static class InstalledSdk
{
    public static async Task ExportAsync(string root, string destination, NativeConfiguration configuration,
        NativeToolchain toolchain, IReadOnlyList<NativeModuleInput> modules, NativeBuildResult build,
        CancellationToken cancellationToken)
    {
        destination = Path.GetFullPath(destination);
        if (Directory.Exists(destination) || File.Exists(destination))
            throw new IOException("SDK destination already exists. Use a new versioned directory.");
        var stage = destination + ".staging-" + Guid.NewGuid().ToString("N");
        Directory.CreateDirectory(stage);
        try
        {
            var packaged = new List<SdkModule>();
            foreach (var module in modules.Where(module => module.Kind == NativeModuleKind.StaticLibrary))
            {
                if (module.PrebuiltLibraries.Count != 0 || module.AdditionalLinkArguments.Count != 0 || module.AdditionalCompileArguments.Count != 0)
                    throw new NotSupportedException($"SDK module '{module.Name}' declares external libraries/custom compile or link flags. Add an explicit SDK packaging/ABI rule before exporting this module.");
                var includes = new List<string>();
                foreach (var directory in module.PublicIncludeDirectories)
                {
                    var relative = $"Headers/{module.Name}/{includes.Count}";
                    CopyTree(directory, Path.Combine(stage, relative));
                    includes.Add(relative);
                }
                var libraries = new List<string>();
                if (build.ModuleOutputs.TryGetValue(module.Name, out var library))
                {
                    var relative = "Libraries/" + Path.GetFileName(library);
                    Directory.CreateDirectory(Path.Combine(stage, "Libraries"));
                    File.Copy(library, Path.Combine(stage, relative));
                    libraries.Add(relative);
                }
                else if (module.Sources.Count != 0)
                    throw new InvalidOperationException($"Module {module.Name} was not built for this SDK target.");
                packaged.Add(new(module.Name, includes.ToArray(), module.PublicDefines.ToArray(),
                    module.Dependencies.ToArray(), libraries.ToArray()));
            }
            // SDK 자체의 도구만으로 외부 게임 규칙을 빌드할 수 있어야 한다.
            foreach (var tool in new[] { "Rocks.Build.Cli", "Rocks.MetaCompiler" })
                CopyTree(Path.GetDirectoryName(BuildCommand.HostTool(root, tool))!, Path.Combine(stage, "Tools", tool));
            File.Copy(Path.Combine(root, "global.json"), Path.Combine(stage, "global.json"));
            File.Copy(Path.Combine(root, ".clang-format"), Path.Combine(stage, ".clang-format"));
            var hashes = Directory.GetFiles(stage, "*", SearchOption.AllDirectories).Order(StringComparer.Ordinal)
                .ToDictionary(file => Path.GetRelativePath(stage, file).Replace('\\', '/'), Hashing.Sha256File, StringComparer.Ordinal);
            var manifest = new SdkManifest(1, configuration.ToString(), toolchain.Platform.ToString(),
                System.Runtime.InteropServices.RuntimeInformation.ProcessArchitecture.ToString(), toolchain.AbiIdentity,
                toolchain.AbiIdentity, packaged.ToArray(), hashes);
            AtomicFile.WriteIfChanged(Path.Combine(stage, "rocks-sdk.json"), JsonSerializer.Serialize(manifest, BuildCommand.JsonOptions));
            cancellationToken.ThrowIfCancellationRequested();
            Directory.Move(stage, destination);
            Console.WriteLine($"Installed SDK: {destination}");
            await Task.CompletedTask;
        }
        finally
        {
            // 삭제 대상은 이 호출이 만든 동일 부모의 임의 stage 하나로 제한한다.
            if (Directory.Exists(stage)) Directory.Delete(stage, recursive: true);
        }
    }

    public static async Task<int> BuildGameAsync(Dictionary<string, string> options, CancellationToken cancellationToken)
    {
        var sdk = Path.GetFullPath(options["sdk"]);
        var project = Path.GetFullPath(options["project"]);
        var output = Path.GetFullPath(options.GetValueOrDefault("output", Path.Combine(project, "out", BuildCommand.Configuration(options).ToString())));
        if (Inside(sdk, output) || Inside(sdk, project))
            throw new ArgumentException("Game source/output must be outside the immutable SDK.");
        Directory.CreateDirectory(output);
        // 앞 실행의 PASS 보고서가 이번 실패를 성공으로 보이게 하지 않는다. 네이티브 출력 자체는 보존한다.
        AtomicFile.WriteIfChanged(Path.Combine(output, "action-report.json"), JsonSerializer.Serialize(new NativeBuildResult
        {
            Succeeded = false, ExecutedActions = 0, SkippedActions = 0, Actions = [],
            ModuleOutputs = new Dictionary<string, string>(), CompilationDatabasePath = "", Failure = "Current game build has not completed."
        }, BuildCommand.JsonOptions));
        var started = DateTime.UtcNow;
        var status = "FAIL";
        string? failure = null;
        try
        {
            var result = await BuildGameCoreAsync(options, cancellationToken);
            status = result == 0 ? "PASS" : "FAIL";
            return result;
        }
        catch (Exception error)
        {
            failure = error.Message;
            throw;
        }
        finally
        {
            AtomicFile.WriteIfChanged(Path.Combine(output, "game-build-evidence.json"), JsonSerializer.Serialize(new
            {
                status, startedUtc = started, finishedUtc = DateTime.UtcNow, sdk, project, error = failure
            }, BuildCommand.JsonOptions));
        }
    }

    private static async Task<int> BuildGameCoreAsync(Dictionary<string, string> options, CancellationToken cancellationToken)
    {
        var sdk = Path.GetFullPath(options["sdk"]);
        var project = Path.GetFullPath(options["project"]);
        var configuration = BuildCommand.Configuration(options);
        var output = Path.GetFullPath(options.GetValueOrDefault("output", Path.Combine(project, "out", configuration.ToString())));
        if (Inside(sdk, output) || Inside(sdk, project))
            throw new ArgumentException("Game source/output must be outside the immutable SDK.");
        var toolchain = await NativeToolchains.DiscoverAsync(options.GetValueOrDefault("compiler"), cancellationToken);
        var manifest = Verify(sdk, configuration, toolchain);
        Directory.CreateDirectory(output);
        Directory.CreateDirectory(Path.Combine(output, "Generated"));
        var rulesFiles = Directory.GetFiles(project, "*.cs", SearchOption.AllDirectories)
            .Where(file => !Inside(output, file) && (file.EndsWith(".Build.cs", StringComparison.Ordinal) || file.EndsWith(".Target.cs", StringComparison.Ordinal)))
            .Order(StringComparer.Ordinal).ToArray();
        if (rulesFiles.Length == 0)
            throw new ArgumentException("No *.Build.cs or *.Target.cs rules found in the game project.");
        var rulesDirectory = Path.Combine(output, "Rules");
        var projectFile = Path.Combine(rulesDirectory, "Rocks.Game.Rules.csproj");
        string Escape(string value) => SecurityElement.Escape(value)!;
        var references = new[] { "Rocks.Build.Abstractions", "Rocks.Build", "Rocks.Tooling.Common" }.Select(name =>
            $"<Reference Include=\"{name}\"><HintPath>{Escape(Path.Combine(sdk, "Tools/Rocks.Build.Cli", name + ".dll"))}</HintPath></Reference>");
        // 사용자 게임 디렉터리의 Directory.Build.props가 호스트 bootstrap을 바꾸지 못하게 입력을 고정한다.
        var projectXml = "<Project Sdk=\"Microsoft.NET.Sdk\"><PropertyGroup>" +
            "<TargetFramework>net10.0</TargetFramework><Nullable>enable</Nullable><ImplicitUsings>enable</ImplicitUsings>" +
            "<TreatWarningsAsErrors>true</TreatWarningsAsErrors><EnableDefaultCompileItems>false</EnableDefaultCompileItems>" +
            "<AssemblyName>Rocks.Game.Rules</AssemblyName><Deterministic>true</Deterministic>" +
            "</PropertyGroup><ItemGroup>" + string.Join("", rulesFiles.Select(file => $"<Compile Include=\"{Escape(file)}\" />")) +
            string.Join("", references) + "</ItemGroup></Project>\n";
        AtomicFile.WriteIfChanged(projectFile, projectXml);
        var compiled = await ProcessRunner.RunAsync(new("dotnet", ["build", projectFile, "--nologo", "-warnaserror",
            "-p:ImportDirectoryBuildProps=false", "-p:ImportDirectoryBuildTargets=false"], sdk, toolchain.Environment), cancellationToken);
        Console.Write(compiled.StdOut);
        Console.Error.Write(compiled.StdErr);
        if (compiled.ExitCode != 0) return 1;
        var rulesPath = Path.Combine(rulesDirectory, "bin/Debug/net10.0/Rocks.Game.Rules.dll");
        var context = new BuildContext(project, output, toolchain.Platform, configuration, sdk);
        var rules = RuleLoader.Load(rulesPath, options.GetValueOrDefault("target", "Game"), context);
        var modules = manifest.Modules.Select(module => new NativeModuleInput
        {
            Name = module.Name,
            PublicIncludeDirectories = module.Includes.Select(path => SafePath(sdk, path)).ToArray(),
            PublicDefines = module.Defines,
            Dependencies = module.Dependencies,
            PrebuiltLibraries = module.Libraries.Select(path => SafePath(sdk, path)).ToArray()
        }).Concat(rules.Modules).ToArray();
        var metadata = await MetadataRunner.GenerateAsync(context, toolchain, modules, rules.Target,
            Path.Combine(sdk, "Tools/Rocks.MetaCompiler/Rocks.MetaCompiler.dll"), cancellationToken);
        if (metadata != 0) return 1;
        var result = await new NativeBuildEngine().BuildAsync(new()
        {
            Toolchain = toolchain, Configuration = configuration, OutputDirectory = output,
            RootDirectory = project, Modules = modules, Targets = rules.Target.Modules,
            ContextFingerprint = "rocks-installed-sdk-v1",
            MaxParallelism = BuildCommand.Jobs(options)
        }, cancellationToken);
        BuildCommand.Print(result);
        AtomicFile.WriteIfChanged(Path.Combine(output, "action-report.json"), JsonSerializer.Serialize(result, BuildCommand.JsonOptions));
        // 실행 중 SDK 변경도 성공으로 인정하지 않는다.
        Verify(sdk, configuration, toolchain);
        return result.Succeeded ? 0 : 1;
    }

    public static SdkManifest Verify(string sdk, NativeConfiguration configuration, NativeToolchain toolchain)
    {
        var manifest = JsonSerializer.Deserialize<SdkManifest>(File.ReadAllText(Path.Combine(sdk, "rocks-sdk.json")), new JsonSerializerOptions { UnmappedMemberHandling = System.Text.Json.Serialization.JsonUnmappedMemberHandling.Disallow })
            ?? throw new InvalidDataException("Empty SDK manifest.");
        if (manifest.Version != 1 || manifest.Configuration != configuration.ToString() ||
            manifest.Platform != toolchain.Platform.ToString() || manifest.CompilerIdentity != toolchain.AbiIdentity ||
            manifest.SdkIdentity != toolchain.AbiIdentity ||
            manifest.Architecture != System.Runtime.InteropServices.RuntimeInformation.ProcessArchitecture.ToString())
            throw new InvalidDataException("SDK configuration, compiler, platform, architecture, or native SDK ABI mismatch.");
        if (manifest.Files is null || manifest.Modules is null || manifest.Files.Count == 0 || manifest.Modules.Length == 0)
            throw new InvalidDataException("SDK manifest is missing its file inventory or module declarations.");
        var actual = new HashSet<string>(StringComparer.Ordinal);
        void Inventory(string directory)
        {
            foreach (var entry in Directory.EnumerateFileSystemEntries(directory))
            {
                if ((File.GetAttributes(entry) & FileAttributes.ReparsePoint) != 0)
                    throw new InvalidDataException("SDK inventory must not contain linked files/directories.");
                if (Directory.Exists(entry)) Inventory(entry);
                else if (Path.GetRelativePath(sdk, entry) != "rocks-sdk.json")
                    actual.Add(Path.GetRelativePath(sdk, entry).Replace('\\', '/'));
            }
        }
        Inventory(sdk);
        if (!actual.SetEquals(manifest.Files.Keys))
            throw new InvalidDataException("SDK file inventory changed. Re-export instead of adding/removing installed headers or tools.");
        if (manifest.Modules.Select(module => module.Name).Distinct(StringComparer.OrdinalIgnoreCase).Count() != manifest.Modules.Length)
            throw new InvalidDataException("SDK contains duplicate module names.");
        foreach (var pair in manifest.Files)
            if (!File.Exists(SafePath(sdk, pair.Key)) || Hashing.Sha256File(SafePath(sdk, pair.Key)) != pair.Value)
                throw new InvalidDataException($"SDK file changed or missing: {pair.Key}");
        foreach (var module in manifest.Modules)
        {
            if (module.Includes is null || module.Libraries is null || module.Dependencies is null || module.Defines is null)
                throw new InvalidDataException($"SDK module '{module.Name}' has incomplete declarations.");
            foreach (var include in module.Includes)
                if (!Directory.Exists(SafePath(sdk, include))) throw new InvalidDataException($"Missing SDK include directory: {include}");
            foreach (var dependency in module.Dependencies)
                if (!manifest.Modules.Any(candidate => candidate.Name == dependency))
                    throw new InvalidDataException($"SDK module '{module.Name}' has unknown dependency '{dependency}'.");
            foreach (var library in module.Libraries)
                if (!manifest.Files.ContainsKey(library)) throw new InvalidDataException("Unverified SDK library.");
        }
        return manifest;
    }

    private static string SafePath(string root, string relative)
    {
        var full = Path.GetFullPath(relative, root);
        if (Path.IsPathRooted(relative) || !Inside(root, full) || relative.Split('/', '\\').Any(part => part == ".."))
            throw new InvalidDataException("SDK path escapes its root.");
        return full;
    }

    private static bool Inside(string root, string candidate) => Path.GetFullPath(candidate)
        .StartsWith(Path.TrimEndingDirectorySeparator(Path.GetFullPath(root)) + Path.DirectorySeparatorChar,
            OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal) ||
        string.Equals(Path.GetFullPath(candidate), Path.GetFullPath(root), OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal);

    private static void CopyTree(string source, string destination)
    {
        Directory.CreateDirectory(destination);
        foreach (var entry in Directory.EnumerateFileSystemEntries(source))
        {
            if ((File.GetAttributes(entry) & FileAttributes.ReparsePoint) != 0)
                throw new IOException("SDK packaging does not accept symbolic links/reparse points.");
            var target = Path.Combine(destination, Path.GetFileName(entry));
            if (Directory.Exists(entry)) CopyTree(entry, target);
            else File.Copy(entry, target);
        }
    }
}
