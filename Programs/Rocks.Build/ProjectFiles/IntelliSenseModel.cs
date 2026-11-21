using System.Security.Cryptography;
using System.Text;

namespace Rocks.Build;

/// <summary>
/// 타깃 하나 × 구성 하나의 IDE 계획이다. DefinitionDirectory는 그 타깃 규칙 타입을 선언한 <c>.Target.cs</c>의 디렉터리(절대 경로),
/// TargetType은 규칙의 타깃 종류 이름이다(Docs/Design/ProjectFiles.ko.md 6.2절).
/// </summary>
public sealed record IdeTargetPlan(string Target, string DefinitionDirectory, string TargetType, string Configuration, IdePlan Plan);

/// <summary>파일 하나가 한 프로젝트 구성에서 IntelliSense에 넘기는 값이다. 모두 전체 목록이다(차이분이 아니다, 8.3절).</summary>
public sealed record IntelliSenseValues(IReadOnlyList<string> Includes, IReadOnlyList<string> Defines, IReadOnlyList<string> ForcedIncludes,
    IReadOnlyList<string> Options)
{
    /// <summary>값을 비교·key로 쓰는 정규 텍스트다.</summary>
    public string Canonical() =>
        string.Join('\n', new[] { Includes, Defines, ForcedIncludes, Options }.Select(list => string.Join('\u001f', list)));
}

/// <summary>프로젝트 구성 하나(타깃 × 구성)다. Key는 <c>타깃|구성</c>이다.</summary>
public sealed record IdeProjectConfiguration(string Target, string Configuration)
{
    public string Key => Target + "|" + Configuration;
}

/// <summary>
/// 파일별 IntelliSense 묶음이다. 같은 묶음의 파일은 모든 프로젝트 구성에서 값이 같다. Values는 프로젝트 구성 Key → 값이고, 그 구성에서
/// 아무 타깃도 그 파일을 컴파일하지 않으면 null이다.
/// </summary>
public sealed record IdeBundle(int Number, IReadOnlyDictionary<string, IntelliSenseValues?> Values);

/// <summary>프로젝트의 소스 하나다. Bundle은 묶음 번호이고, 어느 구성에서도 컴파일되지 않으면 null이다.</summary>
public sealed record IdeSourceFile(string Path, int? Bundle);

/// <summary>
/// 생성할 프로젝트 하나다. Engine이면 엔진 프로젝트(엔진 루트 아래 정의된 타깃이 구성)다. DefaultBundle은 가장 많은 파일이 쓰는 묶음이다
/// (ItemDefinitionGroup 기본값으로 쓴다, 8.3절).
/// </summary>
public sealed record IdeProject(string Name, bool Engine, IReadOnlyList<IdeProjectConfiguration> Configurations,
    IReadOnlyList<IdeSourceFile> Sources, IReadOnlyList<string> Headers, IReadOnlyList<IdeBundle> Bundles, int? DefaultBundle);

/// <summary>
/// 계획들에서 만든 프로젝트 파일 모델이다(Docs/Design/ProjectFiles.ko.md 6.2·8절). DefinitionCopies는 정의 헤더 사본의 파일 이름 →
/// 내용(빌드가 쓰는 정의 헤더와 같은 byte)이다. Notices는 사람에게 알릴 문구다(영어).
/// </summary>
public sealed record IntelliSenseModel(IReadOnlyList<IdeProject> Projects, IReadOnlyDictionary<string, string> DefinitionCopies,
    IReadOnlyList<string> Notices)
{
    /// <summary>정의 헤더 사본을 두는 프로젝트 파일 디렉터리 아래 디렉터리 이름이다.</summary>
    public const string DefinitionsDirectoryName = "Definitions";

    /// <summary>엔진 프로젝트 이름이다(엔진 모드).</summary>
    public const string EngineProjectName = "Rocks";

    // 헤더로 모으는 확장자다(README 헤더 이름 목록의 확장자에서 .c를 뺐다).
    private static readonly string[] HeaderExtensions = [".h", ".hh", ".hpp", ".hxx", ".inl", ".inc", ".ipp", ".tpp"];

    // 정의 헤더 사본 이름에 쓰는 내용 SHA-256 16진 앞 글자 수다.
    private const int DefinitionCopyHashLength = 16;

    /// <summary>
    /// 계획에서 모델을 만든다.
    /// <list type="bullet">
    /// <item>프로젝트: 정의 디렉터리가 <paramref name="engineRoots"/> 아래인 타깃은 엔진 프로젝트의 구성, 그 밖은 타깃마다 프로젝트다.
    /// 엔진 프로젝트에 같은 종류의 타깃이 둘이면 구성 이름이 겹치므로 오류다.</item>
    /// <item>파일 소유: 경로가 엔진 루트 아래면 엔진 프로젝트, 아니면 정의 디렉터리가 가장 가까운 조상인 타깃(여럿이면
    /// <paramref name="baseConfiguration"/>에서 그 파일을 컴파일하는 타깃 중 ordinal 첫째, 없으면 ordinal 첫째), 어디에도 없으면 엔진
    /// 프로젝트다.</item>
    /// <item>IntelliSense 원천: (파일, 프로젝트 구성)마다 그 타깃이 그 구성에서 컴파일하면 그 명령, 아니면 같은 구성에서 컴파일하는 ordinal
    /// 첫 타깃의 명령이다. 한 계획에서 여러 모듈이 컴파일하면 모듈 이름 ordinal 첫째다.</item>
    /// </list>
    /// </summary>
    /// <exception cref="ArgumentException">미분류 인자(<see cref="IntelliSenseArguments"/>), 엔진 프로젝트 구성 충돌.</exception>
    public static IntelliSenseModel Create(IReadOnlyList<IdeTargetPlan> plans, string root, string projectFilesDirectory,
        IReadOnlyList<string> engineRoots, string baseConfiguration)
    {
        root = Path.GetFullPath(root);
        string definitionsDirectory = Path.Combine(Path.GetFullPath(projectFilesDirectory), DefinitionsDirectoryName);
        var notices = new List<string>();
        var copies = new SortedDictionary<string, string>(StringComparer.Ordinal);
        bool UnderEngine(string path) => engineRoots.Any(engineRoot => DirectoryPathOf(path, engineRoot));

        string[] targets = plans.Select(plan => plan.Target).Distinct(StringComparer.Ordinal).Order(StringComparer.Ordinal).ToArray();
        string[] configurations = plans.Select(plan => plan.Configuration).Distinct(StringComparer.Ordinal).ToArray();
        var planOf = plans.ToDictionary(plan => plan.Target + "|" + plan.Configuration, StringComparer.Ordinal);
        var definitionOf = plans.GroupBy(plan => plan.Target, StringComparer.Ordinal)
            .ToDictionary(group => group.Key, group => Path.GetFullPath(group.First().DefinitionDirectory), StringComparer.Ordinal);
        var typeOf = plans.GroupBy(plan => plan.Target, StringComparer.Ordinal)
            .ToDictionary(group => group.Key, group => group.First().TargetType, StringComparer.Ordinal);
        string[] engineTargets = targets.Where(target => UnderEngine(definitionOf[target])).ToArray();
        foreach (var group in engineTargets.GroupBy(target => typeOf[target], StringComparer.Ordinal).Where(group => group.Count() > 1))
        {
            throw new ArgumentException($"Targets {string.Join(", ", group)} of type {group.Key} are defined under the engine roots and would share " +
                "the configuration names of the engine project.");
        }

        // (타깃|구성) → 소스 → 그 계획에서 그 소스를 컴파일하는 명령(모듈 이름 ordinal 첫째).
        var compilesOf = new Dictionary<string, Dictionary<string, IdeCompile>>(StringComparer.Ordinal);
        foreach (IdeTargetPlan plan in plans)
        {
            var bySource = new Dictionary<string, IdeCompile>(NativeToolchains.PathComparer);
            foreach (IdeCompile compile in plan.Plan.Compiles.OrderBy(compile => compile.Module, StringComparer.Ordinal))
            {
                bySource.TryAdd(Path.GetFullPath(compile.Source), compile);
            }

            compilesOf[plan.Target + "|" + plan.Configuration] = bySource;
        }

        bool Compiles(string target, string configuration, string source) =>
            compilesOf.TryGetValue(target + "|" + configuration, out var bySource) && bySource.ContainsKey(source);

        // 대상 소스: 어느 계획이든 컴파일하는 손으로 쓴 소스. 대상 헤더: 계획에 든 모듈의 include·소스 디렉터리 아래 헤더.
        var sources = new SortedSet<string>(compilesOf.Values.SelectMany(bySource => bySource.Keys), StringComparer.Ordinal);
        var headers = new SortedSet<string>(StringComparer.Ordinal);
        var enumerated = new HashSet<string>(NativeToolchains.PathComparer);
        foreach (IdeTargetPlan plan in plans)
        {
            foreach (IdeModule module in plan.Plan.Modules)
            {
                foreach ((string directory, bool recursive) in module.IncludeDirectories.Select(directory => (directory, true))
                    .Concat(module.SourceDirectories.Select(directory => (directory, false))))
                {
                    string full = Path.GetFullPath(directory);
                    if (!enumerated.Add(full + (recursive ? "|r" : "|n")) || IsGeneratedOrHidden(full, root, plans))
                    {
                        continue;
                    }

                    if (NativeToolchains.PathComparer.Equals(Path.TrimEndingDirectorySeparator(full), Path.TrimEndingDirectorySeparator(root)))
                    {
                        notices.Add($"Module {module.Name} includes the repository root; its headers are listed through their own modules only.");
                        continue;
                    }

                    if (!Directory.Exists(full))
                    {
                        continue;
                    }

                    foreach (string header in EnumerateHeaders(full, recursive))
                    {
                        if (!IsGeneratedOrHidden(header, root, plans))
                        {
                            headers.Add(header);
                        }
                    }
                }
            }
        }

        string Owner(string path, bool source)
        {
            if (UnderEngine(path))
            {
                return EngineProjectName;
            }

            var candidates = targets.Where(target => !engineTargets.Contains(target) && DirectoryPathOf(path, definitionOf[target])).ToArray();
            if (candidates.Length == 0)
            {
                notices.Add($"{Path.GetRelativePath(root, path)} is under no target definition directory; listed in {EngineProjectName}.");
                return EngineProjectName;
            }

            int deepest = candidates.Max(target => definitionOf[target].Length);
            string[] closest = candidates.Where(target => definitionOf[target].Length == deepest).ToArray();
            return (source ? closest.FirstOrDefault(target => Compiles(target, baseConfiguration, path)) : null) ?? closest[0];
        }

        var sourcesOf = new Dictionary<string, List<string>>(StringComparer.Ordinal);
        var headersOf = new Dictionary<string, List<string>>(StringComparer.Ordinal);
        foreach (string source in sources)
        {
            string owner = Owner(source, true);
            (sourcesOf.TryGetValue(owner, out var list) ? list : sourcesOf[owner] = []).Add(source);
        }

        foreach (string header in headers)
        {
            string owner = Owner(header, false);
            (headersOf.TryGetValue(owner, out var list) ? list : headersOf[owner] = []).Add(header);
        }

        string CopyOf(string path, string content)
        {
            string name = CopyName(content);
            copies[name] = content;
            return Path.Combine(definitionsDirectory, name);
        }

        IntelliSenseValues? ValuesOf(string target, string configuration, string source)
        {
            IdeCompile? compile = null;
            IdeTargetPlan? owner = null;
            foreach (string candidate in new[] { target }.Concat(targets))
            {
                if (compilesOf.TryGetValue(candidate + "|" + configuration, out var bySource) && bySource.TryGetValue(source, out compile))
                {
                    owner = planOf[candidate + "|" + configuration];
                    break;
                }
            }

            return compile is null ? null : Values(compile, owner!.Plan, CopyOf);
        }

        var projects = new List<IdeProject>();
        string[] projectNames = [.. (engineTargets.Length != 0 || sourcesOf.ContainsKey(EngineProjectName) || headersOf.ContainsKey(EngineProjectName)
            ? new[] { EngineProjectName } : []), .. targets.Where(target => !engineTargets.Contains(target))];
        foreach (string name in projectNames)
        {
            bool engine = name == EngineProjectName;
            IdeProjectConfiguration[] projectConfigurations = (engine ? engineTargets : [name])
                .SelectMany(target => configurations.Where(configuration => planOf.ContainsKey(target + "|" + configuration))
                    .Select(configuration => new IdeProjectConfiguration(target, configuration)))
                .ToArray();
            var bundleKeys = new Dictionary<string, int>(StringComparer.Ordinal);
            var bundles = new List<IdeBundle>();
            var files = new List<IdeSourceFile>();
            var counts = new List<int>();
            foreach (string source in sourcesOf.GetValueOrDefault(name) ?? [])
            {
                var values = new SortedDictionary<string, IntelliSenseValues?>(StringComparer.Ordinal);
                foreach (IdeProjectConfiguration configuration in projectConfigurations)
                {
                    values[configuration.Key] = ValuesOf(configuration.Target, configuration.Configuration, source);
                }

                if (values.Count == 0 || values.Values.All(value => value is null))
                {
                    // 이 프로젝트의 어느 구성에서도 컴파일되지 않는다(다른 프로젝트 타깃만 컴파일하거나 구성이 없는 엔진 프로젝트).
                    files.Add(new(source, null));
                    continue;
                }

                string key = string.Join("\u001e", values.Select(pair => pair.Key + "\u001d" + (pair.Value?.Canonical() ?? "-")));
                if (!bundleKeys.TryGetValue(key, out int number))
                {
                    number = bundles.Count;
                    bundleKeys[key] = number;
                    bundles.Add(new(number, values));
                    counts.Add(0);
                }

                ++counts[number];
                files.Add(new(source, number));
            }

            int? defaultBundle = counts.Count == 0 ? null : counts.IndexOf(counts.Max());
            projects.Add(new(name, engine, projectConfigurations, files, headersOf.GetValueOrDefault(name) ?? [], bundles, defaultBundle));
        }

        return new(projects, copies, notices.Distinct(StringComparer.Ordinal).ToArray());
    }

    /// <summary>정의 헤더 사본의 파일 이름이다(내용 SHA-256 16진 앞 16자 + <c>.h</c>, 8.4절). 같은 내용이면 같은 이름이다.</summary>
    public static string CopyName(string content) =>
        Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(content)))[..DefinitionCopyHashLength] + ".h";

    /// <summary>
    /// 논리 명령 하나를 IntelliSense 값으로 옮긴다. 정의 헤더(<see cref="IdePlan.GeneratedFiles"/>에 있는 강제 include)는
    /// <paramref name="copy"/>가 돌려준 사본 경로로 바꾸고, 출력 밖 강제 include(PCH 헤더)가 있으면 옵션 끝에 <c>/Yu"헤더"</c>를 더한다.
    /// <c>/TP</c>·<c>/TC</c>는 확장자와 다를 때만 옵션으로 남긴다.
    /// </summary>
    public static IntelliSenseValues Values(IdeCompile compile, IdePlan plan, Func<string, string, string> copy)
    {
        var includes = new List<string>();
        var defines = new List<string>();
        var forced = new List<string>();
        var options = new List<string>();
        string? pchHeader = null;
        foreach (string argument in compile.Arguments)
        {
            IntelliSenseArgument classified = IntelliSenseArguments.Classify(argument, compile.Source);
            switch (classified.Kind)
            {
                case IntelliSenseArgumentKind.Include:
                    includes.Add(classified.Value);
                    break;
                case IntelliSenseArgumentKind.Define:
                    defines.Add(classified.Value);
                    break;
                case IntelliSenseArgumentKind.ForcedInclude:
                    if (plan.GeneratedFiles.TryGetValue(Path.GetFullPath(classified.Value), out string? content))
                    {
                        forced.Add(copy(classified.Value, content));
                    }
                    else
                    {
                        forced.Add(classified.Value);
                        pchHeader ??= classified.Value;
                    }

                    break;
                case IntelliSenseArgumentKind.Option:
                    options.Add(classified.Value);
                    break;
                case IntelliSenseArgumentKind.Language:
                    bool c = compile.Source.EndsWith(".c", StringComparison.OrdinalIgnoreCase);
                    if ((classified.Value == "/TC") != c)
                    {
                        options.Add(classified.Value);
                    }

                    break;
            }
        }

        if (pchHeader is not null)
        {
            options.Add("/Yu\"" + pchHeader + "\"");
        }

        return new(includes, defines, forced, options);
    }

    /// <summary>
    /// 왕복 검사(16절 V1): 모델의 값을 논리 명령의 해당 부분으로 되돌려 원래 명령과 순서까지 같은지 본다. 사본 경로는 원래 정의 헤더 경로로,
    /// 옮긴 옵션은 원래 철자로 되돌리고 <c>/Yu</c> 힌트는 뺀다. 다르면 차이를 설명하는 문구, 같으면 null이다.
    /// </summary>
    public static string? RoundTrip(IdeCompile compile, IdePlan plan, IntelliSenseValues values, string projectFilesDirectory)
    {
        string definitions = Path.Combine(Path.GetFullPath(projectFilesDirectory), DefinitionsDirectoryName);
        var expected = compile.Arguments.Select(argument => IntelliSenseArguments.Classify(argument, compile.Source))
            .Where(argument => argument.Kind is IntelliSenseArgumentKind.Include or IntelliSenseArgumentKind.Define
                or IntelliSenseArgumentKind.ForcedInclude or IntelliSenseArgumentKind.Option)
            .ToArray();
        string Original(string forced)
        {
            if (!DirectoryPathOf(forced, definitions))
            {
                return forced;
            }

            // 사본 이름(내용 해시)이 같은 정의 헤더 중 명령이 실제로 쓴 경로를 고른다.
            string name = Path.GetFileName(forced);
            return compile.Arguments.Where(argument => argument.StartsWith("/FI", StringComparison.Ordinal)).Select(argument => argument[3..])
                .FirstOrDefault(path => plan.GeneratedFiles.TryGetValue(Path.GetFullPath(path), out string? content) && CopyName(content) == name)
                ?? forced;
        }

        string[] Kind(IntelliSenseArgumentKind kind) => expected.Where(argument => argument.Kind == kind).Select(argument => argument.Value).ToArray();
        string? Compare(string name, IEnumerable<string> actual, IEnumerable<string> wanted) =>
            actual.SequenceEqual(wanted, StringComparer.Ordinal) ? null : $"{name} differ for {compile.Source}";
        return Compare("includes", values.Includes, Kind(IntelliSenseArgumentKind.Include))
            ?? Compare("definitions", values.Defines, Kind(IntelliSenseArgumentKind.Define))
            ?? Compare("forced includes", values.ForcedIncludes.Select(Original), Kind(IntelliSenseArgumentKind.ForcedInclude))
            ?? Compare("options", values.Options.Where(option => !option.StartsWith("/Yu", StringComparison.Ordinal) && option is not ("/TP" or "/TC"))
                .Select(IntelliSenseArguments.OriginalOption), Kind(IntelliSenseArgumentKind.Option).Select(IntelliSenseArguments.OriginalOption));
    }

    // 디렉터리 하나 아래(같음 포함)인가. 철자 비교는 호스트 규칙(Windows는 대소문자 무시)이다.
    private static bool DirectoryPathOf(string path, string directory) => Fingerprints.IsUnder(Path.GetFullPath(path), Path.GetFullPath(directory));

    // 계획 출력 아래(생성 include 디렉터리 등)이거나 점으로 시작하는 디렉터리 아래인 경로다.
    private static bool IsGeneratedOrHidden(string path, string root, IReadOnlyList<IdeTargetPlan> plans)
    {
        if (plans.Any(plan => DirectoryPathOf(path, plan.Plan.OutputDirectory)))
        {
            return true;
        }

        if (!DirectoryPathOf(path, root))
        {
            return false;
        }

        string relative = Path.GetRelativePath(root, path);
        return relative.Split(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar)
            .Any(segment => segment.Length > 1 && segment[0] == '.' && segment != "..")
            || relative.Split(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar)[0].Equals("out", StringComparison.OrdinalIgnoreCase);
    }

    private static IEnumerable<string> EnumerateHeaders(string directory, bool recursive)
    {
        var pending = new Stack<string>([directory]);
        while (pending.Count != 0)
        {
            string current = pending.Pop();
            foreach (string file in Directory.EnumerateFiles(current))
            {
                if (HeaderExtensions.Contains(Path.GetExtension(file), StringComparer.OrdinalIgnoreCase))
                {
                    yield return Path.GetFullPath(file);
                }
            }

            if (!recursive)
            {
                continue;
            }

            foreach (string child in Directory.EnumerateDirectories(current))
            {
                // 점 디렉터리와 중첩 작업 사본(.git이 있는 디렉터리)은 보지 않는다(include inventory와 같은 규칙).
                if (Path.GetFileName(child).StartsWith('.') || Path.Exists(Path.Combine(child, ".git")))
                {
                    continue;
                }

                pending.Push(child);
            }
        }
    }
}
