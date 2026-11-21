using System.Security.Cryptography;
using System.Text;

namespace Rocks.Build;

/// <summary>
/// 타깃 하나 × 구성 하나의 IDE 계획이다. DefinitionDirectory는 그 타깃 규칙 타입을 선언한 <c>.Target.cs</c>의 디렉터리(절대 경로),
/// DefinitionName은 그 파일 이름에서 <c>.Target.cs</c>를 뗀 것, TargetType은 규칙의 타깃 종류 이름이다(Docs/Design/ProjectFiles.ko.md
/// 6.2절).
/// </summary>
public sealed record IdeTargetPlan(string Target, string DefinitionDirectory, string TargetType, string Configuration, IdePlan Plan,
    string? DefinitionName = null);

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

/// <summary>(파일, 프로젝트 구성)마다 고른 논리 명령과 그 명령이 나온 계획이다(16절 V1이 묶음 값과 대조한다).</summary>
public sealed record IdeChoice(IdeCompile Compile, IdePlan Plan);

/// <summary>
/// 파일별 IntelliSense 묶음이다. 같은 묶음의 파일은 모든 프로젝트 구성에서 값이 같다. Values는 프로젝트 구성 Key → 값이고, 그 구성에서
/// 아무 타깃도 그 파일을 컴파일하지 않으면 null이다.
/// </summary>
public sealed record IdeBundle(int Number, IReadOnlyDictionary<string, IntelliSenseValues?> Values);

/// <summary>
/// 프로젝트의 소스 하나다. Bundle은 묶음 번호이고, 어느 구성에서도 컴파일되지 않으면 null이다. Choices는 프로젝트 구성 Key → 고른 명령
/// (없으면 null)이다.
/// </summary>
public sealed record IdeSourceFile(string Path, int? Bundle, IReadOnlyDictionary<string, IdeChoice?> Choices);

/// <summary>
/// 생성할 프로젝트 하나다. Engine이면 엔진 프로젝트(엔진 루트 아래 정의된 타깃이 구성)다. DefaultBundle은 가장 많은 파일이 쓰는 묶음이다
/// (ItemDefinitionGroup 기본값으로 쓴다, 8.3절. 동률이면 번호가 작은 것).
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

    // Program 타깃 종류의 이름이다(정의 디렉터리 밖 파일을 받는 프로젝트를 고를 때 쓴다).
    private const string ProgramTargetType = "Program";

    // 헤더로 모으는 확장자다(README 헤더 이름 목록의 확장자에서 .c를 뺐다).
    private static readonly string[] HeaderExtensions = [".h", ".hh", ".hpp", ".hxx", ".inl", ".inc", ".ipp", ".tpp"];

    // 정의 헤더 사본 이름에 쓰는 내용 SHA-256 16진 앞 글자 수다.
    private const int DefinitionCopyHashLength = 16;

    /// <summary>
    /// 계획에서 모델을 만든다(6.2절). 계획 목록의 순서는 결과에 영향이 없다.
    /// <list type="bullet">
    /// <item>프로젝트: 정의 디렉터리가 <paramref name="engineRoots"/> 아래인 타깃은 엔진 프로젝트의 구성, 그 밖은 타깃마다 프로젝트다.
    /// 엔진 프로젝트에 같은 종류의 타깃이 둘이면 구성 이름이 겹치므로 오류다.</item>
    /// <item>파일 소유: 경로가 엔진 루트 아래면 엔진 프로젝트. 아니면 정의 디렉터리가 가장 가까운 조상인 타깃이고, 여럿이면 (1) 정의 파일
    /// 이름과 같은 이름의 타깃, (2) <paramref name="baseConfiguration"/>에서 그 파일을 컴파일하는 타깃, (3) ordinal 순이다. 조상이 없으면
    /// 기준 구성에서 그 파일을 컴파일하는 Program 타깃(ordinal 첫째), 그것도 없으면 엔진 프로젝트다.</item>
    /// <item>IntelliSense 원천: (파일, 프로젝트 구성)마다 그 타깃이 그 구성에서 컴파일하면 그 명령, 아니면 같은 구성에서 컴파일하는 ordinal
    /// 첫 타깃의 명령이다. 한 계획에서 여러 모듈이 컴파일하면 모듈 이름 ordinal 첫째다.</item>
    /// <item>묶음: key는 구성별 네 값의 정규 텍스트이고, 번호는 key의 ordinal 정렬 순서다.</item>
    /// </list>
    /// </summary>
    /// <exception cref="ArgumentException">미분류 인자(<see cref="IntelliSenseArguments"/>), 정의 헤더도 PCH 헤더도 아닌 강제 include,
    /// 엔진 프로젝트 구성 충돌.</exception>
    public static IntelliSenseModel Create(IReadOnlyList<IdeTargetPlan> plans, string root, string projectFilesDirectory,
        IReadOnlyList<string> engineRoots, string baseConfiguration)
    {
        root = Path.GetFullPath(root);
        string definitionsDirectory = Path.Combine(Path.GetFullPath(projectFilesDirectory), DefinitionsDirectoryName);
        var copies = new SortedDictionary<string, string>(StringComparer.Ordinal);
        var notices = new SortedSet<string>(StringComparer.Ordinal);
        bool UnderEngine(string path) => engineRoots.Any(engineRoot => DirectoryPathOf(path, engineRoot));

        // 구성 순서는 처음 나온 순서가 아니라 NativeConfiguration 순서다(계획 목록 순서와 무관하게).
        string[] configurations = [.. plans.Select(plan => plan.Configuration).Distinct(StringComparer.Ordinal)
            .OrderBy(ConfigurationOrder).ThenBy(name => name, StringComparer.Ordinal)];
        string[] targets = [.. plans.Select(plan => plan.Target).Distinct(StringComparer.Ordinal).Order(StringComparer.Ordinal)];
        var planOf = plans.ToDictionary(plan => plan.Target + "|" + plan.Configuration, StringComparer.Ordinal);
        IdeTargetPlan First(string target) => plans.Where(plan => plan.Target == target).OrderBy(plan => ConfigurationOrder(plan.Configuration)).First();
        var definitionOf = targets.ToDictionary(target => target, target => Path.GetFullPath(First(target).DefinitionDirectory), StringComparer.Ordinal);
        var typeOf = targets.ToDictionary(target => target, target => First(target).TargetType, StringComparer.Ordinal);
        var definitionNameOf = targets.ToDictionary(target => target, target => First(target).DefinitionName, StringComparer.Ordinal);
        string[] engineTargets = [.. targets.Where(target => UnderEngine(definitionOf[target]))];
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

        // 대상 소스: 어느 구성이든 어느 계획이 컴파일하는 손으로 쓴 소스. 대상 헤더: 계획에 든 모듈의 include·소스 디렉터리 아래 헤더.
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

        // 정의 디렉터리 밖 헤더는 같은 디렉터리 소스가 간 프로젝트를 따른다(Program 타깃 판정은 소스로만 한다).
        var sourceDirectoryOwner = new Dictionary<string, string>(NativeToolchains.PathComparer);
        string Owner(string path, bool source)
        {
            if (UnderEngine(path))
            {
                return EngineProjectName;
            }

            string[] candidates = [.. targets.Where(target => !engineTargets.Contains(target) && DirectoryPathOf(path, definitionOf[target]))];
            if (candidates.Length != 0)
            {
                int deepest = candidates.Max(target => definitionOf[target].Length);
                string[] closest = [.. candidates.Where(target => definitionOf[target].Length == deepest)];
                return closest.FirstOrDefault(target => definitionNameOf[target] == target)
                    ?? (source ? closest.FirstOrDefault(target => Compiles(target, baseConfiguration, path)) : null)
                    ?? closest[0];
            }

            // 정의 디렉터리 밖(.Target.cs가 없는 Programs 디렉터리 등): 기준 구성에서 컴파일하는 Program 타깃의 프로젝트, 없으면 엔진.
            string directory = Path.GetRelativePath(root, Path.GetDirectoryName(path)!);
            string? program = source
                ? targets.FirstOrDefault(target => !engineTargets.Contains(target) && typeOf[target] == ProgramTargetType &&
                    Compiles(target, baseConfiguration, path))
                : sourceDirectoryOwner.GetValueOrDefault(Path.GetDirectoryName(path)!);
            notices.Add($"{directory} is under no target definition directory; its {(source ? "sources" : "headers")} are listed in " +
                $"{program ?? EngineProjectName}.");
            return program ?? EngineProjectName;
        }

        var sourcesOf = new Dictionary<string, List<string>>(StringComparer.Ordinal);
        var headersOf = new Dictionary<string, List<string>>(StringComparer.Ordinal);
        foreach (string source in sources)
        {
            string owner = Owner(source, true);
            (sourcesOf.TryGetValue(owner, out var list) ? list : sourcesOf[owner] = []).Add(source);
            sourceDirectoryOwner.TryAdd(Path.GetDirectoryName(source)!, owner);
        }

        foreach (string header in headers)
        {
            string owner = Owner(header, false);
            (headersOf.TryGetValue(owner, out var list) ? list : headersOf[owner] = []).Add(header);
        }

        string CopyOf(string content)
        {
            string name = CopyName(content);
            copies[name] = content;
            return Path.Combine(definitionsDirectory, name);
        }

        IdeChoice? ChoiceOf(string target, string configuration, string source)
        {
            foreach (string candidate in new[] { target }.Concat(targets))
            {
                if (compilesOf.TryGetValue(candidate + "|" + configuration, out var bySource) && bySource.TryGetValue(source, out IdeCompile? compile))
                {
                    return new(compile, planOf[candidate + "|" + configuration].Plan);
                }
            }

            return null;
        }

        var projects = new List<IdeProject>();
        string[] projectNames = [.. (engineTargets.Length != 0 || sourcesOf.ContainsKey(EngineProjectName) || headersOf.ContainsKey(EngineProjectName)
            ? new[] { EngineProjectName } : []), .. targets.Where(target => !engineTargets.Contains(target))];
        foreach (string name in projectNames)
        {
            bool engine = name == EngineProjectName;
            IdeProjectConfiguration[] projectConfigurations = [.. (engine ? engineTargets : [name])
                .SelectMany(target => configurations.Where(configuration => planOf.ContainsKey(target + "|" + configuration))
                    .Select(configuration => new IdeProjectConfiguration(target, configuration)))];
            var rows = new List<(string Path, SortedDictionary<string, IdeChoice?> Choices, SortedDictionary<string, IntelliSenseValues?> Values, string? Key)>();
            foreach (string source in sourcesOf.GetValueOrDefault(name) ?? [])
            {
                var choices = new SortedDictionary<string, IdeChoice?>(StringComparer.Ordinal);
                var values = new SortedDictionary<string, IntelliSenseValues?>(StringComparer.Ordinal);
                foreach (IdeProjectConfiguration configuration in projectConfigurations)
                {
                    IdeChoice? choice = ChoiceOf(configuration.Target, configuration.Configuration, source);
                    choices[configuration.Key] = choice;
                    values[configuration.Key] = choice is null ? null : Values(choice.Compile, choice.Plan, (_, content) => CopyOf(content));
                }

                // 이 프로젝트의 어느 구성에서도 컴파일되지 않으면(구성이 없는 엔진 프로젝트 등) 묶음이 없다.
                string? key = values.Values.All(value => value is null)
                    ? null
                    : string.Join("\u001e", values.Select(pair => pair.Key + "\u001d" + (pair.Value?.Canonical() ?? "-")));
                rows.Add((source, choices, values, key));
            }

            string[] keys = [.. rows.Select(row => row.Key).OfType<string>().Distinct(StringComparer.Ordinal).Order(StringComparer.Ordinal)];
            var numberOf = keys.Select((key, index) => (key, index)).ToDictionary(pair => pair.key, pair => pair.index, StringComparer.Ordinal);
            var bundles = new IdeBundle[keys.Length];
            var counts = new int[keys.Length];
            var files = new List<IdeSourceFile>();
            foreach (var row in rows)
            {
                int? number = row.Key is null ? null : numberOf[row.Key];
                if (number is int value)
                {
                    bundles[value] ??= new(value, row.Values);
                    ++counts[value];
                }

                files.Add(new(row.Path, number, row.Choices));
            }

            int? defaultBundle = counts.Length == 0 ? null : Array.IndexOf(counts, counts.Max());
            projects.Add(new(name, engine, projectConfigurations, files, headersOf.GetValueOrDefault(name) ?? [], bundles, defaultBundle));
        }

        return new(projects, copies, [.. notices]);
    }

    /// <summary>
    /// 모델 검사(16절 V1): 모든 프로젝트·파일·구성에서 묶음에 접힌 값이 고른 명령과 왕복하는지(<see cref="RoundTrip"/>), 값이 null이면 정말
    /// 그 구성에서 어느 계획도 그 파일을 컴파일하지 않는지 본다. 불일치 문구 목록을 돌려준다(비면 통과).
    /// </summary>
    public static IReadOnlyList<string> Verify(IntelliSenseModel model, IReadOnlyList<IdeTargetPlan> plans, string projectFilesDirectory)
    {
        var failures = new List<string>();
        foreach (IdeProject project in model.Projects)
        {
            foreach (IdeSourceFile file in project.Sources)
            {
                foreach (IdeProjectConfiguration configuration in project.Configurations)
                {
                    IntelliSenseValues? value = file.Bundle is int bundle ? project.Bundles[bundle].Values[configuration.Key] : null;
                    IdeChoice? choice = file.Choices[configuration.Key];
                    if (value is null || choice is null)
                    {
                        bool compiled = plans.Any(plan => plan.Configuration == configuration.Configuration &&
                            plan.Plan.Compiles.Any(compile => NativeToolchains.PathComparer.Equals(Path.GetFullPath(compile.Source), file.Path)));
                        if (compiled || (value is null) != (choice is null))
                        {
                            failures.Add($"{project.Name} {configuration.Key}: {file.Path} has no IntelliSense values but is compiled in that configuration.");
                        }

                        continue;
                    }

                    if (RoundTrip(choice.Compile, choice.Plan, value, projectFilesDirectory) is string mismatch)
                    {
                        failures.Add($"{project.Name} {configuration.Key}: {mismatch}");
                    }
                }
            }
        }

        return failures;
    }

    /// <summary>정의 헤더 사본의 파일 이름이다(내용 SHA-256 16진 앞 16자 + <c>.h</c>, 8.4절). 같은 내용이면 같은 이름이다.</summary>
    public static string CopyName(string content) =>
        Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(content)))[..DefinitionCopyHashLength] + ".h";

    /// <summary>
    /// 논리 명령 하나를 IntelliSense 값으로 옮긴다. 정의 헤더(<see cref="IdePlan.GeneratedFiles"/>에 있는 강제 include)는
    /// <paramref name="copy"/>가 돌려준 사본 경로로 바꾸고, PCH를 쓰면 옵션 끝에 <c>/Yu"PCH 헤더"</c>를 더한다. <c>/TP</c>·<c>/TC</c>는
    /// 확장자와 다를 때만 옵션으로 남긴다.
    /// </summary>
    /// <exception cref="ArgumentException">미분류 인자, 정의 헤더도 PCH 헤더도 아닌 강제 include.</exception>
    public static IntelliSenseValues Values(IdeCompile compile, IdePlan plan, Func<string, string, string> copy)
    {
        var includes = new List<string>();
        var defines = new List<string>();
        var forced = new List<string>();
        var options = new List<string>();
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
                    else if (compile.PchHeader is not null && NativeToolchains.PathComparer.Equals(classified.Value, compile.PchHeader))
                    {
                        forced.Add(classified.Value);
                    }
                    else
                    {
                        throw new ArgumentException($"Forced include {classified.Value} of {compile.Source} is neither a definitions header nor the " +
                            "PCH header (Docs/Design/ProjectFiles.ko.md section 8.4).");
                    }

                    break;
                case IntelliSenseArgumentKind.Option:
                    options.Add(classified.Value);
                    break;
                case IntelliSenseArgumentKind.Language:
                    if ((classified.Value == "/TC") != IsCSource(compile.Source))
                    {
                        options.Add(classified.Value);
                    }

                    break;
            }
        }

        if (compile.PchHeader is not null)
        {
            options.Add(PchHint(compile.PchHeader));
        }

        return new(includes, defines, forced, options);
    }

    /// <summary>
    /// 왕복 검사(16절 V1): IntelliSense 값을 논리 명령으로 되돌려 원래 명령과 맞는지 본다. 분류표(<see cref="IntelliSenseArguments.Classify"/>)를
    /// 쓰지 않고 철자로 직접 대조한다: <c>/I</c>·<c>/D</c>·<c>/FI</c>는 원래 명령의 같은 접두 인자와 순서까지 같고(사본 경로는 명령이 쓴 정의
    /// 헤더로 되돌린다), 옵션은 원래 철자로 되돌린 것이 원래 명령에 같은 순서로 있으며, <c>/TP</c>·<c>/TC</c>는 원래 명령의 언어가 확장자와
    /// 다를 때만 있고, <c>/Yu</c>는 PCH 헤더가 있을 때만 그 헤더로 있다. 다르면 차이를 설명하는 문구, 같으면 null이다.
    /// </summary>
    public static string? RoundTrip(IdeCompile compile, IdePlan plan, IntelliSenseValues values, string projectFilesDirectory)
    {
        string definitions = Path.Combine(Path.GetFullPath(projectFilesDirectory), DefinitionsDirectoryName);
        string[] Prefixed(string prefix) => [.. compile.Arguments.Where(argument => argument.StartsWith(prefix, StringComparison.Ordinal))
            .Select(argument => argument[prefix.Length..])];
        string[] forcedOriginal = Prefixed("/FI");
        string Original(string forced)
        {
            if (!DirectoryPathOf(forced, definitions))
            {
                return forced;
            }

            // 사본 이름(내용 해시)이 같은 정의 헤더 중 명령이 실제로 쓴 경로를 고른다.
            string name = Path.GetFileName(forced);
            return forcedOriginal.FirstOrDefault(path => plan.GeneratedFiles.TryGetValue(Path.GetFullPath(path), out string? content) &&
                CopyName(content) == name) ?? forced;
        }

        string? Compare(string name, IEnumerable<string> actual, IEnumerable<string> wanted) =>
            actual.SequenceEqual(wanted, StringComparer.Ordinal) ? null : $"{name} differ for {compile.Source}";
        string? sequences = Compare("includes", values.Includes, Prefixed("/I"))
            ?? Compare("definitions", values.Defines, Prefixed("/D"))
            ?? Compare("forced includes", values.ForcedIncludes.Select(Original), forcedOriginal);
        if (sequences is not null)
        {
            return sequences;
        }

        string? language = compile.Arguments.FirstOrDefault(argument => argument is "/TP" or "/TC");
        bool languageDiffers = language is not null && (language == "/TC") != IsCSource(compile.Source);
        string[] languageOptions = [.. values.Options.Where(option => option is "/TP" or "/TC")];
        if (!languageOptions.SequenceEqual(languageDiffers ? [language!] : [], StringComparer.Ordinal))
        {
            return $"language option differs for {compile.Source}";
        }

        string[] hints = [.. values.Options.Where(option => option.StartsWith("/Yu", StringComparison.Ordinal))];
        if (!hints.SequenceEqual(compile.PchHeader is null ? [] : [PchHint(compile.PchHeader)], StringComparer.Ordinal))
        {
            return $"PCH hint differs for {compile.Source}";
        }

        int position = 0;
        foreach (string option in values.Options.Where(option => option is not ("/TP" or "/TC") && !option.StartsWith("/Yu", StringComparison.Ordinal)))
        {
            string original = IntelliSenseArguments.OriginalOption(option);
            int found = compile.Arguments.ToList().IndexOf(original, position);
            if (found < 0)
            {
                return $"option {option} ({original}) is not in the command (in order) for {compile.Source}";
            }

            position = found + 1;
        }

        return null;
    }

    // 구성 이름의 정렬 순서다(NativeConfiguration 순서, 모르는 이름은 뒤).
    private static int ConfigurationOrder(string configuration) =>
        Enum.TryParse(configuration, out NativeConfiguration value) ? (int)value : int.MaxValue;

    private static bool IsCSource(string source) => source.EndsWith(".c", StringComparison.OrdinalIgnoreCase);

    private static string PchHint(string header) => "/Yu\"" + header + "\"";

    // 디렉터리 하나 아래(같음 포함)인가. 철자 비교는 호스트 규칙(Windows는 대소문자 무시)이다.
    private static bool DirectoryPathOf(string path, string directory) => Fingerprints.IsUnder(Path.GetFullPath(path), Path.GetFullPath(directory));

    // 계획 출력 아래(생성 include 디렉터리 등)이거나, 루트 아래에서 점으로 시작하는 디렉터리 또는 최상위 out 아래인 경로다.
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

        string[] segments = Path.GetRelativePath(root, path).Split(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        return segments.Any(segment => segment.Length > 1 && segment[0] == '.' && segment != "..")
            || segments[0].Equals("out", StringComparison.OrdinalIgnoreCase);
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
