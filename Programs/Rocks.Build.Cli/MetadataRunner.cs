using Rocks.Build.Rules;
using Rocks.Tooling.Common;

namespace Rocks.Build.Cli;

/// <summary>
/// 모듈 소유 메타데이터를 생성한다(Docs/Design/ReflectionFoundations.ko.md 1.3–1.7절). 타깃에서 도달하는 메타데이터
/// 모듈을 의존 순서로 모듈마다 한 번 MetaCompiler로 실행하고, 모든 모듈의 inventory와 명세 집합을 검증한 뒤 생성
/// 헤더·descriptor를 한 번에 게시한다. 실행 중 산출물은 stage에 두며 의존 모듈은 stage의 descriptor와 생성
/// 루트를 본다(이전 빌드의 게시본을 보지 않는다).
/// </summary>
public static class MetadataRunner
{
    private static readonly StringComparer PathComparer =
        OperatingSystem.IsWindows() ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal;

    /// <summary>
    /// roots는 native 빌드와 같은 root 모듈 집합이다(게임 빌드는 켠 플러그인 모듈을 더한다). null이면 타깃 모듈이다.
    /// 반환값은 0(성공 또는 메타데이터 모듈 없음) 또는 1(진단을 출력함)이다.
    /// </summary>
    public static async Task<int> GenerateAsync(BuildContext context, NativeToolchain toolchain, MetadataPlan plan,
        TargetRules target, string compilerTool, CancellationToken cancellationToken, IReadOnlyList<string>? roots = null)
    {
        // ActionGraph와 같이 모듈 이름은 대소문자를 무시해 비교한다.
        var byName = plan.Modules.ToDictionary(module => module.Name, StringComparer.OrdinalIgnoreCase);
        var reachable = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var order = new List<string>();
        var visiting = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        void Reach(string name)
        {
            if (!byName.TryGetValue(name, out NativeModuleInput? module))
            {
                throw new ArgumentException($"Target '{target.Name}': unknown module '{name}'.");
            }

            if (reachable.Contains(module.Name))
            {
                return;
            }

            if (!visiting.Add(module.Name))
            {
                throw new ArgumentException($"Target '{target.Name}': dependency cycle at '{module.Name}'.");
            }

            foreach (string dependency in module.Dependencies)
            {
                Reach(dependency);
            }

            visiting.Remove(module.Name);
            reachable.Add(module.Name);
            order.Add(module.Name); // 후위 순서: 의존 모듈이 먼저다
        }

        foreach (string name in roots ?? target.Modules)
        {
            Reach(name);
        }

        var metadata = plan.Generated.ToDictionary(module => module.Name, StringComparer.OrdinalIgnoreCase);
        List<MetadataModule> runs = order.Where(metadata.ContainsKey).Select(name => metadata[name]).ToList();
        List<NativeModuleInput> imported = order.Select(name => byName[name]).Where(module => module.ImportedMetadata is not null).ToList();
        if (runs.Count == 0)
        {
            return 0;
        }

        string tag = Guid.NewGuid().ToString("N");
        string stage = Path.Combine(context.OutputDirectory, ".metadata-stage", tag);
        string StageRoot(string module) => Path.Combine(stage, module);
        string StageDescriptor(string module) => Path.Combine(stage, module + ".rocksmeta.json");
        string Inventory(string module) => Path.Combine(stage, module + ".inventory.json");

        // 생성 include 루트(자기 모듈 포함)를 이번 실행의 stage 루트로 바꾼다.
        var rootMap = new Dictionary<string, string>(PathComparer);
        foreach (MetadataModule run in runs)
        {
            rootMap[Path.GetFullPath(run.GeneratedPublic)] = Path.Combine(StageRoot(run.Name), "Public");
            rootMap[Path.GetFullPath(run.GeneratedPrivate)] = Path.Combine(StageRoot(run.Name), "Private");
        }

        var manifests = new List<(string Path, byte[] Snapshot)>();
        bool cleanupSafe = true;
        bool commandFailed = false;
        Exception? pendingError = null;
        try
        {
            foreach (MetadataModule run in runs)
            {
                Directory.CreateDirectory(Path.Combine(StageRoot(run.Name), "Public"));
                Directory.CreateDirectory(Path.Combine(StageRoot(run.Name), "Private"));
                manifests.Add((run.Manifest, BoundedFile.Read(run.Manifest, BoundedFile.MaximumIdentityBytes)));
            }

            foreach (MetadataModule run in runs)
            {
                cancellationToken.ThrowIfCancellationRequested();
                NativeModuleInput owner = byName[run.Name];
                List<NativeModuleInput> dependencies = TransitiveDependencies(owner, byName);
                var arguments = new List<string> { compilerTool, "--module", run.Name };
                foreach (MetadataHeader header in run.Headers)
                {
                    arguments.AddRange(["--input", header.Path, "--include", header.Include]);
                    if (header.Private)
                    {
                        arguments.AddRange(["--private", header.Include]);
                    }
                }

                arguments.AddRange(["--output-root", StageRoot(run.Name), "--descriptor", StageDescriptor(run.Name),
                    "--identity-manifest", run.Manifest, "--identity-inventory", Inventory(run.Name), "--compiler", toolchain.CompilerPath]);
                foreach (NativeModuleInput dependency in dependencies)
                {
                    if (metadata.ContainsKey(dependency.Name))
                    {
                        arguments.AddRange(["--import", StageDescriptor(dependency.Name)]);
                    }
                    else if (dependency.ImportedMetadata is not null)
                    {
                        arguments.AddRange(["--import", dependency.ImportedMetadata.Descriptor]);
                    }
                }

                // native compile과 같은 순서/프로필을 사용한다. 다른 모듈의 private 설정을 누출하지 않는다.
                foreach (string argument in owner.AdditionalCompileArguments.Concat(NativeCompileProfile.Arguments(toolchain, context.Configuration, owner.Optimization)))
                {
                    arguments.Add("--clang-arg=" + argument);
                }

                foreach (string define in owner.PublicDefines.Concat(owner.PrivateDefines).Concat(dependencies.SelectMany(module => module.PublicDefines)).Distinct())
                {
                    arguments.Add("--clang-arg=" + (toolchain.Platform == NativePlatform.Windows ? "/D" : "-D") + define);
                }

                foreach (string directory in MetadataLayout.IncludeOrder(owner, byName))
                {
                    arguments.AddRange(["--include-dir", rootMap.TryGetValue(directory, out string? staged) ? staged : directory]);
                }

                var generated = await ProcessRunner.RunAsync(new("dotnet", arguments, context.RootDirectory,
                    toolchain.Environment, Timeout: TimeSpan.FromMinutes(3)), cancellationToken);
                Console.Write(generated.StdOut);
                Console.Error.Write(generated.StdErr);
                if (generated.ExitCode != 0)
                {
                    commandFailed = true;
                    return generated.ExitCode;
                }
            }

            // 명세별 inventory 검증, 그 뒤 명세 사이 ID 중복 검증(SDK 동봉 명세 포함).
            foreach (MetadataModule run in runs)
            {
                int verified = await RunIdsAsync(context, toolchain, compilerTool,
                    ["ids", "verify", "--manifest", run.Manifest, "--inventory", Inventory(run.Name)], cancellationToken);
                if (verified != 0)
                {
                    commandFailed = true;
                    return verified;
                }
            }

            // 은퇴 항목만 남은 명세(ID 예약)도 넣어 예약된 ID·이름의 재사용을 막는다.
            var set = new List<string> { "ids", "verify-set" };
            foreach (string manifest in runs.Select(run => run.Manifest).Concat(plan.ReservedManifests)
                .Concat(imported.Select(module => module.ImportedMetadata!.IdentityManifest)))
            {
                set.AddRange(["--manifest", manifest]);
            }

            int unique = await RunIdsAsync(context, toolchain, compilerTool, set, cancellationToken);
            if (unique != 0)
            {
                commandFailed = true;
                return unique;
            }

            // 백업은 생성 include 디렉터리 밖에 둔다(복구에 실패해 남은 백업이 다음 게시의 잉여 파일로 지워지지 않게).
            MetadataOutputTransaction.Publish(PublishItems(runs, StageRoot, StageDescriptor), manifests, cancellationToken,
                backupDirectory: Path.Combine(context.OutputDirectory, ".metadata-backup", tag));
            return 0;
        }
        catch (Exception error)
        {
            pendingError = error;
            // 아직 실행 중일 수 있는 child가 사용한 임시 파일은 삭제하지 않는다.
            if (error is ProcessCleanupException)
            {
                cleanupSafe = false;
            }

            throw;
        }
        finally
        {
            if (cleanupSafe)
            {
                MetadataTemporaryCleanup.Finish([stage], pendingError is not null || commandFailed,
                    delete: path =>
                    {
                        if (Directory.Exists(path))
                        {
                            Directory.Delete(path, true);
                        }
                    });
            }
        }
    }

    // stage의 생성물을 게시 위치로, 게시 위치에만 있는 잉여 생성 파일은 삭제 항목으로 만든다. 대소문자만 다른 경로는
    // 그 볼륨이 대소문자를 무시할 때만 삭제하지 않는다 — 같은 파일이라 쓰기 항목이 이미 덮어쓴다(macOS 기본·Windows).
    // 대소문자를 구분하는 볼륨에서는 다른 파일이므로 지운다.
    private static List<(string? Staged, string Final)> PublishItems(IReadOnlyList<MetadataModule> runs,
        Func<string, string> stageRoot, Func<string, string> stageDescriptor)
    {
        var items = new List<(string? Staged, string Final)>();
        foreach (MetadataModule run in runs)
        {
            foreach ((string staged, string published) in new[] { ("Public", run.GeneratedPublic), ("Private", run.GeneratedPrivate) })
            {
                string source = Path.Combine(stageRoot(run.Name), staged);
                var produced = new HashSet<string>(StringComparer.Ordinal);
                var producedIgnoringCase = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                foreach (string file in Directory.EnumerateFiles(source, "*", SearchOption.AllDirectories).Order(StringComparer.Ordinal))
                {
                    string final = Path.Combine(published, Path.GetRelativePath(source, file));
                    produced.Add(Path.GetFullPath(final));
                    producedIgnoringCase.Add(Path.GetFullPath(final));
                    items.Add((file, final));
                }

                if (Directory.Exists(published))
                {
                    foreach (string existing in Directory.EnumerateFiles(published, "*", SearchOption.AllDirectories).Order(StringComparer.Ordinal))
                    {
                        string full = Path.GetFullPath(existing);
                        bool sameFile = produced.Contains(full) || (producedIgnoringCase.Contains(full) && CaseInsensitiveVolume(full));
                        if (!sameFile)
                        {
                            items.Add((null, existing));
                        }
                    }
                }
            }

            items.Add((stageDescriptor(run.Name), run.Descriptor));
        }

        return items;
    }

    // 있는 파일의 이름 대소문자를 바꾼 경로가 같은 파일로 열리면 그 볼륨은 대소문자를 무시한다. 바꿀 글자가 없으면
    // 구분하는 것으로 본다(지우는 쪽이 안전하지 않으므로 아무 이름도 같은 파일로 보지 않는다).
    private static bool CaseInsensitiveVolume(string existing)
    {
        string name = Path.GetFileName(existing);
        string swapped = new(name.Select(c => char.IsUpper(c) ? char.ToLowerInvariant(c) : char.ToUpperInvariant(c)).ToArray());
        return !string.Equals(name, swapped, StringComparison.Ordinal) && File.Exists(Path.Combine(Path.GetDirectoryName(existing)!, swapped));
    }

    private static List<NativeModuleInput> TransitiveDependencies(NativeModuleInput module, IReadOnlyDictionary<string, NativeModuleInput> byName)
    {
        var visited = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var result = new List<NativeModuleInput>();
        void Visit(string name)
        {
            NativeModuleInput current = byName[name];
            if (!visited.Add(current.Name))
            {
                return;
            }

            foreach (string dependency in current.Dependencies)
            {
                Visit(dependency);
            }

            if (!string.Equals(current.Name, module.Name, StringComparison.OrdinalIgnoreCase))
            {
                result.Add(current);
            }
        }

        Visit(module.Name);
        return result;
    }

    private static async Task<int> RunIdsAsync(BuildContext context, NativeToolchain toolchain, string compilerTool,
        IReadOnlyList<string> command, CancellationToken cancellationToken)
    {
        var result = await ProcessRunner.RunAsync(new("dotnet", [compilerTool, .. command], context.RootDirectory,
            toolchain.Environment, Timeout: TimeSpan.FromMinutes(2)), cancellationToken);
        Console.Write(result.StdOut);
        Console.Error.Write(result.StdErr);
        return result.ExitCode;
    }
}
