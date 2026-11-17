using System.Runtime.InteropServices;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace Rocks.Tooling.Common;

/// <summary>
/// 미리 빌드한 바이너리의 출처다(manifest 필드로 정해진다).
/// <list type="bullet">
/// <item>Build: Rocks 오프라인 도구 빌드(<c>&lt;Lib&gt;/Build/</c> 스크립트)가 고정 태그에서 만들어 GitHub Release에 게시한 archive다
/// (manifest <c>build</c>).</item>
/// <item>Sdk: 버전을 고정한 설치 SDK의 파일을 SHA-256 검사 뒤 저장소로 복사한다(manifest <c>sdk</c>, 예: Windows SDK의 FXC).
/// 설치기라 다시 게시하지 않는다.</item>
/// <item>System: 저장소로 받지 않는 시스템 도구다(manifest <c>status: "system"</c>, 예: Xcode Metal 도구). 최소 버전만 고정하고 파일
/// SHA는 고정하지 않는다(도구가 갱신될 때마다 바뀐다). 실제 지문은 워커가 잰다.</item>
/// </list>
/// </summary>
public enum ThirdPartyBinaryOrigin { Build, Sdk, System }

/// <summary>
/// binary 파일의 역할이다(manifest <c>role</c>). Runtime은 워커가 여는 공유 라이브러리, Redist는 Runtime 실행 파일 옆에 복사만
/// 하는 재배포 파일, Symbols는 PDB·dSYM, Tool은 실행 파일, Notice는 라이선스 고지다. 링크하지 않으므로 import library는 없다.
/// </summary>
public enum ThirdPartyFileRole { Runtime, Redist, Symbols, Tool, Notice }

/// <summary>게시한 archive 하나다. <see cref="Url"/>의 마지막 조각이 asset 이름이며 미러 디렉터리에서도 같은 이름으로 찾는다.</summary>
public sealed record ThirdPartyArchive(string Url, string Sha256, long Size)
{
    /// <summary>asset 파일 이름(URL의 마지막 조각)이다. 예: <c>SpirvCross-1.4.363.0-r1-windows-x64.tar.gz</c>.</summary>
    public string FileName => Url[(Url.LastIndexOf('/') + 1)..];
}

/// <summary>
/// 도구 빌드가 받는 원본 하나다. <see cref="Path"/>는 원본 트리 안 위치(주 원본은 null), <see cref="Commit"/>은 고정한 git 커밋,
/// <see cref="Url"/>·<see cref="Sha256"/>은 그 커밋의 원본 archive와 SHA-256이다. 스크립트가 받아 SHA를 검사한다.
/// </summary>
public sealed record ThirdPartyBuildInput(string? Path, string Repository, string Commit, string Url, string Sha256);

/// <summary>
/// Rocks 오프라인 도구 빌드의 고정 입력이다(manifest <c>build</c>). <see cref="Tag"/>는 업스트림 원본 태그, <see cref="Revision"/>은
/// 같은 태그를 다시 빌드할 때(스크립트·patch 변경) 올리는 번호로 Release 태그와 asset 이름에 들어간다. <see cref="Scripts"/>·
/// <see cref="Patches"/>는 라이브러리 폴더 기준 <c>Build/</c> 아래 경로다.
/// </summary>
public sealed record ThirdPartyToolBuild(string Tag, int Revision, ThirdPartyBuildInput Source, IReadOnlyList<ThirdPartyBuildInput> Dependencies,
    IReadOnlyList<string> Scripts, IReadOnlyList<string> Patches)
{
    /// <summary>
    /// 게시 Release 태그다(<c>third-party-&lt;이름&gt;-&lt;버전&gt;-r&lt;revision&gt;</c>). 도구 빌드 workflow와 manifest의 archive URL이
    /// 같은 규칙을 쓴다.
    /// </summary>
    public static string ReleaseTag(string name, string version, int revision) => $"third-party-{name}-{version}-r{revision}";

    /// <summary>호스트별 asset 이름이다(<c>&lt;이름&gt;-&lt;버전&gt;-r&lt;revision&gt;-&lt;호스트&gt;.tar.gz</c>).</summary>
    public static string AssetName(string name, string version, int revision, string host) => $"{name}-{version}-r{revision}-{host}.tar.gz";
}

/// <summary>설치 SDK 출처다(manifest <c>sdk</c>). <see cref="Name"/>은 현재 <c>"Windows SDK"</c>만 받는다.</summary>
public sealed record ThirdPartySdk(string Name, string Version);

/// <summary>
/// 호스트별 파일 하나다. <see cref="Path"/>는 저장소 디렉터리(archive를 푼 위치) 기준 경로다. system 출처는 <see cref="Sha256"/>·
/// <see cref="Size"/>가 null이고 <see cref="Path"/>는 실행 파일 이름이다. sdk 출처는 <see cref="SdkPath"/>가 SDK 루트 기준 원본 경로다.
/// </summary>
public sealed record ThirdPartyHostFile(string Path, ThirdPartyFileRole Role, string? Sha256, long? Size, string? SdkPath);

/// <summary>호스트 하나의 파일 목록이다. build 출처만 <see cref="Archive"/>가 있다.</summary>
public sealed record ThirdPartyHost(string Key, ThirdPartyArchive? Archive, IReadOnlyList<ThirdPartyHostFile> Files)
{
    /// <summary>
    /// 사용자 저장소의 디렉터리 이름(내용 주소)이다. build는 archive SHA-256, sdk는 유일한 파일의 SHA-256이다. system은 저장소에
    /// 두지 않으므로 null이다.
    /// </summary>
    public string? StoreKey => Archive?.Sha256 ?? (Files.Count == 1 ? Files[0].Sha256 : null);
}

/// <summary>binary 종류 manifest의 출처와 호스트별 파일 목록이다(<see cref="ThirdPartyManifest.Binary"/>).</summary>
public sealed record ThirdPartyBinary(string Name, ThirdPartyBinaryOrigin Origin, ThirdPartyToolBuild? Build, ThirdPartySdk? Sdk,
    string? MinimumVersion, IReadOnlyDictionary<string, ThirdPartyHost> Hosts)
{
    /// <summary>
    /// 호스트 키(<see cref="ThirdPartyManifest.HostKeys"/>)의 파일 목록을 돌려준다. 사용 예:
    /// <c>manifest.Binary!.Host(ThirdPartyManifest.CurrentHostKey())</c>.
    /// </summary>
    /// <exception cref="FormatException">manifest가 그 호스트를 적지 않았다(호스트 누락). 다른 호스트 산출물로 대신하지 않는다.</exception>
    public ThirdPartyHost Host(string key) => Hosts.TryGetValue(key, out ThirdPartyHost? host)
        ? host
        : throw new FormatException($"{Name}: binary manifest has no files for host '{key}' (listed: {string.Join(", ", Hosts.Keys)})");
}

public sealed partial record ThirdPartyManifest
{
    /// <summary>
    /// binary 호스트 키다(<c>&lt;플랫폼&gt;-&lt;아키텍처&gt;</c>, 소문자). 지원 개발 호스트는 Windows·macOS뿐이고, x64·ARM64 산출물을
    /// 섞지 않도록 아키텍처를 키에 넣는다.
    /// </summary>
    public static readonly IReadOnlyList<string> HostKeys = ["windows-x64", "windows-arm64", "macos-x64", "macos-arm64"];

    /// <summary>
    /// 현재 OS·OS 아키텍처의 호스트 키다(에뮬레이션 중인 프로세스 아키텍처가 아니라 OS 아키텍처를 본다).
    /// </summary>
    /// <exception cref="PlatformNotSupportedException">지원 개발 호스트가 아니다.</exception>
    public static string CurrentHostKey()
    {
        string platform = OperatingSystem.IsWindows() ? "windows"
            : OperatingSystem.IsMacOS() ? "macos"
            : throw new PlatformNotSupportedException("third-party binaries are provided only for Windows and macOS development hosts");
        string architecture = RuntimeInformation.OSArchitecture switch
        {
            Architecture.X64 => "x64",
            Architecture.Arm64 => "arm64",
            Architecture other => throw new PlatformNotSupportedException($"unsupported host architecture {other}")
        };
        return $"{platform}-{architecture}";
    }

    private static readonly Regex CommitText = new("^[0-9a-f]{40}$", RegexOptions.CultureInvariant, TimeSpan.FromSeconds(1));
    private static readonly Regex VersionText = new("^[0-9A-Za-z][0-9A-Za-z.+-]*$", RegexOptions.CultureInvariant, TimeSpan.FromSeconds(1));

    // binary의 커밋한 공개 헤더 묶음이다. build 출처에서는 모두 있어야 하고 sdk·system에서는 없어야 한다.
    private static readonly string[] BinaryHeaderFields =
        ["upstreamDirectory", "licenses", "includeDirectories", "consumerDefines", "adapterHeaders", "files", "pragmaDirectives"];

    private static readonly string[] BinaryFields = ["schema", "kind", "name", "hosts", "notes"];

    private static readonly string[] OptionalBinaryFields = ["status", "version", "minimumVersion", "build", "sdk", .. BinaryHeaderFields];

    private static readonly string[] SupportedSdks = ["Windows SDK"];

    /// <summary>build 출처 스크립트·patch를 두는 라이브러리 폴더 바로 아래 디렉터리 이름이다.</summary>
    public const string BuildDirectory = "Build";

    /// <summary>
    /// build.scripts가 받는 확장자다. Windows 스크립트(.ps1·.cmd), macOS 스크립트(.sh), 스크립트가 작업 디렉터리에 복사해 쓰는 CMake
    /// 래퍼(.cmake, 업스트림 원본을 고치지 않고 빌드 대상을 더할 때)다.
    /// </summary>
    public static readonly IReadOnlyList<string> BuildScriptExtensions = [".ps1", ".cmd", ".sh", ".cmake"];

    private static ThirdPartyManifest ParseBinary(JsonElement root)
    {
        Dictionary<string, JsonElement> fields = Object(root, "manifest", BinaryFields, optionalFields: OptionalBinaryFields);
        if (Integer(fields["schema"], "schema") != 1)
        {
            throw new FormatException("schema: unsupported version (expected 1)");
        }

        string name = Text(fields["name"], "name");
        if (!ModuleName.IsMatch(name))
        {
            throw new FormatException($"name: '{name}' must be an identifier (it names release assets)");
        }

        bool system = false;
        if (fields.TryGetValue("status", out JsonElement statusElement))
        {
            if (Text(statusElement, "status") != "system")
            {
                throw new FormatException("status: only \"system\" is allowed (omit the field for a published or SDK binary)");
            }

            system = true;
        }

        ThirdPartyBinaryOrigin origin = system ? ThirdPartyBinaryOrigin.System
            : (fields.ContainsKey("build"), fields.ContainsKey("sdk")) switch
            {
                (true, false) => ThirdPartyBinaryOrigin.Build,
                (false, true) => ThirdPartyBinaryOrigin.Sdk,
                _ => throw new FormatException("binary: declare exactly one of build, sdk or status \"system\"")
            };
        if (system && (fields.ContainsKey("build") || fields.ContainsKey("sdk")))
        {
            throw new FormatException("binary: a system tool declares neither build nor sdk");
        }

        // 버전: system은 최소 버전만, 나머지는 고정 버전만 적는다.
        string version;
        string? minimumVersion = null;
        if (system)
        {
            if (fields.ContainsKey("version"))
            {
                throw new FormatException("version: a system tool pins only minimumVersion");
            }

            minimumVersion = VersionField(fields, "minimumVersion");
            version = minimumVersion;
        }
        else
        {
            if (fields.ContainsKey("minimumVersion"))
            {
                throw new FormatException("minimumVersion: only a system tool declares a minimum version");
            }

            version = VersionField(fields, "version");
        }

        bool hasHeaders = origin == ThirdPartyBinaryOrigin.Build;
        foreach (string field in BinaryHeaderFields)
        {
            if (fields.ContainsKey(field) != hasHeaders)
            {
                throw new FormatException(hasHeaders
                    ? $"{field}: a built binary commits its public headers and license (missing field)"
                    : $"{field}: an SDK or system binary has no committed files");
            }
        }

        string upstream = "";
        IReadOnlyList<ThirdPartyLicense> licenses = [];
        IReadOnlyList<string> includeDirectories = [];
        IReadOnlyList<string> consumerDefines = [];
        IReadOnlyList<string> adapterHeaders = [];
        IReadOnlyDictionary<string, string> files = new SortedDictionary<string, string>(StringComparer.Ordinal);
        IReadOnlyList<ThirdPartyPragma> pragmas = [];
        if (hasHeaders)
        {
            upstream = RelativePath(Text(fields["upstreamDirectory"], "upstreamDirectory"), "upstreamDirectory");
            if (upstream.Contains('/') || upstream == BuildDirectory)
            {
                throw new FormatException($"upstreamDirectory: must be a single directory name other than '{BuildDirectory}'");
            }

            licenses = LicenseList(fields["licenses"]);
            includeDirectories = Paths(fields["includeDirectories"], "includeDirectories", nonEmpty: true);
            consumerDefines = DefineList(fields["consumerDefines"], "consumerDefines");
            adapterHeaders = Paths(fields["adapterHeaders"], "adapterHeaders", nonEmpty: true);
            files = FileList(fields["files"]);
            pragmas = PragmaList(fields["pragmaDirectives"]);
        }

        ThirdPartyToolBuild? build = origin == ThirdPartyBinaryOrigin.Build ? ToolBuild(fields["build"]) : null;
        ThirdPartySdk? sdk = null;
        if (origin == ThirdPartyBinaryOrigin.Sdk)
        {
            Dictionary<string, JsonElement> sdkFields = Object(fields["sdk"], "sdk", ["name", "version"]);
            string sdkName = Text(sdkFields["name"], "sdk.name");
            if (!SupportedSdks.Contains(sdkName, StringComparer.Ordinal))
            {
                throw new FormatException($"sdk.name: '{sdkName}' is not supported (supported: {string.Join(", ", SupportedSdks)})");
            }

            sdk = new(sdkName, VersionField(sdkFields, "version", "sdk.version"));
        }

        Dictionary<string, JsonElement> hostFields = Object(fields["hosts"], "hosts", HostKeys, optional: true);
        if (hostFields.Count == 0)
        {
            throw new FormatException("hosts: must list at least one host");
        }

        var hosts = new SortedDictionary<string, ThirdPartyHost>(StringComparer.Ordinal);
        foreach ((string key, JsonElement value) in hostFields)
        {
            hosts.Add(key, Host(key, value, origin, name, version, build));
        }

        return new(name, version, upstream, null, licenses, [], includeDirectories, [],
            new Dictionary<string, IReadOnlyList<string>>(StringComparer.Ordinal), consumerDefines, adapterHeaders, files, pragmas,
            Strings(fields["notes"], "notes"))
        {
            Kind = ThirdPartyKind.Binary,
            Binary = new(name, origin, build, sdk, minimumVersion, hosts)
        };
    }

    private static string VersionField(Dictionary<string, JsonElement> fields, string field, string? label = null)
    {
        label ??= field;
        if (!fields.TryGetValue(field, out JsonElement element))
        {
            throw new FormatException($"manifest: missing field '{label}'");
        }

        string value = Text(element, label);
        if (!VersionText.IsMatch(value))
        {
            throw new FormatException($"{label}: '{value}' must be a version of [0-9A-Za-z.+-] (it names release assets)");
        }

        return value;
    }

    private static ThirdPartyToolBuild ToolBuild(JsonElement element)
    {
        Dictionary<string, JsonElement> fields = Object(element, "build", ["tag", "revision", "source", "dependencies", "scripts", "patches"]);
        int revision = Integer(fields["revision"], "build.revision");
        if (revision < 1)
        {
            throw new FormatException("build.revision: must be at least 1");
        }

        ThirdPartyBuildInput source = BuildInput(fields["source"], "build.source", hasPath: false);
        var dependencies = new List<ThirdPartyBuildInput>();
        var paths = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (JsonElement entry in Array(fields["dependencies"], "build.dependencies"))
        {
            ThirdPartyBuildInput input = BuildInput(entry, "build.dependencies[]", hasPath: true);
            if (!paths.Add(input.Path!))
            {
                throw new FormatException($"build.dependencies[].path: '{input.Path}' appears twice");
            }

            dependencies.Add(input);
        }

        IReadOnlyList<string> scripts = Paths(fields["scripts"], "build.scripts", nonEmpty: true);
        IReadOnlyList<string> patches = Paths(fields["patches"], "build.patches", nonEmpty: false);
        foreach (string script in scripts)
        {
            if (!script.StartsWith(BuildDirectory + "/", StringComparison.Ordinal) ||
                !BuildScriptExtensions.Any(extension => script.EndsWith(extension, StringComparison.Ordinal)))
            {
                throw new FormatException(
                    $"build.scripts: '{script}' must be a {string.Join(", ", BuildScriptExtensions)} file under {BuildDirectory}/");
            }
        }

        foreach (string patch in patches)
        {
            if (!patch.StartsWith(BuildDirectory + "/", StringComparison.Ordinal) || !patch.EndsWith(".patch", StringComparison.Ordinal))
            {
                throw new FormatException($"build.patches: '{patch}' must be a .patch file under {BuildDirectory}/");
            }
        }

        return new(Text(fields["tag"], "build.tag"), revision, source, dependencies, scripts, patches);
    }

    private static ThirdPartyBuildInput BuildInput(JsonElement element, string name, bool hasPath)
    {
        string[] required = hasPath ? ["path", "repository", "commit", "url", "sha256"] : ["repository", "commit", "url", "sha256"];
        Dictionary<string, JsonElement> fields = Object(element, name, required);
        string repository = Https(fields["repository"], name + ".repository");
        string url = Https(fields["url"], name + ".url");
        string commit = Text(fields["commit"], name + ".commit");
        if (!CommitText.IsMatch(commit))
        {
            throw new FormatException($"{name}.commit: must be 40 lowercase hex digits (a pinned git commit)");
        }

        string sha256 = Text(fields["sha256"], name + ".sha256");
        if (!Sha256Text.IsMatch(sha256))
        {
            throw new FormatException($"{name}.sha256: must be 64 lowercase hex digits");
        }

        string? path = hasPath ? RelativePath(Text(fields["path"], name + ".path"), name + ".path") : null;
        return new(path, repository, commit, url, sha256);
    }

    private static ThirdPartyHost Host(string key, JsonElement element, ThirdPartyBinaryOrigin origin, string name, string version,
        ThirdPartyToolBuild? build)
    {
        string label = "hosts." + key;
        Dictionary<string, JsonElement> fields = origin == ThirdPartyBinaryOrigin.Build
            ? Object(element, label, ["archive", "files"])
            : Object(element, label, ["files"]);
        ThirdPartyArchive? archive = null;
        if (build is not null)
        {
            Dictionary<string, JsonElement> archiveFields = Object(fields["archive"], label + ".archive", ["url", "sha256", "size"]);
            string url = Https(archiveFields["url"], label + ".archive.url");
            string sha256 = Text(archiveFields["sha256"], label + ".archive.sha256");
            if (!Sha256Text.IsMatch(sha256))
            {
                throw new FormatException($"{label}.archive.sha256: must be 64 lowercase hex digits");
            }

            long size = Size(archiveFields["size"], label + ".archive.size");
            if (size == 0)
            {
                throw new FormatException($"{label}.archive.size: must be positive");
            }

            archive = new(url, sha256, size);
            string expected = ThirdPartyToolBuild.AssetName(name, version, build.Revision, key);
            if (archive.FileName != expected)
            {
                throw new FormatException($"{label}.archive.url: asset name must be '{expected}' (release asset naming rule)");
            }
        }

        var files = new List<ThirdPartyHostFile>();
        var paths = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (JsonElement entry in Array(fields["files"], label + ".files"))
        {
            string item = label + ".files[]";
            Dictionary<string, JsonElement> fileFields = origin switch
            {
                ThirdPartyBinaryOrigin.Build => Object(entry, item, ["path", "sha256", "size", "role"]),
                ThirdPartyBinaryOrigin.Sdk => Object(entry, item, ["path", "sdkPath", "sha256", "size", "role"]),
                _ => Object(entry, item, ["path", "role"])
            };
            string path = RelativePath(Text(fileFields["path"], item + ".path"), item + ".path");
            if (!paths.Add(path))
            {
                throw new FormatException($"{item}.path: '{path}' appears twice (paths differing only by case are the same file)");
            }

            ThirdPartyFileRole role = Text(fileFields["role"], item + ".role") switch
            {
                "runtime" => ThirdPartyFileRole.Runtime,
                "redist" => ThirdPartyFileRole.Redist,
                "symbols" => ThirdPartyFileRole.Symbols,
                "tool" => ThirdPartyFileRole.Tool,
                "notice" => ThirdPartyFileRole.Notice,
                string other => throw new FormatException($"{item}.role: '{other}' must be runtime, redist, symbols, tool or notice")
            };
            if (origin == ThirdPartyBinaryOrigin.System)
            {
                if (role != ThirdPartyFileRole.Tool || path.Contains('/'))
                {
                    throw new FormatException($"{item}: a system tool lists only executable names with role \"tool\"");
                }

                files.Add(new(path, role, null, null, null));
                continue;
            }

            string sha256 = Text(fileFields["sha256"], item + ".sha256");
            if (!Sha256Text.IsMatch(sha256))
            {
                throw new FormatException($"{item}.sha256: '{path}' must be 64 lowercase hex digits");
            }

            string? sdkPath = origin == ThirdPartyBinaryOrigin.Sdk
                ? RelativePath(Text(fileFields["sdkPath"], item + ".sdkPath"), item + ".sdkPath")
                : null;
            files.Add(new(path, role, sha256, Size(fileFields["size"], item + ".size"), sdkPath));
        }

        if (files.Count == 0)
        {
            throw new FormatException($"{label}.files: must list at least one file");
        }

        if (origin == ThirdPartyBinaryOrigin.Sdk && files.Count != 1)
        {
            // 저장소 디렉터리 이름이 그 파일의 SHA-256이다(내용 주소). 파일이 더 필요하면 manifest를 나눈다.
            throw new FormatException($"{label}.files: an SDK binary copies exactly one file");
        }

        return new(key, archive, files);
    }

    private static string Https(JsonElement element, string name)
    {
        string url = Text(element, name);
        if (!url.StartsWith("https://", StringComparison.Ordinal) || url.EndsWith('/'))
        {
            throw new FormatException($"{name}: must be an https URL naming a resource");
        }

        return url;
    }

    private static long Size(JsonElement element, string name) =>
        element.ValueKind == JsonValueKind.Number && element.TryGetInt64(out long value) && value >= 0
            ? value
            : throw new FormatException($"{name}: must be a non-negative 64-bit integer");
}
