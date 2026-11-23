using Rocks.Tooling.Common;

namespace Rocks.Build;

/// <summary>
/// include 소유 모듈 검사다(Docs/Design/BuildTargets 3.5절 BD11·BS1b). 성공한 빌드의 컴파일 액션마다 의존 기록(<c>&lt;obj&gt;.deps</c> — 이번
/// 실행에서 게시했거나 증분 검증한 기록)의 헤더가 그 소스에 보이는지 본다.
/// <list type="bullet">
/// <item>소유: 경로를 담는 가장 긴 모듈 폴더(<see cref="NativeModuleInput.ModuleDirectory"/>)의 모듈이다(여러 모듈이 같은 폴더면 그 모두 —
/// 하나라도 보이면 된다. 예: Launch 시험 프로그램 셋이 Programs/LaunchTests를 함께 쓴다). 영역은 public include 디렉터리(공개),
/// <c>&lt;폴더&gt;/Tests</c>(시험), 그 밖(비공개)이다. 메타 생성 폴더 <c>&lt;out&gt;/Generated/&lt;M&gt;/Public·Private</c>는 M의 공개·비공개다.
/// 저장소 밖, 출력의 그 밖 파일, 서드파티 루트, 어떤 모듈 폴더에도 없는 경로는 보지 않는다.</item>
/// <item>보임: 모듈 M의 소스에는 M의 세 영역과 M의 컴파일 환경에 보이는 모듈의 공개 영역, 그리고 M의 include 허가 폴더가 보인다(M 자신의
/// include 디렉터리 가운데 다른 모듈 폴더 안의 것 — 규칙이 일부러 보여 주는 비공개 헤더, 예 AsyncCompression의 Compression/Private). 소스
/// 묶음(<see cref="SourceGroup"/>)의 소스에는 묶음을 가진 모듈과 대상 모듈 T의 세 영역, T의 include 허가 폴더, T의 컴파일 환경에 보이는
/// 모듈의 공개·시험 영역, 추가 모듈의 시험 영역과 그 공개 환경의 공개 영역, 내부 모듈의 비공개 영역이 보인다.</item>
/// <item>공유 PCH: 소비 번역 단위는 그 PCH 액션 기록의 경로를 빼고 보고, PCH 액션은 제공 모듈의 공개 환경으로 본다.</item>
/// </list>
/// 공개 헤더 검사 모듈(<see cref="NativeModuleInput.PublicHeaderCheckOf"/>)의 기록으로 대상 모듈의 쓰지 않는 공개 의존을 알린다.
/// </summary>
internal static class IncludeOwnership
{
    // 실패 문구에 싣는 위반 줄의 최대 수다(나머지는 수만 적는다).
    public const int MaxReportedViolations = 40;

    // 소유 영역이다.
    private enum Area { Public, Tests, Private }

    private sealed record Owner(IReadOnlyList<string> Modules, Area Area)
    {
        public string Name => string.Join("/", Modules);
    }

    /// <summary>위반 전체를 쓰는 파일 이름이다(출력 폴더, 위반이 없으면 지운다).</summary>
    public const string ReportFileName = "include-ownership.txt";

    /// <summary>
    /// 검사 결과다. Violations(앞부분)가 있으면 빌드 실패이고 전체는 ReportPath에 있다. Notices는 쓰지 않는 공개 의존 알림이다(0줄 또는 1줄).
    /// </summary>
    public sealed record Result(IReadOnlyList<string> Violations, int ViolationCount, IReadOnlyList<string> Notices, string ReportPath);

    public static Result Check(NativeBuildRequest request, IReadOnlyList<NativeAction> actions)
    {
        var modules = new Dictionary<string, NativeModuleInput>(StringComparer.OrdinalIgnoreCase);
        foreach (NativeModuleInput module in request.Modules)
        {
            modules.TryAdd(module.Name, module);
        }

        var owners = new OwnerMap(request, modules);
        var byId = actions.ToDictionary(action => action.Id, StringComparer.Ordinal);
        var records = new Dictionary<string, IReadOnlyList<DependencyEntry>?>(StringComparer.Ordinal);
        IReadOnlyList<DependencyEntry>? Record(NativeAction action)
        {
            if (!records.TryGetValue(action.Id, out IReadOnlyList<DependencyEntry>? entries))
            {
                entries = action.DependencyRecord is string path ? DependencyRecord.Read(path) : null;
                records[action.Id] = entries;
            }

            return entries;
        }

        var violations = new List<string>();
        void Report(string subject, string path, Owner owner, string viewer)
        {
            {
                violations.Add($"{subject}: includes {path}, which is in the {owner.Area.ToString().ToLowerInvariant()} area of module {owner.Name}; " +
                    $"it is not visible to {viewer}.");
            }
        }

        // 공개 헤더 검사 대상 모듈마다 검사 기록에서 본 소유 모듈이다(쓰지 않는 공개 의존 알림).
        var seenByCheck = new Dictionary<string, HashSet<string>>(StringComparer.OrdinalIgnoreCase);
        // 보임은 (모듈, 소스 묶음 폴더)마다 한 번 계산한다.
        var visibilities = new Dictionary<string, (Visibility Visible, string Viewer)>(StringComparer.OrdinalIgnoreCase);
        foreach (NativeAction action in actions)
        {
            if (action.Kind != "Compile" || action.DependencyRecord is null || !modules.TryGetValue(action.Module, out NativeModuleInput? module))
            {
                continue;
            }

            // 성공한 빌드의 컴파일 액션에는 기록이 있어야 한다(게시했거나 증분 검증했다). 없거나 깨졌으면 검사를 건너뛰지 않고 실패한다.
            if (Record(action) is not { } entries)
            {
                violations.Add($"{action.Id}: the dependency record {action.DependencyRecord} is missing or unreadable, so its includes cannot be checked.");
                continue;
            }

            // 공유 PCH 입력은 PCH 액션이 따로 본다.
            var excluded = new HashSet<string>(NativeToolchains.PathComparer);
            foreach (string dependency in action.Dependencies)
            {
                if (byId.TryGetValue(dependency, out NativeAction? pch) && pch.Kind == "Pch" && Record(pch) is { } pchEntries)
                {
                    excluded.UnionWith(pchEntries.Select(entry => entry.Path));
                }
            }

            string first = action.SourceMembers.Count == 0 ? "" : action.SourceMembers[0];
            SourceGroup? group = first.Length == 0 ? null : SourceGroups.Of(module, first, request.RootDirectory);
            string key = module.Name + "|" + group?.Directory;
            if (!visibilities.TryGetValue(key, out (Visibility Visible, string Viewer) cached))
            {
                cached = group is null
                    ? (Visibility.OfModule(module, modules, owners), "module " + module.Name)
                    : (Visibility.OfGroup(module, group, modules, owners), $"the {group.Target} source group of module {module.Name}");
                visibilities[key] = cached;
            }

            (Visibility visible, string viewer) = cached;
            HashSet<string>? seen = null;
            if (module.PublicHeaderCheckOf is string checkedModule)
            {
                if (!seenByCheck.TryGetValue(checkedModule, out seen))
                {
                    seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                    seenByCheck[checkedModule] = seen;
                }
            }

            foreach (DependencyEntry entry in entries)
            {
                if (excluded.Contains(entry.Path) || owners.Of(entry.Path) is not { } owner || visible.Grants(entry.Path))
                {
                    continue;
                }

                seen?.UnionWith(owner.Modules);
                seen?.UnionWith(owners.PublicOwnersOutsideFolders(entry.Path));
                if (!visible.Allows(owner))
                {
                    Report(Display(request, first.Length == 0 ? action.Id : first), Display(request, entry.Path), owner, viewer);
                }
            }
        }

        // PCH 액션: 모듈 PCH(PrivatePchHeader — 그 모듈의 컴파일 인자로 만든다)는 모듈의 보임, 공유 PCH는 제공 모듈의 공개 환경(제공 모듈과 그
        // 공개 closure의 공개 영역, 제공 모듈 자신은 세 영역)이다. 두 PCH는 Id 모양이 같아 헤더 철자로 가린다.
        foreach (NativeAction action in actions.Where(action => action.Kind == "Pch"))
        {
            if (!modules.TryGetValue(action.Module, out NativeModuleInput? provider))
            {
                continue;
            }

            if (Record(action) is not { } entries)
            {
                violations.Add($"{action.Id}: the dependency record {action.DependencyRecord} is missing or unreadable, so its includes cannot be checked.");
                continue;
            }

            bool modulePch = provider.PrivatePchHeader is string spelling && action.Source is string header &&
                header.Replace('\\', '/').EndsWith("/" + spelling.Replace('\\', '/'), StringComparison.OrdinalIgnoreCase);
            Visibility visible = modulePch ? Visibility.OfModule(provider, modules, owners) : Visibility.OfPublicEnvironment(provider, modules);
            foreach (DependencyEntry entry in entries)
            {
                if (owners.Of(entry.Path) is { } owner && !visible.Grants(entry.Path) && !visible.Allows(owner))
                {
                    Report(action.Id, Display(request, entry.Path), owner, (modulePch ? "the PCH of module " : "the shared PCH of module ") + provider.Name);
                }
            }
        }

        // 쓰지 않는 공개 의존: 공개 헤더 검사 기록에 공개 의존 D가 소유한 헤더가 하나도 없다. 모듈 폴더가 없는 의존(소유를 정할 수 없다)과
        // 서드파티는 빼고, 간접 include도 쓴 것으로 센다.
        var unused = new List<string>();
        foreach ((string checkedName, HashSet<string> seenModules) in seenByCheck.OrderBy(pair => pair.Key, StringComparer.Ordinal))
        {
            if (!modules.TryGetValue(checkedName, out NativeModuleInput? checkedModule))
            {
                continue;
            }

            string[] idle = [.. checkedModule.PublicDependencies.Where(name => modules.TryGetValue(name, out NativeModuleInput? dependency) &&
                    !dependency.ThirdParty && dependency.ModuleDirectory is not null && !seenModules.Contains(dependency.Name))
                .Order(StringComparer.Ordinal)];
            if (idle.Length != 0)
            {
                unused.Add(checkedModule.Name + ": " + string.Join(", ", idle));
            }
        }

        IReadOnlyList<string> notices = unused.Count == 0 ? [] : ["Unused public dependencies (no public header includes them): " + string.Join("; ", unused)];
        string report = Path.Combine(request.OutputDirectory, ReportFileName);
        if (violations.Count == 0)
        {
            File.Delete(report);
        }
        else
        {
            AtomicFile.WriteIfChanged(report, string.Join("\n", violations) + "\n");
        }

        return new([.. violations.Take(MaxReportedViolations)], violations.Count, notices, report);
    }

    // 루트 기준 경로(루트 밖이면 그대로)다.
    private static string Display(NativeBuildRequest request, string path) =>
        DirectoryPath.IsAtOrUnder(path, request.RootDirectory) ? Path.GetRelativePath(request.RootDirectory, path).Replace('\\', '/') : path;

    // 경로 → 소유 모듈·영역이다(한 빌드 안에서 캐시한다).
    private sealed class OwnerMap
    {
        private readonly string root;
        private readonly string output;
        private readonly string generated;
        private readonly string[] thirdPartyRoots;
        // 폴더·디렉터리는 끝 구분자를 붙여 정규화해 두고 접두 비교한다(경로마다 GetFullPath를 다시 하지 않는다).
        private readonly List<(string Directory, NativeModuleInput[] Modules)> folders;
        private readonly List<(string Directory, string Module)> outsidePublic = [];
        private readonly Dictionary<string, string[]> publicDirectories = new(StringComparer.OrdinalIgnoreCase);
        private readonly IReadOnlyDictionary<string, NativeModuleInput> modules;
        private readonly Dictionary<string, Owner?> cache = new(NativeToolchains.PathComparer);

        public OwnerMap(NativeBuildRequest request, IReadOnlyDictionary<string, NativeModuleInput> modules)
        {
            this.modules = modules;
            root = Path.TrimEndingDirectorySeparator(Path.GetFullPath(request.RootDirectory));
            output = Path.TrimEndingDirectorySeparator(Path.GetFullPath(request.OutputDirectory));
            generated = Path.Combine(output, "Generated");
            thirdPartyRoots = [.. ThirdPartyManifest.ThirdPartyRoots.Select(value => Path.GetFullPath(value, root))];
            folders = [.. modules.Values.Where(module => !module.ThirdParty && module.ModuleDirectory is not null)
                .GroupBy(module => Path.TrimEndingDirectorySeparator(Path.GetFullPath(module.ModuleDirectory!, root)), NativeToolchains.PathComparer)
                .Select(group => (group.Key, group.OrderBy(module => module.Name, StringComparer.Ordinal).ToArray()))
                .OrderByDescending(item => item.Key.Length)];
            foreach (NativeModuleInput module in modules.Values)
            {
                publicDirectories[module.Name] = [.. module.PublicIncludeDirectories.Select(value => Prefix(Path.GetFullPath(value, root)))];
            }

            folders = [.. folders.Select(folder => (Prefix(folder.Directory), folder.Modules))];
            thirdPartyRoots = [.. thirdPartyRoots.Select(Prefix)];
            // 모듈 폴더 밖에 있는 public include 디렉터리(쓰지 않는 공개 의존 판정에서 그 모듈이 쓴 것으로 센다).
            foreach (NativeModuleInput module in modules.Values.Where(module => !module.ThirdParty && module.ModuleDirectory is not null))
            {
                string own = Prefix(Path.GetFullPath(module.ModuleDirectory!, root));
                foreach (string directory in publicDirectories[module.Name].Where(directory => !directory.StartsWith(own, DirectoryPath.Comparison)))
                {
                    outsidePublic.Add((directory, module.Name));
                }
            }
        }

        // 끝 구분자를 붙인 정규 경로다(접두 비교가 경로 성분 경계를 지킨다).
        private static string Prefix(string directory) => Path.TrimEndingDirectorySeparator(directory) + Path.DirectorySeparatorChar;

        private static bool Under(string normalized, string prefix) =>
            normalized.StartsWith(prefix, DirectoryPath.Comparison) || normalized.Length + 1 == prefix.Length && prefix.StartsWith(normalized, DirectoryPath.Comparison);

        // 경로를 담는, 모듈 폴더 밖 public include 디렉터리의 모듈이다.
        public IEnumerable<string> PublicOwnersOutsideFolders(string path)
        {
            string normalized = Path.GetFullPath(path);
            return outsidePublic.Where(item => Under(normalized, item.Directory)).Select(item => item.Module);
        }

        // 모듈의 include 허가 폴더다: 그 모듈의 public·private include 디렉터리 가운데 다른 모듈 폴더 안(그 폴더 자신 포함)에 있는 것.
        // 모듈 폴더 밖의 넓은 디렉터리(시험 실행 파일의 Core/Source/Runtime 같은 루트 include)는 허가가 아니다.
        public IReadOnlyList<string> GrantsOf(NativeModuleInput module)
        {
            string? own = module.ModuleDirectory is null ? null : Path.GetFullPath(module.ModuleDirectory, root);
            return [.. module.PublicIncludeDirectories.Concat(module.PrivateIncludeDirectories).Select(value => Path.GetFullPath(value, root))
                .Where(directory => (own is null || !DirectoryPath.IsAtOrUnder(directory, own)) &&
                    folders.Any(folder => Under(Path.GetFullPath(directory), folder.Directory)))];
        }

        public Owner? Of(string path)
        {
            if (!cache.TryGetValue(path, out Owner? owner))
            {
                owner = Find(path);
                cache[path] = owner;
            }

            return owner;
        }

        private Owner? Find(string path)
        {
            // 출력 폴더는 메타 생성 폴더(<out>/Generated/<M>/Public·Private)만 소유가 있다.
            if (DirectoryPath.IsAtOrUnder(path, output))
            {
                if (!DirectoryPath.IsAtOrUnder(path, generated))
                {
                    return null;
                }

                string[] parts = Path.GetRelativePath(generated, path).Split(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
                if (parts.Length < 3 || !modules.TryGetValue(parts[0], out NativeModuleInput? owner))
                {
                    return null;
                }

                return parts[1] switch
                {
                    "Public" => new([owner.Name], Area.Public),
                    "Private" => new([owner.Name], Area.Private),
                    _ => null
                };
            }

            if (!DirectoryPath.IsAtOrUnder(path, root) || thirdPartyRoots.Any(value => Under(Path.GetFullPath(path), value)))
            {
                return null;
            }

            string normalized = Path.GetFullPath(path);
            foreach ((string directory, NativeModuleInput[] owners) in folders)
            {
                if (!Under(normalized, directory))
                {
                    continue;
                }

                string[] names = [.. owners.Select(module => module.Name)];
                if (owners.Any(module => publicDirectories[module.Name].Any(value => Under(normalized, value))))
                {
                    return new(names, Area.Public);
                }

                return new(names, Under(normalized, directory + "Tests" + Path.DirectorySeparatorChar) ? Area.Tests : Area.Private);
            }

            return null;
        }
    }

    // 한 소스(또는 PCH)에 보이는 모듈과 영역이다.
    private sealed class Visibility
    {
        // 세 영역이 모두 보이는 모듈, 공개·시험 영역이 보이는 모듈, 공개 영역만 보이는 모듈, include 허가 폴더다.
        private readonly HashSet<string> all = new(StringComparer.OrdinalIgnoreCase);
        private readonly List<string> grants = [];
        private readonly HashSet<string> publicAndTests = new(StringComparer.OrdinalIgnoreCase);
        private readonly HashSet<string> publicOnly = new(StringComparer.OrdinalIgnoreCase);

        public bool Allows(Owner owner) => owner.Modules.Any(module => all.Contains(module) || publicAndTests.Contains(module) && owner.Area != Area.Private ||
            publicOnly.Contains(module) && owner.Area == Area.Public);

        public bool Grants(string path) => grants.Any(directory => DirectoryPath.IsAtOrUnder(path, directory));

        public static Visibility OfModule(NativeModuleInput module, IReadOnlyDictionary<string, NativeModuleInput> modules, OwnerMap owners)
        {
            var visibility = new Visibility();
            visibility.all.Add(module.Name);
            visibility.grants.AddRange(owners.GrantsOf(module));
            visibility.publicOnly.UnionWith(ModuleVisibility.Of(module, modules).Select(value => value.Name));
            return visibility;
        }

        public static Visibility OfGroup(NativeModuleInput host, SourceGroup group, IReadOnlyDictionary<string, NativeModuleInput> modules,
            OwnerMap owners)
        {
            var visibility = new Visibility();
            visibility.all.Add(host.Name);
            visibility.all.Add(group.Target);
            visibility.all.UnionWith(group.InternalModules);
            if (modules.TryGetValue(group.Target, out NativeModuleInput? target))
            {
                visibility.publicAndTests.UnionWith(ModuleVisibility.Of(target, modules).Select(value => value.Name));
                visibility.grants.AddRange(owners.GrantsOf(target));
            }

            foreach (string extra in group.Modules)
            {
                visibility.publicAndTests.Add(extra);
                visibility.publicOnly.UnionWith(ModuleVisibility.PublicClosure(extra, modules));
            }

            return visibility;
        }

        public static Visibility OfPublicEnvironment(NativeModuleInput provider, IReadOnlyDictionary<string, NativeModuleInput> modules)
        {
            var visibility = new Visibility();
            visibility.all.Add(provider.Name);
            visibility.publicOnly.UnionWith(ModuleVisibility.PublicClosure(provider.Name, modules));
            return visibility;
        }
    }
}
