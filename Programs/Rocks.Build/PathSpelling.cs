namespace Rocks.Build;

/// <summary>
/// 경로를 디스크에 있는 철자로 바꾼다(Docs/Design/UnityBuildPch.ko.md 4.4절). 대소문자를 구분하지 않는 파일 시스템(Windows, macOS
/// 기본 설정)에서는 같은 디렉터리를 <c>c:\work\rocks</c>로도 <c>C:\Work\Rocks</c>로도 열 수 있는데, 빌드가 받은 철자를 그대로 인자와
/// action key에 넣으면 같은 트리가 실행한 철자에 따라 다시 컴파일된다. 빌드는 경로를 받는 곳에서 이것으로 철자를 하나로 맞춘다.
/// <list type="bullet">
/// <item>드라이브 문자는 대문자로 한다.</item>
/// <item>루트부터 성분마다, 그 이름의 항목이 부모 디렉터리에 있으면 파일 시스템이 알려 주는 이름으로 바꾼다. 없는 성분부터는 받은
/// 그대로 둔다(아직 만들지 않은 출력 디렉터리).</item>
/// <item>링크는 풀지 않는다. symlink·junction·<c>subst</c> 드라이브를 거친 경로도 받은 성분의 이름만 바로잡는다.</item>
/// <item>대소문자를 구분하는 파일 시스템에서는 정확히 같은 이름을 먼저 찾으므로 받은 철자가 바뀌지 않는다.</item>
/// <item>있는 경로의 8.3 짧은 이름(<c>PROGRA~1</c>)은 긴 이름이 된다(<see cref="Path.GetFullPath(string)"/>가 편다).</item>
/// </list>
/// 인스턴스 하나는 조회한 디렉터리의 이름 목록을 기억하므로 경로 여러 개를 바꿀 때는 같은 인스턴스를 쓴다. 기억은 만든 시점의
/// 디스크 상태다 — 요청 하나를 바꾸는 동안만 쓰고 버린다. 사용 예:
/// <code>
/// string root = PathSpelling.OnDisk(@"c:\work\ROCKS");   // C:\work\rocks
/// var spelling = new PathSpelling();
/// string[] sources = inputs.Select(spelling.Resolve).ToArray();
/// </code>
/// </summary>
public sealed class PathSpelling
{
    // 받은 절대 경로 → 디스크 철자다.
    private readonly Dictionary<string, string> resolved = new(StringComparer.Ordinal);

    // 디스크 철자의 디렉터리 → 그 안 항목의 이름들이다(읽지 못한 디렉터리는 null).
    private readonly Dictionary<string, HashSet<string>?> listings = new(StringComparer.Ordinal);

    private static readonly EnumerationOptions ListingOptions = new()
    {
        AttributesToSkip = 0,
        IgnoreInaccessible = true,
        RecurseSubdirectories = false,
        ReturnSpecialDirectories = false
    };

    /// <summary>경로 하나를 디스크 철자로 바꾼다(<see cref="Resolve"/>를 한 번 부르는 편의 함수다).</summary>
    /// <exception cref="ArgumentException">절대 경로가 아니다.</exception>
    public static string OnDisk(string path) => new PathSpelling().Resolve(path);

    /// <summary>
    /// 절대 경로를 디스크 철자로 바꾼다. <c>.</c>·<c>..</c>과 구분자는 <see cref="Path.GetFullPath(string)"/>처럼 정리하고, 끝 구분자는
    /// 받은 대로 둔다. 경로가 없거나 읽을 수 없으면 그 성분부터는 받은 그대로다(예외를 던지지 않는다).
    /// </summary>
    /// <exception cref="ArgumentException">절대 경로가 아니다.</exception>
    public string Resolve(string path)
    {
        if (!Path.IsPathFullyQualified(path))
        {
            throw new ArgumentException($"Path spelling needs an absolute path: {path}", nameof(path));
        }

        string full = Path.GetFullPath(path);
        string trimmed = Path.TrimEndingDirectorySeparator(full);
        return ResolveFull(trimmed) + full[trimmed.Length..];
    }

    /// <summary>
    /// 빌드 요청의 경로를 모두 디스크 철자로 바꾼다: 루트·출력·SDK 디렉터리, 모듈의 경로 목록, 툴체인의 도구 경로. 모듈의 상대 경로는
    /// 루트 기준으로 푼다(액션 그래프가 푸는 방식과 같다). 풀 수 없는 경로(잘못된 문자 등)는 받은 그대로 두어 뒤 단계가 지금처럼
    /// 알리게 한다.
    /// </summary>
    public static NativeBuildRequest OnDisk(NativeBuildRequest request)
    {
        var spelling = new PathSpelling();
        string root = spelling.Resolve(request.RootDirectory);
        string Fix(string path)
        {
            try
            {
                return spelling.Resolve(Path.GetFullPath(path, root));
            }
            catch (Exception error) when (error is ArgumentException or NotSupportedException or PathTooLongException)
            {
                return path;
            }
        }

        string[] FixAll(IReadOnlyList<string> paths) => paths.Select(Fix).ToArray();
        // 도구 경로는 절대 경로일 때만 바꾼다(이름만 준 도구는 PATH에서 찾는다).
        string Tool(string path) => Path.IsPathFullyQualified(path) ? Fix(path) : path;
        NativeToolchain toolchain = request.Toolchain;
        return request with
        {
            RootDirectory = root,
            OutputDirectory = spelling.Resolve(request.OutputDirectory),
            SdkDirectory = request.SdkDirectory is null ? null : spelling.Resolve(request.SdkDirectory),
            Toolchain = toolchain with
            {
                CompilerPath = Tool(toolchain.CompilerPath),
                ArchiverPath = Tool(toolchain.ArchiverPath),
                LinkerPath = toolchain.LinkerPath is null ? null : Tool(toolchain.LinkerPath)
            },
            Modules = request.Modules.Select(module => module with
            {
                Sources = FixAll(module.Sources),
                ReflectionSources = FixAll(module.ReflectionSources),
                PublicIncludeDirectories = FixAll(module.PublicIncludeDirectories),
                PrivateIncludeDirectories = FixAll(module.PrivateIncludeDirectories),
                PrebuiltLibraries = FixAll(module.PrebuiltLibraries),
                AdditionalInputs = FixAll(module.AdditionalInputs),
                UnityExclude = FixAll(module.UnityExclude),
                PrivatePchHeader = module.PrivatePchHeader is null ? null : Fix(module.PrivatePchHeader)
            }).ToArray()
        };
    }

    // full은 끝 구분자가 없는 절대 경로다(루트는 예외 — 루트의 끝 구분자는 떼어지지 않는다).
    private string ResolveFull(string full)
    {
        if (resolved.TryGetValue(full, out string? known))
        {
            return known;
        }

        string? parent = Path.GetDirectoryName(full);
        string result;
        if (parent is null)
        {
            // 루트다: 드라이브 문자만 대문자로 한다. UNC·장치 경로·POSIX 루트는 그대로다.
            result = full.Length >= 2 && full[1] == ':' && char.IsAsciiLetterLower(full[0]) ? char.ToUpperInvariant(full[0]) + full[1..] : full;
        }
        else
        {
            string parentOnDisk = ResolveFull(parent);
            result = Path.Combine(parentOnDisk, NameOnDisk(parentOnDisk, Path.GetFileName(full)));
        }

        resolved[full] = result;
        return result;
    }

    // 부모 디렉터리에 있는 그 이름의 실제 철자다. 정확히 같은 이름이 있으면 그것이고(대소문자를 구분하는 파일 시스템은 여기서 끝난다),
    // 없으면 대소문자만 다른 이름 가운데 하나다 — 단, 받은 이름으로 그 항목이 열릴 때만이다(구분하는 파일 시스템에서 대소문자만 다른
    // 다른 파일로 바꾸지 않는다). 어느 쪽도 아니면 받은 그대로다(없는 경로).
    private string NameOnDisk(string parent, string name)
    {
        HashSet<string>? names = Listing(parent);
        if (names is null || names.Contains(name))
        {
            return name;
        }

        string? match = names.Where(candidate => string.Equals(candidate, name, StringComparison.OrdinalIgnoreCase)).Order(StringComparer.Ordinal)
            .FirstOrDefault();
        if (match is null)
        {
            return name;
        }

        string given = Path.Combine(parent, name);
        return File.Exists(given) || Directory.Exists(given) ? match : name;
    }

    private HashSet<string>? Listing(string directory)
    {
        if (listings.TryGetValue(directory, out HashSet<string>? known))
        {
            return known;
        }

        HashSet<string>? names = null;
        try
        {
            if (Directory.Exists(directory))
            {
                names = Directory.EnumerateFileSystemEntries(directory, "*", ListingOptions).Select(entry => Path.GetFileName(entry))
                    .ToHashSet(StringComparer.Ordinal);
            }
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            names = null;
        }

        listings[directory] = names;
        return names;
    }
}
