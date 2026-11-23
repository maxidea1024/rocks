using System.Globalization;
using System.Text;
using System.Text.Json;
using Rocks.Build.Rules;

namespace Rocks.Build.Cli;

/// <summary>모듈 리플렉션 헤더 하나다. Path는 정규 절대 경로, Include는 include 디렉터리 기준 철자다.</summary>
public sealed record MetadataHeader(string Path, string Include, bool Private);

/// <summary>
/// 메타데이터를 생성하는 모듈 하나의 배치다. 생성 헤더는 GeneratedPublic·GeneratedPrivate 아래 원본 철자를 따르고,
/// descriptor는 생성 include 디렉터리 밖(&lt;out&gt;/Metadata)에 둔다. GeneratedSource는 생성 소스 디렉터리
/// (&lt;out&gt;/Generated/&lt;모듈&gt;/Source — include 루트가 아니다)다. 그 아래에 리플렉션 헤더마다 타입별 생성 소스(본문 — TypeSource)가
/// Public·Private로 나뉘어 있고, 바로 아래에 모듈 생성 소스(등록 함수 정의 — ModuleSource)가 있다
/// (Docs/Design/ReflectionGeneratedSources.ko.md 6.1절).
/// </summary>
public sealed record MetadataModule(string Name, string Manifest, IReadOnlyList<MetadataHeader> Headers,
    string GeneratedPublic, string GeneratedPrivate, string GeneratedSource, string Descriptor)
{
    /// <summary>생성 소스 디렉터리의 이름이다(생성 루트 &lt;out&gt;/Generated/&lt;모듈&gt; 바로 아래, MetaCompiler 출력 루트의 Source).</summary>
    public const string SourceDirectoryName = "Source";

    /// <summary>모듈 생성 소스의 파일 이름이다(MetaCompiler의 CompilerOptions.ModuleSourceName과 같다).</summary>
    public static string ModuleSourceName(string module) => module + ".module.generated.cpp";

    /// <summary>모듈 생성 소스의 절대 경로다. 메타 생성이 게시한 뒤에 있고 이 모듈의 번역 단위로 컴파일된다.</summary>
    public string ModuleSource => Path.Combine(GeneratedSource, ModuleSourceName(Name));

    /// <summary>
    /// 타입별 생성 소스의 생성 소스 디렉터리 기준 상대 경로다(MetaCompiler의 CompilerOptions.SourcePath와 같은 규칙): 원본 철자의 ".h"를
    /// ".generated.cpp"로 바꾸고 public 헤더는 Public, private 헤더는 Private 아래에 둔다(두 쪽의 철자가 같을 수 있다).
    /// </summary>
    public static string TypeSourceRelative(MetadataHeader header) =>
        Path.Combine(header.Private ? "Private" : "Public", header.Include[..^HeaderExtension.Length] + ".generated.cpp");

    /// <summary>타입별 생성 소스(그 헤더의 타입 본문)의 절대 경로다. 메타 생성이 게시한 뒤에 있다.</summary>
    public string TypeSource(MetadataHeader header) => Path.GetFullPath(Path.Combine(GeneratedSource, TypeSourceRelative(header)));

    /// <summary>이 모듈의 리플렉션 생성 소스 전부다: 리플렉션 헤더 순서의 타입별 생성 소스, 그다음 모듈 생성 소스.</summary>
    public IEnumerable<string> ReflectionSources => Headers.Select(TypeSource).Append(ModuleSource);

    // 리플렉션 헤더 철자의 확장자다(MetadataLayout.ValidateSpelling이 이 확장자만 받는다).
    private const string HeaderExtension = ".h";
}

/// <summary>
/// 생성 include 디렉터리와 리플렉션 생성 소스를 더한 모듈 목록, 메타데이터 생성 모듈 목록, 그리고 Metadata가 없는 도달 모듈의
/// 은퇴 항목만 남은 명세(ID 예약 — 명세 집합 검증에 넣는다)다.
/// </summary>
public sealed record MetadataPlan(IReadOnlyList<NativeModuleInput> Modules, IReadOnlyList<MetadataModule> Generated,
    IReadOnlyList<string> ReservedManifests);

/// <summary>
/// 모듈 소유 메타데이터의 배치를 정하는 단일 지점이다(Docs/Design/ReflectionFoundations.ko.md 1.1–1.2절). RuleLoader
/// 뒤, ActionGraph 앞에서 한 번 부르고 Program·InstalledSdk·GameInputSnapshot이 모두 이 결과를 쓴다. 검증과 생성
/// 배치는 roots(native 빌드의 root 모듈 — 게임은 켠 플러그인 모듈 포함)에서 도달하는 모듈에만 적용한다. 규칙 로더가
/// 읽은 나머지 모듈은 그대로 둔다.
/// </summary>
public static class MetadataLayout
{
    private static readonly StringComparer PathComparer =
        OperatingSystem.IsWindows() ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal;

    // ActionGraph와 같이 모듈 이름은 대소문자를 무시해 비교한다.
    private static readonly StringComparer NameComparer = StringComparer.OrdinalIgnoreCase;

    /// <summary>
    /// allModules는 다른 모듈 폴더를 알아보는 데 쓰는 전체 규칙 모듈이다(1.10절 — 탐색은 다른 모듈 폴더 안을 훑지 않는다). 엔진 계획은
    /// modules를 타깃에서 닿는 모듈로 줄여 넘기므로(DescriptorModuleSet.Apply) 줄이기 전 목록을 따로 준다. 생략하면 modules다.
    /// </summary>
    public static MetadataPlan Apply(BuildContext context, IReadOnlyList<NativeModuleInput> modules, IEnumerable<string> roots,
        bool createDirectories = true, IReadOnlyList<NativeModuleInput>? allModules = null)
    {
        var byName = new Dictionary<string, NativeModuleInput>(NameComparer);
        foreach (NativeModuleInput module in modules)
        {
            if (!byName.TryAdd(module.Name, module))
            {
                throw new ArgumentException($"Duplicate module name '{module.Name}'.");
            }
        }

        HashSet<string> reachable = Reachable(roots, byName);
        // 모듈 폴더 전체를 훑을 때 그 안에 든 다른 모듈의 폴더는 그 모듈 몫이다(1.10절).
        string[] moduleDirectories = [.. (allModules ?? modules).Where(module => !string.IsNullOrWhiteSpace(module.ModuleDirectory))
            .Select(module => context.PathInRoot(module.ModuleDirectory!)).Distinct(PathComparer)];
        var owners = new Dictionary<string, string>(PathComparer);
        var generated = new List<MetadataModule>();
        var reserved = new List<string>();
        var updated = new List<NativeModuleInput>();
        foreach (NativeModuleInput module in modules)
        {
            if (!reachable.Contains(module.Name))
            {
                updated.Add(module);
                continue;
            }

            if (module.Metadata.Count != 0)
            {
                throw new ArgumentException($"Module '{module.Name}': module rules do not declare Metadata; the build discovers reflection headers " +
                    "by their ROCKS_CLASS/ROCKS_STRUCT/ROCKS_ENUM markup (Docs/Design/ReflectionFoundations.ko.md section 1.10). Remove the list.");
            }

            IReadOnlyList<ReflectionHeader> discovered = Discover(context, module, moduleDirectories);
            if (discovered.Count == 0)
            {
                if (CheckOrphanManifest(context, module) is string reservation)
                {
                    reserved.Add(reservation);
                }

                updated.Add(module);
                continue;
            }

            string directory = InsideRoot(context, module.ModuleDirectory!, $"Module '{module.Name}' ModuleDirectory");
            string manifest = Path.Combine(directory, module.Name + ".rocksids.json");
            ValidateManifest(context, manifest, module.Name);
            var headers = new List<MetadataHeader>();
            foreach (ReflectionHeader found in discovered)
            {
                ValidateSpelling(module.Name, found.Include);
                MetadataHeader header = ResolveHeader(context, module, found);
                if (!owners.TryAdd(header.Path, module.Name))
                {
                    // 같은 모듈이 같은 물리 헤더를 다른 철자로 다시 찾으면(겹친 include 디렉터리) 처음 것만 쓴다.
                    if (NameComparer.Equals(owners[header.Path], module.Name))
                    {
                        continue;
                    }

                    throw new ArgumentException($"Metadata '{found.Include}': duplicate input ownership; modules '{owners[header.Path]}' and '{module.Name}' resolve to '{header.Path}'.");
                }

                headers.Add(header);
            }

            string generatedRoot = Path.Combine(context.OutputDirectory, "Generated", module.Name);
            string generatedPublic = Path.Combine(generatedRoot, "Public");
            string generatedPrivate = Path.Combine(generatedRoot, "Private");
            // IDE 계획(Docs/Design/ProjectFiles.ko.md 8.1절)은 출력에 쓰지 않는다. 경로만 정한다.
            if (createDirectories)
            {
                Directory.CreateDirectory(generatedPublic);
                Directory.CreateDirectory(generatedPrivate);
            }

            string descriptor = Path.Combine(context.OutputDirectory, "Metadata", module.Name + ".rocksmeta.json");
            var layout = new MetadataModule(module.Name, manifest, headers, generatedPublic, generatedPrivate,
                Path.Combine(generatedRoot, MetadataModule.SourceDirectoryName), descriptor);
            generated.Add(layout);
            // 타입별 생성 소스(본문)와 모듈 생성 소스(등록 함수 정의)를 이 모듈의 리플렉션 생성 소스로 더한다. 경로는 모듈 이름과 헤더
            // 철자로 정해지므로 생성 전에 넣을 수 있고, 파일은 메타 생성이 게시한 뒤에 있다(액션 그래프가 그때 존재를 확인한다). 손으로 쓴
            // Sources와 섞지 않는다 — 이 목록이 진입점의 자기 타입 표지(ROCKS_MODULE_TYPES)를 정한다
            // (Docs/Design/ReflectionGeneratedSources.ko.md 6.3절, Docs/Design/ModuleLoader.ko.md 3.10절). 이 소스의 컴파일이 실패하면 빌드가
            // 컴파일러 출력 뒤에 붙일 안내문도 함께 넣는다(7절 — 본문 계약 위반은 빌드의 compile action이 잡는다).
            updated.Add(module with
            {
                PublicIncludeDirectories = headers.Any(header => !header.Private)
                    ? [.. module.PublicIncludeDirectories, generatedPublic]
                    : module.PublicIncludeDirectories,
                PrivateIncludeDirectories = headers.Any(header => header.Private)
                    ? [.. module.PrivateIncludeDirectories, generatedPrivate]
                    : module.PrivateIncludeDirectories,
                ReflectionSources = [.. module.ReflectionSources, .. layout.ReflectionSources],
                ReflectionSourceFailureNote = ReflectionSourceFailureNote(module.Name),
                Metadata = [.. headers.Select(header => header.Include)]
            });
        }

        CheckGeneratedSpellings(generated);
        AddModuleEntrySources(updated, generated);
        var plan = new MetadataPlan(updated, generated, reserved);
        CheckShadowing(plan, reachable);
        return plan;
    }

    /// <summary>build·test·sdk·game 명령의 옵션 이름이다: on이면 MetaCompiler가 게시 전에 생성 소스도 컴파일한다(--verify-sources).</summary>
    public const string VerifySourcesOption = "verify-metadata-sources";

    /// <summary>
    /// 리플렉션 생성 소스의 compile action이 실패했을 때 컴파일러 출력 뒤에 붙는 안내문이다(Docs/Design/ReflectionGeneratedSources.ko.md 7절).
    /// 빌드는 스키마 모델이 없어 원본 위치 안내를 만들지 못하므로 생성 소스의 위치 주석을 가리키고, 게시 전에 생성 소스까지 검증해
    /// 원본 위치를 알려 주는 재실행 방법을 적는다.
    /// </summary>
    public static string ReflectionSourceFailureNote(string module) =>
        $"note: RMC3000: generated metadata contract rejected by Clang (reflection generated source of module '{module}').\n" +
        "note: The failing code was generated by Rocks.MetaCompiler; the '// <header>:<line> <Type>::<member>' comment above the reported line names the reflected declaration.\n" +
        "hint: Fix constructor/destructor noexcept, CLASS/PROPERTY access and GENERATED_BODY friendship there. " +
        $"Rerun the same command with --{VerifySourcesOption} on to have Rocks.MetaCompiler compile the generated sources before publishing (--verify-sources) and report the original source locations.";

    /// <summary>진입점 생성 TU의 파일 이름 접미사다(<c>&lt;모듈&gt;.ModuleEntry.cpp</c>).</summary>
    public const string ModuleEntrySuffix = ".ModuleEntry.cpp";

    /// <summary>
    /// 손으로 쓴 소스와 규칙 생성 소스가 없는 메타데이터 모듈(CoreTypeBindings 등)에 진입점 TU를 더한다(Docs/Design/ModuleLoader.ko.md
    /// 3.10절). 리플렉션 생성 소스가 있으므로 진입점 모듈(<see cref="ModuleTables.EntryModules"/>)이고, 모듈 로더가 그 타입을 등록하려면
    /// 진입 기술이 필요하다. 진입점이 없어도 되는 모듈(서드파티, 모듈 표 제공 모듈에 닿지 않는 모듈)에는 더하지 않는다 — 빌드가
    /// ROCKS_MODULE_NAME을 정의하지 않아 진입점 매크로가 컴파일되지 않는다.
    /// </summary>
    private static void AddModuleEntrySources(List<NativeModuleInput> updated, IReadOnlyList<MetadataModule> generated)
    {
        var byName = new Dictionary<string, NativeModuleInput>(NameComparer);
        foreach (NativeModuleInput module in updated)
        {
            byName.TryAdd(module.Name, module);
        }

        HashSet<string> entries = ModuleTables.EntryModules(byName);
        var metadataModules = new HashSet<string>(generated.Select(module => module.Name), NameComparer);
        for (int index = 0; index < updated.Count; ++index)
        {
            NativeModuleInput module = updated[index];
            if (!metadataModules.Contains(module.Name) || !entries.Contains(module.Name) ||
                module.Sources.Count != 0 || module.GeneratedSources.Count != 0)
            {
                continue;
            }

            updated[index] = module with { GeneratedSources = [ModuleEntrySource(module.Name)] };
        }
    }

    /// <summary>진입점 생성 TU 하나다. 기본 모듈 구현으로 진입점을 정의하고, 빌드가 정의한 ROCKS_MODULE_TYPES=1이 자기 타입 함수를 싣는다.</summary>
    internal static NativeGeneratedSource ModuleEntrySource(string module)
    {
        var text = new StringBuilder();
        text.Append("// Generated by Rocks.Build. Do not edit.\n");
        text.Append(CultureInfo.InvariantCulture,
            $"// Module entry point of metadata module {module}, which has no hand-written sources (Docs/Design/ModuleLoader.ko.md section 3.10).\n");
        text.Append("#include <Core/Module/ModuleEntry.h>\n\n");
        text.Append(CultureInfo.InvariantCulture, $"ROCKS_IMPLEMENT_MODULE(rocks::DefaultModuleImplementation, {module})\n");
        return new NativeGeneratedSource(module + ModuleEntrySuffix, text.ToString());
    }

    /// <summary>roots에서 의존 관계(링크 의존과 DynamicModules)로 도달하는 모듈 이름이다. 없는 모듈은 이름과 함께 거부한다.</summary>
    public static HashSet<string> Reachable(IEnumerable<string> roots, IReadOnlyDictionary<string, NativeModuleInput> byName) =>
        // 실행 중 적재 모듈(DynamicModules)도 빌드하므로 메타데이터 대상이다(Docs/Design/ModuleLoader.ko.md 3.8절).
        LinkClosure.Reachable(roots, byName, (name, from) =>
            throw new ArgumentException($"Module '{from ?? "target"}' depends on unknown module '{name}'."));

    /// <summary>
    /// ActionGraph와 같은 include 검색 순서다: 자기 public, 자기 private, 컴파일 환경에 보이는 의존 모듈 public(후위 순서 —
    /// ModuleVisibility, Docs/Design/BuildTargets 3.5절).
    /// </summary>
    public static IReadOnlyList<string> IncludeOrder(NativeModuleInput module, IReadOnlyDictionary<string, NativeModuleInput> byName)
    {
        List<NativeModuleInput> dependencies = ModuleVisibility.Of(module, byName, (name, from) =>
            throw new ArgumentException($"Module '{from}' depends on unknown module '{name}'."));
        return module.PublicIncludeDirectories.Concat(module.PrivateIncludeDirectories)
            .Concat(dependencies.SelectMany(dependency => dependency.PublicIncludeDirectories))
            .Select(path => Path.GetFullPath(path)).Distinct(PathComparer).ToArray();
    }

    /// <summary>중간 디렉터리의 symlink도 해소한다. 별칭으로 같은 헤더를 두 번 등록하는 실수를 감지한다.</summary>
    public static string Canonical(string path, int depth = 0)
    {
        if (depth > 40)
        {
            throw new IOException("Too many symbolic links: " + path);
        }

        string full = Path.GetFullPath(path);
        string root = Path.GetPathRoot(full) ?? throw new IOException("Input path has no filesystem root: " + full);
        string current = root;
        foreach (string component in full[root.Length..].Split(
            [Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar], StringSplitOptions.RemoveEmptyEntries))
        {
            string candidate = Path.Combine(current, component);
            FileSystemInfo? entry = Directory.Exists(candidate) ? new DirectoryInfo(candidate)
                : File.Exists(candidate) ? new FileInfo(candidate) : null;
            FileSystemInfo? target = entry?.ResolveLinkTarget(returnFinalTarget: true);
            current = target is null ? candidate : Canonical(target.FullName, depth + 1);
        }

        return Path.GetFullPath(current);
    }

    private static string InsideRoot(BuildContext context, string relative, string what)
    {
        string full = context.PathInRoot(relative);
        string fromRoot = Path.GetRelativePath(context.RootDirectory, full);
        if (Path.IsPathRooted(fromRoot) || fromRoot.Split('/', '\\').Any(part => part == ".."))
        {
            throw new ArgumentException($"{what} must be inside the project root: {relative}");
        }

        return full;
    }

    private static void ValidateSpelling(string module, string include)
    {
        if (string.IsNullOrWhiteSpace(include) || Path.IsPathRooted(include) || include.IndexOfAny(['"', '\r', '\n', '<', '>', '\\', ':']) >= 0 ||
            include.Split('/').Any(part => part.Length == 0 || part is "." or "..") || !include.EndsWith(".h", StringComparison.Ordinal))
        {
            throw new ArgumentException($"Module '{module}' Metadata '{include}': include must be a relative forward-slash .h spelling.");
        }

        if (include.EndsWith(".module.h", StringComparison.OrdinalIgnoreCase) || include.EndsWith(".generated.h", StringComparison.OrdinalIgnoreCase))
        {
            throw new ArgumentException($"RMC2005: Module '{module}' Metadata '{include}': the .module.h and .generated.h suffixes are reserved for generated headers.");
        }
    }

    // 모듈의 리플렉션 헤더를 표지로 찾는다(Docs/Design/ReflectionFoundations.ko.md 1.10절). SDK 모듈(ImportedMetadata — 미리 생성된
    // descriptor)과 서드파티 모듈은 찾지 않는다. 표지 헤더가 빌드가 찾지 않는 자리에 있으면 그 타입이 조용히 빠지므로 거부한다:
    // - ModuleDirectory가 있으면 모듈 폴더 전체(시험·빌드 출력·서드파티 폴더와 그 안의 다른 모듈 폴더 제외)에서 include 디렉터리 밖의 표지
    //   헤더 — 생성 헤더가 원본을 포함할 철자가 없다(RMC2003 — 리플렉션 선언이 모듈 입력에 없다).
    // - ModuleDirectory가 없으면 프로젝트 루트 안 include 디렉터리의 표지 헤더 — 명세 위치를 정할 수 없다(RMC2100).
    private static IReadOnlyList<ReflectionHeader> Discover(BuildContext context, NativeModuleInput module, IReadOnlyList<string> moduleDirectories)
    {
        if (module.ImportedMetadata is not null || module.ThirdParty)
        {
            return [];
        }

        IReadOnlyList<string> includes = [.. module.PublicIncludeDirectories, .. module.PrivateIncludeDirectories];
        if (string.IsNullOrWhiteSpace(module.ModuleDirectory))
        {
            // 루트 안이지만 빌드 출력(생성 헤더)과 서드파티(외부 라이브러리 헤더 — 표지가 없고 크다)는 훑지 않는다.
            string root = Path.GetFullPath(context.RootDirectory);
            string output = Path.GetFullPath(context.OutputDirectory);
            foreach (string include in includes.Select(Path.GetFullPath).Where(include => IsUnder(include, root) && !IsUnder(include, output) &&
                !include.Split(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar).Contains("ThirdParty", StringComparer.OrdinalIgnoreCase)))
            {
                // 다른 모듈 폴더 안의 헤더는 그 모듈 몫이다(시험 실행 파일 모듈이 Core/Source/Runtime 같은 상위 폴더를 include로 둔다).
                if (ReflectionHeaderScan.MarkedOutside(include, [], moduleDirectories) is { Count: > 0 } stray)
                {
                    throw new ArgumentException($"RMC2100: Module '{module.Name}' has reflection header '{stray[0]}' but no ModuleDirectory " +
                        "(the identity manifest location); set ModuleDirectory in its rules (Docs/Design/ReflectionFoundations.ko.md section 1.10).");
                }
            }

            return [];
        }

        string directory = InsideRoot(context, module.ModuleDirectory, $"Module '{module.Name}' ModuleDirectory");
        IReadOnlyList<string> nested = [.. moduleDirectories.Where(other => !PathComparer.Equals(other, directory) && IsUnder(other, directory))];
        IReadOnlyList<string> outside = ReflectionHeaderScan.MarkedOutside(directory, includes, nested);
        if (outside.Count != 0)
        {
            throw new ArgumentException($"RMC2003: Module '{module.Name}': reflection header '{outside[0]}' is not under the module's include " +
                "directories, so the build does not scan it and its generated header could not include it; move it to Public/ " +
                "(Docs/Design/ReflectionFoundations.ko.md section 1.10).");
        }

        return ReflectionHeaderScan.Discover(directory, module.PublicIncludeDirectories, module.PrivateIncludeDirectories);
    }

    // path가 directory 자신이거나 그 아래인가다.
    private static bool IsUnder(string path, string directory)
    {
        string prefix = directory.EndsWith(Path.DirectorySeparatorChar) ? directory : directory + Path.DirectorySeparatorChar;
        return PathComparer.Equals(path, directory) || path.StartsWith(prefix, OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal);
    }

    // 찾은 헤더의 정규 경로(symlink 해석)가 프로젝트 루트 안인지 확인한다.
    private static MetadataHeader ResolveHeader(BuildContext context, NativeModuleInput module, ReflectionHeader found)
    {
        string canonical = Canonical(found.Path);
        string fromRoot = Path.GetRelativePath(Canonical(context.RootDirectory), canonical);
        if (Path.IsPathRooted(fromRoot) || fromRoot.Split('/', '\\').Any(part => part == ".."))
        {
            throw new ArgumentException($"Module '{module.Name}' Metadata '{found.Include}': source must resolve inside the project root.");
        }

        return new(canonical, found.Include, found.Private);
    }

    // 명세 경로 규칙: 루트 안, 제외 디렉터리 밖, symlink 아님, 출력 디렉터리 밖. history 검사는 접미사로 찾는다.
    private static void ValidateManifest(BuildContext context, string manifest, string module)
    {
        if (!File.Exists(manifest))
        {
            throw new ArgumentException($"RMC2100: Module '{module}' has Metadata but no identity manifest at '{manifest}'. Create it with the identity authoring tool (Rocks.MetaCompiler ids add).");
        }

        string relative = Path.GetRelativePath(context.RootDirectory, manifest);
        string[] parts = relative.Replace('\\', '/').Split('/');
        var excluded = new HashSet<string>([".git", "out", "bin", "obj", "__pycache__"], PathComparer);
        if (parts.Any(excluded.Contains) || (parts.Length >= 2 && PathComparer.Equals(parts[0], "Build") &&
            (PathComparer.Equals(parts[1], "Artifacts") || PathComparer.Equals(parts[1], "Cache"))))
        {
            throw new ArgumentException($"Identity manifest '{relative}' must not be inside a generated or excluded directory.");
        }

        string canonical = Canonical(manifest);
        if (!PathComparer.Equals(Path.GetFullPath(relative, Canonical(context.RootDirectory)), canonical))
        {
            throw new ArgumentException($"Identity manifest '{relative}' cannot use symbolic link paths.");
        }

        string fromOutput = Path.GetRelativePath(Path.GetFullPath(context.OutputDirectory), manifest);
        if (!Path.IsPathRooted(fromOutput) && !fromOutput.Split('/', '\\').Any(part => part == ".."))
        {
            throw new ArgumentException($"Identity manifest '{relative}' must be outside the output directory.");
        }
    }

    // Metadata가 없는 모듈의 명세는 활성 항목이 없을 때만 허용한다(은퇴 항목만 남은 명세는 ID 예약이다). 허용한
    // 명세 경로를 돌려준다 — 명세 집합 검증(ids verify-set)에 넣어 예약된 ID의 재사용을 막는다.
    private static string? CheckOrphanManifest(BuildContext context, NativeModuleInput module)
    {
        if (string.IsNullOrWhiteSpace(module.ModuleDirectory) || module.ImportedMetadata is not null)
        {
            return null;
        }

        string manifest = Path.Combine(context.PathInRoot(module.ModuleDirectory), module.Name + ".rocksids.json");
        if (!File.Exists(manifest))
        {
            return null;
        }

        using JsonDocument document = JsonDocument.Parse(File.ReadAllBytes(manifest));
        bool active = document.RootElement.TryGetProperty("entries", out JsonElement entries) && entries.EnumerateArray()
            .Any(entry => !entry.TryGetProperty("retired", out JsonElement retired) || retired.ValueKind != JsonValueKind.True);
        if (active)
        {
            throw new ArgumentException($"RMC2102: Module '{module.Name}' has no reflection headers (no ROCKS_CLASS/ROCKS_STRUCT/ROCKS_ENUM markup in its include directories) but its identity manifest '{manifest}' has active entries. Restore the reflected declarations or retire/move the entries (ids retire, ids move).");
        }

        return manifest;
    }

    // 생성 철자(원본 철자 + .generated.h)는 타깃 전체에서 대소문자 무시로 유일해야 한다.
    private static void CheckGeneratedSpellings(IReadOnlyList<MetadataModule> generated)
    {
        var seen = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        foreach (MetadataModule module in generated)
        {
            foreach (MetadataHeader header in module.Headers)
            {
                string spelling = (header.Private ? "private:" + module.Name + ":" : "") + header.Include;
                if (!seen.TryAdd(spelling, module.Name))
                {
                    throw new ArgumentException($"RMC2005: generated header spelling '{header.Include}' collides between modules '{seen[spelling]}' and '{module.Name}' (case-insensitive).");
                }
            }
        }
    }

    // 생성 헤더는 원본을 같은 철자로 포함한다. 그 철자를 쓰는 모든 모듈의 검색 순서에서 같은 파일이어야 한다.
    private static void CheckShadowing(MetadataPlan plan, HashSet<string> reachable)
    {
        var byName = plan.Modules.ToDictionary(module => module.Name, NameComparer);
        foreach (MetadataModule owner in plan.Generated)
        {
            foreach (NativeModuleInput consumer in plan.Modules.Where(module => reachable.Contains(module.Name)))
            {
                bool self = NameComparer.Equals(consumer.Name, owner.Name);
                if (!self && !DependsOn(consumer, owner.Name, byName))
                {
                    continue;
                }

                IReadOnlyList<string> order = IncludeOrder(consumer, byName);
                foreach (MetadataHeader header in owner.Headers.Where(header => self || !header.Private))
                {
                    string? found = order.Select(directory => Path.Combine(directory, header.Include)).FirstOrDefault(File.Exists);
                    if (found is null || !PathComparer.Equals(Canonical(found), header.Path))
                    {
                        throw new ArgumentException($"RMC1001: module '{consumer.Name}' resolves '{header.Include}' to '{found ?? "<not found>"}' instead of the reflected header '{header.Path}' of module '{owner.Name}'. Remove or rename the shadowing header.");
                    }

                    CheckGeneratedShadow(consumer, owner, order, header.Include[..^2] + ".generated.h", header.Private ? owner.GeneratedPrivate : owner.GeneratedPublic);
                }

                // 모듈 생성 헤더도 생성 루트보다 앞선 디렉터리의 같은 철자 파일에 가려지면 안 된다.
                if (owner.Headers.Any(header => !header.Private))
                {
                    CheckGeneratedShadow(consumer, owner, order, $"{owner.Name}/{owner.Name}.module.generated.h", owner.GeneratedPublic);
                }

                if (self && owner.Headers.Any(header => header.Private))
                {
                    CheckGeneratedShadow(consumer, owner, order, $"{owner.Name}/{owner.Name}.private.module.generated.h", owner.GeneratedPrivate);
                }
            }
        }
    }

    // 생성 철자는 검색 순서에서 그 생성 루트보다 앞선 디렉터리에 같은 철자 파일이 없어야 한다(작성자 디렉터리의 낡은
    // .generated.h가 생성 헤더를 가리지 않게).
    private static void CheckGeneratedShadow(NativeModuleInput consumer, MetadataModule owner, IReadOnlyList<string> order, string spelling, string root)
    {
        string generatedRoot = Path.GetFullPath(root);
        foreach (string directory in order)
        {
            if (PathComparer.Equals(directory, generatedRoot))
            {
                return;
            }

            string candidate = Path.Combine(directory, spelling);
            if (File.Exists(candidate))
            {
                throw new ArgumentException($"RMC1001: module '{consumer.Name}' resolves generated header '{spelling}' of module '{owner.Name}' to '{candidate}' before the generated root. Remove the stale or shadowing file.");
            }
        }
    }

    // module의 컴파일 환경에 target 모듈이 보이는지다(생성 헤더 해석 — "도달"이 아니라 "보임", Docs/Design/BuildTargets 3.5절).
    private static bool DependsOn(NativeModuleInput module, string target, IReadOnlyDictionary<string, NativeModuleInput> byName) =>
        ModuleVisibility.Sees(module, target, byName);
}
