using Rocks.Build.Rules;
using Rocks.Tooling.Common;

namespace Rocks.Build.Cli;

/// <summary>
/// 모듈 소유 메타데이터를 생성한다(Docs/Design/ReflectionFoundations.ko.md 1.3–1.7절). 타깃에서 도달하는 메타데이터
/// 모듈을 의존 순서로 모듈마다 한 번 MetaCompiler로 실행하고, 모든 모듈의 inventory와 명세 집합을 검증한 뒤 생성
/// 헤더·생성 소스·descriptor를 한 번에 게시한다. 실행 중 산출물은 stage에 두며 의존 모듈은 stage의 descriptor와 생성
/// 루트를 본다(이전 빌드의 게시본을 보지 않는다). 생성 소스(&lt;stage&gt;/&lt;모듈&gt;/Source — 타입별 본문과 등록 함수 정의)는 include
/// 루트가 아니고 MetaCompiler가 컴파일하지 않는다 — 게시한 뒤 빌드가 그 모듈의 번역 단위로 컴파일한다. 그래서 여기의 게시 전 검증은
/// 생성 헤더까지다. verifySources를 켜면 MetaCompiler가 생성 소스도 게시 전에 컴파일해 본문 계약 위반을 원본 위치와 함께 알린다
/// (Docs/Design/ReflectionGeneratedSources.ko.md 6–7절).
/// </summary>
public static class MetadataRunner
{
    private static readonly StringComparer PathComparer =
        OperatingSystem.IsWindows() ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal;

    /// <summary>
    /// roots는 native 빌드와 같은 root 모듈 집합이다(게임 빌드는 켠 플러그인 모듈을 더한다). null이면 타깃 모듈이다.
    /// 반환값은 0(성공 또는 메타데이터 모듈 없음) 또는 1(진단을 출력함)이다. jobs는 MetaCompiler 한 실행이 함께 돌리는 검증 컴파일
    /// 수다(빌드의 --jobs, null이면 MetaCompiler 기본값). verifySources는 게시 전에 생성 소스도 컴파일하게 한다
    /// (명령 옵션 --verify-metadata-sources on).
    /// </summary>
    public static async Task<int> GenerateAsync(BuildContext context, NativeToolchain toolchain, MetadataPlan plan,
        TargetRules target, string compilerTool, CancellationToken cancellationToken, IReadOnlyList<string>? roots = null,
        int? jobs = null, bool verifySources = false)
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

            foreach (string dependency in module.AllDependencies)
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

        // 실행 중 적재 모듈(DynamicModules)도 생성 대상이다. 링크 폐포를 다 본 뒤 따로 닿으므로 동적 관계는 순환 검사의 참조 사슬이 아니다
        // (Docs/Design/ModuleLoader 3.8절). 그 모듈의 descriptor import는 링크 의존만 따르므로 후위 순서가 그대로 생성 순서다.
        for (int index = 0; index < order.Count; ++index)
        {
            foreach (string dynamic in byName[order[index]].PluginModules.Concat(byName[order[index]].DynamicModules))
            {
                Reach(dynamic);
            }
        }

        var metadata = plan.Generated.ToDictionary(module => module.Name, StringComparer.OrdinalIgnoreCase);
        HashSet<string> entryModules = ModuleDefinitions.EntryModules(plan.Modules);
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
                Directory.CreateDirectory(Path.Combine(StageRoot(run.Name), MetadataModule.SourceDirectoryName));
                manifests.Add((run.Manifest, BoundedFile.Read(run.Manifest, BoundedFile.MaximumIdentityBytes)));
            }

            foreach (MetadataModule run in runs)
            {
                cancellationToken.ThrowIfCancellationRequested();
                NativeModuleInput owner = byName[run.Name];
                List<NativeModuleInput> dependencies = TransitiveDependencies(owner, byName);
                // import 범위는 보임이 아니라 도달(두 목록)이다. import한 descriptor의 타입(그 모듈의 private 타입 포함)이 쓰는 다른
                // 모듈 타입도 이어야 하므로 비공개 의존 쪽 descriptor까지 닫힌 집합이어야 한다(Docs/Design/BuildTargets 3.5절 BD8).
                NativeModuleInput[] importScope = LinkClosure.Of(owner.Name, byName, false).Select(name => byName[name]).ToArray();
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
                foreach (NativeModuleInput dependency in importScope)
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
                foreach (string argument in owner.AdditionalCompileArguments.Concat(NativeCompileProfile.ModuleArguments(toolchain, context.Configuration, owner,
                    NativeLanguage.Cpp, target.LinkType)))
                {
                    arguments.Add("--clang-arg=" + argument);
                }

                // 타깃 매크로 넷은 넘기지 않는다: 메타 컴파일러가 에디터·게임 두 조합을 스스로 정한다(Docs/Design/BuildTargets 3.4절 BS2c — 메타데이터는
                // 타깃과 무관한 합집합).

                // 모듈 정의는 native compile과 같은 정의 헤더로 넘긴다(Docs/Design/UnityBuildPch.ko.md 8.2절 — AST와 compile의 정의가
                // 갈라지지 않는다). 헤더는 여기서 쓰고, 빌드가 같은 내용을 다시 계산해 그대로 둔다.
                string[] defines = ModuleDefinitions.Of(owner, dependencies, entryModules.Contains(owner.Name), target.LinkType, toolchain.Platform);
                if (defines.Length != 0)
                {
                    string definitions = ModuleDefinitions.PathOf(context.OutputDirectory, owner.Name);
                    AtomicFile.WriteIfChanged(definitions, new System.Text.UTF8Encoding(false).GetBytes(ModuleDefinitions.Content(defines)));
                    arguments.AddRange(["--definitions-header", definitions]);
                }

                foreach (string directory in MetadataLayout.IncludeOrder(owner, byName))
                {
                    arguments.AddRange(["--include-dir", rootMap.TryGetValue(directory, out string? staged) ? staged : directory]);
                }

                if (jobs is int verifyJobs)
                {
                    arguments.AddRange(["--jobs", verifyJobs.ToString(System.Globalization.CultureInfo.InvariantCulture)]);
                }

                if (verifySources)
                {
                    arguments.Add("--verify-sources");
                }

                // 리플렉션 클래스·struct의 표지(ROCKS_GENERATED_BODY)를 요구한다(Docs/Design/ReflectionGeneratedSources.ko.md 8.1절).
                arguments.Add("--require-generated-body");

                // 인자는 응답 파일로 넘긴다: 헤더가 많은 모듈은 --input·--include 쌍만으로 Windows 명령줄 상한(32,767자)을 넘는다
                // (Docs/Design/ReflectionGeneratedSources.ko.md 6.3절). 파일은 한 줄에 인자 하나이고 stage 안에 둔다.
                string responseFile = Path.Combine(StageRoot(run.Name), "metacompiler.rsp");
                File.WriteAllText(responseFile, string.Join('\n', arguments.Skip(1)) + "\n", new System.Text.UTF8Encoding(false));
                var generated = await ProcessRunner.RunAsync(new("dotnet", [compilerTool, "@" + responseFile], context.RootDirectory,
                    toolchain.Environment, Timeout: TimeSpan.FromMinutes(3)), cancellationToken);
                Console.Write(generated.StdOut);
                Console.Error.Write(generated.StdErr);
                if (generated.ExitCode != 0)
                {
                    commandFailed = true;
                    return generated.ExitCode;
                }

                // 배치(MetadataLayout)가 모듈의 리플렉션 생성 소스로 넣은 파일을 MetaCompiler가 실제로 냈는지 확인한다. 이름 규칙이
                // 어긋난 도구 조합이면 여기서 멈춘다(게시 뒤 액션 그래프의 "Source does not exist"보다 원인이 분명하다).
                string stagedSources = Path.Combine(StageRoot(run.Name), MetadataModule.SourceDirectoryName);
                foreach (string relative in run.Headers.Select(MetadataModule.TypeSourceRelative).Append(MetadataModule.ModuleSourceName(run.Name)))
                {
                    if (!File.Exists(Path.Combine(stagedSources, relative)))
                    {
                        throw new InvalidOperationException($"Module '{run.Name}': Rocks.MetaCompiler did not produce the generated source " +
                            $"'{relative.Replace('\\', '/')}'. Rebuild the host tools so that Rocks.Build.Cli and Rocks.MetaCompiler match.");
                    }
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
            foreach ((string staged, string published) in new[] { ("Public", run.GeneratedPublic), ("Private", run.GeneratedPrivate),
                         (MetadataModule.SourceDirectoryName, run.GeneratedSource) })
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

    // module의 컴파일 환경에 보이는 의존 모듈이다(전파 정의 — ModuleVisibility, Docs/Design/BuildTargets 3.5절 BD8).
    private static List<NativeModuleInput> TransitiveDependencies(NativeModuleInput module, IReadOnlyDictionary<string, NativeModuleInput> byName) =>
        ModuleVisibility.Of(module, byName, (name, from) =>
            throw new ArgumentException($"Module '{from}' depends on unknown module '{name}'."));

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
